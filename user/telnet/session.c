/*
SurfOS telnet client: telnet
--------------------
File: session.c   Date: 11/01/26
--------------------
The session's loop (tnsession.h). Everything the server sends is hostile input, and two things
may stop: the screen, which a write waits for, and the server's reading, which nothing waits
for. What goes to the server waits in a buffer of ours and leaves when poll() says POLLOUT, so a
server that stops reading never stops the loop. No more is read from it while that buffer may
not hold the answers, which bounds a flood of negotiations: the server's own window fills
instead. The keys are read all the time, for Ctrl+] is the way out of just such a session: while
the server takes nothing, keys are dropped (it is said once) and only Ctrl+] counts.

The prompt of Ctrl+] waits for its line and looks at nothing else: the server's bytes wait for
it in the connection.

The input's end (a pipe's): what is left goes out, then the session waits for the server to close
and ends after QUIET_MS with no byte either way, as a server need not close for a client that has
said all it will, and nobody is there to press Ctrl+].
*/

#include <surf.h>
#include "tnclient.h"
#include "tnsession.h"

#define PIECE       1024                /* read at a time, either way */
#define NET_OUT     (2 * TNC_ROOM(PIECE))
#define WINDOW_MS   1000                /* how often the window's size is looked at: it comes with no signal */
#define QUIET_MS    2000

static const struct tn_session *ses;
static struct tnc t;
static unsigned char net_out[NET_OUT];
static int out_off, out_len;            /* what waits to go to the server */
static unsigned char piece[PIECE], screen[PIECE], echoed[TNC_ECHO_ROOM(PIECE)];
static unsigned long bytes_in, bytes_out;
static int warned;                      /* the keys being dropped has been said */

static int room(void) {
    return NET_OUT - (out_len - out_off);
}

/* the room left, as a tn_buf; given back with used() */
static void net_buf(struct tn_buf *b) {
    if(out_off && out_off == out_len) out_off = out_len = 0;
    if(out_off) {
        memmove(net_out, net_out + out_off, (size_t)(out_len - out_off));
        out_len -= out_off;
        out_off = 0;
    }
    b->p = net_out + out_len;
    b->len = 0;
    b->size = NET_OUT - out_len;
}

static void used(const struct tn_buf *b) {
    out_len += b->len;
}

static int screen_write(const void *p, int n) {
    const char *c = (const char *)p;
    while(n > 0) {
        ssize_t w = write(ses->out, c, (size_t)n);
        if(w < 0 && errno == EINTR) continue;
        if(w <= 0) return -1;
        c += w;
        n -= (int)w;
    }
    return 0;
}

static int screen_say(const char *s) {
    return screen_write(s, (int)strlen(s));
}

/* the window, told when it changed */
static void window(void) {
    struct tn_buf b;
    int r;
    if(!ses->terminal || (r = ttyctl(TTY_GETSIZE, 0)) <= 0) return;
    if(room() < TNC_NAWS_MAX) return;   /* the next look tells it */
    net_buf(&b);
    tnc_size(&t, r >> 16 & 0xffff, r & 0xffff, &b);
    used(&b);
}

static void status(void) {
    char line[160];
    snprintf(line, sizeof(line), "Connected to %s\n", ses->where);
    screen_say(line);
    snprintf(line, sizeof(line), "The %s echoes what is typed; keys go %s\n", tnc_remote_echo(&t) ? "other end" : "this end",
             tnc_by_key(&t) ? "one at a time" : "a line at a time");
    screen_say(line);
    if(t.us[TO_NAWS] == TN_YES) snprintf(line, sizeof(line), "The other end knows the window: %d by %d\n", t.told_cols, t.told_rows);
    else snprintf(line, sizeof(line), "The other end does not ask for the window (%d by %d)\n", t.cols, t.rows);
    screen_say(line);
    snprintf(line, sizeof(line), "Binary: sending %s, receiving %s\n", t.us[TO_BINARY] == TN_YES ? "yes" : "no",
             t.him[TO_BINARY] == TN_YES ? "yes" : "no");
    screen_say(line);
    snprintf(line, sizeof(line), "%lu bytes received, %lu sent, %d waiting to go\n", bytes_in, bytes_out, out_len - out_off);
    screen_say(line);
}

