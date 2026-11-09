/* Host tests for the window toolkit (gk.h; user/libsurf/gk*.c, the desktop applications' track kit), with libgui
   (user/libsurf/gui.c) and the desktop's drawing kit built as they are, against the fake desktop of
   tests/host/gk_fake.c: libgui's view (an origin and a clip), a button's UTF-8 label, a character's code; layout (boxes, growing, alignment, padding, hidden widgets, pages); damage (only what changed
   is presented: a label, a field typed into, a list's row); buttons, check boxes, switches and radio groups by the
   pointer and the keys; the line of text (UTF-8 by whole characters, the selection by Shift and the mouse, a word by
   a double-click, Ctrl+A, C, X, the clipboard only with the focus, a full field, a password field's dots that copy
   nothing, the history, the hint); lists (the keys, a click and a double-click, the wheel and the scroll bar, columns,
   lines added and removed under the selection); a scroll bar on its own; tabs (clicks, keys, the x of a closable one,
   a tab removed, too many to fit); status bars; the focus by Tab and Shift+Tab, past what is hidden, disabled or on a
   page put away; areas (drawn in their own coordinates and clipped, the pointer and the keys, the focus, scroll bars
   in the program's units, the wheel, a part damaged); modal boxes (each answered once: Enter, Esc, a click, a box
   over a box, a password asked); timers and watched descriptors; a new size and the window let go. Track desk's
   calls (surfos/win.h): the menus sent whole when they change, an item chosen in the bar or by its shortcut (a
   control byte with or without WIN_MOD_CTRL, a function key, Ctrl+Shift), a greyed one's shortcut the window's key, a
   field's keys before the shortcuts and an area's after, no room past the bar's limits, a desktop that does not show
   them; the clipboard read into a field (its first line, as much as fits, a character whole), whole, without the
   focus, from a desktop that cannot; choosers asked, answered, cancelled, refused as the kernel refuses them, and a
   line to type the path in on a desktop without them; the theme read at the opening and followed when it changes;
   the keys with their modifiers (Ctrl with the arrows, Backspace and Delete, Ctrl+H, Shift and a click, Ctrl+Tab out
   of an area that takes Tab). Built and run by `make unittest` (and natively on the Mac with the sanitizers: see the
   account). */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <gk.h>
#include "gk_fake.h"
#include "../../user/libsurf/gkpriv.h"           /* a field's state, to see that a wipe leaves no byte */

static int checks, failed;
static void show_got(void);
#define CHECK(x) do { checks++; if(!(x)) { failed++; printf("FAIL %s line %d: %s\n", __FILE__, __LINE__, #x); show_got(); } } while(0)

#define SHEET 0xFFFDF5
#define PICK 0xBFE3DD

/* the events the program heard last, with their texts kept */
#define MAXEV 256
static struct gk_event got[MAXEV];
static char said[MAXEV][160];
static int ngot;

/* every event the program would hear now (gk_next() with 0); at most 100000 of them, a check failing past that, so
   that a toolkit that never says 0 fails the test instead of running it for hours */
static int run(struct gk *ui) {
    struct gk_event ev;
    int r, turns = 0;
    ngot = 0;
    while((r = gk_next(ui, &ev, 0)) > 0) {
        if(++turns > 100000) {
            CHECK(!"run(): gk_next() never said 0");
            return -1;
        }
        if(ngot == MAXEV) continue;
        got[ngot] = ev;
        snprintf(said[ngot], sizeof(said[ngot]), "%s", ev.text ? ev.text : "(null)");
        got[ngot].text = said[ngot];
        ngot++;
    }
    return r;
}

/* what the program heard last, after a failed check */
static void show_got(void) {
    int i;
    for(i = 0; i < ngot; i++)
        printf("  heard: type %d id %d value %d mods %u text \"%s\"\n", got[i].type, got[i].id, got[i].value, got[i].mods,
               got[i].text ? got[i].text : "");
}

static const struct gk_event *heard(int type, int id) {
    int i;
    for(i = 0; i < ngot; i++) if(got[i].type == type && got[i].id == id) return &got[i];
    return NULL;
}

/* the nth event of a type the program heard (from 0), NULL */
static const struct gk_event *nth(int type, int n) {
    int i;
    for(i = 0; i < ngot; i++) if(got[i].type == type && !n--) return &got[i];
    return NULL;
}

static int count(int type) {
    int i, n = 0;
    for(i = 0; i < ngot; i++) n += got[i].type == type;
    return n;
}

static void middle(struct gk_widget *w, int *x, int *y) {
    int bx, by, bw, bh;
    gk_bounds(w, &bx, &by, &bw, &bh);
    *x = bx + bw / 2;
    *y = by + bh / 2;
}

static void click_on(struct gk_widget *w) {
    int x, y;
    middle(w, &x, &y);
    gkf_click(x, y);
}

/* the box around what was presented since the last look is inside the widget's place */
static int presented_within(struct gk_widget *w) {
    int x, y, width, height, bx, by, bw, bh;
    gk_bounds(w, &bx, &by, &bw, &bh);
    if(!gkf_presented(&x, &y, &width, &height)) return 0;
    return x >= bx && y >= by && x + width <= bx + bw && y + height <= by + bh;
}

static struct gk *window(const char *title, int w, int h) {
    struct gk *ui;
    gkf_reset(0, 0);
    ui = gk_open(title, w, h, WIN_RESIZABLE);
    gkf_focus(1);
    return ui;
}

/**** libgui under the toolkit: the view, a button's UTF-8 label, a character, text cut ****/

static void libgui(void) {
    struct gw *win;
    struct gw_view v;
    int len;
    gkf_reset(0, 0);
    win = gw_open("libgui", 200, 100, 0, GW_PAPER);
    CHECK(win != NULL);
    gw_button(win, 10, 10, 80, 24, "Caf\xc3\xa9", GW_SHELL, GW_INK);   /* UTF-8, centred by its 4 characters */
    gw_get_view(win, &v);
    CHECK(v.ox == 0 && v.oy == 0 && v.l == 0 && v.t == 0 && v.r == 200 && v.b == 100);
    gw_origin(win, 100, 50);                            /* drawing's 0, 0 moved; the clip narrowed in its coordinates */
    gw_clip_to(win, 0, 0, 20, 20);
    gw_rect(win, -10, -10, 100, 100, 0x123456);
    gw_get_view(win, &v);
    CHECK(v.ox == 100 && v.oy == 50 && v.l == 100 && v.t == 50 && v.r == 120 && v.b == 70);
    gw_view_reset(win);
    gw_text_cut(win, 0, 80, "abcdefgh", 4, GW_INK);     /* three characters and the dot */
    gw_present_all(win);
    CHECK(gkf_text(10 + (80 - 32) / 2, 10 + 4, "Caf\xc3\xa9", GW_INK) && gkf_pixel(11, 11) == GW_SHELL);
    CHECK(gkf_pixel(100, 50) == 0x123456 && gkf_pixel(119, 69) == 0x123456 && gkf_pixel(120, 70) == GW_PAPER &&
          gkf_pixel(99, 49) == GW_PAPER);
    CHECK(gkf_text(0, 80, "abc\xe2\x88\x99", GW_INK) && !gkf_text(0, 80, "abcd", GW_INK));
    CHECK(gw_char("\xc3\xa9!", 3, &len) == 0x82 && len == 2);   /* é is 437's 0x82 */
    CHECK(gw_char("\xc3", 1, &len) == 0xC3 && len == 1);          /* a byte that starts none: as it is */
    gw_close(win);
}

/**** layout ****/

static void layout(void) {
    struct gk *ui = window("Layout", 400, 300);
    struct gk_widget *label, *row, *a, *field, *c, *tall, *col, *hidden, *pages, *p0, *p1, *spacer, *bottom;
    int x, y, w, h;
    CHECK(ui && gkf_open() && gkf_w() == 400 && gkf_h() == 300 && !strcmp(gkf_title(), "Layout") && gkf_resizable());
    gk_pad(gk_root(ui), 8);
    label = gk_label(gk_root(ui), "Hello");
    row = gk_hbox(gk_root(ui), 4);
    a = gk_button(row, "A", 1);
    field = gk_field(row, 2, 32, 0);
    c = gk_button(row, "C", 3);
    hidden = gk_label(gk_root(ui), "not here");
    gk_show(hidden, 0);
    tall = gk_hbox(gk_root(ui), 0);
    col = gk_vbox(tall, 2);
    gk_label(col, "one");
    gk_label(col, "two\nthree");
    pages = gk_pages(tall);
    p0 = gk_label(pages, "first page");
    p1 = gk_label(pages, "second page, longer");
    (void)p1;
    spacer = gk_spacer(gk_root(ui));
    bottom = gk_button(gk_root(ui), "Bottom", 4);
    gk_align(bottom, GK_END);
    CHECK(gk_ok(ui));
    CHECK(run(ui) == 0 && ngot == 0);
    CHECK(gkf_presented(&x, &y, &w, &h) == 1 && x == 0 && y == 0 && w == 400 && h == 300);
    gk_bounds(label, &x, &y, &w, &h);
    CHECK(x == 8 && y == 8 && w == 40 && h == 22);
    CHECK(gkf_text(8, 11, "Hello", GW_INK));
    gk_bounds(row, &x, &y, &w, &h);                     /* a box in a box fills it across */
    CHECK(x == 8 && y == 30 && w == 384 && h == 24);
    gk_bounds(a, &x, &y, &w, &h);
    CHECK(x == 8 && y == 30 && w == 64 && h == 24);
    gk_bounds(field, &x, &y, &w, &h);                   /* it grows: what is left, centred in the row's height */
    CHECK(x == 76 && y == 31 && w == 384 - 64 - 64 - 8 && h == 22);
    gk_bounds(c, &x, &y, &w, &h);
    CHECK(x == 8 + 384 - 64 && w == 64);
    gk_bounds(hidden, &x, &y, &w, &h);
    CHECK(!gk_shown(hidden) && w == 0 && h == 0);
    gk_bounds(col, &x, &y, &w, &h);
    CHECK(y == 54 && h == 22 + 2 + 38 && w == 5 * 8);
    gk_bounds(pages, &x, &y, &w, &h);
    CHECK(x == 8 + 40 && w == 384 - 40 && h == 62);     /* it grows: the rest of the row, and its height */
    CHECK(gk_value(pages) == 0 && gkf_text(48, y + (h - 16) / 2, "first page", GW_INK));
    gk_set_value(pages, 1);
    run(ui);
    CHECK(gkf_text(48, y + (h - 16) / 2, "second page, longer", GW_INK));
    gk_bounds(p0, &x, &y, &w, &h);
    CHECK(w == 384 - 40);
    gk_bounds(tall, &x, &y, &w, &h);                    /* the widest page is the pages' natural width */
    CHECK(w == 384);
    gk_bounds(bottom, &x, &y, &w, &h);                  /* the spacer took the rest; at the right */
    CHECK(y == 300 - 8 - 24 && x == 400 - 8 - 68 && w == 68);
    gk_bounds(spacer, &x, &y, &w, &h);
    CHECK(y == 54 + 62 && h == 300 - 8 - 24 - 54 - 62);
    /* shown again: it takes its room, and what is below moves and is painted again */
    gkf_presented(&x, &y, &w, &h);
    gk_show(hidden, 1);
    run(ui);
    gk_bounds(hidden, &x, &y, &w, &h);
    CHECK(gk_shown(hidden) && y == 54 && h == 22);
    gk_bounds(col, &x, &y, &w, &h);
    CHECK(y == 76);
    CHECK(gkf_text(8, 54 + 3, "not here", GW_INK));
    /* a new size: laid out again, the field growing with it */
    gkf_resize(600, 200);
    CHECK(run(ui) == 0 && count(GK_EV_RESIZE) == 1 && heard(GK_EV_RESIZE, 0)->x == 600 && heard(GK_EV_RESIZE, 0)->y == 200);
    CHECK(gkf_w() == 600 && gkf_h() == 200);
    gk_bounds(field, &x, &y, &w, &h);
    CHECK(w == 584 - 64 - 64 - 8);
    CHECK(gkf_text(8, 11, "Hello", GW_INK));
    gk_gap(row, 0);                                     /* no room between them: the field takes it */
    run(ui);
    gk_bounds(field, &x, &y, &w, &h);
    CHECK(x == 8 + 64 && w == 584 - 64 - 64);
    gk_close(ui);
    CHECK(!gkf_open());
}

/**** damage ****/

static void damage(void) {
    struct gk *ui = window("Damage", 300, 200);
    struct gk_widget *label, *field, *status;
    int x, y, w, h;
    label = gk_label(gk_root(ui), "abc");
    field = gk_field(gk_root(ui), 1, 40, 0);
    status = gk_status(gk_root(ui));
    gk_set_text(status, "Ready");
    run(ui);
    gkf_presented(&x, &y, &w, &h);
    CHECK(gkf_text(10, 22 + 22 + 3, "Ready", GW_INK));
    gk_set_text(label, "xyz");                          /* the same size: the label alone */
    run(ui);
    CHECK(presented_within(label) && gkf_text(0, 3, "xyz", GW_INK));
    gk_focus(field);
    run(ui);
    gkf_presented(&x, &y, &w, &h);
    gkf_type("hi");
    run(ui);
    CHECK(presented_within(field) && count(GK_EV_CHANGE) == 2 && !strcmp(gk_text(field), "hi"));
    gk_set_text(status, "Typed");
    run(ui);
    CHECK(presented_within(status) && gkf_text(10, 44 + 3, "Typed", GW_INK));
    run(ui);                                            /* nothing changed: nothing presented */
    CHECK(gkf_presented(&x, &y, &w, &h) == 0);
    gk_close(ui);
}

/**** buttons, check boxes, switches, radio groups ****/

