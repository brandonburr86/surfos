/*
SurfOS Undertow: the helpers every part of the browser shares
----------------------
File: util.c  Date: 10/11/26
----------------------
The monotonic clock, error messages, UTF-8 and terminal-safe text (undertow.h).
None of these keep any state.
*/

#include <stdio.h>
#include <string.h>
#include <surf.h>
#include "undertow.h"

/**** the monotonic clock ****/

unsigned int ut_now(void) {
    return uptime();
}

/* now - deadline below 2^31 means now is at or past it, whichever side of the wrap either is */
int ut_expired(unsigned int deadline, unsigned int now) {
    return now - deadline < 0x80000000u;
}

unsigned int ut_remaining(unsigned int deadline, unsigned int now) {
    return ut_expired(deadline, now) ? 0 : deadline - now;
}

/**** error messages ****/

int ut_verror(char *err, size_t cap, const char *fmt, va_list ap) {
    char tmp[UT_ERR_MAX];               /* an argument may be err itself: format apart first */
    size_t i;
    if(!err || !cap) return -1;
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    for(i = 0; tmp[i] && i + 1 < cap; i++) {
        unsigned char c = (unsigned char)tmp[i];
        err[i] = c < 32 || c >= 127 ? '?' : (char)c;
    }
    err[i] = 0;
    return -1;
}

int ut_error(char *err, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    ut_verror(err, cap, fmt, ap);
    va_end(ap);
    return -1;
}

/**** UTF-8 ****/

size_t ut_utf8_decode(const unsigned char *s, size_t len, unsigned int *cp) {
    unsigned int c = s[0], v, min;
    size_t n, i;
    if(c < 0x80) {
        *cp = c;
        return 1;
    }
    if(c >= 0xc2 && c <= 0xdf) { n = 2; v = c & 0x1f; min = 0x80; }
    else if(c >= 0xe0 && c <= 0xef) { n = 3; v = c & 0x0f; min = 0x800; }
    else if(c >= 0xf0 && c <= 0xf4) { n = 4; v = c & 0x07; min = 0x10000; }
    else { *cp = UT_REPLACEMENT; return 1; }      /* a continuation byte, C0, C1, F5-FF */
    if(len < n) { *cp = UT_REPLACEMENT; return 1; }
    for(i = 1; i < n; i++) {
        if((s[i] & 0xc0) != 0x80) { *cp = UT_REPLACEMENT; return 1; }
        v = (v << 6) | (s[i] & 0x3f);
    }
    if(v < min || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff)) { *cp = UT_REPLACEMENT; return 1; }
    *cp = v;
    return n;
}

