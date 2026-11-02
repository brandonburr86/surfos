/*
SurfOS telnet client: telnet
--------------------
File: tnclient.c  Date: 11/01/26
--------------------
The client's side of the telnet protocol (tnclient.h): a reader for what the server sends, a
writer for what is typed, and the options as RFC 1143 decides them. No system calls and no
library calls, so the host tests run this file as it is.
*/

#include "tnclient.h"

enum { S_DATA, S_IAC, S_OPTION, S_SB, S_SB_IAC };
enum { L_TEXT, L_ESC, L_CSI };          /* the watch on the terminal's look */

static void put(struct tn_buf *b, unsigned char c) {
    if(b && b->len < b->size) b->p[b->len++] = c;
}

/* all of it or none, so that no sequence reaches the other end cut short */
static int whole(struct tn_buf *b, const unsigned char *p, int n) {
    int i;
    if(b->len + n > b->size) return 0;
    for(i = 0; i < n; i++) b->p[b->len++] = p[i];
    return 1;
}

static void put3(struct tn_buf *b, int verb, int opt) {
    unsigned char c[3];
    c[0] = TN_IAC;
    c[1] = (unsigned char)verb;
    c[2] = (unsigned char)opt;
    whole(b, c, 3);
}

static void say(struct tn_buf *b, const char *s) {
    while(*s) put(b, (unsigned char)*s++);
}

void tnc_init(struct tnc *t, int cols, int rows, int terminal) {
    unsigned char *p = (unsigned char *)t;
    unsigned int i;
    for(i = 0; i < sizeof(*t); i++) p[i] = 0;
    t->state = S_DATA;
    t->look = L_TEXT;
    t->cols = cols;
    t->rows = rows;
    t->terminal = terminal;
    t->at_line_start = 1;
}

int tnc_remote_echo(const struct tnc *t) {
    return t->him[TO_ECHO] == TN_YES;
}

int tnc_by_key(const struct tnc *t) {
    return t->him[TO_SGA] == TN_YES;
}

/* what the server may do on its side: echo the keys, no go-aheads, send 8 bits. Never asked of
   this end: ECHO, which would have the two ends echo each other for ever */
static int he_may(int opt) {
    return opt == TO_ECHO || opt == TO_SGA || opt == TO_BINARY;
}

/* what this end does: no go-aheads (it never sent any), tell the window, send 8 bits */
static int we_may(int opt) {
    return opt == TO_SGA || opt == TO_NAWS || opt == TO_BINARY;
}

static void ask(struct tnc *t, struct tn_buf *net, int verb, int opt) {
    unsigned char *s = verb == TN_DO ? &t->him[opt] : &t->us[opt];
    if(*s != TN_NO) return;
    *s = TN_WANTYES;
    put3(net, verb, opt);
}

void tnc_opening(struct tnc *t, struct tn_buf *net) {
    if(t->speaks) return;
    t->speaks = 1;
    ask(t, net, TN_DO, TO_SGA);
    ask(t, net, TN_DO, TO_ECHO);
    ask(t, net, TN_DO, TO_BINARY);
    ask(t, net, TN_WILL, TO_BINARY);
    ask(t, net, TN_WILL, TO_NAWS);
}

/* the window, as NAWS says it: two 16-bit numbers, a byte 255 among them doubled */
static void tell_size(struct tnc *t, struct tn_buf *net) {
    unsigned char b[TNC_NAWS_MAX], v[4];
    int n = 0, i;
    if(t->cols <= 0 || t->rows <= 0) return;    /* no size known: nothing to say */
    v[0] = (unsigned char)(t->cols >> 8);
    v[1] = (unsigned char)t->cols;
    v[2] = (unsigned char)(t->rows >> 8);
    v[3] = (unsigned char)t->rows;
    b[n++] = TN_IAC;
    b[n++] = TN_SB;
    b[n++] = TO_NAWS;
    for(i = 0; i < 4; i++) {
        if(v[i] == TN_IAC) b[n++] = TN_IAC;
        b[n++] = v[i];
    }
    b[n++] = TN_IAC;
    b[n++] = TN_SE;
    if(whole(net, b, n)) {
        t->told_cols = t->cols;
        t->told_rows = t->rows;
    }
}

void tnc_size(struct tnc *t, int cols, int rows, struct tn_buf *net) {
    t->cols = cols > 65535 ? 65535 : cols;
    t->rows = rows > 65535 ? 65535 : rows;
    if(t->us[TO_NAWS] == TN_YES && (t->cols != t->told_cols || t->rows != t->told_rows)) tell_size(t, net);
}

void tnc_ayt(struct tnc *t, struct tn_buf *net) {
    unsigned char c[2];
    (void)t;
    c[0] = TN_IAC;
    c[1] = TN_AYT;
    whole(net, c, 2);
}

/* a data byte to the server */
static void send_byte(struct tn_buf *net, unsigned char c) {
    if(c == TN_IAC) put(net, TN_IAC);
    put(net, c);
}

