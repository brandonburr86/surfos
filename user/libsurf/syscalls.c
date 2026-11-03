/*
SurfOS user library: system call wrappers
--------------------
File: syscalls.c  Date: 10/3/26 (roadmap P2), rewritten 10/5/26
--------------------
int 0x80 with the number in eax and the arguments in ebx, ecx and edx. The
kernel answers -errno on failure; the wrappers turn that into -1 and errno.

10/24/26 (pipelines): pipe(), and a write to a pipe nobody reads any more ends the
program with status 141, as SIGPIPE does elsewhere, unless it asked for the error
(broken_pipe_exit(0)). Only a pipe ends it: a connection's EPIPE is an error.
spawnx(), and spawn() and spawnfd() over it with environ (env.c).
10/25/26 (windows for programs): winopen() and winctl(), bare (surfos/win.h).
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
typedef char procinfo_layout_is_fixed[sizeof(struct procinfo) == 48 ? 1 : -1];   /* 11/01/26: was 36 */
typedef char sysinfo_layout_is_fixed[sizeof(struct sysinfo) == 32 ? 1 : -1];     /* 11/01/26 */
typedef char spawnx_layout_is_fixed[sizeof(struct spawnx) == 28 ? 1 : -1];     /* 10/24/26 */
typedef char win_open_layout_is_fixed[sizeof(struct win_open) == 88 ? 1 : -1];  /* 10/25/26 */
typedef char win_present_layout_is_fixed[sizeof(struct win_present) == 16 ? 1 : -1];
typedef char win_info_layout_is_fixed[sizeof(struct win_info) == 20 ? 1 : -1];
typedef char win_event_layout_is_fixed[sizeof(struct win_event) == 16 ? 1 : -1];
typedef char win_clip_layout_is_fixed[sizeof(struct win_clip) == 8 ? 1 : -1];    /* 10/26/26 */

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

/* 10/24/26: a broken pipe ends the program (surf.h) */
static int pipe_exits = 1;

void broken_pipe_exit(int on) { pipe_exits = on; }

static int is_pipe(int fd) {
    struct stat st;
    return fstat(fd, &st) == 0 && !strcmp(st.fs, "pipefs");
}

ssize_t write(int fd, const void *buf, size_t n) {
    long r = sc(SYS_write, fd, (long)buf, (long)n);
    if(r == -EPIPE && pipe_exits && is_pipe(fd)) _exit(141);        /* 128 + 13, SIGPIPE's number */
    return (ssize_t)ret(r);
}

int pipe(int fds[2]) { return (int)ret(sc(SYS_pipe, (long)fds, 0, 0)); }
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

/* 10/24/26: spawn() and spawnfd() are spawnx() with this program's environment (surfos/spawn.h) */
int spawnx(const char *path, char *const argv[], const struct spawnx *o) {
    return (int)ret(sc(SYS_spawnx, (long)path, (long)argv, (long)o));
}
int spawnfd(const char *path, char *const argv[], const int fds[3]) {
    struct spawnx o;
    int i;
    for(i = 0; i < 3; i++) o.fds[i] = fds ? fds[i] : -1;
    o.fds[3] = -1;
    o.envp = environ;
    o.job = 0;
    o.flags = 0;
    return spawnx(path, argv, &o);
}
int spawn(const char *path, char *const argv[]) { return spawnfd(path, argv, NULL); }
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
int tcp_listen_backlog(unsigned short port, int backlog) {
    return (int)ret(sc(SYS_listen, port, backlog < 0 ? 0 : backlog, 0));
}
int tcp_accept(int fd, struct netaddr *peer) { return (int)ret(sc(SYS_accept, fd, (long)peer, 0)); }
int tcp_connect(unsigned int ip, unsigned short port) { return (int)ret(sc(SYS_connect, (long)ip, port, 0)); }
int getentropy(void *buf, size_t len) { return (int)ret(sc(SYS_getentropy, (long)buf, (long)len, 0)); }
int net_resolve(const char *host, unsigned int *ip) { return (int)ret(sc(SYS_resolve, (long)host, (long)ip, 0)); }
int netinfo(struct netinfo *ni) { return (int)ret(sc(SYS_netinfo, (long)ni, 0, 0)); }
/* 10/24/26 (the network tools): the half-close, both ends of a socket, a connection not waited for */
int shutdown(int fd, int how) { return (int)ret(sc(SYS_shutdown, fd, how, 0)); }
int tcp_connect_start(unsigned int ip, unsigned short port) {
    return (int)ret(sc(SYS_connect, (long)ip, port, CONNECT_NOWAIT));
}
int tcp_addrs(int fd, struct netaddr *local, struct netaddr *peer) {
    return (int)ret(sc(SYS_sockaddr, fd, (long)local, (long)peer));
}
int poll(struct pollfd *fds, nfds_t nfds, int timeout_ms) { return (int)ret(sc(SYS_poll, (long)fds, (long)nfds, timeout_ms)); }
int openpty(void) { return (int)ret(sc(SYS_openpty, 0, 0, 0)); }
int spawnpty(const char *path, char *const argv[], int masterfd) {
    return (int)ret(sc(SYS_spawnpty, (long)path, (long)argv, masterfd));
}
int ptyctl(int masterfd, int op, unsigned int arg) { return (int)ret(sc(SYS_ptyctl, masterfd, op, (long)arg)); }
int procinfo(int index, struct procinfo *pi) { return (int)ret(sc(SYS_procinfo, index, (long)pi, sizeof(*pi))); }
int sysinfo(struct sysinfo *si) { return (int)ret(sc(SYS_sysinfo, (long)si, 0, 0)); }     /* 11/01/26 */
int detach(void) { return (int)ret(sc(SYS_detach, 0, 0, 0)); }
/* 10/25/26: windows on the desktop (surfos/win.h) */
int winopen(const struct win_open *o) { return (int)ret(sc(SYS_winopen, (long)o, 0, 0)); }
int winctl(int fd, int op, void *arg) { return (int)ret(sc(SYS_winctl, fd, op, (long)arg)); }
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

/* 10/25/26: how the child ended too (surfos/job.h) */
int waitx(int pid, struct waitx *w, int options) {
    return (int)ret(sc(SYS_waitx, pid, (long)w, options));
}

/* 10/25/26: job control (surfos/job.h) */
int jobctl(int op, int arg) {
    return (int)ret(sc(SYS_job, op, arg, 0));
}