static void buttons(void) {
    struct gk *ui = window("Buttons", 300, 240);
    struct gk_widget *ok, *off, *check, *sw, *radio;
    int x, y, w, h;
    ok = gk_button(gk_root(ui), "OK", 10);
    off = gk_button(gk_root(ui), "Off", 11);
    gk_enable(off, 0);
    check = gk_check(gk_root(ui), "Wi-Fi", 12, 0);
    sw = gk_switch(gk_root(ui), "Join at startup", 13, 1);
    radio = gk_radio(gk_root(ui), 14, 0);
    CHECK(gk_radio_add(radio, "Slow") == 0 && gk_radio_add(radio, "Normal") == 1 && gk_radio_add(radio, "Fast") == 2);
    gk_set_value(radio, 1);
    run(ui);
    gk_bounds(ok, &x, &y, &w, &h);
    CHECK(gkf_pixel(x + 2, y + 2) == GW_SHELL && gkf_text(x + (w - 16) / 2, y + 4, "OK", GW_INK));
    click_on(ok);
    run(ui);
    CHECK(count(GK_EV_CLICK) == 1 && heard(GK_EV_CLICK, 10) && heard(GK_EV_CLICK, 10)->w == ok);
    CHECK(gk_focused(ui) == ok && gkf_pixel(x + 2, y + 2) == GW_TEAL);   /* the focus, lit as a dialog's button */
    gkf_pointer(x + 5, y + 5, WIN_BUTTON_LEFT);         /* held down: ink; let go away from it: no click */
    run(ui);
    CHECK(gkf_pixel(x + 2, y + 2) == GW_INK);
    gkf_pointer(x + 5, y + 100, WIN_BUTTON_LEFT);
    gkf_pointer(x + 5, y + 100, 0);
    run(ui);
    CHECK(count(GK_EV_CLICK) == 0);
    gkf_key(' ', 0);
    gkf_key('\r', 0);
    run(ui);
    CHECK(count(GK_EV_CLICK) == 2);
    click_on(off);
    run(ui);
    CHECK(ngot == 0 && gk_focused(ui) == ok);
    gk_bounds(off, &x, &y, &w, &h);
    CHECK(gkf_pixel(x + 2, y + 2) == GW_BAND && gkf_text(x + (w - 24) / 2, y + 4, "Off", gk_theme(ui)->grey) &&
          gk_theme(ui)->grey == 0xA3B4B3);                /* greyed as the desktop greys its items */
    /* a check box: a click, Space */
    gk_bounds(check, &x, &y, &w, &h);
    CHECK(gkf_text(x + 20, y + 3, "Wi-Fi", GW_INK) && gkf_pixel(x + 5, y + 8) == SHEET);
    click_on(check);
    run(ui);
    CHECK(heard(GK_EV_CHANGE, 12) && heard(GK_EV_CHANGE, 12)->value == 1 && gk_value(check) == 1);
    CHECK(gkf_pixel(x + 1, y + 5) == GW_TEAL);
    gkf_key(' ', 0);
    run(ui);
    CHECK(heard(GK_EV_CHANGE, 12) && heard(GK_EV_CHANGE, 12)->value == 0 && gk_value(check) == 0);
    gk_set_value(check, 1);                             /* the program's change: no event */
    run(ui);
    CHECK(ngot == 0 && gk_value(check) == 1);
    /* a switch */
    gk_bounds(sw, &x, &y, &w, &h);
    CHECK(gk_value(sw) == 1 && gkf_pixel(x + 8, y + 4) == GW_TEAL && gkf_text(x + 32, y + 3, "Join at startup", GW_INK));
    click_on(sw);
    run(ui);
    CHECK(heard(GK_EV_CHANGE, 13) && gk_value(sw) == 0 && gkf_pixel(x + 8, y + 4) == GW_MUTED);
    /* a radio group: a click on an option, the arrows */
    gk_bounds(radio, &x, &y, &w, &h);
    CHECK(h == 3 * 22 && gkf_text(x + 18, y + 22 + 3, "Normal", GW_INK) && gkf_pixel(x + 6, y + 22 + 11) == GW_TEAL);
    gkf_click(x + 30, y + 2 * 22 + 10);
    run(ui);
    CHECK(heard(GK_EV_CHANGE, 14) && heard(GK_EV_CHANGE, 14)->value == 2 && gk_value(radio) == 2);
    gkf_key(WIN_KEY_UP, 0);
    gkf_key(WIN_KEY_UP, 0);
    gkf_key(WIN_KEY_UP, 0);
    run(ui);
    CHECK(count(GK_EV_CHANGE) == 2 && gk_value(radio) == 0);
    gk_close(ui);
}

/**** the line of text ****/

static void field(void) {
    struct gk *ui = window("Field", 300, 120);
    struct gk_widget *f, *small, *secret, *line;
    int x, y, w, h;
    f = gk_field(gk_root(ui), 1, 64, 0);
    small = gk_field(gk_root(ui), 2, 5, 0);
    secret = gk_field(gk_root(ui), 3, 32, GK_PASSWORD);
    line = gk_field(gk_root(ui), 4, 64, 0);
    gk_field_hint(line, "Type a message");
    run(ui);
    gk_bounds(line, &x, &y, &w, &h);
    CHECK(gkf_text(x + 4, y + 3, "Type a message", GW_MUTED));
    gk_focus(f);
    gkf_type("caf\xc3\xa9 au lait");
    run(ui);
    CHECK(!strcmp(gk_text(f), "caf\xc3\xa9 au lait") && count(GK_EV_CHANGE) == 12);   /* é whole: one change */
    gk_bounds(f, &x, &y, &w, &h);
    CHECK(gkf_text(x + 4, y + 3, "caf\xc3\xa9 au lai", GW_INK));   /* (the cursor is on the last's edge) */
    CHECK(gkf_pixel(x, y) == GW_TEAL);                  /* its border lit: it has the keys */
    gkf_key(WIN_KEY_HOME, 0);
    gkf_key(WIN_KEY_RIGHT, 0);
    gkf_key(WIN_KEY_RIGHT, 0);
    gkf_key(WIN_KEY_RIGHT, 0);
    gkf_key(WIN_KEY_RIGHT, WIN_MOD_SHIFT);              /* é, whole */
    gkf_key('e', 0);
    run(ui);
    CHECK(!strcmp(gk_text(f), "cafe au lait"));
    gkf_key(8, 0);                                      /* Backspace */
    gkf_key(0xc3, 0);                                   /* é typed as its two bytes */
    gkf_key(0xa9, 0);
    gkf_key(WIN_KEY_END, 0);
    gkf_key(8, 0);
    gkf_key(WIN_KEY_LEFT, 0);
    gkf_key(WIN_KEY_DELETE, 0);
    run(ui);
    CHECK(!strcmp(gk_text(f), "caf\xc3\xa9 au la"));
    gkf_key(WIN_KEY_LEFT, WIN_MOD_SHIFT);
    gkf_key(WIN_KEY_LEFT, WIN_MOD_SHIFT);
    gkf_key(3, 0);                                      /* Ctrl+C: the selection, to the clipboard */
    run(ui);
    CHECK(!strcmp(gkf_clip(), "la"));
    gkf_key(24, 0);                                     /* Ctrl+X */
    run(ui);
    CHECK(!strcmp(gk_text(f), "caf\xc3\xa9 au ") && !strcmp(gkf_clip(), "la") && gkf_clips() == 2);
    gkf_key(1, 0);                                      /* Ctrl+A, then typing replaces it all */
    gkf_type("new");
    run(ui);
    CHECK(!strcmp(gk_text(f), "new"));
    gkf_key('\r', 0);
    run(ui);
    CHECK(heard(GK_EV_ACTIVATE, 1) && heard(GK_EV_ACTIVATE, 1)->value == 3);
    /* the mouse: a click places the cursor, a drag selects, a double-click takes a word */
    gk_set_text(f, "one two three");
    run(ui);
    gkf_click(x + 4 + 4 * 8 + 2, y + 10);
    gkf_type("X");
    run(ui);
    CHECK(!strcmp(gk_text(f), "one Xtwo three"));
    gkf_advance(1000);
    gkf_drag(x + 4 + 0 * 8, y + 10, x + 4 + 3 * 8, y + 10);
    gkf_type("1");
    run(ui);
    CHECK(!strcmp(gk_text(f), "1 Xtwo three"));
    gkf_advance(1000);
    gkf_click(x + 4 + 9 * 8, y + 10);
    gkf_click(x + 4 + 9 * 8, y + 10);
    gkf_type("3");
    run(ui);
    CHECK(!strcmp(gk_text(f), "1 Xtwo 3"));
    /* without the window's focus: nothing reaches the clipboard, and the border is not lit */
    gkf_key(1, 0);
    gkf_focus(0);
    gkf_key(3, 0);
    run(ui);
    CHECK(gkf_clips() == 2 && gkf_pixel(x, y) == GW_LINE);
    gkf_focus(1);
    /* a full field: no more than its bytes, cut at a character's start */
    gk_focus(small);
    gkf_type("abcd\xc3\xa9");
    run(ui);
    CHECK(!strcmp(gk_text(small), "abcd"));
    /* a password field: dots, and nothing copied */
    gk_focus(secret);
    gkf_type("hunter2");
    gkf_key(1, 0);
    gkf_key(3, 0);
    gkf_key(24, 0);
    run(ui);
    gk_bounds(secret, &x, &y, &w, &h);
    CHECK(!strcmp(gk_text(secret), "hunter2") && gkf_clips() == 2);
    CHECK(gkf_text(x + 4, y + 3, "\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2", GW_INK) && !gkf_text(x + 4, y + 3, "hu", GW_INK));
    CHECK(gk_field_history(secret, 10) < 0);
    gk_field_wipe(secret);
    CHECK(!strcmp(gk_text(secret), ""));
    /* history: Up and Down bring back what was remembered, and what was being typed after the newest */
    CHECK(gk_field_history(line, 3) == 0);
    gk_focus(line);
    gk_set_text(line, "first");
    gk_field_remember(line);
    gk_set_text(line, "second");
    gk_field_remember(line);
    gk_set_text(line, "draft");
    gkf_key(WIN_KEY_UP, 0);
    run(ui);
    CHECK(!strcmp(gk_text(line), "second"));
    gkf_key(WIN_KEY_UP, 0);
    gkf_key(WIN_KEY_UP, 0);
    run(ui);
    CHECK(!strcmp(gk_text(line), "first"));
    gkf_key(WIN_KEY_DOWN, 0);
    gkf_key(WIN_KEY_DOWN, 0);
    run(ui);
    CHECK(!strcmp(gk_text(line), "draft"));
    gkf_key(WIN_KEY_DOWN, 0);                           /* past the newest: nothing more */
    run(ui);
    CHECK(!strcmp(gk_text(line), "draft") && count(GK_EV_CHANGE) == 0);
    gk_set_text(line, "third");
    gk_field_remember(line);
    gk_set_text(line, "fourth");
    gk_field_remember(line);                            /* three kept: "first" went */
    gk_set_text(line, "");
    gkf_key(WIN_KEY_UP, 0);
    gkf_key(WIN_KEY_UP, 0);
    gkf_key(WIN_KEY_UP, 0);
    gkf_key(WIN_KEY_UP, 0);
    run(ui);
    CHECK(!strcmp(gk_text(line), "second"));
    /* a long line scrolls to keep the cursor in view */
    gk_focus(f);
    gk_set_text(f, "");
    gkf_type("0123456789012345678901234567890123456789");
    run(ui);
    gk_bounds(f, &x, &y, &w, &h);
    CHECK(gkf_text(x + 4 + ((w - 8) / 8 - 1 - 9) * 8, y + 3, "12345678", GW_INK));
    gk_close(ui);
}

/**** lists ****/

static void list(void) {
    struct gk *ui = window("List", 300, 200);
    struct gk_widget *l;
    char b[32];
    int i, x, y, w, h, cols[1] = { 120 };
    l = gk_list(gk_root(ui), 5);
    gk_size(l, 0, 6 * GK_ROW_H + 2);
    gk_grow(l, 1, 0);
    for(i = 0; i < 20; i++) {
        snprintf(b, sizeof(b), "row %d\t%d dB", i, -40 - i);
        CHECK(gk_list_add(l, b) == i);
    }
    gk_list_columns(l, 1, cols);
    run(ui);
    gk_bounds(l, &x, &y, &w, &h);
    CHECK(h == 6 * 18 + 2 && gk_list_count(l) == 20 && gk_value(l) == -1);
    CHECK(gkf_text(x + 6, y + 2, "row 0", GW_INK) && gkf_text(x + 6 + 120, y + 2, "-40 dB", GW_INK));
    CHECK(gkf_pixel(x + w - 1 - 12 + 4, y + 4) == GW_MUTED);    /* the thumb, at the top */
    gkf_click(x + 20, y + 1 + 2 * 18 + 5);
    run(ui);
    CHECK(heard(GK_EV_CHANGE, 5) && heard(GK_EV_CHANGE, 5)->value == 2 && gk_focused(ui) == l);
    CHECK(gkf_pixel(x + 3, y + 1 + 2 * 18 + 1) == PICK && !strcmp(gk_text(l), "row 2\t-42 dB"));
    gkf_click(x + 20, y + 1 + 2 * 18 + 5);              /* the same row again, at once: activated */
    run(ui);
    CHECK(heard(GK_EV_ACTIVATE, 5) && heard(GK_EV_ACTIVATE, 5)->value == 2);
    gkf_advance(1000);
    gkf_key(WIN_KEY_DOWN, 0);
    gkf_key(WIN_KEY_PGDN, 0);
    run(ui);
    CHECK(gk_value(l) == 8 && count(GK_EV_CHANGE) == 2);
    CHECK(gkf_text(x + 6, y + 2 + 5 * 18, "row 8", GW_INK));   /* scrolled to keep it in view */
    gkf_key(WIN_KEY_END, 0);
    gkf_key('\r', 0);
    run(ui);
    CHECK(gk_value(l) == 19 && heard(GK_EV_ACTIVATE, 5) && heard(GK_EV_ACTIVATE, 5)->value == 19);
    gkf_key(WIN_KEY_HOME, 0);
    gkf_wheel(x + 20, y + 20, 3);                       /* the wheel moves the view, not the selection */
    run(ui);
    CHECK(gk_value(l) == 0 && gkf_text(x + 6, y + 2, "row 3", GW_INK));
    gkf_key('r', 0);                                    /* the next line that starts with r */
    run(ui);
    CHECK(gk_value(l) == 1);
    /* the scroll bar: a click below the thumb is a page, the thumb dragged to the end shows the end */
    gk_list_reveal(l, 0);
    run(ui);
    gkf_click(x + w - 6, y + h - 4);
    run(ui);
    CHECK(gkf_text(x + 6, y + 2, "row 6", GW_INK));
    gkf_drag(x + w - 6, y + 40, x + w - 6, y + h + 50);
    run(ui);
    CHECK(gkf_text(x + 6, y + 2 + 5 * 18, "row 19", GW_INK));
    /* lines added and taken away keep the selection on its line */
    gk_set_value(l, 5);
    CHECK(gk_list_insert(l, 0, "new first") == 0 && gk_value(l) == 6);
    gk_list_remove(l, 0);
    CHECK(gk_value(l) == 5 && !strcmp(gk_list_text(l, 5), "row 5\t-45 dB"));
    gk_list_remove(l, 5);
    CHECK(gk_value(l) == 4 || gk_value(l) == 5);
    CHECK(gk_list_set(l, 0, "renamed") == 0 && !strcmp(gk_list_text(l, 0), "renamed"));
    CHECK(gk_list_text(l, 99) == NULL && gk_list_set(l, 99, "x") < 0);
    gk_list_clear(l);
    run(ui);
    CHECK(gk_list_count(l) == 0 && gk_value(l) == -1 && !strcmp(gk_text(l), ""));
    gkf_key(WIN_KEY_DOWN, 0);
    run(ui);
    CHECK(ngot == 0);
    gk_close(ui);
}

/**** a scroll bar, tabs, status bars ****/

