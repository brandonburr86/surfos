/*
SurfOS telnet server: telnetd
--------------------
File: telnet.c  Date: 10/6/26
--------------------
The telnet protocol (telnet.h): a reader for what the client sends, a writer for what
the session prints, and the options as RFC 1143 decides them. No system calls and no
library calls, so the host tests run this file as it is.
*/

#include "telnet.h"

enum { S_DATA, S_IAC, S_OPTION, S_SB, S_SB_IAC };

static void put(struct tn_buf *b, unsigned char c) {
    if(b->len < b->size) b->p[b->len++] = c;
}

static void put3(struct tn_buf *b, int verb, int opt) {
    put(b, TN_IAC);
    put(b, (unsigned char)verb);
    put(b, (unsigned char)opt);
}

void telnet_init(struct telnet *t) {
    unsigned char *p = (unsigned char *)t;
    unsigned int i;
    for(i = 0; i < sizeof(*t); i++) p[i] = 0;
    t->state = S_DATA;
    t->sb_opt = -1;
}

/* what the server does on its side: echo the client's keys, no go-aheads */
static int we_may(int opt) {
    return opt == TO_ECHO || opt == TO_SGA;
}

/* what the client may do on its side: no go-aheads, tell the window size */
static int he_may(int opt) {
    return opt == TO_SGA || opt == TO_NAWS;
}

int telnet_opening(struct telnet *t, unsigned char *out) {
    struct tn_buf b;
    b.p = out;
    b.len = 0;
    b.size = TN_OPENING;
    if(t->us[TO_ECHO] == TN_NO) { t->us[TO_ECHO] = TN_WANTYES; put3(&b, TN_WILL, TO_ECHO); }
    if(t->us[TO_SGA] == TN_NO) { t->us[TO_SGA] = TN_WANTYES; put3(&b, TN_WILL, TO_SGA); }
    if(t->him[TO_SGA] == TN_NO) { t->him[TO_SGA] = TN_WANTYES; put3(&b, TN_DO, TO_SGA); }
    if(t->him[TO_NAWS] == TN_NO) { t->him[TO_NAWS] = TN_WANTYES; put3(&b, TN_DO, TO_NAWS); }
    return b.len;
}

/* WILL, WONT, DO or DONT for opt (RFC 1143, without the queue: the server never changes
   its mind). An answer goes out only when the option's state changes. */
static void option(struct telnet *t, int verb, int opt, struct tn_buf *reply) {
    unsigned char *s = verb == TN_WILL || verb == TN_WONT ? &t->him[opt] : &t->us[opt];
    switch(verb) {
    case TN_WILL:                               /* the client offers, or accepts our DO */
    case TN_DO:                                 /* the client asks us, or accepts our WILL */
        if(*s == TN_NO) {
            int yes = verb == TN_WILL ? he_may(opt) : we_may(opt);
            if(yes) *s = TN_YES;
            put3(reply, verb == TN_WILL ? (yes ? TN_DO : TN_DONT) : (yes ? TN_WILL : TN_WONT), opt);
        } else if(*s == TN_WANTYES) {
            *s = TN_YES;                        /* our offer or request accepted */
        } else if(*s == TN_WANTNO) {
            *s = TN_NO;                         /* an answer that contradicts us: NO (RFC 1143) */
        }
        break;
    case TN_WONT:                               /* the client refuses or stops */
    case TN_DONT:
        if(*s == TN_YES) {
            *s = TN_NO;
            put3(reply, verb == TN_WONT ? TN_DONT : TN_WONT, opt);
        } else if(*s == TN_WANTYES || *s == TN_WANTNO) {
            *s = TN_NO;
        }
        break;
    }
}

/* the sub-negotiation is complete: NAWS gives the window (two 16-bit numbers, big endian) */
static void sub_done(struct telnet *t) {
    if(t->sb_opt == TO_NAWS && t->sb_len == 4 && !t->sb_over) {
        int cols = t->sb[0] << 8 | t->sb[1], rows = t->sb[2] << 8 | t->sb[3];
        if(cols > 0 && rows > 0 && (cols != t->cols || rows != t->rows)) {   /* 0: the client does not know */
            t->cols = cols;
            t->rows = rows;
            t->resized = 1;
        }
    }
    t->sb_opt = -1;
}

static void sub_byte(struct telnet *t, unsigned char c) {
    if(t->sb_opt < 0) t->sb_opt = c;            /* the option comes first */
    else if(t->sb_len < TN_SB_MAX) t->sb[t->sb_len++] = c;
    else t->sb_over = 1;
}

/* IAC c */
static void command(struct telnet *t, unsigned char c, struct tn_buf *data, struct tn_buf *reply) {
    const char *s;
    t->state = S_DATA;
    switch(c) {
    case TN_IAC:                                /* IAC IAC: the data byte 255 */
        t->cr = 0;
        put(data, TN_IAC);
        break;
    case TN_WILL:
    case TN_WONT:
    case TN_DO:
    case TN_DONT:
        t->verb = c;
        t->state = S_OPTION;
        break;
    case TN_SB:
        t->state = S_SB;
        t->sb_opt = -1;
        t->sb_len = t->sb_over = 0;
        break;
    case TN_IP:                                 /* interrupt process, break: Ctrl+C */
    case TN_BRK:
        t->cr = 0;
        put(data, 0x03);
        break;
    case TN_AYT:
        for(s = TN_AYT_ANSWER; *s; s++) put(reply, (unsigned char)*s);
        break;
    default:                                    /* NOP, DM, GA, EC, EL, AO, a stray SE */
        break;
    }
}

void telnet_recv(struct telnet *t, const unsigned char *in, int n, struct tn_buf *data, struct tn_buf *reply) {
    int i;
    for(i = 0; i < n; i++) {
        unsigned char c = in[i];
        switch(t->state) {
        case S_DATA:
            if(c == TN_IAC) {
                t->state = S_IAC;
            } else if(t->cr && (c == '\n' || c == 0)) {
                t->cr = 0;                      /* CR LF, CR NUL: Return, a CR alone */
            } else {
                t->cr = c == '\r';
                put(data, c);
            }
            break;
        case S_IAC:
            command(t, c, data, reply);
            break;
        case S_OPTION:
            t->state = S_DATA;
            option(t, t->verb, c, reply);
            break;
        case S_SB:
            if(c == TN_IAC) t->state = S_SB_IAC;
            else sub_byte(t, c);
            break;
        case S_SB_IAC:
            if(c == TN_SE) {
                t->state = S_DATA;
                sub_done(t);
            } else if(c == TN_IAC) {
                t->state = S_SB;
                sub_byte(t, TN_IAC);            /* IAC IAC inside: the byte 255 */
            } else {
                t->sb_opt = -1;                 /* a sub-negotiation cut short: dropped, and */
                command(t, c, data, reply);     /* the command after its IAC counts */
            }
            break;
        }
    }
}

int telnet_send(struct telnet *t, const unsigned char *in, int n, unsigned char *out) {
    int i, k = 0;
    for(i = 0; i < n; i++) {
        unsigned char c = in[i];
        if(c == '\n' && !t->out_cr) out[k++] = '\r';
        if(c == TN_IAC) out[k++] = TN_IAC;
        out[k++] = c;
        t->out_cr = c == '\r';
    }
    return k;
}
