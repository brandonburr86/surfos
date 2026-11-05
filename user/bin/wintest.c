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
10/26/26 (stage C12): in wintest show, x puts "echo wintest-clip" on the desktop's clipboard
(WIN_CLIP: its title says "wintest: clip 0", or the error's name), and y asks to be seen once the
window has lost the focus (WIN_ALERT 1.5 s after it: "wintest: alerted").
11/05/26 (the desktop applications, docs/superpowers/plans/2026-11-05-desktop-apps.md, track desk):
wintest desk [FILE]   the calls of 11/05/26, by hand and for the screen test (tools/gui_apps_desk.py):
                 menus Test and View in the desktop's bar (WIN_MENU; View > Size a submenu, and a Help
                 merged into the desktop's), and its title says what came
                 last: a key with its modifiers ("desk: key 0x102 mods 5"), an item chosen ("desk:
                 menu 21 of 2"), the theme ("desk: theme 1": WIN_THEME, WIN_EV_THEME), a chooser's
                 answer ("desk: took /usb1p1/a.wintest", "desk: cancelled 4"). p pastes (WIN_PASTE:
                 "desk: paste 5 hello"), o asks for the Open chooser for .wintest files, s for Save
                 as (saved.wintest), f for Choose folder, m takes the menus away and gives them back.
                 Files starts wintest with a path (rootfs/apps/wintest.app: the type wintest), which
                 it takes as desk's FILE: its title says "desk: file <path>" first. (The review of
                 stage 2) the close button gets its own question first, "desk: close? y/n": n cancels,
                 y or the close button again ends it.
wintest raw      TTY_RAW on its terminal, then its end: from the desktop's Run program..., the
                 desktop keeps its keys (tty.h's TTY_RAW_KEYS)
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

/* 10/26/26: an error's name for a title, as the screen test reads it */
static const char *err_name(int r) {
    return !r ? "0" : errno == EACCES ? "EACCES" : errno == EPIPE ? "EPIPE" : errno == EFAULT ? "EFAULT" : "error";
}

static int mode_show(void) {
    static const char text[] = "echo wintest-clip";
    struct win_open o;
    struct win_info info;
    struct win_event ev[16];
    struct win_clip clip;
    char title[WIN_TITLE_MAX];
    int fd, n, i, alert_away = 0;
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
            if(ev[i].type == WIN_EV_KEY && ev[i].value == 'x') {         /* 10/26/26: the clipboard */
                clip.text = text;
                clip.len = sizeof(text) - 1;
                snprintf(title, sizeof(title), "wintest: clip %s", err_name(winctl(fd, WIN_CLIP, &clip)));
            }
            if(ev[i].type == WIN_EV_KEY && ev[i].value == 'y') {         /* asked to be seen once it is away */
                alert_away = 1;
                snprintf(title, sizeof(title), "wintest: alert when away");
            }
            if(ev[i].type == WIN_EV_FOCUS && !ev[i].value && alert_away) {
                alert_away = 0;
                msleep(1500);
                snprintf(title, sizeof(title), "wintest: alerted %s", err_name(winctl(fd, WIN_ALERT, NULL)));
                if(!strcmp(title, "wintest: alerted 0")) strcpy(title, "wintest: alerted");
            }
        }
        winctl(fd, WIN_TITLE, title);
    }
    printf("wintest: the desktop let the window go\n");
    close(fd);
    return 0;
}

/**** 11/05/26 (track desk): desk ****/

static struct win_menu_item menu_items[13];

static void menu_entry(int n, unsigned id, unsigned flags, const char *label, const char *keys) {
    memset(&menu_items[n], 0, sizeof(menu_items[n]));
    menu_items[n].id = id;
    menu_items[n].flags = flags;
    strcpy(menu_items[n].label, label);
    strcpy(menu_items[n].keys, keys);
}

static int set_menus(int fd, int on) {
    struct win_menu m;
    menu_entry(0, 1, WIN_MENU_TITLE, "Test", "");
    menu_entry(1, 11, 0, "Hello", "Ctrl+H");
    menu_entry(2, 12, WIN_MENU_CHECKED, "Checked", "");
    menu_entry(3, 0, WIN_MENU_SEPARATOR, "", "");
    menu_entry(4, 13, WIN_MENU_DISABLED, "Greyed", "");
    menu_entry(5, 2, WIN_MENU_TITLE, "View", "");
    menu_entry(6, 21, WIN_MENU_RADIO, "Big", "F5");
    menu_entry(7, 22, 0, "Small", "");
    menu_entry(8, 25, WIN_MENU_SUBMENU, "Size", "");    /* the review of 11/05/26: a submenu and a Help */
    menu_entry(9, 23, WIN_MENU_CHILD, "Normal", "");
    menu_entry(10, 24, WIN_MENU_CHILD, "Large", "Ctrl+=");
    menu_entry(11, 3, WIN_MENU_TITLE | WIN_MENU_HELP, "Help", "");
    menu_entry(12, 31, 0, "About wintest", "");
    m.items = menu_items;
    m.count = on ? 13 : 0;
    return winctl(fd, WIN_MENU, &m);
}

static int ask(int fd, unsigned kind, unsigned id, const char *name) {
    struct win_dialog d;
    memset(&d, 0, sizeof(d));
    d.kind = kind;
    d.id = id;
    strcpy(d.name, name);
    strcpy(d.types, kind == WIN_DLG_FOLDER ? "" : "wintest txt");
    return winctl(fd, WIN_DIALOG, &d);
}