static void bars(void) {
    struct gk *ui = window("Bars", 400, 300);
    struct gk_widget *tabs, *p0, *p1, *p2, *bar, *status, *in0, *in1;
    int x, y, w, h, i;
    tabs = gk_tabs(gk_root(ui), 20, GK_CLOSABLE);
    p0 = gk_tab(tabs, "Wi-Fi");
    p1 = gk_tab(tabs, "Network");
    p2 = gk_tab(tabs, "About");
    in0 = gk_button(p0, "Scan", 21);
    in1 = gk_field(p1, 22, 20, 0);
    gk_label(p2, "SurfOS");
    bar = gk_scrollbar(gk_root(ui), 23, 1);
    gk_scroll_set(bar, 100, 10, 0);
    status = gk_status(gk_root(ui));
    gk_set_text(status, "Left");
    gk_status_right(status, "right");
    run(ui);
    CHECK(gk_tab_count(tabs) == 3 && gk_value(tabs) == 0 && !strcmp(gk_text(tabs), "Wi-Fi"));
    gk_bounds(tabs, &x, &y, &w, &h);
    CHECK(gkf_pixel(x + 9, y + 5) == GW_TEAL && gkf_text(x + 8 + 7, y + 7, "Wi-Fi", GW_PAPER));
    CHECK(gkf_text(x + 8 + 5 * 8 + 14 + 14 + 4 + 7, y + 7, "Network", GW_INK));
    gk_bounds(in0, &x, &y, &w, &h);
    CHECK(y == 30 && gkf_text(x + (w - 32) / 2, y + 4, "Scan", GW_INK));
    gk_bounds(tabs, &x, &y, &w, &h);
    gkf_click(x + 8 + 5 * 8 + 28 + 4 + 20, y + 10);     /* Network */
    run(ui);
    CHECK(heard(GK_EV_CHANGE, 20) && heard(GK_EV_CHANGE, 20)->value == 1 && gk_value(tabs) == 1);
    gk_bounds(in1, &x, &y, &w, &h);
    CHECK(w > 0 && gkf_pixel(x + 5, y + 5) == SHEET);
    gk_bounds(in0, &x, &y, &w, &h);
    CHECK(gkf_pixel(x + 5, y + 23) == GW_PAPER);        /* the first page is put away (below the field) */
    gkf_key(WIN_KEY_RIGHT, 0);                          /* the strip has the keys */
    run(ui);
    CHECK(gk_value(tabs) == 2 && gkf_text(0, 30 + 3, "SurfOS", GW_INK));
    gk_bounds(tabs, &x, &y, &w, &h);
    gkf_click(x + 8 + 5 * 8 + 28 + 4 + 7 * 8 + 28 + 4 + 5 * 8 + 28 - 8, y + 10);   /* About's x */
    run(ui);
    CHECK(heard(GK_EV_TAB_CLOSE, 20) && heard(GK_EV_TAB_CLOSE, 20)->value == 2 && gk_value(tabs) == 2);
    gk_tab_remove(tabs, 2);
    run(ui);
    CHECK(gk_tab_count(tabs) == 2 && gk_value(tabs) == 1 && !strcmp(gk_text(tabs), "Network"));
    gk_tab_label(tabs, 1, "Ethernet");
    run(ui);
    CHECK(!strcmp(gk_text(tabs), "Ethernet"));
    for(i = 0; i < 12; i++) gk_tab(tabs, "A long tab label");   /* too many: they scroll, the one shown in view */
    gk_set_value(tabs, 13);
    run(ui);
    CHECK(gk_value(tabs) == 13 && gkf_pixel(x + w - 36 + 2, y + 6) == GW_SHELL);
    CHECK(gkf_pixel(x + w - 36 + 18 + 2, y + 6) == GW_BAND);   /* the last is in view: nothing more after it */
    gk_bounds(tabs, &x, &y, &w, &h);
    gkf_click(x + w - 36 + 4, y + 10);                  /* the left arrow: the strip one tab back */
    gkf_click(x + w - 36 + 4, y + 10);
    run(ui);
    CHECK(gk_value(tabs) == 13 && gkf_pixel(x + w - 36 + 18 + 2, y + 6) == GW_SHELL);   /* more after: the right one lit */
    CHECK(ngot == 0);                                   /* the arrows move the strip, not the page */
    gkf_key(WIN_KEY_HOME, 0);                           /* the first page, and its tab in view */
    run(ui);
    CHECK(gk_value(tabs) == 0 && gkf_text(x + 8 + 7, y + 7, "Wi-Fi", GW_PAPER) && gkf_pixel(x + w - 36 + 2, y + 6) == GW_BAND);
    /* the scroll bar: a click after the thumb is a page; a drag to the end shows the end */
    gk_bounds(bar, &x, &y, &w, &h);
    CHECK(h == 12 && w == 400);
    gkf_click(x + w - 10, y + 6);
    run(ui);
    CHECK(heard(GK_EV_CHANGE, 23) && gk_value(bar) == 10);
    gkf_drag(x + 50, y + 6, x + 1000, y + 6);         /* (the thumb: 40 pixels from 40) */
    run(ui);
    CHECK(gk_value(bar) == 90);
    gk_bounds(status, &x, &y, &w, &h);
    CHECK(gkf_text(x + 10, y + 3, "Left", GW_INK) && gkf_text(x + w - 24 - 40, y + 3, "right", GW_MUTED));
    CHECK(gkf_pixel(x + 50, y) == GW_LINE && gkf_pixel(x + 50, y + 10) == GW_BAND);
    gk_close(ui);
}

/**** the focus ****/

static void focus(void) {
    struct gk *ui = window("Focus", 300, 300);
    struct gk_widget *a, *b, *hid, *off, *pages, *away, *here, *c;
    a = gk_button(gk_root(ui), "A", 1);
    b = gk_field(gk_root(ui), 2, 10, 0);
    hid = gk_button(gk_root(ui), "Hidden", 3);
    gk_show(hid, 0);
    off = gk_button(gk_root(ui), "Off", 4);
    gk_enable(off, 0);
    pages = gk_pages(gk_root(ui));
    away = gk_button(pages, "Away", 5);
    here = gk_button(pages, "Here", 6);
    gk_set_value(pages, 1);
    c = gk_check(gk_root(ui), "C", 7, 0);
    gk_label(gk_root(ui), "no focus here");
    run(ui);
    CHECK(gk_focused(ui) == NULL);
    gkf_key(9, 0);
    run(ui);
    CHECK(gk_focused(ui) == a);
    gkf_key(9, 0);
    gkf_key(9, 0);
    run(ui);
    CHECK(gk_focused(ui) == here);
    gkf_key(9, 0);
    gkf_key(9, 0);                                      /* round to the first */
    run(ui);
    CHECK(gk_focused(ui) == a);
    gkf_key(WIN_KEY_BACKTAB, 0);
    run(ui);
    CHECK(gk_focused(ui) == c);
    gkf_key(9, WIN_MOD_SHIFT);
    run(ui);
    CHECK(gk_focused(ui) == here);
    gk_set_value(pages, 0);                             /* its page put away: it has the focus no more */
    run(ui);
    CHECK(gk_focused(ui) == NULL);
    gk_focus(b);
    gkf_key('z', 0);
    gkf_key(19, 0);                                     /* Ctrl+S: no field's: the window's */
    run(ui);
    CHECK(!strcmp(gk_text(b), "z") && heard(GK_EV_KEY, 0) && heard(GK_EV_KEY, 0)->value == 19 && !heard(GK_EV_KEY, 0)->w);
    gk_enable(b, 0);
    run(ui);
    CHECK(gk_focused(ui) == NULL);
    gkf_key('q', 0);
    run(ui);
    CHECK(heard(GK_EV_KEY, 0) && heard(GK_EV_KEY, 0)->value == 'q');
    (void)away;
    gk_close(ui);
}

/**** areas ****/

static struct { int calls, w, h, sx, sy, cx, cy, cw, ch; } drawn;

static void draw(struct gk_widget *area, struct gw *win, const struct gk_view *v, void *data) {
    (void)area;
    CHECK(data == &drawn);
    drawn.calls++;
    drawn.w = v->w;
    drawn.h = v->h;
    drawn.sx = v->sx;
    drawn.sy = v->sy;
    drawn.cx = v->cx;
    drawn.cy = v->cy;
    drawn.cw = v->cw;
    drawn.ch = v->ch;
    gw_rect(win, -50, -50, 5000, 5000, 0x123456);       /* clipped to the area */
    gw_text(win, 2, 2, "area", 0xFFFFFF);
}

static void areas(void) {
    struct gk *ui = window("Areas", 300, 200);
    struct gk_widget *top, *area, *after;
    const struct gk_event *e;
    int x, y, w, h;
    top = gk_field(gk_root(ui), 1, 10, 0);
    area = gk_area(gk_root(ui), 30, GK_FOCUSABLE, draw, &drawn);
    after = gk_button(gk_root(ui), "After", 2);
    run(ui);
    gk_bounds(area, &x, &y, &w, &h);
    CHECK(x == 0 && y == 22 && w == 300 && h == 200 - 22 - 24);
    CHECK(drawn.calls == 1 && drawn.w == 300 && drawn.h == h && drawn.cw == 300 && drawn.ch == h);
    CHECK(gkf_pixel(x + 100, y + 100) == 0x123456 && gkf_text(x + 2, y + 2, "area", 0xFFFFFF));
    CHECK(gkf_pixel(5, 5) == SHEET);                    /* the field above it, untouched */
    gk_bounds(after, &x, &y, &w, &h);
    CHECK(gkf_pixel(x + 2, y + 2) == GW_SHELL);
    /* the pointer, in its own coordinates; a click gives it the focus */
    gk_bounds(area, &x, &y, &w, &h);
    gkf_pointer(x + 10, y + 20, 0);
    gkf_pointer(x + 10, y + 20, WIN_BUTTON_LEFT);
    gkf_pointer(x + 400, y + 300, WIN_BUTTON_LEFT);     /* held: anywhere */
    gkf_pointer(x + 400, y + 300, 0);
    run(ui);
    CHECK(count(GK_EV_POINTER) >= 4);
    CHECK(got[0].type == GK_EV_POINTER && got[0].x == 10 && got[0].y == 20 && got[0].value == 0);
    e = NULL;
    {
        int i;
        for(i = 0; i < ngot; i++) if(got[i].type == GK_EV_POINTER && got[i].value == WIN_BUTTON_LEFT) e = &got[i];
    }
    CHECK(e && e->clicks == 1 && e->buttons == WIN_BUTTON_LEFT && e->id == 30);
    CHECK(gk_focused(ui) == area && heard(GK_EV_FOCUS, 30) && heard(GK_EV_FOCUS, 30)->value == 1);
    gkf_key('k', 0);
    gkf_key(9, 0);                                      /* Tab moves on: it does not take Tab */
    run(ui);
    CHECK(heard(GK_EV_KEY, 30) && heard(GK_EV_KEY, 30)->value == 'k' && heard(GK_EV_KEY, 30)->w == area);
    CHECK(gk_focused(ui) == after && heard(GK_EV_FOCUS, 30) && heard(GK_EV_FOCUS, 30)->value == 0);
    gk_focus(area);
    run(ui);
    gkf_focus(0);                                       /* the window's focus too */
    run(ui);
    CHECK(heard(GK_EV_FOCUS, 30) && heard(GK_EV_FOCUS, 30)->value == 0);
    gkf_focus(1);
    run(ui);
    CHECK(heard(GK_EV_FOCUS, 30) && heard(GK_EV_FOCUS, 30)->value == 1);
    /* the wheel, with no extent: the program's */
    gkf_wheel(x + 5, y + 5, 3);
    run(ui);
    CHECK(heard(GK_EV_WHEEL, 30) && heard(GK_EV_WHEEL, 30)->value == 3 && heard(GK_EV_WHEEL, 30)->x == 5);
    /* scroll bars in its units: 1000 rows of 18 pixels down, 40 columns of 64 across */
    gk_area_extent(area, 40, 64, 1000, 18);
    run(ui);
    CHECK(drawn.w == 300 - 12 && drawn.h == h - 12 && drawn.sy == 0);
    CHECK(gkf_pixel(x + 300 - 12 + 4, y + 4) == GW_MUTED);  /* the vertical thumb, at the top */
    gkf_wheel(x + 5, y + 5, 3);
    run(ui);
    CHECK(heard(GK_EV_SCROLL, 30) && heard(GK_EV_SCROLL, 30)->y == 3 && drawn.sy == 3);
    gkf_mods(WIN_MOD_SHIFT);                            /* across with Shift, by 64 pixels a column: a column a row */
    gkf_wheel(x + 5, y + 5, 2);
    gkf_mods(WIN_MOD_CTRL);                             /* with Ctrl: the program's (a zoom) */
    gkf_wheel(x + 5, y + 5, -3);
    gkf_mods(0);
    run(ui);
    CHECK(drawn.sx == 2 && drawn.sy == 3 && heard(GK_EV_WHEEL, 30) && heard(GK_EV_WHEEL, 30)->value == -3 &&
          heard(GK_EV_WHEEL, 30)->mods == WIN_MOD_CTRL);
    gk_scroll_to(area, 0, 3);
    run(ui);
    gkf_click(x + 300 - 6, y + h - 12 - 3);              /* below the thumb: a page */
    run(ui);
    CHECK(drawn.sy == 3 + (h - 12) / 18);
    gkf_drag(x + 300 - 6, y + 10, x + 300 - 6, y + 2000);   /* the thumb to the end */
    run(ui);
    CHECK(drawn.sy == 1000 - (h - 12) / 18);
    gk_scroll_to(area, 5, 7);
    run(ui);
    CHECK(drawn.sx == 5 && drawn.sy == 7 && ngot == 0);
    gk_scroll_to(area, 500, -3);                        /* kept within */
    run(ui);
    CHECK(drawn.sx == 40 - (300 - 12) / 64 && drawn.sy == 0);
    gk_area_reveal(area, 0, 500, 1, 1);                 /* row 500 in view: the least scrolling, so at the bottom */
    run(ui);
    CHECK(drawn.sx == 0 && drawn.sy == 500 - (h - 12) / 18 + 1);
    gk_area_reveal(area, 2, 10, 1, 1);                  /* row 10, above: at the top */
    run(ui);
    CHECK(drawn.sx == 0 && drawn.sy == 10);
    /* a part of it damaged: that part painted, and presented */
    gkf_presented(&x, &y, &w, &h);
    gk_damage(area, 10, 10, 20, 30);
    run(ui);
    gk_bounds(area, &x, &y, &w, &h);
    CHECK(drawn.cx == 10 && drawn.cy == 10 && drawn.cw == 20 && drawn.ch == 30);
    CHECK(gkf_presented(&x, &y, &w, &h) == 1 && x == 10 && y == 22 + 10 && w == 20 && h == 30);
    gk_close(ui);
    (void)top;
}

/**** modal boxes ****/

