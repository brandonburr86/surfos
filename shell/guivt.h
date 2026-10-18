/*
SurfOS Desktop: the Terminal's Screen
--------------------
File: guivt.h Date: 10/17/26
--------------------
The terminal emulator of the desktop's Terminal (shell/guivt.c; its state, struct gui_vt, is in
sys/gui_term.h): what a session prints, drawn into a screen of cells as the console draws it, with
the scrollback above. Pure code: tests/host/test_guiterm.c runs it on its own too.
*/
#ifndef _SHELL_GUIVT_H
#define _SHELL_GUIVT_H
#include <sys/gui.h>

#define VT_PLAIN 0x07                   /* light grey on black: the console's own colours */

void vt_reset(struct gui_vt *vt, int cols, int rows);  /* an empty screen, nothing kept above it */
void vt_feed(struct gui_vt *vt, const char *s, u32 n); /* bytes the session printed */
/* a new size, without reflow: cut or padded; the cursor stays on its line, which stays on the screen */
void vt_resize(struct gui_vt *vt, int cols, int rows);
/* a new start for a session: no sequence half read, plain colours, the cursor shown, at a row's start */
void vt_fresh(struct gui_vt *vt);
const struct gui_vt_cell *vt_line(const struct gui_vt *vt, int i);     /* line i of those kept, 0 the oldest */
bool vt_wrapped(const struct gui_vt *vt, int i);                       /* it goes on in line i + 1 */
static inline int vt_screen(const struct gui_vt *vt) { return vt->count - vt->rows; }   /* the screen's first line */
/* The text of lines l0 to l1, from boundary c0 of the first to c1 of the last (columns of the screen's
   width), as UTF-8 in out: blanks at a line's end left out, a line end after each line but one that went
   on in the next. Its length; *cut when it did not all fit in size. */
u32 vt_text(const struct gui_vt *vt, int l0, int c0, int l1, int c1, char *out, u32 size, bool *cut);
/* the word at column col of line i: the boundaries around it (from == to: no word there) */
void vt_word(const struct gui_vt *vt, int i, int col, int *from, int *to);
#endif
