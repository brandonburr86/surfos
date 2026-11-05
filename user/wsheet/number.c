/*
SurfOS user program: wsheet
--------------------
File: number.c  Date: 11/05/26
--------------------
Numbers as the Spreadsheet reads them and shows them. A value is a double (IEEE 754 binary64);
what a person sees of it is its 15 significant digits, C's DBL_DIG, the most that every double
keeps: any decimal of 15 digits read into a double and printed again to 15 digits comes back the
same. So 0.1 + 0.2 shows as 0.3, and two numbers equal to 15 digits are equal in a comparison
(num_equal()). The digits come from printf's %.14e, which libsurf's printf (user/libsurf/printf.c)
and the host's both give exactly, rounded to nearest; what is cut after that, by a format's
decimals or by ROUND(), is cut a half away from zero, as people round: 2.675 shows as 2.68 with
two decimals although the double is 2.67499999999999982236431605997495353221893310546875.

What is kept, in a file or the formula bar, is the shortest text that strtod() reads back as the
same double (15 to 17 digits), laid out as ECMA-262's Number::toString() lays its digits out
(plain from 1e-7 to 1e21, an exponent outside), with a capital E.

General in a cell of a given width shows as many digits as fit, then fewer decimals, then an
exponent (1.23E+15), and ### when not even that fits; fixed decimals and percents show ### when
they do not fit (the window's part). Nothing here calls anything but snprintf() and strtod().
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "sheetpriv.h"

#define TOKEN_MAX 400                   /* a typed number longer than this is text */

/* a bounded string: what does not fit in size is counted but not written */
struct sb {
    char *out;
    int size, len;
};

static void sb_char(struct sb *b, char c) {
    if(b->len + 1 < b->size) b->out[b->len] = c;
    b->len++;
}

static void sb_text(struct sb *b, const char *s) {
    while(*s) sb_char(b, *s++);
}

static int sb_end(struct sb *b) {
    if(b->size > 0) b->out[b->len < b->size ? b->len : b->size - 1] = 0;
    return b->len;
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }

/* x's first 15 significant digits, rounded to nearest by printf: x = d.dddddddddddddd * 10^*e (0: 15 zeros) */
int num_digits15(double x, char *dig, int *e) {
    char buf[48];
    const char *p;
    int ex = 0, neg = 0;
    if(x < 0) x = -x;
    if(x == 0 || !isfinite(x)) {
        memset(dig, '0', 15);
        *e = 0;
        return 15;
    }
    snprintf(buf, sizeof(buf), "%.14e", x);
    dig[0] = buf[0];
    memcpy(dig + 1, buf + 2, 14);
    p = strchr(buf, 'e');
    if(p) {
        p++;
        if(*p == '-' || *p == '+') neg = *p++ == '-';
        while(is_digit(*p)) ex = ex * 10 + (*p++ - '0');
    }
    *e = neg ? -ex : ex;
    return 15;
}

/* dig[0..15) rounded to keep digits, a half away from zero: how many are left (keep, or 1 after 9s carried into a
   new first digit, when *e grows by one; 0 when the value rounds to nothing) */
static int round_digits(char *dig, int keep, int *e) {
    int i;
    if(keep >= 15) return 15;
    if(keep < 0) return 0;
    if(dig[keep] < '5') return keep;
    for(i = keep - 1; i >= 0; i--) {
        if(dig[i] != '9') {
            dig[i]++;
            return keep;
        }
        dig[i] = '0';
    }
    dig[0] = '1';                       /* 9s carried over, or the first digit itself rounded up */
    for(i = 1; i < 15; i++) dig[i] = '0';
    ++*e;
    return keep ? keep : 1;
}

/* how many of dig[0..n) are left without the 0s at the end */
static int strip(const char *dig, int n) {
    while(n > 0 && dig[n - 1] == '0') n--;
    return n;
}

static int exp_digits(int e) {
    if(e < 0) e = -e;
    return e >= 100 ? 3 : 2;
}

/* the length of d.ddd (k digits) * 10^e written plain */
static int plain_len(int neg, int k, int e) {
    int whole = e >= 0 ? e + 1 : 1, decimals = e >= 0 ? (k > e + 1 ? k - e - 1 : 0) : -e - 1 + k;
    return neg + whole + (decimals ? 1 + decimals : 0);
}

static int sci_len(int neg, int k, int e) {
    return neg + 1 + (k > 1 ? k : 0) + 2 + exp_digits(e);
}