static void boxes(void) {
    struct gk *ui = window("Boxes", 500, 300);
    struct gk_widget *b, *f;
    int x, y, w, h, bx = (500 - 470) / 2, by = (300 - 146) / 2;
    b = gk_button(gk_root(ui), "Behind", 1);
    f = gk_field(gk_root(ui), 2, 10, 0);
    gk_focus(f);
    run(ui);
    gk_message(ui, 40, "Saved", "The sheet was saved.", "sheet.txt");
    run(ui);
    CHECK(gk_box_open(ui) && gk_asking(ui) == 40);
    CHECK(gkf_text(bx + 28, by + 6, "Saved", GW_PAPER) && gkf_text(bx + 16, by + 44, "The sheet was saved.", GW_INK));
    CHECK(gkf_text(bx + 16, by + 66, "sheet.txt", GW_MUTED) && gkf_pixel(bx + 470 + 2, by + 146 + 2) == GW_NAVY);   /* the theme's dark: shadows */
    gkf_type("x");                                      /* a key, a click behind: nobody's */
    gk_bounds(b, &x, &y, &w, &h);
    gkf_click(x + 2, y + 2);
    run(ui);
    CHECK(ngot == 0 && !strcmp(gk_text(f), ""));
    gkf_key('\r', 0);
    run(ui);
    CHECK(count(GK_EV_ANSWER) == 1 && heard(GK_EV_ANSWER, 40)->value == 0 && !gk_box_open(ui));
    CHECK(gk_focused(ui) == f);                         /* the focus back where it was */
    CHECK(gkf_pixel(bx + 470 + 2, by + 146 + 2) == GW_PAPER);   /* painted away */
    /* a question: Right to the second button, Enter; Esc cancels */
    gk_confirm(ui, 41, "Close", "Save the changes?", "", "Save", "Don't save", "Cancel");
    run(ui);
    CHECK(gkf_pixel(bx + 470 - 16 - 88 + 2, by + 146 - 40 + 2) == GW_SHELL);   /* Cancel */
    gkf_key(WIN_KEY_RIGHT, 0);
    gkf_key('\r', 0);
    run(ui);
    CHECK(heard(GK_EV_ANSWER, 41) && heard(GK_EV_ANSWER, 41)->value == 1);
    gk_confirm(ui, 42, "Close", "Save the changes?", "", "Save", "Don't save", "Cancel");
    gkf_key(27, 0);
    run(ui);
    CHECK(heard(GK_EV_ANSWER, 42) && heard(GK_EV_ANSWER, 42)->value == -1);
    gk_confirm(ui, 43, "Close", "Save the changes?", "", "Save", NULL, "Cancel");
    run(ui);
    gkf_click(bx + 470 - 16 - 88 - 8 - 88 + 10, by + 146 - 40 + 10);   /* Save, by the mouse */
    run(ui);
    CHECK(heard(GK_EV_ANSWER, 43) && heard(GK_EV_ANSWER, 43)->value == 0);
    /* a line to type: what was put there is replaced by the first key; Enter answers with the text */
    gk_ask(ui, 44, "Go to", "Line:", "12", 0);
    gkf_type("345\n");
    run(ui);
    CHECK(heard(GK_EV_ANSWER, 44) && heard(GK_EV_ANSWER, 44)->value == 0 && !strcmp(heard(GK_EV_ANSWER, 44)->text, "345"));
    gk_ask(ui, 45, "Join", "Password:", "", GK_PASSWORD);
    gkf_type("secret");
    run(ui);
    CHECK(gkf_text(bx + 16 + 4, (300 - 160) / 2 + 66 + 3, "\xe2\x80\xa2\xe2\x80\xa2", GW_INK));
    gk_message(ui, 46, "Later", "Over the other", "");  /* a box over a box: the other answers -1 first */
    run(ui);
    CHECK(ngot == 1 && got[0].id == 45 && got[0].value == -1 && !strcmp(got[0].text, "") && gk_asking(ui) == 46);
    gk_dismiss(ui);
    run(ui);
    CHECK(heard(GK_EV_ANSWER, 46) && heard(GK_EV_ANSWER, 46)->value == -1 && !gk_box_open(ui));
    gk_ask(ui, 47, "Join", "Password:", "", GK_PASSWORD);
    gkf_type("open sesame");
    gkf_key('\r', 0);
    run(ui);
    CHECK(heard(GK_EV_ANSWER, 47) && !strcmp(heard(GK_EV_ANSWER, 47)->text, "open sesame"));
    gkf_close();                                        /* the close button still reaches the program */
    gk_message(ui, 48, "Busy", "", "");
    run(ui);
    CHECK(heard(GK_EV_CLOSE, 0));
    gk_close(ui);                                       /* with a box open: all of it freed */
}

/**** timers, descriptors, the window's end ****/

/* a burst: more events than gui.h reads at once (GW_QUEUE, 16) that the toolkit takes itself and the program hears
   nothing of (the pointer moving over a label), then a key the program hears. gk_next() gives the key, never 0 first:
   waiting as long as it takes (-1), with time left, and with none (0), the window's queue drained before 0 is said */
static void bursts(void) {
    struct gk *ui = window("Burst", 200, 100);
    struct gk_event ev;
    int ms, k, i, r;
    static const int waits[] = { -1, 1000, 0 };
    gk_label(gk_root(ui), "moves");
    run(ui);
    for(k = 0; k < 3; k++) {
        ms = waits[k];
        for(i = 0; i < 40; i++) gkf_pointer(10 + i % 4, 10, 0);
        gkf_key(WIN_KEY_F12, 0);
        r = gk_next(ui, &ev, ms);
        CHECK(r == 1 && ev.type == GK_EV_KEY && ev.value == WIN_KEY_F12);
        if(r != 1) printf("  (gk_next() with ms %d: %d)\n", ms, r);
        CHECK(gk_next(ui, &ev, 0) == 0);                 /* and then nothing */
    }
    gk_close(ui);
}

/* the review (F2): a timer due again as soon as its handler is done (10 ms, a handler that takes 20) never starves the
   window: a key typed meanwhile is heard, and the window is painted, among the timers */
static void starved(void) {
    struct gk *ui = window("Timers", 200, 100);
    struct gk_widget *l = gk_label(gk_root(ui), "ticks");
    struct gk_event ev;
    int i, keys = 0, timers = 0, presents;
    run(ui);
    gk_timer(ui, 1, 10, 1);
    gkf_advance(10);
    gkf_key(WIN_KEY_F12, 0);
    presents = gkf_presents();
    for(i = 0; i < 20; i++) {
        if(gk_next(ui, &ev, -1) != 1) break;
        if(ev.type == GK_EV_TIMER) {
            timers++;
            gk_set_text(l, timers % 2 ? "tick" : "tock");   /* something to paint */
            gkf_advance(20);                                 /* its handler took 20 ms: the next is due at once */
        } else if(ev.type == GK_EV_KEY) keys++;
    }
    CHECK(keys == 1 && timers >= 10 && gkf_presents() > presents + 5);
    gk_close(ui);
}

static void loop(void) {
    struct gk *ui = window("Loop", 200, 100);
    struct gk_event ev;
    int fds[2];
    char c = 'x';
    gk_label(gk_root(ui), "timers");
    run(ui);
    CHECK(gk_timer(ui, 7, 100, 0) == 0 && gk_timer(ui, 8, 50, 1) == 0);
    run(ui);
    CHECK(ngot == 0);
    gkf_advance(60);
    run(ui);
    CHECK(count(GK_EV_TIMER) == 1 && heard(GK_EV_TIMER, 8));
    gkf_advance(50);
    run(ui);
    CHECK(count(GK_EV_TIMER) == 2 && heard(GK_EV_TIMER, 7) && heard(GK_EV_TIMER, 8));
    gkf_advance(500);                                   /* a repeating timer late: once, and its beat again */
    run(ui);
    CHECK(count(GK_EV_TIMER) == 1 && heard(GK_EV_TIMER, 8));
    gk_timer_stop(ui, 8);
    gkf_advance(500);
    run(ui);
    CHECK(ngot == 0);
    CHECK(pipe(fds) == 0);
    CHECK(gk_watch(ui, fds[0], POLLIN, 9) == 0);
    run(ui);
    CHECK(ngot == 0);
    CHECK(write(fds[1], &c, 1) == 1);
    CHECK(gk_next(ui, &ev, 0) == 1 && ev.type == GK_EV_FD && ev.id == 9 && (ev.value & POLLIN));
    CHECK(read(fds[0], &c, 1) == 1);                    /* read: it is ready no more */
    run(ui);
    CHECK(ngot == 0);
    CHECK(write(fds[1], &c, 1) == 1);
    gk_unwatch(ui, fds[0]);
    run(ui);
    CHECK(ngot == 0);
    close(fds[0]);
    close(fds[1]);
    gkf_close();
    gkf_gone();
    CHECK(run(ui) == -1 && errno == EPIPE && heard(GK_EV_CLOSE, 0));
    CHECK(gk_next(ui, &ev, 0) == -1 && errno == EPIPE);
    gk_close(ui);
    gkf_reset(0, 0);
    gkf_refuse(ENODEV);
    CHECK(gk_open("No desktop", 100, 100, 0) == NULL && errno == ENODEV);
}

/**** track desk's calls: menus, the clipboard read, choosers, the theme; the keys with their modifiers ****/

static const unsigned ocean_roles[WIN_COLORS] = {
    0x245A67, 0xF4F1E6, 0x153D49, 0x6C888C, 0x197D83, 0xF4F1E6, 0xE7E2D2, 0xDEE3D8,
    0xC9CFC4, 0xBFE3DD, 0xFFFDF5, 0x153D49, 0x102E3B, 0xB8E9D9, 0xEE906D, 0xA3B4B3
};

static const struct win_menu_item *entry(const char *label) {
    return gkf_menu_entry(gkf_menu_find(label));
}

static void menus(void) {
    struct gk *ui = window("Menus", 300, 200);
    struct gk_widget *f, *a;
    int file, view, sent, i;
    f = gk_field(gk_root(ui), 1, 40, 0);
    a = gk_area(gk_root(ui), 2, GK_FOCUSABLE, NULL, NULL);
    file = gk_menu(ui, "File");
    CHECK(file == 0);
    CHECK(gk_menu_item(ui, file, 10, "Open", GK_CTRL | 'O', 0) == 0);
    CHECK(gk_menu_item(ui, file, 11, "Save", GK_CTRL | 's', GK_ITEM_OFF) == 0);
    CHECK(gk_menu_line(ui, file) == 0);
    CHECK(gk_menu_item(ui, file, 12, "Quit", GK_CTRL | 'Q', 0) == 0);
    view = gk_menu(ui, "View");
    CHECK(view == 1);
    CHECK(gk_menu_item(ui, view, 20, "Big", WIN_KEY_F5, GK_ITEM_RADIO | GK_ITEM_ON) == 0);
    CHECK(gk_menu_item(ui, view, 21, "Small", WIN_KEY_F6, GK_ITEM_RADIO) == 0);
    CHECK(gk_menu_item(ui, view, 22, "Grid", GK_CTRL | GK_SHIFT | 'K', GK_ITEM_CHECK) == 0);
    CHECK(gk_menu_item(ui, file, 13, "Close", WIN_KEY_DELETE, 0) == 0);  /* at the end of File, after View was made */
    run(ui);
    CHECK(gkf_menu_count() == 10 && gkf_menus_sent() == 1 && gk_menus_shown(ui));
    CHECK(gkf_menu_entry(0)->flags == WIN_MENU_TITLE && !strcmp(gkf_menu_entry(0)->label, "File"));
    CHECK(gkf_menu_find("Open") == 1 && entry("Open")->id == 10 && !strcmp(entry("Open")->keys, "Ctrl+O"));
    CHECK((entry("Save")->flags & WIN_MENU_DISABLED) && !strcmp(entry("Save")->keys, "Ctrl+S"));
    CHECK(gkf_menu_entry(3)->flags == WIN_MENU_SEPARATOR && gkf_menu_find("Close") == 5 && !strcmp(entry("Close")->keys, "Del"));
    CHECK((entry("Big")->flags & WIN_MENU_RADIO) && !strcmp(entry("Big")->keys, "F5") && !(entry("Small")->flags & WIN_MENU_RADIO));
    CHECK(!(entry("Grid")->flags & WIN_MENU_CHECKED) && !strcmp(entry("Grid")->keys, "Ctrl+Shift+K"));
    /* chosen in the desktop's bar */
    CHECK(gkf_menu_choose(10) && !gkf_menu_choose(11));
    run(ui);
    CHECK(ngot == 1 && heard(GK_EV_MENU, 10) && heard(GK_EV_MENU, 10)->value == file && !heard(GK_EV_MENU, 10)->w);
    /* by their shortcuts: no widget has the focus */
    gkf_key(15, WIN_MOD_CTRL);                          /* Ctrl+O */
    gkf_key(17, 0);                                     /* Ctrl+Q from a desktop before the modifiers: the byte alone */
    gkf_key(WIN_KEY_F6, 0);
    gkf_key(11, WIN_MOD_CTRL | WIN_MOD_SHIFT);          /* Ctrl+Shift+K */
    gkf_key(11, WIN_MOD_CTRL);                          /* Ctrl+K: not Grid's */
    gkf_key(19, WIN_MOD_CTRL);                          /* Ctrl+S: greyed, so the window's key */
    run(ui);
    CHECK(count(GK_EV_MENU) == 4 && heard(GK_EV_MENU, 10) && heard(GK_EV_MENU, 12) && heard(GK_EV_MENU, 21) &&
          heard(GK_EV_MENU, 22) && heard(GK_EV_MENU, 21)->value == view);
    CHECK(count(GK_EV_KEY) == 2 && got[ngot - 1].type == GK_EV_KEY && got[ngot - 1].value == 19);
    /* a field's own keys first (Ctrl+A selects in it), and then the shortcuts (Ctrl+O) */
    gk_focus(f);
    gkf_type("abc");
    gkf_key(1, WIN_MOD_CTRL);
    gkf_key(15, WIN_MOD_CTRL);
    run(ui);
    CHECK(heard(GK_EV_MENU, 10) && count(GK_EV_MENU) == 1 && count(GK_EV_KEY) == 0);
    /* an area's shortcuts before the area, which hears the rest */
    gk_focus(a);
    gkf_key(15, WIN_MOD_CTRL);
    gkf_key('x', 0);
    run(ui);
    CHECK(heard(GK_EV_MENU, 10) && heard(GK_EV_KEY, 2) && heard(GK_EV_KEY, 2)->value == 'x' && count(GK_EV_KEY) == 1);
    /* a change is sent again, whole; nothing changed, nothing sent */
    sent = gkf_menus_sent();
    gk_menu_check(ui, 22, 1);
    gk_menu_check(ui, 20, 0);
    gk_menu_check(ui, 21, 1);
    gk_menu_enable(ui, 11, 1);
    run(ui);
    CHECK(gkf_menus_sent() == sent + 1 && (entry("Grid")->flags & WIN_MENU_CHECKED) && !(entry("Big")->flags & WIN_MENU_RADIO));
    CHECK((entry("Small")->flags & WIN_MENU_RADIO) && !(entry("Small")->flags & WIN_MENU_CHECKED) &&
          !(entry("Save")->flags & WIN_MENU_DISABLED));
    run(ui);
    CHECK(gkf_menus_sent() == sent + 1);
    gkf_key(19, WIN_MOD_CTRL);                          /* Save, enabled now */
    run(ui);
    CHECK(heard(GK_EV_MENU, 11));
    /* the Paint review: a box open (a question), the menus go greyed, every title, so nothing is chosen behind it; a
       choice made before that reached the bar is dropped, and a shortcut is the box's key; the close button is still
       the program's to answer. The box gone, the menus come back as they were */
    sent = gkf_menus_sent();
    gk_confirm(ui, 70, "Save the changes?", "", "", "Save", "Don't save", "Cancel");
    run(ui);
    CHECK(gkf_menus_sent() == sent + 1 && (entry("File")->flags & WIN_MENU_DISABLED) &&
          (entry("View")->flags & WIN_MENU_DISABLED) && !(entry("Open")->flags & WIN_MENU_DISABLED));
    CHECK(!gkf_menu_choose(10));                        /* the desktop greys them: none can be chosen */
    gkf_menu_event(10, file);                           /* one chosen just before */
    gkf_key(15, WIN_MOD_CTRL);                          /* Ctrl+O */
    gkf_key(WIN_KEY_F6, 0);
    gkf_close();
    run(ui);
    CHECK(!count(GK_EV_MENU) && !count(GK_EV_KEY) && heard(GK_EV_CLOSE, 0) && gk_box_open(ui));
    gk_dismiss(ui);
    run(ui);
    CHECK(gkf_menus_sent() == sent + 2 && !(entry("File")->flags & WIN_MENU_DISABLED) && heard(GK_EV_ANSWER, 70));
    CHECK(gkf_menu_choose(10));
    run(ui);
    CHECK(heard(GK_EV_MENU, 10));
    gkf_key(19, WIN_MOD_CTRL | 0x80);                   /* a bit no one knows yet: the bits that count */
    gkf_key(19, WIN_MOD_CTRL | WIN_MOD_REPEAT);         /* held down: a repeat, nothing (once a press) */
    gkf_key(19, WIN_MOD_CTRL | WIN_MOD_ALT);            /* Ctrl+Alt+S is another key */
    run(ui);
    CHECK(count(GK_EV_MENU) == 1 && heard(GK_EV_MENU, 11) && count(GK_EV_KEY) == 1 && got[ngot - 1].type == GK_EV_KEY);
    /* no room: the bar's width, eight menus, fifteen entries in a menu */
    CHECK(gk_menu(ui, "A title far too long for the bar") < 0);
    for(i = 0; i < 6; i++) CHECK(gk_menu(ui, "M") == 2 + i);
    CHECK(gk_menu(ui, "M") < 0);
    for(i = 0; i < WIN_MENU_ITEMS; i++) CHECK(gk_menu_item(ui, 2, 100 + i, "Item", 0, 0) == 0);
    CHECK(gk_menu_item(ui, 2, 200, "One more", 0, 0) < 0 && gk_menu_item(ui, 9, 201, "None", 0, 0) < 0);
    run(ui);                                            /* menus with no items are not sent: the kernel takes it */
    CHECK(gkf_menu_count() == 10 + 1 + WIN_MENU_ITEMS && gk_menus_shown(ui));
    gk_close(ui);
    /* a desktop that does not show them yet: the shortcuts still work */
    ui = window("Menus", 300, 200);
    gkf_lacks(1u << WIN_MENU, ENOSYS);
    file = gk_menu(ui, "File");
    gk_menu_item(ui, file, 10, "Open", GK_CTRL | 'O', 0);
    run(ui);
    CHECK(!gk_menus_shown(ui) && gkf_menu_count() == 0);
    gkf_key(15, WIN_MOD_CTRL);
    run(ui);
    CHECK(heard(GK_EV_MENU, 10));
    gkf_lacks(0, 0);
    gk_close(ui);
}

