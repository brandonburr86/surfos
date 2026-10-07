/* Host-side test shim (tests/host/test_telnetd.c): the SurfOS user API that telnetd's server
   (user/telnetd/server.c) uses, mapped to the simulated kernel of the test. From the review of
   10/6/26. */
#ifndef SIM_SURF_H
#define SIM_SURF_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/types.h>

struct netaddr { unsigned int ip; unsigned short port; unsigned short zero; };
struct pollfd { int fd; short events; short revents; };
typedef unsigned int nfds_t;
#define POLLIN   0x001
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020
#define PTY_SETSIZE 1
#define PTY_INDEX   2
#define PTY_MAX     8
#ifndef WNOHANG
#define WNOHANG 1
#endif
#ifndef O_WRONLY
#define O_WRONLY 1
#endif

ssize_t sim_read(int fd, void *buf, size_t n);
ssize_t sim_write(int fd, const void *buf, size_t n);
int sim_close(int fd);
int sim_open(const char *path, int flags);
int sim_poll(struct pollfd *fds, nfds_t n, int timeout);
void sim_exit(int code) __attribute__((noreturn));
int sim_waitpid(int pid, int *status, int options);
int sim_detach(void);
int sim_tcp_accept(int fd, struct netaddr *peer);
int sim_openpty(void);
int sim_spawnpty(const char *path, char *const argv[], int master);
int sim_ptyctl(int master, int op, unsigned int arg);
char *sim_ipstr(unsigned int ip, char buf[16]);
unsigned int sim_uptime(void);

#define read sim_read
#define write sim_write
#define close sim_close
#define open sim_open
#define poll sim_poll
#define _exit sim_exit
#define waitpid sim_waitpid
#define detach sim_detach
#define tcp_accept sim_tcp_accept
#define openpty sim_openpty
#define spawnpty sim_spawnpty
#define ptyctl sim_ptyctl
#define ipstr sim_ipstr
#define uptime sim_uptime
#endif
