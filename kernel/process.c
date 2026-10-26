/*
SurfOS Processes: spawn, ELF loading, entering ring 3
--------------------
File: process.c Date: 10/3/26 (roadmap P2)
--------------------
process_spawn() copies the path and arguments into kernel memory and starts
a kernel thread. That thread creates its address space, switches to it,
maps and reads the ELF segments straight into their user addresses, builds
the stack with argc/argv, opens /dev/console as descriptors 0, 1 and 2 and
drops to ring 3 with an iret. From then on it only re-enters the kernel
through traps.

10/5/26: process_spawn_files() hands the child open files of its parent as
descriptors 0, 1 and 2 instead (the user shell's < > >> redirection).

10/6/26: TF_SHELL passes through too: init starts the user shell, surfsh, with it
and starts it again when it ends. TF_LOADED tells a program that ran from one that
could not be loaded, whatever its exit status says.

10/6/26: from its creation until its program is loaded, a process is inside a call
(TF_INCALL, TC_SYSCALL): a kill waits for the loading to end (task.h), and what the
process was handed (its arguments, its files) is never left behind. A program started
by a task that its console's pager covers is in the foreground (TF_FOREGROUND): Ctrl+C
reaches it (tty.c, task_interrupt_job()).

10/24/26 (pipelines and scripting in surfsh): process_spawn_ex() (SYS_spawnx, surfos/spawn.h)
also hands the child a descriptor 3, leaves a descriptor closed, gives it an environment (on
its stack after argv: argc, argv, envp for crt0), and starts a job in the background that
the pager's Ctrl+C does not reach. A program takes up to SPAWN_MAX_ARGS (128) arguments in
SPAWN_ARG_BYTES (8 KB), and with the environment's 4 KB they all fit in the stack's first
16 KB.

10/25/26: a script runs by its name. A file whose first line is "#!INTERPRETER [ARG]" (at
most SCRIPT_LINE bytes) starts INTERPRETER, which must be an ELF program (no #! behind a
#!), as INTERPRETER [ARG] FILE ARGS..., as on every Unix: ARG is the rest of the line, one
argument, its blanks at the end off. The task is named after the script. A file that is
neither ELF nor #! is refused with -ENOEXEC before anything starts, so that a shell can run
it as a script of its own (POSIX's rule); an ELF file the loader cannot run still fails in
the new task, as before. A new task is in the caller's job (surfos/job.h), or in the one
SYS_spawnx names: a new one (SPAWN_JOB_NEW, the task its first) or one of the caller's; a
detached one (init's) is in none.

10/25/26 (robustness, L15): a process is TF_PROCESS from its creation, which a user's kill
looks for (task_user_may_kill()): a kernel thread is never killed by one. L10, L12: it gets
floating-point registers of its own with that flag (task_alloc(), kernel/fpu.c), and its stack
is 16-aligned at _start. L8: a spawn fails at once (ENOMEM) when fewer than 16 frames are left
for programs, which could not even load it.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/task.h>
#include <surfos/gdt.h>
#include <surfos/elf.h>
#include <surfos/process.h>
#include <surfos/syscall.h>
#include <surfos/spawn.h>
#include <surfos/irq.h>
#include <mm/memory.h>
#include <mm/paging.h>
#include <mm/kalloc.h>
#include <mm/uvm.h>
#include <mm/pmm.h>
#include <fs/vfs.h>
#include <blibc_common.h>

#define USER_STACK_INIT 0x4000          /* mapped before the program starts; the rest on demand */
#define EXIT_LOAD_FAILED 127
#define SCRIPT_LINE 256                 /* 10/25/26: a script's #! line, at most */
#define SPAWN_MIN_FRAMES 16             /* 10/25/26 (L8): fewer left for programs, and a spawn fails at once */

