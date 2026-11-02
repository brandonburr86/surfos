/*
SurfOS user program: awk
--------------------
File: io.c      Date: 11/01/26
--------------------
Files and commands. Everything awk reads comes through one reader with a buffer of its own, on
the descriptor, as a record may end at a byte, at a blank line or at a regular expression, and the
input may be a pipe or a terminal: read() gives what there is and does not wait for a full
buffer. Only one record is held, however large the file.

Every file or command that print or getline names stays open until close() or the end. A program
has 16 descriptors (FOPEN_MAX), and the message says so when they run out. print's streams are
stdio's, so that they are flushed by it before a command runs; system() and popen() flush them all.
*/

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include "awk.h"

#define READ_CHUNK 8192
#define ARGV_MOST 1000000               /* a program may set ARGC to anything, and each number is looked up */

struct reader {
    int fd;
    int tty;                            /* a person may be waiting to see what was printed */
    int eof;
    char *buf;
    unsigned start, end, cap;           /* buf[start..end) is read and unused */
};

struct stream {
    struct stream *next;
    struct str *name;
    int how;                            /* for output TO_FILE, TO_APPEND, TO_PIPE; for input IN_FILE, IN_CMD */
    FILE *f;
    struct reader *in;                  /* input */
};

enum { IN_FILE = 10, IN_CMD };

static struct stream *streams;
static struct reader main_in;           /* the current file of the main input */
static int main_open;
static int args_next = 1;               /* the next of ARGV */
static int files_seen;                  /* a file operand was found, so standard input is not read */
static int main_done;

static void reader_init(struct reader *r, int fd) {
    r->fd = fd;
    r->tty = isatty(fd);
    r->eof = 0;
    r->buf = NULL;
    r->start = r->end = r->cap = 0;
}

/* more bytes after those there are: 0, or -1 with errno */
static int reader_fill(struct reader *r) {
    ssize_t n;
    if(r->eof) return 0;
    if(r->start) {
        memmove(r->buf, r->buf + r->start, r->end - r->start);
        r->end -= r->start;
        r->start = 0;
    }
    if(r->cap - r->end < READ_CHUNK) {
        unsigned cap = r->cap ? r->cap * 2 : READ_CHUNK * 2;
        if(cap <= r->cap || cap >= 0x7ff00000u) fatal("out of memory");
        r->buf = xrealloc(r->buf, cap);
        r->cap = cap;
    }
    if(r->tty) fflush(stdout);
    n = read(r->fd, r->buf + r->end, READ_CHUNK);
    if(n < 0) return -1;
    if(!n) r->eof = 1;
    r->end += (unsigned)n;
    return 0;
}

static struct str *take(struct reader *r, unsigned len, unsigned skip) {
    struct str *s = str_new(r->buf + r->start, len);
    r->start += len + skip;
    return s;
}

/* a record that ends at a byte */
static int read_to_byte(struct reader *r, char c, struct str **rec) {
    unsigned looked = 0;
    for(;;) {
        const char *p = memchr(r->buf ? r->buf + r->start + looked : "", c, r->end - r->start - looked);
        if(p) {
            *rec = take(r, (unsigned)(p - (r->buf + r->start)), 1);
            return 1;
        }
        looked = r->end - r->start;
        if(r->eof) break;
        if(reader_fill(r) < 0) return -1;
    }
    if(r->start == r->end) return 0;
    *rec = take(r, r->end - r->start, 0);
    return 1;
}

/* RS="": newlines first go, the record ends at an empty line, and the last has no newline */
static int read_paragraph(struct reader *r, struct str **rec) {
    unsigned i;
    for(;;) {
        while(r->start < r->end && r->buf[r->start] == '\n') r->start++;
        if(r->start < r->end) break;
        if(r->eof) return 0;
        if(reader_fill(r) < 0) return -1;
    }
    i = 0;
    for(;;) {
        for(; r->start + i + 1 < r->end; i++) {
            if(r->buf[r->start + i] == '\n' && r->buf[r->start + i + 1] == '\n') {
                *rec = take(r, i, 2);
                return 1;
            }
        }
        if(r->eof) break;
        if(reader_fill(r) < 0) return -1;
    }
    i = r->end - r->start;
    if(i && r->buf[r->start + i - 1] == '\n') {
        *rec = take(r, i - 1, 1);
        return 1;
    }
    *rec = take(r, i, 0);
    return 1;
}

