/*
SurfOS System Call Dispatch
--------------------
File: syscall.c Date: 10/3/26 (roadmap P1)
--------------------
int 0x80 lands here. Every pointer a program passes is checked against its
address space before the kernel touches it (uvm_check), so a bad pointer
costs the program -EFAULT and never the kernel a page fault with a lock held.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/task.h>
#include <surfos/trap.h>
#include <surfos/timer.h>
#include <surfos/syscall.h>
#include <surfos/process.h>
#include <mm/uvm.h>
#include <fs/vfs.h>
#include <blibc_common.h>

#define ARG_STR_MAX VFS_PATH_MAX

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

static int sys_spawn(const char *path, char *const argv[]) {
    char *kargv[SPAWN_MAX_ARGS];
    int argc = 0;
    if(!ustr(path)) return -EFAULT;
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
    return process_spawn(path, argc, kargv, curTask->con, 0);
}

static int sys_wait(int pid, int *status) {
    int st = 0, r;
    if(status && !ubuf(status, sizeof(int), true)) return -EFAULT;
    r = task_wait(pid < 0 ? WAIT_ANY : (u_long)pid, &st);
    if(r >= 0 && status) *status = st;
    return r;
}

static int sys_kill(int pid) {
    surf_task *t = task_find((u_long)pid);
    if(!t || pid <= 1) return -EINVAL;
    kill_task(t);
    if(t == curTask) yield();
    return 0;
}

static u_long *syscall_trap(struct trapframe *tf) {
    u_long n = tf->eax, a = tf->ebx, b = tf->ecx, c = tf->edx;
    int r;

    asm volatile("sti");                 /* system calls run with interrupts on, like any task code */
    if(!curTask || !curTask->uvm) {      /* int 0x80 from kernel code: nothing to do for it */
        tf->eax = (u_long)-ENOSYS;
        return (u_long*)tf;
    }
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
    case SYS_sleep:   sleep_ms(a); r = 0; break;
    case SYS_yield:   yield(); r = 0; break;
    case SYS_spawn:   r = sys_spawn((const char *)a, (char *const *)b); break;
    case SYS_wait:    r = sys_wait((int)a, (int *)b); break;
    case SYS_sbrk:    r = (int)uvm_sbrk(curTask->uvm, (long)a); break;
    case SYS_uptime:  r = (int)(getticks() * (1000 / HZ)); break;
    case SYS_kill:    r = sys_kill((int)a); break;
    default:          r = -ENOSYS; break;
    }
    tf->eax = (u_long)r;
    return (u_long*)tf;
}

void init_syscalls(void) {
    trap_set_handler(T_SYSCALL, syscall_trap);
}