struct spawn_args {
    char path[VFS_PATH_MAX];
    int argc;
    char *argv[SPAWN_MAX_ARGS];
    char strs[SPAWN_ARG_BYTES];
    struct file *files[4];              /* the child's 0, 1, 2, 3 (a reference each); NULL: the console (0-2), none (3) */
    u_int closed;                       /* 10/24/26: bit n: no descriptor n at all (0-2) */
    int envc;                           /* 10/24/26: the environment */
    char *envp[SPAWN_ENV_MAX];
    char envstrs[SPAWN_ENV_BYTES];
};

/* drop the references to the files the child has not taken over */
static void release_files(struct spawn_args *sa) {
    int i;
    for(i = 0; i < 4; i++) {
        if(sa->files[i]) vfs_close(sa->files[i]);
        sa->files[i] = NULL;
    }
}

/* 10/25/26 (the review of B1): nomem, the loading failed for want of memory (TF_LOADNOMEM: it may work in a moment) */
static void load_failed(struct spawn_args *sa, struct file *f, const char *why, bool nomem) {
    if(f) vfs_close(f);
    if(nomem) {
        u_long irqf = irq_save();
        curTask->flags |= TF_LOADNOMEM;
        irq_restore(irqf);
    }
    kprintf("%s: %s\n", sa->path, why);
    release_files(sa);
    kfree(sa);
    task_exit(EXIT_LOAD_FAILED);
}

static void enter_user(u_long eip, u_long esp) {
    asm volatile(
        "movw %w0, %%ds\n\t"
        "movw %w0, %%es\n\t"
        "movw %w0, %%fs\n\t"
        "movw %w0, %%gs\n\t"
        "pushl %0\n\t"              /* ss */
        "pushl %1\n\t"              /* esp */
        "pushl $0x202\n\t"          /* eflags: interrupts on */
        "pushl %2\n\t"              /* cs */
        "pushl %3\n\t"              /* eip */
        "iret"
        : : "r"((u_long)USER_DS), "r"(esp), "r"((u_long)USER_CS), "r"(eip) : "memory");
    for(;;);
}