/* d.ddd (k digits, k >= 1) * 10^e written plain: 1234.5, 0.00012 */
static void put_plain(struct sb *b, int neg, const char *dig, int k, int e) {
    int i;
    if(neg) sb_char(b, '-');
    if(e >= 0) {
        for(i = 0; i <= e; i++) sb_char(b, i < k ? dig[i] : '0');
        if(k > e + 1) {
            sb_char(b, '.');
            for(i = e + 1; i < k; i++) sb_char(b, dig[i]);
        }
    } else {
        sb_text(b, "0.");
        for(i = 0; i < -e - 1; i++) sb_char(b, '0');
        for(i = 0; i < k; i++) sb_char(b, dig[i]);
    }
}

/* the same with an exponent, at least two digits of it: 1.5E+20, 1E-05 */
static void put_sci(struct sb *b, int neg, const char *dig, int k, int e) {
    char ex[16];
    int i;
    if(neg) sb_char(b, '-');
    sb_char(b, dig[0]);
    if(k > 1) {
        sb_char(b, '.');
        for(i = 1; i < k; i++) sb_char(b, dig[i]);
    }
    snprintf(ex, sizeof(ex), "E%c%02d", e < 0 ? '-' : '+', e < 0 ? -e : e);
    sb_text(b, ex);
}

/* the fewest digits that strtod() reads back as x (finite, not 0), the 0s at the end dropped: their count, with
   x = d.ddd * 10^*e */
static int shortest_digits(double x, char *dig, int *e) {
    char buf[48];
    const char *p;
    int prec, k = 0, ex = 0, neg = 0;
    if(x < 0) x = -x;
    for(prec = 15; prec < 17; prec++) {
        snprintf(buf, sizeof(buf), "%.*e", prec - 1, x);
        if(strtod(buf, NULL) == x) break;
    }
    if(prec == 17) snprintf(buf, sizeof(buf), "%.16e", x);     /* 17 digits always read back */
    for(p = buf; *p && *p != 'e'; p++) if(is_digit(*p)) dig[k++] = *p;
    if(*p == 'e') {
        p++;
        if(*p == '-' || *p == '+') neg = *p++ == '-';
        while(is_digit(*p)) ex = ex * 10 + (*p++ - '0');
    }
    *e = neg ? -ex : ex;
    return strip(dig, k);
}

/* ECMA-262's layout of k digits whose point is n places in (x = 0.d1d2... * 10^n), with E for e */
static void put_ecma(struct sb *b, int neg, const char *dig, int k, int n) {
    char ex[16];
    int i;
    if(neg) sb_char(b, '-');
    if(k <= n && n <= 21) {
        for(i = 0; i < k; i++) sb_char(b, dig[i]);
        for(; i < n; i++) sb_char(b, '0');
    } else if(0 < n && n <= 21) {
        for(i = 0; i < n; i++) sb_char(b, dig[i]);
        sb_char(b, '.');
        for(; i < k; i++) sb_char(b, dig[i]);
    } else if(-6 < n && n <= 0) {
        sb_text(b, "0.");
        for(i = 0; i < -n; i++) sb_char(b, '0');
        for(i = 0; i < k; i++) sb_char(b, dig[i]);
    } else {
        sb_char(b, dig[0]);
        if(k > 1) {
            sb_char(b, '.');
            for(i = 1; i < k; i++) sb_char(b, dig[i]);
        }
        snprintf(ex, sizeof(ex), "E%c%d", n - 1 < 0 ? '-' : '+', n - 1 < 0 ? 1 - n : n - 1);
        sb_text(b, ex);
    }
}

int num_shortest(double x, char *out, int size) {
    struct sb b = { out, size, 0 };
    char dig[24];
    int k, e;
    if(!isfinite(x)) sb_text(&b, "#NUM!");
    else if(x == 0) sb_char(&b, '0');                   /* -0 too: a sheet never shows it */
    else {
        k = shortest_digits(x, dig, &e);
        put_ecma(&b, x < 0, dig, k, e + 1);
    }
    return sb_end(&b);
}

/* x * 100 with its %: the same digits two places on, so that the text read back with its % is x again */
int num_shortest_percent(double x, char *out, int size) {
    struct sb b = { out, size, 0 };
    char dig[24];
    int k, e;
    if(!isfinite(x)) sb_text(&b, "#NUM!");
    else {
        if(x == 0) sb_char(&b, '0');
        else {
            k = shortest_digits(x, dig, &e);
            put_ecma(&b, x < 0, dig, k, e + 3);
        }
        sb_char(&b, '%');
    }
    return sb_end(&b);
}

