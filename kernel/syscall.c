/*
SurfOS System Call Dispatch
--------------------
File: syscall.c Date: 10/3/26 (roadmap P1)
--------------------
int 0x80 lands here. Every pointer a program passes is checked against its
address space before the kernel touches it (uvm_check), so a bad pointer
costs the program -EFAULT and never the kernel a page fault with a lock held.

10/5/26 (files on USB storage, the editor): rename, fstat, ftruncate, fsync,
sync, statfs, dup2, time, ttyctl and spawnfd (calls 22-31).

10/6/26 (the user shell, surfsh): kcmd and kcmdinfo (calls 32 and 33) run and
list the kernel's shell commands (surfos/kcmd.h). A command runs in the calling
task, as it would in the ring-0 shell: it prints on the caller's console and asks
its questions there, cd moves the caller, run starts the program as the caller's
child (spawn's are detached, init's), a crash command kills the caller, and the
background tasks of funky and tcpecho are detached too. When the caller's
descriptor 1 is a file, the output is kept in memory while the command runs and
written there afterwards, with a note when it did not all fit.

10/6/26: a task is inside a call from its int 0x80 until it returns to user mode
(TC_SYSCALL), and a kernel command is a call of its own (TC_COMMAND): a kill waits
for the call's end, then the task ends (task.h), and Ctrl+C cuts a command short.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/task.h>
#include <surfos/trap.h>
#include <surfos/timer.h>
#include <surfos/syscall.h>
#include <surfos/process.h>
#include <surfos/tty.h>
#include <surfos/irq.h>
#include <surfos/kcmd.h>
#include <sys/bdev.h>
#include <mm/uvm.h>
#include <mm/kalloc.h>
#include <fs/vfs.h>
#include <time.h>
#include <blibc_common.h>

#define ARG_STR_MAX VFS_PATH_MAX

/* the structures user programs see (user/include/sys/stat.h, sys/statfs.h, dirent.h) keep these sizes */
typedef char stat_layout_is_fixed[sizeof(struct stat) == 40 ? 1 : -1];
typedef char statfs_layout_is_fixed[sizeof(struct statfs) == 116 ? 1 : -1];
typedef char dirent_layout_is_fixed[sizeof(struct dirent) == 268 ? 1 : -1];
typedef char kcmd_info_layout_is_fixed[sizeof(struct kcmd_info) == 176 ? 1 : -1];

static bool ustr(const char *s) {
    return uvm_check_str(curTask->uvm, s, ARG_STR_MAX);
}

static bool ubuf(const void *p, u_long len, bool write) {
    return uvm_check(curTask->uvm, (u_long)p, len, write);
}

static int sys_write(int fd, const void *buf, u32 len) {
    struct file *f = fd_get(fd);
    if(!f) return -EBADF;
    if(!ubuf(buf, len, false)) return -EFAULT;
    return vfs_write(f, buf, len);
}

static int sys_read(int fd, void *buf, u32 len) {
    struct file *f = fd_get(fd);
    if(!f) return -EBADF;
    if(!ubuf(buf, len, true)) return -EFAULT;
    return vfs_read(f, buf, len);
}

static int sys_open(const char *path, int flags) {
    struct file *f;
    int r;
    if(!ustr(path)) return -EFAULT;
    r = vfs_open(path, flags, &f);
    if(r) return r;
    r = fd_install(f);
    if(r < 0) vfs_close(f);
    return r;
}

static int sys_readdir(int fd, struct dirent *de) {
    struct file *f = fd_get(fd);
    if(!f) return -EBADF;
    if(!ubuf(de, sizeof(*de), true)) return -EFAULT;
    return vfs_readdir(f, de);
}

static int sys_stat(const char *path, struct stat *st) {
    if(!ustr(path) || !ubuf(st, sizeof(*st), true)) return -EFAULT;
    return vfs_stat(path, st);
}

static int sys_getcwd(char *buf, u32 size) {
    const char *cwd = vfs_getcwd();
    if(!ubuf(buf, size, true)) return -EFAULT;
    if(strlen(cwd) + 1 > size) return -ERANGE;
    strcpy(buf, cwd);
    return (int)strlen(cwd);
}