static void send_return(const struct tnc *t, struct tn_buf *net) {
    put(net, '\r');
    if(t->us[TO_BINARY] != TN_YES) put(net, '\n');
}

/* what waits in the line goes */
static void send_line(struct tnc *t, struct tn_buf *net) {
    int i;
    for(i = 0; i < t->edit.len; i++) send_byte(net, t->edit.text[i]);
    t->edit.len = 0;
}

/* WILL, WONT, DO or DONT for opt (RFC 1143, without the queue: this end never changes its mind).
   An answer goes out only when the option's state changes. */
static void option(struct tnc *t, int verb, int opt, struct tn_buf *net) {
    unsigned char *s = verb == TN_WILL || verb == TN_WONT ? &t->him[opt] : &t->us[opt];
    switch(verb) {
    case TN_WILL:                               /* the server offers, or accepts our DO */
    case TN_DO:                                 /* the server asks us, or accepts our WILL */
        if(*s == TN_NO) {
            int yes = verb == TN_WILL ? he_may(opt) : we_may(opt);
            if(yes) *s = TN_YES;
            put3(net, verb == TN_WILL ? (yes ? TN_DO : TN_DONT) : (yes ? TN_WILL : TN_WONT), opt);
        } else if(*s == TN_WANTYES) {
            *s = TN_YES;                        /* our offer or request accepted */
        } else if(*s == TN_WANTNO) {
            *s = TN_NO;                         /* an answer that contradicts us: NO (RFC 1143) */
        }
        break;
    case TN_WONT:                               /* the server refuses or stops */
    case TN_DONT:
        if(*s == TN_YES) {
            *s = TN_NO;
            put3(net, verb == TN_WONT ? TN_DONT : TN_WONT, opt);
        } else if(*s == TN_WANTYES || *s == TN_WANTNO) {
            *s = TN_NO;
        }
        break;
    }
    if(verb == TN_DO && opt == TO_NAWS && *s == TN_YES &&
       (t->cols != t->told_cols || t->rows != t->told_rows)) tell_size(t, net);
    if(verb == TN_DONT && opt == TO_NAWS) t->told_cols = t->told_rows = 0;    /* said again if it comes back */
    if(verb == TN_WILL && opt == TO_SGA && tnc_by_key(t)) send_line(t, net);  /* keys go as they come from now on */
}

/* the terminal's look: a sequence that leaves something on, watched as it goes by. The console's own
   rule (kernel/console.c) is a CSI with one number for the private modes; anything else, a colour
   that is not a reset, may leave a colour on */
static void look(struct tnc *t, unsigned char c) {
    switch(t->look) {
    case L_ESC:
        t->look = c == '[' ? L_CSI : c == 0x1b ? L_ESC : L_TEXT;
        t->look_num = t->look_mark = t->look_more = 0;
        break;
    case L_CSI:
        if(c == 0x1b) {
            t->look = L_ESC;
        } else if(c >= '0' && c <= '9') {
            if(t->look_num < 100000) t->look_num = t->look_num * 10 + (c - '0');
        } else if(c == '?') {
            t->look_mark = 1;
        } else if(c == ';') {
            t->look_more = 1;
        } else if(c >= 0x40 && c <= 0x7e) {
            t->look = L_TEXT;
            if(t->look_mark && !t->look_more) {
                if(t->look_num == 1049 && (c == 'h' || c == 'l')) t->alt = c == 'h';
                if(t->look_num == 25 && (c == 'h' || c == 'l')) t->cursor_off = c == 'l';
            } else if(!t->look_mark && c == 'm') {
                t->colours = t->look_num != 0 || t->look_more;
            }
        }
        break;
    default:
        if(c == 0x1b) t->look = L_ESC;
    }
}

int tnc_restore(struct tnc *t, char *out) {
    struct tn_buf b;
    b.p = (unsigned char *)out;
    b.len = 0;
    b.size = TNC_RESTORE_MAX;
    if(t->colours) say(&b, "\033[m");
    if(t->cursor_off) say(&b, "\033[?25h");
    if(t->alt) say(&b, "\033[?1049l");
    if(!t->at_line_start) say(&b, "\r\n");
    t->colours = t->cursor_off = t->alt = 0;
    t->at_line_start = 1;
    t->look = L_TEXT;
    return b.len;
}

/* a data byte for the terminal */
static void show(struct tnc *t, struct tn_buf *term, unsigned char c) {
    look(t, c);
    t->at_line_start = c == '\n';
    put(term, c);
}

