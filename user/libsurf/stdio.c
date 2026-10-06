/*
SurfOS user library: buffered streams
--------------------
File: stdio.c   Date: 10/5/26
--------------------
A FILE is a descriptor with one buffer that serves either direction: dir says
whether it holds data read ahead ('r') or data not yet written ('w'). Turning
from reading to writing seeks the descriptor back over what was read ahead;
turning from writing to reading flushes. stdout decides on its first use:
line buffered when it is the console, fully buffered otherwise; stderr is never
buffered. Reading a stream that is not fully buffered (the console) flushes
stdout first, so a prompt shows before the program waits for the line.
*/

#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include "libsurf.h"

#define F_READ   0x01
#define F_WRITE  0x02
#define F_EOF    0x04
#define F_ERR    0x08
#define F_OWNBUF 0x10           /* buf came from malloc() here */
#define F_STATIC 0x20           /* stdin, stdout, stderr: never freed */
#define F_SETUP  0x40           /* the buffering is decided */

struct _FILE {
    int fd;
    int flags;
    int bufmode;                /* _IOFBF, _IOLBF, _IONBF */
    int dir;                    /* 0, 'r' or 'w' */
    unsigned char *buf;
    size_t size;
    size_t rpos, rend;          /* 'r': buf[rpos..rend) is read ahead */
    size_t wlen;                /* 'w': buf[0..wlen) waits to be written */
    int ungot;                  /* a pushed-back character, or EOF */
    unsigned char one;          /* the buffer of an unbuffered stream that reads */
    struct _FILE *next;         /* the streams fopen() made */
};

static unsigned char stdin_buf[BUFSIZ], stdout_buf[BUFSIZ];
static FILE std_in  = { 0, F_READ | F_STATIC, _IOFBF, 0, stdin_buf, BUFSIZ, 0, 0, 0, EOF, 0, NULL };
static FILE std_out = { 1, F_WRITE | F_STATIC, _IOFBF, 0, stdout_buf, BUFSIZ, 0, 0, 0, EOF, 0, NULL };
static FILE std_err = { 2, F_WRITE | F_STATIC | F_SETUP, _IONBF, 0, NULL, 0, 0, 0, 0, EOF, 0, NULL };
FILE *stdin = &std_in, *stdout = &std_out, *stderr = &std_err;

static FILE *open_files;

/* the first use of a standard stream: line buffering for the console */
static void setup(FILE *f) {
    if(f->flags & F_SETUP) return;
    f->flags |= F_SETUP;
    if(f->bufmode == _IOFBF && isatty(f->fd)) f->bufmode = _IOLBF;
}

static void get_buffer(FILE *f) {
    if(f->buf) return;
    if(f->bufmode != _IONBF) {
        if(!f->size) f->size = BUFSIZ;
        f->buf = malloc(f->size);
        if(f->buf) { f->flags |= F_OWNBUF; return; }
        f->bufmode = _IONBF;                            /* no memory: work unbuffered */
    }
    f->buf = &f->one;
    f->size = 1;
}

static int flush_out(FILE *f) {
    size_t done = 0;
    while(done < f->wlen) {
        ssize_t n = write(f->fd, f->buf + done, f->wlen - done);
        if(n <= 0) {
            f->flags |= F_ERR;
            memmove(f->buf, f->buf + done, f->wlen - done);     /* keep what is left */
            f->wlen -= done;
            return EOF;
        }
        done += (size_t)n;
    }
    f->wlen = 0;
    return 0;
}

/* forget what was read ahead, moving the descriptor back to where the reader is */
static void drop_input(FILE *f) {
    off_t back = (off_t)(f->rend - f->rpos) + (f->ungot != EOF);
    if(back) lseek(f->fd, -back, SEEK_CUR);
    f->rpos = f->rend = 0;
    f->ungot = EOF;
}

static int to_write(FILE *f) {
    if(!(f->flags & F_WRITE)) { f->flags |= F_ERR; errno = EBADF; return EOF; }
    if(f->dir == 'w') return 0;
    if(f->dir == 'r') drop_input(f);
    setup(f);
    if(f->bufmode != _IONBF) get_buffer(f);
    f->dir = 'w';
    return 0;
}

