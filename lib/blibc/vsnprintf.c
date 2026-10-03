/*
SurfOS blibc - vsnprintf(), the one formatter behind printf(), kprintf() and snprintf()
(C)2004 Brandon Burr, written 10/2026 (roadmap K5)

Supports flags - 0 + space #, width and precision (numbers or *), lengths hh h l ll z,
conversions d i u x X o c s p %. The 2004 printf walked the stack from &format and knew
%d %u %x %s; this one uses <stdarg.h> and prints "%02x" the way floppy.c always expected.
*/

#include <surfos/types.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define FL_LEFT  0x01
#define FL_ZERO  0x02
#define FL_PLUS  0x04
#define FL_SPACE 0x08
#define FL_ALT   0x10

struct outbuf {
    char *p;
    size_t left;  /* room left including the terminator */
    size_t total; /* characters that would have been written */
};

static void out(struct outbuf *b, char c) {
    if(b->left > 1) {
        *b->p++ = c;
        b->left--;
    }
    b->total++;
}

static void pad(struct outbuf *b, int n, char c) {
    while(n-- > 0) out(b, c);
}

/* edx:eax / d with the quotient known to fit 32 bits (hi < d). No libgcc needed. */
static inline u32 div64_32(u32 hi, u32 lo, u32 d, u32 *rem) {
    u32 q, r;
    asm("divl %4" : "=a"(q), "=d"(r) : "a"(lo), "d"(hi), "r"(d));
    *rem = r;
    return q;
}

/* 64-bit value divided by a small base, as two 32-bit steps */
static u64 udiv64_small(u64 n, u32 base, u32 *rem) {
    u32 hi = (u32)(n >> 32), lo = (u32)n, r;
    u32 qhi = hi / base;
    u32 qlo;
    r = hi % base;
    qlo = div64_32(r, lo, base, &r);
    *rem = r;
    return ((u64)qhi << 32) | qlo;
}

static void fmt_num(struct outbuf *b, u64 val, int neg, unsigned base, int upper, int width, int prec, int flags) {
    char tmp[24];
    int len = 0, zeros, total, pfxlen = 0;
    const char *digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    const char *pfx = "";
    char sign = 0;

    if(!(val == 0 && prec == 0)) {
        do {
            u32 r;
            val = udiv64_small(val, base, &r);
            tmp[len++] = digs[r];
        } while(val);
    }

    if(neg) sign = '-';
    else if(flags & FL_PLUS) sign = '+';
    else if(flags & FL_SPACE) sign = ' ';

    if((flags & FL_ALT) && base == 16 && len) { pfx = upper ? "0X" : "0x"; pfxlen = 2; }
    if((flags & FL_ALT) && base == 8 && (len == 0 || tmp[len-1] != '0')) { pfx = "0"; pfxlen = 1; }

    zeros = (prec > len) ? prec - len : 0;
    if(prec < 0 && (flags & FL_ZERO) && !(flags & FL_LEFT)) {
        int body = len + pfxlen + (sign ? 1 : 0);
        if(width > body) zeros = width - body;
    }
    total = len + zeros + pfxlen + (sign ? 1 : 0);

    if(!(flags & FL_LEFT)) pad(b, width - total, ' ');
    if(sign) out(b, sign);
    while(pfxlen--) out(b, *pfx++);
    pad(b, zeros, '0');
    while(len) out(b, tmp[--len]);
    if(flags & FL_LEFT) pad(b, width - total, ' ');
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    struct outbuf b;
    b.p = buf;
    b.left = size;
    b.total = 0;

    while(*fmt) {
        int flags = 0, width = 0, prec = -1, length = 0; /* length: 0 int, -2 hh, -1 h, 1 l, 2 ll */
        char c;

        if(*fmt != '%') {
            out(&b, *fmt++);
            continue;
        }
        fmt++;

        for(;;) {
            if(*fmt == '-') flags |= FL_LEFT;
            else if(*fmt == '0') flags |= FL_ZERO;
            else if(*fmt == '+') flags |= FL_PLUS;
            else if(*fmt == ' ') flags |= FL_SPACE;
            else if(*fmt == '#') flags |= FL_ALT;
            else break;
            fmt++;
        }
        if(*fmt == '*') {
            width = va_arg(ap, int);
            if(width < 0) { flags |= FL_LEFT; width = -width; }
            fmt++;
        } else {
            while(*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        }
        if(*fmt == '.') {
            fmt++;
            prec = 0;
            if(*fmt == '*') {
                prec = va_arg(ap, int);
                if(prec < 0) prec = -1;
                fmt++;
            } else {
                while(*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
            }
        }
        if(*fmt == 'h') { length = -1; fmt++; if(*fmt == 'h') { length = -2; fmt++; } }
        else if(*fmt == 'l') { length = 1; fmt++; if(*fmt == 'l') { length = 2; fmt++; } }
        else if(*fmt == 'z') { length = 1; fmt++; }

        c = *fmt++;
        switch(c) {
        case 'd':
        case 'i': {
            long long v;
            u64 mag;
            if(length == 2) v = va_arg(ap, long long);
            else if(length == 1) v = va_arg(ap, long);
            else if(length == -1) v = (short)va_arg(ap, int);
            else if(length == -2) v = (signed char)va_arg(ap, int);
            else v = va_arg(ap, int);
            mag = v < 0 ? (u64)0 - (u64)v : (u64)v;
            fmt_num(&b, mag, v < 0, 10, 0, width, prec, flags);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            u64 v;
            if(length == 2) v = va_arg(ap, unsigned long long);
            else if(length == 1) v = va_arg(ap, unsigned long);
            else if(length == -1) v = (unsigned short)va_arg(ap, unsigned int);
            else if(length == -2) v = (unsigned char)va_arg(ap, unsigned int);
            else v = va_arg(ap, unsigned int);
            fmt_num(&b, v, 0, c == 'u' ? 10 : (c == 'o' ? 8 : 16), c == 'X', width, prec, flags);
            break;
        }
        case 'p':
            fmt_num(&b, (u64)(unsigned long)va_arg(ap, void *), 0, 16, 0, width, -1, flags | FL_ALT);
            break;
        case 'c': {
            char ch = (char)va_arg(ap, int);
            if(!(flags & FL_LEFT)) pad(&b, width - 1, ' ');
            out(&b, ch);
            if(flags & FL_LEFT) pad(&b, width - 1, ' ');
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            size_t len;
            if(!s) s = "(null)";
            len = prec >= 0 ? strnlen(s, prec) : strlen(s);
            if(!(flags & FL_LEFT)) pad(&b, width - (int)len, ' ');
            {
                size_t n = len;
                while(n) { out(&b, *s++); n--; }
            }
            if(flags & FL_LEFT) pad(&b, width - (int)len, ' ');
            break;
        }
        case '%':
            out(&b, '%');
            break;
        case 0:
            fmt--; /* "%" at the very end: print it and stop */
            out(&b, '%');
            break;
        default:
            out(&b, '%');
            out(&b, c);
            break;
        }
    }

    if(size) *b.p = 0;
    return (int)b.total;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, (size_t)-1, fmt, ap);
    va_end(ap);
    return n;
}
