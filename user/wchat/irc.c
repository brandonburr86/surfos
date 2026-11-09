/*
SurfOS chat client: wchat
--------------------
File: irc.c     Date: 11/05/26
--------------------
IRC's messages on the wire (irc.h): lines cut from a server's bytes, parsed in place, names
compared under a case mapping, lines put together for sending, base64 for SASL, CTCP. Written
from RFC 1459, RFC 2812, the IRCv3 message-tags specification, the ISUPPORT draft
(draft-brocklesby-irc-isupport) and RFC 4648. No system calls: host-tested as it is.
*/

#include <string.h>
#include "irc.h"

/**** lines ****/

int irc_read(struct irc_reader *r, const char *in, int n, int *used) {
    int i;
    if(r->ready) {                      /* the last line has been looked at */
        r->ready = 0;
        r->len = 0;
    }
    for(i = 0; i < n; i++) {
        char ch = in[i];
        if(ch != '\n') {
            if(r->over) continue;
            if(r->len >= IRC_IN_MAX - 1) {          /* a CR's room is the last: the LF would make it IRC_IN_MAX */
                r->over = 1;
                continue;
            }
            r->line[r->len++] = ch;
            continue;
        }
        if(r->over) {
            r->over = 0;
            r->len = 0;
            *used = i + 1;
            return IRC_LONG;
        }
        if(r->len && r->line[r->len - 1] == '\r') r->len--;
        else if(r->len == IRC_IN_MAX - 1) {         /* no CR: its room held a byte of the line */
            r->len = 0;
            *used = i + 1;
            return IRC_LONG;
        }
        if(!r->len) continue;           /* an empty line says nothing */
        r->line[r->len] = 0;
        r->ready = 1;
        *used = i + 1;
        return memchr(r->line, 0, (size_t)r->len) ? IRC_BAD : IRC_LINE;
    }
    *used = n;
    return IRC_MORE;
}

/**** messages ****/

