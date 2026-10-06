/*
SurfOS user library: POSIX system calls
----------------------
File: unistd.h  Date: 10/5/26
----------------------
Thin wrappers over int 0x80 (user/libsurf/syscalls.c): -1 and errno on failure.
*/

#ifndef _UNISTD_H
#define _UNISTD_H

#include <sys/types.h>
#include <stdint.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
int close(int fd);
off_t lseek(int fd, off_t off, int whence);
int unlink(const char *path);
int rmdir(const char *path);
int chdir(const char *path);
char *getcwd(char *buf, size_t size);           /* buf NULL: a malloc'ed buffer */
int dup2(int oldfd, int newfd);
int fsync(int fd);                              /* the file system's dirty blocks reach the device */
int ftruncate(int fd, off_t size);              /* FAT only shrinks files this way */
void sync(void);
pid_t getpid(void);
unsigned int sleep(unsigned int seconds);
int usleep(useconds_t usec);                    /* rounded up to whole milliseconds */
int isatty(int fd);                             /* 1 for the console (/dev/console, /dev/tty) */
void _exit(int code) __attribute__((noreturn)); /* no stdio flush, no atexit handlers */
void *sbrk(intptr_t incr);                      /* (void *)-1 when there is no more memory */

#endif
