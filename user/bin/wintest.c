/*
SurfOS user program: wintest
--------------------
File: wintest.c  Date: 10/25/26
--------------------
The window calls (include/surfos/win.h) from user space, through libsurf's bare winopen() and
winctl() and no drawing library, for the smoke test (tools/qemu-run.py), the desktop's screen
tests and trying them by hand (stage C3 of docs/superpowers/plans/2026-10-25-app-windows.md).
The check modes end with "WINTEST <mode> PASS", or with "WINTEST <mode> FAIL: <why>" and exit
status 1.

wintest nodesk   where no desktop is: EFAULT for a description that is not this program's to
                 read, EINVAL for a version, flag, size, limits or colour that is none, ENODEV
                 for a good window (this terminal is no desktop's); winctl on a descriptor
                 that is no window: EBADF, EINVAL
wintest pty      wintest nodesk on a pseudo-terminal of its own: refused as well, even while a
                 desktop runs on this console (a telnet session's case, without a network)
wintest show     a resizable window of four coloured quadrants, with the events it gets in its
                 title; a new size is answered with quadrants of that size; the close button
                 ends it, and so does the desktop letting it go
wintest tty      10/25/26: the terminal it runs on, as procinfo says ("WINTEST tty: pty0"): the
                 screen test's way to see a pty's number come back to a pty of its own
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <surf.h>
#include <surfos/proc.h>

#define SELF "/initrd/bin/wintest"

static const char *mode = "";

static int fail(const char *why) {
    printf("WINTEST %s FAIL: %s\n", mode, why);
    return 1;
}

static int pass(void) {
    printf("WINTEST %s PASS\n", mode);
    return 0;
}

/* a window that is one: 200 x 100 of navy, titled */
static void good(struct win_open *o) {
    memset(o, 0, sizeof(*o));
    o->version = WIN_VERSION;
    o->w = 200;
    o->h = 100;
    o->background = 0x102E3B;
    strcpy(o->title, "wintest");
}

/* r is a failure with errno err */
static int refused(int r, int err, const char *what) {
    char why[120];
    if(r == -1 && errno == err) return 1;
    snprintf(why, sizeof(why), "%s: %d with errno %d (%s), not %s", what, r, r < 0 ? errno : 0,
             r < 0 ? strerror(errno) : "no error", strerror(err));
    fail(why);
    return 0;
}

static int mode_nodesk(void) {
    struct win_open o;
    struct win_info info;
    good(&o);
    if(!refused(winopen(NULL), EFAULT, "winopen(NULL)") ||
       !refused(winopen((const struct win_open *)0x1000), EFAULT, "a description below the program") ||
       !refused(winopen((const struct win_open *)0x60000000), EFAULT, "a description where nothing is mapped"))
        return 1;
    printf("    EFAULT: no description, one below the program, one where nothing is mapped\n");
    o.version = 2;
    if(!refused(winopen(&o), EINVAL, "version 2")) return 1;
    good(&o);
    o.flags = 0x80;
    if(!refused(winopen(&o), EINVAL, "an unknown flag")) return 1;
    good(&o);
    o.w = 0;
    if(!refused(winopen(&o), EINVAL, "no width")) return 1;
    good(&o);
    o.min_w = 300;
    o.max_w = 200;
    if(!refused(winopen(&o), EINVAL, "limits that leave no width")) return 1;
    good(&o);
    o.background = 0xFF102E3B;
    if(!refused(winopen(&o), EINVAL, "a colour with its top byte set")) return 1;
    printf("    EINVAL: version 2, an unknown flag, no width, limits that leave no width, a colour of 32 bits\n");
    good(&o);
    if(!refused(winopen(&o), ENODEV, "a good window")) return 1;
    printf("    ENODEV: a good window, on a terminal that is no desktop's\n");
    if(!refused(winctl(99, WIN_INFO, &info), EBADF, "winctl on descriptor 99") ||
       !refused(winctl(0, WIN_INFO, &info), EINVAL, "winctl on descriptor 0, the console")) return 1;
    printf("    EBADF, EINVAL: winctl on no descriptor, and on one that is no window\n");
    return pass();
}

static int mode_pty(void) {
    char *argv[] = { "wintest", "nodesk", NULL };
    static char out[4096];
    int m, pid, n = 0, r, status, index;
    m = openpty();
    if(m < 0) return fail("openpty");
    index = ptyctl(m, PTY_INDEX, 0);
    pid = spawnpty(SELF, argv, m);
    if(pid < 0) return fail("spawnpty");
    while(n < (int)sizeof(out) - 1 && (r = read(m, out + n, sizeof(out) - 1 - n)) > 0) n += r;   /* to its end */
    out[n] = 0;
    waitpid(pid, &status, 0);
    close(m);
    if(!strstr(out, "ENODEV: a good window") || !strstr(out, "WINTEST nodesk PASS")) {
        printf("%s", out);
        return fail("wintest nodesk on a pty of its own did not end as it should");
    }
    printf("    wintest nodesk on pty%d: ENODEV, as on a console where no desktop runs\n", index);
    return pass();
}