/* copy a user argv[] (NULL-terminated) into kargv; an empty one becomes { path } */
static int user_argv(const char *path, char *const argv[], char **kargv) {
    int argc = 0;
    if(argv) {
        for(;;) {
            if(!ubuf(argv + argc, sizeof(char *), false)) return -EFAULT;
            if(!argv[argc]) break;
            if(!ustr(argv[argc])) return -EFAULT;
            if(argc == SPAWN_MAX_ARGS) return -E2BIG;
            kargv[argc] = argv[argc];
            argc++;
        }
    }
    if(!argc) { kargv[0] = (char *)path; argc = 1; }
    return argc;
}

static int sys_spawn(const char *path, char *const argv[]) {
    char *kargv[SPAWN_MAX_ARGS];
    int argc;
    if(!ustr(path)) return -EFAULT;
    argc = user_argv(path, argv, kargv);
    if(argc < 0) return argc;
    return process_spawn(path, argc, kargv, curTask->con, 0);
}

/* spawn with the child's 0, 1 and 2 shared with the caller's fds[i] (-1: the console) */
static int sys_spawnfd(const char *path, char *const argv[], const int *fds) {
    char *kargv[SPAWN_MAX_ARGS];
    struct file *files[3] = { NULL, NULL, NULL };
    int kfds[3], argc, i;
    if(!ustr(path)) return -EFAULT;
    argc = user_argv(path, argv, kargv);
    if(argc < 0) return argc;
    if(fds) {
        if(!ubuf(fds, sizeof(kfds), false)) return -EFAULT;
        memcpy(kfds, fds, sizeof(kfds));
        for(i = 0; i < 3; i++) {
            if(kfds[i] == -1) continue;
            files[i] = fd_get(kfds[i]);
            if(!files[i]) return -EBADF;
        }
    }
    return process_spawn_files(path, argc, kargv, curTask->con, 0, files);
}

/* 10/6/26: a kill of the caller ends the wait (EINTR), and the caller ends with the call */
static int sys_wait(int pid, int *status, int options) {
    int st = 0, r;
    if(status && !ubuf(status, sizeof(int), true)) return -EFAULT;
    if(options & ~WNOHANG) return -EINVAL;
    r = task_wait_opts(pid < 0 ? WAIT_ANY : (u_long)pid, &st, options | WINTR);
    if(r < 0 && (curTask->intr & TI_KILL)) return -EINTR;
    if(r > 0 && status) *status = st;
    return r;
}

/* 10/6/26: the caller killing itself ends when this call does, like any task in a call */
static int sys_kill(int pid) {
    surf_task *t = task_find((u_long)pid);
    if(!t || pid <= 1) return -EINVAL;
    kill_task(t);
    return 0;
}

/**** 10/5/26: calls 22-31 ****/

static int sys_rename(const char *from, const char *to) {
    if(!ustr(from) || !ustr(to)) return -EFAULT;
    return vfs_rename(from, to);
}

static int sys_fstat(int fd, struct stat *st) {
    struct file *f = fd_get(fd);
    struct stat k;
    if(!f) return -EBADF;
    if(!ubuf(st, sizeof(*st), true)) return -EFAULT;
    memset(&k, 0, sizeof(k));
    k.type = f->v->type;
    k.size = f->v->size;
    k.mtime = f->v->mtime;
    k.ino = f->v->ino;
    strlcpy(k.fs, f->v->sb->type->name, sizeof(k.fs));
    strlcpy(k.dev, f->v->sb->dev ? f->v->sb->dev->name : "", sizeof(k.dev));
    memcpy(st, &k, sizeof(k));
    return 0;
}

static int sys_statfs(const char *path, struct statfs *st) {
    struct statfs k;
    int r;
    if(!ustr(path) || !ubuf(st, sizeof(*st), true)) return -EFAULT;
    r = vfs_statfs(path, &k);
    if(r == 0) memcpy(st, &k, sizeof(k));
    return r;
}

