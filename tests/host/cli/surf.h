/* What user/bin/tree.c asks of SurfOS's <surf.h>, for its build on the host (11/01/26): the terminal draws UTF-8. */
#ifndef CLI_SURF_H
#define CLI_SURF_H
#define TTY_CAPS 4
#define TTY_CAP_UTF8 1
static inline int ttyctl(int op, unsigned int arg) {
    (void)op;
    (void)arg;
    return TTY_CAP_UTF8;
}
#endif