/* 11/05/26, the ABI as the review amended it: submenus, the Help menu, labels as the desktop draws them, titles that
   pass for the desktop's, shortcuts as the keys come */
static void submenus(void) {
    struct gk *ui = window("Submenus", 300, 200);
    struct gk_widget *a;
    static const struct win_menu_item bad[2] = { { 0, WIN_MENU_TITLE, "File", "" }, { 1, 0, "Op\x85n", "" } };
    static const struct win_menu_item help_bad[2] = { { 0, WIN_MENU_TITLE | WIN_MENU_HELP, "Help", "" },
                                                      { 1, 0, "About SurfOS", "" } };
    int view, speed, empty, off, help, i;
    a = gk_area(gk_root(ui), 2, GK_FOCUSABLE, NULL, NULL);
    CHECK(gk_menu(ui, "Apps") < 0 && gk_menu(ui, " window ") < 0 && gk_menu(ui, "SURFOS") < 0 &&
          gk_menu(ui, "\xe2\x89\x8b Go") < 0 && gk_menu(ui, "Sea \xe2\x89\x88") < 0 && gk_menu(ui, "\xc2\xa0" "Apps") < 0);
    view = gk_menu(ui, "View");
    CHECK(view == 0);
    CHECK(gk_menu_item(ui, view, 20, "Zoom in", GK_CTRL | '+', 0) == 0);
    speed = gk_submenu(ui, view, 25, "Speed", 0);
    CHECK(speed == 1);
    CHECK(gk_menu_item(ui, speed, 30, "Slow", 0, GK_ITEM_RADIO) == 0 && gk_menu_line(ui, speed) == 0 &&
          gk_menu_item(ui, speed, 31, "Fast", GK_CTRL | WIN_KEY_TAB, GK_ITEM_RADIO | GK_ITEM_ON) == 0);
    CHECK(gk_submenu(ui, speed, 26, "Deeper", 0) < 0);  /* one level deep */
    empty = gk_submenu(ui, view, 27, "Nothing yet", 0);   /* no items: neither it nor its item is sent */
    off = gk_submenu(ui, view, 28, "Greyed", GK_ITEM_OFF);
    CHECK(empty == 2 && off == 3 && gk_menu_item(ui, off, 50, "Hidden", WIN_KEY_F7, 0) == 0);
    CHECK(gk_menu_item(ui, view, 21, "Bad \x01\x7f\xc2\x85\xff\xe0\x80\x80 end", 0, 0) == 0);
    help = gk_menu(ui, "Help");
    CHECK(help == 4 && gk_menu(ui, "help") < 0 && gk_submenu(ui, help, 29, "Sub", 0) < 0);
    CHECK(gk_menu_item(ui, help, 61, "About SurfOS", 0, 0) < 0 &&   /* the desktop's own Help items, which follow */
          gk_menu_item(ui, help, 61, " keyboard AND mouse\xc2\xa0", 0, 0) < 0);
    for(i = 0; i < WIN_MENU_HELP_ITEMS; i++) CHECK(gk_menu_item(ui, help, 40 + i, "About", 0, 0) == 0);
    CHECK(gk_menu_item(ui, help, 60, "One more", 0, 0) < 0);
    for(i = 3; i < WIN_MENU_ITEMS; i++) CHECK(gk_menu_item(ui, speed, 70 + i, "x", 0, 0) == 0);
    CHECK(gk_menu_item(ui, speed, 99, "x", 0, 0) < 0);
    run(ui);
    CHECK(gk_menus_shown(ui) && gkf_menu_find("Nothing yet") < 0 && gkf_menu_find("Help") >= 0 &&
          entry("Help")->flags == (WIN_MENU_TITLE | WIN_MENU_HELP));
    CHECK(entry("Speed")->flags == WIN_MENU_SUBMENU && entry("Slow")->flags == WIN_MENU_CHILD &&
          entry("Fast")->flags == (WIN_MENU_CHILD | WIN_MENU_RADIO) && !strcmp(entry("Fast")->keys, "Ctrl+Tab") &&
          gkf_menu_entry(gkf_menu_find("Slow") + 1)->flags == (WIN_MENU_SEPARATOR | WIN_MENU_CHILD));
    CHECK(entry("Greyed")->flags == (WIN_MENU_SUBMENU | WIN_MENU_DISABLED) && entry("Hidden")->flags == WIN_MENU_CHILD);
    CHECK(gkf_menu_find("Bad ??????? end") >= 0 && !strcmp(entry("Zoom in")->keys, "Ctrl++"));
    CHECK(gw_menu(gk_gw(ui), bad, 2) < 0 && errno == EINVAL);   /* (the fake holds a label to the kernel's rule) */
    CHECK(gw_menu(gk_gw(ui), help_bad, 2) < 0 && errno == EINVAL);
    /* chosen in a submenu: the menu it is in; never a greyed submenu's */
    CHECK(gkf_menu_choose(30) && !gkf_menu_choose(25) && !gkf_menu_choose(50));
    run(ui);
    CHECK(ngot == 1 && heard(GK_EV_MENU, 30) && heard(GK_EV_MENU, 30)->value == view);
    /* shortcuts as the keys come: Ctrl+Shift+= is '+' with both; Ctrl+Tab an item's, so no move; a greyed submenu's
       nothing; a letter typed into an area is the area's, even when a shortcut has it */
    CHECK(gk_menu_item(ui, view, 22, "Pause", 'P', 0) == 0);
    gkf_key('p', 0);                                    /* no widget has the focus: the shortcut */
    run(ui);
    CHECK(ngot == 1 && heard(GK_EV_MENU, 22));
    gk_focus(a);
    gkf_key('+', WIN_MOD_CTRL | WIN_MOD_SHIFT);
    gkf_key(WIN_KEY_TAB, WIN_MOD_CTRL);
    gkf_key(WIN_KEY_F7, 0);
    gkf_key('p', 0);
    run(ui);
    CHECK(heard(GK_EV_MENU, 20) && heard(GK_EV_MENU, 31) && heard(GK_EV_MENU, 31)->value == view &&
          gk_focused(ui) == a && count(GK_EV_MENU) == 2 && count(GK_EV_KEY) == 2 && got[ngot - 1].value == 'p');
    gk_menu_enable(ui, 28, 1);                          /* the submenu's item by its id: in it, its shortcut works */
    gkf_key(WIN_KEY_F7, 0);
    run(ui);
    CHECK(heard(GK_EV_MENU, 50) && entry("Greyed")->flags == WIN_MENU_SUBMENU);
    gk_close(ui);
}

/* desk's fix round: a drag starts only from a press the window saw over the widget. A button held since a press
   outside a box (nobody's) is no drag over a field once the box has gone, and an area hears it as a hover */
static void drags(void) {
    struct gk *ui = window("Drags", 300, 200);
    struct gk_widget *f, *a;
    int x, y, w, h, i, held = 0;
    f = gk_field(gk_root(ui), 1, 40, 0);
    a = gk_area(gk_root(ui), 2, GK_FOCUSABLE, NULL, NULL);
    gk_grow(a, 1, 1);
    run(ui);
    gk_set_text(f, "abcdef");
    gk_focus(f);
    gkf_key(WIN_KEY_END, 0);
    gk_message(ui, 9, "Over", "", "");
    run(ui);
    gkf_pointer(2, 198, WIN_BUTTON_LEFT);               /* outside the box: nobody's press */
    gkf_key(27, 0);                                     /* the box answered, gone */
    run(ui);
    gk_bounds(f, &x, &y, &w, &h);
    gkf_pointer(x + 4, y + 10, WIN_BUTTON_LEFT);        /* still held, over the field's start */
    gk_bounds(a, &x, &y, &w, &h);
    gkf_pointer(x + 10, y + 10, WIN_BUTTON_LEFT);       /* and over the area */
    gkf_pointer(x + 12, y + 12, 0);
    run(ui);
    for(i = 0; i < ngot; i++) if(got[i].type == GK_EV_POINTER && got[i].buttons) held++;
    CHECK(held == 0 && heard(GK_EV_POINTER, 2));
    gkf_type("X");
    run(ui);
    CHECK(!strcmp(gk_text(f), "abcdefX"));              /* no selection made: the cursor still at the end */
    gk_bounds(a, &x, &y, &w, &h);                       /* a press it saw: a drag, the buttons told */
    gkf_drag(x + 10, y + 10, x + 40, y + 30);
    run(ui);
    for(i = 0, held = 0; i < ngot; i++) if(got[i].type == GK_EV_POINTER && got[i].buttons) held++;
    CHECK(held >= 2);
    gk_close(ui);
}

/* a password field wiped (gk_field_wipe()): every byte it held is zero, its text's buffer whole and a character typed
   partway (its first byte, waiting for the rest); the keys that typed them are gone from gui.h's queue too */
static void wipes(void) {
    struct gk *ui = window("Wipe", 300, 100);
    struct gk_widget *pw = gk_field(gk_root(ui), 1, 16, GK_PASSWORD);
    const struct gk_line *f = pw->k;
    int i, kept = 0;
    run(ui);
    gk_focus(pw);
    gkf_type("s3cr\xc3\xa9t");
    gkf_key(0xC3, 0);                                   /* the first byte of one more */
    run(ui);
    CHECK(f->len == 7 && f->nparts == 1 && (unsigned char)f->part[0] == 0xC3 && !strcmp(gk_text(pw), "s3cr\xc3\xa9t"));
    for(i = 0; i < GW_QUEUE; i++) kept += gk_gw(ui)->queue[i].type == WIN_EV_KEY;
    CHECK(kept == 0);
    gk_field_wipe(pw);
    for(i = 0, kept = 0; i <= f->max; i++) kept += f->buf[i] != 0;
    for(i = 0; i < (int)sizeof(f->part); i++) kept += f->part[i] != 0;
    CHECK(kept == 0 && f->len == 0 && f->nparts == 0 && !strcmp(gk_text(pw), ""));
    gkf_type("ab");                                     /* typing goes on as in an empty field */
    run(ui);
    CHECK(!strcmp(gk_text(pw), "ab"));
    gk_close(ui);
}

/* the games review: a shortcut acts once a press, the keyboard's repeats of its key ignored (Ctrl+Z held: one undo),
   unless its item asks for them (GK_ITEM_REPEAT: Zoom in, held, zooms on); the repeats are nobody's keys either, and a
   field with the focus takes neither (it types repeats of what it types) */
static void repeats(void) {
    struct gk *ui = window("Repeats", 300, 200);
    struct gk_widget *f = gk_field(gk_root(ui), 1, 20, 0);
    int edit = gk_menu(ui, "Edit");
    CHECK(gk_menu_item(ui, edit, 1, "Undo", GK_CTRL | 'Z', 0) == 0 &&
          gk_menu_item(ui, edit, 2, "Zoom in", GK_CTRL | '=', GK_ITEM_REPEAT) == 0);
    run(ui);
    gkf_key(26, WIN_MOD_CTRL);
    gkf_key(26, WIN_MOD_CTRL | WIN_MOD_REPEAT);
    gkf_key(26, WIN_MOD_CTRL | WIN_MOD_REPEAT);
    gkf_key('=', WIN_MOD_CTRL);
    gkf_key('=', WIN_MOD_CTRL | WIN_MOD_REPEAT);
    gkf_key('=', WIN_MOD_CTRL | WIN_MOD_REPEAT);
    run(ui);
    CHECK(count(GK_EV_MENU) == 4 && nth(GK_EV_MENU, 0)->id == 1 && nth(GK_EV_MENU, 1)->id == 2 &&
          nth(GK_EV_MENU, 3)->id == 2 && !count(GK_EV_KEY));
    gk_focus(f);
    gkf_key(26, WIN_MOD_CTRL);
    gkf_key(26, WIN_MOD_CTRL | WIN_MOD_REPEAT);
    gkf_key('a', 0);
    gkf_key('a', WIN_MOD_REPEAT);
    run(ui);
    CHECK(count(GK_EV_MENU) == 1 && !count(GK_EV_KEY) && !strcmp(gk_text(f), "aa"));
    gk_close(ui);
}

/* the review (F3): a cut takes only what the clipboard took. A text longer than WIN_CLIP_MAX, Ctrl+A, Ctrl+X: the
   clipboard refuses it (EINVAL), and the text stays, the program told why (GK_EV_UNCOPIED); a field's cut without
   the focus likewise (EACCES); a cut the clipboard takes deletes */
