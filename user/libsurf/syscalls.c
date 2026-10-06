/*
SurfOS user library: system call wrappers
--------------------
File: syscalls.c  Date: 10/3/26 (roadmap P2), rewritten 10/5/26
--------------------
int 0x80 with the number in eax and the arguments in ebx, ecx and edx. The
kernel answers -errno on failure; the wrappers turn that into -1 and errno.
*/

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <dirent.h>
#include <surf.h>
#include "sysnum.h"

/* the kernel's layouts (kernel/syscall.c checks the same sizes) */
typedef char stat_layout_is_fixed[sizeof(struct stat) == 40 ? 1 : -1];
typedef char statfs_layout_is_fixed[sizeof(struct statfs) == 116 ? 1 : -1];
typedef char dirent_layout_is_fixed[sizeof(struct dirent) == 268 ? 1 : -1];
typedef char kcmd_info_layout_is_fixed[sizeof(struct kcmd_info) == 176 ? 1 : -1];
typedef char netaddr_layout_is_fixed[sizeof(struct netaddr) == 8 ? 1 : -1];
typedef char netinfo_layout_is_fixed[sizeof(struct netinfo) == 32 ? 1 : -1];
typedef char pollfd_layout_is_fixed[sizeof(struct pollfd) == 8 ? 1 : -1];
typedef char procinfo_layout_is_fixed[sizeof(struct procinfo) == 36 ? 1 : -1];

int errno;