/* General: 15 digits at most; with chars > 0 as many as fit in chars characters */
int num_general(double x, int chars, char *out, int size) {
    struct sb b = { out, size, 0 };
    char dig[16], d2[16];
    int neg = x < 0, e, e2, k, k2, keep, m, i;
    if(!isfinite(x)) {
        sb_text(&b, "#NUM!");
        return sb_end(&b);
    }
    if(x == 0) {
        sb_char(&b, '0');
        return sb_end(&b);
    }
    num_digits15(x, dig, &e);
    k = strip(dig, 15);
    if(chars <= 0) {
        if(e >= -9 && e < 15) put_plain(&b, neg, dig, k, e);
        else put_sci(&b, neg, dig, k, e);
        return sb_end(&b);
    }
    if(e >= -9 && e < 15) {
        if(plain_len(neg, k, e) <= chars) {
            put_plain(&b, neg, dig, k, e);
            return sb_end(&b);
        }
        memcpy(d2, dig, 15);
        e2 = e;
        if(e >= 0 && chars - neg - (e + 1) >= 0) {     /* the whole part fits: fewer decimals */
            int decimals = chars - neg - (e + 1) - 1;
            keep = round_digits(d2, e + 1 + (decimals > 0 ? decimals : 0), &e2);
            k2 = strip(d2, keep);
            if(k2 && plain_len(neg, k2, e2) <= chars) {
                put_plain(&b, neg, d2, k2, e2);
                return sb_end(&b);
            }
        } else if(e < 0 && chars - neg - 2 - (-e - 1) >= 1) {      /* 0.000ddd with fewer digits */
            keep = round_digits(d2, chars - neg - 2 - (-e - 1), &e2);
            k2 = strip(d2, keep);
            if(k2 && plain_len(neg, k2, e2) <= chars) {
                put_plain(&b, neg, d2, k2, e2);
                return sb_end(&b);
            }
        }
    }
    for(m = k; m >= 1; m--) {                           /* an exponent, with as many digits as fit */
        memcpy(d2, dig, 15);
        e2 = e;
        keep = round_digits(d2, m, &e2);
        k2 = strip(d2, keep);
        if(k2 && sci_len(neg, k2, e2) <= chars) {
            put_sci(&b, neg, d2, k2, e2);
            return sb_end(&b);
        }
    }
    if(e < 0) {                                         /* below 1 in a cell too narrow for that: a whole number */
        memcpy(d2, dig, 15);
        e2 = e;
        keep = round_digits(d2, e + 1, &e2);
        if(!keep) sb_char(&b, '0');
        else if(neg + 1 <= chars) sb_text(&b, neg ? "-1" : "1");
        if(b.len) return sb_end(&b);
    }
    for(i = 0; i < chars; i++) sb_char(&b, '#');
    return sb_end(&b);
}

/* a fixed number of decimals, or a percent with them: 1,234.5 is 1234.50 (no separators); -0.001 is 0.00 */
int num_fixed(double x, int decimals, int percent, char *out, int size) {
    struct sb b = { out, size, 0 };
    char dig[16];
    int e = 0, n = 0, i, j;
    if(!isfinite(x)) {
        sb_text(&b, "#NUM!");
        return sb_end(&b);
    }
    if(decimals < 0) decimals = 0;
    if(decimals > SH_DECIMALS_MAX) decimals = SH_DECIMALS_MAX;
    if(x != 0) {
        num_digits15(x, dig, &e);
        if(percent) e += 2;                             /* in decimal, so 0.07 is 7% exactly */
        n = round_digits(dig, e + 1 + decimals, &e);
    }
    if(n && x < 0) sb_char(&b, '-');                    /* a value that rounds to 0 shows no sign */
    if(n && e >= 0) for(i = 0; i <= e; i++) sb_char(&b, i < n ? dig[i] : '0');
    else sb_char(&b, '0');
    if(decimals) {
        sb_char(&b, '.');
        for(j = 0; j < decimals; j++) {
            i = e + 1 + j;
            sb_char(&b, n && i >= 0 && i < n ? dig[i] : '0');
        }
    }
    if(percent) sb_char(&b, '%');
    return sb_end(&b);
}

/* ROUND(x, digits): x's 15 digits rounded at that decimal place, a half away from zero; digits < 0 rounds to tens,
   hundreds and so on. A place past x's 15 digits leaves x as it is */
double num_round(double x, int digits) {
    struct sb b;
    char dig[16], buf[48];
    int e, n;
    if(x == 0 || !isfinite(x)) return x;
    if(digits > 400) digits = 400;
    if(digits < -400) digits = -400;
    num_digits15(x, dig, &e);
    if(e + 1 + digits > 15) return x;
    n = round_digits(dig, e + 1 + digits, &e);
    if(!n) return 0;
    b.out = buf;
    b.size = sizeof(buf);
    b.len = 0;
    put_sci(&b, x < 0, dig, n, e);
    sb_end(&b);
    return strtod(buf, NULL);
}

