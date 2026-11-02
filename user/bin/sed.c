/*
SurfOS user program: sed
--------------------
File: sed.c     Date: 11/01/26
--------------------
sed [-nEs] [-i[suffix]] [-e script] [-f file] [script] [file...]: a stream editor. Each line of the
files (standard input without them, - for it) goes into the pattern space, the script runs on
it, and it is printed. Options, which may come after the files as with GNU's sed, up to a --:

  -n          print only what p and the like ask for (so does a first line that is #n and nothing else)
  -e script   this text is script; -f file: the file's text is; several add up
  -E, -r      extended regular expressions (user/libsurf/regex.c; man regex)
  -s          each file on its own: line numbers and $ start again
  -i[suffix]  each file is replaced by what comes out, and -s holds. With a suffix the old file
              is kept as file<suffix>, or as the suffix with each * being the file's name

Addresses: n, $, /re/ and \cREc (I after one ignores case), first~step; a second one, after a comma:
those and +n, ~n; 0,/re/; ! after them turns them over. Commands: { } = a b c d D g G h H i l n N p P
q Q r s t T w x y : #, and s's flags g p I i w and a number, with & and \0-\9 in the replacement.

This sed follows GNU's where POSIX leaves a choice: one-line a i c, \n \t \cX \dNNN \oNNN \xHH in
patterns, replacements and y, q and Q with a status, a missing last newline stays missing. Not here:
case conversion in replacements, the flags m e, the commands e F R W z v, -z -u -l and the long
options.

A line is any number of bytes, with NUL bytes in it too, and a character is a UTF-8 sequence: y works on
characters, and an empty match in s///g steps over a whole one. l prints the bytes.

No input is read whole. What must go on disk is safe: -i and w write through saveutil.h, so that a
file is either as it was or complete, and a failure part of the way leaves it as it was. A w file that
did not exist appears when sed ends. The input is read through descriptors, as is the output, with buffers
of this program's own; all of this file's other state is in one struct, so that tests/host/test_sed.c
can run main() many times on a simulated file system, with writes and renames failing.

Status 0, q's or Q's number, 2 for trouble: a mistake in the script, a file that could not be
read (the others are still processed) or written. 141 when the reader of the output has gone.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <regex.h>
#include <surf.h>
#include "saveutil.h"

#define INPUT_BUF   16384
#define OUTPUT_BUF  8192
#define MAX_BLOCKS  256                 /* { inside { */
#define STATUS_TROUBLE 2
#define STATUS_PIPE 141
#define LIST_WIDTH  70                  /* l's lines */
#define UNTIL_SAID  ((unsigned long)-1) /* cmd.stop: the second address will say which line */

/* a growing string. nl, for a line: its newline was there, and is not kept here */
struct str {
    char *s;
    size_t len, cap;
    int nl;
};

/* where lines go, with what has not been written yet. owed: the last line went out without a newline, because its input had none,
   and the next thing written brings it */
struct sink {
    int fd;
    int owed;
    int failed;
    int unbuffered;
    size_t n;
    char *buf;
    struct sink *through;             /* w /dev/stdout: this one's buffer, for the order, not its missing newline */
};

struct wfile {
    struct wfile *next;
    char *name;
    struct sink o;
    struct su_save *save;               /* NULL: a device, standard output or standard error */
};

struct re {
    struct re *next;
    regex_t rx;
};

enum { AD_NONE, AD_LINE, AD_STEP, AD_LAST, AD_RE, AD_ZERO, AD_PLUS, AD_TILDE };

struct address {
    int type;
    unsigned long n, step;              /* the line; first and step; the count of +n and ~n */
    struct re *re;                      /* AD_RE: NULL for //, the last one used */
};

/* a piece of a replacement: text, or group >= 0 */
struct part {
    int group;
    char *text;
    size_t len;
};

struct replacing {
    struct re *re;
    struct part *parts;
    int nparts;
    char *text;
    int maxref;
    unsigned long nth;
    int every, show;                    /* the flags g and p */
    struct wfile *w;
};

/* y: pairs of characters. ascii[c]: the last pair for that character, -1 for none */
struct ypair {
    char from[4], to[4];
    int fromlen, tolen;
};

struct ymap {
    short ascii[128];
    struct ypair *pairs;
    int n;
};

struct cmd {
    struct address from, to;
    int negate;
    int c;                              /* the letter */
    int target;                         /* { b t T: the command to go on with */
    unsigned long began;                /* the line the last range began on, 0 for none yet */
    unsigned long stop;                 /* the range under way ends with this line, or UNTIL_SAID; 0: none is under way */
    char *label;                        /* b t T: until the labels are known */
    char *text;                         /* a i c; r: the file */
    size_t len;
    long number;                        /* q Q: the status; l: the width, -1 for the default */
    struct wfile *w;
    struct replacing *sub;
    struct ymap *y;
};

struct label {
    struct label *next;
    char *name;
    int at;
};

struct queued {                         /* what a and r leave for the end of the cycle */
    struct queued *next;
    const char *text;
    size_t len;
    const char *file;
};

static struct {
    /* options */
    int quiet, extended, separate, inplace;
    const char *suffix;
    /* the script as text, while it is read */
    char *sc;
    size_t sclen, sccap, at;
    int any_script;
    /* the script */
    struct cmd *prog;
    int ncmd, capcmd;
    struct label *labels;
    struct re *res;
    struct wfile *wfiles;
    int blocks[MAX_BLOCKS], nblocks;
    /* the input */
    char **files;
    int nfiles, nextfile;
    int fd;                             /* -1 for none */
    const char *name;
    int file_bad;                       /* it could not be read to its end: no replacing it */
    int file_read;                      /* it held something */
    int new_file;                       /* no line has been read from it yet */
    char *ibuf;
    size_t ipos, iend;
    unsigned long lineno;
    /* the spaces */
    struct str line, hold, built;
    struct queued *qhead, *qtail;
    int substituted;                       /* for t */
    struct re *last_re;
    /* the output */
    struct sink out;                  /* standard output */
    struct sink *cur;                 /* where lines go: out, or file with -i */
    struct sink file;
    struct su_save save;
    int saving;                         /* save is under way, for the input file save.path */
    int tty;
    /* how it ends */
    int fatal;
    int status;
} g;

/**** messages ****/