static int mode_desk(const char *file) {
    static char clip[WIN_CLIP_MAX + 1];
    struct win_open o;
    struct win_info info;
    struct win_theme theme;
    struct win_event ev[16];
    struct win_buffer b;
    char title[WIN_TITLE_MAX], path[WIN_PATH_MAX];
    int fd, n, i, r, menus = 1, asking = 0;
    good(&o);
    o.w = 320;
    o.h = 200;
    strcpy(o.title, "desk: ready");
    fd = winopen(&o);
    if(fd < 0) return fail(strerror(errno));
    if(winctl(fd, WIN_INFO, &info) < 0 || quadrants(info.w, info.h) < 0 || present_all(fd) < 0)
        return fail(strerror(errno));
    if(set_menus(fd, 1) < 0) printf("wintest: WIN_MENU: %s\n", strerror(errno));
    if(winctl(fd, WIN_THEME, &theme) < 0) printf("wintest: WIN_THEME: %s\n", strerror(errno));
    if(file) {
        snprintf(title, sizeof(title), "desk: file %s", file);
        winctl(fd, WIN_TITLE, title);
    }
    for(;;) {
        n = read(fd, ev, sizeof(ev));
        if(n == 0) break;
        if(n < 0) {
            if(errno == EINTR) continue;
            return fail(strerror(errno));
        }
        for(i = 0; i < n / (int)sizeof(ev[0]); i++) {
            const struct win_event *e = &ev[i];
            title[0] = 0;
            /* 11/05/26 (the review of stage 2): its own question first, as a program with unsaved changes asks; y or a
               second press of the close button ends it, n cancels (and the next press asks again) */
            if((e->type == WIN_EV_CLOSE && asking) || (e->type == WIN_EV_KEY && asking && e->value == 'y')) {
                printf("wintest: closed\n");
                close(fd);
                return 0;
            }
            if(e->type == WIN_EV_CLOSE) {
                asking = 1;
                snprintf(title, sizeof(title), "desk: close? y/n");
            } else if(e->type == WIN_EV_KEY && asking && e->value == 'n') {
                asking = 0;
                snprintf(title, sizeof(title), "desk: close cancelled");
            } else if(e->type == WIN_EV_KEY && !e->mods && e->value == 'p') {
                b.buf = clip;
                b.size = sizeof(clip);
                r = winctl(fd, WIN_PASTE, &b);
                if(r < 0) snprintf(title, sizeof(title), "desk: paste %s", strerror(errno));
                else snprintf(title, sizeof(title), "desk: paste %d %.24s", r, clip);
            } else if(e->type == WIN_EV_KEY && !e->mods && (e->value == 'o' || e->value == 's' || e->value == 'f')) {
                winctl(fd, WIN_TITLE, "desk: asking");  /* what the chooser's title names it by */
                r = ask(fd, e->value == 'o' ? WIN_DLG_OPEN : e->value == 's' ? WIN_DLG_SAVE : WIN_DLG_FOLDER,
                        e->value == 'o' ? 3 : e->value == 's' ? 4 : 5, e->value == 's' ? "saved.wintest" : "");
                if(r < 0) snprintf(title, sizeof(title), "desk: asked %s", strerror(errno));
            } else if(e->type == WIN_EV_KEY && !e->mods && e->value == 'm') {
                menus = !menus;
                r = set_menus(fd, menus);
                snprintf(title, sizeof(title), "desk: menus %s", r < 0 ? strerror(errno) : menus ? "on" : "off");
            } else if(e->type == WIN_EV_KEY) {
                snprintf(title, sizeof(title), "desk: key 0x%x mods %u", e->value, e->mods);
            } else if(e->type == WIN_EV_MENU) {
                snprintf(title, sizeof(title), "desk: menu %u of %d", e->value, e->x);
            } else if(e->type == WIN_EV_THEME) {
                snprintf(title, sizeof(title), "desk: theme %u", e->value);
            } else if(e->type == WIN_EV_DIALOG && e->value) {
                b.buf = path;
                b.size = sizeof(path);
                r = winctl(fd, WIN_TAKE, &b);
                snprintf(title, sizeof(title), "desk: took %s", r < 0 ? strerror(errno) : path);
            } else if(e->type == WIN_EV_DIALOG) {
                snprintf(title, sizeof(title), "desk: cancelled %d", e->x);
            }
            if(title[0]) winctl(fd, WIN_TITLE, title);
        }
    }
    printf("wintest: the desktop let the window go\n");
    close(fd);
    return 0;
}

/* 11/05/26 (the review of stage 2): raw mode asked for on this terminal, and the program ends. Started by the desktop
   (Apps > Run program...), its terminal is the desktop's console, whose raw mode stays the desktop's (EBUSY: tty.h's
   TTY_RAW_KEYS), and its end changes nothing: the screen check types on after it */
static int mode_raw(void) {
    int r = ttyctl(TTY_RAW, 1);
    printf("wintest raw: TTY_RAW %s\n", r < 0 ? strerror(errno) : "taken");
    if(r == 0) ttyctl(TTY_RAW, 0);
    return 0;
}

int main(int argc, char **argv) {
    mode = argc > 1 ? argv[1] : "";
    if(!strcmp(mode, "desk")) return mode_desk(argc > 2 ? argv[2] : NULL);    /* 11/05/26 */
    if(!strcmp(mode, "raw")) return mode_raw();
    if(mode[0] == '/') return mode_desk(mode);          /* a file, from Files (rootfs/apps/wintest.app) */
    if(!strcmp(mode, "nodesk")) return mode_nodesk();
    if(!strcmp(mode, "pty")) return mode_pty();
    if(!strcmp(mode, "show")) return mode_show();
    if(!strcmp(mode, "tty")) return mode_tty();
    fprintf(stderr, "usage: wintest nodesk | pty | show | tty | desk [file] | raw\n");
    return 2;
}
