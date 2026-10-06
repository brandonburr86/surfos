/*
SurfOS system calls kept outside kernel/syscall.c
----------------------
File: sysimpl.h Date: 10/6/26
----------------------
Kernel only. The telnet server's calls (34-43, surfos/syscall.h) live with what they
work on: net/sock.c (network connections), fs/poll.c (poll), kernel/pty.c
(pseudo-terminals), kernel/proc.c (the process list, detach). syscall_trap() calls them
with the user's arguments as they are: each checks its own pointers with user_buf() and
user_str(), and copies an argv with user_argv(), as kernel/syscall.c does for its own.
They return a value or -errno.
*/

#ifndef _SURFOS_SYSIMPL_H
#define _SURFOS_SYSIMPL_H

#include <surfos/types.h>

struct netaddr;
struct netinfo;
struct pollfd;
struct procinfo;

/* kernel/syscall.c */
bool user_str(const char *s);                                   /* a NUL-terminated string the caller may read */
bool user_buf(const void *p, u_long len, bool write);           /* len bytes the caller may read (or write) */
int user_argv(const char *path, char *const argv[], char **kargv);   /* argc, or -errno; kargv points into user memory */

/* net/sock.c */
int sys_listen(u16 port);
int sys_accept(int fd, struct netaddr *peer);
int sys_connect(u32 ip, u16 port);
int sys_netinfo(struct netinfo *ni);

/* fs/poll.c */
int sys_poll(struct pollfd *fds, int nfds, int timeout_ms);

/* kernel/pty.c */
int sys_openpty(void);
int sys_spawnpty(const char *path, char *const argv[], int masterfd);
int sys_ptyctl(int masterfd, int op, u32 arg);

/* kernel/proc.c */
int sys_procinfo(int index, struct procinfo *pi);
int sys_detach(void);

#endif