/* equal as people see them: the same to 15 significant digits */
int num_equal(double a, double b) {
    char da[16], db[16];
    int ea, eb;
    if(a == b) return 1;
    if(a == 0 || b == 0 || (a < 0) != (b < 0) || !isfinite(a) || !isfinite(b)) return 0;
    num_digits15(a, da, &ea);
    num_digits15(b, db, &eb);
    return ea == eb && !memcmp(da, db, 15);
}

/* the digits of a number up to an exponent and a %, into buf for strtod(): the count of digits read, -1 when the
   text is not one. Signs, a point with digits on either side, e or E and digits */
static int scan(const char *s, int len, int *i, char *buf, int *n, int *decimals) {
    int digits = 0;
    if(*i < len && (s[*i] == '+' || s[*i] == '-')) buf[(*n)++] = s[(*i)++];
    while(*i < len && is_digit(s[*i])) {
        buf[(*n)++] = s[(*i)++];
        digits++;
    }
    if(*i < len && s[*i] == '.') {
        buf[(*n)++] = s[(*i)++];
        while(*i < len && is_digit(s[*i])) {
            buf[(*n)++] = s[(*i)++];
            digits++;
            ++*decimals;
        }
    }
    return digits ? digits : -1;
}

/* an exponent at s[*i]: 1 with it in *x (cut at 100000, past any double's), 0 when there is none */
static int scan_exp(const char *s, int len, int *i, long *x) {
    int j = *i + 1, neg = 0, digits = 0;
    long v = 0;
    if(*i >= len || (s[*i] != 'e' && s[*i] != 'E')) return 0;
    if(j < len && (s[j] == '+' || s[j] == '-')) neg = s[j++] == '-';
    while(j < len && is_digit(s[j])) {
        if(v < 100000) v = v * 10 + (s[j] - '0');
        j++;
        digits++;
    }
    if(!digits) return -1;
    *x = neg ? -v : v;
    *i = j;
    return 1;
}

/* a number as typed: 12, -3.5, .5, 5., 1e3, 2.5E-3, 50%, 12.5% (whole, no spaces). 1 with *v; 0 when it is not
   one (also too large for a double). *pct: 0, or with a % 1 + the decimals typed (for the cell's percent format) */
int num_read(const char *s, int len, double *v, int *pct) {
    char buf[TOKEN_MAX + 32];
    int i = 0, n = 0, decimals = 0, percent = 0, r;
    long ex = 0;
    if(len <= 0 || len > TOKEN_MAX) return 0;
    if(scan(s, len, &i, buf, &n, &decimals) < 0) return 0;
    r = scan_exp(s, len, &i, &ex);
    if(r < 0) return 0;
    if(i < len && s[i] == '%') {
        percent = 1;
        i++;
    }
    if(i != len) return 0;
    if(percent) ex -= 2;                                /* in decimal: 7% is the double nearest 0.07 */
    snprintf(buf + n, sizeof(buf) - (size_t)n, "e%ld", ex);
    *v = strtod(buf, NULL);
    if(!isfinite(*v)) return 0;
    if(*v == 0) *v = 0;                                 /* no -0 */
    *pct = percent ? 1 + (decimals > SH_DECIMALS_MAX ? SH_DECIMALS_MAX : decimals) : 0;
    return 1;
}

/* a number as RFC 8259 (JSON) writes one: -?(0|[1-9][0-9]*)(.[0-9]+)?([eE][+-]?[0-9]+)?, which CSV's fields that
   are numbers are; 1 with *v, 0 when it is not one, so that 007, .5, +1 and 1,000 stay text */
int num_read_json(const char *s, int len, double *v) {
    char buf[TOKEN_MAX + 1];
    int i = 0;
    long ex;
    if(len <= 0 || len > TOKEN_MAX) return 0;
    if(s[i] == '-') i++;
    if(i >= len || !is_digit(s[i])) return 0;
    if(s[i] == '0') i++;
    else while(i < len && is_digit(s[i])) i++;
    if(i < len && s[i] == '.') {
        i++;
        if(i >= len || !is_digit(s[i])) return 0;
        while(i < len && is_digit(s[i])) i++;
    }
    if(scan_exp(s, len, &i, &ex) < 0) return 0;
    if(i != len) return 0;
    memcpy(buf, s, (size_t)len);
    buf[len] = 0;
    *v = strtod(buf, NULL);
    if(!isfinite(*v)) return 0;
    if(*v == 0) *v = 0;
    return 1;
}
