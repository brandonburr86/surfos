/*
SurfOS user program: wpaint
--------------------
File: main.c    Date: 11/05/26
--------------------
Paint, a window to draw pictures in (window.c; the desktop applications, track paint of
docs/superpowers/plans/2026-11-05-desktop-apps.md): the loop, gk_next() until the window closes or
the desktop lets it go, the pattern of a program built on the toolkit (gk.h).

wpaint            a new picture
wpaint PICTURE    PICTURE opened (BMP, PNG, GIF or JPEG), as Files' Open with starts it

Status 0; 1 when there is no window (this terminal is no desktop's); 2 for a wrong use.
*/

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "window.h"

int main(int argc, char **argv) {
    static struct pwin w;                               /* the canvas's strip buffer is large: not on the stack */
    struct gk_event ev;
    if(argc > 2 || (argc == 2 && !argv[1][0])) {
        fprintf(stderr, "usage: wpaint [picture]\n");
        return 2;
    }
    if(pw_open(&w, argc == 2 ? argv[1] : NULL) < 0) {
        fprintf(stderr, "wpaint: no window: %s\n", errno == ENODEV ? "this terminal is no desktop's" : strerror(errno));
        return 1;
    }
    while(gk_next(w.ui, &ev, -1) > 0) if(!pw_event(&w, &ev)) break;
    pw_close(&w);
    return 0;
}
