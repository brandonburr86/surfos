/*
SurfOS System Calls
----------------------
File: syscall.h Date: 10/3/26 (roadmap P1)
----------------------
Shared by the kernel (kernel/syscall.c) and user programs (user/libsurf).
int 0x80 with the number in eax and up to five arguments in ebx, ecx, edx,
esi, edi; the result comes back in eax, negative values are -errno.

10/6/26: calls 32 and 33 run and describe the kernel's shell commands for the
user shell, surfsh (surfos/kcmd.h).
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
#define SYS_wait    18   /* (int pid, int *status, int options): pid, -1 when there is nothing to wait for;
                            options WNOHANG (1, 10/6/26): 0 when children are left but none has ended */
#define SYS_sbrk    19   /* (long incr): the old break, 0 on failure */
#define SYS_uptime  20   /* (): milliseconds since boot */
#define SYS_kill    21   /* (int pid) */
/* 10/5/26 (files on USB storage, the editor) */
#define SYS_rename  22   /* (const char *from, const char *to) */
#define SYS_fstat   23   /* (int fd, struct stat *st) */
#define SYS_ftruncate 24 /* (int fd, u32 size) */
#define SYS_fsync   25   /* (int fd) */
#define SYS_sync    26   /* () */
#define SYS_statfs  27   /* (const char *path, struct statfs *st) */
#define SYS_dup2    28   /* (int oldfd, int newfd): newfd */
#define SYS_time    29   /* (): seconds since 1970, from the CMOS clock */
#define SYS_ttyctl  30   /* (int op, u32 arg): TTY_RAW, TTY_GETSIZE (surfos/tty.h) */
#define SYS_spawnfd 31   /* (const char *path, char *const argv[], const int fds[3]): pid; the child's 0, 1, 2 */
/* 10/6/26 (the SurfOS shell, surfsh): the kernel's shell commands, surfos/kcmd.h */
#define SYS_kcmd    32   /* (char *const argv[]): the command's status, -ENOENT when there is none */
#define SYS_kcmdinfo 33  /* (int index, struct kcmd_info *out): 0, -ENOENT after the last */
#define NR_SYSCALLS 34

#define SPAWN_MAX_ARGS 32

#endif
