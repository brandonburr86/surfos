/*
SurfOS file transfer client: ftp
--------------------
File: ftpclient.c  Date: 11/01/26
--------------------
The client's side of FTP (ftpclient.h), with no system calls. Host-tested as it is
(tests/host/test_ftpc.c), with malformed, split and oversized input.
*/

#include <stdio.h>
#include <string.h>
#include "ftpclient.h"

static int digit(char c) {
    return c >= '0' && c <= '9';
}

/**** UTF-8 ****/

/* the length of the valid sequence at s, 1 for ASCII; 0 when it is none. s ends in a NUL, which
   stops a sequence cut short */
static int sequence(const unsigned char *s) {
    int n, i;
    if(s[0] < 0x80) return 1;
    if(s[0] < 0xc2 || s[0] > 0xf4) return 0;
    n = s[0] < 0xe0 ? 2 : s[0] < 0xf0 ? 3 : 4;
    for(i = 1; i < n; i++) if((s[i] & 0xc0) != 0x80) return 0;
    if(s[0] == 0xe0 && s[1] < 0xa0) return 0;       /* overlong */
    if(s[0] == 0xed && s[1] > 0x9f) return 0;       /* a surrogate */
    if(s[0] == 0xf0 && s[1] < 0x90) return 0;       /* overlong */
    if(s[0] == 0xf4 && s[1] > 0x8f) return 0;       /* past U+10FFFF */
    return n;
}

/* the character at s and its bytes (*len): a sequence, or one invalid byte, as its own value */
static unsigned int char_at(const unsigned char *s, int *len) {
    int n = sequence(s), i;
    unsigned int v;
    if(n <= 1) {
        *len = 1;
        return s[0];
    }
    v = s[0] & (0xff >> (n + 1));
    for(i = 1; i < n; i++) v = v << 6 | (s[i] & 0x3f);
    *len = n;
    return v;
}

/**** replies ****/

void ftpc_reply_init(struct ftpc_reply *r) {
    r->code = r->lines = r->bytes = r->multi = 0;
    r->text[0] = 0;
}

