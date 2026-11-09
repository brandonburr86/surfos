/*
SurfOS user program: wsheet
--------------------
File: main.c    Date: 11/05/26
--------------------
The Spreadsheet: a window on the desktop with a sheet of 256 columns and 65,536 rows, its formulas
worked out as anything changes, saved in its own text format (.sheet), which keeps them, or as CSV,
which keeps the values (app.c is the window, the other files of user/wsheet/ the sheet). The desktop's
Apps menu and its tile start it; Files starts it with a .sheet's or a .csv's path. Its loop is the
toolkit's: gk_next() until the window closes or goes, and between the events the next piece of a file
being read.

wsheet [file]     the window, with the file read into it
*/

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "app.h"

int main(int argc, char **argv) {
    static struct app a;
    struct gk_event ev;
    int r;
    if(argc > 2 || (argc == 2 && argv[1][0] == '-')) {
        fprintf(stderr, "usage: wsheet [file]\n");
        return 2;
    }
    if(app_open(&a, &io_files, argc > 1 ? argv[1] : NULL) < 0) {
        fprintf(stderr, "wsheet: no window: %s\n", errno == ENODEV ? "this terminal is no desktop's" : strerror(errno));
        return 1;
    }
    for(;;) {
        r = gk_next(a.ui, &ev, app_reading(&a) ? 0 : -1);
        if(r < 0) break;                        /* the desktop let the window go, or a kill */
        if(r > 0 && !app_event(&a, &ev)) break;
        if(app_reading(&a)) app_read(&a);
    }
    app_close(&a);
    return 0;
}
