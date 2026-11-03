/*
SurfOS user programs: replacing a file
----------------------
File: saveutil.h  Date: 11/01/26
----------------------
sed -i, patch, tar, unzip and the rest replace a file the way edit saves one (user/bin/edit.c; the
desktop's Notepad too, shell/guifs.c): the new contents go into a new file beside it, <file>.tmp, or
<file>.1.tmp to .9.tmp when the user has a file of that name (O_EXCL: theirs is never touched), and
that is fsynced and renamed over the file. Until the rename the old file is as it was.

    struct su_save s;
    if(su_begin(&s, path) < 0) ...              nothing was made; errno says why
    ... write to s.fd ...
    r = su_commit(&s);                          or su_abort(&s), which deletes the new file

After an I/O error a rename may have half happened, the new name written and the old one not yet
gone: both then hold the same clusters, and deleting the new file would free the old one's. So
su_commit() deletes the new file only when the rename failed before it changed anything. Otherwise
it returns SU_KEPT, and s.tmp names the new file if it is there. su_say() says all this for a
program that has no more to add.

su_begin() refuses (EBUSY, s.twin names it) when a name beside the file already looks like such a
pair: one of the new names, or the name a .tmp or .part comes from, with the file's size and time,
as edit looks (twin_beside()). Both are left alone: the disk wants checking first.

Unlike edit, nothing here writes a file in place: where no such name fits or the file system cannot
rename, the program says so and the file stays as it was.
*/

#ifndef _SAVEUTIL_H
#define _SAVEUTIL_H

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#define SU_UNUSED __attribute__((unused))
#define SU_KEPT (-2)                    /* su_commit(): the rename may have half happened */

struct su_save {
    char path[PATH_MAX];                /* the file */
    char tmp[PATH_MAX];                 /* the new file; "" once it is gone */
    char twin[PATH_MAX];                /* su_begin()'s EBUSY: the name that looks like the file's other half */
    int fd;                             /* the new file, open for writing; -1 once closed */
};

/* a rename that failed with one of these changed nothing (edit.c's list: the system call's check of its
   strings, fs/vfs.c's checks, and fs/fat.c's before it writes a name) */
static SU_UNUSED int su_nothing_changed(int e) {
    return e == ENOSYS || e == EXDEV || e == EROFS || e == EBUSY || e == EINVAL || e == ENOENT || e == EFAULT ||
           e == ENOTDIR || e == EISDIR || e == ENOTEMPTY || e == ENAMETOOLONG || e == ENOSPC || e == ENOMEM;
}

static SU_UNUSED int su_same_entry(const struct stat *a, const char *name) {
    struct stat b;
    return stat(name, &b) == 0 && S_ISREG(b.type) && b.size == a->size && b.mtime == a->mtime;
}

/* a name beside path with its size and time, into twin: edit.c's twin_beside() */
static SU_UNUSED int su_twin_beside(const char *path, char *twin, size_t size) {
    static const char *const ends[] = { ".tmp", ".part" };
    struct stat st;
    size_t n = strlen(path), e;
    unsigned i;
    int k, w;
    if(stat(path, &st) < 0 || !S_ISREG(st.type) || !st.size) return 0;     /* an empty file holds no clusters */
    for(k = 0; k <= 10; k++) {
        if(k < 10) w = snprintf(twin, size, k ? "%s.%d.tmp" : "%s.tmp", path, k);
        else w = snprintf(twin, size, "%s.part", path);
        if(w < (int)size && su_same_entry(&st, twin)) return 1;
    }
    for(i = 0; i < sizeof(ends) / sizeof(ends[0]); i++) {       /* the file is <name>.tmp, <name>.N.tmp, <name>.part */
        e = strlen(ends[i]);
        if(n <= e || n >= size || strcasecmp(path + n - e, ends[i])) continue;      /* FAT's names ignore case */
        memcpy(twin, path, n - e);
        twin[n - e] = 0;
        if(su_same_entry(&st, twin)) return 1;
        if(i == 0 && n - e >= 3 && twin[n - e - 2] == '.' && twin[n - e - 1] >= '1' && twin[n - e - 1] <= '9') {
            twin[n - e - 2] = 0;                        /* a.txt beside a.txt.1.tmp */
            if(su_same_entry(&st, twin)) return 1;
        }
    }
    twin[0] = 0;
    return 0;
}

