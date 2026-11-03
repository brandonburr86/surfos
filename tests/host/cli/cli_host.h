/* Force-included where a tool of the diff track is built for the host (11/01/26): the tools as they are, on the host's C
   library and file system, so that tests/test_cli_diff.py can run them beside the real ones. The host's headers come
   first; then SurfOS's fields of struct stat (fileutil.h and saveutil.h use them) get the POSIX names, in the tools only. */
#ifndef CLI_HOST_H
#define CLI_HOST_H
#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#ifndef S_ISREG
#error no S_ISREG
#endif
/* CLI_FAIL=op:n[:errno] in the environment makes the nth and later calls of write (to a file, not 0 to 2), fsync or
   rename fail, as a full disk or a stick pulled out would: tests/test_cli_diff.py pulls patch at each one */
static inline int cli_fail(const char *op) {
    static int seen;
    const char *spec = getenv("CLI_FAIL"), *e;
    size_t n = strlen(op);
    int at;
    if(!spec || strncmp(spec, op, n) || spec[n] != ':') return 0;
    at = atoi(spec + n + 1);
    e = strchr(spec + n + 1, ':');
    if(++seen < at) return 0;
    errno = e ? atoi(e + 1) : ENOSPC;
    return 1;
}
static inline long cli_write(int fd, const void *p, size_t n) {
    return fd > 2 && cli_fail("write") ? -1 : (long)write(fd, p, n);
}
static inline int cli_fsync(int fd) {
    return cli_fail("fsync") ? -1 : fsync(fd);
}
static inline int cli_rename(const char *from, const char *to) {
    return cli_fail("rename") ? -1 : rename(from, to);
}
#define write cli_write
#define fsync cli_fsync
#define rename cli_rename
/* SurfOS's open() takes no mode, and saveutil.h gives none: here one is needed, or the file comes out unreadable */
static inline int cli_open(const char *path, int flags) {
    return open(path, flags, 0666);
}
#define open(path, ...) cli_open(path, CLI_FIRST(__VA_ARGS__, 0))
#define CLI_FIRST(a, ...) (a)
#define type st_mode
#define size st_size
#define mtime st_mtime
#endif