/* newfd becomes another descriptor for oldfd's open file (shared position), closing what it was */
static int sys_dup2(int oldfd, int newfd) {
    struct file *f = fd_get(oldfd), *old;
    u_long flags;
    if(!f || newfd < 0 || newfd >= NR_OPEN) return -EBADF;
    if(oldfd == newfd) return newfd;
    flags = irq_save();                  /* the reference count is shared with other tasks after spawnfd */
    f->refs++;
    irq_restore(flags);
    old = curTask->files[newfd];
    curTask->files[newfd] = f;
    if(old) vfs_close(old);
    return newfd;
}

/* days since 1970-01-01 (Howard Hinnant's days_from_civil, as in fs/fat.c) */
static i32 days_from_civil(i32 y, u32 m, u32 d) {
    i32 era, yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (i32)(m + (m > 2 ? -3 : 9)) + 2) / 5 + (i32)d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* seconds since 1970 from the CMOS clock, read as it is (the time FAT stamps files with) */
static int sys_time(void) {
    time_t t;
    get_time(&t);
    if(t.year < 1970 || t.month < 1 || t.month > 12 || t.date < 1 || t.date > 31 || t.hour > 23 || t.minute > 59 || t.second > 59)
        return 0;
    return (int)((u32)days_from_civil((i32)t.year, t.month, t.date) * 86400 + t.hour * 3600 + t.minute * 60 + t.second);
}

/**** 10/6/26: the kernel's shell commands (surfos/kcmd.h) ****/

/* What a kernel command running in a task's system call holds. A command can end its task
   (crashgp, any fault in it), and so can a kill that could not wait for its end (10/6/26: a
   kill waits for the end of the call, 2 s at most, surfos/task.h): then reap() hands the
   task to kcmd_task_gone() and these are released all the same. The record
   is on the task's kernel stack, which stays until the task is reaped; a task runs one
   command at a time. 10/6/26 (review): the record is on the list before the call holds
   anything and leaves it only after the call let go of everything, and each field is set
   together with taking what it names, interrupts off (and cleared together with letting it
   go), so whenever the task stops, the record says exactly what it holds. */
struct kcmd_call {
    surf_task *task;
    char *args;                 /* argv[] and its strings in one block */
    char *capture;              /* KCMD_CAPTURE_MAX bytes for a redirection, or NULL */
    struct file *out;           /* descriptor 1's file while it is captured for (a reference) */
    struct kcmd_call *next;
};
static struct kcmd_call *kcmd_calls;

static void kcmd_track(struct kcmd_call *c) {
    u_long flags = irq_save();
    c->next = kcmd_calls;
    kcmd_calls = c;
    irq_restore(flags);
}

/* let go of what c holds, each pointer cleared with its object */
static void kcmd_release(struct kcmd_call *c) {
    struct file *f;
    u_long flags = irq_save();
    f = c->out;
    c->out = NULL;
    if(f && f->refs > 1) {              /* not the last reference: that only counts down, at once */
        vfs_close(f);
        f = NULL;
    }
    kfree(c->capture);
    c->capture = NULL;
    kfree(c->args);
    c->args = NULL;
    irq_restore(flags);
    if(f) vfs_close(f);                 /* the last one (descriptor 1 was closed meanwhile): it may write */
}

void kcmd_task_gone(surf_task *t) {
    struct kcmd_call **pp, *c = NULL;
    u_long flags = irq_save();
    for(pp = &kcmd_calls; *pp; pp = &(*pp)->next) {
        if((*pp)->task == t) { c = *pp; *pp = c->next; break; }
    }
    irq_restore(flags);
    if(c) kcmd_release(c);
}

static void kcmd_forget(struct kcmd_call *c) {
    struct kcmd_call **pp;
    u_long flags = irq_save();
    for(pp = &kcmd_calls; *pp; pp = &(*pp)->next) {
        if(*pp == c) { *pp = c->next; break; }
    }
    irq_restore(flags);
}

/* is name one of the commands? (before memory is set aside for its output) */
static bool kcmd_exists(const char *name) {
    struct kcmd_info info;
    int i;
    for(i = 0; shell_command_info(i, &info) == 0; i++) if(!strcmp(info.name, name)) return true;
    return false;
}

static int write_all(struct file *f, const char *p, u32 len) {
    while(len) {
        int n = vfs_write(f, p, len);
        if(n <= 0) return n < 0 ? n : -EIO;
        p += n;
        len -= (u32)n;
    }
    return 0;
}

