/*
SurfOS telnet client: telnet
--------------------
File: tnclient.h  Date: 11/01/26
--------------------
The client's side of the telnet protocol (RFC 854) as a module with neither system calls nor
library calls, as telnetd's telnet.c is: session.c links it, and tests/host/test_telnetc.c runs
the same file on the host, with telnetd's own module at the other end. The numbers and struct
tn_buf are telnetd's (telnet.h).

From the server (tnc_recv): the data for the terminal, and what to send back. A command may be
split across reads at any byte. IAC IAC is a data byte 255; every other command (AYT, IP, GA,
NOP, DM and the like) changes nothing here: nothing a server sends makes this end speak but
option negotiation. While the server does not send in binary mode, CR NUL is a CR and a NUL
alone is nothing (it is NVT's no-op); CR LF stays as it is, for the terminal. A sub-negotiation
is read to its end and dropped, however long: nothing here takes one. Options follow RFC 1143,
so two ends cannot negotiate in a loop, and an answer goes out only when an option changes.
This end lets the server do ECHO, SUPPRESS-GO-AHEAD and BINARY, and does SUPPRESS-GO-AHEAD,
BINARY and NAWS itself; everything else is refused, each time it is asked. ECHO on this end is
refused above all: two ends echoing each other would never stop.

Who speaks first: on port 23 this end offers its options at once (tnc_opening()). On another
port the server may be no telnet server (mail, a web server talked to by hand): this end is
silent until the server's first negotiation, and then makes the same offers.

From the keyboard (tnc_keys): Return is CR LF, or a CR alone once this end sends in binary
mode (telnetd takes either); an LF right after a CR is the end of that Return, and an LF alone
(a pipe's line end) is a Return too. A byte 255 goes as IAC IAC. Ctrl+] (TNC_ESCAPE) stops
there on a terminal: it belongs to the caller, and is never sent. RFC 854's terminal has a go-
ahead and a line at a time: until the server has agreed to SUPPRESS-GO-AHEAD, keys wait in a
line being edited here (Backspace, Ctrl+U, Ctrl+C), which goes out at its Return or when that
agreement comes; once it has, each key goes as it is typed. While the server does not ECHO,
this end shows what is typed: the terminal is in raw mode and shows nothing by itself. A
sequence of a special key is dropped in a line being edited, as the server would take it for
text.

The terminal's look (tnc_restore): a server may leave its terminal on the alternate screen, with
the cursor hidden, in colours. What the session printed is watched for those, and the bytes
that undo them are given at the end, whatever way it ended. Hence a server gets no way to
leave the shell's terminal unusable, and the last words of telnet appear on the main screen.

Buffers: tnc_recv() writes at most n bytes to the terminal and, per TNC_ROOM(), answers that
fit; tnc_keys() writes at most TNC_KEYS_ROOM(n) to the network and TNC_ECHO_ROOM(n) to the
screen. The session leaves that room, so no answer is lost; an answer that does not fit goes
whole or not at all, as telnetd's.
*/

#ifndef _TNCLIENT_H
#define _TNCLIENT_H

#include "../telnetd/telnet.h"

#define TNC_ESCAPE    0x1d          /* Ctrl+]: the small prompt */
#define TNC_LINE_MAX  1024          /* a line edited here; a longer one goes out in pieces */
#define TNC_OPENING   15            /* bytes tnc_opening() writes at most */
#define TNC_NAWS_MAX  13            /* IAC SB NAWS, four bytes each doubled, IAC SE */
#define TNC_RESTORE_MAX 32          /* bytes tnc_restore() writes at most */

/* room for the answers to n bytes from the server: a command of 3 bytes gets 3 back and a window
   size besides, a command split across reads counts in its last byte, and the first negotiation
   brings this end's offers; and a line being edited goes out when the server's agreement comes */
#define TNC_ROOM(n)       (6 * (n) + TNC_OPENING + TNC_NAWS_MAX + 3 + TNC_KEYS_ROOM(0))
/* room for what n keys send: each key doubled at worst, and a whole line with its Return */
#define TNC_KEYS_ROOM(n)  (2 * (n) + 2 * TNC_LINE_MAX + 8)
/* room for what n keys show: Backspace's "\b \b", Ctrl+U's "^U\r\n" */
#define TNC_ECHO_ROOM(n)  (4 * (n) + 8)

enum { TNC_E_MORE, TNC_E_LINE, TNC_E_FULL, TNC_E_INTR };

/* a line being typed, here or at the prompt of Ctrl+] */
struct tnc_edit {
    unsigned char text[TNC_LINE_MAX];
    int len;
    int esc;                        /* inside the sequence of a special key */
    int cr;                         /* the last key was a CR: an LF right after it is that key's end */
};

struct tnc {
    int state;                      /* where the reader is: data, after IAC, an option, a sub-negotiation */
    int verb;                       /* WILL, WONT, DO or DONT, waiting for its option */
    int cr;                         /* the last data byte was CR: a NUL right after it goes */
    int speaks;                     /* this end has made its offers (tnc_opening(), or the server's first negotiation) */
    int key_cr;                     /* the last key was a CR: an LF right after it goes */
    int terminal;                   /* keys come from a terminal: Ctrl+] is the caller's */
    int escaped;                    /* tnc_keys() stopped at Ctrl+] */
    unsigned char us[256];          /* each option on this end's side (WILL/WONT): TN_NO ... */
    unsigned char him[256];         /* each option on the server's side (DO/DONT) */
    int cols, rows;                 /* the window */
    int told_cols, told_rows;       /* what NAWS said last; 0 until it has */
    struct tnc_edit edit;           /* the line waiting for the server's agreement */
    int look;                       /* the watch on the terminal's look: where it is in a sequence */
    int look_num, look_mark, look_more;   /* the number so far; '?' came; a ';' came */
    int alt, cursor_off, colours;   /* what the session left on */
    int at_line_start;              /* what was printed ends with a line end */
};

void tnc_init(struct tnc *t, int cols, int rows, int terminal);

/* this end's offers, for a connection to port 23 */
void tnc_opening(struct tnc *t, struct tn_buf *net);

/* n bytes from the server: data into term (at most n), answers into net */
void tnc_recv(struct tnc *t, const unsigned char *in, int n, struct tn_buf *term, struct tn_buf *net);

/* n keys: what they send into net, what they show into echo; how many were taken. Stops after
   Ctrl+] when t->terminal, with t->escaped set: the caller clears it */
int tnc_keys(struct tnc *t, const unsigned char *in, int n, struct tn_buf *net, struct tn_buf *echo);

/* the window's size, told to the server when NAWS is on and it changed */
void tnc_size(struct tnc *t, int cols, int rows, struct tn_buf *net);

/* IAC AYT: the server's answer comes as data */
void tnc_ayt(struct tnc *t, struct tn_buf *net);

/* the bytes that undo what the session left on the terminal, into out (TNC_RESTORE_MAX): their
   count, and the terminal counts as restored */
int tnc_restore(struct tnc *t, char *out);

/* does the server echo? does it take keys one at a time? */
int tnc_remote_echo(const struct tnc *t);
int tnc_by_key(const struct tnc *t);

/* a key for a line: Backspace (DEL or BS), Ctrl+U, Ctrl+C (TNC_E_INTR), Return (TNC_E_LINE; the
   line is e->text, e->len bytes, and the caller clears it), the room used up (TNC_E_FULL, the key
   kept). echo, which may be NULL, gets what the key shows */
int tnc_edit_key(struct tnc_edit *e, unsigned char c, struct tn_buf *echo);

#endif