/* the new task: build the image and go */
static void user_entry(void *arg) {
    struct spawn_args *sa = (struct spawn_args *)arg;
    struct uvm *m;
    struct file *f = NULL;
    struct elf32_ehdr eh;
    struct elf32_phdr ph;
    u_long top = 0, sp, argv_user[SPAWN_MAX_ARGS + 1], env_user[SPAWN_ENV_MAX + 1];
    int i, r, fd;

    {
        /* 10/25/26: Ctrl+Z changes the flags from an interrupt (job control); and (the review of B1) the address space
           is the task's before interrupts come back on: one killed on the spot meanwhile (kill_escalate()) leaves
           uvmreap nothing to miss */
        u_long irqf = irq_save();
        m = uvm_create();
        curTask->uvm = m;
        if(m) curTask->flags |= TF_USER;
        irq_restore(irqf);
    }
    if(!m) load_failed(sa, f, "out of memory for the address space", true);
    uvm_switch(m);                                        /* schedule() keeps it loaded for this task */

    r = vfs_open(sa->path, O_RDONLY, &f);
    if(r) { f = NULL; load_failed(sa, f, strerror(r), r == -ENOMEM); }
    if(vfs_read(f, &eh, sizeof(eh)) != (int)sizeof(eh) || memcmp(eh.e_ident, ELF_MAGIC, 4) ||
       eh.e_ident[4] != ELFCLASS32 || eh.e_ident[5] != ELFDATA2LSB || eh.e_type != ET_EXEC || eh.e_machine != EM_386 ||
       eh.e_phentsize != sizeof(ph) || !eh.e_phnum) load_failed(sa, f, "not a static i386 ELF executable", false);
    if(eh.e_entry < USER_START || eh.e_entry >= USER_END) load_failed(sa, f, "entry point outside user memory", false);

    for(i = 0; i < eh.e_phnum; i++) {
        u_long end;
        if(vfs_lseek(f, (i32)(eh.e_phoff + i * sizeof(ph)), SEEK_SET) < 0 || vfs_read(f, &ph, sizeof(ph)) != (int)sizeof(ph))
            load_failed(sa, f, "cannot read the program headers", false);
        if(ph.p_type != PT_LOAD || !ph.p_memsz) continue;
        end = ph.p_vaddr + ph.p_memsz;
        if(ph.p_vaddr < USER_START || end > USER_END - USER_STACK_MAX || end < ph.p_vaddr || ph.p_filesz > ph.p_memsz)
            load_failed(sa, f, "segment outside user memory", false);
        r = uvm_map_range(m, ph.p_vaddr, ph.p_memsz, PTE_W);
        if(r) load_failed(sa, f, r == -ENOMEM ? "out of memory for a segment" : "a segment could not be mapped", r == -ENOMEM);
        if(ph.p_filesz) {
            if(vfs_lseek(f, (i32)ph.p_offset, SEEK_SET) < 0 || vfs_read(f, (void *)ph.p_vaddr, ph.p_filesz) != (int)ph.p_filesz)
                load_failed(sa, f, "cannot read a segment", false);
        }
        if(end > top) top = end;
    }
    vfs_close(f);
    f = NULL;
    if(!top) load_failed(sa, f, "nothing to load", false);
    m->brk_start = m->brk = PAGE_ALIGN_UP(top);

    /* the stack: strings at the top (the environment's, then argv's), then the envp and argv arrays,
       then argc, argv and envp for _start (10/24/26: envp; at most SPAWN_ARG_BYTES + SPAWN_ENV_BYTES
       of strings and both arrays, well inside the pages mapped here) */
    if(uvm_map_range(m, USER_END - USER_STACK_INIT, USER_STACK_INIT, PTE_W) != 0)
        load_failed(sa, f, "out of memory for the stack", true);
    m->stack_low = USER_END - USER_STACK_INIT;
    sp = USER_END;
    for(i = sa->envc - 1; i >= 0; i--) {
        size_t n = strlen(sa->envp[i]) + 1;
        sp -= n;
        memcpy((void *)sp, sa->envp[i], n);
        env_user[i] = sp;
    }
    env_user[sa->envc] = 0;
    for(i = sa->argc - 1; i >= 0; i--) {
        size_t n = strlen(sa->argv[i]) + 1;
        sp -= n;
        memcpy((void *)sp, sa->argv[i], n);
        argv_user[i] = sp;
    }
    argv_user[sa->argc] = 0;
    /* 10/25/26 (L12): the arrays and the three words placed so that _start finds its stack 16-aligned, as the
       i386 ABI has it (a program's SSE code keeps 16-byte values there; crt0 aligns it again for main) */
    sp &= ~15UL;
    sp -= (0 - (sa->envc + 1 + sa->argc + 1 + 3) * sizeof(u_long)) & 15;
    sp -= (sa->envc + 1) * sizeof(u_long);
    memcpy((void *)sp, env_user, (sa->envc + 1) * sizeof(u_long));
    {
        u_long envp_addr = sp, argv_addr;
        sp -= (sa->argc + 1) * sizeof(u_long);
        memcpy((void *)sp, argv_user, (sa->argc + 1) * sizeof(u_long));
        argv_addr = sp;
        sp -= 3 * sizeof(u_long);
        ((u_long *)sp)[0] = (u_long)sa->argc;
        ((u_long *)sp)[1] = argv_addr;
        ((u_long *)sp)[2] = envp_addr;
    }

    /* 0, 1, 2: the files the parent passed, otherwise the console (10/24/26: or nothing); 3 only
       when one was passed */
    for(fd = 0; fd < 4; fd++) {
        struct file *cf = sa->files[fd];
        sa->files[fd] = NULL;                             /* the descriptor table owns the reference now */
        if(!cf && (fd == 3 || (sa->closed & (1u << fd)) ||
                   vfs_open("/dev/console", fd == 0 ? O_RDONLY : O_WRONLY, &cf) != 0)) continue;
        curTask->files[fd] = cf;
    }

    kfree(sa);
    {
        u_long irqf = irq_save();
        curTask->flags |= TF_LOADED;                      /* 10/6/26: it runs: init tells a load failure apart */
        irq_restore(irqf);
    }
    task_call_leave(TC_SYSCALL);                          /* 10/6/26: loaded; a kill that waited ends it here */
    enter_user(eh.e_entry, sp);
}