static void cuts(void) {
    struct gk *ui = window("Cut", 300, 200);
    struct gk_widget *t = gk_text_area(gk_root(ui), 1, 300000, 0), *f = gk_field(gk_root(ui), 2, 40, 0);
    char *big = malloc(200001);
    int clips;
    memset(big, 'a', 200000);
    big[200000] = 0;
    run(ui);
    gk_set_text(t, big);
    gk_focus(t);
    clips = gkf_clips();
    gkf_key(1, WIN_MOD_CTRL);
    gkf_key(24, WIN_MOD_CTRL);
    run(ui);
    CHECK(strlen(gk_text(t)) == 200000 && gkf_clips() == clips && heard(GK_EV_UNCOPIED, 1) &&
          heard(GK_EV_UNCOPIED, 1)->value == EINVAL && !heard(GK_EV_CHANGE, 1));
    gk_set_text(f, "keep");
    gk_focus(f);
    gkf_key(1, WIN_MOD_CTRL);
    gkf_focus(0);
    gkf_key(24, WIN_MOD_CTRL);
    run(ui);
    CHECK(!strcmp(gk_text(f), "keep") && heard(GK_EV_UNCOPIED, 2) && heard(GK_EV_UNCOPIED, 2)->value == EACCES);
    gkf_focus(1);
    gkf_key(1, WIN_MOD_CTRL);
    gkf_key(24, WIN_MOD_CTRL);
    run(ui);
    CHECK(!strcmp(gk_text(f), "") && !strcmp(gkf_clip(), "keep") && !heard(GK_EV_UNCOPIED, 2));
    free(big);
    gk_close(ui);
}

/* the review (F4, F5): a key held down acts once. Enter on a focused button clicks it once, and the program's question
   that the click opens is not answered by Enter's repeats; Space held on a check box turns it once; a line to type
   still takes its repeats. A question answers its first button on Enter, or the one gk_confirm_default() names, and
   Left and Right stop at the ends: Left from the safe answer never comes round to the other */
static void held(void) {
    struct gk *ui = window("Held", 400, 200);
    struct gk_widget *b = gk_button(gk_root(ui), "Delete", 1), *c = gk_check(gk_root(ui), "Wi-Fi", 2, 0);
    run(ui);
    gk_focus(b);
    gkf_key('\r', 0);
    run(ui);
    CHECK(count(GK_EV_CLICK) == 1);
    gk_confirm(ui, 80, "Delete it?", "", "", "Cancel", "Delete", NULL);
    gkf_key('\r', WIN_MOD_REPEAT);
    gkf_key('\r', WIN_MOD_REPEAT);
    gkf_key(27, WIN_MOD_REPEAT);
    run(ui);
    CHECK(gk_box_open(ui) && !count(GK_EV_ANSWER) && !count(GK_EV_CLICK) && !count(GK_EV_KEY));
    gkf_key(WIN_KEY_LEFT, 0);                           /* from Cancel, the left end: it stays */
    gkf_key('\r', 0);
    run(ui);
    CHECK(heard(GK_EV_ANSWER, 80) && heard(GK_EV_ANSWER, 80)->value == 0);
    gk_confirm_default(ui, 81, "Delete it?", "", "", "Delete", NULL, "Cancel", 2);
    gkf_key(WIN_KEY_RIGHT, 0);                          /* Cancel, the right end: it stays */
    gkf_key('\r', 0);
    run(ui);
    CHECK(heard(GK_EV_ANSWER, 81) && heard(GK_EV_ANSWER, 81)->value == -1);
    gk_confirm_default(ui, 82, "Delete it?", "", "", "Delete", NULL, "Cancel", 2);
    gkf_key(WIN_KEY_LEFT, 0);                           /* past the button without a label, to Delete; and no further */
    gkf_key(WIN_KEY_LEFT, 0);
    gkf_key(' ', 0);
    run(ui);
    CHECK(heard(GK_EV_ANSWER, 82) && heard(GK_EV_ANSWER, 82)->value == 0);
    gk_focus(c);
    gkf_key(' ', 0);
    gkf_key(' ', WIN_MOD_REPEAT);
    gkf_key(' ', WIN_MOD_REPEAT);
    run(ui);
    CHECK(gk_value(c) == 1 && count(GK_EV_CHANGE) == 1);
    gk_focus(b);
    gkf_key(' ', 0);
    gkf_key(' ', WIN_MOD_REPEAT);
    run(ui);
    CHECK(count(GK_EV_CLICK) == 1);
    gk_ask(ui, 83, "Name", "Name:", "", 0);
    gkf_key('a', 0);
    gkf_key('a', WIN_MOD_REPEAT);
    gkf_key(' ', WIN_MOD_REPEAT);
    gkf_key('\r', WIN_MOD_REPEAT);                      /* held: no answer */
    run(ui);
    CHECK(gk_box_open(ui) && !count(GK_EV_ANSWER));
    gkf_key('\r', 0);
    run(ui);
    CHECK(heard(GK_EV_ANSWER, 83) && !strcmp(heard(GK_EV_ANSWER, 83)->text, "aa "));
    gk_close(ui);
}

/* the review (F6): Ctrl+[ \ ] ^ and / (or _), which the desktop sends as 27 to 31 with WIN_MOD_CTRL, are the shortcuts
   they are written as, and say so in the bar */
static void punct(void) {
    struct gk *ui = window("Punct", 300, 200);
    int m = gk_menu(ui, "Edit");
    static const struct { unsigned key; const char *keys; } want[] = {
        { 27, "Ctrl+[" }, { 29, "Ctrl+]" }, { 28, "Ctrl+\\" }, { 30, "Ctrl+^" }, { 31, "Ctrl+/" } };
    int i;
    CHECK(gk_menu_item(ui, m, 1, "Outdent", GK_CTRL | '[', 0) == 0 && gk_menu_item(ui, m, 2, "Indent", GK_CTRL | ']', 0) == 0 &&
          gk_menu_item(ui, m, 3, "Back", GK_CTRL | '\\', 0) == 0 && gk_menu_item(ui, m, 4, "Top", GK_CTRL | '^', 0) == 0 &&
          gk_menu_item(ui, m, 5, "Comment", GK_CTRL | '/', 0) == 0);
    run(ui);
    for(i = 0; i < 5; i++) CHECK(!strcmp(gkf_menu_entry(1 + i)->keys, want[i].keys));
    for(i = 0; i < 5; i++) gkf_key(want[i].key, WIN_MOD_CTRL);
    gkf_key(31, WIN_MOD_CTRL | WIN_MOD_SHIFT);          /* Ctrl+_ with Shift, as a keyboard types it: Comment too */
    gkf_key(27, 0);                                     /* Esc is no shortcut of these */
    run(ui);
    CHECK(count(GK_EV_MENU) == 6 && nth(GK_EV_MENU, 0)->id == 1 && nth(GK_EV_MENU, 1)->id == 2 &&
          nth(GK_EV_MENU, 2)->id == 3 && nth(GK_EV_MENU, 3)->id == 4 && nth(GK_EV_MENU, 4)->id == 5 &&
          nth(GK_EV_MENU, 5)->id == 5 && count(GK_EV_KEY) == 1);
    gk_close(ui);
}

/* the review (F7): gk_set_text() says whether the widget holds the text as given */
static void fits(void) {
    struct gk *ui = window("Fits", 300, 200);
    struct gk_widget *f = gk_field(gk_root(ui), 1, 8, 0), *t = gk_text_area(gk_root(ui), 2, 64, 0);
    struct gk_widget *l = gk_label(gk_root(ui), "x");
    CHECK(gk_set_text(f, "abc") == 0 && gk_set_text(f, "abcdefghij") == 1 && !strcmp(gk_text(f), "abcdefgh"));
    CHECK(gk_set_text(f, "a\tb") == 1 && !strcmp(gk_text(f), "a b") && gk_set_text(f, "") == 0);
    CHECK(gk_set_text(f, "1234567\xc3\xa9") == 1 && !strcmp(gk_text(f), "1234567"));   /* é cut whole */
    CHECK(gk_set_text(t, "one\ntwo") == 0 && gk_set_text(t, "one\r\ntwo") == 1 && !strcmp(gk_text(t), "one\ntwo"));
    CHECK(gk_set_text(l, "a label") == 0 && gk_set_text(NULL, "x") == -1);
    gk_close(ui);
}

/* the review (F8): the fake desktop as the kernel and the desktop are. gk_next() with a time and nothing to give says 0
   once the time has passed (the fake's clock moved on by it); a timer comes at its time while gk_next() waits as long
   as it takes; a wait for ever with nothing to come fails (EINTR) instead of hanging the test. Pointer moves merge in
   the window's queue, and what comes past WIN_EVENTS is lost; a chooser takes the keys while it is up; capitals and
   the characters Shift makes are typed with Shift */
static void fakes(void) {
    struct gk *ui = window("Fake", 300, 200);
    struct gk_event ev;
    unsigned t0;
    int i;
    gk_label(gk_root(ui), "fake");
    run(ui);
    t0 = gkf_clock();
    CHECK(gk_next(ui, &ev, 250) == 0 && gkf_clock() - t0 >= 250);
    gk_timer(ui, 9, 40, 0);
    t0 = gkf_clock();
    CHECK(gk_next(ui, &ev, -1) == 1 && ev.type == GK_EV_TIMER && ev.id == 9 && gkf_clock() - t0 == 40);
    CHECK(gk_next(ui, &ev, -1) == -1 && errno == EINTR && gkf_forever() == 1);
    for(i = 0; i < 100; i++) gkf_pointer(10 + i, 10, 0);
    CHECK(gkf_queued() == 1 && gkf_lost() == 0);       /* merged into one */
    gkf_pointer(110, 10, WIN_BUTTON_LEFT);              /* a press and a release: never merged */
    gkf_pointer(110, 10, 0);
    CHECK(gkf_queued() == 3);
    run(ui);
    for(i = 0; i < WIN_EVENTS + 6; i++) gkf_key(WIN_KEY_F11, 0);
    run(ui);
    CHECK(count(GK_EV_KEY) == WIN_EVENTS && gkf_lost() == 6);
    gkf_type("Ab!");
    run(ui);
    CHECK(count(GK_EV_KEY) == 3 && got[0].mods == WIN_MOD_SHIFT && got[1].mods == 0 && got[2].mods == WIN_MOD_SHIFT);
    gkf_key(WIN_KEY_F12, 0);
    CHECK(gk_next(ui, &ev, 0) == 1 && gk_choose(ui, 5, WIN_DLG_OPEN, "/", "", "") == 0 && gkf_dialog());
    gkf_key('x', 0);                                    /* the chooser's, not the window's */
    gkf_key(27, 0);                                     /* Esc: it cancels */
    run(ui);
    CHECK(!count(GK_EV_KEY) && heard(GK_EV_CHOSEN, 5) && heard(GK_EV_CHOSEN, 5)->value == 0);
    gk_close(ui);
}

/* the review's lows. F9: a watch ends once told its descriptor hung up with nothing to read, or is none. F10, F11: a
   long line and a long label take linear time (a quadratic one took seconds at 64 KB). F12: a box's own line is the
   box's, the program hearing only its answer. F13: a lone first byte before a lone continuation byte, which make a
   character together, leave the cursor after it, never inside. F15: absurd numbers from a program are held, not
   overflowed (the sanitizers' run sees any overflow) */
static void lows(void) {
    struct gk *ui = window("Lows", 400, 300);
    struct gk_widget *f = gk_field(gk_root(ui), 1, 300000, 0), *a = gk_area(gk_root(ui), 2, 0, NULL, NULL);
    struct gk_widget *sl = gk_slider(gk_root(ui), 3, -2147483647 - 1, 2147483647, 1000000000), *l;
    char *big = malloc(262145);
    int fds[2], i, fd_events = 0;
    clock_t t0;
    double took;
    run(ui);
    CHECK(pipe(fds) == 0 && gk_watch(ui, fds[0], POLLIN, 77) == 0);
    close(fds[1]);
    run(ui);
    run(ui);
    CHECK(count(GK_EV_FD) == 0);                        /* (the first run had it) */
    gk_watch(ui, fds[0], POLLIN, 77);
    for(i = 0; i < 3; i++) {
        run(ui);
        fd_events += count(GK_EV_FD);
    }
    CHECK(fd_events == 1);
    close(fds[0]);
    gk_watch(ui, fds[0], POLLIN, 78);                   /* a descriptor that is none: told once, then dropped */
    run(ui);
    CHECK(heard(GK_EV_FD, 78) && (heard(GK_EV_FD, 78)->value & POLLNVAL));
    run(ui);
    CHECK(!heard(GK_EV_FD, 78));
    memset(big, 'w', 262144);
    big[262144] = 0;
    t0 = clock();
    gk_set_text(f, big);
    gk_focus(f);
    gkf_key(WIN_KEY_HOME, 0);
    gkf_key(WIN_KEY_END, 0);
    run(ui);
    l = gk_label(gk_root(ui), big);
    run(ui);
    took = (double)(clock() - t0) / CLOCKS_PER_SEC;
    CHECK(took < 3.0);
    if(took >= 3.0) printf("  (256 KB in a field and a label: %.1f s)\n", took);
    gk_remove(l);
    gk_ask(ui, 9, "Secret", "Password:", "", GK_PASSWORD);
    gkf_type("hunter2\n");
    run(ui);
    CHECK(count(GK_EV_CHANGE) == 0 && heard(GK_EV_ANSWER, 9) && !strcmp(heard(GK_EV_ANSWER, 9)->text, "hunter2"));
    gk_set_text(f, "\xa9");                            /* a lone continuation byte; Home; a lone first byte, then x */
    gkf_key(WIN_KEY_HOME, 0);
    gkf_key(0xC3, 0);
    gkf_key('x', 0);
    run(ui);
    CHECK(!strcmp(gk_text(f), "\xc3\xa9x"));
    gk_size(a, 2147483647, 2147483647);                 /* F15: held, not overflowed */
    gk_gap(gk_root(ui), 2147483647);
    gk_pad(gk_root(ui), 2147483647);
    gk_damage(a, -2147483647 - 1, -2147483647 - 1, 2147483647, 2147483647);
    gk_damage(a, 2147483647, 2147483647, 2147483647, 2147483647);
    gk_set_value(sl, 2147483647);
    gk_focus(sl);
    gkf_key(WIN_KEY_PGUP, 0);
    gkf_key(WIN_KEY_RIGHT, 0);
    gkf_key(WIN_KEY_END, 0);
    gkf_key(WIN_KEY_PGDN, 0);
    run(ui);
    CHECK(gk_ok(ui) && gk_value(sl) >= -2147483647 - 1);
    free(big);
    gk_close(ui);
}

