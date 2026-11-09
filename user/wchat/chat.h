/*
SurfOS chat client: wchat
--------------------
File: chat.h    Date: 11/05/26
--------------------
The Chat's model: one connection to an IRC server, the rooms it shows (the server's own, the
channels, the private queries), their lines with the time they came, their nick lists, the
registration with CAP and SASL PLAIN, what a line typed does, and what goes to the server and
when. chat.c holds the rooms and the connection, recv.c what the server sends, input.c what is
typed. None of them makes a system call: main.c hands in the time (now, minute) before each
call, gives the server's bytes to chat_recv() and sends what chat_send() gives, so
tests/host/test_wchat.c runs the model on the host as it is, against hostile servers.

Everything a server sends is hostile. Its lines are bounded (irc.h), its text cleaned
(ircfmt.h), and every list it can make grow has a limit below: rooms, nicks, lines, bytes. A
room a server would add past a limit is not made, and what it says for that room is shown in
the server's room instead. A nick, a channel or a user name from a server is used in a line
going out only after irc_valid_nick() or irc_valid_channel() took it, and every line going out
is put together by irc_compose().

The password, for the server (PASS) or for SASL PLAIN, is used once: it goes into the line that
carries it, and then the copy here and the line are wiped, as is anything left of it when the
connection ends. It is never shown, logged or written anywhere: a reconnection asks for it again.

What goes out is paced as RFC 1459 section 8.10 says a server counts a client's lines: two
seconds a line, and nothing while that count is eight seconds ahead of the clock, so a long
paste never gets the client thrown out for flooding. Registration, PONG and QUIT jump the
queue: a PONG behind a paste would come too late.
*/

#ifndef _WCHAT_CHAT_H
#define _WCHAT_CHAT_H

#include "irc.h"
#include "ircfmt.h"

#define CHAT_ROOMS      64              /* the server's room, channels and queries together */
#define CHAT_NICK_MAX   64              /* a nick's bytes */
#define CHAT_CHAN_MAX   200             /* a channel's name */
#define CHAT_HOST_MAX   253             /* a server's name (a DNS name) */
#define CHAT_PASS_MAX   256             /* a password's bytes */
#define CHAT_LINES      2000            /* a room's lines kept: the oldest go */
#define CHAT_BYTES      (8L << 20)      /* the lines of every room together */
#define CHAT_NICKS      8192            /* a channel's list */
#define CHAT_NICKS_ALL  65536           /* every channel's list together */
#define CHAT_HISTORY    64              /* lines typed, kept for Up and Down (the input field's, gk_field_history()) */
#define CHAT_INPUT_MAX  2048            /* a line typed */
#define CHAT_QUEUE      128             /* lines waiting to go */
#define CHAT_URGENT     16              /* of them, registration, PONG and QUIT, which go first */
#define CHAT_PREFIXES   16              /* ISUPPORT PREFIX's modes */
#define CHAT_JOINING    8               /* channels /join asked for, whose rooms come in front */
#define CHAT_NO_TIME    0xffff          /* a line's minute when the clock was not known */

#define CHAT_LINE_MS    2000            /* RFC 1459 section 8.10: what a line costs */
#define CHAT_BURST_MS   8000            /* nothing goes while the count is this far ahead */
#define CHAT_IDLE_MS    90000           /* the server said nothing: a PING */
#define CHAT_PING_MS    60000           /* no answer to it: the connection is lost */
#define CHAT_CAP_MS     30000           /* no answer to CAP LS: CAP END */
#define CHAT_QUIT_MS    3000            /* after QUIT: the server's close waited for */

enum { CHAT_OFF, CHAT_CONNECTING, CHAT_REGISTERING, CHAT_ONLINE, CHAT_QUITTING };
enum { ROOM_SERVER, ROOM_CHANNEL, ROOM_QUERY };
enum { CHAT_PASS_NONE, CHAT_PASS_SERVER, CHAT_PASS_SASL };