static int letter(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static int digit(int c) {
    return c >= '0' && c <= '9';
}

/* the command at s: letters (made upper case here) or three digits */
static int command_ok(char *s, int *numeric) {
    int n = (int)strlen(s), i;
    *numeric = -1;
    if(n == 3 && digit(s[0]) && digit(s[1]) && digit(s[2])) {
        *numeric = (s[0] - '0') * 100 + (s[1] - '0') * 10 + (s[2] - '0');
        return 1;
    }
    if(!n || n > IRC_COMMAND_MAX) return 0;
    for(i = 0; i < n; i++) {
        if(!letter(s[i])) return 0;
        if(s[i] >= 'a') s[i] = (char)(s[i] - 32);
    }
    return 1;
}

static char *skip_spaces(char *p) {
    while(*p == ' ') p++;
    return p;
}

/* the end of the part at p: its first space, which becomes a NUL; the next part after the spaces */
static char *cut_part(char *p) {
    while(*p && *p != ' ') p++;
    if(*p) *p++ = 0;
    return skip_spaces(p);
}

int irc_parse(char *line, struct irc_msg *m) {
    static char none[] = "";
    char *p = line, *bang, *at;
    memset(m, 0, sizeof(*m));
    m->numeric = -1;
    m->nick = m->user = m->host = none;
    if(*p == '@') {
        m->tags = p + 1;
        while(*p && *p != ' ') p++;
        if(!*p) return -1;
        p = cut_part(p);
    }
    if(*p == ':') {
        m->prefix = p + 1;
        while(*p && *p != ' ') p++;
        if(!*p || p == m->prefix) return -1;
        p = cut_part(p);
    }
    m->command = p;
    p = cut_part(p);
    if(!command_ok(m->command, &m->numeric)) return -1;
    while(*p) {
        if(*p == ':' || m->argc == IRC_PARAMS - 1) {    /* the last: the rest of the line, spaces and all */
            m->argv[m->argc++] = *p == ':' ? p + 1 : p;
            break;
        }
        m->argv[m->argc++] = p;
        p = cut_part(p);
    }
    if(!m->prefix) {
        m->server = 1;
        return 0;
    }
    m->nick = m->prefix;
    bang = strchr(m->prefix, '!');
    at = strchr(bang ? bang : m->prefix, '@');
    if(at) {
        *at = 0;
        m->host = at + 1;
    }
    if(bang) {
        *bang = 0;
        m->user = bang + 1;
    }
    m->server = !bang && !at && strchr(m->prefix, '.') != NULL;     /* a nick never holds a dot */
    return 0;
}

/**** names ****/

int irc_lower(int cm, int c) {
    int last = cm == IRC_CM_ASCII ? 'Z' : cm == IRC_CM_STRICT ? ']' : '^';
    return c >= 'A' && c <= last ? c + 32 : c;
}

int irc_casecmp(int cm, const char *a, const char *b) {
    const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
    for(;; x++, y++) {
        int p = irc_lower(cm, *x), q = irc_lower(cm, *y);
        if(p != q || !p) return p - q;
    }
}

int irc_casemap_named(const char *name) {
    if(!strcmp(name, "ascii")) return IRC_CM_ASCII;
    if(!strcmp(name, "strict-rfc1459")) return IRC_CM_STRICT;
    return IRC_CM_RFC1459;
}

/* bytes no name may hold: they would end a part, a message or a list */
static int name_bytes_ok(const char *s, int max) {
    const unsigned char *p = (const unsigned char *)s;
    int n = 0;
    if(!*p || *p == ':') return 0;
    for(; *p; p++, n++) if(*p <= ' ' || *p == ',' || *p == 0x7f || n >= max) return 0;
    return 1;
}

int irc_valid_nick(const char *s, int max, const char *chantypes) {
    const unsigned char *p;
    if(!name_bytes_ok(s, max) || strpbrk(s, "!@*?")) return 0;
    for(p = (const unsigned char *)s; *p; p++) if(*p > 0x7e) return 0;     /* RFC 2812's nicks are ASCII */
    return !chantypes || !strchr(chantypes, s[0]);
}

int irc_valid_channel(const char *s, int max, const char *chantypes) {
    return name_bytes_ok(s, max) && chantypes && strchr(chantypes, s[0]);     /* s[0] is no NUL here */
}

/**** lines going out ****/

static int append(char *out, int size, int *o, const char *s, int middle) {
    const unsigned char *p = (const unsigned char *)s;
    if(middle && (!*p || *p == ':')) return -1;
    for(; *p; p++) {
        if(*p == '\r' || *p == '\n' || (middle && *p == ' ')) return -1;
        if(*o + 1 >= size) return -1;
        out[(*o)++] = (char)*p;
    }
    return 0;
}

int irc_compose(char *out, int size, const char *cmd, const char *const *mid, int nmid, const char *trail) {
    int o = 0, i;
    const char *c;
    if(size < 3 || !*cmd) return -1;
    for(c = cmd; *c; c++) if(!letter(*c)) return -1;
    if(append(out, size, &o, cmd, 1)) return -1;
    for(i = 0; i < nmid; i++) {
        if(o + 1 >= size) return -1;
        out[o++] = ' ';
        if(append(out, size, &o, mid[i], 1)) return -1;
    }
    if(trail) {
        if(o + 2 >= size) return -1;
        out[o++] = ' ';
        out[o++] = ':';
        if(append(out, size, &o, trail, 0)) return -1;
    }
    if(o + 2 > IRC_LINE_MAX || o + 3 > size) return -1;
    out[o++] = '\r';
    out[o++] = '\n';
    out[o] = 0;
    return o;
}

int irc_base64(const unsigned char *in, int n, char *out, int size) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int i, o = 0;
    if(n < 0 || (n + 2) / 3 * 4 + 1 > size) return -1;
    for(i = 0; i < n; i += 3) {
        unsigned v = (unsigned)in[i] << 16;
        if(i + 1 < n) v |= (unsigned)in[i + 1] << 8;
        if(i + 2 < n) v |= in[i + 2];
        out[o++] = alphabet[v >> 18 & 63];
        out[o++] = alphabet[v >> 12 & 63];
        out[o++] = i + 1 < n ? alphabet[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? alphabet[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

/**** CTCP ****/

int irc_ctcp(const char *text, char *cmd, int size, const char **arg, int *arglen) {
    const char *p = text + 1, *end;
    int n = 0;
    if(text[0] != 1 || size < 2) return 0;
    while(letter(*p) || digit(*p)) {
        if(n >= size - 1) return 0;
        cmd[n++] = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
        p++;
    }
    if(!n || (*p && *p != ' ' && *p != 1)) return 0;
    cmd[n] = 0;
    if(*p == ' ') p++;
    end = strchr(p, 1);
    if(!end) end = p + strlen(p);
    *arg = p;
    *arglen = (int)(end - p);
    return 1;
}
