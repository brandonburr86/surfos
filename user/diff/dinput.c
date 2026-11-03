/*
SurfOS user programs: what diff and patch read
--------------------
File: dinput.c  Date: 11/01/26
--------------------
See dinput.h.
*/

#include "dinput.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* all of a pipe or a terminal, in memory: it cannot be read again */
static int keep_all(struct dsrc *s, int fd) {
    size_t cap = 65536, n = 0;
    unsigned char *p = malloc(cap);
    if(!p) {
        errno = ENOMEM;
        return -1;
    }
    for(;;) {
        long r;
        if(n == cap) {
            unsigned char *q = cap < 0x40000000u ? realloc(p, cap * 2) : NULL;
            if(!q) {
                free(p);
                errno = ENOMEM;
                return -1;
            }
            p = q;
            cap *= 2;
        }
        r = read(fd, p + n, cap - n);
        if(r < 0) {
            int e = errno;
            free(p);
            errno = e;
            return -1;
        }
        if(!r) break;
        n += (size_t)r;
    }
    s->mem = p;
    s->size = (unsigned)n;
    s->own = 1;
    return 0;
}

int dsrc_open(struct dsrc *s, const char *name) {
    struct stat st;
    int fd = strcmp(name, "-") ? open(name, O_RDONLY) : 0, r;
    memset(s, 0, sizeof(*s));
    s->fd = -1;
    if(fd < 0) return -1;
    if(fstat(fd, &st) < 0) r = -1;
    else if(S_ISDIR(st.st_mode)) {
        errno = EISDIR;
        r = -1;
    } else if(S_ISREG(st.st_mode) && st.st_size > 0 && lseek(fd, 0, SEEK_SET) == 0) {
        /* size 0 goes below: a device says 0 and may hold anything, and a pipe is no place to seek */
        s->fd = fd;
        s->size = (unsigned)st.st_size;
        s->own = fd != 0;
        return 0;
    } else r = keep_all(s, fd);
    if(fd) {
        int e = errno;
        close(fd);
        errno = e;
    }
    return r;
}

void dsrc_memory(struct dsrc *s, const void *p, unsigned n) {
    memset(s, 0, sizeof(*s));
    s->fd = -1;
    s->mem = p;
    s->size = n;
}

void dsrc_stream(struct dsrc *s, int fd) {
    memset(s, 0, sizeof(*s));
    s->fd = fd;
    s->stream = 1;
    s->size = DSRC_ALL;
}

void dsrc_close(struct dsrc *s) {
    if(s->own && s->fd >= 0) close(s->fd);
    if(s->own && s->fd < 0) free((void *)s->mem);
    s->own = 0;
    s->fd = -1;
    s->mem = NULL;
    s->size = 0;
}

/* lseek takes 32 signed bits, so a place past 2 GB is reached by reading up to it */
static int go_to(struct dsrc *s, unsigned off) {
    static char skipped[4096];
    if(s->stream) {
        errno = EINVAL;
        return -1;
    }
    if(off <= INT_MAX) {
        if(lseek(s->fd, (off_t)off, SEEK_SET) < 0) return -1;
        s->pos = off;
        return 0;
    }
    if(s->pos > off || s->pos < INT_MAX) {
        if(lseek(s->fd, INT_MAX, SEEK_SET) < 0) return -1;
        s->pos = INT_MAX;
    }
    while(s->pos < off) {
        unsigned n = off - s->pos < sizeof(skipped) ? off - s->pos : (unsigned)sizeof(skipped);
        long r = read(s->fd, skipped, n);
        if(r <= 0) {
            if(!r) errno = EIO;
            return -1;
        }
        s->pos += (unsigned)r;
    }
    return 0;
}

long dsrc_read(struct dsrc *s, unsigned off, void *buf, unsigned n) {
    long r;
    if(!s->stream) {
        if(off >= s->size) return 0;
        if(n > s->size - off) n = s->size - off;
    }
    if(s->fd < 0) {
        memcpy(buf, s->mem + off, n);
        return (long)n;
    }
    if(off != s->pos && go_to(s, off) < 0) return -1;
    r = read(s->fd, buf, n);
    if(r > 0) s->pos += (unsigned)r;
    return r;
}

long dsrc_read_full(struct dsrc *s, unsigned off, void *buf, unsigned n) {
    unsigned got = 0;
    while(got < n) {
        long r = dsrc_read(s, off + got, (char *)buf + got, n - got);
        if(r < 0) return -1;
        if(!r) break;
        got += (unsigned)r;
    }
    return (long)got;
}

int dr_start(struct dreader *r, struct dsrc *s, unsigned from, unsigned to) {
    memset(r, 0, sizeof(*r));
    r->src = s;
    r->next = from;
    r->end = to;
    if(!(r->buf = malloc(DR_BLOCK))) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

void dr_stop(struct dreader *r) {
    free(r->buf);
    r->buf = NULL;
}

void dr_seek(struct dreader *r, unsigned off) {
    r->next = off;
    r->pos = r->len = 0;
}

int dr_refill(struct dreader *r) {
    unsigned want = r->end - r->next < DR_BLOCK ? r->end - r->next : DR_BLOCK;
    long n;
    r->pos = r->len = 0;
    if(!want || r->failed) return -1;
    n = dsrc_read(r->src, r->next, r->buf, want);
    if(n <= 0) {
        if(n < 0) r->failed = errno ? errno : EIO;
        else if(!r->src->stream) r->failed = EIO;       /* it said it was longer: it changed under us */
        return -1;
    }
    r->next += (unsigned)n;
    r->len = (unsigned)n;
    r->pos = 1;
    return r->buf[0];
}

/* to out too when out is given */
static int next_line(struct dreader *r, FILE *out) {
    int any = 0;
    for(;;) {
        const unsigned char *p, *nl;
        unsigned n;
        if(r->pos == r->len) {
            int c = dr_refill(r);
            if(c < 0) return any ? 0 : -1;
            r->pos = 0;                         /* looked at below, with the rest of the block */
        }
        p = r->buf + r->pos;
        n = r->len - r->pos;
        nl = memchr(p, '\n', n);
        any = 1;
        if(nl) {
            if(out) fwrite(p, 1, (size_t)(nl - p), out);
            r->pos += (unsigned)(nl - p) + 1;
            return 1;
        }
        if(out) fwrite(p, 1, n, out);
        r->pos = r->len;
    }
}

int dr_skip_line(struct dreader *r) {
    return next_line(r, NULL);
}

int dr_copy_line(struct dreader *r, FILE *out) {
    return next_line(r, out);
}