static int to_read(FILE *f) {
    if(!(f->flags & F_READ)) { f->flags |= F_ERR; errno = EBADF; return EOF; }
    if(f->dir == 'r') return 0;
    if(f->dir == 'w' && flush_out(f)) return EOF;
    setup(f);
    get_buffer(f);
    f->dir = 'r';
    return 0;
}

static int refill(FILE *f) {
    ssize_t n;
    if(f->bufmode != _IOFBF && std_out.dir == 'w') flush_out(&std_out);
    n = read(f->fd, f->buf, f->size);
    if(n <= 0) {
        f->flags |= n == 0 ? F_EOF : F_ERR;
        return EOF;
    }
    f->rpos = 0;
    f->rend = (size_t)n;
    return 0;
}

/* the one way bytes go out */
static size_t put_bytes(FILE *f, const void *data, size_t n) {
    const unsigned char *p = data;
    size_t done = 0;
    if(to_write(f)) return 0;
    if(f->bufmode == _IONBF) {
        while(done < n) {
            ssize_t w = write(f->fd, p + done, n - done);
            if(w <= 0) { f->flags |= F_ERR; break; }
            done += (size_t)w;
        }
        return done;
    }
    while(done < n) {
        size_t chunk = n - done, room = f->size - f->wlen;
        if(!f->wlen && chunk >= f->size) {             /* a big write with an empty buffer: straight through */
            ssize_t w = write(f->fd, p + done, chunk);
            if(w <= 0) { f->flags |= F_ERR; break; }
            done += (size_t)w;
            continue;
        }
        if(chunk > room) chunk = room;
        memcpy(f->buf + f->wlen, p + done, chunk);
        f->wlen += chunk;
        done += chunk;
        if(f->wlen == f->size && flush_out(f)) break;
    }
    if(f->bufmode == _IOLBF && f->wlen && memchr(p, '\n', done)) flush_out(f);
    return done;
}

/**** opening and closing ****/

static int parse_mode(const char *mode, int *oflags, int *fflags) {
    int plus = strchr(mode, '+') != NULL;
    switch(mode[0]) {
    case 'r': *oflags = plus ? O_RDWR : O_RDONLY; break;
    case 'w': *oflags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC; break;
    case 'a': *oflags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND; break;
    default: errno = EINVAL; return -1;
    }
    if(strchr(mode, 'x')) *oflags |= O_EXCL;
    *fflags = plus ? F_READ | F_WRITE : mode[0] == 'r' ? F_READ : F_WRITE;
    return 0;
}

FILE *fdopen(int fd, const char *mode) {
    int oflags, fflags;
    FILE *f;
    if(parse_mode(mode, &oflags, &fflags)) return NULL;
    f = calloc(1, sizeof(FILE));
    if(!f) return NULL;
    f->fd = fd;
    f->flags = fflags | F_SETUP;
    f->bufmode = _IOFBF;
    f->ungot = EOF;
    f->next = open_files;
    open_files = f;
    return f;
}

FILE *fopen(const char *path, const char *mode) {
    int oflags, fflags, fd;
    FILE *f;
    if(parse_mode(mode, &oflags, &fflags)) return NULL;
    fd = open(path, oflags);
    if(fd < 0) return NULL;
    f = fdopen(fd, mode);
    if(!f) close(fd);
    return f;
}

int fclose(FILE *f) {
    int r = fflush(f);
    FILE **pp;
    if(close(f->fd) < 0) r = EOF;
    if(f->flags & F_OWNBUF) free(f->buf);
    f->buf = NULL;
    f->flags &= ~F_OWNBUF;
    if(f->flags & F_STATIC) return r;
    for(pp = &open_files; *pp; pp = &(*pp)->next) {
        if(*pp == f) { *pp = f->next; break; }
    }
    free(f);
    return r;
}

