/*
SurfOS user library: system call wrappers and the tiny libc
--------------------
File: syscalls.c  Date: 10/3/26 (roadmap P2)
--------------------
*/

#include "../include/surf.h"

static inline long sc(long n, long a, long b, long c) {
    long r;
    asm volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

void exit(int code) { for(;;) sc(SYS_exit, code, 0, 0); }
int write(int fd, const void *buf, u32 len) { return (int)sc(SYS_write, fd, (long)buf, (long)len); }
int read(int fd, void *buf, u32 len) { return (int)sc(SYS_read, fd, (long)buf, (long)len); }
int open(const char *path, int flags) { return (int)sc(SYS_open, (long)path, flags, 0); }
int close(int fd) { return (int)sc(SYS_close, fd, 0, 0); }
int lseek(int fd, i32 off, int whence) { return (int)sc(SYS_lseek, fd, off, whence); }
int readdir(int fd, struct dirent *de) { return (int)sc(SYS_readdir, fd, (long)de, 0); }
int stat(const char *path, struct stat *st) { return (int)sc(SYS_stat, (long)path, (long)st, 0); }
int chdir(const char *path) { return (int)sc(SYS_chdir, (long)path, 0, 0); }
int getcwd(char *buf, u32 size) { return (int)sc(SYS_getcwd, (long)buf, (long)size, 0); }
int mkdir(const char *path) { return (int)sc(SYS_mkdir, (long)path, 0, 0); }
int unlink(const char *path) { return (int)sc(SYS_unlink, (long)path, 0, 0); }
int rmdir(const char *path) { return (int)sc(SYS_rmdir, (long)path, 0, 0); }
int getpid(void) { return (int)sc(SYS_getpid, 0, 0, 0); }
int sleep(u32 ms) { return (int)sc(SYS_sleep, (long)ms, 0, 0); }
int yield(void) { return (int)sc(SYS_yield, 0, 0, 0); }
int spawn(const char *path, char *const argv[]) { return (int)sc(SYS_spawn, (long)path, (long)argv, 0); }
int wait(int pid, int *status) { return (int)sc(SYS_wait, pid, (long)status, 0); }
void *sbrk(long incr) { return (void *)sc(SYS_sbrk, incr, 0, 0); }
u32 uptime(void) { return (u32)sc(SYS_uptime, 0, 0, 0); }
int kill(int pid) { return (int)sc(SYS_kill, pid, 0, 0); }

/**** stdio ****/

int printf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if(n > (int)sizeof(buf) - 1) n = sizeof(buf) - 1;
    return write(1, buf, n);
}

int dprintf(int fd, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if(n > (int)sizeof(buf) - 1) n = sizeof(buf) - 1;
    return write(fd, buf, n);
}

int puts(const char *s) {
    write(1, s, strlen(s));
    return write(1, "\n", 1);
}

int putchar(int c) {
    char ch = (char)c;
    return write(1, &ch, 1) == 1 ? c : -1;
}

int readline(char *buf, size_t size) {
    int n = read(0, buf, size - 1);
    if(n <= 0) return -1;
    if(buf[n - 1] == '\n') n--;
    buf[n] = 0;
    return n;
}

/**** a bump allocator on sbrk(); free() is a no-op ****/

void *malloc(size_t n) {
    void *p;
    n = (n + 15) & ~15UL;
    p = sbrk((long)n);
    if(!p) return NULL;
    return p;
}

void free(void *p) {
    (void)p;
}

const char *strerror(int err) {
    static const char *const msgs[] = {
        "Success", "Operation not permitted", "No such file or directory", "No such process", "Interrupted",
        "I/O error", "No such device or address", "Argument list too long", "Exec format error",
        "Bad file descriptor", "No child processes", "Try again", "Out of memory", "Permission denied",
        "Bad address", "Block device required", "Device or resource busy", "File exists", "Cross-device link",
        "No such device", "Not a directory", "Is a directory", "Invalid argument", "File table overflow",
        "Too many open files", "Not a typewriter", "Text file busy", "File too large", "No space left on device",
        "Illegal seek", "Read-only file system", "Too many links", "Broken pipe", "Math argument", "Result too large",
        "Deadlock", "File name too long", "No record locks", "Function not implemented", "Directory not empty",
    };
    if(err < 0) err = -err;
    if(err >= (int)(sizeof(msgs) / sizeof(msgs[0]))) return "Unknown error";
    return msgs[err];
}