int process_spawn(const char *path, int argc, char *const argv[], surf_console *con, u_int flags) {
    return process_spawn_ex(path, argc, argv, con, flags, NULL);
}

int process_spawn_files(const char *path, int argc, char *const argv[], surf_console *con, u_int flags,
                        struct file *const files[3]) {
    struct spawn_more m;
    int i;
    if(!files) return process_spawn_ex(path, argc, argv, con, flags, NULL);
    memset(&m, 0, sizeof(m));
    for(i = 0; i < 3; i++) m.files[i] = files[i];
    return process_spawn_ex(path, argc, argv, con, flags, &m);
}

/* 10/25/26: the start of the file at path: 0 for an ELF program (the loader checks the rest), 1 for a
   script, its #! line's interpreter and argument ("" for none) in *interp and *arg (inside line), -ENOEXEC
   for anything else; another -errno when it cannot be read */
static int program_kind(const char *path, char *line, char **interp, char **arg) {
    struct file *f;
    char *s, *e, *a, *z;
    int n, k, r = vfs_open(path, O_RDONLY, &f);
    if(r) return r;
    n = vfs_read(f, line, SCRIPT_LINE);
    vfs_close(f);
    if(n < 0) return n;
    if(n >= 4 && !memcmp(line, ELF_MAGIC, 4)) return 0;
    if(n < 2 || line[0] != '#' || line[1] != '!') return -ENOEXEC;
    for(k = 0; k < n && line[k] != '\n'; k++);
    if(k == SCRIPT_LINE) return -ENAMETOOLONG;          /* no end to the #! line */
    line[k] = 0;
    for(z = line + k; z > line + 2 && (z[-1] == ' ' || z[-1] == '\t' || z[-1] == '\r'); ) *--z = 0;
    for(s = line + 2; *s == ' ' || *s == '\t'; s++);
    for(e = s; *e && *e != ' ' && *e != '\t'; e++);
    if(e == s) return -ENOEXEC;                         /* #! and nothing after it */
    for(a = e; *a == ' ' || *a == '\t'; a++);
    *e = 0;
    *interp = s;
    *arg = a;
    return 1;
}

/* 10/25/26: 1 when the file at path begins as an ELF program, 0 when not, -errno when it cannot be read */
static int is_elf(const char *path) {
    struct file *f;
    char m[4];
    int n, r = vfs_open(path, O_RDONLY, &f);
    if(r) return r;
    n = vfs_read(f, m, sizeof(m));
    vfs_close(f);
    return n == (int)sizeof(m) && !memcmp(m, ELF_MAGIC, 4);
}

