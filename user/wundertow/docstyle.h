/*
SurfOS Undertow's window: a page's CSS on its boxes and runs
----------------------
File: docstyle.h  Date: 11/01/26
----------------------
The cascade, for doc.c: the sheets of a page (css.h) and its style attributes over what HTML gives. HTML's own
attributes and elements stay, below the page's CSS.

doc.c keeps its counters of open inline elements (<b>, <font>, ...), which are as forgiving as browsers' formatting
elements. This keeps a stack of its own beside them, of every element that is open, inline ones too, which selectors
are matched against, with what the cascade said of each. What it says of a run is an opinion over doc.c's counters:
ds_style() and ds_color() take what HTML gives and return what the page's CSS makes of it. An element that says
something by itself (words: <b> bold, <font color> its colour, an <a href> its link colour) ends the opinions of the
elements around it on that, as its own rule in a browser's style sheet does; its own declarations come after that.

Selectors are matched when an element opens, against the elements open then: all of them look only at an element's
ancestors, so that is all they need. So a page's sheets come first: ds_new() takes them from the whole page, wherever
in it a <style> stands, in the order they stand in. The cost is bounded: selectors are filed by what their subject
asks for (css.h's index), and past DS_WORK_MAX steps of matching a page's sheets count no more (its style attributes
do), with a notice. Colours, sizes and margins are all kept as numbers, and nothing in a sheet reaches a screen.

A page's root elements are always there, as in a browser: the html and body frames exist before any tag does, and a
real <html> or <body> gives them their attributes when it comes. What they say of the page goes to the document
(ds_finish()): its colours, and the page's margins on the root box.
*/

#ifndef WUNDERTOW_DOCSTYLE_H
#define WUNDERTOW_DOCSTYLE_H

#include <stddef.h>
#include "doc.h"

#define DS_DEPTH    256                 /* elements open: deeper ones have no style of their own */
#define DS_WORK_MAX 32000000u           /* steps of matching a page gets */
#define DS_PX_MAX   256                 /* the largest font-size kept, in pixels */

/* what an element says by itself: the WS_* it gives, and these */
#define DS_W_COLOR  0x10000u            /* a colour (<font color>) */

/* how an element is displayed: what the cascade said of it */
enum {
    DS_ASIS,                            /* it said nothing */
    DS_NONE,                            /* display: none */
    DS_BLOCK,                           /* block */
    DS_INLINE,                          /* inline */
    DS_SHOWN                            /* something else that shows it (table-cell, ...): as HTML has it */
};

struct ds_element {
    const unsigned char *in;            /* the page, with the tag's name at in[q]; NULL: no tag made it */
    size_t len, q;
    int cs;                             /* the page's charset (html_scan.h) */
    int el;                             /* E_* */
    const char *name;                   /* its name in lowercase */
    unsigned int words;                 /* what it says by itself */
    unsigned short style;               /* the style around it, the colour around it: what inherit gets */
    unsigned int color;
};

struct ds;

/* The page's cascade: the <style> elements of the page (len bytes, in charset cs). NULL without memory. */
struct ds *ds_new(const unsigned char *in, size_t len, int cs);
void ds_free(struct ds *s);

/* The elements open now; closing to that many again closes what opened since */
int ds_height(const struct ds *s);
void ds_close(struct ds *s, int height);

/* An element opens: how it is displayed. Its frame stays open until ds_close() or ds_end(). */
int ds_open(struct ds *s, const struct ds_element *e);

/* <html> or <body> is here (DS_ROOT_*): it gives the frame that was waiting for it its attributes */
#define DS_ROOT_HTML 0
#define DS_ROOT_BODY 1
void ds_root(struct ds *s, int which, const struct ds_element *e);

/* Is the page's root display: none, and so all of the page? */
int ds_hidden(const struct ds *s);

/* An end tag of a name: the nearest open element of that name, down to floor, closes */
void ds_end(struct ds *s, const char *name, int floor);

/* Is that frame's element of this name? */
int ds_named(const struct ds *s, int height, const char *name);

/* What the page's CSS said of the element that just opened goes on its box */
void ds_box(struct ds *s, struct wd_block *x, int display);

/* The look of text now, over HTML's */
unsigned short ds_style(const struct ds *s, unsigned short style);
unsigned int ds_color(const struct ds *s, unsigned int color);
unsigned int ds_bg(const struct ds *s);
unsigned short ds_px(const struct ds *s);

/* At the page's end: what its root elements said of it, on d and its root box; 1 when some of its style was left out */
int ds_finish(struct ds *s, struct wd_document *d);

#endif
