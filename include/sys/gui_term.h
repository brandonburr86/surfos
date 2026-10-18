/*
SurfOS Desktop: Terminal
--------------------
File: gui_term.h Date: 10/17/26
--------------------
The Terminal's state in gui_state (shell/guiterm.c): a shell session on a pseudo-terminal
(struct gui_sys's term_*) and the screen it draws. Included by sys/gui.h; owned by the
Terminal (track TERM of docs/superpowers/plans/2026-10-17-gui-suite.md).
10/17/26: the screen is struct gui_vt (shell/guivt.c): cells as the console keeps them (a code
page 437 character and a VGA attribute), GUI_VT_LINES lines in a ring, the last rows of which
are the screen and the rest its scrollback; and what the session has not taken of what was typed
or pasted yet. 1,024 lines of 160 cells and the clipboard's worth of input: about 460 KB.
*/
#ifndef _SYS_GUI_TERM_H
#define _SYS_GUI_TERM_H
#include <sys/cp437.h>

#define GUI_VT_COLS 160                 /* the widest screen: a maximized window 1280 pixels wide has 157 columns */
#define GUI_VT_ROWS 64                  /* the tallest */
#define GUI_VT_LINES 1024               /* lines kept: the screen and its scrollback (960 lines or more) */
#define GUI_VT_PARAMS 8                 /* an escape sequence's numbers, as the console keeps them (ESC_PARAMS) */
#define GUI_TERM_PEND (GUI_CLIP_MAX + 512)   /* typed and pasted bytes the session has not taken yet */

struct gui_vt_cell { u8 ch, attr; };    /* attr: VGA order, as the console's: foreground 0-15, background 0-7 << 4 */

/* The screen: cols x rows cells at the end of the ring of lines. Line i of those kept (0 the oldest) is
   cell[(first + i) % GUI_VT_LINES]; the screen's row y is line count - rows + y. A line's number, which
   stays with its text while lines are added and dropped, is dropped + i. */
struct gui_vt {
    int cols, rows;
    int x, y;                           /* the cursor, on the screen */
    int count;                          /* lines kept: rows to GUI_VT_LINES */
    u32 first, dropped;                 /* the oldest line's place in the ring; lines dropped from the top ever */
    bool wrap, cursor_off;              /* a VT100 wrap is pending (the last column was written); ESC[?25l */
    u8 fg, bg;                          /* SGR, as the console keeps it: 0, or the VGA colour + 1 */
    bool bold, inverse;
    u8 esc, mark;                       /* the escape sequence being read: where, and its private mark */
    int nparam;
    u16 param[GUI_VT_PARAMS];
    struct utf8_dec utf;                /* a UTF-8 character half read (driver/cp437.c) */
    u8 wrapped[GUI_VT_LINES];           /* by place in the ring: the line went on in the next (a wrap) */
    struct gui_vt_cell cell[GUI_VT_LINES][GUI_VT_COLS];
};

struct gui_term {
    int handle;                         /* the session (struct gui_sys), -1 when there is none */
    bool ended;                         /* the session ended (exit): the window says so */
    char status[GUI_STATUS];
    /* 10/17/26 */
    int error;                          /* why no session could start (term_open's -errno), 0 */
    bool follow;                        /* the view is the screen; else it shows from line number view */
    u32 view, news;                     /* news: lines that came below a view scrolled back */
    bool selecting, selected, thumb;    /* a drag selects; a selection is there; the scroll bar's thumb is held */
    u32 sel_line, end_line;             /* the selection: from (sel_line, sel_col) to (end_line, end_col), */
    int sel_col, end_col;               /* line numbers and the boundaries between cells */
    int thumb_at, drag_x, drag_y;       /* where the thumb was taken; the pointer in a drag (window-relative) */
    u32 drag_ms, click_ms, click_line;  /* the last scroll of a drag past the edge; the last click, for a double-click */
    int click_col;
    u32 pend_at, pend_len;              /* pend[pend_at, pend_len): for the session, once it takes them */
    char pend[GUI_TERM_PEND];
    struct gui_vt vt;
};
#endif
