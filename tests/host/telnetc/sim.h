/* The simulated kernel of tests/host/test_telnetc.c (11/01/26): what user/telnet/session.c and main.c call in place of
   SurfOS's system calls (tests/host/telnetc/surf.h maps the names), defined by the test. */
#ifndef SIM_TELNETC_H
#define SIM_TELNETC_H
#include <stdio.h>
#include <sys/types.h>

struct pollfd;
ssize_t sim_read(int fd, void *buf, size_t n);
ssize_t sim_write(int fd, const void *buf, size_t n);
int sim_close(int fd);
int sim_poll(struct pollfd *p, unsigned int n, int timeout);
int sim_ttyctl(int op, unsigned int arg);
int sim_isatty(int fd);
unsigned int sim_uptime(void);
int sim_tcp_connect(unsigned int ip, unsigned short port);
int sim_net_resolve(const char *host, unsigned int *ip);
int sim_ipparse(const char *s, unsigned int *ip);
char *sim_ipstr(unsigned int ip, char buf[16]);
int sim_fprintf(FILE *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int sim_fflush(FILE *f);
#endif
