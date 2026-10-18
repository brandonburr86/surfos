/*
SurfOS Desktop: the Terminal's Screen
--------------------
File: guivt.h Date: 10/17/26
--------------------
The terminal emulator of the desktop's Terminal (shell/guivt.c; its state, struct gui_vt, is in
sys/gui_term.h): what a session prints, drawn into a screen of cells as the console draws it, with
the scrollback above. Pure code: tests/host/test_guiterm.c runs it on its own too.
10/18/26 (the review): vt_take() feeds within a budget of cells written; a wrap mark is the width the
line wrapped at, for vt_text() and vt_word(), which follow a wrap. 10/18/26 (the second review):
vt_shown(), the text of lines as they show.
*/
#ifndef _SHELL_GUIVT_H
#define _SHELL_GUIVT_H
#include <sys/gui.h>

#define VT_PLAIN 0x07                   /* light grey on black: the console's own colours */

void vt_reset(struct gui_vt *vt, int cols, int rows);  /* an empty screen, nothing kept above it */
void vt_feed(struct gui_vt *vt, const char *s, u32 n); /* bytes the session printed */
/* 10/18/26: the same while fewer than budget more cells have been written (an erase writes many for a few bytes):
   how many bytes it took, at least one when n and budget are not 0 (one byte writes at most a screenful) */
u32 vt_take(struct gui_vt *vt, const char *s, u32 n, u32 budget);
/* a new size, without reflow: cut or padded; the cursor stays on its line, which stays on the screen */
void vt_resize(struct gui_vt *vt, int cols, int rows);
/* a new start for a session: no sequence half read, plain colours, the cursor shown, at a row's start */
void vt_fresh(struct gui_vt *vt);
const struct gui_vt_cell *vt_line(const struct gui_vt *vt, int i);     /* line i of those kept, 0 the oldest */
int vt_wrapped(const struct gui_vt *vt, int i);    /* line i goes on in line i + 1 from this column; 0: it does not */
static inline int vt_screen(const struct gui_vt *vt) { return vt->count - vt->rows; }   /* the screen's first line */
/* The text of lines l0 to l1, from boundary c0 of the first to c1 of the last, as UTF-8 in out: blanks at a
   line's end left out, a line end after each line but one that went on in the next, which is joined where it
   wrapped (10/18/26: whatever the width is now). A line in the middle, and the last one when c1 is at the
   screen's width or beyond, is taken to its end: all of a line kept from a wider screen. Its length (0, and out
   untouched, when there are only blanks); *cut when it did not all fit in size. */
u32 vt_text(const struct gui_vt *vt, int l0, int c0, int l1, int c1, char *out, u32 size, bool *cut);
/* 10/18/26: the same of lines l0 to l1 whole, as they show: each cut at the screen's width (a line kept from a
   wider screen loses what does not show), joined to the next only where it wrapped within that width */
u32 vt_shown(const struct gui_vt *vt, int l0, int l1, char *out, u32 size, bool *cut);
/* the word at column col of line i, from boundary (*l0, *c0) to (*l1, *c1): 10/18/26 across the wraps of the lines
   it goes on in; a cell that is no word's is taken alone (outside the lines: nothing, l0 == l1 and c0 == c1) */
void vt_word(const struct gui_vt *vt, int i, int col, int *l0, int *c0, int *l1, int *c1);
#endif
