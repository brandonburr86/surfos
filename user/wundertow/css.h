/*
SurfOS Undertow's window: style sheets
----------------------
File: css.h  Date: 11/01/26
----------------------
A page's CSS, hostile bytes as much as its HTML, read tolerantly as CSS Syntax Level 3 reads it and kept in a form
docstyle.c matches elements against. Everything here is a number: a name is its hash (css_name()), a value its
property, unit and amount. No address, no string of a sheet is kept, so nothing in a sheet can reach the screen or
make the browser load anything; the one exception is css_imports(), which hands @import's strings to whoever may
load them.

What is kept:
- selectors: type, class, id, descendant, child, grouping, and :link, each with its specificity. Every link counts
  as :link: a sheet is never told which were visited, so a :visited selector matches nothing, and a link gets its
  page's unvisited look;
- declarations: color, background-color, display, font-weight, font-style, text-decoration, margin-*, padding-*,
  text-align, font-size, and the shorthands background, font, margin and padding, each with its !important.

What is not: anything else is skipped, as the standard says. A declaration it cannot read goes alone; a selector
that is malformed takes its whole rule, and one that is well-formed but needs what is not here (:hover, [attr],
a + b) matches nothing, the others of its list stay. inherit, initial, unset and revert work on what is kept. @media
blocks count for a screen, without looking at features: a query with one is a window of no known size, and does not
match. Every other at-rule is skipped, its block with it. @import is css_imports()'s, and url() is never read at all.

Bounded: a sheet's bytes, the rules, selectors and declarations of a page, a rule's selectors, how deep brackets
open. Past a limit the sheet stops where it is and what came before it stays (truncated says so); every allocation
is checked. Integers only: a number is kept in 1/256ths and saturates.
*/

#ifndef WUNDERTOW_CSS_H
#define WUNDERTOW_CSS_H

#include <stddef.h>

#define CSS_BYTES_MAX      (1024u * 1024)  /* a sheet: a page's limit (a bigger one is left out whole) */
#define CSS_RULES_MAX      16384u          /* rules of a page's sheets together */
#define CSS_SELECTORS_MAX  32768u          /* selectors of a page */
#define CSS_PARTS_MAX      131072u         /* the parts of those selectors */
#define CSS_DECLS_MAX      65536u          /* declarations of a page */
#define CSS_RULE_SELECTORS 256u            /* selectors of one rule: a longer list loses its rule */
#define CSS_SEL_PARTS      32              /* parts of one selector: a longer one matches nothing */
#define CSS_DEPTH_MAX      32              /* brackets and blocks open at once: past it the sheet stops */
#define CSS_IMPORTS_MAX    8u              /* @import rules of a sheet that are looked at */
#define CSS_URL_MAX        2048u           /* bytes of an @import's address kept */
#define CSS_BUCKETS        4096u           /* the index of selectors */

enum {
    CP_COLOR, CP_BACKGROUND, CP_DISPLAY, CP_WEIGHT, CP_STYLE, CP_DECORATION, CP_MARGIN_TOP, CP_MARGIN_RIGHT,
    CP_MARGIN_BOTTOM, CP_MARGIN_LEFT, CP_PADDING_TOP, CP_PADDING_RIGHT, CP_PADDING_BOTTOM, CP_PADDING_LEFT, CP_ALIGN,
    CP_SIZE, CP_COUNT
};

/* a declaration's unit: what its value is */
enum {
    CU_NONE,        /* a word's value: display, weight, style, decoration, alignment (CSS_*) */
    CU_RGB,         /* 0x00RRGGBB */
    CU_TRANSPARENT, /* no colour: value 0 */
    CU_INHERIT,     /* what the parent's is (inherit, unset, currentcolor): color, weight, style, size, alignment,
                       text-decoration */
    CU_PX,          /* pixels, in 1/256ths */
    CU_EM,          /* the font's size: margins the element's, font-size the parent's, in 1/256ths */
    CU_REM,         /* the page's base size, in 1/256ths */
    CU_PCT,         /* percent, in 1/256ths: margins of the containing block's width, font-size of the parent's */
    CU_AUTO,        /* margin: auto */
    CU_INITIAL,     /* color: the page's text colour (initial) */
    CU_REVERT,      /* what the page's rules say counts for nothing here: HTML's own look stands (revert) */
    CU_BASE         /* font-size: of the browser's base size, in 1/256ths: its keywords, initial; rem goes by the root's */
};

enum { CSS_D_NONE, CSS_D_BLOCK, CSS_D_INLINE, CSS_D_ASIS };    /* ASIS: shown, as HTML has it (display: table-cell) */
enum { CSS_A_LEFT, CSS_A_CENTER, CSS_A_RIGHT };
#define CSS_LINE_UNDER   1              /* text-decoration */
#define CSS_LINE_THROUGH 2

struct css_decl {
    unsigned char prop;                 /* CP_* */
    unsigned char unit;                 /* CU_* */
    unsigned char important;
    int value;
};

/* A selector's parts, its subject's compound first, right to left: each compound's simple selectors (none: '*'),
   then how it joins the next */
enum { CSS_TYPE, CSS_ID, CSS_CLASS, CSS_LINK, CSS_DESCENDANT, CSS_CHILD };

struct css_part {
    unsigned int kind;                  /* CSS_* */
    unsigned int hash;                  /* css_type() of a type, css_name() of an id or class */
};

struct css_selector {
    unsigned int first, nparts;         /* parts[first .. first + nparts) */
    unsigned int spec;                  /* ids << 20 | classes << 10 | types */
    unsigned int decl, ndecl;           /* decls[decl .. decl + ndecl): the rule's */
    unsigned int key;                   /* the subject's id, else a class, else its type: what the index files it by */
    int next;                           /* the next selector of its bucket, -1 */
};

/* What a page's sheets come to, in the cascade's order: the selector's place in sels[] is its order */
struct css {
    struct css_part *parts;
    size_t nparts, cparts;
    struct css_selector *sels;
    size_t nsels, csels;
    struct css_decl *decls;
    size_t ndecls, cdecls;
    size_t rules;
    int any;                            /* selectors with no key ('*', :link alone): -1 for none */
    int head[CSS_BUCKETS];              /* the others by key: -1 for none */
    int truncated;                      /* a limit stopped a sheet, or a sheet was too big */
    int failed;                         /* out of memory */
};

void css_init(struct css *c);
void css_free(struct css *c);

/* Add a sheet of n bytes after those before it: 0, or -1 out of memory (what came before stays usable) */
int css_add(struct css *c, const char *s, size_t n);

/* The declarations of a style attribute: how many were written to out (cap at most; those past it are dropped) */
size_t css_style(const char *s, size_t n, struct css_decl *out, size_t cap);

/* A media query list ("screen", "print", "screen and (max-width:600px)", ""): does it hold on this screen? */
int css_media(const char *s, size_t n);

/* The addresses of a sheet's @import rules whose media hold, as written, in urls: how many. Only the string form:
   @import url(...) is an url(), which is never followed. */
size_t css_imports(const char *s, size_t n, char (*urls)[CSS_URL_MAX + 1], size_t cap);

/* A class's or an id's name as the selectors' hashes have it; a tag's name (lowercased, 15 bytes at most, as
   html_scan.h keeps it) likewise. Never 0. */
unsigned int css_name(const char *s, size_t n);
unsigned int css_type(const char *s, size_t n);

#endif
