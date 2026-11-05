/*
SurfOS user program: wkit
--------------------
File: main.c    Date: 11/05/26
--------------------
The window toolkit's demonstration (gk.h): a window with every widget (demo.c), for trying the
toolkit by hand and for the desktop's screen checks (tools/gui_apps_kit.py). Its loop is the
pattern of a program built on the toolkit: gk_next() until the window closes or goes.

wkit          the window, on the desktop of the terminal it was started from
*/

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "demo.h"

int main(int argc, char **argv) {
    static struct demo d;
    struct gk_event ev;
    (void)argv;
    if(argc > 1) {
        fprintf(stderr, "usage: wkit\n");
        return 2;
    }
    if(demo_open(&d) < 0) {
        fprintf(stderr, "wkit: no window: %s\n", errno == ENODEV ? "this terminal is no desktop's" : strerror(errno));
        return 1;
    }
    while(gk_next(d.ui, &ev, -1) > 0) if(!demo_event(&d, &ev)) break;
    demo_close(&d);
    return 0;
}
