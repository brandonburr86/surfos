/*
SurfOS user library (libsurf)
----------------------
File: surf.h    Date: 10/3/26 (roadmap P2), 10/5/26
----------------------
What only SurfOS has: starting and waiting for programs, the terminal control
call of full-screen programs, the uptime and the raw directory call. Since
10/6/26 also the kernel's shell commands (kcmd), for the user shell surfsh.

Since 10/5/26 libsurf is a small C library with standard headers (stdio.h,
stdlib.h, string.h, unistd.h, fcntl.h, sys/stat.h, dirent.h, time.h, ...) and
the POSIX convention: a failed call returns -1 (or NULL) and sets errno. This
header still includes all of them, so code written for the first libsurf keeps
building; where its names clashed with the standard ones the standard won:
readdir() takes a DIR * (the raw call is getdent()), mkdir() takes a mode,
sleep() counts seconds (msleep() milliseconds), getcwd() returns the buffer,
and errors come back as -1 with errno instead of -errno.
*/

#ifndef _SURF_H
#define _SURF_H

#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <limits.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statfs.h>

/* the kernel's short type names, for the first programs */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef signed char i8;
typedef short i16;
typedef int i32;
typedef long long i64;

/* the kernel's names for what crosses the boundary (include/fs/vfs.h) */
#define VFS_NAME_MAX 255
#define VFS_PATH_MAX 256
#define VN_FILE S_IFREG
#define VN_DIR  S_IFDIR

/* programs */
int spawn(const char *path, char *const argv[]);                    /* the new pid; argv NULL: { path } */
int spawnfd(const char *path, char *const argv[], const int fds[3]);
    /* the same, with the child's descriptors 0, 1 and 2 sharing this process's fds[0..2]
       (-1: the console), for redirection: the files stay open here too */
int wait(int pid, int *status);     /* pid -1: any child. status: the exit code, -1 when it was killed */
#define WNOHANG 1                   /* 10/6/26: waitpid() answers 0 at once while the children run on */
int waitpid(int pid, int *status, int options);
int kill(int pid);
int yield(void);
unsigned int uptime(void);          /* milliseconds since boot */
int msleep(unsigned int ms);

/* the terminal (include/surfos/tty.h has the full description) */
#define TTY_RAW     1   /* arg 1: keys one at a time, no echo, special keys as escape sequences; 0: lines */
#define TTY_GETSIZE 2   /* returns (columns << 16) | rows */
#define TTY_PAGE    3   /* arg 1: this program's output and its children's is paged (--More--); 0: paging ends, if
                           this program turned it on. Under a shell that pages it, 1 and 0 only start a new screenful */
#define TTY_CAPS    4   /* returns the TTY_CAP_ flags */
#define TTY_SKIP    5   /* 10/6/26, arg 1: this program dropped keys it had read (the rest of a paste after an
                           Enter): what comes from the serial line or a pty's master is dropped until it pauses for
                           200 ms; Ctrl+C gets through. Raw mode's end does the same with keys queued. 0: end it */
#define TTY_CAP_UTF8  1 /* the console draws UTF-8: ASCII, code page 437's characters and SurfOS's ▸ ≋ … */
#define TTY_CAP_COLOR 2 /* the console draws the ANSI colours */
int ttyctl(int op, unsigned int arg);

/* the kernel's shell commands: lsblk, fdisk, mount, ps, ... (surfos/kcmd.h) */
#include <surfos/kcmd.h>
int kcmd(char *const argv[]);                   /* the command's status (0 success); -1 with errno ENOENT: none */
int kcmdinfo(int index, struct kcmd_info *out); /* 0; -1 with errno ENOENT after the last entry */

/* 10/6/26: network connections (surfos/sock.h has the rules): descriptors for read, write, close */
#include <surfos/sock.h>
int tcp_listen(unsigned short port);                    /* a listening descriptor */
int tcp_accept(int fd, struct netaddr *peer);           /* the next connection (waits); peer may be NULL */
int tcp_connect(unsigned int ip, unsigned short port);  /* ip in network byte order */
int netinfo(struct netinfo *ni);                        /* the default interface; -1 ENODEV without one */
char *ipstr(unsigned int ip, char buf[16]);             /* "192.168.1.20" from network byte order */
int ipparse(const char *s, unsigned int *ip);           /* "192.168.1.20" -> network byte order; 0, or -1 */

/* poll() is in <poll.h>; pseudo-terminals (surfos/pty.h) and processes (surfos/proc.h) */
#include <poll.h>
#include <surfos/pty.h>
#include <surfos/proc.h>
int openpty(void);                                      /* a pseudo-terminal's master descriptor */
int spawnpty(const char *path, char *const argv[], int masterfd);   /* a program on that pty: its pid */
int ptyctl(int masterfd, int op, unsigned int arg);     /* PTY_SETSIZE, PTY_INDEX */
int procinfo(int index, struct procinfo *pi);           /* task number index; -1 ENOENT after the last */
int detach(void);                                       /* init becomes this process's parent */

/* a directory entry from a descriptor opened with O_DIRECTORY: 0 an entry, 1 the end, -1 an error */
int getdent(int fd, struct dirent *de);

/* a line from descriptor 0 without its newline (stdout is flushed first); -1 at the end of input */
int readline(char *buf, size_t size);

#endif
