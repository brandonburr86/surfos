/*
SurfOS Desktop: the Terminal's Side of struct gui_sys
--------------------
File: guisys_term.c Date: 10/17/26
--------------------
A shell session on a pseudo-terminal for the Terminal (kernel/pty.c); track TERM of
docs/superpowers/plans/2026-10-17-gui-suite.md. A placeholder until the track's work lands.
*/
#include <surfos/types.h>
#include <fs/vfs.h>
#include "guisys.h"

int guisys_term_open(void *ctx, int cols, int rows) {
    (void)ctx; (void)cols; (void)rows;
    return -ENOSYS;
}

int guisys_term_read(void *ctx, int h, char *buf, u32 n) {
    (void)ctx; (void)h; (void)buf; (void)n;
    return -EPIPE;
}

int guisys_term_write(void *ctx, int h, const char *buf, u32 n) {
    (void)ctx; (void)h; (void)buf; (void)n;
    return -EPIPE;
}

int guisys_term_resize(void *ctx, int h, int cols, int rows) {
    (void)ctx; (void)h; (void)cols; (void)rows;
    return -EPIPE;
}

void guisys_term_close(void *ctx, int h) {
    (void)ctx; (void)h;
}
