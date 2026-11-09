/*
SurfOS chat client: wchat
--------------------
File: ircfmt.c  Date: 11/05/26
--------------------
IRC's text on the screen (ircfmt.h): a server's bytes decoded and cleaned, the formatting codes
read into runs of one style or left out, the colours, and rows. Written from RFC 3629 (UTF-8),
the WHATWG Encoding Standard's index of windows-1252, and the IRC formatting document of
ircdocs. No system calls: host-tested as it is.
*/

#include <string.h>
#include "ircfmt.h"

/**** characters ****/

/* the length of the valid UTF-8 sequence at s (n bytes left), 0 when there is none (RFC 3629 section 4: no
   overlong forms, no surrogates, nothing past U+10FFFF) */
static int sequence(const unsigned char *s, int n) {
    int len, i;
    if(s[0] < 0x80) return 1;
    if(s[0] < 0xc2 || s[0] > 0xf4) return 0;
    len = s[0] < 0xe0 ? 2 : s[0] < 0xf0 ? 3 : 4;
    if(len > n) return 0;
    for(i = 1; i < len; i++) if((s[i] & 0xc0) != 0x80) return 0;
    if(s[0] == 0xe0 && s[1] < 0xa0) return 0;
    if(s[0] == 0xed && s[1] > 0x9f) return 0;
    if(s[0] == 0xf0 && s[1] < 0x90) return 0;
    if(s[0] == 0xf4 && s[1] > 0x8f) return 0;
    return len;
}

static unsigned decode(const unsigned char *s, int len) {
    unsigned v;
    int i;
    if(len == 1) return s[0];
    v = s[0] & (0x7f >> len);
    for(i = 1; i < len; i++) v = v << 6 | (s[i] & 0x3f);
    return v;
}

/* windows-1252's 0x80-0x9F (the WHATWG index); 0 where it has none */
static const unsigned short cp1252[32] = {
    0x20ac, 0, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017d, 0,
    0, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0, 0x017e, 0x0178
};

static int is_code(unsigned c) {
    return c == IRC_BOLD || c == IRC_COLOR || c == IRC_HEXCOLOR || c == IRC_PLAIN || c == IRC_MONO ||
           c == IRC_REVERSE || c == IRC_ITALIC || c == IRC_STRIKE || c == IRC_UNDERLINE;
}

int irc_is_code(int c) {
    return c >= 0 && is_code((unsigned)c);
}

/* nothing to show: controls, and the marks that are invisible but move or join what is */
static int hidden(unsigned c) {
    if(c < 0x20) return !is_code(c);
    if(c >= 0x7f && c <= 0x9f) return 1;
    return (c >= 0x200b && c <= 0x200f) || (c >= 0x202a && c <= 0x202e) || (c >= 0x2060 && c <= 0x2064) ||
           (c >= 0x2066 && c <= 0x2069) || c == 0xfeff;
}

