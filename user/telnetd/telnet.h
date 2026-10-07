/*
SurfOS telnet server: telnetd
--------------------
File: telnet.h  Date: 10/6/26
--------------------
The telnet protocol (RFC 854) as telnetd speaks it, as a module with neither system
calls nor library calls: telnetd's server links it, and tests/host/test_telnet.c runs
the same file on the host.

From the client (telnet_recv): the data for the terminal, and what to send back.
Commands start with IAC (255) and may be split across reads at any byte. IAC IAC is a
data byte 255; IAC IP and IAC BRK become Ctrl+C (3), IAC EC the Backspace key (DEL,
127), IAC EL Ctrl+U (21, which clears the line); IAC AYT is answered at once; the other
commands (NOP, DM, GA, AO) change nothing. CR LF and CR NUL are a CR alone, as a
terminal's Return key. Options follow RFC 1143: the server will do ECHO and
SUPPRESS-GO-AHEAD and wants the client to do SUPPRESS-GO-AHEAD and NAWS (its window
size, "negotiate about window size"). Every other option is refused (WONT, DONT) each
time the client asks for it; a request for what is so already, and the client's answer
to the server's own request, get no answer, so two ends cannot negotiate in a loop. A
NAWS sub-negotiation of 4 bytes sets cols and rows (0 means unknown) and raises resized;
one of another length changes nothing. Each answer goes into reply whole or not at all:
a client that sends faster than it reads loses answers, never a part of one.

To the client (telnet_send): LF becomes CR LF (a CR LF that is there already stays one),
a CR followed by anything else CR NUL, and a data byte 255 IAC IAC. Whether a CR at the
end of what was printed so far is CR LF or CR NUL, the next byte says: the next call's,
or the NUL comes before the next answer to the client.

telnet_opening(): the server's first bytes: WILL ECHO, WILL SUPPRESS-GO-AHEAD,
DO SUPPRESS-GO-AHEAD, DO NAWS. The client then echoes nothing itself and sends each key
as it is typed, which surfsh's line editor needs.
*/

#ifndef _TELNET_H
#define _TELNET_H

#define TN_SE    240                /* the end of a sub-negotiation */
#define TN_NOP   241
#define TN_DM    242                /* data mark */
#define TN_BRK   243                /* break */
#define TN_IP    244                /* interrupt process */
#define TN_AO    245                /* abort output */
#define TN_AYT   246                /* are you there */
#define TN_EC    247                /* erase character */
#define TN_EL    248                /* erase line */
#define TN_GA    249                /* go ahead */
#define TN_SB    250                /* a sub-negotiation begins */
#define TN_WILL  251
#define TN_WONT  252
#define TN_DO    253
#define TN_DONT  254
#define TN_IAC   255                /* interpret as command */

#define TO_ECHO  1
#define TO_SGA   3                  /* suppress go ahead */
#define TO_NAWS  31                 /* negotiate about window size (RFC 1073) */

#define TN_NO      0                /* an option's state, each side (RFC 1143) */
#define TN_YES     1
#define TN_WANTNO  2
#define TN_WANTYES 3

#define TN_SB_MAX     32            /* bytes of a sub-negotiation kept (NAWS has 4) */
#define TN_OPENING    12            /* bytes telnet_opening() writes */
#define TN_AYT_ANSWER "\r\n[SurfOS: yes]\r\n"

struct telnet {
    int state;                      /* where the reader is: data, after IAC, an option, a sub-negotiation */
    int verb;                       /* WILL, WONT, DO or DONT, waiting for its option */
    int cr;                         /* the last data byte was CR: an LF or NUL right after it goes */
    int sb_opt, sb_len, sb_over;    /* the sub-negotiation being read: its option, bytes, too many */
    unsigned char sb[TN_SB_MAX];
    unsigned char us[256];          /* each option on the server's side (WILL/WONT): TN_NO ... */
    unsigned char him[256];         /* each option on the client's side (DO/DONT) */
    int cols, rows;                 /* the client's window (NAWS); 0 until it says */
    int resized;                    /* NAWS changed cols or rows: the caller acts, then clears it */
    int out_cr;                     /* the session's output so far ends in a CR (its LF or NUL comes next) */
};

/* bytes, as far as the room goes: what is left out is dropped */
struct tn_buf {
    unsigned char *p;
    int len, size;
};

void telnet_init(struct telnet *t);

/* the server's first negotiation into out (TN_OPENING bytes at most); its length */
int telnet_opening(struct telnet *t, unsigned char *out);

/* n bytes from the client: data for the terminal into data (never more than n), answers
   for the client into reply */
void telnet_recv(struct telnet *t, const unsigned char *in, int n, struct tn_buf *data, struct tn_buf *reply);

/* n bytes the session printed, for the client, into out (2 * n + 1 bytes at most: the +1 is the
   NUL after a CR that ended the call before); its length */
int telnet_send(struct telnet *t, const unsigned char *in, int n, unsigned char *out);

#endif
