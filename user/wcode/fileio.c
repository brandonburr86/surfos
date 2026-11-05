/*
SurfOS user program: wcode
--------------------
File: fileio.c  Date: 11/05/26
--------------------
Reading and saving the Code Editor's files (fileio.h says the rules). A file is read straight into
the document's buffer, the size fstat() gave and one byte more to see that it did not grow, so a
16 MB file needs no second copy. A document is written from the two halves of its buffer, as its
bytes are: its line ends are the file's, so nothing is converted on the way out. A short write is
a full disk (ENOSPC), as edit takes it.
*/
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <saveutil.h>
#include "fileio.h"

#define CHUNK (64u << 10)

static const char *base(const char *path) {
    const char *b = strrchr(path, '/');
    return b ? b + 1 : path;
}

int wc_file_read(struct wc_doc *d, const char *path) {
    struct stat st;
    char *p, extra, twin[WC_PATH];
    unsigned got = 0, want;
    int fd, n = 0, r;
    if(!strncmp(path, "/dev/", 5) || !strcmp(path, "/dev")) {
        snprintf(d->msg, sizeof(d->msg), "%s is a device, not a file to edit", base(path));
        return -1;
    }
    fd = open(path, O_RDONLY);
    if(fd < 0) {
        snprintf(d->msg, sizeof(d->msg), "Cannot open %s: %s", base(path), strerror(errno));
        return -1;
    }
    if(fstat(fd, &st) < 0) {
        snprintf(d->msg, sizeof(d->msg), "Cannot read %s: %s", base(path), strerror(errno));
        close(fd);
        return -1;
    }
    if(S_ISDIR(st.type) || st.size > WC_TEXT_MAX) {
        snprintf(d->msg, sizeof(d->msg), S_ISDIR(st.type) ? "%s is a folder" :
                 "%s is larger than 16 MB, the most the editor opens", base(path));
        close(fd);
        return -1;
    }
    p = wc_text_load_begin(&d->t, st.size);
    if(!p) {
        snprintf(d->msg, sizeof(d->msg), "Not opened: %s needs more memory than there is", base(path));
        close(fd);
        return -1;
    }
    while(got < st.size) {
        want = st.size - got < CHUNK ? st.size - got : CHUNK;
        n = (int)read(fd, p + got, want);
        if(n <= 0) break;
        got += (unsigned)n;
    }
    if(n >= 0 && got == st.size) n = (int)read(fd, &extra, 1) > 0 ? -2 : 0;    /* it grew while it was read */
    else if(n >= 0 && got < st.size) n = -2;                                  /* or shrank */
    close(fd);
    wc_text_load_end(&d->t, n < 0 ? 0 : got);
    if(n < 0) {
        snprintf(d->msg, sizeof(d->msg), n == -2 ? "Not opened: %s changed while it was read" : "Cannot read %s",
                 base(path));
        return -1;
    }
    r = wc_doc_loaded(d, path);
    if(r == WC_NOT_TEXT) {                      /* the caller drops the document */
        snprintf(d->msg, sizeof(d->msg), "%s is not text (it has a NUL byte): open it in Hex", base(path));
        return -1;
    }
    if(su_twin_beside(path, twin, sizeof(twin))) {     /* edit's care from the start */
        d->unsure = 1;
        snprintf(d->twin, sizeof(d->twin), "%s", base(twin));
        snprintf(d->msg, sizeof(d->msg), "%s may share this file's data: check the disk (fsck) first", d->twin);
    }
    return 0;
}

/* the text, as its bytes are, into fd */
static int write_all(struct wc_doc *d, int fd) {
    const char *part[2];
    unsigned len[2], k, done, n;
    ssize_t w;
    wc_text_parts(&d->t, &part[0], &len[0], &part[1], &len[1]);
    for(k = 0; k < 2; k++)
        for(done = 0; done < len[k]; done += n) {
            n = len[k] - done < CHUNK ? len[k] - done : CHUNK;
            w = write(fd, part[k] + done, n);
            if(w == (ssize_t)n) continue;
            if(w >= 0) errno = ENOSPC;          /* a short write: the disk is full */
            return -1;
        }
    return 0;
}

/* the text into a new file beside path (path.tmp, path.1.tmp ...): 1 with d->msg, or -1 */
static int copy_beside(struct wc_doc *d, const char *path, char *made, unsigned size) {
    char name[WC_PATH];
    unsigned serial = wc_undo_serial(&d->u);
    int fd = -1, k, e = ENAMETOOLONG;
    if(d->copied[0] && d->copied_serial == serial) {   /* nothing changed since the last copy */
        snprintf(d->msg, sizeof(d->msg), "Not saved over the file; the text is in %s", d->copied);
        if(made) snprintf(made, size, "%s", d->copied);
        return 1;
    }
    for(k = 0; k < 10; k++) {
        if(snprintf(name, sizeof(name), k ? "%s.%d.tmp" : "%s.tmp", path, k) >= (int)sizeof(name)) break;
        fd = open(name, O_WRONLY | O_CREAT | O_EXCL);
        if(fd >= 0) break;
        e = errno;
        if(e != EEXIST) break;                  /* a file the user has under that name: the next name */
    }
    if(fd < 0) {
        if(e == EEXIST) snprintf(d->msg, sizeof(d->msg), "Not saved: no name is left for a copy beside the file");
        else snprintf(d->msg, sizeof(d->msg), "Not saved: %s", strerror(e));
        return -1;
    }
    if(write_all(d, fd) < 0 || fsync(fd) < 0) {
        e = errno;
        close(fd);
        unlink(name);                           /* made by this save: it goes again */
        snprintf(d->msg, sizeof(d->msg), "Not saved: %s", strerror(e));
        return -1;
    }
    if(close(fd) < 0) {
        e = errno;
        unlink(name);
        snprintf(d->msg, sizeof(d->msg), "Not saved: %s", strerror(e));
        return -1;
    }
    snprintf(d->copied, sizeof(d->copied), "%s", base(name));
    d->copied_serial = serial;
    wc_undo_close(&d->u);                       /* typing on is a step of its own: a text the copy does not hold */
    if(made) snprintf(made, size, "%s", name);
    if(d->twin[0])
        snprintf(d->msg, sizeof(d->msg), "Not saved over the file (%s may share its data); a copy is in %s", d->twin,
                 d->copied);
    else snprintf(d->msg, sizeof(d->msg), "Not saved over the file; a copy is in %s: check the disk", d->copied);
    return 1;
}

