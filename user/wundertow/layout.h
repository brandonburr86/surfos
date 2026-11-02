/*
SurfOS Undertow's window: a page laid out at a width
----------------------
File: layout.h  Date: 10/25/26
----------------------
A document (doc.h) at a width in pixels becomes a list of things to paint, top to bottom as the page reads:
text, backgrounds, frames, rules, list markers, fields and images, each at its place on the page, with the link
it belongs to. paint.c paints any part of it; the window finds links under the pointer and the place of a
fragment target in it. Whole pixels only, no floating point.

An image's box has the size its width and height attributes give it, from the picture's own shape when only one
of them is there; else, once the picture is known (doc.h's pics), the picture's size; until then its alt text in a
frame, or nothing when it has no alt text. It is never wider than the room it has, and keeps its shape.

The rules, as browsers have them by default: an 8-pixel margin around the page; paragraphs, headings, lists
and quotes 16 pixels apart (the base font's height; two margins together are the larger of them); lists,
definitions and quotes indented by 40; headings in the sizes a browser gives them, at the nearest size there is
(h1 32, h2 24, the others 16, over a base of 16); lines broken at spaces, and a word too wide for a line
broken where the line ends; pre's lines kept, and broken only where they would leave the page; a list item's
marker in its indentation, on its first line; tables sized to their cells' content within the room they have
(their width attribute too), cells 2 pixels apart, with their padding and borders.
*/

#ifndef WUNDERTOW_LAYOUT_H
#define WUNDERTOW_LAYOUT_H

#include <stddef.h>
#include "doc.h"

enum {
    WI_TEXT,                            /* a run's text on one line: off, len in the document's text */
    WI_FILL,                            /* a background (a table's, a row's, a cell's, a mark's): color */
    WI_FRAME,                           /* a one-pixel frame (a table's and its cells' borders) */
    WI_RULE,                            /* <hr> */
    WI_MARK,                            /* a list item's marker: off, len in the layout's own marks */
    WI_FIELD,                           /* a form field's box: sub is its WF_*; off, len its label */
    WI_IMAGE                            /* an image's box: off, len its alt text */
};

struct wl_item {
    unsigned char kind;                 /* WI_* */
    unsigned char scale;                /* the text's size: WF_S16, WF_S24, WF_S32 (font.h) */
    unsigned char sub;                  /* WI_FIELD: WF_*; WI_FIELD's checked */
    unsigned char checked;
    unsigned short style;               /* WS_* of the run */
    int x, y, w, h;                     /* on the page, in pixels */
    unsigned int color;                 /* its colour, or WD_NONE: the page's (text, link, frame) */
    int link;                           /* the link it belongs to, or -1 */
    unsigned int off, len;
    /* 11/1/26 */
    int image;                          /* WI_IMAGE: the document's pics[image], or -1 */
    int control;                        /* WI_FIELD: the document's controls[control], or -1 */
};

struct wl_layout {
    struct wl_item *items;
    size_t count, cap;
    char *marks;                        /* the markers' text ("•", "12."), UTF-8 */
    size_t marks_len, marks_cap;
    int width, height;                  /* the page's size: at least the width it was laid out at */
    int truncated;                      /* WL_ITEMS_MAX reached: the rest of the page is not laid out */
};

#define WL_ITEMS_MAX 1000000u
#define WL_MARGIN    8                  /* around the page */
#define WL_WIDTH_MAX 4096               /* the widest page laid out: --render's 8000 rows of it are 128 MB at most */

/* Lay d out at width pixels (64 to WL_WIDTH_MAX): 0 and *out (free it with wl_free()), or -1 without memory */
int wl_layout(const struct wd_document *d, int width, struct wl_layout *out);
void wl_free(struct wl_layout *l);

/* The link of what is at (x, y) on the page, or -1 */
int wl_link_at(const struct wl_layout *l, int x, int y);

/* 11/1/26: the control whose field is at (x, y), or -1; the item of a control, or -1 */
int wl_control_at(const struct wl_layout *l, int x, int y);
long wl_control_item(const struct wl_layout *l, int control);

/* The top of the line where the document's text offset is (a fragment target's place), 0 when none */
int wl_offset_y(const struct wl_layout *l, size_t offset);

/* 11/1/26: what is at the top of a view that starts at y, so that the same can be there after the page is laid out
   again: its text offset and how far into it y is (1), or 0 when nothing is. wl_place_y() is that place in a layout. */
int wl_place(const struct wl_layout *l, int y, size_t *offset, int *into);
int wl_place_y(const struct wl_layout *l, size_t offset, int into);

/* The layout as text, one line per item (the host tests' fixtures): malloc'ed, NULL without memory */
char *wl_dump(const struct wl_layout *l, const struct wd_document *d);

#endif
