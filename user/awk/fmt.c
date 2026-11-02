/*
SurfOS user program: awk
--------------------
File: fmt.c     Date: 11/01/26
--------------------
printf and sprintf, and through them CONVFMT and OFMT. The format comes from the program, so it
never reaches the C library as it stands: each conversion is taken apart here and the C library
gets a new one made of the flags, numbers and one letter that were found, so that %n and a %s
without its argument do not exist. What is no conversion is printed as it stands.

%c and %s count characters, whole UTF-8 sequences, in their width and precision. %c takes the
character with that code when its argument is a number, as UTF-8 above 127, or the first character
of a string. Whole numbers beyond 64 bits, where %d has no answer, are printed as %.0f prints them.
*/

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "awk.h"

void buf_add(struct buf *b, const char *s, unsigned n) {
    if((uint64_t)b->len + n + 1 > b->cap) {
        uint64_t cap = b->cap ? b->cap : 64;
        while(cap < (uint64_t)b->len + n + 1) cap *= 2;
        if(cap >= 0x7ff00000u) fatal("out of memory");
        b->p = xrealloc(b->p, (size_t)cap);
        b->cap = (unsigned)cap;
    }
    if(n) memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

void buf_ch(struct buf *b, int c) {
    char t = (char)c;
    buf_add(b, &t, 1);
}

static void pad(struct buf *b, unsigned n) {
    static const char blanks[] = "                ";
    while(n) {
        unsigned k = n < 16 ? n : 16;
        buf_add(b, blanks, k);
        n -= k;
    }
}

/* text of chars characters, in a field of width */
static void put_field(struct buf *b, const char *s, unsigned n, unsigned chars, long width, int left) {
    unsigned fill = width > 0 && (unsigned long)width > chars ? (unsigned)((unsigned long)width - chars) : 0;
    if(!left) pad(b, fill);
    buf_add(b, s, n);
    if(left) pad(b, fill);
}

static unsigned encode(unsigned c, char *t) {
    if(c < 0x800) {
        t[0] = (char)(0xc0 | c >> 6);
        t[1] = (char)(0x80 | (c & 63));
        return 2;
    }
    if(c < 0x10000) {
        t[0] = (char)(0xe0 | c >> 12);
        t[1] = (char)(0x80 | (c >> 6 & 63));
        t[2] = (char)(0x80 | (c & 63));
        return 3;
    }
    t[0] = (char)(0xf0 | c >> 18);
    t[1] = (char)(0x80 | (c >> 12 & 63));
    t[2] = (char)(0x80 | (c >> 6 & 63));
    t[3] = (char)(0x80 | (c & 63));
    return 4;
}

static void put_char(struct buf *b, const struct value *v, long width, int left) {
    char t[4];
    if(v->kind == V_NUM || (v->kind == V_INPUT && looks_numeric(v->u.s->s, v->u.s->len))) {
        double d = to_num(v), low = fmod(d, 256);
        unsigned n = 1, c;
        c = d >= 0 && d < 0x110000 ? (unsigned)d : isnan(low) ? 0 : (unsigned)((long long)low & 255);
        if(c < 128 || (c >= 0xd800 && c < 0xe000)) t[0] = (char)c;
        else n = encode(c, t);
        put_field(b, t, n, 1, width, left);
    } else {
        struct str *s = to_str(v);
        if(s->len) put_field(b, s->s, char_len(s->s, s->len), 1, width, left);
        else put_field(b, "", 0, 0, width, left);
        str_put(s);
    }
}

static void put_string(struct buf *b, const struct value *v, long width, long prec, int left) {
    struct str *s = to_str(v);
    unsigned n = s->len, chars;
    if(prec < 0) {
        chars = str_chars(s);
    } else {
        unsigned i = 0;
        chars = 0;
        while(i < n && (unsigned long)chars < (unsigned long)prec) {
            i += char_len(s->s + i, n - i);
            chars++;
        }
        n = i;
    }
    put_field(b, s->s, n, chars, width, left);
    str_put(s);
}

/* the conversion the C library is given: flags, width, precision, the length and the letter */
static void c_format(struct buf *b, const char *flags, long width, long prec, const char *length, char conv, int is_ll,
                     long long ll, double d) {
    char spec[32];
    size_t n = 0, cap;
    char *t;
    int len;
    spec[n++] = '%';
    while(*flags) spec[n++] = *flags++;
    spec[n++] = '*';
    if(prec >= 0) {
        spec[n++] = '.';
        spec[n++] = '*';
    }
    while(*length) spec[n++] = *length++;
    spec[n++] = conv;
    spec[n] = 0;
    /* the longest a double takes, 310 digits before the point, and what was asked for */
    cap = (size_t)width + (prec > 0 ? (size_t)prec : 0) + 400;
    if(cap >= 0x7ff00000u) fatal("out of memory");
    t = xalloc(cap);
    if(is_ll) len = prec >= 0 ? snprintf(t, cap, spec, (int)width, (int)prec, ll) : snprintf(t, cap, spec, (int)width, ll);
    else len = prec >= 0 ? snprintf(t, cap, spec, (int)width, (int)prec, d) : snprintf(t, cap, spec, (int)width, d);
    if(len < 0) fatal("a conversion of printf failed");
    buf_add(b, t, (unsigned)len);
    free(t);
}

static void put_number(struct buf *b, const char *flags, long width, long prec, char conv, double d) {
    char plain[8];
    unsigned n = 0, i;
    for(i = 0; flags[i]; i++) if(flags[i] != '0') plain[n++] = flags[i];
    plain[n] = 0;
    if(isnan(d)) d = fabs(d);           /* the sign of a NaN is the machine's business: x87's are negative */
    switch(conv) {
    case 'd': case 'i':
        if(isnan(d) || isinf(d)) c_format(b, plain, width, -1, "", 'f', 0, 0, d);
        else if(fabs(d) < 9.2e18) c_format(b, flags, width, prec, "ll", 'd', 1, (long long)d, 0);
        else c_format(b, flags, width, 0, "", 'f', 0, 0, d);
        return;
    case 'o': case 'x': case 'X': case 'u':
        if(isnan(d) || isinf(d)) c_format(b, plain, width, -1, "", 'f', 0, 0, d);
        else if(d < 0 && d > -9.2e18) c_format(b, flags, width, prec, "ll", conv, 1, (long long)d, 0);
        else if(d >= 0 && d < 9.2e18) c_format(b, flags, width, prec, "ll", conv, 1, (long long)d, 0);
        else c_format(b, flags, width, 0, "", 'f', 0, 0, d);
        return;
    default:
        c_format(b, flags, width, prec, "", conv, 0, 0, d);
        return;
    }
}

/* a * in the format: a number of any size, which format_text() then refuses if it is too large */
static long count_of(const struct value *v) {
    double d = to_num(v);
    if(isnan(d)) return 0;
    return d > 1e9 ? 1000000000L : d < -1e9 ? -1000000000L : (long)d;
}

/* a number in the format */
static long read_count(const char *fmt, unsigned flen, unsigned *i) {
    long v = 0;
    while(*i < flen && fmt[*i] >= '0' && fmt[*i] <= '9') {
        if(v < 100000000) v = v * 10 + (fmt[*i] - '0');
        (*i)++;
    }
    return v;
}

void format_text(struct buf *b, const char *fmt, unsigned flen, const struct value *args, int nargs) {
    unsigned i = 0;
    int next = 0;
    if(!b->p) buf_add(b, "", 0);
    while(i < flen) {
        unsigned start = i, nf = 0;
        char flags[8];
        long width = 0, prec = -1;
        int left = 0;
        char c;
        const struct value *arg;
        if(fmt[i] != '%') {
            const char *p = memchr(fmt + i, '%', flen - i);
            unsigned n = p ? (unsigned)(p - (fmt + i)) : flen - i;
            buf_add(b, fmt + i, n);
            i += n;
            continue;
        }
        i++;
        if(i < flen && fmt[i] == '%') {
            buf_ch(b, '%');
            i++;
            continue;
        }
        for(; i < flen && strchr("-+ #0", fmt[i]) && fmt[i]; i++) {
            if(fmt[i] == '-') left = 1;
            if(!memchr(flags, fmt[i], nf) && nf < sizeof(flags) - 1) flags[nf++] = fmt[i];
        }
        flags[nf] = 0;
        if(i < flen && fmt[i] == '*') {
            i++;
            if(next >= nargs) fatal("not enough arguments for printf's format");
            width = count_of(&args[next++]);
            if(width < 0) {
                width = -width;
                left = 1;
                if(!memchr(flags, '-', nf) && nf < sizeof(flags) - 1) {
                    flags[nf++] = '-';
                    flags[nf] = 0;
                }
            }
        } else {
            width = read_count(fmt, flen, &i);
        }
        if(i < flen && fmt[i] == '.') {
            i++;
            if(i < flen && fmt[i] == '*') {
                i++;
                if(next >= nargs) fatal("not enough arguments for printf's format");
                prec = count_of(&args[next++]);
            } else {
                prec = read_count(fmt, flen, &i);
            }
        }
        while(i < flen && strchr("hlLqjzt", fmt[i]) && fmt[i]) i++;     /* C's lengths mean nothing here */
        if(i == flen) {
            buf_add(b, fmt + start, flen - start);
            break;
        }
        c = fmt[i++];
        if(!strchr("cdiouxXeEfFgGs", c) || !c) {
            buf_add(b, fmt + start, i - start);
            continue;
        }
        if(next >= nargs) fatal("not enough arguments for printf's format");
        arg = &args[next++];
        if(width > 100000000) fatal("a width in printf's format is too large");
        if(prec > 100000000) fatal("a precision in printf's format is too large");
        if(c == 'c') put_char(b, arg, width, left);
        else if(c == 's') put_string(b, arg, width, prec, left);
        else put_number(b, flags, width, prec, c, to_num(arg));
    }
}
