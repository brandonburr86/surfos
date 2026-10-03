/*
SurfOS user library (libsurf)
----------------------
File: surf.h    Date: 10/3/26 (roadmap P2)
----------------------
System call wrappers and the small libc user programs link against. The
string, memory and formatting functions come from lib/blibc, built a second
time for user space; printf() writes to descriptor 1.
*/

#ifndef _SURF_H
#define _SURF_H

#include <surfos/types.h>
#include <surfos/syscall.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

/* the kernel's structures and constants that cross the boundary */
#define VFS_NAME_MAX 255
#define VFS_PATH_MAX 256
#define VN_FILE 1
#define VN_DIR  2
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0040
#define O_EXCL      0x0080
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_DIRECTORY 0x10000
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

struct dirent { char name[VFS_NAME_MAX + 1]; int type; u32 size; u32 mtime; };
struct stat { int type; u32 size; u32 mtime; u32 ino; char fs[8]; char dev[16]; };

/* system calls (negative return = -errno) */
void exit(int code) __attribute__((noreturn));
int write(int fd, const void *buf, u32 len);
int read(int fd, void *buf, u32 len);
int open(const char *path, int flags);
int close(int fd);
int lseek(int fd, i32 off, int whence);
int readdir(int fd, struct dirent *de);
int stat(const char *path, struct stat *st);
int chdir(const char *path);
int getcwd(char *buf, u32 size);
int mkdir(const char *path);
int unlink(const char *path);
int rmdir(const char *path);
int getpid(void);
int sleep(u32 ms);
int yield(void);
int spawn(const char *path, char *const argv[]);
int wait(int pid, int *status);
void *sbrk(long incr);
u32 uptime(void);
int kill(int pid);

/* libc bits */
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int printf(const char *fmt, ...);
int dprintf(int fd, const char *fmt, ...);
int puts(const char *s);
int putchar(int c);
int readline(char *buf, size_t size);       /* a line from fd 0 without the newline; -1 at end of input */
void *malloc(size_t n);
void free(void *p);
const char *strerror(int err);

#endif
