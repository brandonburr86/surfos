/*
SurfOS system calls kept outside kernel/syscall.c
----------------------
File: sysimpl.h Date: 10/6/26
----------------------
Kernel only. The telnet server's calls (34-43, surfos/syscall.h) live with what they
work on: net/sock.c (network connections), fs/poll.c (poll), kernel/pty.c
(pseudo-terminals), kernel/proc.c (the process list, detach); 10/24/26: fs/pipe.c (pipes);
10/25/26: kernel/win.c (windows for programs).
syscall_trap() calls them
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
struct sysinfo;
struct win_open;

/* kernel/syscall.c */
bool user_str(const char *s);                                   /* a NUL-terminated string the caller may read */
bool user_buf(const void *p, u_long len, bool write);           /* len bytes the caller may read (or write) */
int user_argv(const char *path, char *const argv[], char **kargv);   /* argc, or -errno; kargv points into user memory */

/* kernel/websvc.c */
int sys_resolve(const char *host, u32 *ip);
int sys_getentropy(void *buf, size_t len);

/* net/sock.c (10/6/26: a port as the caller passed it, -EINVAL above 65535) */
int sys_listen(u32 port, u32 backlog);              /* 10/17/26: backlog 0: the default */
int sys_accept(int fd, struct netaddr *peer);
int sys_connect(u32 ip, u32 port, u32 flags);       /* 10/24/26: flags CONNECT_NOWAIT, or 0 */
int sys_netinfo(struct netinfo *ni);
int sys_shutdown(int fd, u32 how);                  /* 10/24/26 (the network tools): the half-close */
int sys_sockaddr(int fd, struct netaddr *local, struct netaddr *peer);

/* fs/poll.c */
int sys_poll(struct pollfd *fds, int nfds, int timeout_ms);

/* kernel/pty.c */
int sys_openpty(void);
int sys_spawnpty(const char *path, char *const argv[], int masterfd);
int sys_ptyctl(int masterfd, int op, u32 arg);

/* kernel/proc.c */
int sys_procinfo(int index, struct procinfo *pi, u32 size);
int sys_detach(void);
int sys_sysinfo(struct sysinfo *si);                /* 11/01/26 */

/* fs/pipe.c (10/24/26) */
int sys_pipe(int *fds);

/* kernel/win.c (10/25/26) */
int sys_winopen(const struct win_open *o);
int sys_winctl(int fd, int op, void *arg);

#endif