/* 10/25/26: the terminal this program is on, as procinfo says */
static int mode_tty(void) {
    struct procinfo pi;
    int i, me = (int)getpid();
    for(i = 0; procinfo(i, &pi) == 0; i++) {
        if(pi.pid != me) continue;
        if(pi.tty >= PROC_TTY_PTY) printf("WINTEST tty: pty%d\n", pi.tty - PROC_TTY_PTY);
        else if(pi.tty >= 0) printf("WINTEST tty: F%d\n", pi.tty + 1);
        else printf("WINTEST tty: none\n");
        return 0;
    }
    return fail("not in the task list");
}

/**** show ****/

static unsigned *image;
static int image_w, image_h;

/* quadrants of w x h: coral, teal, sky and sand */
static int quadrants(int w, int h) {
    unsigned *p = malloc((size_t)w * h * 4);
    int x, y;
    if(!p) return -1;
    for(y = 0; y < h; y++)
        for(x = 0; x < w; x++)
            p[y * w + x] = y < h / 2 ? (x < w / 2 ? 0xEE906D : 0x197D83) : (x < w / 2 ? 0x659ECB : 0xD0B056);
    free(image);
    image = p;
    image_w = w;
    image_h = h;
    return 0;
}

static int present_all(int fd) {
    struct win_present p;
    memset(&p, 0, sizeof(p));
    p.pixels = image;
    p.stride = image_w;
    p.w = image_w;
    p.h = image_h;
    return winctl(fd, WIN_PRESENT, &p);
}

static void describe(const struct win_event *e, char *out, size_t n) {
    switch(e->type) {
    case WIN_EV_KEY: snprintf(out, n, "wintest: key %u", e->value); break;
    case WIN_EV_POINTER: snprintf(out, n, "wintest: pointer %d,%d buttons %u", e->x, e->y, e->value); break;
    case WIN_EV_WHEEL: snprintf(out, n, "wintest: wheel %d at %d,%d", (int)e->value, e->x, e->y); break;
    case WIN_EV_FOCUS: snprintf(out, n, "wintest: focus %u", e->value); break;
    case WIN_EV_RESIZE: snprintf(out, n, "wintest: size %dx%d", e->x, e->y); break;
    case WIN_EV_CLOSE: snprintf(out, n, "wintest: close"); break;
    default: snprintf(out, n, "wintest: event %u", e->type); break;
    }
}

static int mode_show(void) {
    struct win_open o;
    struct win_info info;
    struct win_event ev[16];
    char title[WIN_TITLE_MAX];
    int fd, n, i;
    good(&o);
    o.w = 320;
    o.h = 200;
    o.flags = WIN_RESIZABLE;
    fd = winopen(&o);
    if(fd < 0) return fail(strerror(errno));
    if(winctl(fd, WIN_INFO, &info) < 0 || quadrants(info.w, info.h) < 0 || present_all(fd) < 0)
        return fail(strerror(errno));
    printf("wintest: a window of %ux%u on a desktop of %ux%u at %ux\n", info.w, info.h, info.screen_w, info.screen_h,
           info.scale);
    for(;;) {
        n = read(fd, ev, sizeof(ev));
        if(n == 0) break;                               /* the desktop let it go */
        if(n < 0) {
            if(errno == EINTR) continue;
            return fail(strerror(errno));
        }
        for(i = 0; i < n / (int)sizeof(ev[0]); i++) {
            describe(&ev[i], title, sizeof(title));
            if(ev[i].type == WIN_EV_CLOSE) {
                printf("wintest: closed\n");
                close(fd);
                return 0;
            }
            if(ev[i].type == WIN_EV_RESIZE && quadrants(ev[i].x, ev[i].y) == 0 &&
               winctl(fd, WIN_SETSIZE, (void *)(unsigned long)((ev[i].x << 16) | ev[i].y)) == 0)
                present_all(fd);
        }
        winctl(fd, WIN_TITLE, title);
    }
    printf("wintest: the desktop let the window go\n");
    close(fd);
    return 0;
}

int main(int argc, char **argv) {
    mode = argc > 1 ? argv[1] : "";
    if(!strcmp(mode, "nodesk")) return mode_nodesk();
    if(!strcmp(mode, "pty")) return mode_pty();
    if(!strcmp(mode, "show")) return mode_show();
    if(!strcmp(mode, "tty")) return mode_tty();
    fprintf(stderr, "usage: wintest nodesk | pty | show | tty\n");
    return 2;
}