static void say(const char *fmt, ...) {
    va_list ap;
    fputs("sed: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    g.status = STATUS_TROUBLE;
}

/* the reason to stop now, said */
static void die_soon(const char *what) {
    if(!g.fatal) say("%s", what);
    g.fatal = 1;
}

/* a mistake in the script at the place reached */
static int bad(const char *why) {
    size_t i;
    int line = 1;
    for(i = 0; i < g.at && i < g.sclen; i++) line += g.sc[i] == '\n';
    say("script line %d: %s", line, why);
    return -1;
}

/**** strings ****/

static int str_room(struct str *b, size_t more) {
    size_t need = b->len + more + 1, cap;
    char *p;
    if(need < b->len) {
        die_soon("out of memory");
        return -1;
    }
    if(need <= b->cap) return 0;
    cap = b->cap ? b->cap : 128;
    while(cap < need) {
        if(cap > (size_t)-1 / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    p = realloc(b->s, cap);
    if(!p) {
        die_soon("out of memory");
        return -1;
    }
    b->s = p;
    b->cap = cap;
    return 0;
}

static int str_add(struct str *b, const char *p, size_t n) {
    if(str_room(b, n)) return -1;
    if(n) memcpy(b->s + b->len, p, n);
    b->len += n;
    b->s[b->len] = 0;
    return 0;
}

static int str_addc(struct str *b, int c) {
    char ch = (char)c;
    return str_add(b, &ch, 1);
}

static void *alloc(size_t n) {
    void *p = calloc(1, n);
    if(!p) die_soon("out of memory");
    return p;
}

/* the bytes of the character at s, n bytes there: a whole UTF-8 sequence, as user/libsurf/regex.c has them, or 1 */
static size_t char_bytes(const unsigned char *s, size_t n) {
    unsigned b = s[0];
    size_t len, k;
    if(b < 0xC2 || b > 0xF4) return 1;
    len = b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
    if(n < len) return 1;
    for(k = 1; k < len; k++) if((s[k] & 0xC0) != 0x80) return 1;
    if((b == 0xE0 && s[1] < 0xA0) || (b == 0xED && s[1] > 0x9F) || (b == 0xF0 && s[1] < 0x90) || (b == 0xF4 && s[1] > 0x8F)) return 1;
    return len;
}

/**** output ****/

static int push_out(struct sink *o) {
    size_t done = 0;
    while(done < o->n) {
        ssize_t w = write(o->fd, o->buf + done, o->n - done);
        if(w <= 0) {
            if(!o->failed) {
                o->failed = 1;
                if(errno == EPIPE && o == &g.out) {     /* the reader has gone: no fault of anyone's */
                    g.fatal = 1;
                    g.status = STATUS_PIPE;
                } else {
                    die_soon(strerror(w < 0 ? errno : EIO));
                }
            }
            break;
        }
        done += (size_t)w;
    }
    o->n = 0;
    return o->failed ? -1 : 0;
}

static void put(struct sink *o, const char *p, size_t n) {
    struct sink *b = o->through ? o->through : o;
    while(n) {
        size_t k = OUTPUT_BUF - b->n;
        if(k > n) k = n;
        memcpy(b->buf + b->n, p, k);
        b->n += k;
        p += k;
        n -= k;
        if(b->n == OUTPUT_BUF) push_out(b);
    }
}

static void line_start(struct sink *o) {
    if(o->owed) {
        put(o, "\n", 1);
        o->owed = 0;
    }
}

static void line_done(struct sink *o) {
    struct sink *b = o->through ? o->through : o;
    if(o->unbuffered || (b == &g.out && g.tty)) push_out(b);
}

static void put_line(struct sink *o, const char *p, size_t n, int nl) {
    line_start(o);
    put(o, p, n);
    if(nl) put(o, "\n", 1);
    else o->owed = 1;
    line_done(o);
}

/**** the input ****/

static void reset_ranges(void) {
    int i;
    for(i = 0; i < g.ncmd; i++) {
        g.prog[i].began = 0;
        g.prog[i].stop = g.prog[i].from.type == AD_ZERO ? UNTIL_SAID : 0;
    }
}

/* the file's new contents are done with: they replace it, or if it held nothing, or could not be read to the end, do not.
   The old file is copied to the backup before the rename, so that it is never gone from both names. */
static void backup_name(char *out, size_t size, const char *path) {
    const char *base = strrchr(path, '/'), *s;
    size_t n = 0, dirlen = 0;
    base = base ? base + 1 : path;
    dirlen = (size_t)(base - path);
    if(!strchr(g.suffix, '*')) {
        snprintf(out, size, "%s%s", path, g.suffix);
        return;
    }
    if(!strchr(g.suffix, '/')) n = (size_t)snprintf(out, size, "%.*s", (int)dirlen, path);
    for(s = g.suffix; *s && n < size - 1; s++) {
        if(*s == '*') n += (size_t)snprintf(out + n, size - n, "%s", base);
        else out[n++] = *s;
        if(n >= size) n = size - 1;
    }
    out[n] = 0;
}

static int copy_to_backup(const char *path) {
    struct su_save b;
    char name[PATH_MAX];
    char *buf = g.ibuf;
    int in, r;
    ssize_t n = 0;
    backup_name(name, sizeof(name), path);
    if(strlen(name) >= sizeof(name) - 1) {
        say("%s: the backup's name is too long", path);
        return -1;
    }
    if(su_begin(&b, name) < 0) {
        su_say("sed", &b, -1);
        g.status = STATUS_TROUBLE;
        return -1;
    }
    in = open(path, O_RDONLY);
    while(in >= 0 && (n = read(in, buf, INPUT_BUF)) > 0) {
        ssize_t done = 0;
        while(done < n) {
            ssize_t w = write(b.fd, buf + done, (size_t)(n - done));
            if(w <= 0) {
                n = -1;
                break;
            }
            done += w;
        }
        if(n < 0) break;
    }
    if(in < 0 || n < 0) {
        say("%s: %s", name, strerror(errno));
        if(in >= 0) close(in);
        su_abort(&b);
        return -1;
    }
    close(in);
    r = su_commit(&b);
    if(r < 0) {
        su_say("sed", &b, r);
        g.status = STATUS_TROUBLE;
        return -1;
    }
    return 0;
}

static void finish_file(int keep) {
    int r;
    if(!g.saving) return;
    g.saving = 0;
    if(g.file.n && !g.file.failed) push_out(&g.file);
    g.cur = &g.out;
    if(!keep || g.file.failed || g.file_bad || !g.file_read || (g.suffix && copy_to_backup(g.save.path) < 0)) {
        su_abort(&g.save);
        return;
    }
    r = su_commit(&g.save);
    if(r < 0) {
        su_say("sed", &g.save, r);
        g.status = STATUS_TROUBLE;
    }
}

static void close_input(void) {
    if(g.fd > 0) close(g.fd);
    g.fd = -1;
    g.ipos = g.iend = 0;
}

/* on to the next file, as far as it opens: 1, or 0 when there is none. With -i also what it needs for the output */
static int next_file(void) {
    while(g.nextfile < g.nfiles) {
        const char *name = g.files[g.nextfile++];
        struct stat st;
        if(!strcmp(name, "-") && !g.inplace) {
            g.fd = 0;
            name = "standard input";
        } else {
            if(g.inplace && (stat(name, &st) == 0 && !S_ISREG(st.type))) {
                say("%s: not a regular file", name);
                continue;
            }
            g.fd = open(name, O_RDONLY);
            if(g.fd < 0) {
                g.fd = -1;
                say("%s: %s", name, strerror(errno));
                continue;
            }
        }
        g.name = name;
        g.file_bad = g.file_read = 0;
        g.new_file = 1;
        if(g.inplace) {
            int e;
            if(su_begin(&g.save, name) < 0) {
                e = errno;
                close_input();
                errno = e;
                su_say("sed", &g.save, -1);
                g.status = STATUS_TROUBLE;
                continue;
            }
            g.file.fd = g.save.fd;
            g.file.n = 0;
            g.file.owed = g.file.failed = 0;
            g.saving = 1;
            g.cur = &g.file;
        }
        return 1;
    }
    return 0;
}

/* is there more in the file that is open? */
static int file_has_more(void) {
    ssize_t n;
    if(g.ipos < g.iend) return 1;
    if(g.fd < 0) return 0;
    n = read(g.fd, g.ibuf, INPUT_BUF);
    if(n > 0) {
        g.ipos = 0;
        g.iend = (size_t)n;
        g.file_read = 1;
        return 1;
    }
    if(n < 0) {
        say("%s: %s", g.name, strerror(errno));
        g.file_bad = 1;
    }
    close_input();
    return 0;
}

/* was that the last line? What is after it is looked at only when it matters, for $ n N: a terminal is not asked
   for a line before one is needed */
static int at_last(void) {
    if(file_has_more()) return 0;
    if(g.separate) return 1;
    while(next_file()) if(file_has_more()) return 0;
    return 1;
}

static void flush_queue(void) {
    struct queued *q;
    while((q = g.qhead)) {
        g.qhead = q->next;
        line_start(g.cur);
        if(q->text) put(g.cur, q->text, q->len);
        else {                                  /* a file that is not there is nothing */
            int fd = open(q->file, O_RDONLY);
            if(fd >= 0) {
                char *buf = malloc(OUTPUT_BUF);
                ssize_t n;
                if(!buf) die_soon("out of memory");
                while(buf && (n = read(fd, buf, OUTPUT_BUF)) > 0) put(g.cur, buf, (size_t)n);
                free(buf);
                close(fd);
            }
        }
        line_done(g.cur);
        free(q);
    }
    g.qtail = NULL;
}

static void enqueue(const char *text, size_t len, const char *file) {
    struct queued *q = alloc(sizeof(*q));
    if(!q) return;
    q->text = text;
    q->len = len;
    q->file = file;
    if(g.qtail) g.qtail->next = q;
    else g.qhead = q;
    g.qtail = q;
}

/* the next line into the pattern space, after a newline with append: 1, or 0 at the end of the input. A line ends
   with its file, whether or not a newline does */
static int next_line(int append) {
    flush_queue();
    g.substituted = 0;
    if(g.fatal) return 0;
    while(!file_has_more()) {
        finish_file(1);
        if(!next_file()) return 0;
    }
    if(g.new_file) {
        g.new_file = 0;
        if(g.separate) {                        /* a new file, with a line in it: all starts again */
            g.lineno = 0;
            g.hold.len = 0;
            reset_ranges();
        }
    }
    if(append) str_addc(&g.line, '\n');
    else g.line.len = 0;
    for(;;) {
        char *p = g.ibuf + g.ipos, *e;
        size_t n = g.iend - g.ipos, k;
        e = memchr(p, '\n', n);
        k = e ? (size_t)(e - p) : n;
        if(str_add(&g.line, p, k)) return 0;
        g.ipos += k;
        if(e) {
            g.ipos++;
            g.line.nl = 1;
            break;
        }
        if(!file_has_more()) {
            g.line.nl = 0;
            break;
        }
    }
    if(g.line.len > (size_t)INT_MAX) {
        die_soon("a line of more than 2 GB");
        return 0;
    }
    g.lineno++;
    return 1;
}

/**** the script: text ****/

static int at_end(void) {
    return g.at >= g.sclen;
}

static int peek(void) {
    return at_end() ? -1 : (unsigned char)g.sc[g.at];
}

static void skip_blanks(void) {
    while(!at_end() && (g.sc[g.at] == ' ' || g.sc[g.at] == '\t')) g.at++;
}

/* the pieces of the script join with newlines, so -e 'a\' -e 'text' is one command */
static int add_script(const char *text, size_t n) {
    struct str b = { g.sc, g.sclen, g.sccap, 0 };
    int r = str_add(&b, text, n) || str_addc(&b, '\n') ? -1 : 0;
    g.sc = b.s;
    g.sclen = b.len;
    g.sccap = b.cap;
    g.any_script = 1;
    return r;
}

static int add_script_file(const char *name) {
    struct str b = { NULL, 0, 0, 0 };
    char buf[1024];
    ssize_t n;
    int fd = strcmp(name, "-") ? open(name, O_RDONLY) : 0, r;
    if(fd < 0) {
        say("%s: %s", name, strerror(errno));
        return -1;
    }
    while((n = read(fd, buf, sizeof(buf))) > 0) {
        if(str_add(&b, buf, (size_t)n)) break;
    }
    if(n < 0) say("%s: %s", name, strerror(errno));
    if(fd) close(fd);
    if(n < 0 || g.fatal) {
        free(b.s);
        return -1;
    }
    if(b.len && b.s[b.len - 1] == '\n') b.len--;        /* add_script() puts it back */
    r = add_script(b.s ? b.s : "", b.len);
    free(b.s);
    return r;
}

static int read_number(unsigned long *out) {
    unsigned long v = 0;
    if(!(peek() >= '0' && peek() <= '9')) return -1;
    for(; peek() >= '0' && peek() <= '9'; g.at++) {
        unsigned d = (unsigned)(g.sc[g.at] - '0');
        if(v > (ULONG_MAX - d) / 10) return bad("a number too large");
        v = v * 10 + d;
    }
    *out = v;
    return 0;
}

/* the value of a digit in any base up to 16, 99 for what is none */
static int digit_value(int c) {
    static const char digits[] = "0123456789abcdef";
    const char *p = c ? strchr(digits, c >= 'A' && c <= 'F' ? c + 32 : c) : NULL;
    return p ? (int)(p - digits) : 99;
}

/* The character that the n bytes at s stand for, s being just after a backslash: a letter of the table, c and any character, or d,
   o or x and digits. The bytes it takes, 0 when it is none of those. */
static size_t escaped_char(const char *s, size_t n, int *c) {
    static const struct {
        char letter, value;
    } letters[] = { { 'a', 7 }, { 'f', 12 }, { 'n', '\n' }, { 'r', 13 }, { 't', 9 }, { 'v', 11 } };
    static const struct {
        char letter, base, digits;
    } numbers[] = { { 'd', 10, 3 }, { 'o', 8, 3 }, { 'x', 16, 2 } };
    size_t i, k;
    int value = 0;
    for(i = 0; i < sizeof(letters) / sizeof(letters[0]); i++) {
        if(letters[i].letter == s[0]) {
            *c = letters[i].value;
            return 1;
        }
    }
    if(s[0] == 'c') {
        if(n < 2) return 0;
        *c = (s[1] >= 'a' && s[1] <= 'z' ? s[1] - 32 : s[1]) ^ 0x40;
        return 2;
    }
    for(i = 0; i < sizeof(numbers) / sizeof(numbers[0]); i++) {
        if(numbers[i].letter != s[0]) continue;
        for(k = 1; k < n && k <= (size_t)numbers[i].digits && digit_value(s[k]) < numbers[i].base; k++) {
            value = value * numbers[i].base + digit_value(s[k]);
        }
        if(k == 1) return 0;
        *c = value & 0xFF;
        return k;
    }
    return 0;
}

/* [ just passed, in the script: the bracket expression into b, for the text up to a delimiter: 0, -1 when it does not end.
   A delimiter inside one is no delimiter. */
static int copy_bracket(struct str *b) {
    int c, d;
    if(str_addc(b, '[')) return -1;
    if(peek() == '^') {
        str_addc(b, '^');
        g.at++;
    }
    if(peek() == ']') {
        str_addc(b, ']');
        g.at++;
    }
    for(;;) {
        c = peek();
        if(c < 0 || c == '\n') return -1;
        g.at++;
        if(str_addc(b, c)) return -1;
        if(c == ']') return 0;
        if(c == '[' && (peek() == '.' || peek() == ':' || peek() == '=')) {
            d = peek();
            g.at++;
            str_addc(b, d);
            for(;;) {
                c = peek();
                if(c < 0 || c == '\n') return -1;
                g.at++;
                str_addc(b, c);
                if(c == d && peek() == ']') {
                    g.at++;
                    str_addc(b, ']');
                    break;
                }
            }
        }
    }
}

enum { IN_REPLACEMENT, IN_PATTERN, IN_Y };          /* the three kinds of piece of a script */

/* What a backslash and c, the character after it, come to in a piece of script, as POSIX has it: the delimiter stands for itself, a
   pattern's \n is a newline already, and in a replacement \<newline> is a newline. All other pairs stay for regcomp(),
   parse_replacement() or y_chars(), which is why \& stays in a replacement that has & as its delimiter. */
static void add_pair(struct str *to, int c, int delim, int piece) {
    enum { STAYS, CHAR, NEWLINE };
    static const char meaning[3][5] = {
        /* the delimiter, & as the delimiter, a newline, n, any other */
        { CHAR, STAYS, CHAR, STAYS, STAYS },            /* IN_REPLACEMENT */
        { CHAR, CHAR, CHAR, NEWLINE, STAYS },           /* IN_PATTERN */
        { CHAR, CHAR, CHAR, STAYS, STAYS },             /* IN_Y */
    };
    int kind = c == delim ? c == '&' : c == '\n' ? 2 : c == 'n' ? 3 : 4;
    switch(meaning[piece][kind]) {
    case STAYS:
        str_addc(to, '\\');
        str_addc(to, c);
        break;
    case CHAR:
        str_addc(to, c);
        break;
    default:
        str_addc(to, '\n');
    }
}

/* The piece of script up to the delimiter, which is passed: NULL when a line or the script ends first. Runs of bytes that mean
   nothing here go across whole; only the bytes in stop[] are looked at. */
static char *take_piece(int delim, int piece, size_t *len) {
    char stop[256];
    struct str b = { NULL, 0, 0, 0 };
    memset(stop, 0, sizeof(stop));
    stop['\n'] = stop['\\'] = stop[delim] = 1;
    if(piece == IN_PATTERN) stop['['] = 1;
    if(str_room(&b, 0)) return NULL;
    b.s[0] = 0;
    for(;;) {
        size_t from = g.at;
        int c;
        while(!at_end() && !stop[(unsigned char)g.sc[g.at]]) g.at++;
        str_add(&b, g.sc + from, g.at - from);
        if(at_end()) break;
        c = (unsigned char)g.sc[g.at++];
        if(c == delim) {
            *len = b.len;
            return b.s;
        }
        if(c == '\n' || (c == '\\' && at_end())) break;
        if(c == '\\') add_pair(&b, (unsigned char)g.sc[g.at++], delim, piece);
        else if(copy_bracket(&b)) break;
    }
    free(b.s);
    return NULL;
}

/* A pattern as regcomp() wants it: the escapes that are characters made so, in a bracket too */
static char *pattern_text(const char *s, size_t n) {
    struct str b = { NULL, 0, 0, 0 };
    size_t i = 0;
    int c;
    str_room(&b, n);
    if(g.fatal) return NULL;
    b.s[0] = 0;
    while(i < n) {
        size_t k;
        if(s[i] == '\\' && i + 1 < n) {
            k = escaped_char(s + i + 1, n - i - 1, &c);
            if(k) {
                str_addc(&b, c);
                i += 1 + k;
            } else {
                str_add(&b, s + i, 2);
                i += 2;
            }
            continue;
        }
        if(s[i] != '[') {
            str_add(&b, s + i, 1);
            i++;
            continue;
        }
        str_addc(&b, '[');                      /* a bracket: up to its end, as copy_bracket() found it */
        i++;
        if(i < n && s[i] == '^') str_addc(&b, s[i++]);
        if(i < n && s[i] == ']') str_addc(&b, s[i++]);
        while(i < n && s[i] != ']') {
            if(s[i] == '[' && i + 1 < n && (s[i + 1] == '.' || s[i + 1] == ':' || s[i + 1] == '=')) {
                int d = s[i + 1];
                str_add(&b, s + i, 2);
                i += 2;
                while(i < n && !(s[i] == d && i + 1 < n && s[i + 1] == ']')) str_addc(&b, s[i++]);
                if(i < n) {
                    str_add(&b, s + i, 2);
                    i += 2;
                }
                continue;
            }
            if(s[i] == '\\' && i + 1 < n && (k = escaped_char(s + i + 1, n - i - 1, &c))) {
                str_addc(&b, c);
                i += 1 + k;
                continue;
            }
            str_addc(&b, s[i++]);
        }
        if(i < n) str_addc(&b, s[i++]);
    }
    if(g.fatal) {
        free(b.s);
        return NULL;
    }
    if(strlen(b.s) != b.len) {
        free(b.s);
        bad("a pattern cannot hold a NUL byte");
        return NULL;
    }
    return b.s;
}

/* the compiled pattern for s, freed here; NULL with no error for the empty pattern, which means the last one used */
static int make_re(char *s, size_t n, int icase, struct re **out) {
    struct re *re;
    char *text, msg[128];
    int rc;
    *out = NULL;
    if(!n) {
        free(s);
        return icase ? bad("an empty pattern takes no flags") : 0;
    }
    text = pattern_text(s, n);
    free(s);
    if(!text) return -1;
    re = alloc(sizeof(*re));
    if(!re) {
        free(text);
        return -1;
    }
    rc = regcomp(&re->rx, text, (g.extended ? REG_EXTENDED : 0) | (icase ? REG_ICASE : 0));
    free(text);
    if(rc) {
        regerror(rc, &re->rx, msg, sizeof(msg));
        free(re);
        return bad(msg);
    }
    re->next = g.res;
    g.res = re;
    *out = re;
    return 0;
}

static struct wfile *open_wfile(const char *name) {
    struct wfile *w;
    struct stat st;
    for(w = g.wfiles; w; w = w->next) if(!strcmp(w->name, name)) return w;
    w = alloc(sizeof(*w));
    if(!w) return NULL;
    w->name = strdup(name);
    w->o.buf = malloc(OUTPUT_BUF);
    w->next = g.wfiles;
    g.wfiles = w;
    if(!w->name || !w->o.buf) {
        die_soon("out of memory");
        return NULL;
    }
    w->o.fd = -1;
    if(!strcmp(name, "/dev/stdout")) {
        w->o.through = &g.out;
        return w;
    }
    if(!strcmp(name, "/dev/stderr")) {
        w->o.fd = 2;
        w->o.unbuffered = 1;
        return w;
    }
    if(!strncmp(name, "/dev/", 5) || (stat(name, &st) == 0 && !S_ISREG(st.type))) {     /* a device: there is nothing to keep */
        w->o.fd = open(name, O_WRONLY);
        if(w->o.fd < 0) {
            say("%s: %s", name, strerror(errno));
            return NULL;
        }
        return w;
    }
    w->save = alloc(sizeof(*w->save));
    if(!w->save) return NULL;
    if(su_begin(w->save, name) < 0) {
        su_say("sed", w->save, -1);
        g.status = STATUS_TROUBLE;
        free(w->save);
        w->save = NULL;
        return NULL;
    }
    w->o.fd = w->save->fd;
    return w;
}

/* the rest of the line, for r and w */
static char *take_filename(void) {
    size_t from, n;
    char *s;
    skip_blanks();
    from = g.at;
    while(!at_end() && g.sc[g.at] != '\n') g.at++;
    n = g.at - from;
    if(!n) {
        bad("a file name is missing");
        return NULL;
    }
    s = malloc(n + 1);
    if(!s) {
        die_soon("out of memory");
        return NULL;
    }
    memcpy(s, g.sc + from, n);
    s[n] = 0;
    return s;
}

/* a label ends at a blank, a ; or a }, and what follows it is the next command */
static char *take_label(void) {
    size_t from, n;
    char *s;
    skip_blanks();
    from = g.at;
    while(!at_end() && !strchr(" \t\n;}", g.sc[g.at])) g.at++;
    n = g.at - from;
    s = malloc(n + 1);
    if(!s) {
        die_soon("out of memory");
        return NULL;
    }
    memcpy(s, g.sc + from, n);
    s[n] = 0;
    return s;
}

/* After a command there may be blanks, and then only what parse_script() itself goes on from: a ; or a newline, a }, a #, or the end */
static int command_done(void) {
    skip_blanks();
    if(at_end() || memchr(";\n}#", g.sc[g.at], 4)) return 0;
    return bad("extra characters after a command");
}

/* The text of a i c. Classic form: a\, a newline, lines ending in \ but the last. One-line form: blanks are dropped and
   the line is the text; after a\ on the same line they are kept. A backslash goes, leaving what it stood before
   (a newline does not end the text then), and \t and the like are characters. */
static int take_text(struct cmd *c) {
    struct str b = { NULL, 0, 0, 0 };
    int ch;
    skip_blanks();
    if(at_end() || peek() == '\n') return bad("a text is missing after a, i or c");
    if(peek() == '\\') {
        g.at++;
        skip_blanks();
        if(peek() == '\n') {
            g.at++;
            if(at_end()) {                      /* a\ and no more: nothing, not even an empty line */
                c->text = malloc(1);
                if(!c->text) die_soon("out of memory");
                return g.fatal ? -1 : 0;
            }
        } else if(!at_end()) {                  /* a\text: blanks stay, and so the skipping is undone */
            while(g.at > 0 && (g.sc[g.at - 1] == ' ' || g.sc[g.at - 1] == '\t')) g.at--;
        }
    }
    str_room(&b, 0);
    if(g.fatal) return -1;
    b.s[0] = 0;
    while(!at_end()) {
        ch = (unsigned char)g.sc[g.at++];
        if(ch == '\n') break;
        if(ch == '\\' && !at_end()) {
            int e;
            size_t k = escaped_char(g.sc + g.at, g.sclen - g.at, &e);
            if(k && g.sc[g.at] != 'n') {        /* \n here is n, as in GNU's sed */
                g.at += k;
                ch = e;
            } else {
                ch = (unsigned char)g.sc[g.at++];
            }
        }
        str_addc(&b, ch);
    }
    str_addc(&b, '\n');
    if(g.fatal) {
        free(b.s);
        return -1;
    }
    c->text = b.s;
    c->len = b.len;
    return 0;
}

/**** the script: s and y ****/

static int add_part(struct replacing *s, int group, char *text, size_t len) {
    struct part *p = realloc(s->parts, (size_t)(s->nparts + 1) * sizeof(*p));
    if(!p) {
        die_soon("out of memory");
        return -1;
    }
    s->parts = p;
    p[s->nparts].group = group;
    p[s->nparts].text = text;
    p[s->nparts++].len = len;
    return 0;
}

/* the replacement into pieces; the literal pieces live in s->text, which is as long as t at most, plus nothing */
static int parse_replacement(struct replacing *sub, const char *t, size_t n) {
    char *q, *lit;
    size_t i = 0;
    sub->text = q = lit = malloc(n + 1);
    if(!q) {
        die_soon("out of memory");
        return -1;
    }
    while(i < n) {
        int c = (unsigned char)t[i++], group = -1;
        if(c == '&') group = 0;
        else if(c == '\\' && i < n) {
            int d = (unsigned char)t[i], e;
            size_t k;
            if(d >= '0' && d <= '9') {
                group = d - '0';
                i++;
            } else if((k = escaped_char(t + i, n - i, &e))) {
                c = e;
                i += k;
            } else {
                c = d;
                i++;
            }
        }
        if(group < 0) {
            *q++ = (char)c;
            continue;
        }
        if(q > lit && add_part(sub, -1, lit, (size_t)(q - lit))) return -1;
        lit = q;
        if(add_part(sub, group, NULL, 0)) return -1;
        if(group > sub->maxref) sub->maxref = group;
    }
    if(q > lit && add_part(sub, -1, lit, (size_t)(q - lit))) return -1;
    return 0;
}

static int parse_s(struct cmd *c) {
    struct replacing *sub = alloc(sizeof(*sub));
    size_t relen, rlen;
    char *re, *rp;
    int delim, icase = 0, num = 0;
    if(!sub) return -1;
    c->sub = sub;
    sub->nth = 1;
    delim = peek();
    if(delim < 0 || delim == '\n' || delim >= 0x80) return bad("s needs a delimiter of one byte");
    g.at++;
    if(!(re = take_piece(delim, IN_PATTERN, &relen))) return g.fatal ? -1 : bad("s is not finished");
    if(!(rp = take_piece(delim, IN_REPLACEMENT, &rlen))) {
        free(re);
        return g.fatal ? -1 : bad("s is not finished");
    }
    if(parse_replacement(sub, rp, rlen)) {
        free(re);
        free(rp);
        return -1;
    }
    free(rp);
    for(;;) {
        int f = peek();
        if(f == 'g') {
            if(sub->every) { free(re); return bad("s with g twice"); }
            sub->every = 1;
            g.at++;
        } else if(f == 'p') {
            if(sub->show) { free(re); return bad("s with p twice"); }
            sub->show = 1;
            g.at++;
        } else if(f == 'I' || f == 'i') {
            icase = 1;
            g.at++;
        } else if(f >= '0' && f <= '9') {
            if(num) { free(re); return bad("s with two numbers"); }
            num = 1;
            if(read_number(&sub->nth) < 0) { free(re); return -1; }
            if(!sub->nth) { free(re); return bad("the number of s cannot be 0"); }
        } else if(f == 'w') {
            char *name;
            g.at++;
            if(!(name = take_filename())) { free(re); return -1; }
            sub->w = open_wfile(name);
            free(name);
            if(!sub->w) { free(re); return -1; }
            break;
        } else if(f == ' ' || f == '\t') {
            g.at++;
        } else {
            if(f < 0 || f == '\n' || f == ';' || f == '}' || f == '#') break;
            free(re);
            return bad("an unknown flag of s");
        }
    }
    if(make_re(re, relen, icase, &sub->re)) return -1;
    if(sub->re && (size_t)sub->maxref > sub->re->rx.re_nsub) return bad("a \\N in the replacement that the pattern has no group for");
    return command_done();
}

/* the characters of a string of y into list: each 1 to 4 bytes */
static int y_chars(const char *s, size_t n, char (*list)[4], int *lens, int *count) {
    size_t i = 0;
    int k = 0;
    while(i < n) {
        char one[4];
        int len = 1, e;
        size_t used;
        if(s[i] == '\\' && i + 1 < n && s[i + 1] == '\\') {
            one[0] = '\\';
            i += 2;
        } else if(s[i] == '\\' && i + 1 < n && (used = escaped_char(s + i + 1, n - i - 1, &e))) {
            one[0] = (char)e;
            i += 1 + used;
        } else {
            len = (int)char_bytes((const unsigned char *)s + i, n - i);
            memcpy(one, s + i, (size_t)len);
            i += (size_t)len;
        }
        memcpy(list[k], one, (size_t)len);
        lens[k++] = len;
    }
    *count = k;
    return 0;
}

static int parse_y(struct cmd *c) {
    struct ymap *y = alloc(sizeof(*y));
    char *from = NULL, *to = NULL, (*fl)[4] = NULL, (*tl)[4] = NULL;
    int *fn = NULL, *tn = NULL, nf = 0, nt = 0, i, r = -1, delim;
    size_t lf, lt;
    if(!y) return -1;
    c->y = y;
    delim = peek();
    if(delim < 0 || delim == '\n' || delim >= 0x80) return bad("y needs a delimiter of one byte");
    g.at++;
    if(!(from = take_piece(delim, IN_Y, &lf)) || !(to = take_piece(delim, IN_Y, &lt))) {
        if(!g.fatal) bad("y is not finished");
        goto out;
    }
    fl = malloc((lf + 1) * 4);
    tl = malloc((lt + 1) * 4);
    fn = malloc((lf + 1) * sizeof(int));
    tn = malloc((lt + 1) * sizeof(int));
    if(!fl || !tl || !fn || !tn) {
        die_soon("out of memory");
        goto out;
    }
    y_chars(from, lf, fl, fn, &nf);
    y_chars(to, lt, tl, tn, &nt);
    if(nf != nt) {
        bad("the two strings of y have different lengths");
        goto out;
    }
    y->pairs = malloc(((size_t)nf + 1) * sizeof(*y->pairs));
    if(!y->pairs) {
        die_soon("out of memory");
        goto out;
    }
    y->n = nf;
    for(i = 0; i < 128; i++) y->ascii[i] = -1;
    for(i = 0; i < nf; i++) {
        memcpy(y->pairs[i].from, fl[i], 4);
        memcpy(y->pairs[i].to, tl[i], 4);
        y->pairs[i].fromlen = fn[i];
        y->pairs[i].tolen = tn[i];
        if(fn[i] == 1 && (unsigned char)fl[i][0] < 128) y->ascii[(unsigned char)fl[i][0]] = i < 32767 ? (short)i : -1;
    }
    r = command_done();
out:
    free(from);
    free(to);
    free(fl);
    free(tl);
    free(fn);
    free(tn);
    return r;
}

/**** the script: addresses and commands ****/

/* an address at the place reached: 1, 0 for none here, -1 for a mistake */
static int parse_addr(struct address *a, int second) {
    int c = peek();
    if(c >= '0' && c <= '9') {
        if(read_number(&a->n) < 0) return -1;
        a->type = AD_LINE;
        skip_blanks();
        if(peek() == '~') {
            g.at++;
            skip_blanks();
            a->step = 0;
            if(peek() >= '0' && peek() <= '9' && read_number(&a->step) < 0) return -1;
            if(a->step) a->type = AD_STEP;
        }
        return 1;
    }
    if(c == '$') {
        g.at++;
        a->type = AD_LAST;
        return 1;
    }
    if(second && (c == '+' || c == '~')) {
        g.at++;
        skip_blanks();
        if(read_number(&a->n) < 0) return bad("a number is missing after + or ~");
        a->type = c == '+' ? AD_PLUS : AD_TILDE;
        return 1;
    }
    if(c == '/' || c == '\\') {
        char *re;
        size_t n;
        int icase = 0;
        g.at++;
        if(c == '\\') {
            c = peek();
            if(c < 0 || c == '\n' || c >= 0x80) return bad("an address needs a delimiter of one byte");
            g.at++;
        }
        if(!(re = take_piece(c, IN_PATTERN, &n))) return g.fatal ? -1 : bad("an address is not finished");
        a->type = AD_RE;
        for(;;) {
            skip_blanks();
            if(peek() != 'I') break;
            icase = 1;
            g.at++;
        }
        if(make_re(re, n, icase, &a->re)) return -1;
        return 1;
    }
    return 0;
}

static int new_cmd(void) {
    if(g.ncmd == g.capcmd) {
        int cap = g.capcmd ? g.capcmd * 2 : 32;
        struct cmd *p = realloc(g.prog, (size_t)cap * sizeof(*p));
        if(!p) {
            die_soon("out of memory");
            return -1;
        }
        g.prog = p;
        g.capcmd = cap;
    }
    memset(&g.prog[g.ncmd], 0, sizeof(g.prog[0]));
    g.prog[g.ncmd].number = -1;
    return g.ncmd++;
}

static int parse_script(void) {
    /* #n and nothing else on the line. GNU's 4.9 takes any comment that starts so, and sed -i then empties the file */
    if(g.sclen >= 2 && g.sc[0] == '#' && g.sc[1] == 'n' && (g.sclen == 2 || g.sc[2] == '\n')) g.quiet = 1;
    for(;;) {
        struct cmd *c;
        struct address from, to;
        int ch, negate = 0, i, r;
        while(!at_end() && strchr(" \t\n;", g.sc[g.at])) g.at++;
        if(at_end()) break;
        memset(&from, 0, sizeof(from));
        memset(&to, 0, sizeof(to));
        r = parse_addr(&from, 0);
        if(r < 0) return -1;
        if(r) {
            skip_blanks();
            if(peek() == ',') {
                g.at++;
                skip_blanks();
                r = parse_addr(&to, 1);
                if(r < 0) return -1;
                if(!r) return bad("an address is missing after the comma");
            }
            if(from.type == AD_LINE && !from.n) {
                if(to.type != AD_RE) return bad("line 0 is only for 0,/re/");
                from.type = AD_ZERO;
            }
        }
        skip_blanks();
        if(peek() == '!') {
            negate = 1;
            g.at++;
            skip_blanks();
            if(peek() == '!') return bad("! twice");
        }
        ch = peek();
        if(ch < 0) return bad("a command is missing");
        g.at++;
        if(ch == '#') {
            if(from.type != AD_NONE) return bad("a comment takes no address");
            while(!at_end() && g.sc[g.at] != '\n') g.at++;
            continue;
        }
        if(ch == '}') {
            if(from.type != AD_NONE) return bad("} takes no address");
            if(!g.nblocks) return bad("} without its {");
            g.prog[g.blocks[--g.nblocks]].target = g.ncmd;
            if(command_done()) return -1;
            continue;
        }
        if(ch == ':') {
            struct label *l;
            char *name;
            if(from.type != AD_NONE) return bad(": takes no address");
            if(!(name = take_label())) return -1;
            if(!*name) {
                free(name);
                return bad("a label is missing");
            }
            l = alloc(sizeof(*l));
            if(!l) {
                free(name);
                return -1;
            }
            l->name = name;
            l->at = g.ncmd;
            l->next = g.labels;
            g.labels = l;
            continue;
        }
        if((i = new_cmd()) < 0) return -1;
        c = &g.prog[i];
        c->from = from;
        c->to = to;
        c->negate = negate;
        c->c = ch;
        switch(ch) {
        case '{':
            if(g.nblocks == MAX_BLOCKS) return bad("{ inside { too deep");
            g.blocks[g.nblocks++] = i;
            break;
        case '=': case 'd': case 'D': case 'g': case 'G': case 'h': case 'H': case 'n': case 'N': case 'p': case 'P': case 'x':
            if(command_done()) return -1;
            break;
        case 'a': case 'i': case 'c':
            if(take_text(c)) return -1;
            break;
        case 'b': case 't': case 'T':
            if(!(c->label = take_label())) return -1;
            break;
        case 'q': case 'Q': case 'l': {
            unsigned long v;
            if((ch == 'q' || ch == 'Q') && to.type != AD_NONE) return bad("q and Q take one address");
            skip_blanks();
            if(peek() >= '0' && peek() <= '9') {
                if(read_number(&v) < 0) return -1;
                c->number = v > 0x7FFFFFFFUL ? 0x7FFFFFFFL : (long)v;
            }
            if(command_done()) return -1;
            break;
        }
        case 'r':
            if(!(c->text = take_filename())) return -1;
            break;
        case 'w': {
            char *name = take_filename();
            if(!name) return -1;
            c->w = open_wfile(name);
            free(name);
            if(!c->w) return -1;
            break;
        }
        case 's':
            if(parse_s(c)) return -1;
            break;
        case 'y':
            if(parse_y(c)) return -1;
            break;
        default:
            g.at--;
            return bad("an unknown command");
        }
    }
    if(g.nblocks) return bad("{ without its }");
    return 0;
}

/* the jumps: a b to no label goes to the end */
static int resolve_labels(void) {
    int i;
    for(i = 0; i < g.ncmd; i++) {
        struct cmd *c = &g.prog[i];
        struct label *l;
        if(c->c != 'b' && c->c != 't' && c->c != 'T') continue;
        c->target = g.ncmd;
        if(!*c->label) continue;
        for(l = g.labels; l; l = l->next) if(!strcmp(l->name, c->label)) break;
        if(!l) {
            say("no label %s to jump to", c->label);
            return -1;
        }
        c->target = l->at;
    }
    return 0;
}

/**** running it ****/

/* the pattern, or the last one used, in the pattern space from offset from: 1 or 0, and -1 with the run stopped */
static int search(struct re *re, size_t from, regmatch_t *m, size_t nm) {
    char msg[128];
    int rc;
    if(!re) re = g.last_re;
    if(!re) {
        die_soon("no previous regular expression");
        return -1;
    }
    g.last_re = re;
    m[0].rm_so = (regoff_t)from;
    m[0].rm_eo = (regoff_t)g.line.len;
    rc = regexec(&re->rx, g.line.s, nm, m, REG_STARTEND);
    if(!rc) return 1;
    if(rc == REG_NOMATCH) return 0;
    regerror(rc, &re->rx, msg, sizeof(msg));
    die_soon(msg);
    return -1;
}

static int addr_hit(const struct address *a) {
    regmatch_t m[1];
    switch(a->type) {
    case AD_LINE: return g.lineno == a->n;
    case AD_STEP: return g.lineno >= a->n && (g.lineno - a->n) % a->step == 0;
    case AD_LAST: return at_last();
    case AD_RE: return search(a->re, 0, m, 0) > 0;
    }
    return 0;
}

/* the line a range that starts on this one stops at, if its second address can say now. A range to a line already passed is this
   line alone, as is +0; a line that is a multiple of N goes on to the next one under ~N */
static unsigned long range_stop(const struct cmd *c) {
    unsigned long n = c->to.n;
    switch(c->to.type) {
    case AD_LINE: return n;
    case AD_PLUS: return g.lineno + n;
    case AD_TILDE: return n ? (g.lineno / n + 1) * n : g.lineno;
    }
    return UNTIL_SAID;
}

/* Does a range start on this line? A first address that is a line number starts one range in a run, and n or N may have carried
   sed past that line: it starts where it is seen, unless the whole range was jumped over. */
static int range_starts(struct cmd *c) {
    unsigned long l = g.lineno, stop;
    if(c->from.type != AD_LINE) {
        if(!addr_hit(&c->from)) return 0;
    } else if(c->began || l < c->from.n || (l > c->from.n && c->to.type == AD_LINE && l > c->to.n)) {
        return 0;
    }
    stop = range_stop(c);
    c->began = l;
    c->stop = stop > l ? stop : 0;
    return 1;
}

/* Is this line still in the range under way? A pattern as the second address is first tried on the line after the range began.
   Past the end by a jump: a line number was the end and this line is out, but +N and ~N keep it, as the one that ends them. */
static int range_goes_on(struct cmd *c) {
    unsigned long l = g.lineno, stop = c->stop;
    if(stop == UNTIL_SAID) {
        if(addr_hit(&c->to)) c->stop = 0;
        return 1;
    }
    if(l < stop) return 1;
    c->stop = 0;
    return c->to.type != AD_LINE || l == stop;
}

static int in_range(struct cmd *c) {
    return c->stop ? range_goes_on(c) : range_starts(c);
}

static int selected(struct cmd *c) {
    int hit;
    if(c->from.type == AD_NONE) hit = 1;
    else if(c->to.type == AD_NONE) hit = addr_hit(&c->from);
    else hit = in_range(c);
    return hit != c->negate;
}

static void substitute(struct cmd *c) {
    struct replacing *s = c->sub;
    regmatch_t m[10];
    size_t start = 0, copied = 0, n = g.line.len, prev_end = (size_t)-1;
    unsigned long count = 0;
    int did = 0, i, r;
    g.built.len = 0;
    if(str_room(&g.built, n)) return;
    g.built.s[0] = 0;
    while(start <= n) {
        size_t so, eo;
        r = search(s->re, start, m, (size_t)s->maxref + 1);
        if(r <= 0) {
            if(r < 0) return;
            break;
        }
        if(g.last_re->rx.re_nsub < (size_t)s->maxref) {     /* only an empty pattern gets here */
            die_soon("a \\N in the replacement that the pattern has no group for");
            return;
        }
        so = (size_t)m[0].rm_so;
        eo = (size_t)m[0].rm_eo;
        if(so == eo && so == prev_end) {        /* nothing, right after the last match: that is no match */
            if(so >= n) break;
            start = so + char_bytes((const unsigned char *)g.line.s + so, n - so);
            continue;
        }
        prev_end = eo;
        if(++count >= s->nth) {
            if(str_add(&g.built, g.line.s + copied, so - copied)) return;
            for(i = 0; i < s->nparts; i++) {
                const struct part *p = &s->parts[i];
                if(p->group < 0) {
                    if(str_add(&g.built, p->text, p->len)) return;
                } else if(m[p->group].rm_so >= 0) {
                    if(str_add(&g.built, g.line.s + m[p->group].rm_so, (size_t)(m[p->group].rm_eo - m[p->group].rm_so))) return;
                }
            }
            copied = eo;
            did = 1;
            if(!s->every) break;
        }
        if(so == eo) {
            if(so >= n) break;
            start = so + char_bytes((const unsigned char *)g.line.s + so, n - so);
        } else {
            start = eo;
        }
    }
    if(!did) return;
    if(str_add(&g.built, g.line.s + copied, n - copied)) return;
    {
        struct str t = g.line;
        int nl = g.line.nl;
        g.line = g.built;
        g.built = t;
        g.line.nl = nl;
    }
    g.substituted = 1;
    if(s->show) put_line(g.cur, g.line.s, g.line.len, g.line.nl);
    if(s->w) put_line(&s->w->o, g.line.s, g.line.len, g.line.nl);
}

static void translate(const struct ymap *y) {
    size_t i = 0, n = g.line.len;
    g.built.len = 0;
    if(str_room(&g.built, n)) return;
    while(i < n) {
        const unsigned char *p = (const unsigned char *)g.line.s + i;
        size_t k = char_bytes(p, n - i);
        const struct ypair *hit = NULL;
        if(*p < 128) {
            if(y->ascii[*p] >= 0) hit = &y->pairs[y->ascii[*p]];
        } else {
            int t;
            for(t = 0; t < y->n; t++) {
                if((size_t)y->pairs[t].fromlen == k && !memcmp(y->pairs[t].from, p, k)) hit = &y->pairs[t];
            }
        }
        if(hit ? str_add(&g.built, hit->to, (size_t)hit->tolen) : str_add(&g.built, (const char *)p, k)) return;
        i += k;
    }
    {
        struct str t = g.line;
        int nl = g.line.nl;
        g.line = g.built;
        g.built = t;
        g.line.nl = nl;
    }
    if(!g.line.s) str_add(&g.line, "", 0);
}

/* what l shows for a byte, as POSIX asks: the escapes it names, octal for what cannot be seen */
static void spell_byte(struct str *to, int c) {
    static const char named[] = "abtnvfr";      /* 7 to 13 */
    char octal[8];
    if(c == '\\') str_add(to, "\\\\", 2);
    else if(c >= ' ' && c < 127) str_addc(to, c);
    else if(c >= 7 && c <= 13) {
        str_addc(to, '\\');
        str_addc(to, named[c - 7]);
    } else {
        snprintf(octal, sizeof(octal), "\\%03o", (unsigned)c);
        str_add(to, octal, 4);
    }
}

/* the length of what spell_byte() wrote at p: an octal number, another escape, or a character */
static size_t spelled_len(const char *p) {
    if(p[0] != '\\') return 1;
    return p[1] >= '0' && p[1] <= '7' ? 4 : 2;
}

/* l: the pattern space spelled out, then cut into lines of width - 1 columns and a backslash, at the edges of what was spelled. After
   a cut one always goes, however narrow the width. */
static void list_line(long width) {
    size_t room = width > 0 ? (size_t)width - 1 : (size_t)-1, i, at = 0;
    int cut = 0;
    g.built.len = 0;
    for(i = 0; i < g.line.len; i++) spell_byte(&g.built, (unsigned char)g.line.s[i]);
    if(g.fatal) return;
    line_start(g.cur);
    for(;;) {
        size_t n = cut ? spelled_len(g.built.s + at) : 0;
        while(at + n < g.built.len && n + spelled_len(g.built.s + at + n) <= room) n += spelled_len(g.built.s + at + n);
        put(g.cur, g.built.s + at, n);
        at += n;
        if(at == g.built.len) break;
        put(g.cur, "\\\n", 2);
        cut = 1;
    }
    put(g.cur, "$\n", 2);
    line_done(g.cur);
}

static int put_number(unsigned long v) {
    char buf[24];
    int n = snprintf(buf, sizeof(buf), "%lu\n", v);
    line_start(g.cur);
    put(g.cur, buf, (size_t)n);
    line_done(g.cur);
    return 0;
}

/* one space from another. Whether a newline ended it goes with the text, as it does in GNU's sed */
static void copy_space(struct str *to, const struct str *from) {
    to->len = 0;
    str_add(to, from->s ? from->s : "", from->len);
    to->nl = from->nl;
}

static void append_space(struct str *to, const struct str *from) {
    str_addc(to, '\n');
    str_add(to, from->s ? from->s : "", from->len);
    to->nl = from->nl;
}

/* the script on one line after another */
static void run(void) {
    int restart = 0;
    for(;;) {
        int pc = 0, print = 1;
        if(!restart && !next_line(0)) return;
        restart = 0;
        while(pc < g.ncmd) {
            struct cmd *c = &g.prog[pc];
            if(!selected(c)) {
                if(g.fatal) return;
                pc = c->c == '{' ? c->target : pc + 1;
                continue;
            }
            if(g.fatal) return;
            pc++;
            switch(c->c) {
            case '{': break;
            case '=': put_number(g.lineno); break;
            case 'a': enqueue(c->text, c->len, NULL); break;
            case 'i':
                line_start(g.cur);
                put(g.cur, c->text, c->len);
                line_done(g.cur);
                break;
            case 'c':
                if(!c->stop || c->to.type == AD_NONE) {
                    line_start(g.cur);
                    put(g.cur, c->text, c->len);
                    line_done(g.cur);
                }
                print = 0;
                goto cycle_end;
            case 'b': pc = c->target; break;
            case 't':
                if(g.substituted) {
                    g.substituted = 0;
                    pc = c->target;
                }
                break;
            case 'T':
                if(!g.substituted) pc = c->target;
                else g.substituted = 0;
                break;
            case 'd':
                print = 0;
                goto cycle_end;
            case 'D': {
                char *nl = memchr(g.line.s, '\n', g.line.len);
                print = 0;
                if(nl) {
                    size_t k = (size_t)(nl - g.line.s) + 1;
                    memmove(g.line.s, nl + 1, g.line.len - k + 1);
                    g.line.len -= k;
                    restart = 1;
                }
                goto cycle_end;
            }
            case 'g': copy_space(&g.line, &g.hold); break;
            case 'G': append_space(&g.line, &g.hold); break;
            case 'h': copy_space(&g.hold, &g.line); break;
            case 'H': append_space(&g.hold, &g.line); break;
            case 'x': {
                struct str t = g.line;
                g.line = g.hold;
                g.hold = t;
                if(!g.line.s) str_add(&g.line, "", 0);
                break;
            }
            case 'l': list_line(c->number < 0 ? LIST_WIDTH : c->number); break;
            case 'n':
                if(at_last()) goto cycle_end;   /* with no next line it ends as the script's end does: printed once */
                if(!g.quiet) put_line(g.cur, g.line.s, g.line.len, g.line.nl);
                if(!next_line(0)) return;
                break;
            case 'N':
                if(at_last()) goto cycle_end;
                if(!next_line(1)) return;
                break;
            case 'p': put_line(g.cur, g.line.s, g.line.len, g.line.nl); break;
            case 'P': {
                char *nl = memchr(g.line.s, '\n', g.line.len);
                if(nl) put_line(g.cur, g.line.s, (size_t)(nl - g.line.s), 1);
                else put_line(g.cur, g.line.s, g.line.len, g.line.nl);
                break;
            }
            case 'q':
                if(!g.quiet) put_line(g.cur, g.line.s, g.line.len, g.line.nl);
                line_start(g.cur);                      /* q gives a last line its newline, as GNU's sed does */
                line_done(g.cur);
                flush_queue();
                /* fall through */
            case 'Q':
                if(c->number > 0) g.status = (int)(c->number & 0xFF);
                return;
            case 'r': enqueue(NULL, 0, c->text); break;
            case 's': substitute(c); break;
            case 'w': put_line(&c->w->o, g.line.s, g.line.len, g.line.nl); break;
            case 'y': translate(c->y); break;
            }
            if(g.fatal) return;
        }
    cycle_end:
        if(g.fatal) return;
        if(print && !g.quiet) put_line(g.cur, g.line.s, g.line.len, g.line.nl);
        if(g.fatal) return;
    }
}

/**** the end ****/

/* w's files, standard output: keep says that the run went well */
static void finish_outputs(int keep) {
    struct wfile *w;
    for(w = g.wfiles; w; w = w->next) {
        int r;
        if(w->o.n && !w->o.failed) push_out(&w->o);
        if(!w->save) {
            if(w->o.fd > 2) close(w->o.fd);
            continue;
        }
        if(!keep || w->o.failed) {
            su_abort(w->save);
            continue;
        }
        r = su_commit(w->save);
        if(r < 0) {
            su_say("sed", w->save, r);
            g.status = STATUS_TROUBLE;
        }
    }
}

static void free_all(void) {
    struct wfile *w;
    struct label *l;
    struct re *re;
    struct queued *q;
    int i;
    for(i = 0; i < g.ncmd; i++) {
        struct cmd *c = &g.prog[i];
        free(c->label);
        free(c->text);
        if(c->sub) {
            free(c->sub->parts);
            free(c->sub->text);
            free(c->sub);
        }
        if(c->y) {
            free(c->y->pairs);
            free(c->y);
        }
    }
    free(g.prog);
    while((w = g.wfiles)) {
        g.wfiles = w->next;
        free(w->name);
        free(w->o.buf);
        free(w->save);
        free(w);
    }
    while((l = g.labels)) {
        g.labels = l->next;
        free(l->name);
        free(l);
    }
    while((re = g.res)) {
        g.res = re->next;
        regfree(&re->rx);
        free(re);
    }
    while((q = g.qhead)) {
        g.qhead = q->next;
        free(q);
    }
    free(g.sc);
    free(g.files);
    free(g.ibuf);
    free(g.line.s);
    free(g.hold.s);
    free(g.built.s);
    free(g.out.buf);
    free(g.file.buf);
}

static int usage(void) {
    fprintf(stderr, "usage: sed [-nEs] [-i[suffix]] [-e script] [-f file] [script] [file...]\n");
    g.status = STATUS_TROUBLE;
    return -1;
}

static int parse_args(int argc, char **argv) {
    int i, only_files = 0;
    g.files = malloc(((size_t)argc + 1) * sizeof(char *));
    if(!g.files) {
        die_soon("out of memory");
        return -1;
    }
    for(i = 1; i < argc; i++) {
        char *a = argv[i];
        if(only_files || a[0] != '-' || !a[1]) {
            g.files[g.nfiles++] = a;
            continue;
        }
        if(!strcmp(a, "--")) {
            only_files = 1;
            continue;
        }
        for(a++; *a; a++) {
            const char *arg;
            int r;
            switch(*a) {
            case 'n': g.quiet = 1; break;
            case 'E': case 'r': g.extended = 1; break;
            case 's': g.separate = 1; break;
            case 'i':
                g.inplace = g.separate = 1;
                if(a[1]) g.suffix = a + 1;
                a += strlen(a) - 1;
                break;
            case 'e': case 'f':
                arg = a[1] ? a + 1 : i + 1 < argc ? argv[++i] : NULL;
                if(!arg) return usage();
                r = *a == 'e' ? add_script(arg, strlen(arg)) : add_script_file(arg);
                if(r) return -1;
                a += strlen(a) - 1;
                break;
            default:
                return usage();
            }
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    int i, keep;
    memset(&g, 0, sizeof(g));
    g.fd = -1;
    broken_pipe_exit(0);
    if(parse_args(argc, argv) < 0) goto done;
    if(!g.any_script) {
        if(!g.nfiles) {
            usage();
            goto done;
        }
        if(add_script(g.files[0], strlen(g.files[0]))) goto done;
        g.nfiles--;
        for(i = 0; i < g.nfiles; i++) g.files[i] = g.files[i + 1];
    }
    if(g.inplace && !g.nfiles) {
        say("-i needs files");
        goto done;
    }
    if(!g.nfiles) {
        static char dash[] = "-";
        g.files[g.nfiles++] = dash;
    }
    g.out.fd = 1;
    g.tty = isatty(1);
    g.out.buf = malloc(OUTPUT_BUF);
    g.file.buf = malloc(OUTPUT_BUF);
    g.ibuf = malloc(INPUT_BUF);
    if(!g.out.buf || !g.file.buf || !g.ibuf) {
        die_soon("out of memory");
        goto done;
    }
    g.file.fd = -1;
    g.cur = &g.out;
    str_add(&g.line, "", 0);
    str_add(&g.hold, "", 0);
    g.hold.nl = 1;
    str_add(&g.built, "", 0);
    if(g.fatal) goto done;
    if(parse_script() < 0 || resolve_labels() < 0) {
        g.status = STATUS_TROUBLE;
        goto discard;
    }
    reset_ranges();
    run();
    keep = !g.fatal;
    close_input();
    finish_file(keep);
    push_out(&g.out);
    keep = !g.fatal;
    finish_outputs(keep);
    if(!g.out.failed) push_out(&g.out);
    goto done;
discard:
    finish_outputs(0);
done:
    close_input();
    if(g.saving) finish_file(0);
    free_all();
    return g.status;
}
