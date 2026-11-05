/*
SurfOS user program: wpaint
--------------------
File: files.c   Date: 11/05/26
--------------------
Paint's pictures as files, through libsurf's codecs (image.h, track img of
docs/superpowers/plans/2026-11-05-desktop-apps.md).

Opening: the file is read whole, at most PF_FILE_MAX bytes, and decoded by wimg_decode(), which
knows the kind from the first bytes (BMP, PNG, GIF, JPEG), never from the name, and gives a whole
picture or says why not. Its pixels' transparency is put on white (Paint has none), and the
picture becomes the canvas (paint_adopt(), which counts it from then on). For that moment the
file's bytes and the decoder's own memory are held beside Paint's (paint.h's bound is Paint's
own blocks): a file is hostile, so the decoder bounds what it takes before it asks. A file that
cannot be read or decoded leaves the canvas as it was.

Saving: the name's ending says the kind, .bmp a BMP of 24 bits and .png a PNG of 8 bits a sample,
RGB; Paint writes no other (main.c offers .png instead). What floats and what is being typed is
put down first. The codec gets the canvas's own pixels, which are solid, and hands the bytes to
put() as it makes them, so nothing as large as the picture is held twice. A file the user has
is replaced only through user/bin/saveutil.h: a new file beside it, fsynced and renamed over it,
the old one as it was until then, and none of this when a name beside it looks like the half of
a pair a failed save left (the desktop's care, as Notepad's). A file Paint makes is O_CREAT |
O_EXCL, and is deleted again when the save fails partway.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <image.h>
#include "saveutil.h"
#include "paint.h"
#include "files.h"

static const char *base(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static int ends(const char *path, const char *end) {
    size_t n = strlen(path), e = strlen(end);
    return n >= e && !strcasecmp(path + n - e, end);   /* FAT's names ignore case */
}

int pf_kind(const char *path) {
    if(ends(path, ".bmp")) return PF_BMP;
    if(ends(path, ".png")) return PF_PNG;
    return 0;
}

void pf_save_name(const char *path, char *out, size_t n) {
    const char *name = base(path), *dot = strrchr(name, '.');
    size_t keep;
    if(pf_kind(path) || !n) {
        if(n) snprintf(out, n, "%s", path);
        return;
    }
    keep = dot && dot > name ? (size_t)(dot - path) : strlen(path);  /* "photo.jpg" and "photo" become "photo.png" */
    if(keep + 5 > n) keep = n > 5 ? n - 5 : 0;
    snprintf(out, n, "%.*s.png", (int)keep, path);
}

void pf_flatten(unsigned *px, size_t n) {
    unsigned c, t, a, r, g, b;
    size_t i;
    for(i = 0; i < n; i++) {
        c = px[i];
        t = c >> 24;                                    /* 0 solid, 255 not there */
        if(!t) continue;
        a = 255 - t;
        r = ((c >> 16 & 0xFF) * a + 255 * t + 127) / 255;
        g = ((c >> 8 & 0xFF) * a + 255 * t + 127) / 255;
        b = ((c & 0xFF) * a + 255 * t + 127) / 255;
        px[i] = r << 16 | g << 8 | b;
    }
}

/**** opening ****/

/* the whole file into *data (malloc'ed), *len bytes: 0, or -1 with why */
static int read_file(const char *path, unsigned char **data, size_t *len, char *why, size_t cap) {
    struct stat st;
    unsigned char *buf;
    size_t want, got = 0;
    long r;
    int fd = open(path, O_RDONLY);
    *data = NULL;
    *len = 0;
    if(fd < 0) {
        snprintf(why, cap, "%s", strerror(errno));
        return -1;
    }
    if(fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        snprintf(why, cap, "It is not a file");
        close(fd);
        return -1;
    }
    if((unsigned long)st.st_size > PF_FILE_MAX) {
        snprintf(why, cap, "It has more than the %u MB Paint opens", PF_FILE_MAX >> 20);
        close(fd);
        return -1;
    }
    want = (size_t)st.st_size;
    if(!want) {
        snprintf(why, cap, "It is empty: no picture");
        close(fd);
        return -1;
    }
    buf = (unsigned char *)malloc(want);
    if(!buf) {
        snprintf(why, cap, "Not enough memory to read it");
        close(fd);
        return -1;
    }
    while(got < want) {
        r = (long)read(fd, buf + got, want - got);
        if(r <= 0) {                                    /* an error, or it ends before it said: cut short meanwhile */
            snprintf(why, cap, "%s", r < 0 ? strerror(errno) : "It could not be read to its end");
            free(buf);
            close(fd);
            return -1;
        }
        got += (size_t)r;
    }
    close(fd);
    *data = buf;
    *len = got;
    return 0;
}

int pf_open(struct paint *p, const char *path, char *why, size_t cap) {
    struct wimg m;
    unsigned char *data;
    size_t len;
    char err[WIMG_ERR_MAX];
    if(read_file(path, &data, &len, why, cap) < 0) return -1;
    if(wimg_decode(data, len, &m, err, sizeof(err)) < 0) {
        free(data);
        snprintf(why, cap, "Not read: %s", err);
        return -1;
    }
    free(data);
    pf_flatten(m.px, (size_t)m.w * (size_t)m.h);
    if(paint_adopt(p, m.px, m.w, m.h) < 0) {            /* not taken: still the decoder's */
        snprintf(why, cap, "%s", p->msg);
        wimg_free(&m);
        return -1;
    }
    return 0;
}

