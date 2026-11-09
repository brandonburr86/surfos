/*
SurfOS host tests: a fake desktop for programs' windows
--------------------
File: gk_fake.h Date: 11/05/26
--------------------
So that a program's window is tested natively (the desktop applications, track kit:
docs/superpowers/plans/2026-11-05-desktop-apps.md). libgui (gui.c) and the toolkit (gk*.c) are built
for the host as they are (Makefile: GK_HOST_OBJ and GK_HOST_FLAGS), and this is the desktop at the
other end of their system calls: winopen() and winctl() as the kernel's window calls answer
(include/surfos/win.h), the window's events through a pipe whose read end is the window's
descriptor (so read() and poll() are the host's own), and uptime() a clock the test moves.

A test sends events as the desktop sends them, lets the program take them (with the toolkit,
gk_next(ui, &ev, 0) until it says 0), and looks at what the window presented, as the desktop shows
it: gkf_pixel(), gkf_text() (the console's glyphs in a colour at a place), and how many presents
there were and where (damage). Two presses of the left button within 500 ms of the fake clock are
a double-click, as on the desktop: gkf_advance() between clicks that must not be one.
*/
#ifndef _GK_FAKE_H
#define _GK_FAKE_H

#include <surfos/win.h>

/* everything forgotten: no window, the clock at 1000, the desktop's content area at most max_w x max_h (0: 958 x
   457, the laptop's) */
void gkf_reset(int max_w, int max_h);
void gkf_refuse(int err);               /* the next winopen() fails with this errno (ENODEV: no desktop) */
int gkf_open(void);                     /* a window is open (its descriptor not yet closed) */
int gkf_w(void);
int gkf_h(void);
const char *gkf_title(void);
int gkf_resizable(void);

/* what the desktop shows of the window: its presented pixels (the background where none were) */
unsigned gkf_pixel(int x, int y);
/* text drawn at x, y in fg: each glyph's lit pixels fg, and none of its others */
int gkf_text(int x, int y, const char *utf8, unsigned fg);
int gkf_find(const char *utf8, unsigned fg, int *x, int *y);    /* where it is, the first from the top left */
int gkf_presents(void);                 /* WIN_PRESENTs so far */
/* the box around what was presented since the last call (0 x 0 for nothing), and how many presents; then each of those
   presents' rectangle (the first 64), until the next call: 0 past them */
int gkf_presented(int *x, int *y, int *w, int *h);
int gkf_presented_rect(int i, int *x, int *y, int *w, int *h);

/* events, as the desktop sends them */
void gkf_key(unsigned key, unsigned mods);
void gkf_type(const char *text);        /* each byte a key, \n as Enter (\r) */
void gkf_pointer(int x, int y, unsigned buttons);
void gkf_click(int x, int y);           /* the left button pressed and let go there */
void gkf_drag(int x0, int y0, int x1, int y1);   /* pressed at one place, moved and let go at the other */
void gkf_wheel(int x, int y, int rows);
void gkf_mods(unsigned mods);           /* the modifiers held for the pointer's and the wheel's events from now on */
void gkf_focus(int on);
void gkf_resize(int w, int h);          /* the user's new size: the program answers with WIN_SETSIZE */
void gkf_close(void);                   /* the close button */
void gkf_gone(void);                    /* the desktop lets the window go */
void gkf_advance(unsigned ms);          /* uptime() moves on */
unsigned gkf_clock(void);

/* the clipboard: what the window put there (WIN_CLIP, only while it has the focus), and what WIN_PASTE gives */
const char *gkf_clip(void);
int gkf_clips(void);
void gkf_set_paste(const char *text);  /* the desktop's clipboard now (newer than a WIN_CLIP before: until then,
                                           WIN_PASTE gives the window its own copy, as the kernel does) */
int gkf_alerts(void);                   /* WIN_ALERTs */

/* 11/05/26, track desk's calls (surfos/win.h), checked as the kernel checks them and done as the desktop does them:
   ops (bits 1 << WIN_MENU and so on) answered with err from now on: ENOSYS as a desktop without them yet, EINVAL as a
   kernel before them (0: all of them done again) */
void gkf_lacks(unsigned ops, int err);
/* the menus the window's last WIN_MENU gave (entries, titles, lines and submenus' children included), and how many
   WIN_MENUs; an entry; the index of the entry with a label (-1); and the item of an id chosen in the bar or a submenu
   (WIN_EV_MENU, x its menu's id; 0 when it is not there, opens a submenu or is greyed, or its menu or submenu is) */
int gkf_menu_count(void);
int gkf_menus_sent(void);
const struct win_menu_item *gkf_menu_entry(int i);
int gkf_menu_find(const char *label);
int gkf_menu_choose(int id);
/* the chooser's request waiting for its answer (NULL: none), and its answer: path chosen (WIN_TAKE gives it), NULL
   cancelled. A request needs a key, a press or a menu choice sent since the last one (EACCES), as the kernel's does;
   gkf_gone() answers one that waits with 0 */
const struct win_dialog *gkf_dialog(void);
void gkf_dialog_answer(const char *path);
/* a new theme (colors: WIN_COLORS by role, NULL: as they were): WIN_EV_THEME to a window whose program asked it */
void gkf_set_theme(unsigned which, const unsigned *colors);

#endif