/* A regular expression: a match that reaches the end of what has been read might be longer with
   more, so that is read first. An empty match separates nothing. */
static int read_to_regex(struct reader *r, const struct regex *re, struct str **rec) {
    for(;;) {
        unsigned from = 0, n = r->end - r->start;
        regoff_t so = 0, eo = 0;
        int found = 0;
        while(from <= n && r->buf) {
            if(!regex_find(re, r->buf + r->start, from, n, &so, &eo)) break;
            if(eo > so) {
                found = 1;
                break;
            }
            if((unsigned)so >= n) break;
            from = (unsigned)so + 1;
        }
        if(found && ((unsigned)eo < n || r->eof)) {
            *rec = take(r, (unsigned)so, (unsigned)(eo - so));
            return 1;
        }
        if(r->eof) break;
        if(reader_fill(r) < 0) return -1;
    }
    if(r->start == r->end) return 0;
    *rec = take(r, r->end - r->start, 0);
    return 1;
}

static int read_record(struct reader *r, struct str **rec) {
    struct str *rs = to_str(&A.global[S_RS]);
    int rc;
    if(!rs->len) {
        rc = read_paragraph(r, rec);
    } else if(rs->len == 1) {
        rc = read_to_byte(r, rs->s[0], rec);
    } else {
        struct value v;
        v.kind = V_STR;
        v.u.s = rs;
        rc = read_to_regex(r, regex_of(&v), rec);
    }
    str_put(rs);
    return rc;
}

/**** the main input ****/

