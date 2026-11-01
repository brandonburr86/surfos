/*
SurfOS Undertow's window: a page as a tree of boxes
----------------------
File: doc.h  Date: 10/25/26
----------------------
A response becomes a document the window lays out (layout.c) and paints (paint.c): a tree of block boxes,
each holding either blocks or a range of inline runs, which are styled text, line breaks, images (their alt
text, until images are loaded) and form fields (shown, not used). The tree follows the HTML standard as
Undertow's text does (html.c): the same scanner (html_scan.c), the same implied ends and scopes, the same
charset rules, the same limits; html.h's link and anchor types, with links resolved as html.c resolves them.

Block boxes:
- WB_BLOCK: a paragraph, a heading (level 1-6), a division, a quote, a preformatted text, a list (its items
  are a level deeper), a definition, a caption, or an anonymous block made for inline content that comes
  between blocks (the CSS rule: a block holds blocks or inline content, never both);
- WB_ITEM: a list item, with its marker;
- WB_TABLE, WB_ROW, WB_CELL: a table's rows and cells (thead, tbody and tfoot are not kept);
- WB_HR: a rule.

Every string is safe: no ut_unsafe_char() character, and none that a console cannot show (ut_html_hidden());
text has its whitespace collapsed (but in WS_PRE runs), so a run never starts a block with a space.
*/

#ifndef WUNDERTOW_DOC_H
#define WUNDERTOW_DOC_H

#include <stddef.h>
#include "http.h"
#include "html.h"

#define WD_NONE        0xff000000u      /* a colour the page did not give */
#define WD_TEXT        0xfe000000u      /* a colour: the page's text colour, also for a link (CSS's inherit, initial) */

#define WD_BLOCKS_MAX  32768u           /* boxes of a page, at most; past them the page stops (a notice says so) */
#define WD_RUNS_MAX    131072u          /* runs of a page, at most */
#define WD_IMAGES_MAX  2048u            /* images whose address is kept */
#define WD_SIZE_MAX    4096             /* width and height attributes, in pixels, at most */

enum { WB_BLOCK, WB_ITEM, WB_TABLE, WB_ROW, WB_CELL, WB_HR };

#define WBF_PARA    0x0001              /* a paragraph's space above and below (p, pre, blockquote, lists, ...) */
#define WBF_INDENT  0x0002              /* indented at the left (dd, a list's items) */
#define WBF_QUOTE   0x0004              /* indented at both sides (blockquote) */
#define WBF_PRE     0x0008              /* whitespace kept (pre, listing, xmp, plain text) */
#define WBF_ANON    0x0010              /* made for inline content between blocks */
#define WBF_LIST    0x0020              /* a list (ul, ol, menu, dir) */
#define WBF_HEADER  0x0040              /* a header cell (th) */
#define WBF_CAPTION 0x0080              /* a table's caption */

enum { WA_INHERIT, WA_LEFT, WA_CENTER, WA_RIGHT };
enum { WMK_NONE, WMK_DISC, WMK_CIRCLE, WMK_SQUARE, WMK_DECIMAL, WMK_LOWER_ALPHA, WMK_UPPER_ALPHA,
       WMK_LOWER_ROMAN, WMK_UPPER_ROMAN };

struct wd_block {
    unsigned char kind;                 /* WB_* */
    unsigned char level;                /* a heading's 1-6, else 0 */
    unsigned char align;                /* WA_*: its inline content's (WA_INHERIT: the parent's); a table's place */
    unsigned char marker;               /* WB_ITEM: WMK_* */
    unsigned short flags;               /* WBF_* */
    unsigned short colspan;             /* WB_CELL: 1-64 */
    unsigned short border;              /* WB_TABLE: the border attribute, 0-8 pixels */
    unsigned short padding;             /* WB_TABLE: cellpadding, 0-32 pixels (or 1 when not given) */
    short width;                        /* WB_TABLE, WB_CELL: pixels (> 0), percent (< 0, -1..-100), or 0 */
    long number;                        /* WB_ITEM with a counting marker: its number */
    unsigned int bg;                    /* its background: bgcolor (WB_TABLE, WB_ROW, WB_CELL), the page's CSS, or WD_NONE */
    int parent, first, last, next;      /* the tree: indices into blocks[], -1 for none; blocks[0] is the root */
    unsigned int run, nruns;            /* inline content (a block without children): runs[run .. run + nruns) */
    short margin[4], pad[4];            /* the page's CSS, margins and paddings: top, right, bottom, left: pixels, or
                                           percent of the width when negative, where given says so */
    unsigned char given;                /* 1 << side: that margin is given; 16 << side: that padding */
};

