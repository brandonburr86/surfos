/*
SurfOS System Calls
----------------------
File: syscall.h Date: 10/3/26 (roadmap P1)
----------------------
Shared by the kernel (kernel/syscall.c) and user programs (user/libsurf).
int 0x80 with the number in eax and up to five arguments in ebx, ecx, edx,
esi, edi; the result comes back in eax, negative values are -errno.
*/

#ifndef _SURFOS_SYSCALL_H
#define _SURFOS_SYSCALL_H

#include <surfos/types.h>

#define SYS_exit     1   /* (int code) */
#define SYS_write    2   /* (int fd, const void *buf, u32 len) */
#define SYS_read     3   /* (int fd, void *buf, u32 len) */
#define SYS_open     4   /* (const char *path, int flags) */
#define SYS_close    5   /* (int fd) */
#define SYS_lseek    6   /* (int fd, i32 off, int whence) */
#define SYS_readdir  7   /* (int fd, struct dirent *de): 0 entry, 1 end */
#define SYS_stat     8   /* (const char *path, struct stat *st) */
#define SYS_chdir    9   /* (const char *path) */
#define SYS_getcwd  10   /* (char *buf, u32 size) */
#define SYS_mkdir   11   /* (const char *path) */
#define SYS_unlink  12   /* (const char *path) */
#define SYS_rmdir   13   /* (const char *path) */
#define SYS_getpid  14   /* () */
#define SYS_sleep   15   /* (u32 ms) */
#define SYS_yield   16   /* () */
#define SYS_spawn   17   /* (const char *path, char *const argv[]): pid */
#define SYS_wait    18   /* (int pid, int *status): pid, -1 when there is nothing to wait for */
#define SYS_sbrk    19   /* (long incr): the old break, 0 on failure */
#define SYS_uptime  20   /* (): milliseconds since boot */
#define SYS_kill    21   /* (int pid) */
#define NR_SYSCALLS 22

#define SPAWN_MAX_ARGS 32

#endif