/* a line's kind, which the window colours */
enum { LN_MSG, LN_ACTION, LN_NOTICE, LN_EVENT, LN_INFO, LN_ERROR };
#define LF_SELF      1                  /* ours */
#define LF_HIGHLIGHT 2                  /* it names our nick, or it is a private message */

/* what changed since the window last drew (chat.dirty) */
#define CHAT_DIRTY_ROOMS  1             /* the list of rooms: names, marks */
#define CHAT_DIRTY_LINES  2             /* the active room's lines */
#define CHAT_DIRTY_NICKS  4             /* the active room's nick list */
#define CHAT_DIRTY_TOPIC  8             /* the active room's topic */
#define CHAT_DIRTY_STATUS 16            /* the connection */
#define CHAT_DIRTY_ALL    31

struct chat_line {
    unsigned short minute;              /* of the day, from the PC's clock; CHAT_NO_TIME */
    unsigned char kind, flags;
    unsigned long seq;                  /* the room's count of lines before it: what the view keeps its place by */
    char *from;                         /* a nick, "" for none */
    char *text;                         /* cleaned, the formatting codes kept */
};

struct chat_nick {
    char *name;
    unsigned modes;                     /* bit i: PREFIX's i-th mode, the 0th the highest (o in (ov)@+) */
};

struct chat_room {
    unsigned id;                        /* its serial number: what the window keeps its tabs by */
    int kind;
    char name[CHAT_CHAN_MAX + 1];       /* a channel's or a query's; the server's room's is "" */
    char label[3 * CHAT_CHAN_MAX + 1];  /* the name to show: controls, invisible marks and codes out (11/09/26) */
    int joined;                         /* a channel we are in */
    int parting;                        /* a channel our /part left: its PART closes the room */
    char *topic;                        /* NULL for none */
    struct chat_line **ring;            /* CHAT_LINES of them, made with the first line */
    int first, count;
    unsigned long seq;                  /* lines added since the room was made */
    long bytes;                         /* what its lines take */
    struct chat_nick *nicks;            /* sorted by name, under the case mapping */
    int nnicks, nicks_room;
    int names_done;                     /* the list after our JOIN came whole (366) */
    int nicks_over;                     /* a nick did not fit: the list is not all of them */
    int unread;                         /* lines since it was last the active room */
    int highlight;                      /* one of them has LF_HIGHLIGHT */
};

/* the connect dialog's fields */
struct chat_params {
    char host[CHAT_HOST_MAX + 1];
    unsigned port;
    int tls;
    char nick[CHAT_NICK_MAX + 1];
    int pass_kind;                      /* CHAT_PASS_* */
    char pass[CHAT_PASS_MAX + 1];
    int plain_ok;                       /* the password may go without TLS: the person said so, asked (11/09/26) */
};