/* 10/24/26: with a descriptor 3, closed descriptors, an environment, in the background (process.h) */
int process_spawn_ex(const char *path, int argc, char *const argv[], surf_console *con, u_int flags,
                     const struct spawn_more *more) {
    struct spawn_args *sa;
    struct stat st;
    const char *base, *const *args = (const char *const *)argv;
    const char *head[3];
    char line[SCRIPT_LINE + 1], *interp = NULL, *iarg = NULL;
    size_t used = 0;
    int i, r, kind, nhead = 0, nargs = argc, job;

    if(pmm_room(PMM_USER) < SPAWN_MIN_FRAMES) return -ENOMEM;   /* 10/25/26 (L8): it could not even load */
    r = vfs_stat(path, &st);
    if(r) return r;
    if(st.type != VN_FILE || !strcmp(st.fs, "devfs")) return -EACCES;      /* 10/25/26: a device is read by no #! check */
    if(argc < 1 || argc > SPAWN_MAX_ARGS) return -EINVAL;
    kind = program_kind(path, line, &interp, &iarg);    /* 10/25/26: an ELF program, or a script */
    if(kind < 0) return kind;
    if(kind == 1) {                                     /* INTERPRETER [ARG] FILE ARGS... */
        r = vfs_stat(interp, &st);
        if(r) return r;
        if(st.type != VN_FILE) return -EACCES;
        r = is_elf(interp);
        if(r <= 0) return r < 0 ? r : -ENOEXEC;        /* no #! behind a #! */
        head[nhead++] = interp;
        if(*iarg) head[nhead++] = iarg;
        head[nhead++] = path;
        nargs = nhead + argc - 1;
        if(nargs > SPAWN_MAX_ARGS) return -E2BIG;
        args++;                                         /* argv[0] gives way to the interpreter's */
    }
    sa = (struct spawn_args *)kcalloc(1, sizeof(struct spawn_args));
    if(!sa) return -ENOMEM;
    r = vfs_normalize(vfs_getcwd(), kind == 1 ? interp : path, sa->path, sizeof(sa->path));
    if(r) { kfree(sa); return r; }
    for(i = 0; i < nargs; i++) {
        const char *a = i < nhead ? head[i] : args[i - nhead];
        size_t n = strlen(a) + 1;
        if(used + n > SPAWN_ARG_BYTES) { kfree(sa); return -E2BIG; }
        memcpy(sa->strs + used, a, n);
        sa->argv[i] = sa->strs + used;
        used += n;
    }
    sa->argc = nargs;
    if(more) {                                      /* 10/24/26: the environment */
        if(more->envc < 0 || more->envc > SPAWN_ENV_MAX) { kfree(sa); return -E2BIG; }
        for(i = 0, used = 0; i < more->envc; i++) {
            size_t n = strlen(more->envp[i]) + 1;
            if(used + n > SPAWN_ENV_BYTES) { kfree(sa); return -E2BIG; }
            memcpy(sa->envstrs + used, more->envp[i], n);
            sa->envp[i] = sa->envstrs + used;
            used += n;
        }
        sa->envc = more->envc;
        sa->closed = more->closed & 7;
    }
    for(i = 0; more && i < 4; i++) {
        if(!more->files[i]) continue;
        {
            u_long irqf = irq_save();               /* the count is shared with the parent's descriptors */
            more->files[i]->refs++;
            irq_restore(irqf);
        }
        sa->files[i] = more->files[i];
    }
    base = strrchr(kind == 1 ? path : sa->path, '/');   /* 10/25/26: a script's task is named after it */
    base = base ? base + 1 : (kind == 1 ? path : sa->path);
    if(!(flags & TF_DETACHED) && con && console_paging(con) && !(more && more->background))
        flags |= TF_FOREGROUND;                     /* 10/6/26; 10/24/26: not a job started in the background */
    /* 10/25/26: its job (surfos/job.h), set before it can run: the one spawnx asks for, else the
       caller's on the caller's terminal (none on a pty's: kthread_create_job()). In none: a detached
       task, and what a kernel command starts (run waits for its end and knows of no stop) */
    job = (flags & TF_DETACHED) || (curTask && (curTask->in_call & TC_COMMAND)) ? SPAWN_JOB_NONE : more ? more->job : 0;
    /* 10/25/26 (L15): TF_PROCESS, so that a user's kill may end it (task_user_may_kill()) */
    r = kthread_create_job(base, con, user_entry, sa, PL_NORMAL,
                           (flags & (TF_DETACHED | TF_SHELL | TF_FOREGROUND)) | TF_INCALL | TF_PROCESS, job);
    if(r < 0) { release_files(sa); kfree(sa); }
    return r;
}