int fflush(FILE *f) {
    int r = 0;
    if(!f) {
        FILE *g;
        if(fflush(stdout)) r = EOF;
        if(fflush(stderr)) r = EOF;
        for(g = open_files; g; g = g->next) if(fflush(g)) r = EOF;
        return r;
    }
    if(f->dir == 'w') r = flush_out(f);
    else if(f->dir == 'r') drop_input(f);
    if(!r) f->dir = 0;
    return r;
}

/* 10/6/26: the stream forgets what it holds, written or read ahead, and its error: for a
   shell whose redirected output could not be written (a full disk), so that the rest does
   not reach the console once descriptor 1 is the console again */
int fpurge(FILE *f) {
    f->wlen = 0;
    f->rpos = f->rend = 0;
    f->ungot = EOF;
    f->dir = 0;
    f->flags &= ~F_ERR;
    return 0;
}

int setvbuf(FILE *f, char *buf, int mode, size_t size) {
    if(mode != _IOFBF && mode != _IOLBF && mode != _IONBF) return -1;
    fflush(f);
    if(f->flags & F_OWNBUF) free(f->buf);
    f->flags &= ~F_OWNBUF;
    f->buf = mode == _IONBF ? NULL : (unsigned char *)buf;
    f->size = mode == _IONBF ? 0 : (buf ? size : (size ? size : BUFSIZ));
    f->bufmode = mode;
    f->flags |= F_SETUP;
    return 0;
}

void setbuf(FILE *f, char *buf) {
    setvbuf(f, buf, buf ? _IOFBF : _IONBF, BUFSIZ);
}

/**** characters, lines, blocks ****/

int fgetc(FILE *f) {
    if(to_read(f)) return EOF;
    if(f->ungot != EOF) {
        int c = f->ungot;
        f->ungot = EOF;
        return c;
    }
    if(f->rpos == f->rend) {
        if(f->flags & F_EOF) return EOF;
        if(refill(f)) return EOF;
    }
    return f->buf[f->rpos++];
}

int getc(FILE *f) { return fgetc(f); }
int getchar(void) { return fgetc(stdin); }

int ungetc(int c, FILE *f) {
    if(c == EOF || to_read(f)) return EOF;
    if(f->rpos > 0) f->buf[--f->rpos] = (unsigned char)c;
    else if(f->ungot == EOF) f->ungot = (unsigned char)c;
    else return EOF;
    f->flags &= ~F_EOF;
    return (unsigned char)c;
}

char *fgets(char *s, int n, FILE *f) {
    int i = 0, c = 0;
    if(n <= 0) return NULL;
    while(i < n - 1) {
        c = fgetc(f);
        if(c == EOF) break;
        s[i++] = (char)c;
        if(c == '\n') break;
    }
    if(!i || (c == EOF && (f->flags & F_ERR))) return NULL;
    s[i] = 0;
    return s;
}

size_t fread(void *p, size_t size, size_t n, FILE *f) {
    unsigned char *dst = p;
    size_t total, done = 0;
    if(!size || !n) return 0;
    if(n > (size_t)-1 / size) { errno = EOVERFLOW; return 0; }
    total = size * n;
    if(to_read(f)) return 0;
    if(f->ungot != EOF) { dst[done++] = (unsigned char)f->ungot; f->ungot = EOF; }
    while(done < total) {
        size_t avail = f->rend - f->rpos;
        if(avail) {
            if(avail > total - done) avail = total - done;
            memcpy(dst + done, f->buf + f->rpos, avail);
            f->rpos += avail;
            done += avail;
            continue;
        }
        if(f->flags & F_EOF) break;
        if(total - done >= f->size) {                   /* big reads go straight to the caller's buffer */
            ssize_t r;
            if(f->bufmode != _IOFBF && std_out.dir == 'w') flush_out(&std_out);
            r = read(f->fd, dst + done, total - done);
            if(r <= 0) { f->flags |= r == 0 ? F_EOF : F_ERR; break; }
            done += (size_t)r;
            continue;
        }
        if(refill(f)) break;
    }
    return done / size;
}

size_t fwrite(const void *p, size_t size, size_t n, FILE *f) {
    if(!size || !n) return 0;
    if(n > (size_t)-1 / size) { errno = EOVERFLOW; return 0; }
    return put_bytes(f, p, size * n) / size;
}