/* the new file for path, open for writing in s->fd: 0; or -1 and errno, with nothing made: EEXIST when all ten
   names are the user's, EBUSY when s->twin looks like the file's other half */
static SU_UNUSED int su_begin(struct su_save *s, const char *path) {
    int k, e = ENAMETOOLONG;
    s->fd = -1;
    s->tmp[0] = s->twin[0] = 0;
    if(snprintf(s->path, sizeof(s->path), "%s", path) >= (int)sizeof(s->path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if(su_twin_beside(s->path, s->twin, sizeof(s->twin))) {
        errno = EBUSY;
        return -1;
    }
    for(k = 0; k < 10; k++) {
        if(snprintf(s->tmp, sizeof(s->tmp), k ? "%s.%d.tmp" : "%s.tmp", path, k) >= (int)sizeof(s->tmp)) break;
        s->fd = open(s->tmp, O_WRONLY | O_CREAT | O_EXCL);
        if(s->fd >= 0) return 0;
        e = errno;
        if(e != EEXIST) break;                  /* a file the user has under that name: the next name */
    }
    s->tmp[0] = 0;
    errno = e;
    return -1;
}

/* the old file stays: the new one is closed and deleted */
static SU_UNUSED void su_abort(struct su_save *s) {
    int e = errno;
    if(s->fd >= 0) close(s->fd);
    s->fd = -1;
    if(s->tmp[0]) unlink(s->tmp);
    s->tmp[0] = 0;
    errno = e;
}

/* 0: the file is the new one. -1 and errno: it is as it was, and the new file is gone. SU_KEPT and errno: the
   rename may have half happened, or the new file would not go; s->tmp names it if it is there */
static SU_UNUSED int su_commit(struct su_save *s) {
    struct stat st;
    int e = fsync(s->fd) < 0 ? (errno ? errno : EIO) : 0;
    if(close(s->fd) < 0 && !e) e = errno ? errno : EIO;
    s->fd = -1;
    if(e) {
        unlink(s->tmp);                         /* made by this save: what of the contents it got */
        s->tmp[0] = 0;
        errno = e;
        return -1;
    }
    if(rename(s->tmp, s->path) == 0) {
        s->tmp[0] = 0;
        return 0;
    }
    e = errno;
    if(su_nothing_changed(e)) {
        if(unlink(s->tmp) < 0 && errno != ENOENT) {
            errno = e;
            return SU_KEPT;                     /* it would not go: the contents are in it */
        }
        s->tmp[0] = 0;
        errno = e;
        return -1;
    }
    if(stat(s->tmp, &st) < 0) s->tmp[0] = 0;
    errno = e;
    return SU_KEPT;
}

/* what r, from su_begin() or su_commit(), and errno mean, on stderr */
static SU_UNUSED void su_say(const char *prog, const struct su_save *s, int r) {
    int e = errno;
    if(r == SU_KEPT && s->tmp[0])
        fprintf(stderr, "%s: %s: %s: the new contents are in %s; check the disk before changing either\n",
                prog, s->path, strerror(e), s->tmp);
    else if(r == SU_KEPT) fprintf(stderr, "%s: %s: %s: check the disk\n", prog, s->path, strerror(e));
    else if(e == EBUSY && s->twin[0])
        fprintf(stderr, "%s: %s: %s may share its data: check the disk first\n", prog, s->path, s->twin);
    else if(e == EEXIST)
        fprintf(stderr, "%s: %s: no name is free beside it: move %s.tmp out of the way\n", prog, s->path, s->path);
    else fprintf(stderr, "%s: %s: %s\n", prog, s->path, strerror(e));
}

#endif
