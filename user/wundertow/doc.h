/*
SurfOS Undertow's window: a page as a tree of boxes
----------------------
File: doc.h  Date: 10/25/26
----------------------
A response becomes a document the window lays out (layout.c) and paints (paint.c): a tree of block boxes,
each holding either blocks or a range of inline runs, which are styled text, line breaks, images and form
fields (shown, not used). The tree follows the HTML standard as
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
    unsigned int bg;                    /* WB_TABLE, WB_ROW, WB_CELL: bgcolor, or WD_NONE */
    int parent, first, last, next;      /* the tree: indices into blocks[], -1 for none; blocks[0] is the root */
    unsigned int run, nruns;            /* inline content (a block without children): runs[run .. run + nruns) */
};

enum { WR_TEXT, WR_BR, WR_IMAGE, WR_FIELD };

/* Forms (11/1/26, stage 2.6; form.h has what they do). A run of kind WR_FIELD shows a control; a control's value is what
   a person made of it, so everything here but the page's defaults changes while the page is shown. */
#define WD_FORMS_MAX     512u           /* forms of a page */
#define WD_CONTROLS_MAX  4096u          /* controls of a page */
#define WD_OPTIONS_MAX   16384u         /* a select's options, all of a page's */
#define WD_FORM_BYTES    (1u << 20)     /* the names, values and labels of a page's forms: a budget of their own */
#define WD_VALUE_MAX     (64u << 10)    /* the bytes of what is in a field */

#define WC_HIDDEN  8                    /* a control's kind, past the WF_*: <input type=hidden>, which has no box */
enum { WC_SUBMIT = 1, WC_RESET, WC_PUSH };      /* what a button does */
enum { WM_GET, WM_POST, WM_DIALOG };    /* a form's method */

struct wd_form {
    char *action;                       /* its address, absolute; NULL: not one that can be used */
    char *id;
    unsigned char method;               /* WM_* */
    unsigned char password;             /* it has a password field */
    unsigned char too_much;             /* a limit took something of it: it is never sent */
    unsigned char here;                 /* no action: the page's own address (until the page's end) */
    unsigned char action_cut;           /* an action longer than was kept */
    unsigned char accept;               /* its accept-charset, a UT_CS_* + 1 (UT_CS_ASCII: one not written), 0: none */
};

struct wd_option {
    char *value, *label;
    unsigned char on, dflt, disabled;
};

struct wd_control {
    unsigned char kind, role;           /* WF_*, WC_HIDDEN; a button's WC_* */
    unsigned char checked, dflt_checked;
    unsigned char disabled, readonly, multiple, image, cut, action_cut, has_value;
    unsigned char formmethod;           /* a button's: WM_* + 1, 0: the form's */
    int form;                           /* forms[form], or -1 */
    int run;                            /* its box, -1: none */
    char *name;                         /* NULL: none */
    char *owner;                        /* its form= attribute */
    char *formaction;                   /* a button's address, absolute; NULL: none, or none it can use (action_cut) */
    char *value, *dflt;                 /* the value now, and as the page has it */
    size_t len, cap, caret;             /* of value: its bytes, the room for it, where typing goes */
    size_t first;                       /* the first byte shown (a field too short for its text) */
    int top;                            /* an area's first line shown */
    int maxlength;                      /* -1: none */
    int rows, cols, size;               /* 0: none */
    unsigned int first_option, noptions;
    unsigned char here;                 /* an empty formaction: the page's own address (until the page's end) */
};

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
    int control;                        /* WR_FIELD: controls[control], or -1 */
};

/* What the window has of an image, by the number in images[] (11/1/26, stage 2.4): the frame shows until px does */
enum { WP_WAIT, WP_RUNNING, WP_DONE, WP_FAILED, WP_REFUSED };

struct wd_picture {
    unsigned char state;                /* WP_*: waiting, being loaded, here, not to be had, not allowed */
    int w, h;                           /* its own size once known, else 0 */
    int pw, ph;                         /* the pixels kept: smaller than w x h when it is large (image.h) */
    unsigned int *px;                   /* image.h's 0xTTRRGGBB, or NULL */
    int sw, sh;                         /* the pixels at the size it is shown at, where it is shown smaller */
    unsigned int *scaled;
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
    /* 11/1/26 */
    struct wd_picture *pics;            /* image_count of them: the window fills them in */
    unsigned char *visited;             /* link_count of them, or NULL: 1 for a link to where this window has been */
    struct wd_form *forms;
    size_t form_count;
    struct wd_control *controls;
    size_t control_count;
    struct wd_option *options;
    size_t option_count;
    size_t form_bytes;                  /* what they hold */
    int form_charset;                   /* what the page is in: what a form is sent in */
};

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