int fputc(int c, FILE *f) {
    unsigned char ch = (unsigned char)c;
    if(f->dir == 'w' && f->bufmode == _IOFBF && f->wlen < f->size) {    /* the common case, quickly */
        f->buf[f->wlen++] = ch;
        return ch;
    }
    return put_bytes(f, &ch, 1) == 1 ? ch : EOF;
}

int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c) { return fputc(c, stdout); }

int fputs(const char *s, FILE *f) {
    size_t n = strlen(s);
    return put_bytes(f, s, n) == n ? 0 : EOF;
}

int puts(const char *s) {
    if(fputs(s, stdout) == EOF) return EOF;
    return fputc('\n', stdout) == EOF ? EOF : 0;
}

/**** formatted output into a stream, in pieces of up to 256 bytes ****/

struct file_sink {
    struct sink s;
    FILE *f;
    size_t used;
    char chunk[256];
};

static void file_flush(struct file_sink *d) {
    if(d->used && put_bytes(d->f, d->chunk, d->used) != d->used) d->s.fail = 1;
    d->used = 0;
}

static void file_put(struct sink *s, const char *p, size_t n) {
    struct file_sink *d = (struct file_sink *)s;
    while(n) {
        size_t k = sizeof(d->chunk) - d->used;
        if(k > n) k = n;
        memcpy(d->chunk + d->used, p, k);
        d->used += k;
        p += k;
        n -= k;
        if(d->used == sizeof(d->chunk)) file_flush(d);
    }
}

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    struct file_sink d;
    int n;
    memset(&d.s, 0, sizeof(d.s));
    d.s.put = file_put;
    d.f = f;
    d.used = 0;
    n = __format(&d.s, fmt, ap);
    file_flush(&d);
    return d.s.fail ? -1 : n;
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int vprintf(const char *fmt, va_list ap) {
    return vfprintf(stdout, fmt, ap);
}

int printf(const char *fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return n;
}

/**** positions and state ****/

int fseek(FILE *f, long off, int whence) {
    if(f->dir == 'w' && flush_out(f)) return -1;
    if(f->dir == 'r' && whence == SEEK_CUR) off -= (long)(f->rend - f->rpos) + (f->ungot != EOF);
    f->rpos = f->rend = 0;
    f->ungot = EOF;
    f->dir = 0;
    if(lseek(f->fd, off, whence) < 0) return -1;
    f->flags &= ~F_EOF;
    return 0;
}

long ftell(FILE *f) {
    off_t pos = lseek(f->fd, 0, SEEK_CUR);
    if(pos < 0) return -1;
    if(f->dir == 'r') pos -= (off_t)(f->rend - f->rpos) + (f->ungot != EOF);
    else if(f->dir == 'w') pos += (off_t)f->wlen;
    return pos;
}

void rewind(FILE *f) {
    fseek(f, 0, SEEK_SET);
    f->flags &= ~F_ERR;
}

int feof(FILE *f) { return (f->flags & F_EOF) != 0; }
int ferror(FILE *f) { return (f->flags & F_ERR) != 0; }
void clearerr(FILE *f) { f->flags &= ~(F_EOF | F_ERR); }
int fileno(FILE *f) { return f->fd; }

/**** files by name ****/

int remove(const char *path) {
    if(unlink(path) == 0) return 0;
    if(errno != EISDIR) return -1;
    return rmdir(path);
}

void perror(const char *s) {
    const char *msg = strerror(errno);
    if(s && *s) fprintf(stderr, "%s: %s\n", s, msg);
    else fprintf(stderr, "%s\n", msg);
}

/* surf.h: a line from descriptor 0 (through stdin) without its newline */
int readline(char *buf, size_t size) {
    size_t n;
    if(!size) return -1;
    fflush(stdout);
    if(!fgets(buf, (int)size, stdin)) return -1;
    n = strlen(buf);
    if(n && buf[n - 1] == '\n') buf[--n] = 0;
    else {                                              /* longer than the buffer: drop the rest of the line */
        int c;
        while((c = fgetc(stdin)) != EOF && c != '\n');
    }
    return (int)n;
}
