/*
SurfOS user program: wcharmap
--------------------
File: charmap.h Date: 10/26/26
--------------------
The Character Map's model (charmap.c): the console's 256 characters (code page 437), the one
chosen, what a key or a press does, what Copy takes and what the status bar says, Go to code (its
band in the window and what it takes), and where everything is for the window's height, so that
tests/host/test_wcharmap.c runs it on the host as it is. The desktop's Character Map
(shell/guichar.c, stage C12 of docs/superpowers/plans/2026-10-25-app-windows.md) moved out of the
kernel: the same grid, panel, keys and button, in a window of its own; Go to code, which was the
desktop's dialog, is a band of the window's own.
*/
#ifndef _WCHARMAP_CHARMAP_H
#define _WCHARMAP_CHARMAP_H

/* the window, as the desktop's Character Map's was: 460 x 360 with its frame, this its content */
#define CHARMAP_W 458
#define CHARMAP_H 331
#define CHARMAP_TITLE_H 28              /* the frame's title bar above the content, a pixel of border at the left */
#define CHARMAP_STATUS_H 22             /* the status bar */
#define CHARMAP_STATUS 96
#define CHARMAP_GO_MAX 24               /* Go to code's text */
#define CHARMAP_DOUBLE_MS 500           /* two clicks on a cell this close together copy it */

/* where things are, in the window's coordinates (the desktop's: the title bar's height above the content) */
#define CHARMAP_SHEET 0xFFFDF5          /* the grid's background, as Notepad's text */
#define CHARMAP_GX 26                   /* the grid's left, and each cell's width */
#define CHARMAP_CW 17
#define CHARMAP_PX 312                  /* the panel of the chosen character, beside the grid */
#define CHARMAP_PW 138
#define CHARMAP_BOX_Y 34                /* the chosen character, large */
#define CHARMAP_BIG 6
#define CHARMAP_INFO_Y 148              /* its code, code point and bytes, and what is special about it */
#define CHARMAP_COPY_W 96               /* the Copy button */
#define CHARMAP_COPY_H 26

/* the grid's top and its cells' height, the code's digits above it, the status bar, the Copy button, for the
   window's height with its frame: 360, or less on a small screen, where the cells are a pixel lower so that the
   status bar covers none of them; and Go to code's band over the status bar, with its field and buttons */
struct charmap_layout { int gy, ch, head_y, sy, copy_y, band_y, field_x, field_w, go_x, cancel_x, button_y; };
void charmap_layout(int win_h, struct charmap_layout *l);

/* the keys that are no bytes (surfos/win.h's WIN_KEY_*) */
#define CHARMAP_UP 0x100
#define CHARMAP_DOWN 0x101
#define CHARMAP_LEFT 0x102
#define CHARMAP_RIGHT 0x103
#define CHARMAP_HOME 0x104
#define CHARMAP_END 0x105
#define CHARMAP_PGUP 0x106
#define CHARMAP_PGDN 0x107

struct wcharmap {
    int selected;                       /* the code page 437 character, 0-255 */
    int win_h;                          /* the window's height with its frame: the layout follows it */
    unsigned click_ms;                  /* the last click on a cell, for a double-click: when, and on which */
    int click_code;
    int asking;                         /* Go to code's band is open: it has the keys */
    int fresh;                          /* and what it holds was put there: the first key typed replaces it */
    char go[CHARMAP_GO_MAX];            /* what it holds */
    char status[CHARMAP_STATUS];
};

/* what a key or a press asks main.c to do besides drawing again */
#define CHARMAP_DRAW 1                  /* draw again */
#define CHARMAP_COPY 2                  /* Copy: charmap_copy()'s bytes to the clipboard */
#define CHARMAP_CLOSE 3                 /* the window closes */

void charmap_init(struct wcharmap *m, int win_h);
int charmap_utf8(int code, char *out);          /* its UTF-8 (CP437_UTF8_MAX bytes at most), 0 for none: 0x00 */
/* the code a text names: 247 (three digits at most), 0xF7 (two hex digits), U+224B (six at most), or one character
   (not a digit or a space), with spaces around it: 0-255, -1 when it names none, -2 for a code point the console
   lacks (*u: it) */
int charmap_parse(const char *s, unsigned *u);
int charmap_key(struct wcharmap *m, int key);   /* 0, or CHARMAP_DRAW, CHARMAP_COPY, CHARMAP_CLOSE */
int charmap_press(struct wcharmap *m, int x, int y, unsigned now_ms);   /* the left button, window's coordinates */
/* Copy: the chosen character's UTF-8 in out (n bytes at least CP437_UTF8_MAX), its length, and the status bar says
   it was copied; 0 for the empty cell, which the status bar says copies nothing */
int charmap_copy(struct wcharmap *m, char *out, int n);
void charmap_notes(int code, const char **a, const char **b);  /* what is special about it, in two short lines */
void charmap_status(const struct wcharmap *m, char *out, int n);   /* the status bar's line */
void charmap_bytes(int code, char *out, int n);                 /* its UTF-8 in hex, "E2 89 8B", or "none" */

#endif