/* IAC c */
static void command(struct tnc *t, unsigned char c, struct tn_buf *term, struct tn_buf *net) {
    t->state = S_DATA;
    switch(c) {
    case TN_IAC:                                /* IAC IAC: the data byte 255 */
        t->cr = 0;
        show(t, term, TN_IAC);
        break;
    case TN_WILL:
    case TN_WONT:
    case TN_DO:
    case TN_DONT:
        tnc_opening(t, net);                    /* it speaks telnet: now this end's offers go too */
        t->verb = c;
        t->state = S_OPTION;
        break;
    case TN_SB:
        tnc_opening(t, net);
        t->state = S_SB;
        break;
    default:                                    /* NOP, DM, GA, AO, a stray SE; AYT, IP, BRK, EC, EL: not a server's to ask */
        break;
    }
}

void tnc_recv(struct tnc *t, const unsigned char *in, int n, struct tn_buf *term, struct tn_buf *net) {
    int i;
    for(i = 0; i < n; i++) {
        unsigned char c = in[i];
        switch(t->state) {
        case S_DATA:
            if(c == TN_IAC) {
                t->state = S_IAC;
            } else if(t->him[TO_BINARY] == TN_YES) {
                t->cr = 0;
                show(t, term, c);
            } else if(c == 0) {
                t->cr = 0;                      /* CR NUL: a CR alone; a NUL by itself: NVT's no-op */
            } else {
                t->cr = c == '\r';
                show(t, term, c);
            }
            break;
        case S_IAC:
            command(t, c, term, net);
            break;
        case S_OPTION:
            t->state = S_DATA;
            option(t, t->verb, c, net);
            break;
        case S_SB:
            if(c == TN_IAC) t->state = S_SB_IAC;
            break;
        case S_SB_IAC:
            if(c == TN_SE) t->state = S_DATA;
            else if(c == TN_IAC) t->state = S_SB;   /* IAC IAC inside: the byte 255 */
            else command(t, c, term, net);          /* a sub-negotiation cut short: the command after its IAC counts */
            break;
        }
    }
}

/* how many bytes the last character of s (n bytes) has: a whole UTF-8 sequence, or one invalid byte */
static int last_char(const unsigned char *s, int n) {
    int k;
    for(k = 1; k <= 4 && k <= n; k++) {
        unsigned char c = s[n - k];
        if((c & 0xc0) == 0x80) continue;
        return (c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1) == k ? k : 1;
    }
    return 1;
}

int tnc_edit_key(struct tnc_edit *e, unsigned char c, struct tn_buf *echo) {
    if(e->esc == 1) {                           /* ESC [: a special key's sequence; ESC and a key: Alt */
        e->esc = c == '[' ? 2 : 0;
        return TNC_E_MORE;
    }
    if(e->esc == 2) {
        if(c >= 0x40 && c <= 0x7e) e->esc = 0;
        return TNC_E_MORE;
    }
    if(c == '\n' && e->cr) {
        e->cr = 0;
        return TNC_E_MORE;
    }
    e->cr = c == '\r';
    switch(c) {
    case 0x1b:
        e->esc = 1;
        return TNC_E_MORE;
    case '\r':
    case '\n':
        say(echo, "\r\n");
        return TNC_E_LINE;
    case 0x7f:
    case 0x08:
        if(e->len) {
            e->len -= last_char(e->text, e->len);
            say(echo, "\b \b");
        }
        return TNC_E_MORE;
    case 0x15:
        e->len = 0;
        say(echo, "^U\r\n");
        return TNC_E_MORE;
    case 0x03:
        e->len = 0;
        say(echo, "^C\r\n");
        return TNC_E_INTR;
    }
    if(c < 0x20 && c != '\t') return TNC_E_MORE;
    e->text[e->len++] = c;
    put(echo, c);
    return e->len == TNC_LINE_MAX ? TNC_E_FULL : TNC_E_MORE;
}

int tnc_keys(struct tnc *t, const unsigned char *in, int n, struct tn_buf *net, struct tn_buf *echo) {
    int i;
    for(i = 0; i < n; i++) {
        unsigned char c = in[i];
        struct tn_buf *seen = tnc_remote_echo(t) ? (struct tn_buf *)0 : echo;
        if(t->terminal && c == TNC_ESCAPE) {
            t->escaped = 1;
            return i + 1;
        }
        if(!tnc_by_key(t)) {                    /* a line at a time, edited here */
            int r = tnc_edit_key(&t->edit, c, seen);
            if(r == TNC_E_LINE) {
                send_line(t, net);
                send_return(t, net);
            } else if(r == TNC_E_FULL) {
                send_line(t, net);
            } else if(r == TNC_E_INTR && t->speaks) {
                unsigned char ip[2];
                ip[0] = TN_IAC;
                ip[1] = TN_IP;
                whole(net, ip, 2);
            }
            continue;
        }
        if(c == '\n' && t->key_cr) {
            t->key_cr = 0;
            continue;
        }
        t->key_cr = c == '\r';
        if(c == '\r' || c == '\n') {
            send_return(t, net);
            say(seen, "\r\n");
            continue;
        }
        send_byte(net, c);
        if(c == 0x7f || c == 0x08) say(seen, "\b \b");
        else if(c >= 0x20 || c == '\t') put(seen, c);
    }
    return n;
}