/* the size of a kernel command's argument, its NUL included: -EFAULT when it is not in the
   caller's memory, -E2BIG when it is longer than ARG_STR_MAX - 1 bytes */
static int arg_size(const char *s) {
    if(ustr(s)) return (int)strlen(s) + 1;
    return ubuf(s, ARG_STR_MAX, false) ? -E2BIG : -EFAULT;
}

/* argv is checked as user_argv() checks it and copied, strings and all: a command may write
   into its arguments (strtok), and user memory is not the kernel's to keep */
static int sys_kcmd(char *const argv[]) {
    struct kcmd_call call;
    struct kcapture cap;
    struct file *f;
    char **kargv, *p, note[120];
    int size[SPAWN_MAX_ARGS], argc = 0, i, r, w = 0;
    u_long bytes = 0, flags;

    if(!argv) return -EINVAL;
    for(;;) {
        if(!ubuf(argv + argc, sizeof(char *), false)) return -EFAULT;
        if(!argv[argc]) break;
        if(argc == SPAWN_MAX_ARGS) return -E2BIG;
        size[argc] = arg_size(argv[argc]);
        if(size[argc] < 0) return size[argc];
        bytes += (u_long)size[argc];
        argc++;
    }
    if(!argc) return -EINVAL;
    if(!kcmd_exists(argv[0])) return -ENOENT;            /* checked user memory: this task alone can change it */

    memset(&call, 0, sizeof(call));
    call.task = curTask;
    kcmd_track(&call);                                   /* listed before it holds anything */
    flags = irq_save();
    call.args = (char *)kalloc((argc + 1) * sizeof(char *) + bytes);
    irq_restore(flags);
    if(!call.args) { r = -ENOMEM; goto out; }
    kargv = (char **)call.args;
    p = call.args + (argc + 1) * sizeof(char *);
    for(i = 0; i < argc; i++) {                          /* the bytes measured above, no more */
        memcpy(p, argv[i], (size_t)size[i]);
        p[size[i] - 1] = 0;
        kargv[i] = p;
        p += size[i];
    }
    kargv[argc] = NULL;

    flags = irq_save();                                  /* descriptor 1 a file: a redirection */
    f = fd_get(1);
    if(f && !devfs_is_console(f)) {
        f->refs++;                                       /* shared with other tasks' descriptors */
        call.out = f;
    }
    irq_restore(flags);
    if(call.out) {                                       /* the output is kept and written afterwards */
        flags = irq_save();
        call.capture = (char *)kalloc(KCMD_CAPTURE_MAX);
        irq_restore(flags);
        if(!call.capture) { r = -ENOMEM; goto out; }
        cap.buf = call.capture;
        cap.len = 0;
        cap.size = KCMD_CAPTURE_MAX;
        cap.cut = 0;
        console_capture_begin(&cap);
    }
    task_call_enter(TC_COMMAND);                         /* 10/6/26: Ctrl+C cuts it short */
    r = shell_run_command(argc, kargv);
    if(task_call_leave(TC_COMMAND)) r = KCMD_INTERRUPTED;
    if(call.capture) {
        console_capture_end();
        w = write_all(call.out, cap.buf, cap.len);
        if(!w && cap.cut) {                                  /* the note says what was left out */
            snprintf(note, sizeof(note), "%s[%s: the output was cut here: %lu more bytes did not fit in %u KB]\n",
                     cap.len && cap.buf[cap.len - 1] != '\n' ? "\n" : "", kargv[0], cap.cut, (u_int)(KCMD_CAPTURE_MAX / 1024));
            w = write_all(call.out, note, strlen(note));
        }
        if(w) {
            printf("    %s: its output could not be written to descriptor 1: %s\n", kargv[0], strerror(w));
            if(r == 0) r = 1;                                /* the command worked; keeping what it said did not */
        }
    }
out:
    kcmd_release(&call);                                 /* everything let go of, then off the list */
    kcmd_forget(&call);
    return r;
}