static int encode(unsigned c, char *out) {
    if(c < 0x80) {
        out[0] = (char)c;
        return 1;
    }
    if(c < 0x800) {
        out[0] = (char)(0xc0 | c >> 6);
        out[1] = (char)(0x80 | (c & 0x3f));
        return 2;
    }
    if(c < 0x10000) {
        out[0] = (char)(0xe0 | c >> 12);
        out[1] = (char)(0x80 | (c >> 6 & 0x3f));
        out[2] = (char)(0x80 | (c & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | c >> 18);
    out[1] = (char)(0x80 | (c >> 12 & 0x3f));
    out[2] = (char)(0x80 | (c >> 6 & 0x3f));
    out[3] = (char)(0x80 | (c & 0x3f));
    return 4;
}

int irc_text(char *out, int size, const char *in, int n) {
    const unsigned char *s = (const unsigned char *)in;
    int i, len, o = 0, utf8 = 1;
    char b[4];
    if(size < 1) return 0;
    for(i = 0; i < n; i += len)                         /* all of it UTF-8, or none of it */
        if(!(len = sequence(s + i, n - i))) {
            utf8 = 0;
            break;
        }
    for(i = 0; i < n; i += len) {
        unsigned c;
        int k;
        if(utf8) {
            len = sequence(s + i, n - i);
            c = decode(s + i, len);
        } else {
            len = 1;
            c = s[i] >= 0x80 && s[i] < 0xa0 ? cp1252[s[i] - 0x80] : s[i];
            if(!c) continue;
        }
        if(c == '\t') c = ' ';
        if(hidden(c)) continue;
        if(c == 0x224b || c == 0x2248) c = '~';     /* the console draws both as SurfOS's mark, the Chat's own lines' */
        k = encode(c, b);
        if(o + k > size - 1) break;
        memcpy(out + o, b, (size_t)k);
        o += k;
    }
    out[o] = 0;
    return o;
}

/**** the codes ****/

static int digit(char c) {
    return c >= '0' && c <= '9';
}

static int hexval(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* the six hex digits at s as 0xRRGGBB, -1 when they are not there */
static long hex6(const char *s) {
    long v = 0;
    int i;
    for(i = 0; i < 6; i++) {
        int h = hexval(s[i]);
        if(h < 0) return -1;
        v = v << 4 | h;
    }
    return v;
}

/* up to two digits at s: their value and how many (*n), or -1 when none */
static int two_digits(const char *s, int *n) {
    if(!digit(s[0])) {
        *n = 0;
        return -1;
    }
    if(digit(s[1])) {
        *n = 2;
        return (s[0] - '0') * 10 + s[1] - '0';
    }
    *n = 1;
    return s[0] - '0';
}

int irc_code_len(const char *s) {
    int n, m;
    if(!is_code((unsigned char)*s)) return 0;
    if(*s == IRC_COLOR) {
        if(two_digits(s + 1, &n) < 0) return 1;
        if(s[1 + n] == ',' && two_digits(s + 2 + n, &m) >= 0) return 2 + n + m;
        return 1 + n;
    }
    if(*s == IRC_HEXCOLOR) {
        if(hex6(s + 1) < 0) return 1;
        if(s[7] == ',' && hex6(s + 8) >= 0) return 14;
        return 7;
    }
    return 1;
}

void irc_style_reset(struct irc_style *st) {
    memset(st, 0, sizeof(*st));
    st->fg = st->bg = IRC_DEFAULT;
}

/* the code at s, which is one, applied to st */
static void apply(const char *s, struct irc_style *st) {
    int n, m, v;
    long h;
    switch((unsigned char)*s) {
    case IRC_BOLD: st->bold ^= 1; break;
    case IRC_ITALIC: st->italic ^= 1; break;
    case IRC_UNDERLINE: st->underline ^= 1; break;
    case IRC_STRIKE: st->strike ^= 1; break;
    case IRC_MONO: st->mono ^= 1; break;
    case IRC_REVERSE: st->reverse ^= 1; break;
    case IRC_PLAIN: irc_style_reset(st); break;
    case IRC_COLOR:
        if((v = two_digits(s + 1, &n)) < 0) {
            st->fg = st->bg = IRC_DEFAULT;
            break;
        }
        st->fg = v == 99 ? IRC_DEFAULT : v;
        if(s[1 + n] == ',' && (v = two_digits(s + 2 + n, &m)) >= 0) st->bg = v == 99 ? IRC_DEFAULT : v;
        break;
    case IRC_HEXCOLOR:
        if((h = hex6(s + 1)) < 0) {
            st->fg = st->bg = IRC_DEFAULT;
            break;
        }
        st->fg = IRC_RGB(h);
        if(s[7] == ',' && (h = hex6(s + 8)) >= 0) st->bg = IRC_RGB(h);
        break;
    }
}

int irc_run(const char *s, int *pos, struct irc_style *st, int *start, int *len) {
    int p = *pos, k;
    while((k = irc_code_len(s + p)) > 0) {
        apply(s + p, st);
        p += k;
    }
    if(!s[p]) {
        *pos = p;
        return 0;
    }
    *start = p;
    while(s[p] && !is_code((unsigned char)s[p])) p++;
    *len = p - *start;
    *pos = p;
    return 1;
}

int irc_strip(char *out, int size, const char *s) {
    struct irc_style st;
    int pos = 0, start, len, o = 0;
    if(size < 1) return 0;
    irc_style_reset(&st);
    while(irc_run(s, &pos, &st, &start, &len)) {
        if(len > size - 1 - o) {                    /* cut between characters */
            len = size - 1 - o;
            while(len > 0 && ((unsigned char)s[start + len] & 0xc0) == 0x80) len--;
        }
        memcpy(out + o, s + start, (size_t)len);
        o += len;
        if(o >= size - 1) break;
    }
    out[o] = 0;
    return o;
}

/**** colours ****/

/* 0-15 by their names in the formatting document: white, black, blue, green, red, brown, magenta, orange,
   yellow, light green, cyan, light cyan, light blue, pink, grey, light grey; our shades of them */
static const unsigned basic[16] = {
    0xffffff, 0x000000, 0x1f2f9f, 0x1f8f1f, 0xe02020, 0x7f3f1f, 0x8f1f9f, 0xef7f0f,
    0xefdf1f, 0x3fdf3f, 0x1f8f8f, 0x3fdfdf, 0x3f5fef, 0xef3fcf, 0x7f7f7f, 0xc7c7c7
};

/* a hue (0-11, steps of 30 degrees) at a lightness and saturation in thousandths, as RGB (HSL's formulas,
   in integers) */
static unsigned hsl(int hue, int l, int s) {
    int c = (1000 - (2 * l - 1000 < 0 ? 1000 - 2 * l : 2 * l - 1000)) * s / 1000;
    int h2 = hue * 30 * 1000 / 60;                  /* the hue in sixths of the circle, thousandths */
    int r = h2 % 2000 - 1000;
    int x = c * (1000 - (r < 0 ? -r : r)) / 1000, m = l - c / 2, rgb[3], i;
    switch(h2 / 1000) {
    case 0: rgb[0] = c; rgb[1] = x; rgb[2] = 0; break;
    case 1: rgb[0] = x; rgb[1] = c; rgb[2] = 0; break;
    case 2: rgb[0] = 0; rgb[1] = c; rgb[2] = x; break;
    case 3: rgb[0] = 0; rgb[1] = x; rgb[2] = c; break;
    case 4: rgb[0] = x; rgb[1] = 0; rgb[2] = c; break;
    default: rgb[0] = c; rgb[1] = 0; rgb[2] = x; break;
    }
    for(i = 0; i < 3; i++) rgb[i] = (rgb[i] + m) * 255 / 1000;
    return (unsigned)rgb[0] << 16 | (unsigned)rgb[1] << 8 | (unsigned)rgb[2];
}

unsigned irc_rgb(int color) {
    /* 16-87: six rows of twelve hues, dark to light; 88-98: greys, black to white */
    static const short light[6] = { 200, 330, 470, 600, 730, 850 };
    if(color >= 0x1000000 && color <= 0x1ffffff) return (unsigned)color & 0xffffff;
    if(color < 0 || color > 98) return 0;
    if(color < 16) return basic[color];
    if(color < 88) return hsl((color - 16) % 12, light[(color - 16) / 12], 900);
    return (unsigned)((color - 88) * 255 / 10) * 0x010101u;
}

/* brightness by the eye's weights (ITU-R BT.601), 0-255 */
static int brightness(unsigned c) {
    return (int)(((c >> 16 & 255) * 299 + (c >> 8 & 255) * 587 + (c & 255) * 114) / 1000);
}

int irc_readable(unsigned fg, unsigned bg) {
    int d = brightness(fg) - brightness(bg);
    return d >= 90 || d <= -90;
}

/**** rows ****/

int irc_cells(const char *s) {
    int n = 0, k;
    while(*s) {
        if((k = irc_code_len(s)) > 0) {
            s += k;
            continue;
        }
        if(((unsigned char)*s & 0xc0) != 0x80) n++;
        s++;
    }
    return n;
}

int irc_wrap(const char *s, int at, int cols, int *next) {
    int p = at, cells = 0, space = -1, k;
    if(cols < 1) cols = 1;
    for(;;) {
        while((k = irc_code_len(s + p)) > 0) p += k;
        if(!s[p]) {
            *next = p;
            return p;
        }
        if(cells == cols) break;                    /* another character: it goes on the next row */
        if(s[p] == ' ') space = p;
        p++;
        while(((unsigned char)s[p] & 0xc0) == 0x80) p++;
        cells++;
    }
    if(s[p] != ' ' && space > at) p = space;        /* the row ends at the last space, not in a word */
    *next = p;
    for(;;) {                                       /* the spaces at the break are nobody's; codes before a */
        int q = *next;                              /* word stay with it */
        while((k = irc_code_len(s + q)) > 0) q += k;
        if(s[q] != ' ') break;
        *next = q + 1;
    }
    return p;
}