enum { WR_TEXT, WR_BR, WR_IMAGE, WR_FIELD };

#define WS_BOLD     0x0001
#define WS_ITALIC   0x0002
#define WS_MONO     0x0004              /* code, tt, kbd, samp, pre (every font is monospaced today) */
#define WS_UNDER    0x0008
#define WS_STRIKE   0x0010
#define WS_SUB      0x0020
#define WS_SUP      0x0040
#define WS_PRE      0x0080              /* spaces kept (line breaks are WR_BR) */
#define WS_NOWRAP   0x0100              /* nobr: no line break inside */
#define WS_MARK     0x0200              /* mark: highlighted */
#define WS_NOLINE   0x0400              /* a link without its underline (text-decoration: none) */

enum { WZ_NORMAL, WZ_H1, WZ_H2, WZ_H3, WZ_H4, WZ_H5, WZ_H6, WZ_BIG, WZ_SMALL };

enum { WF_TEXT, WF_PASSWORD, WF_BUTTON, WF_CHECKBOX, WF_RADIO, WF_SELECT, WF_AREA, WF_FILE };

struct wd_run {
    unsigned char kind;                 /* WR_* */
    unsigned char size;                 /* WZ_*: a heading's level, big, small */
    unsigned char field;                /* WR_FIELD: WF_* */
    unsigned char checked;              /* WR_FIELD: a checkbox or radio button that is checked */
    unsigned short style;               /* WS_* */
    unsigned short w, h;                /* WR_IMAGE: width and height attributes (0: not given) */
    unsigned int color;                 /* <font color>, or WD_NONE: the page's text colour, or its link colour */
    int link;                           /* links[link], or -1 */
    int image;                          /* WR_IMAGE: images[image], or -1 */
    unsigned int off, len;              /* its text in text[]: WR_TEXT the text, WR_IMAGE the alt text,
                                           WR_FIELD the label (a value, a button's text, the first option) */
    unsigned int bg;                    /* the page's CSS: the background behind the text, or WD_NONE */
    unsigned short px;                  /* the page's CSS: the font's size in pixels, 0: by size */
};

struct wd_document {
    char *text;                         /* every run's text, text_len bytes and a NUL */
    size_t text_len;
    struct wd_block *blocks;
    size_t block_count;
    struct wd_run *runs;
    size_t run_count;
    struct ut_link *links;              /* html.h's: the label (its text, one line), the address, usable */
    size_t link_count;
    struct ut_anchor *anchors;          /* html.h's: a name and the text offset its element starts at */
    size_t anchor_count;
    char **images;                      /* each image's address: absolute when usable, else NULL */
    size_t image_count;
    char *title;                        /* safe text, one line, at most UT_FIELD_MAX bytes; "" when none */
    unsigned int bg, fg, link_color, vlink_color;  /* <body bgcolor text link vlink>, or WD_NONE */
    size_t bytes;                       /* everything the document owns */
    int truncated;                      /* html.h's UT_TRUNC_*, and WD_TRUNC_BOXES */
    char notice[160];                   /* safe text for the status bar, "" when there is nothing */
};

extern int wd_base;                     /* the base font size of the pages built, in pixels (16: the desktop's font) */

#define WD_TRUNC_BOXES 8                /* boxes or runs past WD_BLOCKS_MAX or WD_RUNS_MAX */

/* Build r's document: text/html and application/xhtml+xml as HTML, any other text as preformatted text, with
   the charset and base rules of html.h. 0 and *out (free it with wd_free()), or -1 with a message and *out
   unchanged (out of memory). */
int wd_build(const struct ut_response *r, struct wd_document *out, char *err, size_t cap);

/* Free everything a document owns and zero it; a zeroed document, or NULL, is fine. */
void wd_free(struct wd_document *d);

/* The text offset of the fragment target name, as ut_anchor_offset() finds it. 0 and *offset, or -1. */
int wd_anchor_offset(const struct wd_document *d, const char *fragment, size_t *offset);

/* A colour as HTML's legacy attributes give it ("#rrggbb", "#rgb", "rrggbb", a name): 0x00RRGGBB, or WD_NONE */
unsigned int wd_color(const char *s);

/* The document as text, one line per box and run (the host tests' fixtures): malloc'ed, NULL without memory */
char *wd_dump(const struct wd_document *d);

#endif
