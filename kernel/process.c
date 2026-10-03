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
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/task.h>
#include <surfos/gdt.h>
#include <surfos/elf.h>
#include <surfos/process.h>
#include <surfos/syscall.h>
#include <mm/memory.h>
#include <mm/paging.h>
#include <mm/kalloc.h>
#include <mm/uvm.h>
#include <fs/vfs.h>
#include <blibc_common.h>

#define USER_STACK_INIT 0x4000          /* mapped before the program starts; the rest on demand */
#define SPAWN_STR_BYTES 2048
#define EXIT_LOAD_FAILED 127

struct spawn_args {
    char path[VFS_PATH_MAX];
    int argc;
    char *argv[SPAWN_MAX_ARGS];
    char strs[SPAWN_STR_BYTES];
};

static void load_failed(struct spawn_args *sa, struct file *f, const char *why) {
    if(f) vfs_close(f);
    kprintf("%s: %s\n", sa->path, why);
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
    u_long top = 0, sp, argv_user[SPAWN_MAX_ARGS + 1];
    int i, r, fd;

    m = uvm_create();
    if(!m) load_failed(sa, f, "out of memory for the address space");
    curTask->uvm = m;
    curTask->flags |= TF_USER;
    uvm_switch(m);                                        /* schedule() keeps it loaded for this task */

    r = vfs_open(sa->path, O_RDONLY, &f);
    if(r) { f = NULL; load_failed(sa, f, strerror(r)); }
    if(vfs_read(f, &eh, sizeof(eh)) != (int)sizeof(eh) || memcmp(eh.e_ident, ELF_MAGIC, 4) ||
       eh.e_ident[4] != ELFCLASS32 || eh.e_ident[5] != ELFDATA2LSB || eh.e_type != ET_EXEC || eh.e_machine != EM_386 ||
       eh.e_phentsize != sizeof(ph) || !eh.e_phnum) load_failed(sa, f, "not a static i386 ELF executable");
    if(eh.e_entry < USER_START || eh.e_entry >= USER_END) load_failed(sa, f, "entry point outside user memory");

    for(i = 0; i < eh.e_phnum; i++) {
        u_long end;
        if(vfs_lseek(f, (i32)(eh.e_phoff + i * sizeof(ph)), SEEK_SET) < 0 || vfs_read(f, &ph, sizeof(ph)) != (int)sizeof(ph))
            load_failed(sa, f, "cannot read the program headers");
        if(ph.p_type != PT_LOAD || !ph.p_memsz) continue;
        end = ph.p_vaddr + ph.p_memsz;
        if(ph.p_vaddr < USER_START || end > USER_END - USER_STACK_MAX || end < ph.p_vaddr || ph.p_filesz > ph.p_memsz)
            load_failed(sa, f, "segment outside user memory");
        if(uvm_map_range(m, ph.p_vaddr, ph.p_memsz, PTE_W) != 0) load_failed(sa, f, "out of memory for a segment");
        if(ph.p_filesz) {
            if(vfs_lseek(f, (i32)ph.p_offset, SEEK_SET) < 0 || vfs_read(f, (void *)ph.p_vaddr, ph.p_filesz) != (int)ph.p_filesz)
                load_failed(sa, f, "cannot read a segment");
        }
        if(end > top) top = end;
    }
    vfs_close(f);
    f = NULL;
    if(!top) load_failed(sa, f, "nothing to load");
    m->brk_start = m->brk = PAGE_ALIGN_UP(top);

    /* the stack: strings at the top, then the argv array, then argc and argv for _start */
    if(uvm_map_range(m, USER_END - USER_STACK_INIT, USER_STACK_INIT, PTE_W) != 0) load_failed(sa, f, "out of memory for the stack");
    m->stack_low = USER_END - USER_STACK_INIT;
    sp = USER_END;
    for(i = sa->argc - 1; i >= 0; i--) {
        size_t n = strlen(sa->argv[i]) + 1;
        sp -= n;
        memcpy((void *)sp, sa->argv[i], n);
        argv_user[i] = sp;
    }
    argv_user[sa->argc] = 0;
    sp &= ~3UL;
    sp -= (sa->argc + 1) * sizeof(u_long);
    memcpy((void *)sp, argv_user, (sa->argc + 1) * sizeof(u_long));
    {
        u_long argv_addr = sp;
        sp -= 2 * sizeof(u_long);
        ((u_long *)sp)[0] = (u_long)sa->argc;
        ((u_long *)sp)[1] = argv_addr;
    }

    /* 0, 1, 2: the console */
    for(fd = 0; fd < 3; fd++) {
        struct file *cf;
        if(vfs_open("/dev/console", fd == 0 ? O_RDONLY : O_WRONLY, &cf) == 0) fd_install(cf);
    }

    kfree(sa);
    enter_user(eh.e_entry, sp);
}

int process_spawn(const char *path, int argc, char *const argv[], surf_console *con, u_int flags) {
    struct spawn_args *sa;
    struct stat st;
    surf_task *t;
    const char *base;
    size_t used = 0;
    int i, r;

    r = vfs_stat(path, &st);
    if(r) return r;
    if(st.type != VN_FILE) return -EACCES;
    if(argc < 1 || argc > SPAWN_MAX_ARGS) return -EINVAL;
    sa = (struct spawn_args *)kcalloc(1, sizeof(struct spawn_args));
    if(!sa) return -ENOMEM;
    r = vfs_normalize(vfs_getcwd(), path, sa->path, sizeof(sa->path));
    if(r) { kfree(sa); return r; }
    for(i = 0; i < argc; i++) {
        size_t n = strlen(argv[i]) + 1;
        if(used + n > SPAWN_STR_BYTES) { kfree(sa); return -E2BIG; }
        memcpy(sa->strs + used, argv[i], n);
        sa->argv[i] = sa->strs + used;
        used += n;
    }
    sa->argc = argc;
    base = strrchr(sa->path, '/');
    base = base ? base + 1 : sa->path;
    t = kthread_create_on(base, con, user_entry, sa, PL_NORMAL, flags & TF_DETACHED);
    if(!t) { kfree(sa); return -ENOMEM; }
    return (int)t->pid;
}
