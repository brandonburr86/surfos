/* Force-included where Paint's files.c (user/wpaint/) is built for its host test (tests/host/test_wpaint_files.c, 11/05/26,
   the desktop applications, track paint). The host's headers come first; then SurfOS's names of struct stat's fields,
   which user/bin/saveutil.h uses, for the host's; a mode for open(), which SurfOS's takes and ignores and saveutil.h
   leaves out; and the file calls made to fail on request: from the nth call on (wph_at[], counted from 1) each fails
   with wph_errno[] (a read with errno 0 says the file ended), as a full disk, a stick pulled out or a rename that
   failed halfway would. wph_short makes every write take at most that many bytes. With WPH_NAMES_ONLY (the test
   itself) only the names are declared. */
#ifndef WPAINT_HOST_H
#define WPAINT_HOST_H

enum { WPH_OPEN, WPH_READ, WPH_WRITE, WPH_FSYNC, WPH_CLOSE, WPH_RENAME, WPH_UNLINK, WPH_CALLS };
extern int wph_at[WPH_CALLS], wph_errno[WPH_CALLS], wph_count[WPH_CALLS], wph_short;

#ifndef WPH_NAMES_ONLY
#define _DARWIN_C_SOURCE
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static inline int wph_fail(int op) {
    wph_count[op]++;
    if(!wph_at[op] || wph_count[op] < wph_at[op]) return 0;
    errno = wph_errno[op];
    return 1;
}
static inline int wph_open(const char *path, int flags) {
    return wph_fail(WPH_OPEN) ? -1 : open(path, flags, 0666);
}
static inline long wph_read(int fd, void *b, size_t n) {
    if(wph_fail(WPH_READ)) return errno ? -1 : 0;
    return (long)read(fd, b, n);
}
static inline long wph_write(int fd, const void *b, size_t n) {
    if(wph_fail(WPH_WRITE)) return errno ? -1 : 0;
    return (long)write(fd, b, wph_short > 0 && n > (size_t)wph_short ? (size_t)wph_short : n);
}
static inline int wph_fsync(int fd) {
    return wph_fail(WPH_FSYNC) ? -1 : fsync(fd);
}
static inline int wph_close(int fd) {
    int r = close(fd);
    return wph_fail(WPH_CLOSE) ? -1 : r;
}
static inline int wph_rename(const char *from, const char *to) {
    return wph_fail(WPH_RENAME) ? -1 : rename(from, to);
}
static inline int wph_unlink(const char *path) {
    return wph_fail(WPH_UNLINK) ? -1 : unlink(path);
}
#define open(path, ...) wph_open(path, WPH_FIRST(__VA_ARGS__, 0))
#define WPH_FIRST(a, ...) (a)
#define read wph_read
#define write wph_write
#define fsync wph_fsync
#define close wph_close
#define rename wph_rename
#define unlink wph_unlink
#define type st_mode
#define size st_size
#define mtime st_mtime
#endif
#endif