static void paste(void) {
    struct gk *ui = window("Paste", 300, 200);
    struct gk_widget *small, *big;
    char buf[8], *all;
    int len;
    small = gk_field(gk_root(ui), 1, 10, 0);
    big = gk_field(gk_root(ui), 2, 64, 0);
    run(ui);
    gkf_set_paste("hello world\nsecond line");
    gk_focus(small);
    gkf_key(22, WIN_MOD_CTRL);                          /* Ctrl+V: the first line, as much as fits */
    run(ui);
    CHECK(!strcmp(gk_text(small), "hello worl") && heard(GK_EV_CHANGE, 1));
    gk_focus(big);
    gkf_type("> ");
    gkf_key(22, 0);                                     /* from a desktop before the modifiers */
    run(ui);
    CHECK(!strcmp(gk_text(big), "> hello world"));
    all = gk_paste_all(ui, &len);
    CHECK(all && len == 23 && !strcmp(all, "hello world\nsecond line"));
    free(all);
    CHECK(gk_paste(ui, buf, 6) == 5 && !strcmp(buf, "hello"));
    gkf_set_paste("ab\xc3\xa9");
    CHECK(gk_paste(ui, buf, 4) == 2 && !strcmp(buf, "ab"));    /* é cut whole */
    CHECK(gk_paste(ui, buf, 5) == 4 && !strcmp(buf, "ab\xc3\xa9"));
    gkf_set_paste(NULL);
    CHECK(gk_paste(ui, buf, 5) == 0 && !strcmp(buf, ""));
    /* the window's own copy, not yet the desktop's clipboard, is pasted (desk's kernel: it is the newer), and a newer
       clipboard after it */
    gkf_set_paste("old");
    CHECK(gk_copy(ui, "mine", 4) == 0 && gk_paste(ui, buf, 8) == 4 && !strcmp(buf, "mine"));
    gkf_set_paste("newer");
    CHECK(gk_paste(ui, buf, 8) == 5 && !strcmp(buf, "newer"));
    gkf_set_paste("x");
    gkf_focus(0);                                       /* without the focus: nothing */
    run(ui);
    CHECK(gk_paste(ui, buf, 5) < 0 && errno == EACCES && !gk_paste_all(ui, &len) && errno == EACCES);
    gkf_focus(1);
    gkf_lacks(1u << WIN_PASTE, ENOSYS);
    CHECK(gk_paste(ui, buf, 5) < 0 && errno == ENOSYS);
    gkf_key(22, WIN_MOD_CTRL);
    run(ui);
    CHECK(!strcmp(gk_text(big), "> hello world") && count(GK_EV_CHANGE) == 0);
    gkf_lacks(0, 0);
    gk_close(ui);
}

/* a key typed and taken: the program is answering a person now, as a chooser must be asked for */
static int answering(struct gk *ui) {
    struct gk_event ev;
    gkf_key(WIN_KEY_F12, 0);
    return gk_next(ui, &ev, 0) == 1 && ev.type == GK_EV_KEY;
}

static void choosers(void) {
    struct gk *ui = window("Choose", 500, 300);
    const struct win_dialog *d;
    int i;
    int bx = (500 - 470) / 2, by = (300 - 160) / 2;
    gk_label(gk_root(ui), "files");
    run(ui);
    CHECK(gk_choose(ui, 29, WIN_DLG_OPEN, "/", "", "") < 0 && errno == EACCES && !gkf_dialog());   /* by itself: never */
    gk_timer(ui, 5, 10, 0);                             /* nor from a timer */
    gkf_advance(20);
    {
        struct gk_event ev;
        CHECK(gk_next(ui, &ev, 0) == 1 && ev.type == GK_EV_TIMER);
        CHECK(gk_choose(ui, 29, WIN_DLG_OPEN, "/", "", "") < 0 && errno == EACCES && !gkf_dialog());
    }
    CHECK(answering(ui));
    CHECK(gk_choose(ui, 30, WIN_DLG_SAVE, "/usb1p1", "notes.txt", "txt md") == 0);
    d = gkf_dialog();
    CHECK(d && d->kind == WIN_DLG_SAVE && d->id == 30 && !strcmp(d->dir, "/usb1p1") && !strcmp(d->name, "notes.txt") &&
          !strcmp(d->types, "txt md") && !strcmp(d->title, ""));
    CHECK(gk_choose(ui, 31, WIN_DLG_OPEN, "", "", "") < 0 && errno == EBUSY);
    gkf_dialog_answer("/usb1p1/notes.txt");
    run(ui);
    CHECK(heard(GK_EV_CHOSEN, 30) && heard(GK_EV_CHOSEN, 30)->value == 1 && !strcmp(heard(GK_EV_CHOSEN, 30)->text,
                                                                                     "/usb1p1/notes.txt"));
    CHECK(answering(ui) && gk_choose(ui, 31, WIN_DLG_OPEN, NULL, NULL, NULL) == 0 && gkf_dialog() &&
          !strcmp(gkf_dialog()->dir, ""));
    gkf_dialog_answer(NULL);
    run(ui);
    CHECK(heard(GK_EV_CHOSEN, 31) && heard(GK_EV_CHOSEN, 31)->value == 0 && !strcmp(heard(GK_EV_CHOSEN, 31)->text, ""));
    /* a request that is none comes back from the desktop's checks (the kernel's rules: surfos/win.h) */
    CHECK(gk_choose(ui, 32, 7, "/", "", "") < 0 && errno == EINVAL);
    CHECK(answering(ui) && gk_choose(ui, 32, WIN_DLG_OPEN, "usb1p1", "", "") < 0 && errno == EINVAL);
    CHECK(answering(ui) && gk_choose(ui, 32, WIN_DLG_SAVE, "/", "a/b", "") < 0 && errno == EINVAL);
    CHECK(answering(ui) && gk_choose(ui, 32, WIN_DLG_OPEN, "/", "", "p.ng") < 0 && errno == EINVAL);
    CHECK(answering(ui) && gk_choose(ui, 32, WIN_DLG_OPEN, "/", "", "a b c d e f g h i j k l m n o p q") < 0 &&
          errno == EINVAL);
    CHECK(answering(ui) && gk_choose(ui, 32, WIN_DLG_OPEN, "/", "", "abcdefghijklmnop") < 0 && errno == EINVAL);
    CHECK(answering(ui) && gk_choose(ui, 32, WIN_DLG_OPEN, "/", "", "=Make*") < 0 && errno == EINVAL);
    CHECK(answering(ui) && gk_choose(ui, 32, WIN_DLG_OPEN, "/", "", "\x01") < 0 && errno == EINVAL);
    CHECK(!gkf_dialog());
    /* whole names after '=', sixteen types */
    CHECK(answering(ui) && gk_choose(ui, 32, WIN_DLG_OPEN, "/", "", "=Makefile mk =.profile c h a b d e f g i j k l m") == 0 &&
          gkf_dialog());
    gkf_dialog_answer(NULL);
    run(ui);
    CHECK(heard(GK_EV_CHOSEN, 32) && heard(GK_EV_CHOSEN, 32)->value == 0);
    gkf_focus(0);
    run(ui);
    CHECK(answering(ui) && gk_choose(ui, 32, WIN_DLG_OPEN, "/", "", "") < 0 && errno == EACCES);
    gkf_focus(1);
    run(ui);
    /* a desktop without choosers: a line over the window to type the path in, answered the same way */
    gkf_lacks(1u << WIN_DIALOG, ENOSYS);
    CHECK(answering(ui) && gk_choose(ui, 33, WIN_DLG_OPEN, "/usb1p1", "", "png") == 0 && gk_box_open(ui));
    CHECK(gk_choose(ui, 34, WIN_DLG_OPEN, "/", "", "") < 0 && errno == EBUSY);
    run(ui);
    CHECK(gkf_text(bx + 28, by + 6, "Open a file", GW_PAPER) && gkf_text(bx + 16 + 4, by + 66 + 3, "/usb1p1", GW_INK));
    gkf_type("sea.png\n");
    run(ui);
    CHECK(heard(GK_EV_CHOSEN, 33) && heard(GK_EV_CHOSEN, 33)->value == 1 &&
          !strcmp(heard(GK_EV_CHOSEN, 33)->text, "/usb1p1/sea.png") && !heard(GK_EV_ANSWER, 33) && !gk_box_open(ui));
    CHECK(answering(ui) && gk_choose(ui, 35, WIN_DLG_SAVE, "/usb1p1", "cat.png", "png") == 0);
    run(ui);
    CHECK(gkf_text(bx + 28, by + 6, "Save as", GW_PAPER));
    gkf_key(1, WIN_MOD_CTRL);                           /* the whole path replaced: a name alone is in the folder */
    gkf_type("dog.png\n");
    run(ui);
    CHECK(heard(GK_EV_CHOSEN, 35) && !strcmp(heard(GK_EV_CHOSEN, 35)->text, "/usb1p1/dog.png"));
    CHECK(answering(ui) && gk_choose(ui, 36, WIN_DLG_FOLDER, "/usb1p1", "", "") == 0);
    gkf_key(27, 0);
    run(ui);
    CHECK(heard(GK_EV_CHOSEN, 36) && heard(GK_EV_CHOSEN, 36)->value == 0);
    CHECK(answering(ui) && gk_choose(ui, 37, WIN_DLG_OPEN, "/", "", "") == 0);
    gk_message(ui, 38, "Over it", "", "");              /* a box over the line: the chooser answered 0 */
    run(ui);
    CHECK(heard(GK_EV_CHOSEN, 37) && heard(GK_EV_CHOSEN, 37)->value == 0);
    gk_dismiss(ui);
    run(ui);
    gkf_lacks(0, 0);
    /* 11/05/26, desk's implementation: no room for the answer (EAGAIN, while WIN_EVENTS events wait unread): the
       request kept, and asked again once they are read; refused by then (the focus gone): answered as cancelled */
    CHECK(answering(ui));
    for(i = 0; i < WIN_EVENTS; i++) gkf_key(WIN_KEY_F11, 0);   /* (keys: moves would merge, as the kernel merges them) */
    CHECK(gk_choose(ui, 42, WIN_DLG_OPEN, "/", "", "") == 0 && !gkf_dialog());
    CHECK(gk_choose(ui, 43, WIN_DLG_OPEN, "/", "", "") < 0 && errno == EBUSY);
    run(ui);
    CHECK(gkf_dialog() && gkf_dialog()->id == 42);
    gkf_dialog_answer("/b");
    run(ui);
    CHECK(heard(GK_EV_CHOSEN, 42) && heard(GK_EV_CHOSEN, 42)->value == 1 && !strcmp(heard(GK_EV_CHOSEN, 42)->text, "/b"));
    CHECK(answering(ui));
    for(i = 0; i < WIN_EVENTS; i++) gkf_key(WIN_KEY_F11, 0);   /* (keys: moves would merge, as the kernel merges them) */
    CHECK(gk_choose(ui, 44, WIN_DLG_OPEN, "/", "", "") == 0 && !gkf_dialog());
    gkf_focus(0);
    run(ui);
    CHECK(!gkf_dialog() && heard(GK_EV_CHOSEN, 44) && heard(GK_EV_CHOSEN, 44)->value == 0);
    gkf_focus(1);
    run(ui);
    /* desk's fix round: a request while an answer's path waits to be taken is EBUSY; the toolkit takes the path as the
       answer comes, so its own next request is never refused for it */
    CHECK(answering(ui) && gk_choose(ui, 47, WIN_DLG_OPEN, "/", "", "") == 0);
    gkf_dialog_answer("/c");
    gkf_key(WIN_KEY_F12, 0);
    {
        struct win_dialog dd;
        memset(&dd, 0, sizeof(dd));
        dd.kind = WIN_DLG_OPEN;
        CHECK(gw_dialog(gk_gw(ui), &dd) < 0 && errno == EBUSY);
    }
    run(ui);
    CHECK(heard(GK_EV_CHOSEN, 47) && !strcmp(heard(GK_EV_CHOSEN, 47)->text, "/c"));
    CHECK(answering(ui) && gk_choose(ui, 48, WIN_DLG_OPEN, "/", "", "") == 0);
    gkf_dialog_answer(NULL);
    run(ui);
    CHECK(heard(GK_EV_CHOSEN, 48) && heard(GK_EV_CHOSEN, 48)->value == 0);
    /* a chooser asked just before the desktop let the window go: answered 0; a path chosen just before is still taken */
    CHECK(answering(ui) && gk_choose(ui, 40, WIN_DLG_OPEN, "/", "", "") == 0 && gkf_dialog());
    gkf_gone();
    CHECK(run(ui) < 0 && heard(GK_EV_CHOSEN, 40) && heard(GK_EV_CHOSEN, 40)->value == 0);
    gk_close(ui);
    ui = window("Choose", 500, 300);
    CHECK(answering(ui) && gk_choose(ui, 41, WIN_DLG_OPEN, "/", "", "") == 0);
    gkf_dialog_answer("/sea.txt");
    gkf_gone();
    run(ui);
    CHECK(heard(GK_EV_CHOSEN, 41) && !strcmp(heard(GK_EV_CHOSEN, 41)->text, "/sea.txt"));
    gk_close(ui);
    /* a kernel before them all (WIN_THEME says so as the window opens): the line, even for what its checks refuse */
    gkf_reset(0, 0);
    gkf_lacks(0xFFFFFFFFu, EINVAL);
    ui = gk_open("Choose", 500, 300, 0);
    gkf_focus(1);
    gk_label(gk_root(ui), "files");
    run(ui);
    CHECK(answering(ui) && gk_choose(ui, 39, WIN_DLG_OPEN, "/", "", "p.ng") == 0 && gk_box_open(ui));
    gkf_type("a\n");
    run(ui);
    CHECK(heard(GK_EV_CHOSEN, 39) && !strcmp(heard(GK_EV_CHOSEN, 39)->text, "/a"));
    gkf_lacks(0, 0);
    gk_close(ui);
}

static void theme(void) {
    struct gk *ui;
    struct gk_widget *label, *b;
    unsigned dusk[WIN_COLORS];
    int x, y, w, h;
    memcpy(dusk, ocean_roles, sizeof(dusk));
    dusk[WIN_COLOR_DESKTOP] = 0x142638;
    dusk[WIN_COLOR_WINDOW] = 0x202830;
    dusk[WIN_COLOR_TEXT] = 0xE0E8E0;
    dusk[WIN_COLOR_BUTTON] = 0x405060;
    gkf_reset(0, 0);
    gkf_set_theme(WIN_THEME_DUSK, dusk);
    ui = gk_open("Theme", 200, 100, 0);
    gkf_focus(1);
    label = gk_label(gk_root(ui), "dusk");
    b = gk_button(gk_root(ui), "B", 1);
    run(ui);
    CHECK(gk_theme(ui)->which == WIN_THEME_DUSK && gk_theme(ui)->paper == 0x202830 && gk_theme(ui)->desktop == 0x142638);
    CHECK(gkf_pixel(150, 80) == 0x202830 && gkf_text(0, 3, "dusk", 0xE0E8E0));
    gk_bounds(b, &x, &y, &w, &h);
    CHECK(gkf_pixel(x + 1, y + 1) == 0x405060);
    gkf_set_theme(WIN_THEME_OCEAN, ocean_roles);        /* a new theme: told, and all of the window painted in it */
    run(ui);
    CHECK(heard(GK_EV_THEME, 0) && heard(GK_EV_THEME, 0)->value == WIN_THEME_OCEAN && gk_theme(ui)->paper == GW_PAPER);
    CHECK(gkf_pixel(150, 80) == GW_PAPER && gkf_text(0, 3, "dusk", GW_INK) && gkf_pixel(x + 1, y + 1) == GW_SHELL);
    (void)label;
    gk_close(ui);
    gkf_reset(0, 0);                                    /* a kernel before WIN_THEME: the ocean's */
    gkf_lacks(0xFFFFFFFFu, EINVAL);
    ui = gk_open("Theme", 200, 100, 0);
    CHECK(ui && gk_theme(ui)->which == WIN_THEME_OCEAN && gk_theme(ui)->paper == GW_PAPER && gk_theme(ui)->on_teal == GW_PAPER);
    gkf_lacks(0, 0);
    gk_close(ui);
}