size_t ut_utf8_encode(unsigned int cp, unsigned char out[4]) {
    if(cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) cp = UT_REPLACEMENT;
    if(cp < 0x80) { out[0] = (unsigned char)cp; return 1; }
    if(cp < 0x800) {
        out[0] = (unsigned char)(0xc0 | cp >> 6);
        out[1] = (unsigned char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if(cp < 0x10000) {
        out[0] = (unsigned char)(0xe0 | cp >> 12);
        out[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (unsigned char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (unsigned char)(0xf0 | cp >> 18);
    out[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
    out[3] = (unsigned char)(0x80 | (cp & 0x3f));
    return 4;
}

int ut_unsafe_char(unsigned int cp) {
    return cp < 0x20 || cp == 0x7f || (cp >= 0x80 && cp <= 0x9f) || cp == 0x061c ||
           cp == 0x200e || cp == 0x200f || cp == 0x2028 || cp == 0x2029 ||
           (cp >= 0x202a && cp <= 0x202e) || (cp >= 0x2066 && cp <= 0x2069) || cp == 0xfeff;
}

/**** ASCII for a terminal without UTF-8 ****/

/* U+00A0-U+00FF */
static const char *const latin1[96] = {
    " ", "!", "c", "GBP", "$", "Y", "|", "S", "\"", "(c)", "a", "<<", "-", "", "(R)", "-",
    "o", "+/-", "2", "3", "'", "u", "P", ".", ",", "1", "o", ">>", "1/4", "1/2", "3/4", "?",
    "A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I", "I",
    "D", "N", "O", "O", "O", "O", "O", "x", "O", "U", "U", "U", "U", "Y", "Th", "ss",
    "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
    "d", "n", "o", "o", "o", "o", "o", "/", "o", "u", "u", "u", "u", "y", "th", "y"
};

/* other common characters, sorted by code point for the binary search */
static const struct { unsigned int cp; const char *ascii; } others[] = {
    { 0x0131, "i" }, { 0x0141, "L" }, { 0x0142, "l" }, { 0x0152, "OE" }, { 0x0153, "oe" },
    { 0x0160, "S" }, { 0x0161, "s" }, { 0x0178, "Y" }, { 0x017d, "Z" }, { 0x017e, "z" },
    { 0x0192, "f" }, { 0x02c6, "^" }, { 0x02dc, "~" },
    { 0x2002, " " }, { 0x2003, " " }, { 0x2004, " " }, { 0x2005, " " }, { 0x2006, " " },
    { 0x2007, " " }, { 0x2008, " " }, { 0x2009, " " }, { 0x200a, " " }, { 0x200b, "" },
    { 0x200c, "" }, { 0x200d, "" }, { 0x2010, "-" }, { 0x2011, "-" }, { 0x2012, "-" },
    { 0x2013, "-" }, { 0x2014, "--" }, { 0x2015, "--" }, { 0x2018, "'" }, { 0x2019, "'" },
    { 0x201a, "'" }, { 0x201b, "'" }, { 0x201c, "\"" }, { 0x201d, "\"" }, { 0x201e, "\"" },
    { 0x201f, "\"" }, { 0x2020, "+" }, { 0x2021, "++" }, { 0x2022, "*" }, { 0x2023, ">" },
    { 0x2024, "." }, { 0x2026, "..." }, { 0x202f, " " }, { 0x2030, "%o" }, { 0x2032, "'" },
    { 0x2033, "\"" }, { 0x2039, "<" }, { 0x203a, ">" }, { 0x2044, "/" }, { 0x205f, " " },
    { 0x2060, "" }, { 0x20ac, "EUR" }, { 0x2122, "(TM)" }, { 0x2190, "<-" }, { 0x2191, "^" },
    { 0x2192, "->" }, { 0x2193, "v" }, { 0x2194, "<->" }, { 0x21d0, "<=" }, { 0x21d2, "=>" },
    { 0x2212, "-" }, { 0x2215, "/" }, { 0x2217, "*" }, { 0x221e, "inf" }, { 0x2248, "~" },
    { 0x224b, "~" }, { 0x2260, "!=" }, { 0x2264, "<=" }, { 0x2265, ">=" }, { 0x2500, "-" },
    { 0x2502, "|" }, { 0x25aa, "*" }, { 0x25b6, ">" }, { 0x25b8, ">" }, { 0x25cf, "*" },
    { 0x25e6, "o" }, { 0x2605, "*" }, { 0x2713, "v" }, { 0x2714, "v" }, { 0x2717, "x" },
    { 0x2718, "x" }, { 0x3000, " " }, { 0xfeff, "" }, { 0xfffd, "?" }
};

const char *ut_ascii_for(unsigned int cp) {
    static const char printable[95 * 2] =
        " \0!\0\"\0#\0$\0%\0&\0'\0(\0)\0*\0+\0,\0-\0.\0/\0" "0\0" "1\0" "2\0" "3\0" "4\0" "5\0" "6\0" "7\0"
        "8\0" "9\0:\0;\0<\0=\0>\0?\0@\0A\0B\0C\0D\0E\0F\0G\0H\0I\0J\0K\0L\0M\0N\0O\0P\0Q\0R\0S\0T\0"
        "U\0V\0W\0X\0Y\0Z\0[\0\\\0]\0^\0_\0`\0a\0b\0c\0d\0e\0f\0g\0h\0i\0j\0k\0l\0m\0n\0o\0p\0q\0"
        "r\0s\0t\0u\0v\0w\0x\0y\0z\0{\0|\0}\0~";
    size_t lo = 0, hi = sizeof(others) / sizeof(others[0]);
    if(cp >= 0x20 && cp < 0x7f) return printable + (cp - 0x20) * 2;
    if(cp >= 0xa0 && cp <= 0xff) return latin1[cp - 0xa0];
    while(lo < hi) {
        size_t mid = (lo + hi) / 2;
        if(others[mid].cp == cp) return others[mid].ascii;
        if(others[mid].cp < cp) lo = mid + 1;
        else hi = mid;
    }
    return "?";
}

/**** safe text ****/

size_t ut_safe_text(char *out, size_t cap, const char *in, size_t len, int utf8) {
    const unsigned char *s = (const unsigned char *)in;
    unsigned char buf[4];
    const char *rep;
    unsigned int cp;
    size_t i = 0, o = 0, n;
    if(!out || !cap) return 0;
    if(!s) len = 0;
    while(i < len) {
        i += ut_utf8_decode(s + i, len - i, &cp);
        if(cp == '\t' || cp == '\n' || cp == '\r' || cp == '\v' || cp == '\f' ||
           cp == 0x85 || cp == 0x2028 || cp == 0x2029) cp = ' ';
        else if(ut_unsafe_char(cp)) continue;
        if(utf8) {
            n = ut_utf8_encode(cp, buf);
            rep = (const char *)buf;
        } else {
            rep = ut_ascii_for(cp);
            n = strlen(rep);
        }
        if(n >= cap - o) break;             /* it does not fit whole: stop here */
        memcpy(out + o, rep, n);
        o += n;
    }
    out[o] = 0;
    return o;
}
