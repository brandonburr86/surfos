/*
SurfOS user program: awk
--------------------
File: io.c      Date: 11/01/26
--------------------
Files and commands. Everything awk reads comes through one reader with a buffer of its own, on
the descriptor, as a record may end at a byte, at a string, at a blank line or at a regular
expression, and the input may be a pipe or a terminal: read() gives what there is and does not
wait for a full buffer. Only one record is held, and what is read past it, however large the file.

Every file or command that print or getline names stays open until close() or the end. A program
has 16 descriptors (FOPEN_MAX), and the message says so when they run out. print's streams are
stdio's, so that they are flushed by it before a command runs; system() and popen() flush them all.
What a file's last buffer held goes out when it is closed, and a disk that has filled up may only
be found then: that ends awk like any other write that fails.
*/

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include "awk.h"

#define READ_CHUNK 8192
#define RS_MARGIN 65536                 /* what is read past a match of RS before more input is taken to leave it alone */
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
    int why;                            /* output: what a write that fflush() reported failed of, for its end to say */
};

enum { IN_FILE = 10, IN_CMD };

static struct stream *streams;
static struct reader main_in;           /* the current file of the main input */
static int main_open;
static int args_next = 1;               /* the next of ARGV */
static int files_seen;                  /* a file operand was found, so standard input is not read */
static int main_done;
static int write_failed;

static void reader_init(struct reader *r, int fd) {
    r->fd = fd;
    r->tty = isatty(fd);
    r->eof = 0;
    r->buf = NULL;
    r->start = r->end = r->cap = 0;
}

/* More bytes after those there are: 0, or -1 with errno. What is held moves down only when the room behind it has gone,
   and the buffer doubles once it is more than half full then: much may be held, and it may be asked for often. */
