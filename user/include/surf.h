/*
SurfOS user library (libsurf)
----------------------
File: surf.h    Date: 10/3/26 (roadmap P2), 10/5/26
----------------------
What only SurfOS has: starting and waiting for programs, the terminal control
call of full-screen programs, the uptime and the raw directory call.

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
int kill(int pid);
int yield(void);
unsigned int uptime(void);          /* milliseconds since boot */
int msleep(unsigned int ms);

/* the terminal (include/surfos/tty.h has the full description) */
#define TTY_RAW     1   /* arg 1: keys one at a time, no echo, special keys as escape sequences; 0: lines */
#define TTY_GETSIZE 2   /* returns (columns << 16) | rows */
int ttyctl(int op, unsigned int arg);

/* a directory entry from a descriptor opened with O_DIRECTORY: 0 an entry, 1 the end, -1 an error */
int getdent(int fd, struct dirent *de);

/* a line from descriptor 0 without its newline (stdout is flushed first); -1 at the end of input */
int readline(char *buf, size_t size);

#endif
