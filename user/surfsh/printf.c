/*
SurfOS shell: surfsh
--------------------
File: printf.c  Date: 10/25/26
--------------------
printf FORMAT [ARG...], as POSIX has it. The format's characters come out as they are, its \
sequences as what they stand for (\n \t \r \a \b \f \v \\ \" \' and \ddd, the byte of the
octal number ddd, one to three digits), and each % takes the next argument: %s a string, %b a
string with those \ sequences in it (\0ddd there, and \c, which ends all output), %c its first
character, %d and %i a number, %u %o %x %X one without a sign, %% a %. Between the % and the
letter: the flags - + space # 0, a width and a .precision, numbers or * (from an argument). A
number is decimal, 0x... hex, 0... octal, or 'c: the character's code. The format is used
again while arguments are left; one that is missing is "" or 0. An argument that is no number
is said, counts as what of it was one, and the status is 1. 10/25/26: a width or a precision
is at most PRINTF_WIDE.
*/

#include "surfsh.h"

#define PRINTF_WIDE 65536

/* the \ sequence at s (s[0] is the \): what it stands for into out, the characters it takes;
   in %b's argument \0ddd and \c too (*stop: the output ends) */
static int escape(const char *s, struct sbuf *out, int in_b, int *stop) {
    static const char from[] = "\\\"'abfnrtv", to[] = "\\\"'\a\b\f\n\r\t\v";
    const char *e;
    int k = 1, v = 0, n = 0;
    if(!s[1]) {
        sb_putc(out, '\\');
        return 1;
    }
    if(in_b && s[1] == 'c') {
        *stop = 1;
        return 2;
    }
    if((e = strchr(from, s[1]))) {
        sb_putc(out, to[e - from]);
        return 2;
    }
    if(in_b && s[1] == '0') k = 2;              /* \0ddd in %b */
    while(n < 3 && s[k] >= '0' && s[k] <= '7') {
        v = v * 8 + (s[k++] - '0');
        n++;
    }
    if(n || (in_b && k == 2)) {
        sb_putc(out, (char)v);
        return k;
    }
    sb_putc(out, '\\');                         /* a \ before anything else is itself */
    sb_putc(out, s[1]);
    return 2;
}

/* an argument as a number: 'c, or what strtol makes of it; *ok 0 (said) when it is not all a number */
static long number(const char *s, int unsigned_ok, int *ok) {
    char *end;
    long v;
    if(s[0] == '\'' || s[0] == '"') return (unsigned char)s[1];
    if(!*s) return 0;
    v = unsigned_ok && s[0] != '-' ? (long)strtoul(s, &end, 0) : strtol(s, &end, 0);
    if(*end) {
        fprintf(stderr, "surfsh: printf: %s: not a number\n", s);
        *ok = 0;
    }
    return v;
}

/* 10/25/26: a number in the n characters of a conversion (its width, its precision) above PRINTF_WIDE? */
static int too_wide(const char *spec, int n) {
    int i = 0;
    while(i < n) {
        long v = 0;
        if(!isdigit((unsigned char)spec[i])) {
            i++;
            continue;
        }
        for(; i < n && isdigit((unsigned char)spec[i]); i++) if(v <= PRINTF_WIDE) v = v * 10 + (spec[i] - '0');
        if(v > PRINTF_WIDE) return 1;
    }
    return 0;
}

/* the output of printf argv[0] argv[1...] into out: 0, or 1 when an argument was no number, 2 for a
   format that is wrong (said) */
int printf_text(struct sbuf *out, int argc, char **argv) {
    const char *fmt = argv[0];
    int a = 1, ok = 1, stop = 0;
    if(argc < 1) return STATUS_USAGE;
    for(;;) {
        const char *p = fmt;
        int took = 0;
        while(*p && !stop) {
            char spec[32], conv;
            int n = 0;
            if(*p == '\\') {
                p += escape(p, out, 0, &stop);
                continue;
            }
            if(*p != '%') {
                sb_putc(out, *p++);
                continue;
            }
            if(p[1] == '%') {
                sb_putc(out, '%');
                p += 2;
                continue;
            }
            spec[n++] = *p++;                   /* %, the flags, the width, the precision */
            while(*p && strchr("-+ #0", *p) && n < 8) spec[n++] = *p++;
            if(*p == '*') {
                n += snprintf(spec + n, sizeof(spec) - (size_t)n, "%ld", a < argc ? number(argv[a++], 0, &ok) : 0L);
                took = 1;
                p++;
            } else {
                while(isdigit((unsigned char)*p) && n < 16) spec[n++] = *p++;
            }
            if(*p == '.') {
                spec[n++] = *p++;
                if(*p == '*') {
                    n += snprintf(spec + n, sizeof(spec) - (size_t)n, "%ld", a < argc ? number(argv[a++], 0, &ok) : 0L);
                    took = 1;
                    p++;
                } else {
                    while(isdigit((unsigned char)*p) && n < 24) spec[n++] = *p++;
                }
            }
            if(n > 26) {
                fprintf(stderr, "surfsh: printf: a conversion too long in %s\n", fmt);
                return STATUS_USAGE;
            }
            if(too_wide(spec, n)) {
                fprintf(stderr, "surfsh: printf: a width or a precision above %d in %s\n", PRINTF_WIDE, fmt);
                return STATUS_USAGE;
            }
            conv = *p ? *p++ : 0;
            if(conv && strchr("sbcdiuoxX", conv)) took = 1;
            switch(conv) {
            case 's':
            case 'b': {
                const char *arg = a < argc ? argv[a++] : "";
                spec[n++] = 's';
                spec[n] = 0;
                if(conv == 'b') {
                    struct sbuf b = { NULL, 0, 0 };
                    const char *q = arg;
                    while(*q && !stop) {
                        if(*q == '\\') q += escape(q, &b, 1, &stop);
                        else sb_putc(&b, *q++);
                    }
                    sb_printf(out, spec, sb_str(&b));
                    sb_free(&b);
                } else {
                    sb_printf(out, spec, arg);
                }
                break;
            }
            case 'c': {                         /* the first character, as a string of one */
                char one[2] = { 0, 0 };
                if(a < argc) one[0] = argv[a++][0];
                spec[n++] = 's';
                spec[n] = 0;
                sb_printf(out, spec, one);
                break;
            }
            case 'd':
            case 'i':
            case 'u':
            case 'o':
            case 'x':
            case 'X': {
                long v = a < argc ? number(argv[a++], conv != 'd' && conv != 'i', &ok) : 0;
                spec[n++] = 'l';
                spec[n++] = conv;
                spec[n] = 0;
                if(conv == 'd' || conv == 'i') sb_printf(out, spec, v);
                else sb_printf(out, spec, (unsigned long)v);
                break;
            }
            default:
                fprintf(stderr, "surfsh: printf: %%%c: not a conversion (%%s %%b %%c %%d %%i %%u %%o %%x %%X %%%%)\n",
                        conv ? conv : '%');
                return STATUS_USAGE;
            }
        }
        if(stop || a >= argc || !took) break;   /* again while arguments are left */
    }
    return ok ? 0 : STATUS_FAIL;
}

int b_printf(int argc, char **argv) {
    struct sbuf out = { NULL, 0, 0 };
    int status;
    if(argc < 2) {
        fprintf(stderr, "usage: printf format [args]\n");
        return STATUS_USAGE;
    }
    status = printf_text(&out, argc - 1, argv + 1);
    if(out.len) fwrite(out.s, 1, (size_t)out.len, stdout);
    sb_free(&out);
    return status;
}