/* var=value on the command line */
static int is_assignment(const char *s) {
    const char *p = s;
    if(!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_')) return 0;
    while((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_') p++;
    return *p == '=';
}

void assign_operand(const char *s) {
    const char *eq = strchr(s, '=');
    int id = global_id(s, (unsigned)(eq - s));
    struct buf b = { NULL, 0, 0 };
    struct value v;
    parse_assign_value(eq + 1, (unsigned)strlen(eq + 1), &b);
    v.kind = V_INPUT;
    v.u.s = str_new(b.p ? b.p : "", b.len);
    free(b.p);
    global_set(id, &v);
    val_put(&v);
}

static void set_filename(const char *name) {
    struct value v;
    v.kind = V_STR;
    v.u.s = str_new(name, (unsigned)strlen(name));
    val_put(&A.global[S_FILENAME]);
    A.global[S_FILENAME] = v;
}

/* ARGV is read as it is reached: the program may have changed it. Assignments on the way are made. */
static int open_next_file(void) {
    double argc = to_num(&A.global[S_ARGC]);
    while(args_next < argc && args_next < ARGV_MOST) {
        char t[16];
        struct str *key, *arg;
        struct value *v;
        int n = snprintf(t, sizeof(t), "%d", args_next++);
        key = str_new(t, (unsigned)n);
        v = array_elem(A.global[S_ARGV].u.a, key, 0);
        str_put(key);
        if(!v) continue;                /* deleted */
        arg = to_str(v);
        if(!arg->len) {
            str_put(arg);
            continue;
        }
        if(is_assignment(arg->s)) {
            assign_operand(arg->s);
            str_put(arg);
            continue;
        }
        files_seen = 1;
        if(!strcmp(arg->s, "-")) {
            reader_init(&main_in, 0);
        } else {
            int fd = open(arg->s, O_RDONLY);
            if(fd < 0) fatal("%s: %s", arg->s, strerror(errno));
            reader_init(&main_in, fd);
        }
        set_filename(arg->s);
        str_put(arg);
        return 1;
    }
    if(!files_seen) {
        files_seen = 1;
        reader_init(&main_in, 0);
        return 1;
    }
    return 0;
}

static void close_main(void) {
    if(main_in.fd > 0) close(main_in.fd);
    free(main_in.buf);
    main_in.buf = NULL;
    main_open = 0;
}

void skip_file(void) {
    if(main_open) close_main();
}

int main_record(struct str **rec) {
    if(main_done) return 0;
    for(;;) {
        int rc;
        if(!main_open) {
            if(!open_next_file()) {
                main_done = 1;
                return 0;
            }
            main_open = 1;
            val_put(&A.global[S_FNR]);
            A.global[S_FNR].kind = V_NUM;
            A.global[S_FNR].u.num = 0;
        }
        rc = read_record(&main_in, rec);
        if(rc < 0) {
            struct str *name = to_str(&A.global[S_FILENAME]);
            fatal("%s: %s", name->len ? name->s : "standard input", strerror(errno));
        }
        if(rc) return 1;
        close_main();
    }
}

/**** files and commands by name ****/

static struct stream *stream_find(struct str *name, int in) {
    struct stream *s;
    for(s = streams; s; s = s->next)
        if((s->in != NULL) == in && s->name->len == name->len && !memcmp(s->name->s, name->s, name->len)) return s;
    return NULL;
}

static struct stream *stream_add(struct str *name, int how) {
    struct stream *s = xalloc(sizeof(*s));
    s->name = str_ref(name);
    s->how = how;
    s->f = NULL;
    s->in = NULL;
    s->next = streams;
    streams = s;
    return s;
}

static void stream_free(struct stream *s) {
    struct stream **p;
    for(p = &streams; *p != s; p = &(*p)->next);
    *p = s->next;
    if(s->in) {
        free(s->in->buf);
        free(s->in);
    }
    str_put(s->name);
    free(s);
}

/* "/dev/stdout" and "/dev/stderr" need no file: awk's own, as every awk has them */
FILE *output_to(int how, struct str *name) {
    struct stream *s;
    FILE *f;
    if(how != TO_PIPE) {
        if(!strcmp(name->s, "/dev/stdout")) return stdout;
        if(!strcmp(name->s, "/dev/stderr")) return stderr;
    }
    if(strlen(name->s) != name->len || !name->len) fatal("output to a name that is empty or has a NUL in it");
    s = stream_find(name, 0);
    if(s) return s->f;
    f = how == TO_PIPE ? popen(name->s, "w") : fopen(name->s, how == TO_FILE ? "w" : "a");
    if(!f) fatal("%s: %s", name->s, errno == EMFILE ? "too many files open (close some)" : strerror(errno));
    s = stream_add(name, how);
    s->f = f;
    return f;
}

/* a file or a command to read from, opened the first time */
static struct stream *input_from(int cmd, struct str *name) {
    struct stream *s = stream_find(name, 1);
    FILE *f;
    if(s) return s;
    if(strlen(name->s) != name->len) return NULL;
    if(cmd) f = popen(name->s, "r");
    else if(!strcmp(name->s, "-") || !strcmp(name->s, "/dev/stdin")) f = stdin;
    else f = fopen(name->s, "r");
    if(!f) return NULL;
    s = stream_add(name, cmd ? IN_CMD : IN_FILE);
    s->f = f;
    s->in = xalloc(sizeof(*s->in));
    reader_init(s->in, fileno(f));
    return s;
}

int getline_from(int cmd, struct str *name, struct str **rec) {
    struct stream *s = input_from(cmd, name);
    return s ? read_record(s->in, rec) : -1;
}

static int stream_end(struct stream *s) {
    int rc;
    if(s->how == TO_PIPE || s->how == IN_CMD) rc = pclose(s->f);
    else if(s->f == stdin) rc = 0;
    else rc = fclose(s->f);
    stream_free(s);
    return rc;
}

int stream_close(struct str *name) {
    struct stream *s;
    int rc = -1, found;
    do {                                /* a name may be open both ways */
        found = 0;
        for(s = streams; s; s = s->next) {
            if(s->name->len == name->len && !memcmp(s->name->s, name->s, name->len)) {
                rc = stream_end(s);
                found = 1;
                break;
            }
        }
    } while(found);
    return rc;
}

int stream_flush(struct str *name) {
    struct stream *s;
    int rc = -1;
    if(!name) return fflush(NULL) ? -1 : 0;
    if(!strcmp(name->s, "/dev/stdout")) return fflush(stdout) ? -1 : 0;
    if(!strcmp(name->s, "/dev/stderr")) return 0;
    for(s = streams; s; s = s->next)
        if(!s->in && s->name->len == name->len && !memcmp(s->name->s, name->s, name->len))
            rc = fflush(s->f) ? -1 : 0;
    return rc;
}

/* everything written and every command ended, at the end of the program */
void streams_end(void) {
    while(streams) stream_end(streams);
}
