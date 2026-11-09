/*
SurfOS user program: wmines
--------------------
File: main.c    Date: 11/05/26
--------------------
Mines, a game in a window (window.c, on the toolkit gk.h; the game is view.c and mines.c): a field
of covered cells, some of them mines, to clear by revealing every cell that holds none. The
desktop's Apps menu starts it (rootfs/apps/wmines.app), or the Terminal: `wmines &`. Its loop is
the toolkit's: gk_next() until the window closes or the desktop lets it go. The fields are new each
time: the generator is seeded from the PC's clock and the milliseconds since boot.

wmines        the window, on the desktop of the terminal it was started from

Status: 0 when the window closed, 2 for trouble (a word given, or no window here).
*/

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <surf.h>
#include "window.h"

int main(int argc, char **argv) {
    static struct mwin m;
    struct gk_event ev;
    (void)argv;
    if(argc > 1) {
        fprintf(stderr, "usage: wmines\n");
        return 2;
    }
    if(mw_open(&m, (unsigned)time(NULL) ^ uptime() * 2654435761u) < 0) {
        fprintf(stderr, "wmines: no window: %s\n", errno == ENODEV ? "this terminal is no desktop's" : strerror(errno));
        return 2;
    }
    while(gk_next(m.ui, &ev, -1) > 0) if(!mw_event(&m, &ev)) break;
    mw_close(&m);
    return 0;
}
