/*
SurfOS chat client: wchat
--------------------
File: window.h  Date: 11/05/26
--------------------
The Chat's window (window.c) on the toolkit (gk.h): a form to connect with, and the rooms as tabs
over their messages, the nick list beside them and the input line below, a status bar, menus in
the desktop's bar. Its logic is apart from its loop (main.c) and from the system calls that are
SurfOS's own (proc.c: the helper started, killed and collected, the clock), so that
tests/host/test_wchatwin.c drives it on the fake desktop (tests/host/gk_fake.c), playing the helper
at the other ends of its pipes. The model is chat.h's; the helper's frames are link.h's.
*/
#ifndef _WCHAT_WINDOW_H
#define _WCHAT_WINDOW_H

#include <gk.h>
#include "chat.h"
#include "link.h"

#define CW_W 760                        /* the window's content */
#define CW_H 440
#define CW_MIN_W 480
#define CW_MIN_H 300
#define CW_ROW_H 16                     /* a row of messages: the console's 8 x 16 characters */
#define CW_MARGIN 4                     /* pixels left of the messages */
#define CW_TIME 6                       /* cells of "10:15 " */
#define CW_WHO 20                       /* cells of a nick at the start of a line, at most */
#define CW_NICKS_W 150                  /* the nick list */
#define CW_OUTBOX 8192                  /* bytes for the helper waiting for its pipe */
#define CW_REAP_MS 250                  /* with a helper: how often to look whether it was collected */

/* what the program hears from */
enum {
    ID_PAGES = 1, ID_TABS, ID_TOPIC, ID_AREA, ID_NICKS, ID_INPUT,
    ID_HOST, ID_PORT, ID_TLS, ID_NICK, ID_PASSKIND, ID_PASS, ID_CONNECT, ID_CANCEL,
    BOX_CLOSE = 100, BOX_ABOUT,
    MENU_CONNECT = 200, MENU_DISCONNECT, MENU_CLOSE_ROOM, MENU_QUIT, MENU_PREV, MENU_NEXT, MENU_CODES, MENU_ABOUT,
    WATCH_FROM = 300, WATCH_TO,
};

enum { PAGE_CONNECT, PAGE_CHAT };

struct chatwin {
    struct gk *ui;
    struct chat *c;
    struct gk_widget *pages, *tabs, *topic, *area, *nicks, *input, *status;
    struct gk_widget *host, *port, *tls, *nick, *passkind, *pass, *formerr;
    unsigned tab_room[CHAT_ROOMS];      /* the room each tab shows, by its id */
    int ntabs;
    /* the active room's messages as rows: rows[seq % CHAT_LINES] for each line kept */
    unsigned shown;                     /* the room they are for (its id; 0: none yet) */
    int cols;                           /* the characters a row holds */
    unsigned long synced, first;        /* the room's seq when they were counted, and its first line's then */
    int total;                          /* rows in all */
    unsigned short rows[CHAT_LINES];
    unsigned nicks_room;                /* the room the nick list shows */
    unsigned topic_color;               /* the topic line's colour as last set */
    /* the helper */
    int pid, zombie;                    /* its pid; one killed, still to be collected */
    int to_fd, from_fd;                 /* the pipes to and from it, -1 when closed */
    int out_watched;
    struct link_reader link;
    char outbox[CW_OUTBOX];
    int outlen;
    int closing;                        /* the window closes once the connection has ended */
    char title[200];                    /* the title last given, whole (the desktop cuts it at WIN_TITLE_MAX) */
};

int chatwin_open(struct chatwin *w, struct chat *c);   /* 0, or -1 with errno (ENODEV: no desktop here) */
int chatwin_event(struct chatwin *w, const struct gk_event *ev);   /* 0 once the window is to close */
int chatwin_tick(struct chatwin *w);    /* after each gk_next(): the time, the helper, what goes out, what is shown;
                                           0 once the window is to close */
int chatwin_wait(const struct chatwin *w);  /* the ms gk_next() may wait, -1 as long as it takes */
void chatwin_close(struct chatwin *w);  /* the helper killed, the window closed */

/* SurfOS's own (proc.c; the host tests stand in for them) */
/* the helper for host, port and tls: its pid, and the pipe the window writes to it (*to_fd) and reads from it
   (*from_fd); -1 with errno */
int chat_spawn(const char *host, unsigned port, int tls, int *to_fd, int *from_fd);
void chat_kill(int pid);
int chat_reaped(int pid);               /* 1 once it has ended and been collected, 0 while it runs */
int chat_minute(void);                  /* the PC's clock: the minute of the day, -1 when not known */

#endif
