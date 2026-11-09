/*
SurfOS user program: wswell
--------------------
File: main.c    Date: 11/05/26
--------------------
Swell, a falling-blocks game of SurfOS's own, in a window (window.c, on the toolkit gk.h; the game
is view.c and swell.c): pieces of two, three and five cells fall into a sea 12 cells wide; full
rows wash away, and from level 3 the swell lifts the sea a row of sand at a time. The desktop's
Apps menu starts it (rootfs/apps/wswell.app), or the Terminal: `wswell &`. Its loop is the
toolkit's: gk_next() until the window closes or the desktop lets it go. The pieces come in a new
order each time: the generator is seeded from the PC's clock and the milliseconds since boot.

wswell        the window, on the desktop of the terminal it was started from

Status: 0 when the window closed, 2 for trouble (a word given, or no window here).
*/

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <surf.h>
#include "window.h"

int main(int argc, char **argv) {
    static struct swwin w;
    struct gk_event ev;
    (void)argv;
    if(argc > 1) {
        fprintf(stderr, "usage: wswell\n");
        return 2;
    }
    if(ww_open(&w, (unsigned)time(NULL) ^ uptime() * 2654435761u) < 0) {
        fprintf(stderr, "wswell: no window: %s\n", errno == ENODEV ? "this terminal is no desktop's" : strerror(errno));
        return 2;
    }
    while(gk_next(w.ui, &ev, -1) > 0) if(!ww_event(&w, &ev)) break;
    ww_close(&w);
    return 0;
}
