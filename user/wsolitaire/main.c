/*
SurfOS user program: wsolitaire
--------------------
File: main.c    Date: 11/05/26
--------------------
Solitaire, Klondike in a window (window.c, on the toolkit gk.h; the game is table.c and
klondike.c): seven piles to build down in alternate colours and four foundations to build up by
suit, drawing one card or three from the stock. The desktop's Apps menu starts it
(rootfs/apps/wsolitaire.app), or the Terminal: `wsolitaire &`. Its loop is the toolkit's:
gk_next() until the window closes or the desktop lets it go. The deals are new each time: the
generator is seeded from the PC's clock and the milliseconds since boot.

wsolitaire    the window, on the desktop of the terminal it was started from

Status: 0 when the window closed, 2 for trouble (a word given, or no window here).
*/

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <surf.h>
#include "window.h"

int main(int argc, char **argv) {
    static struct swin s;
    struct gk_event ev;
    (void)argv;
    if(argc > 1) {
        fprintf(stderr, "usage: wsolitaire\n");
        return 2;
    }
    if(sw_open(&s, (unsigned)time(NULL) ^ uptime() * 2654435761u) < 0) {
        fprintf(stderr, "wsolitaire: no window: %s\n", errno == ENODEV ? "this terminal is no desktop's" :
                strerror(errno));
        return 2;
    }
    while(gk_next(s.ui, &ev, -1) > 0) if(!sw_event(&s, &ev)) break;
    sw_close(&s);
    return 0;
}