static int reader_fill(struct reader *r) {
    ssize_t n;
    if(r->eof) return 0;
    if(r->cap - r->end < READ_CHUNK) {
        if(r->start) {
            memmove(r->buf, r->buf + r->start, r->end - r->start);
            r->end -= r->start;
            r->start = 0;
        }
        if(r->end >= r->cap / 2) {
            unsigned cap = r->cap ? r->cap * 2 : READ_CHUNK * 2;
            if(cap <= r->cap || cap >= 0x7ff00000u) fatal("out of memory");
            r->buf = xrealloc(r->buf, cap);
            r->cap = cap;
        }
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

/* a separator with no operator in it, whose match more input cannot change: it is taken as soon as it is there */
static int read_to_string(struct reader *r, const char *sep, unsigned len, struct str **rec) {
    unsigned looked = 0;
    for(;;) {
        const char *s = r->buf ? r->buf + r->start : "";
        unsigned n = r->end - r->start;
        for(; looked + len <= n; looked++) {
            const char *p = memchr(s + looked, sep[0], n - len - looked + 1);
            if(!p) {
                looked = n - len + 1;
                break;
            }
            looked = (unsigned)(p - s);
            if(!memcmp(p, sep, len)) {
                *rec = take(r, looked, len);
                return 1;
            }
        }
        if(r->eof) break;
        if(reader_fill(r) < 0) return -1;
    }
    if(r->start == r->end) return 0;
    *rec = take(r, r->end - r->start, 0);
    return 1;
}

/* A regular expression. More input could make a match longer, or bring an earlier one, and the library has no way to say
   whether it could, so a match is taken once RS_MARGIN bytes past it are here, or the input has ended. Then the same bytes
   give the same records however they arrive, unless a match depends on what comes further on than that. A person at a
   terminal cannot be made to type that much: there it is one byte past the match, a newline usually.
   Each search goes from the record's start, so each reads as much again as the last did: a long record costs about three
   searches of itself, not one for each read. An empty match separates nothing. */
static int read_to_regex(struct reader *r, const struct regex *re, struct str **rec) {
    unsigned grow = 0;
    for(;;) {
        unsigned from = 0, n = r->end - r->start, want = n + grow;
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
        if(found && (r->eof || (unsigned)eo + (r->tty ? 1 : RS_MARGIN) <= n)) {
            *rec = take(r, (unsigned)so, (unsigned)(eo - so));
            return 1;
        }
        if(r->eof) break;
        if(found && (unsigned)eo + RS_MARGIN > want) want = (unsigned)eo + RS_MARGIN;
        grow = n;
        do {
            if(reader_fill(r) < 0) return -1;
        } while(!r->tty && !r->eof && r->end - r->start < want);
    }
    if(r->start == r->end) return 0;
    *rec = take(r, r->end - r->start, 0);
    return 1;
}

/* An RS with no operator in it, in whole characters, is the string it says. A byte in no sequence stays with the regular
   expressions, which go by characters and would not find it inside one. */
static int is_plain(const struct str *rs) {
    unsigned i = 0;
    while(i < rs->len) {
        unsigned k = char_len(rs->s + i, rs->len - i);
        if(memchr("\\^$.[]|()*+?{}", rs->s[i], 14) || (k == 1 && (signed char)rs->s[i] < 0)) return 0;
        i += k;
    }
    return 1;
}

static int read_record(struct reader *r, struct str **rec) {
    struct str *rs = to_str(&A.global[S_RS]);
    int rc;
    if(!rs->len) {
        rc = read_paragraph(r, rec);
    } else if(rs->len == 1) {
        rc = read_to_byte(r, rs->s[0], rec);
    } else if(is_plain(rs)) {
        rc = read_to_string(r, rs->s, rs->len, rec);
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

/* A name that holds an array, a function's and a word of the language cannot take a value, and ENVIRON and ARGV are
   arrays even before they are made, at -v. This is what keeps ARGV an array for open_next_file(), which takes it as one:
   every other way to write to a name goes through check_scalar(). */
void assign_operand(const char *s) {
    const char *eq = strchr(s, '=');
    unsigned len = (unsigned)(eq - s), i;
    int id, taken = is_reserved(s, len);
    struct buf b = { NULL, 0, 0 };
    struct value v;
    for(i = 0; i < A.nfn; i++) taken |= !strncmp(A.fn[i].name, s, len) && !A.fn[i].name[len];
    id = taken ? -1 : global_id(s, len);
    if(taken || id == S_ENVIRON || id == S_ARGV || A.global[id].kind == V_ARRAY) fatal("%s: that name cannot take a value", s);
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

/* A file and a command may have one name, each way, and are four streams then; > and >> of a name are one. */
static int stream_kind(int how) {
    return how == TO_APPEND ? TO_FILE : how;
}

static struct stream *stream_find(struct str *name, int how) {
    struct stream *s;
    for(s = streams; s; s = s->next)
        if(stream_kind(s->how) == stream_kind(how) && s->name->len == name->len && !memcmp(s->name->s, name->s, name->len))
            return s;
    return NULL;
}

static struct stream *stream_add(struct str *name, int how) {
    struct stream *s = xalloc(sizeof(*s));
    s->name = str_ref(name);
    s->how = how;
    s->f = NULL;
    s->in = NULL;
    s->why = 0;
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
    s = stream_find(name, how);
    if(s) return s->f;
    f = how == TO_PIPE ? popen(name->s, "w") : fopen(name->s, how == TO_FILE ? "w" : "a");
    if(!f) fatal("%s: %s", name->s, errno == EMFILE ? "too many files open (close some)" : strerror(errno));
    s = stream_add(name, how);
    s->f = f;
    return f;
}

/* a file or a command to read from, opened the first time */
static struct stream *input_from(int cmd, struct str *name) {
    struct stream *s = stream_find(name, cmd ? IN_CMD : IN_FILE);
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

/* A write that fails here is said, with the name; what is to become of awk is the caller's to decide. pclose() keeps
   its flush to itself, so that is done first. */
static int stream_end(struct stream *s) {
    int rc, why = s->why;
    errno = 0;
    if(!s->in && (fflush(s->f) == EOF || ferror(s->f)) && !why) why = errno ? errno : EIO;
    if(s->how == TO_PIPE || s->how == IN_CMD) {
        rc = pclose(s->f);
    } else if(s->f == stdin) {
        rc = 0;
    } else {
        rc = fclose(s->f);
        if(rc == EOF && !s->in && !why) why = errno ? errno : EIO;
    }
    if(why) {
        say_error("%s: %s", s->name->s, strerror(why));
        write_failed = 1;
    }
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
    if(write_failed) stop();
    return rc;
}

/* A program may not look at what fflush() gave, so a failure is kept for the stream's end to say too. */
int stream_flush(struct str *name) {
    struct stream *s;
    int rc = 0, found = 0;
    if(!name || !strcmp(name->s, "/dev/stdout")) {
        if(fflush(stdout)) rc = -1;
        found = 1;
    }
    if(name && !strcmp(name->s, "/dev/stderr")) return 0;
    for(s = streams; s; s = s->next) {
        if(s->in || (name && (s->name->len != name->len || memcmp(s->name->s, name->s, name->len)))) continue;
        found = 1;
        if(fflush(s->f)) {
            s->why = errno;
            rc = -1;
        }
    }
    return found ? rc : -1;
}

/* everything written and every command ended, at the end of the program */
void streams_end(void) {
    while(streams) stream_end(streams);
    if(write_failed) A.exit_status = 2;
}
