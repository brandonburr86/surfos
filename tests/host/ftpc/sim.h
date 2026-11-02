/* The simulated kernel of tests/host/test_ftpcs.c (11/01/26): what user/ftp/session.c, commands.c and main.c
   call in place of SurfOS's system calls (tests/host/ftpc/surf.h maps the names), defined by the test. */
#ifndef SIM_FTPC_H
#define SIM_FTPC_H
#include <stdio.h>
#include <stdarg.h>
#include <sys/types.h>

struct sim_stat {                       /* SurfOS's layout and names (user/include/sys/stat.h) */
    int type;
    unsigned int size, mtime, ino;
    char fs[8], dev[16];
};
struct pollfd;
struct dirent;

int sim_open(const char *path, int flags, ...);
int sim_close(int fd);
ssize_t sim_read(int fd, void *buf, size_t n);
ssize_t sim_write(int fd, const void *buf, size_t n);
int sim_fsync(int fd);
int sim_ftruncate(int fd, off_t size);
int sim_stat(const char *path, struct sim_stat *st);
int sim_rename(const char *from, const char *to);
int sim_unlink(const char *path);
int sim_chdir(const char *path);
char *sim_getcwd(char *buf, size_t size);
void *sim_opendir(const char *path);
struct dirent *sim_readdir(void *d);
int sim_closedir(void *d);
int sim_poll(struct pollfd *p, unsigned int n, int timeout);
int sim_ttyctl(int op, unsigned int arg);
int sim_isatty(int fd);
unsigned int sim_uptime(void);
int sim_readline(char *buf, size_t size);
int sim_tcp_connect(unsigned int ip, unsigned short port);
int sim_net_resolve(const char *host, unsigned int *ip);
int sim_ipparse(const char *s, unsigned int *ip);
char *sim_ipstr(unsigned int ip, char buf[16]);
int sim_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int sim_fprintf(FILE *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int sim_vfprintf(FILE *f, const char *fmt, va_list ap) __attribute__((format(printf, 2, 0)));
int sim_fputs(const char *s, FILE *f);
int sim_fputc(int c, FILE *f);
int sim_fflush(FILE *f);
int sim_ferror(FILE *f);
char *sim_strdup(const char *s);
#endif
