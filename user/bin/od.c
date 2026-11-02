/*
SurfOS user program: od
--------------------
File: od.c      Date: 11/01/26
--------------------
od [-A base] [-t type]... [-c] [-x] [-b] [-N bytes] [-j bytes] [-v] [file...]: the bytes of the
files as one stream (standard input without them, - for it), 16 to a line, each line after
its address. -A says how addresses are written: o octal (the default), d decimal, x hex, n
not at all. -t says how the bytes are: a named characters, c characters and escapes, and
d signed, u unsigned, o octal, x hex numbers of 1, 2, 4 or 8 bytes (x2, or C S I L for
those sizes), the default size being 4; a z after a type adds the bytes as text at the end
of the line. Each -t gives a row, lined up under the first; without one, -t o2. -c is -t c,
-x is -t x2, -b is -t o1. Numbers of several bytes are little-endian, as this machine's are.
-j skips that many bytes, -N shows that many at most; both take 0x and 0 for hex and octal,
and b, k, m for 512, 1024 and 1048576 times. A line like the one before it becomes a *, unless -v.

Floating point (-t f) is not here: the digits other systems print for it are their own. A skip past
the end of the input is trouble, status 2. Bytes go through in blocks of 16: any size of file will do.
*/

#include "../diff/cli.h"

#define PER_LINE 16
#define MAX_SPECS 16

struct spec {
    char kind;                          /* a c d o u x */
    int bytes;                          /* of each value */
    int digits;                         /* a value's column width */
    int text;                           /* z */
};

static struct spec specs[MAX_SPECS];
static int nspecs;
static char base = 'o';
static int line_width;                  /* the columns of a row of 16 bytes: the widest type's, a space before each value */

static const char *const ascii_names[33] = {
    "nul", "soh", "stx", "etx", "eot", "enq", "ack", "bel", "bs", "ht", "nl", "vt", "ff", "cr", "so", "si", "dle",
    "dc1", "dc2", "dc3", "dc4", "nak", "syn", "etb", "can", "em", "sub", "esc", "fs", "gs", "rs", "us", "sp"
};

/* the column widths of a number: 1, 2, 4, 8 bytes in octal, signed, unsigned, hex */
static int number_digits(char kind, int bytes) {
    static const unsigned char o[] = { 3, 6, 11, 22 }, d[] = { 4, 6, 11, 20 }, u[] = { 3, 5, 10, 20 }, x[] = { 2, 4, 8, 16 };
    int k = bytes == 1 ? 0 : bytes == 2 ? 1 : bytes == 4 ? 2 : 3;
    return kind == 'o' ? o[k] : kind == 'd' ? d[k] : kind == 'u' ? u[k] : x[k];
}

static int add_spec(char kind, int bytes, int text) {
    struct spec *s;
    if(nspecs == MAX_SPECS) {
        fprintf(stderr, "od: at most %d types\n", MAX_SPECS);
        return -1;
    }
    s = &specs[nspecs++];
    s->kind = kind;
    s->bytes = bytes;
    s->text = text;
    s->digits = kind == 'a' || kind == 'c' ? 3 : number_digits(kind, bytes);
    return 0;
}

/* -t's argument: several types may be written together */
static int parse_types(const char *s) {
    if(!*s) return -1;
    while(*s) {
        char kind = *s++;
        int bytes = 1, text = 0;
        if(kind == 'f') {
            fprintf(stderr, "od: -t f: floating point is not supported\n");
            return -1;
        }
        if(!strchr("acdoux", kind)) return -1;
        if(kind != 'a' && kind != 'c') {
            bytes = 4;
            if(*s == 'C') bytes = 1;
            else if(*s == 'S') bytes = 2;
            else if(*s == 'I' || *s == 'L') bytes = 4;
            else if(*s >= '0' && *s <= '9') {
                bytes = *s - '0';
                if(bytes != 1 && bytes != 2 && bytes != 4 && bytes != 8) return -1;
                if(s[1] >= '0' && s[1] <= '9') return -1;
            } else s--;
            s++;
        }
        if(*s == 'z') {
            text = 1;
            s++;
        }
        if(add_spec(kind, bytes, text) < 0) return -1;
    }
    return 0;
}