struct chat {
    unsigned now;                       /* milliseconds since boot, from main.c before each call */
    int minute;                         /* the PC's clock's minute of the day, -1 when not known */
    int state;                          /* CHAT_OFF ... */
    struct chat_params p;               /* what was asked for; the password wiped once used */
    char tls[16];                       /* "TLSv1.3" once connected, "" over plain TCP */
    char nick[CHAT_NICK_MAX + 1];       /* ours, as the server has it */
    char user[CHAT_HOST_MAX + 1];       /* our user@host as the server shows it, "" until known */
    int tries;                          /* other nicks tried at registration */
    int cap, sasl;                      /* the negotiation (chat.c's CAP_*, SASL_*) */
    int sasl_offered, sasl_plain;       /* what CAP LS said */
    unsigned cap_at, last_rx, ping_at, quit_at;   /* when CAP LS went, the server last spoke, our PING went, QUIT */
    int ending;                         /* the connection must end now: main.c closes it, then chat_down() */
    char why[160];                      /* why it ends, when the model ends it */
    /* the server's ISUPPORT, or RFC 2812's defaults */
    int casemap;
    int casemap_set;                    /* this connection's CASEMAPPING came: later ones are ignored (11/09/26) */
    char prefix_modes[CHAT_PREFIXES + 1], prefix_chars[CHAT_PREFIXES + 1];
    char chantypes[9], statusmsg[9];
    char chanmodes[4][33];              /* CHANMODES' A, B, C and D */
    int nicklen;
    char network[33];
    /* the rooms; room[0] is the server's */
    struct chat_room *room[CHAT_ROOMS];
    int rooms, active;
    long bytes;                         /* the lines of every room */
    int nicks_all;
    char joining[CHAT_JOINING][CHAT_CHAN_MAX + 1];
    /* going out */
    char urgent[CHAT_URGENT][IRC_LINE_MAX + 1];
    char queue[CHAT_QUEUE][IRC_LINE_MAX + 1];
    int uhead, ucount, qhead, qcount;
    unsigned send_at;                   /* RFC 1459 section 8.10's count, mirrored */
    unsigned next_id;                   /* the next room's id */
    /* the server's lines */
    struct irc_reader rd;
    unsigned long long_lines, bad_lines;    /* lines dropped: over IRC_IN_MAX; with a NUL or no message */
    unsigned long dropped_told;         /* how many of them a line has said */
    unsigned dropped_at;                /* when it did */
    int show_codes;                     /* formatting shown (1), or left out */
    unsigned dirty;                     /* CHAT_DIRTY_*: main.c clears it when it has drawn */
};

/* the server's room, nothing connected. 0, or -1 when out of memory */
int chat_init(struct chat *c);
/* every room and line let go, the password wiped */
void chat_free(struct chat *c);

/* The connect dialog's fields: 0, or -1 with why not in err (size bytes), a sentence. */
int chat_check(const struct chat_params *p, char *err, int size);
/* A connection asked for (CHAT_CONNECTING, while main.c's helper makes it): 0, or -1 with why not.
   A server other than the last one's rooms go. */
int chat_connect(struct chat *c, const struct chat_params *p, char *err, int size);
void chat_up(struct chat *c, const char *tls);      /* it is made: registration starts */
void chat_down(struct chat *c, const char *why);    /* it ended, or never came: why is shown */
void chat_recv(struct chat *c, const char *in, int n);     /* the server's bytes */
void chat_tick(struct chat *c);                     /* what time does: PING, giving up on CAP or QUIT */
int chat_send(struct chat *c, char *out, int size); /* what may go now, whole lines: its length */
int chat_wait(const struct chat *c);                /* ms until tick or send has something to do; -1: nothing */
/* /quit, or the window closing: QUIT with reason (NULL: none) and what waits dropped; a connection being
   made ends at once (ending) */
void chat_quit(struct chat *c, const char *reason);

/* A line typed in the active room: a message, or a command (/join /part /msg /me /nick /topic /query
   /quit); what went wrong is a line in the room. */
void chat_input(struct chat *c, const char *line);
/* 11/09/26 (the review): does a line typed carry a password, being for NickServ (/msg NickServ IDENTIFY ..., or a line
   in NickServ's query)? Such a line is shown with its first word only, and the window keeps it out of its history */
int chat_secret(const struct chat *c, const char *line);

void chat_select(struct chat *c, int room);         /* the active room: its marks go */
/* the room's close: a channel we are in is left (its room goes with the PART), anything else goes now;
   0, or -1 for the server's room */
int chat_close_room(struct chat *c, int room);

/* for the window */
const struct chat_line *chat_line_at(const struct chat_room *r, int i);    /* 0 the oldest kept */
const char *chat_room_label(const struct chat *c, int room);
char chat_nick_mark(const struct chat *c, const struct chat_nick *n);      /* '@', '+' ..., or ' ' */
/* the room's nicks by rank, then name: their indexes into r->nicks in order (r->nnicks of them) */
void chat_nick_order(const struct chat *c, const struct chat_room *r, int *order);
void chat_status(const struct chat *c, char *out, int size);
void chat_title(const struct chat *c, char *out, int size);
int chat_find(const struct chat *c, int kind, const char *name);          /* a room's index, -1 */

#endif
