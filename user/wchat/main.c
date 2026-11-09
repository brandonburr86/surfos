/*
SurfOS user program: wchat
--------------------
File: main.c    Date: 11/05/26
--------------------
Chat, an IRC client in a window (the desktop applications, track chat): the window (window.c) on
the model (chat.c, recv.c, input.c), and the connection in a helper process (helper.c, link.h).

    wchat                               the window, on the desktop of the terminal it was started from
    wchat --helper HOST PORT tls|tcp    the window's own helper: not for people

The loop is the toolkit's: gk_next() for as long as the model may wait, each event to the window,
then the window's tick. A write to the helper's pipe once the helper is gone says EPIPE instead of
ending the program (broken_pipe_exit(0)): the window says the connection ended and goes on.
Status 0, 1 when there is no window, 2 for a usage error.
*/

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <surf.h>
#include "window.h"

extern const char *chat_self;
int helper_main(int argc, char **argv);

int main(int argc, char **argv) {
    static struct chat c;
    static struct chatwin w;
    struct gk_event ev;
    int r;
    if(argc > 1 && !strcmp(argv[1], "--helper")) return helper_main(argc, argv);
    if(argc > 1) {
        fprintf(stderr, "usage: wchat\n");
        return 2;
    }
    if(argv[0] && strchr(argv[0], '/')) chat_self = argv[0];    /* the helper is this program */
    broken_pipe_exit(0);
    if(chat_init(&c)) {
        fprintf(stderr, "wchat: out of memory\n");
        return 1;
    }
    if(chatwin_open(&w, &c) < 0) {
        fprintf(stderr, "wchat: no window: %s\n", errno == ENODEV ? "this terminal is no desktop's" : strerror(errno));
        chat_free(&c);
        return 1;
    }
    while((r = gk_next(w.ui, &ev, chatwin_wait(&w))) >= 0) {
        if(r > 0 && !chatwin_event(&w, &ev)) break;
        if(!chatwin_tick(&w)) break;
    }
    chatwin_close(&w);
    chat_free(&c);
    return 0;
}