static void set_line_width(void) {
    int i;
    for(i = 0; i < nspecs; i++) {
        int w = (specs[i].digits + 1) * (PER_LINE / specs[i].bytes);
        if(w > line_width) line_width = w;
    }
}

static void put_address(unsigned long long at) {
    if(base == 'o') printf("%07llo", at);
    else if(base == 'd') printf("%07llu", at);
    else if(base == 'x') printf("%06llx", at);
}

/* a value of the row in text, right to be aligned by the caller */
static void format_value(const struct spec *s, const unsigned char *p, char *out, size_t size) {
    unsigned long long v = 0;
    int i;
    for(i = 0; i < s->bytes; i++) v |= (unsigned long long)p[i] << (8 * i);
    switch(s->kind) {
    case 'o':
        snprintf(out, size, "%0*llo", s->digits, v);
        break;
    case 'x':
        snprintf(out, size, "%0*llx", s->digits, v);
        break;
    case 'u':
        snprintf(out, size, "%llu", v);
        break;
    case 'd':
        if(s->bytes < 8 && (v >> (8 * s->bytes - 1))) v |= ~0ULL << (8 * s->bytes);
        snprintf(out, size, "%lld", (long long)v);
        break;
    case 'a': {
        unsigned c = (unsigned)v & 127;
        snprintf(out, size, "%s", c <= 32 ? ascii_names[c] : c == 127 ? "del" : "");
        if(c > 32 && c < 127) snprintf(out, size, "%c", c);
        break;
    }
    default: {                          /* c */
        static const char escapes[] = "\0\0\0\0\0\0\0abtnvfr";
        unsigned c = (unsigned)v;
        if(c == 0) snprintf(out, size, "\\0");
        else if(c < 14 && escapes[c]) snprintf(out, size, "\\%c", escapes[c]);
        else if(c >= 32 && c < 127) snprintf(out, size, "%c", c);
        else snprintf(out, size, "%03o", c);
    }
    }
}

/* A row of the n bytes there are of PER_LINE (the buffer holds PER_LINE, zeros after the end). So that rows of different types
   line up, each value ends in the column that is its share of the line: the j-th of v at line_width * j / v, rounded up. That is
   what od elsewhere prints for every mix of types. */
static void put_row(const struct spec *s, const unsigned char *b, int n) {
    int values = PER_LINE / s->bytes, shown = (n + s->bytes - 1) / s->bytes, column = 0, j;
    for(j = 1; j <= shown; j++) {
        char text[32];
        int end = (line_width * j + values - 1) / values;
        format_value(s, b + (j - 1) * s->bytes, text, sizeof(text));
        printf("%*s", end - column, text);
        column = end;
    }
    if(s->text) {
        int i;
        printf("%*s  >", line_width - column, "");     /* the text starts in one place, on the last line too */
        for(i = 0; i < n; i++) putchar(b[i] >= 32 && b[i] < 127 ? b[i] : '.');
        putchar('<');
    }
    putchar('\n');
}

static void put_line(unsigned long long at, const unsigned char *b, int n) {
    int i;
    put_address(at);
    for(i = 0; i < nspecs; i++) {
        if(i && base != 'n') printf("%*s", base == 'x' ? 6 : 7, "");
        put_row(&specs[i], b, n);
    }
}

static char **names;                    /* the files still to read */
static int nnames, stream_rc;
static FILE *cur;
static const char *cur_name;

/* up to n bytes of the files as one stream */
static size_t stream_read(unsigned char *p, size_t n) {
    size_t got = 0;
    while(got < n) {
        size_t k;
        if(!cur) {
            if(!nnames) break;
            cur_name = *names++;
            nnames--;
            if(!(cur = lu_open(cur_name))) {
                stream_rc = 2;
                continue;
            }
        }
        k = fread(p + got, 1, n - got, cur);
        got += k;
        if(got < n) {
            if(ferror(cur)) {
                fprintf(stderr, "od: %s: %s\n", cur_name, strerror(errno));
                stream_rc = 2;
            }
            lu_close(cur);
            cur = NULL;
        }
    }
    return got;
}