/* a file that is not there yet: made with O_EXCL, gone again when the save fails */
static int save_new(struct wc_doc *d, const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL), e;
    if(fd < 0) {
        snprintf(d->msg, sizeof(d->msg), errno == EEXIST ? "Not saved: %s appeared meanwhile; save again" :
                 "Not saved: %s", errno == EEXIST ? base(path) : strerror(errno));
        return -1;
    }
    if(write_all(d, fd) < 0 || fsync(fd) < 0) {
        e = errno;
        close(fd);
        unlink(path);
        errno = e;
        snprintf(d->msg, sizeof(d->msg), "Not saved: %s", strerror(e));
        return -1;
    }
    if(close(fd) < 0) {
        e = errno;
        unlink(path);
        snprintf(d->msg, sizeof(d->msg), "Not saved: %s", strerror(e));
        return -1;
    }
    return 0;
}

int wc_file_save(struct wc_doc *d, const char *to) {
    struct su_save s;
    struct stat st;
    char path[WC_PATH];
    int own = !to || !strcmp(to, d->path), r, e;
    snprintf(path, sizeof(path), "%s", to ? to : d->path);
    if(!path[0]) {
        snprintf(d->msg, sizeof(d->msg), "Not saved: it has no name yet (Save as)");
        return -1;
    }
    if(own && d->unsure) return copy_beside(d, path, NULL, 0);
    if(stat(path, &st) < 0) {
        if(errno != ENOENT) {
            snprintf(d->msg, sizeof(d->msg), "Not saved: %s", strerror(errno));
            return -1;
        }
        if(save_new(d, path) < 0) return -1;
    } else if(S_ISDIR(st.type)) {
        snprintf(d->msg, sizeof(d->msg), "Not saved: %s is a folder", base(path));
        return -1;
    } else {
        if(su_begin(&s, path) < 0) {
            e = errno;
            if(e == EBUSY && s.twin[0] && own) {    /* found now: edit's care from here on */
                d->unsure = 1;
                snprintf(d->twin, sizeof(d->twin), "%s", base(s.twin));
                return copy_beside(d, path, NULL, 0);
            }
            if(e == EBUSY && s.twin[0])
                snprintf(d->msg, sizeof(d->msg), "Not saved: %s may share its data with %s: check the disk", base(path),
                         base(s.twin));
            else if(e == EEXIST)
                snprintf(d->msg, sizeof(d->msg), "Not saved: no name is free beside it: move %s.tmp out of the way",
                         base(path));
            else if(e == EROFS) snprintf(d->msg, sizeof(d->msg), "Not saved: the disk is read-only (Save as elsewhere)");
            else snprintf(d->msg, sizeof(d->msg), "Not saved: %s", strerror(e));
            return -1;
        }
        if(write_all(d, s.fd) < 0) {
            e = errno;
            su_abort(&s);
            snprintf(d->msg, sizeof(d->msg), "Not saved: %s", strerror(e));
            return -1;
        }
        r = su_commit(&s);
        if(r == SU_KEPT && su_nothing_changed(errno) && s.tmp[0]) {   /* the rename changed nothing, the new file stayed */
            snprintf(d->msg, sizeof(d->msg), "Not saved: %s. The text is in %s", strerror(errno), base(s.tmp));
            return -1;
        }
        if(r == SU_KEPT) {                      /* the rename may have half happened: edit's care from here on */
            e = errno;
            if(!own) snprintf(d->path, sizeof(d->path), "%s", path);
            d->unsure = 1;
            d->twin[0] = 0;
            if(s.tmp[0]) {
                snprintf(d->copied, sizeof(d->copied), "%s", base(s.tmp));
                d->copied_serial = wc_undo_serial(&d->u);
                wc_undo_close(&d->u);
                snprintf(d->msg, sizeof(d->msg), "Not saved: %s. The text is in %s; check the disk before changing either",
                         strerror(e), d->copied);
            } else snprintf(d->msg, sizeof(d->msg), "Not saved: %s: check the disk", strerror(e));
            return -1;
        }
        if(r < 0) {
            snprintf(d->msg, sizeof(d->msg), "Not saved: %s", strerror(errno));
            return -1;
        }
    }
    if(!own) {                                  /* Save as: the document is that file now */
        int guessed = d->lang == WC_LANG_TEXT || !d->path[0];
        snprintf(d->path, sizeof(d->path), "%s", path);
        d->unsure = 0;
        d->copied[0] = d->twin[0] = 0;
        if(guessed) wc_doc_set_lang(d, wc_lang_guess(path, NULL, 0));
    }
    wc_doc_saved(d);
    snprintf(d->msg, sizeof(d->msg), "Saved %s", base(path));
    return 0;
}

int wc_file_rescue(struct wc_doc *d, char *where, unsigned size) {
    if(!d->path[0] || wc_doc_safe(d)) return -1;
    return copy_beside(d, d->path, where, size) > 0 ? 0 : -1;
}