/* what the line at the prompt says: 1 the session goes on, 0 quit, -1 another line is wanted */
static int prompt_line(struct tnc_edit *e) {
    char *s = (char *)e->text;
    int n = e->len;
    struct tn_buf b;
    while(n && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    s[n < TNC_LINE_MAX ? n : TNC_LINE_MAX - 1] = 0;
    while(*s == ' ' || *s == '\t') s++;
    if(!*s) return 1;
    if(!strcmp(s, "quit")) return 0;
    if(!strcmp(s, "status")) {
        status();
        return 1;
    }
    if(!strcmp(s, "send ayt")) {
        if(room() < 2) screen_say("telnet: the other end takes nothing\n");
        else {
            net_buf(&b);
            tnc_ayt(&t, &b);
            used(&b);
        }
        return 1;
    }
    screen_say("quit leaves, status says how things stand, send ayt asks if the other end is there; "
               "an empty line goes back\n");
    return -1;
}

/* Ctrl+] came: the prompt, with what was typed after it in ahead (n bytes), of which *used were its own: the rest
   came after its line, and goes on to the session. 1 the session goes on, 0 quit */
static int prompt(const unsigned char *ahead, int n, int *used) {
    static struct tnc_edit e;
    int off = 0;
    memset(&e, 0, sizeof(e));
    if(screen_say(t.at_line_start ? "telnet> " : "\r\ntelnet> ") < 0) return 0;
    t.at_line_start = 1;
    for(;;) {
        unsigned char c;
        struct tn_buf b;
        unsigned char show[8];
        int r;
        if(off < n) c = ahead[off++];
        else {
            ssize_t m = read(ses->in, &c, 1);
            if(m < 0 && errno == EINTR) continue;
            if(m <= 0) return 0;            /* the terminal has gone */
        }
        b.p = show;
        b.len = 0;
        b.size = sizeof(show);
        r = tnc_edit_key(&e, c, &b);
        *used = off;
        if(b.len && screen_write(show, b.len) < 0) return 0;
        if(r == TNC_E_INTR) return 1;
        if(r == TNC_E_LINE || r == TNC_E_FULL) {
            r = prompt_line(&e);
            if(r >= 0) return r;
            memset(&e, 0, sizeof(e));
            if(screen_say("telnet> ") < 0) return 0;
        }
    }
}

/* m keys in piece: 1 go on, 0 quit, -1 the screen failed */
static int take_keys(int m) {
    int off = 0;
    while(off < m) {
        if(room() < TNC_KEYS_ROOM(m - off)) {   /* the server takes nothing: only Ctrl+] counts */
            int i;
            for(i = off; i < m && !(ses->terminal && piece[i] == TNC_ESCAPE); i++);
            if(i == m) {
                if(!warned) {
                    warned = 1;
                    if(screen_say("\r\ntelnet: the other end takes nothing: keys are dropped (Ctrl+] then quit leaves)\r\n") < 0)
                        return -1;
                    t.at_line_start = 1;
                }
                return 1;
            }
            off = i + 1;
        } else {
            struct tn_buf net, echo;
            net_buf(&net);
            echo.p = echoed;
            echo.len = 0;
            echo.size = sizeof(echoed);
            off += tnc_keys(&t, piece + off, m - off, &net, &echo);
            used(&net);
            if(echo.len) {
                if(screen_write(echoed, echo.len) < 0) return -1;
                t.at_line_start = echoed[echo.len - 1] == '\n';
            }
            if(!t.escaped) continue;
            t.escaped = 0;
        }
        {
            int used = 0, r = prompt(piece + off, m - off, &used);
            if(!r) return 0;
            off += used;
        }
    }
    return 1;
}

/* what the server sent: n bytes in piece */
static int take_net(int n) {
    struct tn_buf term, net;
    term.p = screen;
    term.len = 0;
    term.size = sizeof(screen);
    net_buf(&net);
    tnc_recv(&t, piece, n, &term, &net);
    used(&net);
    return term.len ? screen_write(screen, term.len) : 0;
}

/* the end: the terminal as it was */
static int finish(int why) {
    char undo[TNC_RESTORE_MAX];
    int n = tnc_restore(&t, undo), e = errno;
    if(n && why != TNS_SCREEN) screen_write(undo, n);
    errno = e;
    return why;
}

int tn_session_run(const struct tn_session *s) {
    struct tn_buf b;
    int keys_open = 1;
    unsigned int quiet_since = 0;
    ses = s;
    out_off = out_len = warned = 0;
    bytes_in = bytes_out = 0;
    {
        int r = s->terminal ? ttyctl(TTY_GETSIZE, 0) : 0;
        tnc_init(&t, r > 0 ? r >> 16 & 0xffff : 0, r > 0 ? r & 0xffff : 0, s->terminal);
    }
    if(s->speak_first) {
        net_buf(&b);
        tnc_opening(&t, &b);
        used(&b);
    }
    for(;;) {
        struct pollfd p[2];
        int n = 0, ki = -1, ni, r, timeout = -1, waiting, reading;
        window();
        waiting = out_len - out_off;
        reading = room() >= TNC_ROOM(PIECE);
        if(keys_open) {
            p[n].fd = s->in;
            p[n].events = POLLIN;
            p[n].revents = 0;
            ki = n++;
        } else {
            int left = QUIET_MS - (int)(uptime() - quiet_since);
            if(left <= 0) return finish(TNS_ENDED);
            timeout = left;
        }
        if(timeout < 0 && t.us[TO_NAWS] == TN_YES && s->terminal) timeout = WINDOW_MS;
        p[n].fd = s->sock;
        p[n].events = (short)((reading ? POLLIN : 0) | (waiting ? POLLOUT : 0));
        p[n].revents = 0;
        ni = n++;
        r = poll(p, (nfds_t)n, timeout);
        if(r < 0) {
            if(errno == EINTR) continue;        /* a kill ends this task when the call does */
            return finish(TNS_LOST);
        }
        if(ki >= 0 && p[ki].revents) {
            ssize_t m = read(s->in, piece, sizeof(piece));
            if(m < 0 && errno != EINTR) return finish(TNS_LOST);
            if(m == 0) {
                if(s->terminal) return finish(TNS_ENDED);
                keys_open = 0;
                quiet_since = uptime();
            } else if(m > 0) {
                r = take_keys((int)m);
                if(r < 0) return finish(TNS_SCREEN);
                if(!r) return finish(TNS_QUIT);
            }
        }
        if((p[ni].revents & POLLOUT) && out_len > out_off) {
            ssize_t w = write(s->sock, net_out + out_off, (size_t)(out_len - out_off));
            if(w < 0 && errno != EINTR) return finish(TNS_LOST);
            if(w > 0) {
                out_off += (int)w;
                bytes_out += (unsigned long)w;
                quiet_since = uptime();
            }
        }
        /* POLLHUP or POLLERR say so whether asked or not, and read() is how to learn what: what waits for
           a server that has gone waits for nothing, and leaving it there would have poll() answer at once for ever */
        if((p[ni].revents & (POLLHUP | POLLERR | POLLNVAL)) && !reading) out_off = out_len = 0;
        if(reading && (p[ni].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
            ssize_t m = read(s->sock, piece, sizeof(piece));
            if(m < 0 && errno == EINTR) continue;
            if(m < 0) return finish(TNS_LOST);
            if(m == 0) return finish(TNS_CLOSED);
            bytes_in += (unsigned long)m;
            quiet_since = uptime();
            if(take_net((int)m) < 0) return finish(TNS_SCREEN);
        }
    }
}