/* a byte count: decimal, 0x hex or 0 octal, then b, k or m */
static int parse_bytes(const char *s, unsigned long long *v) {
    unsigned long long n = 0;
    unsigned b = 10, d;
    const char *start;
    if(s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        b = 16;
        s += 2;
    } else if(s[0] == '0' && s[1]) b = 8;
    start = s;
    for(;; s++) {
        if(*s >= '0' && *s <= '9') d = (unsigned)(*s - '0');
        else if(b == 16 && *s >= 'a' && *s <= 'f') d = (unsigned)(*s - 'a') + 10;
        else if(b == 16 && *s >= 'A' && *s <= 'F') d = (unsigned)(*s - 'A') + 10;
        else break;
        if(d >= b || n > (ULLONG_MAX >> 4) - 15) return -1;
        n = n * b + d;
    }
    if(s == start) return -1;
    if(*s == 'b') n <<= 9;
    else if(*s == 'k') n <<= 10;
    else if(*s == 'm') n <<= 20;
    else if(*s) return -1;
    if(*s && s[1]) return -1;
    *v = n;
    return 0;
}

static int usage(void) {
    fprintf(stderr, "usage: od [-A base] [-t type]... [-c] [-x] [-b] [-N bytes] [-j bytes] [-v] [file...]\n");
    return 2;
}

int main(int argc, char **argv) {
    static char stdin_name[] = "-";
    static char *only_stdin[1] = { stdin_name };
    static unsigned char block[PER_LINE], prev[PER_LINE], skip_buf[4096];
    unsigned long long skip = 0, limit = 0, at;
    int limited = 0, every = 0, have_prev = 0, starred = 0, i = 1;
    lu_prog = "od";
    for(; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *o;
        if(!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        for(o = argv[i] + 1; *o; o++) {
            const char *arg;
            if(*o == 'c') {
                if(add_spec('c', 1, 0) < 0) return 2;
                continue;
            }
            if(*o == 'x') {
                if(add_spec('x', 2, 0) < 0) return 2;
                continue;
            }
            if(*o == 'b') {
                if(add_spec('o', 1, 0) < 0) return 2;
                continue;
            }
            if(*o == 'v') {
                every = 1;
                continue;
            }
            if(!strchr("AtNj", *o)) return usage();
            arg = o[1] ? o + 1 : ++i < argc ? argv[i] : NULL;
            if(!arg) return usage();
            if(*o == 'A') {
                if(arg[1] || !strchr("odxn", arg[0])) return usage();
                base = arg[0];
            } else if(*o == 't') {
                if(parse_types(arg) < 0) {
                    fprintf(stderr, "od: %s: not a type\n", arg);
                    return 2;
                }
            } else {
                if(parse_bytes(arg, *o == 'N' ? &limit : &skip) < 0) {
                    fprintf(stderr, "od: %s: not a count of bytes\n", arg);
                    return 2;
                }
                if(*o == 'N') limited = 1;
            }
            break;
        }
    }
    if(!nspecs) add_spec('o', 2, 0);
    set_line_width();
    if(i < argc) {
        names = argv + i;
        nnames = argc - i;
    } else {
        names = only_stdin;
        nnames = 1;
    }
    at = skip;
    while(skip) {
        size_t want = skip > sizeof(skip_buf) ? sizeof(skip_buf) : (size_t)skip, got = stream_read(skip_buf, want);
        if(!got) {
            fprintf(stderr, "od: cannot skip past the end of the input\n");
            return 2;
        }
        skip -= got;
    }
    for(;;) {
        size_t want = limited && limit < PER_LINE ? (size_t)limit : PER_LINE, n;
        if(!want) break;
        n = stream_read(block, want);
        if(!n) break;
        if(n < PER_LINE) memset(block + n, 0, PER_LINE - n);
        if(!every && n == PER_LINE && have_prev && !memcmp(block, prev, PER_LINE)) {
            if(!starred) puts("*");
            starred = 1;
        } else {
            put_line(at, block, (int)n);
            starred = 0;
        }
        memcpy(prev, block, PER_LINE);
        have_prev = 1;
        at += n;
        if(limited) limit -= n;
        if(n < want) break;
    }
    if(base != 'n') {
        put_address(at);
        putchar('\n');
    }
    if(cur) lu_close(cur);
    return cli_flush("od") ? 2 : stream_rc;
}