static inline long sc(long n, long a, long b, long c) {
    long r;
    asm volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

/* -errno from the kernel becomes -1 with errno set */
static long ret(long r) {
    if(r < 0 && r >= -4095) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

void _exit(int code) { for(;;) sc(SYS_exit, code, 0, 0); }
ssize_t read(int fd, void *buf, size_t n) { return (ssize_t)ret(sc(SYS_read, fd, (long)buf, (long)n)); }
ssize_t write(int fd, const void *buf, size_t n) { return (ssize_t)ret(sc(SYS_write, fd, (long)buf, (long)n)); }
int close(int fd) { return (int)ret(sc(SYS_close, fd, 0, 0)); }
off_t lseek(int fd, off_t off, int whence) { return (off_t)ret(sc(SYS_lseek, fd, off, whence)); }
int unlink(const char *path) { return (int)ret(sc(SYS_unlink, (long)path, 0, 0)); }
int rmdir(const char *path) { return (int)ret(sc(SYS_rmdir, (long)path, 0, 0)); }
int chdir(const char *path) { return (int)ret(sc(SYS_chdir, (long)path, 0, 0)); }
int dup2(int oldfd, int newfd) { return (int)ret(sc(SYS_dup2, oldfd, newfd, 0)); }
int fsync(int fd) { return (int)ret(sc(SYS_fsync, fd, 0, 0)); }
void sync(void) { sc(SYS_sync, 0, 0, 0); }
pid_t getpid(void) { return (pid_t)sc(SYS_getpid, 0, 0, 0); }
int stat(const char *path, struct stat *st) { return (int)ret(sc(SYS_stat, (long)path, (long)st, 0)); }
int fstat(int fd, struct stat *st) { return (int)ret(sc(SYS_fstat, fd, (long)st, 0)); }
int mkdir(const char *path, mode_t mode) { (void)mode; return (int)ret(sc(SYS_mkdir, (long)path, 0, 0)); }
int statfs(const char *path, struct statfs *st) { return (int)ret(sc(SYS_statfs, (long)path, (long)st, 0)); }
int rename(const char *from, const char *to) { return (int)ret(sc(SYS_rename, (long)from, (long)to, 0)); }

int open(const char *path, int flags, ...) {
    return (int)ret(sc(SYS_open, (long)path, flags, 0));
}

int creat(const char *path, mode_t mode) {
    (void)mode;
    return open(path, O_WRONLY | O_CREAT | O_TRUNC);
}

int ftruncate(int fd, off_t size) {
    if(size < 0) { errno = EINVAL; return -1; }
    return (int)ret(sc(SYS_ftruncate, fd, size, 0));
}

char *getcwd(char *buf, size_t size) {
    char *mine = NULL;
    if(!buf) {
        if(!size) size = PATH_MAX;
        buf = mine = malloc(size);
        if(!buf) { errno = ENOMEM; return NULL; }
    }
    if(ret(sc(SYS_getcwd, (long)buf, (long)size, 0)) < 0) {
        free(mine);
        return NULL;
    }
    return buf;
}

unsigned int sleep(unsigned int seconds) {
    sc(SYS_sleep, (long)(seconds * 1000), 0, 0);
    return 0;
}

int usleep(useconds_t usec) {
    sc(SYS_sleep, (long)((usec + 999) / 1000), 0, 0);
    return 0;
}

void *sbrk(intptr_t incr) {
    long r = sc(SYS_sbrk, incr, 0, 0);
    if(r == 0) { errno = ENOMEM; return (void *)-1; }    /* the kernel says 0 when it cannot */
    return (void *)r;
}

time_t time(time_t *t) {
    time_t now = (time_t)(unsigned long)sc(SYS_time, 0, 0, 0);
    if(t) *t = now;
    return now;
}

/* the console is the devfs file behind /dev/console and /dev/tty */
int isatty(int fd) {
    struct stat a, b;
    if(fstat(fd, &a) < 0) return 0;
    if(!strcmp(a.fs, "devfs") && ((stat("/dev/console", &b) == 0 && b.ino == a.ino) ||
                                 (stat("/dev/tty", &b) == 0 && b.ino == a.ino))) return 1;
    errno = ENOTTY;
    return 0;
}

/**** SurfOS calls (surf.h) ****/

int spawn(const char *path, char *const argv[]) { return (int)ret(sc(SYS_spawn, (long)path, (long)argv, 0)); }
int spawnfd(const char *path, char *const argv[], const int fds[3]) {
    return (int)ret(sc(SYS_spawnfd, (long)path, (long)argv, (long)fds));
}
int kill(int pid) { return (int)ret(sc(SYS_kill, pid, 0, 0)); }
int yield(void) { return (int)sc(SYS_yield, 0, 0, 0); }
unsigned int uptime(void) { return (unsigned int)sc(SYS_uptime, 0, 0, 0); }
int msleep(unsigned int ms) { sc(SYS_sleep, (long)ms, 0, 0); return 0; }
int ttyctl(int op, unsigned int arg) { return (int)ret(sc(SYS_ttyctl, op, (long)arg, 0)); }

/* 10/6/26: the kernel's shell commands (surfos/kcmd.h) */
int kcmd(char *const argv[]) { return (int)ret(sc(SYS_kcmd, (long)argv, 0, 0)); }
int kcmdinfo(int index, struct kcmd_info *out) { return (int)ret(sc(SYS_kcmdinfo, index, (long)out, 0)); }

/* 10/6/26: network connections, poll, pseudo-terminals, processes (surfos/sock.h, poll.h, pty.h, proc.h) */
int tcp_listen(unsigned short port) { return (int)ret(sc(SYS_listen, port, 0, 0)); }
int tcp_accept(int fd, struct netaddr *peer) { return (int)ret(sc(SYS_accept, fd, (long)peer, 0)); }
int tcp_connect(unsigned int ip, unsigned short port) { return (int)ret(sc(SYS_connect, (long)ip, port, 0)); }
int netinfo(struct netinfo *ni) { return (int)ret(sc(SYS_netinfo, (long)ni, 0, 0)); }
int poll(struct pollfd *fds, nfds_t nfds, int timeout_ms) { return (int)ret(sc(SYS_poll, (long)fds, (long)nfds, timeout_ms)); }
int openpty(void) { return (int)ret(sc(SYS_openpty, 0, 0, 0)); }
int spawnpty(const char *path, char *const argv[], int masterfd) {
    return (int)ret(sc(SYS_spawnpty, (long)path, (long)argv, masterfd));
}
int ptyctl(int masterfd, int op, unsigned int arg) { return (int)ret(sc(SYS_ptyctl, masterfd, op, (long)arg)); }
int procinfo(int index, struct procinfo *pi) { return (int)ret(sc(SYS_procinfo, index, (long)pi, 0)); }
int detach(void) { return (int)ret(sc(SYS_detach, 0, 0, 0)); }
int getdent(int fd, struct dirent *de) { return (int)ret(sc(SYS_readdir, fd, (long)de, 0)); }

int wait(int pid, int *status) {
    return waitpid(pid, status, 0);
}

/* 10/6/26: options WNOHANG: 0 at once while children are left but none has ended */
int waitpid(int pid, int *status, int options) {
    long r = sc(SYS_wait, pid, (long)status, options);
    if(r == -1) { errno = ECHILD; return -1; }          /* the kernel's "nothing to wait for" */
    return (int)ret(r);
}
