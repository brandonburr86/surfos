/*
SurfOS user program: wsheet
--------------------
File: io.c      Date: 11/05/26
--------------------
The Spreadsheet's files through the system calls (struct sheet_io in app.h; the tests put files in
memory in its place). A file is read as it comes, a piece at a time (app.c asks). A file the user
has is replaced only as user/bin/saveutil.h replaces one: the new contents go into a new file beside
it, which is fsynced and renamed over it, never by truncating it; a pair that looks like one a
failed rename left (a .tmp of the file's size and time beside it) is left alone and the save
refused, as Notepad refuses it. A file that is not there yet is made with O_EXCL, so that nothing of
somebody else's is ever written over, and deleted again when the save fails partway.
*/

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "saveutil.h"
#include "app.h"

struct out {
    int fd, err;
};

static int f_open(void *ctx, const char *path, long *size) {
    struct stat st;
    int fd = open(path, O_RDONLY), e;
    (void)ctx;
    if(fd < 0) return -1;
    if(fstat(fd, &st) < 0) e = errno;           /* st is read only once fstat() filled it (the review's L11) */
    else e = S_ISDIR(st.st_mode) ? EISDIR : 0;
    if(e) {
        close(fd);
        errno = e;
        return -1;
    }
    *size = (long)st.st_size;
    return fd;
}

static int f_read(void *ctx, int h, char *buf, int n) {
    (void)ctx;
    return (int)read(h, buf, (size_t)n);
}

static void f_close(void *ctx, int h) {
    (void)ctx;
    close(h);
}

static const char *base(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int put(void *ctx, const char *b, int n) {
    struct out *o = ctx;
    ssize_t w;
    while(n > 0) {
        w = write(o->fd, b, (size_t)n);
        if(w <= 0) {
            o->err = w < 0 ? errno : ENOSPC;
            return -1;
        }
        b += w;
        n -= (int)w;
    }
    return 0;
}

static int f_save(void *ctx, const char *path, const struct sheet *s, int kind, char *why, int size) {
    struct su_save sv;
    struct stat st;
    struct out o;
    int r, e;
    (void)ctx;
    o.err = 0;
    if(stat(path, &st) == 0) {                  /* the user's file: replaced, never truncated */
        if(S_ISDIR(st.st_mode)) {
            snprintf(why, (size_t)size, "it is a folder");
            return -1;
        }
        if(su_begin(&sv, path) < 0) {
            if(errno == EBUSY && sv.twin[0])
                snprintf(why, (size_t)size, "%s beside it may share its space: check the disk first", base(sv.twin));
            else if(errno == EEXIST) snprintf(why, (size_t)size, "no name is free beside it for the new contents");
            else snprintf(why, (size_t)size, "%s", strerror(errno));
            return -1;
        }
        o.fd = sv.fd;
        if(sh_write(s, kind, put, &o) < 0) {
            errno = o.err;
            su_abort(&sv);
            snprintf(why, (size_t)size, "%s", strerror(o.err));
            return -1;
        }
        r = su_commit(&sv);
        if(r == 0) return 0;
        if(r == SU_KEPT && sv.tmp[0])
            snprintf(why, (size_t)size, "%s: the new contents are in %s; check the disk", strerror(errno),
                     base(sv.tmp));
        else snprintf(why, (size_t)size, "%s%s", strerror(errno), r == SU_KEPT ? ": check the disk" : "");
        return -1;
    }
    o.fd = open(path, O_WRONLY | O_CREAT | O_EXCL);     /* a new file: nobody's else, and gone if this fails */
    if(o.fd < 0) {
        snprintf(why, (size_t)size, "%s", strerror(errno));
        return -1;
    }
    if(sh_write(s, kind, put, &o) < 0 || fsync(o.fd) < 0) {
        e = o.err ? o.err : errno;
        close(o.fd);
        unlink(path);
        snprintf(why, (size_t)size, "%s", strerror(e));
        return -1;
    }
    if(close(o.fd) < 0) {
        e = errno;
        unlink(path);
        snprintf(why, (size_t)size, "%s", strerror(e));
        return -1;
    }
    return 0;
}

const struct sheet_io io_files = { NULL, f_open, f_read, f_close, f_save };
