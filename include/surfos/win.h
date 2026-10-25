/*
SurfOS windows for programs
----------------------
File: win.h     Date: 10/25/26
----------------------
A window on the desktop for an ordinary program (milestone M12,
docs/superpowers/plans/2026-10-25-app-windows.md). The program draws in its own memory and
presents what changed; the desktop (shell/gui.c) draws the window's pixels inside a frame of
its own and sends the program what happens in the window, as 16-byte events.

The events (struct win_event): a key as it was typed (value: the byte, or WIN_KEY_* for the
keys that are no bytes); the pointer over the window's content, and anywhere while a button
pressed there is held (x, y: logical pixels from the content's top left; a position outside
the content says the pointer left it; value: the buttons held, WIN_BUTTON_*); the wheel over
the content (x, y as for the pointer; value: rows, as an int, positive down, 3 a notch); the
focus (value 1: the window came to the front, 0: it left it); a new size (x, y: the content
size the user gave the window by its corner or by maximizing it: the program answers with an
image of that size); and the close button. mods stays 0 for now.

This header uses no SurfOS types: user programs reach it through user/include/surfos/win.h.
The desktop's side at its end (struct win_state, struct win_view) is seen only by a file that
has the kernel's types (surfos/types.h, included first as everywhere in the kernel).
*/

#ifndef _SURFOS_WIN_H
#define _SURFOS_WIN_H

#define WIN_TITLE_MAX   64              /* a title's bytes with its NUL; a longer one is cut between characters */
#define WIN_RESIZABLE   1               /* flags: the user may resize and maximize the window */

struct win_event { unsigned short type, mods; int x, y; unsigned value; };     /* 16 bytes */
#define WIN_EV_KEY      1
#define WIN_EV_POINTER  2
#define WIN_EV_WHEEL    3
#define WIN_EV_FOCUS    4
#define WIN_EV_RESIZE   5
#define WIN_EV_CLOSE    6

/* the keys that are no bytes (the desktop's GUI_UP .. GUI_INSERT, sys/gui.h) */
#define WIN_KEY_UP      0x100
#define WIN_KEY_DOWN    0x101
#define WIN_KEY_LEFT    0x102
#define WIN_KEY_RIGHT   0x103
#define WIN_KEY_HOME    0x104
#define WIN_KEY_END     0x105
#define WIN_KEY_PGUP    0x106
#define WIN_KEY_PGDN    0x107
#define WIN_KEY_DELETE  0x108
#define WIN_KEY_INSERT  0x109

#define WIN_BUTTON_LEFT   1
#define WIN_BUTTON_RIGHT  2
#define WIN_BUTTON_MIDDLE 4

#ifdef _SURFOS_TYPES_H
/* The desktop's side (shell/guiclient.c, through struct gui_sys's win_* calls). A window as the desktop draws it:
   its title; the size of its pixels, which are its content; the limits and flags it asked for (max 0: as large as
   the desktop allows); its background, shown where its pixels do not reach; a serial that moves with every present,
   size and title; and the pid of the program that opened it. */
struct win_state {
    char title[WIN_TITLE_MAX];
    int w, h, min_w, min_h, max_w, max_h;
    u32 flags, background, serial;
    int pid;
};
/* its pixels, 0x00RRGGBB in rows of w, lent to the desktop until its next update or close */
struct win_view { const u32 *pixels; int w, h; };
#endif

#endif