static void modifiers(void) {
    struct gk *ui = window("Keys", 300, 200);
    struct gk_widget *f, *a, *after;
    int x, y, w, h;
    f = gk_field(gk_root(ui), 1, 64, 0);
    a = gk_area(gk_root(ui), 2, GK_FOCUSABLE | GK_TAKES_TAB, NULL, NULL);
    after = gk_button(gk_root(ui), "After", 3);
    gk_focus(f);
    gkf_type("one two three");
    gkf_key(WIN_KEY_LEFT, WIN_MOD_CTRL);                /* to "three"'s start */
    gkf_key(WIN_KEY_LEFT, WIN_MOD_CTRL | WIN_MOD_SHIFT);    /* and "two " selected */
    gkf_key('2', 0);
    run(ui);
    CHECK(!strcmp(gk_text(f), "one 2three"));
    gkf_key(WIN_KEY_END, 0);
    gkf_key(WIN_KEY_BACKSPACE, WIN_MOD_CTRL);           /* Ctrl+Backspace: the word before */
    run(ui);
    CHECK(!strcmp(gk_text(f), "one "));
    gkf_key(127, 0);                                    /* Backspace (127) */
    run(ui);
    CHECK(!strcmp(gk_text(f), "one"));
    gkf_key(8, WIN_MOD_CTRL);                           /* Ctrl+H is no Backspace: the window's */
    run(ui);
    CHECK(!strcmp(gk_text(f), "one") && heard(GK_EV_KEY, 0) && heard(GK_EV_KEY, 0)->value == 8);
    gk_set_text(f, "one two");
    gkf_key(WIN_KEY_HOME, 0);
    gkf_key(WIN_KEY_DELETE, WIN_MOD_CTRL);              /* Ctrl+Delete: the word after */
    run(ui);
    CHECK(!strcmp(gk_text(f), " two"));
    /* Shift and a click: the selection to there */
    gk_set_text(f, "abcdef");
    gk_bounds(f, &x, &y, &w, &h);
    gkf_key(WIN_KEY_HOME, 0);
    gkf_mods(WIN_MOD_SHIFT);
    gkf_click(x + 4 + 3 * 8 + 1, y + 10);
    gkf_mods(0);
    gkf_type("X");
    run(ui);
    CHECK(!strcmp(gk_text(f), "Xdef"));
    /* a field types no key with Ctrl or Alt (Ctrl+0, Alt+X: the window's), and every repeat */
    gk_set_text(f, "");
    gkf_key('0', WIN_MOD_CTRL);
    gkf_key('x', WIN_MOD_ALT);
    gkf_key('z', 0);
    gkf_key('z', WIN_MOD_REPEAT);
    gkf_key(WIN_KEY_ENTER, WIN_MOD_CTRL);               /* Ctrl+Enter: no Enter */
    gkf_key(13, WIN_MOD_CTRL);                          /* Ctrl+M: no Enter either */
    run(ui);
    CHECK(!strcmp(gk_text(f), "zz") && count(GK_EV_KEY) == 4 && !heard(GK_EV_ACTIVATE, 1) && nth(GK_EV_KEY, 0)->value == '0' &&
          nth(GK_EV_KEY, 1)->value == 'x' && nth(GK_EV_KEY, 1)->mods == WIN_MOD_ALT &&
          nth(GK_EV_KEY, 2)->value == WIN_KEY_ENTER && nth(GK_EV_KEY, 3)->value == 13);
    gkf_key(13, WIN_MOD_SHIFT);                         /* Shift+Enter: Enter, its modifiers told */
    run(ui);
    CHECK(heard(GK_EV_ACTIVATE, 1) && heard(GK_EV_ACTIVATE, 1)->mods == WIN_MOD_SHIFT);
    /* an area that takes Tab hears it, and Ctrl+I (9 with Ctrl); Ctrl+Tab still moves on, Ctrl+Shift+Tab back */
    gk_focus(a);
    gkf_key(9, 0);
    gkf_key(WIN_KEY_BACKTAB, WIN_MOD_SHIFT);
    gkf_key(9, WIN_MOD_CTRL);
    run(ui);
    CHECK(gk_focused(ui) == a && count(GK_EV_KEY) == 3 && nth(GK_EV_KEY, 1)->value == WIN_KEY_BACKTAB &&
          nth(GK_EV_KEY, 1)->mods == WIN_MOD_SHIFT && nth(GK_EV_KEY, 2)->value == 9 && nth(GK_EV_KEY, 2)->mods == WIN_MOD_CTRL);
    gkf_key(WIN_KEY_TAB, WIN_MOD_CTRL);
    run(ui);
    CHECK(gk_focused(ui) == after && !count(GK_EV_KEY) && heard(GK_EV_FOCUS, 2) && heard(GK_EV_FOCUS, 2)->value == 0);
    gk_focus(a);
    gkf_key(WIN_KEY_TAB, WIN_MOD_CTRL | WIN_MOD_SHIFT);
    run(ui);
    CHECK(gk_focused(ui) == f);
    /* from a widget that does not take Tab, Ctrl+Tab moves on too; Ctrl and a button's key is no click */
    gkf_key(WIN_KEY_TAB, WIN_MOD_CTRL);
    run(ui);
    CHECK(gk_focused(ui) == a);
    gk_focus(after);
    gkf_key(' ', WIN_MOD_CTRL);
    gkf_key(13, WIN_MOD_CTRL);
    run(ui);
    CHECK(!heard(GK_EV_CLICK, 3) && count(GK_EV_KEY) == 2);
    /* the wheel and the pointer carry their modifiers to an area */
    gk_bounds(a, &x, &y, &w, &h);
    gkf_mods(WIN_MOD_CTRL);
    gkf_pointer(x + 5, y + 5, 0);
    gkf_mods(0);
    run(ui);
    CHECK(heard(GK_EV_POINTER, 2) && heard(GK_EV_POINTER, 2)->mods == WIN_MOD_CTRL);
    gk_close(ui);
}

/**** stage 2: a text of several lines, progress bars, sliders, palettes ****/

static void text_area(void) {
    struct gk *ui = window("Text", 300, 200);
    struct gk_widget *t, *ro, *after;
    int x, y, w, h, from, to;
    t = gk_text_area(gk_root(ui), 50, 64, 0);
    gk_size(t, 0, 4 * 16 + 4);
    gk_grow(t, 1, 0);
    ro = gk_text_area(gk_root(ui), 51, 200, GK_READONLY);
    gk_size(ro, 0, 2 * 16 + 4);
    gk_grow(ro, 1, 0);
    after = gk_button(gk_root(ui), "After", 52);
    run(ui);
    gk_bounds(t, &x, &y, &w, &h);
    gk_focus(t);
    gkf_type("one\ntwo\nthree");
    run(ui);
    CHECK(!strcmp(gk_text(t), "one\ntwo\nthree") && heard(GK_EV_CHANGE, 50));
    CHECK(gkf_text(x + 5, y + 2, "one", GW_INK) && gkf_text(x + 5, y + 2 + 16, "two", GW_INK));
    gkf_key(WIN_KEY_UP, 0);                             /* Up from after "three": the end of "two" */
    gkf_key(WIN_KEY_LEFT, WIN_MOD_SHIFT);
    gkf_key(WIN_KEY_LEFT, WIN_MOD_SHIFT);
    gkf_key(3, 0);
    run(ui);
    CHECK(!strcmp(gkf_clip(), "wo"));
    gkf_key(WIN_KEY_END, 0);
    gkf_key(8, 0);
    gkf_key(WIN_KEY_DELETE, 0);                         /* the line end: the lines join */
    run(ui);
    CHECK(!strcmp(gk_text(t), "one\ntwthree"));
    gkf_key(9, 0);                                      /* Tab is not its own: the focus moves on */
    run(ui);
    CHECK(gk_focused(ui) == ro);
    gk_set_text(t, "\tx\ny");                           /* a tab to the next multiple of 8 */
    run(ui);
    CHECK(gkf_text(x + 5 + 8 * 8, y + 2, "x", GW_INK) && gkf_text(x + 5, y + 2 + 16, "y", GW_INK));
    gk_focus(t);
    gkf_key(1, 0);                                      /* Ctrl+A, then typing past its 64 bytes: cut */
    gkf_type("01234567890123456789012345678901234");     /* (in two goes: the queue holds WIN_EVENTS) */
    run(ui);
    gkf_type("56789012345678901234567890123456789");
    run(ui);
    CHECK(strlen(gk_text(t)) == 64);
    gk_set_text(t, "a\nb\nc\nd\ne\nf\ng\nh");
    for(from = 0; from < 7; from++) gkf_key(WIN_KEY_DOWN, 0);
    gkf_key(WIN_KEY_END, 0);
    run(ui);
    gk_text_cursor(t, &from, &to);
    CHECK(from == to && from == (int)strlen("a\nb\nc\nd\ne\nf\ng\nh") && gkf_text(x + 5, y + 2 + 2 * 16, "g", GW_INK));   /* (the cursor is on "h"'s edge) */
    gkf_wheel(x + 20, y + 20, -3);                      /* the wheel moves the view, not the cursor */
    run(ui);
    CHECK(gkf_text(x + 5, y + 2, "b", GW_INK));
    gkf_advance(1000);
    gkf_click(x + 5 + 2, y + 2 + 5);                    /* "b" */
    gkf_type("B");
    run(ui);
    CHECK(!strncmp(gk_text(t), "a\nBb", 4));
    /* the keys as surfos/win.h has them: Ctrl+End, Ctrl+Backspace a word, Ctrl+Left by words; Alt+X, Ctrl+Enter and
       Ctrl+I (9 with Ctrl) the window's, no tab */
    gk_set_text(t, "sea and sand");
    gkf_key(WIN_KEY_END, WIN_MOD_CTRL);
    gkf_key(WIN_KEY_BACKSPACE, WIN_MOD_CTRL);
    gkf_key(WIN_KEY_LEFT, WIN_MOD_CTRL);
    gkf_key('x', WIN_MOD_ALT);
    gkf_key(WIN_KEY_ENTER, WIN_MOD_CTRL);
    gkf_key(9, WIN_MOD_CTRL);
    gkf_key('!', 0);
    run(ui);
    CHECK(!strcmp(gk_text(t), "sea !and ") && count(GK_EV_KEY) == 3 && gk_focused(ui) == t);
    /* one that takes Tab: a tab typed; Ctrl+Tab moves on */
    {
        struct gk_widget *tabby = gk_text_area(gk_root(ui), 53, 64, GK_TAKES_TAB);
        gk_focus(tabby);
        gkf_key(9, 0);
        gkf_key('x', 0);
        gkf_key(WIN_KEY_TAB, WIN_MOD_CTRL);
        run(ui);
        CHECK(!strcmp(gk_text(tabby), "\tx") && gk_focused(ui) != tabby);
        gk_remove(tabby);
    }
    /* read-only: shown, selected and copied, never changed */
    gk_set_text(ro, "fixed text");
    gk_focus(ro);
    gkf_type("x");
    gkf_key(8, 0);
    gkf_key(1, 0);
    gkf_key(3, 0);
    gkf_key(24, 0);
    run(ui);
    CHECK(!strcmp(gk_text(ro), "fixed text") && !strcmp(gkf_clip(), "fixed text") && count(GK_EV_CHANGE) == 0);
    (void)after;
    gk_close(ui);
}

static void progress_slider_palette(void) {
    struct gk *ui = window("More", 300, 200);
    struct gk_widget *p, *s, *pal, *tools;
    static const unsigned pencil[4] = { 0x153D49, 0xFF000000u, 0xFF000000u, 0x153D49 };
    int x, y, w, h, i;
    p = gk_progress(gk_root(ui), 200);
    gk_set_text(p, "Half");
    s = gk_slider(gk_root(ui), 60, 0, 100, 5);
    pal = gk_palette(gk_root(ui), 61, 4, 16, 16);
    for(i = 0; i < 6; i++) CHECK(gk_palette_color(pal, (unsigned)(0x100000 * i)) == i);
    tools = gk_palette(gk_root(ui), 62, 2, 24, 24);
    CHECK(gk_palette_icon(tools, pencil, 2, 2) == 0 && gk_palette_icon(tools, pencil, 2, 2) == 1);
    CHECK(gk_palette_icon(tools, pencil, 40, 40) < 0);  /* larger than a cell */
    gk_set_value(p, 100);
    run(ui);
    gk_bounds(p, &x, &y, &w, &h);
    CHECK(gkf_pixel(x + 2, y + 5) == GW_TEAL && gkf_pixel(x + w - 3, y + 5) == SHEET);
    CHECK(gkf_pixel(x + (w - 2) / 2 - 1, y + h - 2) == GW_TEAL && gkf_pixel(x + (w - 2) / 2 + 3, y + h - 2) == SHEET);
    gk_set_value(p, 999);                               /* kept within */
    CHECK(gk_value(p) == 200);
    /* the slider: keys by its step, a click on its track, the wheel */
    gk_bounds(s, &x, &y, &w, &h);
    gkf_click(x + w - 2, y + h / 2);
    run(ui);
    CHECK(gk_value(s) == 100 && heard(GK_EV_CHANGE, 60) && gk_focused(ui) == s);
    gkf_key(WIN_KEY_LEFT, 0);
    gkf_key(WIN_KEY_PGDN, 0);
    run(ui);
    CHECK(gk_value(s) == 45 && count(GK_EV_CHANGE) == 2);
    gkf_key(WIN_KEY_HOME, 0);
    gkf_wheel(x + 10, y + 5, -3);                       /* away from the user: up */
    run(ui);
    CHECK(gk_value(s) == 5);
    gkf_drag(x + 1, y + 5, x + w / 2, y + 5);
    run(ui);
    CHECK(gk_value(s) == 50);
    /* the palette: a click, the arrows */
    gk_bounds(pal, &x, &y, &w, &h);
    CHECK(w == 4 * 18 + 2 && h == 2 * 18 + 2 && gk_value(pal) == -1);
    gkf_click(x + 1 + 18 + 8, y + 1 + 8);
    run(ui);
    CHECK(gk_value(pal) == 1 && heard(GK_EV_CHANGE, 61) && gk_palette_get_color(pal, 1) == 0x100000);
    gkf_key(WIN_KEY_DOWN, 0);
    run(ui);
    CHECK(gk_value(pal) == 5);
    CHECK(gk_palette_set_color(pal, 5, 0x123456) == 0 && gk_palette_get_color(pal, 5) == 0x123456);
    run(ui);
    CHECK(gkf_pixel(x + 1 + 18 + 8, y + 1 + 18 + 8) == 0x123456);
    gk_bounds(tools, &x, &y, &w, &h);
    gkf_click(x + 1 + 26 + 12, y + 1 + 12);
    run(ui);
    CHECK(gk_value(tools) == 1 && gkf_pixel(x + 1 + 26 + 11, y + 1 + 11) == 0x153D49);
    gk_close(ui);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    libgui();
    layout();
    damage();
    buttons();
    field();
    list();
    bars();
    focus();
    areas();
    boxes();
    loop();
    starved();
    bursts();
    menus();
    submenus();
    drags();
    wipes();
    repeats();
    cuts();
    held();
    punct();
    fits();
    fakes();
    lows();
    paste();
    choosers();
    theme();
    modifiers();
    text_area();
    progress_slider_palette();
    gkf_reset(0, 0);
    printf("test_gk: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
