/*
SurfOS user programs: a file made from an archive
----------------------
File: outfile.h Date: 11/01/26
----------------------
gzip, tar and unzip write files the same way, and this is the one place that says how. A new file
is made with O_CREAT | O_EXCL and deleted again if it cannot be finished; one that exists is replaced
through saveutil.h when the program may replace it, and otherwise left alone. A file the user had
is never truncated, and until of_commit() it is as it was.

    struct out_file f;
    r = of_open(&f, "tar", path, 1);            0, or -1 said; OF_EXISTS when replace is 0 and it does
    ... write to f.fd ...
    of_commit(&f, "tar", 0);                    0, or -1 said; 1: fsync a new file too
    or of_drop(&f);                             failed part way

path must stay valid until then. of_parents() makes the folders above a path.
*/

#ifndef _OUTFILE_H
#define _OUTFILE_H

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "../bin/saveutil.h"

#define OF_UNUSED __attribute__((unused))
#define OF_EXISTS (-2)

struct out_file {
    const char *path;
    int fd;
    int replacing;                      /* through saveutil, in save */
    struct su_save save;
};

static OF_UNUSED void of_say(const char *prog, const char *path) {
    fprintf(stderr, "%s: %s: %s\n", prog, path, strerror(errno));
}

static OF_UNUSED int of_open(struct out_file *f, const char *prog, const char *path, int replace) {
    struct stat st;
    f->path = path;
    f->replacing = 0;
    f->fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if(f->fd >= 0) return 0;
    if(errno != EEXIST) {
        of_say(prog, path);
        return -1;
    }
    if(stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        errno = EISDIR;
        of_say(prog, path);
        return -1;
    }
    if(!replace) return OF_EXISTS;
    if(su_begin(&f->save, path) < 0) {
        su_say(prog, &f->save, -1);
        return -1;
    }
    f->fd = f->save.fd;
    f->replacing = 1;
    return 0;
}

/* failed part way: what the user had is as it was, and what was made goes */
static OF_UNUSED void of_drop(struct out_file *f) {
    if(f->replacing) {
        su_abort(&f->save);
        return;
    }
    close(f->fd);
    unlink(f->path);
}

/* all written: under its name. sync_new: a new file is on the disk too (a replaced one always is) */
static OF_UNUSED int of_commit(struct out_file *f, const char *prog, int sync_new) {
    int r = 0, e = 0;
    if(f->replacing) {
        r = su_commit(&f->save);
        if(r) su_say(prog, &f->save, r);
        return r ? -1 : 0;
    }
    if(sync_new && fsync(f->fd) < 0) {
        r = -1;
        e = errno;
    }
    if(close(f->fd) < 0 && !r) {
        r = -1;
        e = errno;
    }
    if(r) {
        errno = e;
        of_say(prog, f->path);
        unlink(f->path);
    }
    return r;
}

/* every folder above the last / of path: 0, or -1 and errno, with path as it was */
static OF_UNUSED int of_parents(char *path) {
    struct stat st;
    char *slash = strrchr(path, '/'), *p;
    if(!slash || slash == path) return 0;
    *slash = 0;
    if(stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        *slash = '/';
        return 0;
    }
    *slash = '/';
    for(p = strchr(path + 1, '/'); p; p = strchr(p + 1, '/')) {
        int e;
        *p = 0;
        e = mkdir(path, 0777) < 0 && errno != EEXIST ? errno : 0;
        *p = '/';
        if(e) {
            errno = e;
            return -1;
        }
    }
    return 0;
}

#endif
