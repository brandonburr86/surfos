/*
SurfOS Undertow: the line typed at the undertow> prompt
----------------------
File: edit.h  Date: 10/11/26
----------------------
The console's own line editing stops at 255 bytes and an address may have 2048,
so Undertow reads its commands in raw mode (ttyctl(TTY_RAW, 1)) with a small
editor of its own, up to UT_COMMAND_MAX bytes, and is back in line mode before
it returns: a load, and anything printed, happen in line mode, where Ctrl+C
interrupts the program as it does the shell's others and the pager works.

The line is kept on one row: when it is longer than the row, the row scrolls
sideways to keep the cursor in sight (so the prompt never wraps and the view
above it never moves). Keys: printable ASCII, and UTF-8 characters where the
terminal draws them; Left, Right, Home, End (^A, ^E), Backspace, Delete; ^U
clears the line, ^K deletes to its end, ^W the word before the cursor; Up and
Down go through the lines typed in this session; Enter. ^C quits Undertow (in
raw mode it is a key, not a kill), and so does ^D on an empty line (it deletes
otherwise). PgDn and PgUp on an empty line are next and prev. A terminal's
escape sequences are read to their final byte, so one the editor does not know
leaves nothing in the line. A line that would be longer than UT_COMMAND_MAX
takes no more characters, and Enter refuses it whole, so its cut-off start
never runs; ^U (or deleting it all) starts again.

Keys that came after the Enter (the rest of a paste) are dropped, and the
terminal skips the rest of the paste until the input pauses (TTY_SKIP), as
surfsh does; drop_typed drops the keys typed before the prompt too (during a
load), so nothing typed then becomes part of the next command.

The terminal is reached through the calls in struct ut_edit_io and ttyctl(),
so the host tests drive the editor with keys of their own.
*/

#ifndef UNDERTOW_EDIT_H
#define UNDERTOW_EDIT_H

#include <stddef.h>
#include "view.h"

#define UT_EDIT_EOF   (-1)      /* the end of the input, Ctrl+D on an empty line */
#define UT_EDIT_LONG  (-2)      /* the line was longer than UT_COMMAND_MAX: refused */
#define UT_EDIT_QUIT  (-3)      /* Ctrl+C */
#define UT_EDIT_NEXT  (-4)      /* PgDn on an empty line */
#define UT_EDIT_PREV  (-5)      /* PgUp on an empty line */

#define UT_EDIT_HISTORY 64      /* lines kept for Up and Down */

struct ut_edit_io {
    void *ctx;
    long (*read)(void *ctx, unsigned char *buf, size_t cap);    /* > 0 bytes (waits for one); else the end */
    int (*ready)(void *ctx);                                    /* 1: a byte can be read at once */
    void (*write)(void *ctx, const char *buf, size_t len);
};

struct ut_editor;

/* An editor (on the heap), NULL when there is no memory. */
struct ut_editor *ut_edit_new(const struct ut_edit_io *io);
void ut_edit_free(struct ut_editor *e);         /* NULL is fine */

/* Draw the prompt on a fresh row and read a line for the terminal t: its length, the line in
   buf (UT_COMMAND_MAX + 1 bytes, terminated), or one of UT_EDIT_*. Without raw mode (ttyctl
   fails) the console's own line editing reads it. */
int ut_edit_line(struct ut_editor *e, const struct ut_term *t, int drop_typed, char *buf);

#endif