/**** saving ****/

struct sink { int fd, err; };

/* the codec's bytes, written whole: 0, or -1 with the write's errno kept */
static int put(void *ctx, const void *data, size_t n) {
    struct sink *k = (struct sink *)ctx;
    const char *q = (const char *)data;
    long w;
    while(n) {
        w = (long)write(k->fd, q, n);
        if(w <= 0) {                                    /* none written is an error too, or this would never end */
            k->err = w < 0 && errno ? errno : EIO;
            return -1;
        }
        q += w;
        n -= (size_t)w;
    }
    return 0;
}

static int encode(struct paint *p, int kind, struct sink *k, char *err, size_t cap) {
    struct wimg m;
    m.w = p->w;
    m.h = p->h;
    m.px = p->px;
    return kind == PF_BMP ? wimg_save_bmp(&m, put, k, err, cap) : wimg_save_png(&m, 0, put, k, err, cap);
}

/* why a save failed: the file system's error, or the codec's */
static void failed(int e, const char *err, char *why, size_t cap) {
    if(e == EROFS) snprintf(why, cap, "That volume is read-only: choose another folder");
    else if(e == ENOSPC) snprintf(why, cap, "The volume is full");
    else if(e) snprintf(why, cap, "%s", strerror(e));
    else snprintf(why, cap, "Not saved: %s", err && err[0] ? err : "no memory");
}

/* a file the user has: through saveutil.h */
static int replace(struct paint *p, const char *path, int kind, char *why, size_t cap, char *kept, size_t kcap) {
    struct su_save s;
    struct sink k;
    char err[WIMG_ERR_MAX] = "";
    int r, e;
    if(su_begin(&s, path) < 0) {
        e = errno;
        if(e == EBUSY && s.twin[0])
            snprintf(why, cap, "%s beside it may share its space: have the disk checked first (fsck)", base(s.twin));
        else if(e == EEXIST) snprintf(why, cap, "No name is free beside it: move %s.tmp out of the way", base(path));
        else failed(e, NULL, why, cap);
        return -1;
    }
    k.fd = s.fd;
    k.err = 0;
    if(encode(p, kind, &k, err, sizeof(err)) < 0) {
        su_abort(&s);
        failed(k.err, err, why, cap);
        return -1;
    }
    r = su_commit(&s);
    if(!r) return 0;
    e = errno;
    if(r == SU_KEPT) {
        if(s.tmp[0]) {
            if(kept && kcap) snprintf(kept, kcap, "%s", s.tmp);
            snprintf(why, cap, "The disk failed (%s): the picture is in %s; have the disk checked", strerror(e),
                     base(s.tmp));
        } else snprintf(why, cap, "The disk failed (%s): have it checked", strerror(e));
        return PF_KEPT;
    }
    failed(e, NULL, why, cap);
    return -1;
}

/* a file Paint makes: 0; 1 when there is one of that name after all (made meanwhile); -1 with why */
static int create(struct paint *p, const char *path, int kind, char *why, size_t cap) {
    struct sink k;
    char err[WIMG_ERR_MAX] = "";
    int r;
    k.fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if(k.fd < 0) {
        if(errno == EEXIST) return 1;
        failed(errno, NULL, why, cap);
        return -1;
    }
    k.err = 0;
    r = encode(p, kind, &k, err, sizeof(err));
    if(!r && fsync(k.fd) < 0) {
        r = -1;
        k.err = errno ? errno : EIO;
    }
    if(close(k.fd) < 0 && !r) {
        r = -1;
        k.err = errno ? errno : EIO;
    }
    if(r < 0) {
        unlink(path);                                   /* nothing half made is left */
        failed(k.err, err, why, cap);
        return -1;
    }
    return 0;
}

int pf_save(struct paint *p, const char *path, char *why, size_t cap, char *kept, size_t kcap) {
    struct stat st;
    int kind = pf_kind(path), tries, r;
    if(kept && kcap) kept[0] = 0;
    if(cap) why[0] = 0;
    if(!kind) {
        snprintf(why, cap, "Paint saves BMP and PNG pictures: end the name in .bmp or .png");
        return -1;
    }
    paint_put_down(p);
    for(tries = 0; tries < 2; tries++) {
        if(stat(path, &st) == 0) {
            if(!S_ISREG(st.st_mode)) {
                snprintf(why, cap, "There is a folder of that name: choose another");
                return -1;
            }
            r = replace(p, path, kind, why, cap, kept, kcap);
        } else if(errno != ENOENT) {
            failed(errno, NULL, why, cap);
            return -1;
        } else if((r = create(p, path, kind, why, cap)) == 1) continue;
        if(!r) paint_saved(p);
        return r;
    }
    snprintf(why, cap, "Another program made a file of that name meanwhile: save again");
    return -1;
}