/* the code at the start of line, 0 when there is none: three digits, the first of them 1 to 5 */
static int code_of(const char *line) {
    if(line[0] < '1' || line[0] > '5' || !digit(line[1]) || !digit(line[2])) return 0;
    return (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
}

static void keep_text(struct ftpc_reply *r, const char *line) {
    snprintf(r->text, sizeof(r->text), "%s", line[3] ? line + 4 : "");
}

int ftpc_reply_line(struct ftpc_reply *r, const char *line, int toolong) {
    r->lines++;
    r->bytes += toolong ? FTP_LINE_MAX : (int)strlen(line);
    if(r->lines > FTPC_REPLY_LINES || r->bytes > FTPC_REPLY_BYTES) return FTPC_R_BAD;
    if(!r->code) {
        if(toolong || !(r->code = code_of(line))) return FTPC_R_BAD;
        if(line[3] == '-') {
            r->multi = 1;
            keep_text(r, line);
            return FTPC_R_MORE;
        }
        if(line[3] && line[3] != ' ') return FTPC_R_BAD;
        keep_text(r, line);
        return FTPC_R_DONE;
    }
    if(!toolong && code_of(line) == r->code && (!line[3] || line[3] == ' ')) {
        keep_text(r, line);
        return FTPC_R_DONE;
    }
    return FTPC_R_MORE;
}

/**** commands ****/

int ftpc_command(char *out, int size, const char *verb, const char *arg) {
    int o = 0;
    const unsigned char *s;
    for(s = (const unsigned char *)verb; *s; s++) {
        if(o + 3 > size) return -1;
        out[o++] = (char)*s;
    }
    if(arg) {
        if(o + 3 > size) return -1;
        out[o++] = ' ';
        for(s = (const unsigned char *)arg; *s; s++) {
            if(*s < 0x20 || *s == 0x7f) return -1;
            if(o + (*s == 0xff ? 2 : 1) + 2 > size) return -1;
            if(*s == 0xff) out[o++] = (char)0xff;
            out[o++] = (char)*s;
        }
    }
    out[o++] = '\r';
    out[o++] = '\n';
    return o;
}

/**** what replies say ****/

int ftpc_parse_epsv(const char *text, unsigned short *port) {
    const char *p;
    for(p = strchr(text, '('); p; p = strchr(p + 1, '(')) {
        char d = p[1];
        long v = 0;
        int i;
        if(d < '!' || d > '~' || digit(d) || p[2] != d || p[3] != d) continue;
        for(i = 0; digit(p[4 + i]) && i < 5; i++) v = v * 10 + (p[4 + i] - '0');
        if(i && p[4 + i] == d && p[5 + i] == ')' && v >= 1 && v <= 65535) {
            *port = (unsigned short)v;
            return 0;
        }
    }
    return -1;
}

int ftpc_parse_pasv(const char *text, unsigned short *port) {
    char span[24];                      /* "255,255,255,255,255,255" */
    const char *p = text;
    while(*p) {
        size_t n = 0;
        unsigned int ip;
        if(!digit(*p)) {
            p++;
            continue;
        }
        while(digit(p[n]) || p[n] == ',') n++;
        if(n < sizeof(span)) {
            memcpy(span, p, n);
            span[n] = 0;
            if(ftp_parse_port(span, &ip, port) == 0) return 0;
        }
        p += n;
    }
    return -1;
}

int ftpc_port_ok(unsigned short port) {
    return port >= FTPC_PORT_MIN;
}

int ftpc_parse_path(const char *text, char *out, int size) {
    const char *p = strchr(text, '"');
    int o = 0;
    if(!p || size < 1) return -1;
    for(p++; *p; p++) {
        if(*p == '"') {
            if(p[1] != '"') {
                out[o] = 0;
                return 0;
            }
            p++;
        }
        if(o + 1 >= size) return -1;
        out[o++] = *p;
    }
    return -1;
}

/* 2^64 - 1 is this and a 5. No 64-bit division here: programs link no libgcc, and without -O it would call __udivdi3 */
#define SIZE_TENTH 1844674407370955161ULL

int ftpc_parse_size(const char *text, unsigned long long *size) {
    unsigned long long v = 0;
    int n = 0;
    while(*text == ' ') text++;
    for(; digit(*text); text++, n++) {
        unsigned int d = (unsigned int)(*text - '0');
        if(v > SIZE_TENTH || (v == SIZE_TENTH && d > 5)) return -1;
        v = v * 10 + d;
    }
    if(!n || (*text && *text != ' ')) return -1;
    *size = v;
    return 0;
}

/**** what is shown ****/

void ftpc_text(char *out, int size, const char *text) {
    const unsigned char *s = (const unsigned char *)text;
    int o = 0;
    if(size < 1) return;
    while(*s) {
        int n = sequence(s), i;
        if(n == 1 && *s >= 0x20 && *s != 0x7f) {
            if(o + 1 >= size) break;
            out[o++] = (char)*s++;
        } else if(n > 1 && !(n == 2 && s[0] == 0xc2 && s[1] < 0xa0)) {      /* U+0080 to U+009F: the C1 controls */
            if(o + n >= size) break;
            for(i = 0; i < n; i++) out[o++] = (char)*s++;
        } else {
            if(o + 1 >= size) break;
            out[o++] = *s == '\t' ? ' ' : '?';
            s += n > 1 ? n : 1;
        }
    }
    out[o] = 0;
}

void ftpc_scrub(char *text, const char *secret) {
    size_t n = strlen(secret), i;
    char *p;
    if(!n) return;
    for(p = strstr(text, secret); p; p = strstr(p + n, secret))
        for(i = 0; i < n; i++) p[i] = '*';
}

/**** what is typed ****/

int ftpc_words(char *line, char **word, int max) {
    char *s = line;
    int n = 0;
    for(;;) {
        char *d;
        char end;
        int quoted = 0;
        while(*s == ' ' || *s == '\t') s++;
        if(!*s) return n;
        if(n == max) return -1;
        word[n++] = d = s;
        for(; *s; s++) {
            if(*s == '"') quoted = !quoted;
            else if(quoted && *s == '\\' && (s[1] == '"' || s[1] == '\\')) *d++ = *++s;
            else if(!quoted && (*s == ' ' || *s == '\t')) break;
            else *d++ = *s;
        }
        if(quoted) return -2;
        end = *s;
        *d = 0;
        if(!end) return n;
        s++;
    }
}

const char *ftpc_last_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

int ftpc_join(char *out, int size, const char *dir, const char *name) {
    size_t n = strlen(dir);
    int r = n && dir[n - 1] != '/' ? snprintf(out, (size_t)size, "%s/%s", dir, name)
                                   : snprintf(out, (size_t)size, "%s%s", dir, name);
    return r < 0 || r >= size ? -1 : 0;
}

int ftpc_split(const char *path, char *dir, int size, const char **name) {
    const char *slash = strrchr(path, '/');
    size_t n = slash ? (size_t)(slash - path) : 0;
    if(slash && !n) n = 1;              /* "/name": the folder is "/" */
    if(n >= (size_t)size) return -1;
    memcpy(dir, path, n);
    dir[n] = 0;
    *name = slash ? slash + 1 : path;
    return 0;
}

/**** names from a server ****/

int ftpc_safe_name(const char *name) {
    const unsigned char *s = (const unsigned char *)name;
    size_t n = strlen(name);
    if(!n || n > FTPC_NAME_MAX || name[0] == '-' || name[n - 1] == '.' || name[n - 1] == ' ') return 0;
    while(*s) {
        int k = sequence(s);
        if(!k || *s < 0x20 || *s == 0x7f) return 0;
        if(k == 2 && s[0] == 0xc2 && s[1] < 0xa0) return 0;     /* the C1 controls */
        if(k == 1 && strchr("/\\:*?\"<>|", *s)) return 0;
        s += k;
    }
    return 1;
}

static unsigned int fold(unsigned int c, int icase) {
    return icase && c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* the bracket at p (just after its '['): *hit, whether c is in it, and where it ends (just after its ']'); NULL
   when it has no end, and the '[' stands for itself */
static const char *bracket(const char *p, unsigned int c, int icase, int *hit) {
    int neg = 0, first = 1, n;
    *hit = 0;
    if(*p == '!' || *p == '^') {
        neg = 1;
        p++;
    }
    c = fold(c, icase);
    for(;;) {
        unsigned int lo, hi;
        if(!*p) return NULL;
        if(*p == ']' && !first) break;
        first = 0;
        if(*p == '\\' && p[1]) p++;
        lo = fold(char_at((const unsigned char *)p, &n), icase);
        p += n;
        hi = lo;
        if(*p == '-' && p[1] && p[1] != ']') {
            p++;
            if(*p == '\\' && p[1]) p++;
            hi = fold(char_at((const unsigned char *)p, &n), icase);
            p += n;
        }
        if(c >= lo && c <= hi) *hit = 1;
    }
    *hit = *hit != neg;
    return p + 1;
}

int ftpc_glob(const char *pattern, const char *name, int icase) {
    const char *p = pattern, *n = name, *star_p = NULL, *star_n = NULL;
    if(*n == '.' && *p != '.' && !(*p == '\\' && p[1] == '.')) return 0;
    while(*n) {
        int nlen, plen, hit = 0;
        unsigned int c = char_at((const unsigned char *)n, &nlen);
        if(*p == '*') {
            while(*p == '*') p++;
            star_p = p;
            star_n = n;
            continue;
        }
        if(*p == '?') {
            p++;
            n += nlen;
            continue;
        }
        if(*p == '[') {
            const char *e = bracket(p + 1, c, icase, &hit);
            if(e) {
                if(hit) {
                    p = e;
                    n += nlen;
                    continue;
                }
                goto again;
            }
        }
        if(*p) {
            const char *q = *p == '\\' && p[1] ? p + 1 : p;
            unsigned int v = char_at((const unsigned char *)q, &plen);
            if(fold(v, icase) == fold(c, icase)) {
                p = q + plen;
                n += nlen;
                continue;
            }
        }
    again:
        if(!star_p) return 0;           /* back to the last star, which takes one more character */
        char_at((const unsigned char *)star_n, &nlen);
        star_n += nlen;
        p = star_p;
        n = star_n;
    }
    while(*p == '*') p++;
    return !*p;
}

static int line_ended(struct ftpc_lines *l) {
    int got = l->toolong ? FTP_LONG : FTP_LINE;
    if(!l->toolong && l->len && l->line[l->len - 1] == '\r') l->len--;
    l->line[l->toolong ? 0 : l->len] = 0;
    l->len = l->toolong = 0;
    return got;
}

int ftpc_lines_feed(struct ftpc_lines *l, const char *in, int n, int *got) {
    int i;
    for(i = 0; i < n; i++) {
        char c = in[i];
        if(c == '\n') {
            *got = line_ended(l);
            return i + 1;
        }
        if(l->len == FTPC_LIST_LINE) l->toolong = 1;
        else l->line[l->len++] = c ? c : 0x7f;
    }
    *got = FTP_MORE;
    return n;
}

int ftpc_lines_end(struct ftpc_lines *l) {
    if(!l->len && !l->toolong) return FTP_MORE;
    if(l->toolong) {
        l->len = l->toolong = 0;
        return FTP_MORE;
    }
    return line_ended(l);
}

/**** a password as it is typed ****/

int ftpc_secret_key(struct ftpc_secret *s, unsigned char c) {
    if(s->esc == 1) {
        s->esc = c == '[' ? 2 : 0;
        return FTPC_S_MORE;
    }
    if(s->esc == 2) {
        if(c >= 0x40 && c <= 0x7e) s->esc = 0;
        return FTPC_S_MORE;
    }
    switch(c) {
    case '\r':
    case '\n':
        s->text[s->len] = 0;
        return FTPC_S_DONE;
    case 0x03:
    case 0x04:
        return FTPC_S_CANCEL;
    case 0x1b:
        s->esc = 1;
        return FTPC_S_MORE;
    case 0x15:
        ftpc_wipe(s->text, s->len);
        s->len = s->over = 0;
        return FTPC_S_MORE;
    case 0x7f:
    case 0x08:
        while(s->len > 0 && (s->text[--s->len] & 0xc0) == 0x80) s->text[s->len] = 0;
        s->text[s->len] = 0;
        return FTPC_S_MORE;
    }
    if(c < 0x20) return FTPC_S_MORE;
    if(s->len == FTPC_SECRET_MAX) s->over = 1;
    else s->text[s->len++] = (char)c;
    return FTPC_S_MORE;
}

void ftpc_wipe(void *p, int n) {
    volatile unsigned char *v = (volatile unsigned char *)p;
    while(n-- > 0) *v++ = 0;
}