static int sys_kcmdinfo(int index, struct kcmd_info *out) {
    struct kcmd_info k;
    int r;
    if(!ubuf(out, sizeof(*out), true)) return -EFAULT;
    r = shell_command_info(index, &k);
    if(r == 0) memcpy(out, &k, sizeof(k));
    return r;
}

static u_long *syscall_trap(struct trapframe *tf) {
    u_long n = tf->eax, a = tf->ebx, b = tf->ecx, c = tf->edx;
    int r;

    if(!curTask || !curTask->uvm) {      /* int 0x80 from kernel code: nothing to do for it */
        tf->eax = (u_long)-ENOSYS;
        return (u_long*)tf;
    }
    curTask->in_call |= TC_SYSCALL;      /* 10/6/26: interrupts are still off: a kill waits for the call from here */
    asm volatile("sti");                 /* system calls run with interrupts on, like any task code */
    switch(n) {
    case SYS_exit:    task_exit((int)a);                                     /* does not return */
    case SYS_write:   r = sys_write((int)a, (const void *)b, c); break;
    case SYS_read:    r = sys_read((int)a, (void *)b, c); break;
    case SYS_open:    r = sys_open((const char *)a, (int)b); break;
    case SYS_close:   r = fd_close((int)a); break;
    case SYS_lseek:   r = fd_get((int)a) ? vfs_lseek(fd_get((int)a), (i32)b, (int)c) : -EBADF; break;
    case SYS_readdir: r = sys_readdir((int)a, (struct dirent *)b); break;
    case SYS_stat:    r = sys_stat((const char *)a, (struct stat *)b); break;
    case SYS_chdir:   r = ustr((const char *)a) ? vfs_chdir((const char *)a) : -EFAULT; break;
    case SYS_getcwd:  r = sys_getcwd((char *)a, b); break;
    case SYS_mkdir:   r = ustr((const char *)a) ? vfs_mkdir((const char *)a) : -EFAULT; break;
    case SYS_unlink:  r = ustr((const char *)a) ? vfs_unlink((const char *)a) : -EFAULT; break;
    case SYS_rmdir:   r = ustr((const char *)a) ? vfs_rmdir((const char *)a) : -EFAULT; break;
    case SYS_getpid:  r = (int)curTask->pid; break;
    case SYS_sleep:   sleep_intr(a); r = 0; break;        /* 10/6/26: a kill cuts it short */
    case SYS_yield:   yield(); r = 0; break;
    case SYS_spawn:   r = sys_spawn((const char *)a, (char *const *)b); break;
    case SYS_wait:    r = sys_wait((int)a, (int *)b, (int)c); break;
    case SYS_sbrk:    r = (int)uvm_sbrk(curTask->uvm, (long)a); break;
    case SYS_uptime:  r = (int)(getticks() * (1000 / HZ)); break;
    case SYS_kill:    r = sys_kill((int)a); break;
    case SYS_rename:  r = sys_rename((const char *)a, (const char *)b); break;
    case SYS_fstat:   r = sys_fstat((int)a, (struct stat *)b); break;
    case SYS_ftruncate: r = vfs_ftruncate(fd_get((int)a), b); break;
    case SYS_fsync:   r = vfs_fsync(fd_get((int)a)); break;
    case SYS_sync:    r = vfs_sync(); break;
    case SYS_statfs:  r = sys_statfs((const char *)a, (struct statfs *)b); break;
    case SYS_dup2:    r = sys_dup2((int)a, (int)b); break;
    case SYS_time:    r = sys_time(); break;
    case SYS_ttyctl:  r = tty_ctl(curTask->tty, (int)a, b); break;
    case SYS_spawnfd: r = sys_spawnfd((const char *)a, (char *const *)b, (const int *)c); break;
    case SYS_kcmd:    r = sys_kcmd((char *const *)a); break;
    case SYS_kcmdinfo: r = sys_kcmdinfo((int)a, (struct kcmd_info *)b); break;
    default:          r = -ENOSYS; break;
    }
    tf->eax = (u_long)r;
    task_call_leave(TC_SYSCALL);         /* 10/6/26: a kill that waited for this call ends the task here */
    return (u_long*)tf;
}

void init_syscalls(void) {
    trap_set_handler(T_SYSCALL, syscall_trap);
}
