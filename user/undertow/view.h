/*
SurfOS Undertow: the screen: wrapped text and one screen of a page at a time
----------------------
File: view.h  Date: 10/11/26
----------------------
A page is read a screen at a time, as the shell's other programs print: no
full-screen library, no clearing of the screen. A view is exactly rows - 1
lines, so with the undertow> prompt under it it fills the terminal and the
shell's pager (TTY_PAGE) never stops it at --More--:

    Undertow - Example Domain                        [HTTPS verified TLSv1.3]
    https://example.com/
    ------------------------------------------------------------------------
    the page's rows: rows - 6 of them (blank ones after the end of the page)
    ------------------------------------------------------------------------
    lines 1-19 of 240 (7%)  Enter: next screen  NUMBER: follow a link  help
    undertow>

The rules are U+2500 (box drawing) where the console draws UTF-8, dashes
otherwise. The status at the right is [HTTPS verified TLSv1.3] or
[HTTP unencrypted], after [HTTP 404 Not Found] when the status is not 200; the
title and the status give way (shorter forms first) on a narrow terminal. A
terminal of fewer than 8 rows gets the title line, the page's rows and the
position line only.

A character is drawn as itself where the terminal can draw it, and as its
ut_ascii_for() stand-in where it cannot: on a terminal without UTF-8 every
character but ASCII; on the SurfOS console, whose cells are code page 437
(driver/cp437.c, shared through libsurf), every character outside it, so thin
and zero-width spaces, the euro sign or a dagger show as " ", nothing, "EUR"
or "+" rather than '?'; a telnet session's terminal draws them all. Text is
wrapped to the width in display columns, as the characters are drawn (one a
character drawn as itself, the length of a stand-in), breaking at spaces and
inside a word longer than a row, never inside a UTF-8 sequence. A wrapped line's continuation rows are
indented under its text after the line's leading spaces and list marker ("* ",
"1. "), unless that would leave fewer than 20 columns. A row ends with "\n", or
with "\r\n" when it fills the width (the console wraps the VT100 way, and a
full row's "\n" alone would leave an empty row on the screen).

Colours (escape sequences of the subset in include/surfos/tty.h) only when
the terminal has them, and only for the chrome, link markers and headings;
without UTF-8 every character goes through ut_ascii_for(). Every byte printed
from a page or a server is made safe: one-line texts (the title, the address,
messages) through ut_safe_text(), the page's rows character by character by
the same rules: ut_unsafe_char() characters are left out, line breaks and tabs
are spaces, ill-formed UTF-8 shows as U+FFFD.

Reading positions are byte offsets in a page's text: a screen starts at the
row that holds the position, so a change of the terminal's size, a find or an
anchor shows the right text at the top. Nothing here keeps any state.
*/

#ifndef UNDERTOW_VIEW_H
#define UNDERTOW_VIEW_H

#include <stdio.h>
#include <stddef.h>
#include "html.h"
#include "session.h"

struct ut_term {
    unsigned int cols, rows;    /* the terminal's size (TTY_GETSIZE) */
    int utf8;                   /* it draws UTF-8 (TTY_CAP_UTF8) */
    int color;                  /* it draws the ANSI colours (TTY_CAP_COLOR) */
    int remote;                 /* a telnet session's (a pseudo-terminal): it draws any character;
                                   0: the SurfOS console, which draws code page 437's */
};

#define UT_SPAN_TYPED 16        /* a span kind of the view's own lists: what to type (teal) */

/* How this terminal draws the character cp: NULL for as itself, else its ASCII stand-in (see the
   top). --dump on a terminal draws its lines this way too, without wrapping them. */
const char *ut_view_stand_in(const struct ut_term *t, unsigned int cp);

/* The rows of page text a view has on this terminal: rows - 6, at least 1. */
unsigned int ut_view_text_rows(const struct ut_term *t);

/* Rows of text (len bytes): where the row that holds offset starts (len or more: the start of
   the last row); where the screen after the one at from's row starts (len when that screen
   reaches the end); where the screen before it starts (0 at the top). */
size_t ut_view_row_start(const struct ut_term *t, const char *text, size_t len, size_t offset);
size_t ut_view_next(const struct ut_term *t, const char *text, size_t len, size_t from);
size_t ut_view_prev(const struct ut_term *t, const char *text, size_t len, size_t from);

/* One screen of p from the row that holds from (see the top): rows - 1 lines. message, when
   not NULL or "", takes the place of the keys on the position line (cut to fit), as does the
   page's notice on its first screen. Returns ut_view_next() of from. */
size_t ut_view_page(FILE *out, const struct ut_term *t, const struct ut_page *p, size_t from,
                    const char *message);

/* The welcome screen, rows - 1 lines like a view: what to type, then clock and https, its two
   lines about HTTPS (the clock it checks dates against, and whether it can work here). */
void ut_view_welcome(FILE *out, const struct ut_term *t, const char *clock, const char *https);

/* len bytes of lines, wrapped as a page's rows are, with the colours of spans (UT_SPAN_HEADING,
   UT_SPAN_LINK, UT_SPAN_TYPED): help, lists and messages. The shell's pager may stop these. */
void ut_view_text(FILE *out, const struct ut_term *t, const char *text, size_t len,
                  const struct ut_span *spans, size_t nspans);

/* --dump's output: the document's text line by line, then, when it has links, an empty line,
   "Links:" and "[N] address" for each; every line through ut_safe_text(); on a terminal (t not
   NULL) each character as ut_view_stand_in() draws it, the lines never wrapped (a script reading
   the serial line keeps them whole). 0, or -1 and why when not all of it could be written: out of
   memory (then nothing is written), or out's error flag (ferror()) once it is flushed. */
int ut_view_dump(FILE *out, const struct ut_term *t, const struct ut_document *d, char *err, size_t cap);

/* before, s and after on one row: s (safe text) is cut short with an ellipsis to fit. */
void ut_view_line(FILE *out, const struct ut_term *t, const char *before, const char *s, const char *after);

#endif
