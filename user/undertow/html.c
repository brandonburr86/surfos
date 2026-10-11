/*
SurfOS Undertow: HTML (and plain text) as readable text with numbered links
----------------------
File: html.c  Date: 10/11/26
----------------------
html.h's renderer, in one pass over the body with no recursion and no tree:

- a tokenizer after the HTML standard's: comments and bogus comments, raw
  text elements (script, style, title, textarea, ...) that end only at their
  own end tag, attributes quoted or not, character references;
- a small stack of the open elements that shape the text (paragraphs, lists,
  tables, headings, ...) with the standard's rules for closing them, so that
  unclosed and mismatched tags still give sensible text; inline elements are
  not kept at all;
- a writer that collapses whitespace, starts lines, indents lists and quotes,
  joins table cells with " | ", numbers links after their labels, and lets no
  unsafe character (ut_unsafe_char()) into anything it makes.

Everything is bounded: the stack (deeper elements are only counted), each
attribute that is kept, the text, the links, the anchors, the spans, the
title. Whatever is past a bound is skipped in time linear in the input.
*/

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "undertow.h"
#include "url.h"
#include "html.h"

#define STACK_MAX    128                /* open elements kept; deeper ones are only counted */
#define INDENT_MAX   32                 /* columns of indentation at most, however deep the lists */
#define HREF_MAX     4096               /* bytes of an address kept from an attribute (url.c takes 2048) */
#define PRESCAN      1024               /* bytes searched for <meta charset> */
#define LABEL_MAX    64                 /* bytes of a charset's name kept for the notice */
#define HEADING_SPANS (UT_SPANS_MAX - UT_LINKS_MAX)     /* every link keeps room for its [N] */
#define AHASH        1024               /* slots of the anchor names' hash table */
#define RULE         "----------------------------------------"         /* <hr>: 40 of them */

static const char cut_notice[] = "[Undertow: the rest of this page is not shown: it is over 512 KB]";

enum { CS_UTF8, CS_1252, CS_ASCII, CS_UTF16 };

/**** elements ****/

#define F_VOID    0x0001    /* no content and no end tag */
#define F_LINE    0x0002    /* starts and ends a line */
#define F_PARA    0x0004    /* a blank line before and after */
#define F_CLOSEP  0x0008    /* its start tag ends an open <p> */
#define F_PUSH    0x0010    /* kept on the stack while it is open */
#define F_SCOPE   0x0020    /* an end tag inside cannot close what is outside (td, th, table, ...) */
#define F_RAW     0x0040    /* raw text up to its own end tag (script, style, ...) */
#define F_RCDATA  0x0080    /* the same, with character references (title, textarea) */
#define F_HIDE    0x0100    /* its content is not shown (template, svg, math, object, ...) */
#define F_OUT     0x0200    /* ends an open svg or math (the standard's list for foreign content) */
#define F_LIST    0x0400    /* ul, ol, menu, dir */
#define F_HEAD    0x0800    /* h1-h6 */
#define F_SPECIAL 0x1000    /* the standard's special elements: they stop the search for an li, dt or dd to close */

enum {
    E_OTHER, E_A, E_ADDRESS, E_APPLET, E_AREA, E_ARTICLE, E_ASIDE, E_B, E_BASE, E_BASEFONT, E_BGSOUND, E_BIG,
    E_BLOCKQUOTE, E_BODY, E_BR, E_BUTTON, E_CAPTION, E_CENTER, E_CODE, E_COL, E_COLGROUP, E_DATALIST, E_DD,
    E_DETAILS, E_DIALOG, E_DIR, E_DIV, E_DL, E_DT, E_EM, E_EMBED, E_FIELDSET, E_FIGCAPTION, E_FIGURE, E_FONT,
    E_FOOTER, E_FORM, E_FRAME, E_FRAMESET, E_H1, E_H2, E_H3, E_H4, E_H5, E_H6, E_HEAD, E_HEADER, E_HGROUP, E_HR,
    E_HTML, E_I, E_IFRAME, E_IMAGE, E_IMG, E_INPUT, E_KEYGEN, E_LEGEND, E_LI, E_LINK, E_LISTING, E_MAIN, E_MARQUEE,
    E_MATH, E_MENU, E_META, E_NAV, E_NOBR, E_NOEMBED, E_NOFRAMES, E_NOSCRIPT, E_OBJECT, E_OL, E_OPTGROUP, E_OPTION,
    E_P, E_PARAM, E_PRE, E_RUBY, E_S, E_SCRIPT, E_SEARCH, E_SECTION, E_SELECT, E_SMALL, E_SOURCE, E_SPAN, E_STRIKE,
    E_STRONG, E_STYLE, E_SUB, E_SUMMARY, E_SUP, E_SVG, E_TABLE, E_TBODY, E_TD, E_TEMPLATE, E_TEXTAREA, E_TFOOT,
    E_TH, E_THEAD, E_TITLE, E_TR, E_TRACK, E_TT, E_U, E_UL, E_VAR, E_WBR, E_XMP, E_COUNT
};
static const struct element {
    const char *name;
    unsigned char id;
    unsigned short flags;
} elements[] = {
    { "a", E_A, 0 },
    { "address", E_ADDRESS, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL },
    { "applet", E_APPLET, F_HIDE|F_SCOPE|F_SPECIAL },
    { "area", E_AREA, F_VOID|F_SPECIAL },
    { "article", E_ARTICLE, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL },
    { "aside", E_ASIDE, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL },
    { "b", E_B, F_OUT },
    { "base", E_BASE, F_VOID|F_SPECIAL },
    { "basefont", E_BASEFONT, F_VOID|F_SPECIAL },
    { "bgsound", E_BGSOUND, F_VOID|F_SPECIAL },
    { "big", E_BIG, F_OUT },
    { "blockquote", E_BLOCKQUOTE, F_PARA|F_CLOSEP|F_PUSH|F_SPECIAL|F_OUT },
    { "body", E_BODY, F_SPECIAL|F_OUT },
    { "br", E_BR, F_VOID|F_SPECIAL|F_OUT },
    { "button", E_BUTTON, F_SPECIAL },
    { "caption", E_CAPTION, F_LINE|F_PUSH|F_SCOPE|F_SPECIAL },
    { "center", E_CENTER, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL|F_OUT },
    { "code", E_CODE, F_OUT },
    { "col", E_COL, F_VOID|F_SPECIAL },
    { "colgroup", E_COLGROUP, F_SPECIAL },
    { "datalist", E_DATALIST, F_HIDE },
    { "dd", E_DD, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL|F_OUT },
    { "details", E_DETAILS, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL },
    { "dialog", E_DIALOG, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL },
    { "dir", E_DIR, F_LIST|F_CLOSEP|F_PUSH|F_SPECIAL },
    { "div", E_DIV, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL|F_OUT },
    { "dl", E_DL, F_PARA|F_CLOSEP|F_PUSH|F_SPECIAL|F_OUT },
    { "dt", E_DT, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL|F_OUT },
    { "em", E_EM, F_OUT },
    { "embed", E_EMBED, F_VOID|F_SPECIAL|F_OUT },
    { "fieldset", E_FIELDSET, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL },
    { "figcaption", E_FIGCAPTION, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL },
    { "figure", E_FIGURE, F_PARA|F_CLOSEP|F_PUSH|F_SPECIAL },
    { "font", E_FONT, F_OUT },
    { "footer", E_FOOTER, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL },
    { "form", E_FORM, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL },
    { "frame", E_FRAME, F_VOID|F_SPECIAL },
    { "frameset", E_FRAMESET, F_SPECIAL },
    { "h1", E_H1, F_PARA|F_CLOSEP|F_PUSH|F_HEAD|F_SPECIAL|F_OUT },
    { "h2", E_H2, F_PARA|F_CLOSEP|F_PUSH|F_HEAD|F_SPECIAL|F_OUT },
    { "h3", E_H3, F_PARA|F_CLOSEP|F_PUSH|F_HEAD|F_SPECIAL|F_OUT },
    { "h4", E_H4, F_PARA|F_CLOSEP|F_PUSH|F_HEAD|F_SPECIAL|F_OUT },
    { "h5", E_H5, F_PARA|F_CLOSEP|F_PUSH|F_HEAD|F_SPECIAL|F_OUT },
    { "h6", E_H6, F_PARA|F_CLOSEP|F_PUSH|F_HEAD|F_SPECIAL|F_OUT },
    { "head", E_HEAD, F_SPECIAL|F_OUT },
    { "header", E_HEADER, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL },
    { "hgroup", E_HGROUP, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL },
    { "hr", E_HR, F_VOID|F_CLOSEP|F_SPECIAL|F_OUT },
    { "html", E_HTML, F_SPECIAL },
    { "i", E_I, F_OUT },
    { "iframe", E_IFRAME, F_RAW|F_SPECIAL },
    { "image", E_IMAGE, F_VOID|F_SPECIAL },
    { "img", E_IMG, F_VOID|F_SPECIAL|F_OUT },
    { "input", E_INPUT, F_VOID|F_SPECIAL },
    { "keygen", E_KEYGEN, F_VOID|F_SPECIAL },
    { "legend", E_LEGEND, F_LINE|F_PUSH },
    { "li", E_LI, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL|F_OUT },
    { "link", E_LINK, F_VOID|F_SPECIAL },
    { "listing", E_LISTING, F_PARA|F_CLOSEP|F_PUSH|F_SPECIAL|F_OUT },
    { "main", E_MAIN, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL },
    { "marquee", E_MARQUEE, F_SPECIAL },
    { "math", E_MATH, F_HIDE },
    { "menu", E_MENU, F_LIST|F_CLOSEP|F_PUSH|F_SPECIAL|F_OUT },
    { "meta", E_META, F_VOID|F_SPECIAL|F_OUT },
    { "nav", E_NAV, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL },
    { "nobr", E_NOBR, F_OUT },
    { "noembed", E_NOEMBED, F_RAW|F_SPECIAL },
    { "noframes", E_NOFRAMES, F_LINE|F_PUSH|F_SPECIAL },
    { "noscript", E_NOSCRIPT, F_SPECIAL },
    { "object", E_OBJECT, F_HIDE|F_SCOPE|F_SPECIAL },
    { "ol", E_OL, F_LIST|F_CLOSEP|F_PUSH|F_SPECIAL|F_OUT },
    { "optgroup", E_OPTGROUP, 0 },
    { "option", E_OPTION, 0 },
    { "p", E_P, F_PARA|F_CLOSEP|F_PUSH|F_SPECIAL|F_OUT },
    { "param", E_PARAM, F_VOID|F_SPECIAL },
    { "pre", E_PRE, F_PARA|F_CLOSEP|F_PUSH|F_SPECIAL|F_OUT },
    { "ruby", E_RUBY, F_OUT },
    { "s", E_S, F_OUT },
    { "script", E_SCRIPT, F_RAW|F_SPECIAL },
    { "search", E_SEARCH, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL },
    { "section", E_SECTION, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL },
    { "select", E_SELECT, F_SPECIAL },
    { "small", E_SMALL, F_OUT },
    { "source", E_SOURCE, F_VOID|F_SPECIAL },
    { "span", E_SPAN, F_OUT },
    { "strike", E_STRIKE, F_OUT },
    { "strong", E_STRONG, F_OUT },
    { "style", E_STYLE, F_RAW|F_SPECIAL },
    { "sub", E_SUB, F_OUT },
    { "summary", E_SUMMARY, F_LINE|F_CLOSEP|F_PUSH|F_SPECIAL },
    { "sup", E_SUP, F_OUT },
    { "svg", E_SVG, F_HIDE },
    { "table", E_TABLE, F_LINE|F_CLOSEP|F_PUSH|F_SCOPE|F_SPECIAL|F_OUT },
    { "tbody", E_TBODY, F_SPECIAL },
    { "td", E_TD, F_PUSH|F_SCOPE|F_SPECIAL },
    { "template", E_TEMPLATE, F_HIDE|F_SCOPE|F_SPECIAL },
    { "textarea", E_TEXTAREA, F_RCDATA|F_SPECIAL },
    { "tfoot", E_TFOOT, F_SPECIAL },
    { "th", E_TH, F_PUSH|F_SCOPE|F_SPECIAL },
    { "thead", E_THEAD, F_SPECIAL },
    { "title", E_TITLE, F_RCDATA|F_SPECIAL },
    { "tr", E_TR, F_LINE|F_PUSH|F_SPECIAL },
    { "track", E_TRACK, F_VOID|F_SPECIAL },
    { "tt", E_TT, F_OUT },
    { "u", E_U, F_OUT },
    { "ul", E_UL, F_LIST|F_CLOSEP|F_PUSH|F_SPECIAL|F_OUT },
    { "var", E_VAR, F_OUT },
    { "wbr", E_WBR, F_VOID|F_SPECIAL },
    { "xmp", E_XMP, F_PARA|F_CLOSEP|F_RAW|F_SPECIAL }
};

/**** character references ****/

static const struct entity {
    const char *name;
    unsigned int cp;
    int legacy;                         /* also without its semicolon, as browsers have it */
} entities[] = {
    { "AElig", 0xc6, 1 }, { "AMP", 0x26, 1 }, { "Aacute", 0xc1, 1 }, { "Acirc", 0xc2, 1 }, { "Agrave", 0xc0, 1 },
    { "Aring", 0xc5, 1 }, { "Atilde", 0xc3, 1 }, { "Auml", 0xc4, 1 }, { "COPY", 0xa9, 1 }, { "Ccedil", 0xc7, 1 },
    { "Dagger", 0x2021, 0 }, { "ETH", 0xd0, 1 }, { "Eacute", 0xc9, 1 }, { "Ecirc", 0xca, 1 }, { "Egrave", 0xc8, 1 },
    { "Euml", 0xcb, 1 }, { "GT", 0x3e, 1 }, { "Iacute", 0xcd, 1 }, { "Icirc", 0xce, 1 }, { "Igrave", 0xcc, 1 },
    { "Iuml", 0xcf, 1 }, { "LT", 0x3c, 1 }, { "Ntilde", 0xd1, 1 }, { "Oacute", 0xd3, 1 }, { "Ocirc", 0xd4, 1 },
    { "Ograve", 0xd2, 1 }, { "Oslash", 0xd8, 1 }, { "Otilde", 0xd5, 1 }, { "Ouml", 0xd6, 1 },
    { "Prime", 0x2033, 0 }, { "QUOT", 0x22, 1 }, { "REG", 0xae, 1 }, { "THORN", 0xde, 1 }, { "Uacute", 0xda, 1 },
    { "Ucirc", 0xdb, 1 }, { "Ugrave", 0xd9, 1 }, { "Uuml", 0xdc, 1 }, { "Yacute", 0xdd, 1 }, { "aacute", 0xe1, 1 },
    { "acirc", 0xe2, 1 }, { "acute", 0xb4, 1 }, { "aelig", 0xe6, 1 }, { "agrave", 0xe0, 1 }, { "amp", 0x26, 1 },
    { "apos", 0x27, 0 }, { "aring", 0xe5, 1 }, { "atilde", 0xe3, 1 }, { "auml", 0xe4, 1 }, { "bdquo", 0x201e, 0 },
    { "brvbar", 0xa6, 1 }, { "bull", 0x2022, 0 }, { "ccedil", 0xe7, 1 }, { "cedil", 0xb8, 1 }, { "cent", 0xa2, 1 },
    { "copy", 0xa9, 1 }, { "curren", 0xa4, 1 }, { "dagger", 0x2020, 0 }, { "darr", 0x2193, 0 }, { "deg", 0xb0, 1 },
    { "divide", 0xf7, 1 }, { "eacute", 0xe9, 1 }, { "ecirc", 0xea, 1 }, { "egrave", 0xe8, 1 },
    { "emsp", 0x2003, 0 }, { "ensp", 0x2002, 0 }, { "eth", 0xf0, 1 }, { "euml", 0xeb, 1 }, { "euro", 0x20ac, 0 },
    { "frac12", 0xbd, 1 }, { "frac14", 0xbc, 1 }, { "frac34", 0xbe, 1 }, { "ge", 0x2265, 0 }, { "gt", 0x3e, 1 },
    { "harr", 0x2194, 0 }, { "hellip", 0x2026, 0 }, { "iacute", 0xed, 1 }, { "icirc", 0xee, 1 },
    { "iexcl", 0xa1, 1 }, { "igrave", 0xec, 1 }, { "iquest", 0xbf, 1 }, { "iuml", 0xef, 1 }, { "laquo", 0xab, 1 },
    { "larr", 0x2190, 0 }, { "ldquo", 0x201c, 0 }, { "le", 0x2264, 0 }, { "lrm", 0x200e, 0 },
    { "lsaquo", 0x2039, 0 }, { "lsquo", 0x2018, 0 }, { "lt", 0x3c, 1 }, { "macr", 0xaf, 1 }, { "mdash", 0x2014, 0 },
    { "micro", 0xb5, 1 }, { "middot", 0xb7, 1 }, { "minus", 0x2212, 0 }, { "nbsp", 0xa0, 1 },
    { "ndash", 0x2013, 0 }, { "ne", 0x2260, 0 }, { "not", 0xac, 1 }, { "ntilde", 0xf1, 1 }, { "oacute", 0xf3, 1 },
    { "ocirc", 0xf4, 1 }, { "ograve", 0xf2, 1 }, { "ordf", 0xaa, 1 }, { "ordm", 0xba, 1 }, { "oslash", 0xf8, 1 },
    { "otilde", 0xf5, 1 }, { "ouml", 0xf6, 1 }, { "para", 0xb6, 1 }, { "permil", 0x2030, 0 }, { "plusmn", 0xb1, 1 },
    { "pound", 0xa3, 1 }, { "prime", 0x2032, 0 }, { "quot", 0x22, 1 }, { "raquo", 0xbb, 1 }, { "rarr", 0x2192, 0 },
    { "rdquo", 0x201d, 0 }, { "reg", 0xae, 1 }, { "rlm", 0x200f, 0 }, { "rsaquo", 0x203a, 0 },
    { "rsquo", 0x2019, 0 }, { "sbquo", 0x201a, 0 }, { "sect", 0xa7, 1 }, { "shy", 0xad, 1 }, { "sup1", 0xb9, 1 },
    { "sup2", 0xb2, 1 }, { "sup3", 0xb3, 1 }, { "szlig", 0xdf, 1 }, { "thinsp", 0x2009, 0 }, { "thorn", 0xfe, 1 },
    { "times", 0xd7, 1 }, { "trade", 0x2122, 0 }, { "uacute", 0xfa, 1 }, { "uarr", 0x2191, 0 },
    { "ucirc", 0xfb, 1 }, { "ugrave", 0xf9, 1 }, { "uml", 0xa8, 1 }, { "uuml", 0xfc, 1 }, { "yacute", 0xfd, 1 },
    { "yen", 0xa5, 1 }, { "yuml", 0xff, 1 }, { "zwj", 0x200d, 0 }, { "zwnj", 0x200c, 0 }
};

/* windows-1252's 0x80-0x9F; its five holes stay C1 controls, which are never shown */
static const unsigned short w1252[32] = {
    0x20ac, 0x0081, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008d,
    0x017d, 0x008f, 0x0090, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a,
    0x0153, 0x009d, 0x017e, 0x0178
};

/* The WHATWG labels of the two charsets Undertow reads (and of UTF-16, which a <meta> may name) */
static const char *const utf8_labels[] = {
    "unicode-1-1-utf-8", "unicode11utf8", "unicode20utf8", "utf-8", "utf8", "x-unicode20utf8"
};
static const char *const w1252_labels[] = {
    "ansi_x3.4-1968", "ascii", "cp1252", "cp819", "csisolatin1", "ibm819", "iso-8859-1", "iso-ir-100", "iso8859-1",
    "iso88591", "iso_8859-1", "iso_8859-1:1987", "l1", "latin1", "us-ascii", "windows-1252", "x-cp1252"
};
static const char *const utf16_labels[] = {
    "csunicode", "iso-10646-ucs-2", "ucs-2", "unicode", "unicodefeff", "unicodefffe", "utf-16", "utf-16be", "utf-16le"
};

/**** the renderer's state ****/

struct frame {                          /* an open element that shapes the text */
    unsigned char el;
    unsigned char nested;               /* a list inside a list: no blank lines around it */
    unsigned char cell_pending;         /* td, th: it has shown nothing yet */
    unsigned char row_shown;            /* tr: one of its cells has shown something */
    unsigned char data;                 /* table: it has had a <th>, so every cell keeps its column */
    unsigned short text;                /* the indentation of the lines of its text */
    unsigned short mcol;                /* lists and items: the column of the markers */
    short lst;                          /* the nearest list or item around it (no quote between), or -1 */
    long next;                          /* ol: the number of its next item */
};

enum { A_HREF, A_ID, A_NAME, A_ALT, A_VALUE, A_TYPE, A_START, A_ARIA, A_TITLE, A_COUNT };

static const struct { const char *name; size_t cap; } attr_info[A_COUNT] = {
    { "href", HREF_MAX }, { "id", UT_FIELD_MAX + 1 }, { "name", UT_FIELD_MAX + 1 }, { "alt", UT_FIELD_MAX },
    { "value", UT_FIELD_MAX }, { "type", 32 }, { "start", 32 }, { "aria-label", UT_FIELD_MAX }, { "title", UT_FIELD_MAX }
};

struct tag {                            /* the tag being read */
    char name[16];                      /* lowercase */
    int el, end, selfclose;
    size_t len[A_COUNT];                /* the bytes kept of each attribute */
    unsigned char seen[A_COUNT], over[A_COUNT];
    char *val[A_COUNT];                 /* NUL-terminated UTF-8, in the buffers below */
    char href[HREF_MAX + 1], id[UT_FIELD_MAX + 2], name_[UT_FIELD_MAX + 2], alt[UT_FIELD_MAX + 1],
         value[UT_FIELD_MAX + 1], type[33], start[33], aria[UT_FIELD_MAX + 1], title[UT_FIELD_MAX + 1];
};

struct rd {
    const struct ut_response *r;
    const unsigned char *in;
    size_t len, pos;
    int cs;                             /* CS_UTF8, CS_1252, CS_ASCII (a charset Undertow cannot read) */
    int xhtml;                          /* <tag /> is a whole element */
    char charset[LABEL_MAX];            /* the name of a charset Undertow cannot read */

    /* the text */
    char *text;
    size_t tlen, tcap, limit;
    size_t line_start;                  /* where the last line starts */
    unsigned int col;                   /* characters on it after the indentation */
    int breaks;                         /* line breaks wanted before the next text: 1 a new line, 2 a blank line too */
    int space;                          /* a collapsible space is wanted before the next text */
    int stopped;                        /* the text is full: nothing more is shown */
    int failed;                         /* out of memory */
    int truncated;                      /* UT_TRUNC_* */
    int pre;                            /* inside pre, listing or xmp (or plain text) */
    int headings;                       /* open h1-h6 */
    int tables;                         /* open tables */
    int cells_pending;                  /* open cells that have shown nothing yet: no line breaks for them */
    int sep;                            /* the next text continues a row: " | " first */
    char marker[16];                    /* a list item's marker, waiting for its first text */
    int marker_on;
    unsigned int marker_col;
    int marker_frame;

    /* open elements */
    struct frame st[STACK_MAX];         /* st[0] is the root */
    int depth;
    unsigned short open[E_COUNT];       /* how many of each kind are on the stack */
    unsigned int over[E_COUNT];         /* and how many more were too deep to be kept */
    unsigned int hide[E_COUNT];         /* open elements whose content is not shown */
    int skip;                           /* their sum */

    /* links, buttons, selects */
    int link_open, link_num, link_text; /* link_num 0: a link without a number */
    char label[UT_FIELD_MAX + 1];
    size_t label_len;
    char link_alt[UT_FIELD_MAX + 1];    /* its text if it shows none: aria-label or title, its own or inside it */
    size_t link_alt_len;
    int label_gap, label_full;
    int button;                         /* 1: open, nothing shown yet; 2: "[button: " shown */
    int select, option, select_shown;   /* option 1: in the first one; 2: past it */
    int forms;

    /* what the document gets */
    struct ut_link *links;
    char **hrefs;                       /* each link's address as written, until the end */
    unsigned char long_href[UT_LINKS_MAX];  /* it was longer than HREF_MAX: unusable */
    size_t nlinks, links_cap;
    struct ut_anchor *anchors;
    size_t nanchors, anchors_cap, aplaced;  /* anchors[aplaced..] wait for the next text */
    unsigned short ahash[AHASH];        /* index + 1 into anchors, by name */
    struct ut_span *spans;
    size_t nspans, spans_cap, heading_spans;
    int span_on;
    size_t span_start;
    char title[UT_FIELD_MAX + 1];
    size_t title_len;
    int title_seen;
    struct ut_url *base;                /* the first <base href> that resolved */
    unsigned char *decoded;             /* a UTF-16 body as UTF-8 */

    int given;                          /* the document has taken the text, links, anchors and spans */

    struct tag tag;
    char scratch[UT_FIELD_MAX + 16];
};

/**** small helpers ****/

static int ws(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; }
static int alpha(unsigned char c) { return (c | 0x20) >= 'a' && (c | 0x20) <= 'z'; }
static int digit(unsigned char c) { return c >= '0' && c <= '9'; }
static int alnum(unsigned char c) { return alpha(c) || digit(c); }
static unsigned char lower(unsigned char c) { return c >= 'A' && c <= 'Z' ? (unsigned char)(c + 32) : c; }

static int hexval(unsigned char c) {
    if(digit(c)) return c - '0';
    c |= 0x20;
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* s (n bytes) is the lowercase word w in any case */
static int ci_eq(const unsigned char *s, size_t n, const char *w) {
    size_t i;
    for(i = 0; i < n; i++) if(!w[i] || lower(s[i]) != (unsigned char)w[i]) return 0;
    return !w[n];
}

/* s (avail bytes) starts with the lowercase word w in any case */
static int ci_prefix(const unsigned char *s, size_t avail, const char *w) {
    size_t i;
    for(i = 0; w[i]; i++) if(i >= avail || lower(s[i]) != (unsigned char)w[i]) return 0;
    return 1;
}

/* Never shown: ut_unsafe_char()'s (NUL and every control, the bidirectional marks and controls) and the
   characters that are invisible on a console: the soft hyphen (&shy;), the combining grapheme joiner, the
   Mongolian vowel separator, the zero-width space, the word joiner and the invisible operators, and the
   variation selectors. The zero-width non-joiner and joiner stay: scripts and emoji need them. */
static int hidden_char(unsigned int cp) {
    return ut_unsafe_char(cp) || cp == 0xad || cp == 0x34f || cp == 0x180e || cp == 0x200b ||
           (cp >= 0x2060 && cp <= 0x2064) || (cp >= 0xfe00 && cp <= 0xfe0f);
}

/* HTML's whitespace, and the line and paragraph separators, which ut_safe_text() makes spaces too */
static int space_char(unsigned int cp) {
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\f' || cp == '\r' || cp == 0x85 || cp == 0x2028 || cp == 0x2029;
}

/* The bytes of s (n of them) that are characters' first bytes: its width on the console */
static unsigned int width(const char *s, size_t n) {
    unsigned int w = 0;
    size_t i;
    for(i = 0; i < n; i++) if(((unsigned char)s[i] & 0xc0) != 0x80) w++;
    return w;
}

/**** the page's characters ****/

/* The character at in[*pos] (before end) in the page's charset; *pos moves past it. CR LF and CR are LF,
   as the HTML standard has it before anything else looks at the text. */
static unsigned int next_char(const struct rd *rd, size_t *pos, size_t end) {
    const unsigned char *s = rd->in + *pos;
    unsigned int cp;
    if(*s < 0x80) {
        *pos += 1;
        if(*s != '\r') return *s;
        if(*pos < end && rd->in[*pos] == '\n') *pos += 1;
        return '\n';
    }
    if(rd->cs == CS_UTF8) {
        *pos += ut_utf8_decode(s, end - *pos, &cp);
        return cp;
    }
    *pos += 1;
    if(rd->cs == CS_1252) return *s < 0xa0 ? w1252[*s - 0x80] : *s;
    return '?';
}

static const struct entity *find_entity(const unsigned char *s, size_t n) {
    size_t lo = 0, hi = sizeof(entities) / sizeof(entities[0]);
    while(lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strncmp(entities[mid].name, (const char *)s, n);
        if(!c && entities[mid].name[n]) c = 1;          /* the table's name goes on: it is the greater */
        if(!c) return &entities[mid];
        if(c < 0) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

/* A numeric reference's character, as browsers make it */
static unsigned int numeric(unsigned int v) {
    if(!v || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff)) return UT_REPLACEMENT;
    if(v >= 0x80 && v <= 0x9f) return w1252[v - 0x80];
    return v;
}

/* A character reference at in[*pos], which is '&' (before end): its character, with *pos past it; or '&'
   alone, with *pos past the '&' only, when it is none: unknown names stay as written. A name without its
   semicolon counts only for the legacy names (Latin-1, amp, lt, gt, quot), and in an attribute only when
   neither '=' nor a letter or digit follows it, as in browsers. Bounded: a name has 32 letters at most. */
static unsigned int char_ref(const struct rd *rd, size_t *pos, size_t end, int attr) {
    const unsigned char *s = rd->in;
    const struct entity *e;
    size_t p = *pos + 1, start, k;
    unsigned int v = 0;
    int hex = 0, d;
    if(p < end && s[p] == '#') {
        p++;
        if(p < end && (s[p] == 'x' || s[p] == 'X')) {
            hex = 1;
            p++;
        }
        start = p;
        while(p < end && (d = hex ? hexval(s[p]) : digit(s[p]) ? s[p] - '0' : -1) >= 0) {
            if(v <= 0x10ffff) v = v * (hex ? 16 : 10) + (unsigned int)d;        /* past that it is U+FFFD anyway */
            p++;
        }
        if(p == start) {
            *pos += 1;
            return '&';
        }
        if(p < end && s[p] == ';') p++;
        *pos = p;
        return numeric(v);
    }
    start = p;
    while(p < end && p - start < 32 && alnum(s[p])) p++;
    if(p > start && p < end && s[p] == ';') {
        e = find_entity(s + start, p - start);
        if(e) {
            *pos = p + 1;
            return e->cp;
        }
    } else if(p > start) {
        for(k = p - start < 6 ? p - start : 6; k >= 2; k--) {
            e = find_entity(s + start, k);
            if(!e || !e->legacy) continue;
            if(attr && start + k < end && (s[start + k] == '=' || alnum(s[start + k]))) break;
            *pos = start + k;
            return e->cp;
        }
    }
    *pos += 1;
    return '&';
}

/**** which charset ****/

/* The charset a label names (any case, whitespace around it): CS_UTF8, CS_1252, CS_UTF16, or -1 */
static int charset_of(const char *label) {
    const unsigned char *s = (const unsigned char *)label;
    size_t n = strlen(label), i;
    while(n && ws(s[n - 1])) n--;
    while(n && ws(*s)) {
        s++;
        n--;
    }
    for(i = 0; i < sizeof(utf8_labels) / sizeof(utf8_labels[0]); i++) if(ci_eq(s, n, utf8_labels[i])) return CS_UTF8;
    for(i = 0; i < sizeof(w1252_labels) / sizeof(w1252_labels[0]); i++) if(ci_eq(s, n, w1252_labels[i])) return CS_1252;
    for(i = 0; i < sizeof(utf16_labels) / sizeof(utf16_labels[0]); i++) if(ci_eq(s, n, utf16_labels[i])) return CS_UTF16;
    return -1;
}

/* A charset Undertow cannot read: only ASCII is shown, and the notice names it (its printable ASCII) */
static void unknown_charset(struct rd *rd, const char *label) {
    size_t i, k = 0;
    rd->cs = CS_ASCII;
    for(i = 0; label[i] && k + 1 < sizeof(rd->charset); i++)
        if((unsigned char)label[i] > ' ' && (unsigned char)label[i] < 0x7f) rd->charset[k++] = label[i];
    rd->charset[k] = 0;
    if(!k) snprintf(rd->charset, sizeof(rd->charset), "%s", "a charset it does not name");
}

/* One attribute of a tag, as the standard's prescan reads it: 1 with its name (lowercase) and value (cut to
   their buffers), or 0 at the end of the tag. *i moves past it. */
static int pre_attr(const unsigned char *s, size_t n, size_t *i, char *name, size_t ncap, char *val, size_t vcap) {
    size_t p = *i, k = 0, v = 0;
    unsigned char q;
    while(p < n && (ws(s[p]) || s[p] == '/')) p++;
    if(p >= n || s[p] == '>') {
        *i = p;
        return 0;
    }
    do {
        if(k + 1 < ncap) name[k++] = (char)lower(s[p]);
        p++;
    } while(p < n && !ws(s[p]) && s[p] != '/' && s[p] != '>' && s[p] != '=');
    name[k] = 0;
    while(p < n && ws(s[p])) p++;
    if(p < n && s[p] == '=') {
        p++;
        while(p < n && ws(s[p])) p++;
        if(p < n && (s[p] == '"' || s[p] == '\'')) {
            q = s[p++];
            while(p < n && s[p] != q) {
                if(v + 1 < vcap) val[v++] = (char)s[p];
                p++;
            }
            if(p < n) p++;
        } else {
            while(p < n && !ws(s[p]) && s[p] != '>') {
                if(v + 1 < vcap) val[v++] = (char)s[p];
                p++;
            }
        }
    }
    val[v] = 0;
    *i = p;
    return 1;
}

static void copy_cut(char *dst, size_t cap, const char *src) {    /* src, cut to cap - 1 bytes */
    size_t n = strlen(src);
    if(n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* The charset in a Content-Type value ("text/html; charset=windows-1252"), as the standard extracts it */
static int charset_in_content(const char *content, char *label, size_t cap) {
    const unsigned char *s = (const unsigned char *)content;
    size_t n = strlen(content), i = 0, k = 0;
    unsigned char q;
    while(i + 7 <= n) {
        if(!ci_prefix(s + i, n - i, "charset")) {
            i++;
            continue;
        }
        i += 7;
        while(i < n && ws(s[i])) i++;
        if(i >= n || s[i] != '=') continue;
        i++;
        while(i < n && ws(s[i])) i++;
        if(i < n && (s[i] == '"' || s[i] == '\'')) {
            q = s[i++];
            if(!memchr(s + i, q, n - i)) return 0;
            while(s[i] != q) {
                if(k + 1 < cap) label[k++] = (char)s[i];
                i++;
            }
        } else
            while(i < n && !ws(s[i]) && s[i] != ';') {
                if(k + 1 < cap) label[k++] = (char)s[i];
                i++;
            }
        label[k] = 0;
        return k > 0;
    }
    return 0;
}

/* <meta>'s attributes (*i just past "<meta"): 1 and the charset it gives */
static int meta_charset(const unsigned char *s, size_t n, size_t *i, char *label, size_t cap) {
    char name[16], val[256], content[256];
    int pragma = 0;
    label[0] = content[0] = 0;
    while(pre_attr(s, n, i, name, sizeof(name), val, sizeof(val))) {
        if(!strcmp(name, "http-equiv")) pragma = ci_eq((const unsigned char *)val, strlen(val), "content-type");
        else if(!strcmp(name, "content") && !content[0]) copy_cut(content, sizeof(content), val);
        else if(!strcmp(name, "charset") && !label[0]) copy_cut(label, cap, val);
    }
    if(label[0]) return 1;
    return pragma && charset_in_content(content, label, cap);
}

/* The first <meta charset> or <meta http-equiv=Content-Type> in the first PRESCAN bytes, outside comments */
static int prescan(const unsigned char *s, size_t n, char *label, size_t cap) {
    const unsigned char *j;
    char name[16], val[256];
    size_t i = 0, k;
    if(n > PRESCAN) n = PRESCAN;
    while(i < n && (j = memchr(s + i, '<', n - i)) != NULL) {
        i = (size_t)(j - s);
        if(n - i >= 4 && !memcmp(s + i, "<!--", 4)) {
            for(k = i + 2; k + 2 < n && !(s[k] == '-' && s[k + 1] == '-' && s[k + 2] == '>'); k++) ;
            i = k + 3;
        } else if(n - i >= 6 && ci_prefix(s + i + 1, n - i - 1, "meta") && (ws(s[i + 5]) || s[i + 5] == '/')) {
            i += 5;
            if(meta_charset(s, n, &i, label, cap)) return 1;
        } else if(i + 2 < n && (alpha(s[i + 1]) || (s[i + 1] == '/' && alpha(s[i + 2])))) {
            i += 2;
            while(i < n && !ws(s[i]) && s[i] != '>') i++;
            while(pre_attr(s, n, &i, name, sizeof(name), val, sizeof(val))) ;
        } else if(i + 1 < n && (s[i + 1] == '!' || s[i + 1] == '/' || s[i + 1] == '?')) {
            j = memchr(s + i, '>', n - i);
            i = j ? (size_t)(j - s) + 1 : n;
        } else i++;
    }
    return 0;
}

/* Well-formed UTF-8; a character cut short by the end of the body is forgiven when a whole one came
   before it (a single Latin-1 letter at the end is not taken for the start of one) */
static int utf8_valid(const unsigned char *s, size_t n) {
    size_t i = 0, k, need;
    unsigned int cp;
    int whole = 0;
    while(i < n) {
        if(s[i] < 0x80) {
            i++;
            continue;
        }
        k = ut_utf8_decode(s + i, n - i, &cp);
        if(k > 1) {
            i += k;
            whole = 1;
            continue;
        }
        need = s[i] >= 0xc2 && s[i] <= 0xdf ? 2 : s[i] >= 0xe0 && s[i] <= 0xef ? 3 : s[i] >= 0xf0 && s[i] <= 0xf4 ? 4 : 0;
        if(!need || n - i >= need || !whole) return 0;
        for(k = 1; i + k < n; k++) if((s[i + k] & 0xc0) != 0x80) return 0;
        return 1;
    }
    return 1;
}

/* A UTF-16 body (a byte order mark FF FE or FE FF, else a charset that names UTF-16): decoded into UTF-8, which
   the renderer then reads (surrogate pairs joined; a lone surrogate or an odd last byte is U+FFFD). 0, or -1
   when memory is out. */
static int decode_utf16(struct rd *rd) {
    const unsigned char *s = rd->in;
    unsigned char *out;
    size_t n = rd->len, i, o = 0;
    unsigned int u, v, cp;
    int be;
    const char *label = rd->r->charset;
    if(n >= 2 && ((s[0] == 0xff && s[1] == 0xfe) || (s[0] == 0xfe && s[1] == 0xff))) {
        be = s[0] == 0xfe;
        s += 2;
        n -= 2;
    } else if(!(n >= 3 && s[0] == 0xef && s[1] == 0xbb && s[2] == 0xbf) && charset_of(label) == CS_UTF16)
        be = ci_eq((const unsigned char *)label, strlen(label), "utf-16be") ||
             ci_eq((const unsigned char *)label, strlen(label), "unicodefffe");
    else return 0;
    out = malloc(n / 2 * 3 + 4);
    if(!out) return -1;
    for(i = 0; i + 1 < n; i += 2) {
        u = be ? (unsigned int)s[i] << 8 | s[i + 1] : (unsigned int)s[i + 1] << 8 | s[i];
        cp = u;
        if(u >= 0xd800 && u <= 0xdbff && i + 3 < n) {
            v = be ? (unsigned int)s[i + 2] << 8 | s[i + 3] : (unsigned int)s[i + 3] << 8 | s[i + 2];
            if(v >= 0xdc00 && v <= 0xdfff) {
                cp = 0x10000 + ((u - 0xd800) << 10) + (v - 0xdc00);
                i += 2;
            }
        }
        o += ut_utf8_encode(cp, out + o);           /* a lone surrogate is written as U+FFFD */
    }
    if(i < n) o += ut_utf8_encode(UT_REPLACEMENT, out + o);
    out[o] = 0;
    rd->decoded = out;
    rd->in = out;
    rd->len = o;
    rd->cs = CS_UTF8;
    return 0;
}

/* html.h's order: a byte order mark, the response's charset, a <meta> in the first 1024 bytes (HTML only),
   then UTF-8 if the bytes are UTF-8 and windows-1252 if not. */
static void choose_charset(struct rd *rd, int html) {
    const unsigned char *s = rd->in;
    char label[LABEL_MAX];
    int cs;
    if(rd->len >= 3 && s[0] == 0xef && s[1] == 0xbb && s[2] == 0xbf) {
        rd->cs = CS_UTF8;
        rd->pos = 3;
        return;
    }
    if(rd->r->charset[0]) {
        cs = charset_of(rd->r->charset);
        if(cs == CS_UTF8 || cs == CS_1252) rd->cs = cs;
        else unknown_charset(rd, rd->r->charset);
        return;
    }
    if(html && prescan(s, rd->len, label, sizeof(label))) {
        cs = charset_of(label);
        if(cs == CS_UTF8 || cs == CS_UTF16) rd->cs = CS_UTF8;          /* a page that says UTF-16 in ASCII is not */
        else if(cs == CS_1252) rd->cs = CS_1252;
        else unknown_charset(rd, label);
        return;
    }
    rd->cs = utf8_valid(s, rd->len) ? CS_UTF8 : CS_1252;
}

/* No Content-Type: a body is HTML when it starts like HTML (the MIME sniffing standard's list) */
static int looks_like_html(const unsigned char *s, size_t n) {
    static const char *const starts[] = {
        "!doctype html", "html", "head", "script", "iframe", "h1", "div", "font", "table", "a", "style", "title",
        "b", "body", "br", "p", "!--"
    };
    size_t i = 0, k, m;
    if(n >= 3 && s[0] == 0xef && s[1] == 0xbb && s[2] == 0xbf) i = 3;
    while(i < n && ws(s[i])) i++;
    if(i >= n || s[i] != '<') return 0;
    i++;
    for(k = 0; k < sizeof(starts) / sizeof(starts[0]); k++) {
        m = strlen(starts[k]);
        if(ci_prefix(s + i, n - i, starts[k]) && i + m < n && (ws(s[i + m]) || s[i + m] == '>')) return 1;
    }
    return 0;
}

/**** the text ****/

#define FL(el) ((el) ? elements[(el) - 1].flags : 0)   /* the enum follows the table's order */

static void link_close(struct rd *rd);

/* Room for n more bytes of text: 0, or -1 when the text is full (the page is cut there) or memory is out */
static int room(struct rd *rd, size_t n) {
    size_t cap;
    char *t;
    if(rd->stopped || rd->failed) return -1;
    if(rd->tlen + n > rd->limit) {
        rd->stopped = 1;
        rd->truncated |= UT_TRUNC_TEXT;
        return -1;
    }
    if(rd->tlen + n < rd->tcap) return 0;
    cap = rd->tcap ? rd->tcap * 2 : 4096;
    while(cap <= rd->tlen + n) cap *= 2;
    if(cap > UT_TEXT_MAX + 1) cap = UT_TEXT_MAX + 1;
    t = realloc(rd->text, cap);
    if(!t) {
        rd->failed = 1;
        return -1;
    }
    rd->text = t;
    rd->tcap = cap;
    return 0;
}

static int put(struct rd *rd, const char *s, size_t n) {
    if(room(rd, n)) return -1;
    memcpy(rd->text + rd->tlen, s, n);
    rd->tlen += n;
    return 0;
}

static int pad(struct rd *rd, unsigned int n) {
    if(room(rd, n)) return -1;
    memset(rd->text + rd->tlen, ' ', n);
    rd->tlen += n;
    return 0;
}

static void add_span(struct rd *rd, size_t offset, size_t len, int kind) {
    struct ut_span *s;
    size_t cap;
    if(kind == UT_SPAN_HEADING && rd->heading_spans >= HEADING_SPANS) return;
    if(rd->nspans == rd->spans_cap) {
        cap = rd->spans_cap ? rd->spans_cap * 2 : 16;
        s = realloc(rd->spans, cap * sizeof(*s));
        if(!s) {
            rd->failed = 1;
            return;
        }
        rd->spans = s;
        rd->spans_cap = cap;
    }
    if(kind == UT_SPAN_HEADING) rd->heading_spans++;
    s = &rd->spans[rd->nspans++];
    s->offset = offset;
    s->len = len;
    s->kind = kind;
}

/* A heading's span ends here (spans never hold a line break, a link's [N] or spaces at their ends) */
static void close_span(struct rd *rd) {
    size_t end = rd->tlen;
    if(!rd->span_on) return;
    rd->span_on = 0;
    while(end > rd->span_start && rd->text[end - 1] == ' ') end--;
    if(end > rd->span_start) add_span(rd, rd->span_start, end - rd->span_start, UT_SPAN_HEADING);
}

/* Anchors waiting for their element's text get this offset */
static void place_anchors(struct rd *rd, size_t offset) {
    while(rd->aplaced < rd->nanchors) rd->anchors[rd->aplaced++].offset = offset;
}

/* The line's trailing spaces go; anchors that pointed at them wait for the next text again */
static void trim(struct rd *rd) {
    size_t old = rd->tlen;
    while(rd->tlen > rd->line_start && rd->text[rd->tlen - 1] == ' ') rd->tlen--;
    if(rd->tlen == old) return;
    while(rd->aplaced > 0 && rd->anchors[rd->aplaced - 1].offset >= rd->tlen) rd->aplaced--;
}

/* The line breaks wanted, before the next text: the line ends, and a blank line follows if one was
   wanted; never two blank lines, and none at the start */
static int line_breaks(struct rd *rd) {
    int n = rd->breaks;
    rd->breaks = 0;
    rd->space = 0;
    close_span(rd);
    trim(rd);
    rd->col = 0;
    if(!rd->tlen) return 0;
    if(rd->tlen > rd->line_start) {
        if(put(rd, "\n", 1)) return -1;
        rd->line_start = rd->tlen;
        n--;
    }
    if(n > 0 && rd->text[rd->line_start - 2] != '\n') {
        if(put(rd, "\n", 1)) return -1;
        rd->line_start = rd->tlen;
    }
    return 0;
}

static void want(struct rd *rd, int n) {            /* 1: a new line, 2: a blank line too */
    if(!rd->cells_pending && rd->breaks < n) rd->breaks = n;
}

static void newline(struct rd *rd) {                /* <br>, a newline in pre */
    if(!rd->cells_pending && rd->breaks < 2) rd->breaks++;
}

/* The open cells that had shown nothing have now, and so have their rows */
static void cells_shown(struct rd *rd) {
    int i, row = 0;
    for(i = rd->depth - 1; i > 0 && (rd->cells_pending || row); i--) {
        struct frame *f = &rd->st[i];
        if((f->el == E_TD || f->el == E_TH) && f->cell_pending) {
            f->cell_pending = 0;
            rd->cells_pending--;
            row = 1;
        } else if(f->el == E_TR && row) {
            f->row_shown = 1;
            row = 0;
        }
    }
}

/* What comes before any text: the line breaks wanted; at the start of a line its indentation and a list
   item's marker, else the space wanted or a row's " | ". Anchors waiting for text get its place. Returns
   1 when something stands between this text and the text before (for a link's label), 0 when nothing
   does, -1 when the text is full. */
static int begin(struct rd *rd) {
    int gap = 0;
    size_t n;
    if(rd->stopped || rd->failed) return -1;
    if(rd->breaks) {
        if(line_breaks(rd)) return -1;
        gap = rd->tlen > 0;
    }
    if(rd->tlen == rd->line_start) {
        place_anchors(rd, rd->tlen);
        if(pad(rd, rd->marker_on ? rd->marker_col : rd->st[rd->depth - 1].text)) return -1;
    } else if(rd->sep) {
        if(put(rd, " | ", 3)) return -1;
        rd->col += 3;
        gap = 1;
        place_anchors(rd, rd->tlen);
    } else {
        if(rd->space) {
            if(put(rd, " ", 1)) return -1;
            rd->col++;
            gap = 1;
        }
        place_anchors(rd, rd->tlen);
    }
    rd->space = 0;
    rd->sep = 0;
    if(rd->marker_on) {
        n = strlen(rd->marker);
        if(put(rd, rd->marker, n)) return -1;
        rd->col += (unsigned int)n;
        rd->marker_on = 0;
    }
    if(rd->cells_pending) cells_shown(rd);
    return gap;
}

/* A link's label: its text on one line, spaces collapsed, cut between characters at UT_FIELD_MAX */
static void label_add(struct rd *rd, const char *s, size_t n, int gap) {
    if(!rd->link_num || rd->label_full) return;
    if(n == 1 && *s == ' ') {
        rd->label_gap = 1;
        return;
    }
    if((gap || rd->label_gap) && rd->label_len) {
        if(rd->label_len + 1 + n > UT_FIELD_MAX) {
            rd->label_full = 1;
            return;
        }
        rd->label[rd->label_len++] = ' ';
    }
    rd->label_gap = 0;
    if(rd->label_len + n > UT_FIELD_MAX) {
        rd->label_full = 1;
        return;
    }
    memcpy(rd->label + rd->label_len, s, n);
    rd->label_len += n;
}

/* Text: s is n bytes of safe UTF-8 (one character, or a placeholder); " " is a space that does not collapse */
static void emit(struct rd *rd, const char *s, size_t n) {
    int gap;
    if(rd->button == 1) {                   /* a button's first text */
        rd->button = 2;
        emit(rd, "[button: ", 9);
    }
    if(rd->select && rd->option == 1 && !rd->select_shown) {
        rd->select_shown = 1;
        emit(rd, "[select: ", 9);
    }
    gap = begin(rd);
    if(gap < 0) return;
    if(rd->headings && !rd->span_on && *s != ' ') {
        rd->span_on = 1;
        rd->span_start = rd->tlen;
    }
    if(put(rd, s, n)) return;
    rd->col += width(s, n);
    if(rd->link_open) {
        rd->link_text = 1;
        label_add(rd, s, n, gap);
    }
}

static void atom(struct rd *rd, const char *s) {
    emit(rd, s, strlen(s));
}

/* s right after the text before it, with the space or line break that waits still waiting: the "]"
   that ends a placeholder */
static void closer(struct rd *rd, const char *s) {
    size_t n = strlen(s);
    if(rd->stopped || rd->failed || put(rd, s, n)) return;
    rd->col += (unsigned int)n;
    if(rd->link_open) {
        rd->link_text = 1;
        rd->label_gap = 0;
        label_add(rd, s, n, 0);
    }
}

/* Words already collapsed and safe (an alt text, a button's value): their spaces collapse with the text's */
static void words(struct rd *rd, const char *s, size_t n) {
    size_t i = 0, k;
    unsigned int cp;
    while(i < n) {
        if(s[i] == ' ') {
            rd->space = 1;
            i++;
            continue;
        }
        k = ut_utf8_decode((const unsigned char *)s + i, n - i, &cp);
        emit(rd, s + i, k);
        i += k;
    }
}

/* A tab in pre or plain text: spaces to the next 8-column stop */
static void tab(struct rd *rd) {
    emit(rd, " ", 1);
    while(rd->col % 8 && !rd->stopped && !rd->failed) emit(rd, " ", 1);
}

/* One character of the page's text */
static void text_char(struct rd *rd, unsigned int cp) {
    unsigned char b[4];
    if(rd->skip || (rd->select && rd->option != 1) || rd->stopped || rd->failed) return;
    if(rd->pre) {
        if(cp == '\n') {
            newline(rd);
            return;
        }
        if(cp == '\t') {
            tab(rd);
            return;
        }
        if(space_char(cp) || cp == 0xa0) {
            emit(rd, " ", 1);
            return;
        }
    } else {
        if(space_char(cp)) {
            rd->space = 1;
            return;
        }
        if(cp == 0xa0) {                    /* a no-break space is a space that does not collapse, */
            if(rd->cells_pending) rd->space = 1;    /* except where a cell starts: <td>&nbsp;</td> is empty */
            else emit(rd, " ", 1);
            return;
        }
    }
    if(hidden_char(cp)) return;
    emit(rd, (const char *)b, ut_utf8_encode(cp, b));
}

/* s (n bytes of UTF-8 from an attribute) into rd->scratch: unsafe characters dropped, whitespace collapsed,
   none at the ends. Returns its length. */
static size_t collapse(struct rd *rd, const char *s, size_t n) {
    size_t i = 0, k, len = 0;
    unsigned char b[4];
    unsigned int cp;
    int gap = 0;
    while(i < n) {
        k = ut_utf8_decode((const unsigned char *)s + i, n - i, &cp);
        i += k;
        if(space_char(cp) || cp == 0xa0) {
            gap = 1;
            continue;
        }
        if(hidden_char(cp)) continue;
        k = ut_utf8_encode(cp, b);
        if(len + (gap && len) + k > UT_FIELD_MAX) break;
        if(gap && len) rd->scratch[len++] = ' ';
        gap = 0;
        memcpy(rd->scratch + len, b, k);
        len += k;
    }
    rd->scratch[len] = 0;
    return len;
}

/**** open elements ****/

static int is_list(int el) { return el == E_UL || el == E_OL || el == E_MENU || el == E_DIR; }

static unsigned short capped(unsigned int n, unsigned int max) { return (unsigned short)(n > max ? max : n); }

/* Open el on the stack with its parent's indentation; NULL when the stack is full (it is only counted) */
static struct frame *push(struct rd *rd, int el) {
    struct frame *p = &rd->st[rd->depth - 1], *f;
    if(rd->depth == STACK_MAX) {
        rd->over[el]++;
        return NULL;
    }
    f = &rd->st[rd->depth];
    memset(f, 0, sizeof(*f));
    f->el = (unsigned char)el;
    f->text = p->text;
    f->mcol = p->mcol;
    f->lst = (short)(is_list(p->el) || p->el == E_LI ? rd->depth - 1 : p->lst);
    if(el == E_BLOCKQUOTE || el == E_DD) {
        f->text = capped(p->text + 2u, INDENT_MAX);
        f->lst = -1;
    }
    rd->open[el]++;
    rd->depth++;
    return f;
}

static void button_close(struct rd *rd);
static void select_close(struct rd *rd);
static int table_at(const struct rd *rd);

/* Close the top element, with what its end does to the text */
static void pop(struct rd *rd) {
    struct frame *f = &rd->st[rd->depth - 1];
    int el = f->el, t;
    unsigned int fl = FL(el);
    if(el == E_TD || el == E_TH) {                  /* a cell's end closes what is open inside it */
        link_close(rd);
        button_close(rd);
        select_close(rd);
        t = table_at(rd);                           /* its own table: the cell is on top */
        if(f->cell_pending && t && rd->st[t].data) emit(rd, "-", 1);   /* an empty cell of a data table */
    }
    rd->depth--;
    rd->open[el]--;
    switch(el) {
    case E_LI:
        if(rd->marker_on && rd->marker_frame == rd->depth) rd->marker_on = 0;   /* an item that showed nothing */
        want(rd, 1);
        return;
    case E_TD: case E_TH:
        if(f->cell_pending && !--rd->cells_pending) rd->sep = 0;
        return;
    case E_CAPTION:
        link_close(rd);
        want(rd, 1);
        return;
    case E_TABLE:
        rd->tables--;
        want(rd, 1);
        return;
    case E_PRE: case E_LISTING:
        rd->pre--;
        want(rd, 2);
        return;
    }
    if(fl & F_HEAD) {
        rd->headings--;
        close_span(rd);
    }
    if(fl & F_LIST) want(rd, f->nested ? 1 : 2);
    else if(fl & F_PARA) want(rd, 2);
    else if(fl & F_LINE) want(rd, 1);
}

static void pop_to(struct rd *rd, int i) {          /* close st[i] and everything inside it */
    while(rd->depth > i && rd->depth > 1) pop(rd);
}

/* The nearest open el that an end tag reaches: the standard's scopes (an end tag in a cell, a caption,
   a table or a list item's list cannot close what is outside it). 0: none. */
enum { SCOPE, SCOPE_LIST };
static int in_scope(const struct rd *rd, int el, int kind) {
    int i, e;
    if(!rd->open[el]) return 0;
    for(i = rd->depth - 1; i > 0; i--) {
        e = rd->st[i].el;
        if(e == el) return i;
        if((FL(e) & F_SCOPE) || (kind == SCOPE_LIST && (e == E_UL || e == E_OL))) return 0;
    }
    return 0;
}

static int heading_in_scope(const struct rd *rd) {
    int i;
    if(!rd->headings) return 0;
    for(i = rd->depth - 1; i > 0; i--) {
        if(FL(rd->st[i].el) & F_HEAD) return i;
        if(FL(rd->st[i].el) & F_SCOPE) return 0;
    }
    return 0;
}

static void close_p(struct rd *rd) {
    int i = in_scope(rd, E_P, SCOPE);
    if(i) pop_to(rd, i);
}

/* <li> closes the open item of its list; <dt> and <dd> the open term or definition (the standard's walk) */
static void close_item(struct rd *rd, int el) {
    int i, e;
    for(i = rd->depth - 1; i > 0; i--) {
        e = rd->st[i].el;
        if(el == E_LI ? e == E_LI : e == E_DT || e == E_DD) {
            pop_to(rd, i);
            return;
        }
        if((FL(e) & F_SPECIAL) && e != E_ADDRESS && e != E_DIV && e != E_P) return;
    }
}

/* An integer attribute (ol start, li value) as the standard reads it, kept within a billion */
static int parse_int(const char *s, long *out) {
    long v = 0;
    int neg = 0, digits = 0;
    while(ws((unsigned char)*s)) s++;
    if(*s == '-' || *s == '+') neg = *s++ == '-';
    while(digit((unsigned char)*s)) {
        if(v < 100000000L) v = v * 10 + (*s - '0');     /* at most 999999999: no overflow, even in 32 bits */
        else v = 999999999L;
        s++;
        digits++;
    }
    if(!digits) return -1;
    *out = neg ? -v : v;
    return 0;
}

static void list_start(struct rd *rd, struct tag *t) {
    struct frame *p = &rd->st[rd->depth - 1], *f;
    int nested = p->lst >= 0 || is_list(p->el) || p->el == E_LI;
    want(rd, nested ? 1 : 2);
    f = push(rd, t->el);
    if(!f) return;
    f->nested = (unsigned char)nested;
    f->mcol = f->lst >= 0 ? capped(rd->st[f->lst].mcol + 2u, INDENT_MAX) : p->text;    /* two more per level */
    f->text = capped(f->mcol + 2u, INDENT_MAX);
    f->next = 1;
    if(t->el == E_OL && t->seen[A_START]) parse_int(t->val[A_START], &f->next);
}

static void item_start(struct rd *rd, struct tag *t) {
    struct frame *f, *list;
    long num;
    size_t n;
    close_item(rd, E_LI);
    want(rd, 1);
    f = push(rd, E_LI);
    if(!f) return;
    list = f->lst >= 0 && is_list(rd->st[f->lst].el) ? &rd->st[f->lst] : NULL;
    f->mcol = list ? list->mcol : rd->st[rd->depth - 2].text;
    if(list && list->el == E_OL) {
        num = list->next;
        if(t->seen[A_VALUE]) parse_int(t->val[A_VALUE], &num);
        list->next = num + 1;
        snprintf(rd->marker, sizeof(rd->marker), "%ld. ", num);
    } else snprintf(rd->marker, sizeof(rd->marker), "%s", "* ");
    n = strlen(rd->marker);
    f->text = capped(f->mcol + (unsigned int)n, INDENT_MAX + 8);
    rd->marker_on = 1;
    rd->marker_col = f->mcol;
    rd->marker_frame = rd->depth - 1;
}

/**** tables: a line per row, " | " between the cells that show something ****/

static int table_at(const struct rd *rd) {          /* the innermost open table, 0: none */
    int i;
    if(!rd->tables) return 0;
    for(i = rd->depth - 1; i > 0; i--) if(rd->st[i].el == E_TABLE) return i;
    return 0;
}

static int above(const struct rd *rd, int from, int el, int el2) {     /* the nearest el or el2 above st[from] */
    int i;
    for(i = rd->depth - 1; i > from; i--) if(rd->st[i].el == el || rd->st[i].el == el2) return i;
    return 0;
}

static void row_start(struct rd *rd) {
    int t = table_at(rd);
    if(!t) return;                                  /* a row outside a table is no row */
    pop_to(rd, t + 1);
    want(rd, 1);
    push(rd, E_TR);
}

static void cell_start(struct rd *rd, int el) {
    int t = table_at(rd), r, c;
    struct frame *f;
    if(!t) return;
    if(el == E_TH) rd->st[t].data = 1;              /* a data table from its first header cell on */
    link_close(rd);
    c = above(rd, t, E_TD, E_TH);
    if(c) pop_to(rd, c);
    r = above(rd, t, E_TR, E_TR);
    if(!r) {                                        /* a cell without a row: the row is implied */
        pop_to(rd, t + 1);
        want(rd, 1);
        if(!push(rd, E_TR)) return;
        r = rd->depth - 1;
    } else pop_to(rd, r + 1);
    if(rd->st[r].row_shown) {                       /* the row's line goes on */
        rd->breaks = 0;
        rd->space = 0;
        rd->sep = 1;
    }
    f = push(rd, el);
    if(!f) {
        rd->sep = 0;
        return;
    }
    f->cell_pending = 1;
    rd->cells_pending++;
}

static void caption_start(struct rd *rd) {
    int t = table_at(rd);
    if(!t) return;
    pop_to(rd, t + 1);
    link_close(rd);
    want(rd, 1);
    push(rd, E_CAPTION);
}

static void table_end(struct rd *rd, int el) {      /* </table>, </tr>, </td>, </th>, </caption> */
    int t = table_at(rd), i;
    if(!t) return;
    if(el == E_TABLE) i = t;
    else i = above(rd, t, el, el);
    if(i) pop_to(rd, i);
}

/**** anchors ****/

/* An id, or an <a name>: kept with the offset of its element's text, which is not known yet. The first of
   two equal names counts; a name over UT_FIELD_MAX bytes, or with a character that is not safe, is not
   kept (it could not be named exactly anyway). */
static void add_anchor(struct rd *rd, const char *name, size_t len, int over) {
    unsigned int h = 2166136261u;
    size_t i, slot, k;
    unsigned int cp;
    struct ut_anchor *a;
    if(!len || over || len > UT_FIELD_MAX || rd->stopped || rd->failed) return;
    for(i = 0; i < len; i += k) {
        k = ut_utf8_decode((const unsigned char *)name + i, len - i, &cp);
        if(hidden_char(cp)) return;
    }
    for(i = 0; i < len; i++) h = (h ^ (unsigned char)name[i]) * 16777619u;
    for(slot = h % AHASH; rd->ahash[slot]; slot = (slot + 1) % AHASH)
        if(!strcmp(rd->anchors[rd->ahash[slot] - 1].name, name)) return;
    if(rd->nanchors == UT_ANCHORS_MAX) {
        rd->truncated |= UT_TRUNC_ANCHORS;
        return;
    }
    if(rd->nanchors == rd->anchors_cap) {
        size_t cap = rd->anchors_cap ? rd->anchors_cap * 2 : 8;
        a = realloc(rd->anchors, cap * sizeof(*a));
        if(!a) {
            rd->failed = 1;
            return;
        }
        rd->anchors = a;
        rd->anchors_cap = cap;
    }
    a = &rd->anchors[rd->nanchors];
    a->name = malloc(len + 1);
    if(!a->name) {
        rd->failed = 1;
        return;
    }
    memcpy(a->name, name, len + 1);
    a->offset = 0;
    rd->ahash[slot] = (unsigned short)(rd->nanchors + 1);
    rd->nanchors++;
}

/**** links ****/

/* An address as written, without the ASCII whitespace and controls at its ends */
static const char *stripped(const char *s, size_t *n) {
    while(*n && (unsigned char)s[*n - 1] <= ' ') --*n;
    while(*n && (unsigned char)*s <= ' ') {
        s++;
        --*n;
    }
    return s;
}

/* A label for a link that shows no text: an element's aria-label, else its title (the first that has one:
   the <a> itself, else an element inside it) */
static void link_alt(struct rd *rd, struct tag *t) {
    size_t n = 0;
    if(t->seen[A_ARIA]) n = collapse(rd, t->val[A_ARIA], t->len[A_ARIA]);
    if(!n && t->seen[A_TITLE]) n = collapse(rd, t->val[A_TITLE], t->len[A_TITLE]);
    memcpy(rd->link_alt, rd->scratch, n);
    rd->link_alt_len = n;
}

static void link_start(struct rd *rd, struct tag *t) {
    size_t n = t->len[A_HREF], cap;
    const char *h = stripped(t->val[A_HREF], &n);
    void *p;
    link_close(rd);
    if(!t->seen[A_HREF] || !n) return;              /* no address: no link */
    rd->link_open = 1;
    rd->link_text = 0;
    rd->link_num = 0;
    rd->label_len = 0;
    rd->label_gap = 0;
    rd->label_full = 0;
    rd->link_alt_len = 0;
    if(rd->stopped || rd->failed) return;
    link_alt(rd, t);
    if(rd->nlinks == UT_LINKS_MAX) {                /* its text, but no number */
        rd->truncated |= UT_TRUNC_LINKS;
        return;
    }
    if(rd->nlinks == rd->links_cap) {
        cap = rd->links_cap ? rd->links_cap * 2 : 8;
        p = realloc(rd->hrefs, cap * sizeof(*rd->hrefs));
        if(p) rd->hrefs = p;
        p = p ? realloc(rd->links, cap * sizeof(*rd->links)) : NULL;
        if(!p) {
            rd->failed = 1;
            return;
        }
        rd->links = p;
        rd->links_cap = cap;
    }
    rd->hrefs[rd->nlinks] = malloc(n + 1);
    if(!rd->hrefs[rd->nlinks]) {
        rd->failed = 1;
        return;
    }
    memcpy(rd->hrefs[rd->nlinks], h, n);
    rd->hrefs[rd->nlinks][n] = 0;
    rd->long_href[rd->nlinks] = t->over[A_HREF];
    memset(&rd->links[rd->nlinks], 0, sizeof(rd->links[0]));
    rd->link_num = (int)++rd->nlinks;
}

/* A link's "[N]": right after its label, with the space or line break that waits still waiting; where the
   link is when it showed nothing. 0, or -1 when it does not fit. */
static int link_marker(struct rd *rd) {
    char m[16];
    size_t n = (size_t)snprintf(m, sizeof(m), "[%d]", rd->link_num);
    if(rd->link_text) {
        if(rd->stopped || rd->failed) return -1;
    } else if(begin(rd) < 0) return -1;
    close_span(rd);
    if(put(rd, m, n)) return -1;
    rd->col += (unsigned int)n;
    add_span(rd, rd->tlen - n, n, UT_SPAN_LINK);
    return rd->failed ? -1 : 0;
}

static void link_close(struct rd *rd) {
    char *label;
    if(!rd->link_open) return;
    if(rd->link_num && !rd->label_len && rd->link_alt_len)
        words(rd, rd->link_alt, rd->link_alt_len);     /* no text: its aria-label or title shows as its label */
    rd->link_open = 0;
    if(!rd->link_num) return;
    if(!link_marker(rd)) {
        label = malloc(rd->label_len + 1);
        if(!label) {
            rd->failed = 1;
            return;
        }
        memcpy(label, rd->label, rd->label_len);
        label[rd->label_len] = 0;
        rd->links[rd->link_num - 1].label = label;
        return;
    }
    if(rd->failed) return;
    rd->nlinks--;                                   /* the text is full: its [N] is not in it, so it is no link */
    free(rd->hrefs[rd->nlinks]);
    rd->hrefs[rd->nlinks] = NULL;
}

/* Remove tabs and line breaks, and write spaces as %20, as browsers do before they resolve an address */
static char *clean_href(const char *h) {
    size_t n = 0, i;
    char *c;
    for(i = 0; h[i]; i++) n += h[i] == ' ' ? 3 : h[i] == '\t' || h[i] == '\n' || h[i] == '\r' ? 0 : 1;
    c = malloc(n + 1);
    if(!c) return NULL;
    for(n = 0, i = 0; h[i]; i++) {
        if(h[i] == '\t' || h[i] == '\n' || h[i] == '\r') continue;
        if(h[i] == ' ') {
            memcpy(c + n, "%20", 3);
            n += 3;
        } else c[n++] = h[i];
    }
    c[n] = 0;
    return c;
}

/* Well-formed UTF-8 without a character that is never shown: an address that is safe to print */
static int safe_utf8(const char *s) {
    size_t n = strlen(s), i = 0, k;
    unsigned int cp;
    while(i < n) {
        k = ut_utf8_decode((const unsigned char *)s + i, n - i, &cp);
        if((k == 1 && (unsigned char)s[i] >= 0x80) || hidden_char(cp)) return 0;
        i += k;
    }
    return 1;
}

/* Resolve href against base into u: 0, 1 when it does not resolve (or not to a safe address), -1 out of memory */
static int resolve(const struct ut_url *base, const char *href, struct ut_url *u) {
    char err[128], *c = clean_href(href);
    int rc;
    if(!c) return -1;
    err[0] = 0;
    rc = ut_url_resolve(base, c, u, err, sizeof(err));
    free(c);
    if(!rc) return safe_utf8(u->text) ? 0 : 1;
    return strstr(err, "out of memory") ? -1 : 1;
}

static void base_href(struct rd *rd, struct tag *t) {
    size_t n = t->len[A_HREF];
    const char *h;
    char *copy;
    struct ut_url *u;
    int rc;
    if(rd->base || !t->seen[A_HREF] || t->over[A_HREF]) return;
    h = stripped(t->val[A_HREF], &n);
    if(!n) return;
    copy = malloc(n + 1);
    u = malloc(sizeof(*u));
    if(!copy || !u) {
        free(copy);
        free(u);
        rd->failed = 1;
        return;
    }
    memcpy(copy, h, n);
    copy[n] = 0;
    rc = resolve(&rd->r->url, copy, u);
    free(copy);
    if(rc < 0) rd->failed = 1;
    if(rc) free(u);
    else rd->base = u;                              /* the first <base> that resolves counts */
}

/**** images and forms ****/

static void image(struct rd *rd, struct tag *t) {
    size_t n;
    if(!t->seen[A_ALT] || !(n = collapse(rd, t->val[A_ALT], t->len[A_ALT]))) return;
    if(rd->link_open || rd->button) words(rd, rd->scratch, n);         /* a label: no brackets */
    else {
        emit(rd, "[", 1);
        words(rd, rd->scratch, n);
        closer(rd, "]");
    }
}

/* A button or a select is a box: the space at the end of its text stays inside it */
static void button_close(struct rd *rd) {
    int b = rd->button;
    rd->button = 0;
    if(b == 2) {
        closer(rd, "]");
        rd->space = 0;
    } else if(b == 1) atom(rd, "[button]");
}

static void select_close(struct rd *rd) {
    int shown = rd->select_shown;
    if(!rd->select) return;
    rd->select = rd->option = rd->select_shown = 0;
    if(shown) {
        closer(rd, "]");
        rd->space = 0;
    } else atom(rd, "[select]");
}

static int is_type(struct tag *t, const char *type) {
    return t->seen[A_TYPE] && ci_eq((const unsigned char *)t->val[A_TYPE], t->len[A_TYPE], type);
}

static void input(struct rd *rd, struct tag *t) {
    const char *label = NULL;
    size_t n;
    if(is_type(t, "hidden")) return;
    rd->forms = 1;
    if(is_type(t, "checkbox")) atom(rd, "[checkbox]");
    else if(is_type(t, "radio")) atom(rd, "[radio button]");
    else if(is_type(t, "file")) atom(rd, "[file field]");
    else if(is_type(t, "password")) atom(rd, "[password field]");
    else if(is_type(t, "submit") || is_type(t, "reset") || is_type(t, "button") || is_type(t, "image")) {
        if(is_type(t, "image")) n = t->seen[A_ALT] ? collapse(rd, t->val[A_ALT], t->len[A_ALT]) : 0;
        else n = t->seen[A_VALUE] ? collapse(rd, t->val[A_VALUE], t->len[A_VALUE]) : 0;
        if(!n && !t->seen[is_type(t, "image") ? A_ALT : A_VALUE])
            label = is_type(t, "reset") ? "Reset" : is_type(t, "button") ? NULL : "Submit";
        if(n) {
            emit(rd, "[button: ", 9);
            words(rd, rd->scratch, n);
            closer(rd, "]");
        } else if(label) {
            emit(rd, "[button: ", 9);
            atom(rd, label);
            closer(rd, "]");
        } else atom(rd, "[button]");
    } else atom(rd, "[text field]");
}

/* A tag inside <select>: 1 when it was dealt with here (only options count; anything else is ignored) */
static int select_tag(struct rd *rd, struct tag *t) {
    int el = t->el;
    if(el == E_OPTION || el == E_OPTGROUP) {
        if(!t->end && el == E_OPTION && !rd->option) rd->option = 1;
        else if(rd->option == 1) rd->option = 2;
        return 1;
    }
    if(el == E_SELECT || (!t->end && (el == E_INPUT || el == E_KEYGEN || el == E_TEXTAREA))) {
        select_close(rd);
        return el == E_SELECT;
    }
    if(el == E_TABLE || el == E_CAPTION || el == E_TBODY || el == E_THEAD || el == E_TFOOT || el == E_TR ||
       el == E_TD || el == E_TH) {                  /* a table's structure ends a select inside it */
        select_close(rd);
        return 0;
    }
    return !(el == E_SCRIPT || el == E_STYLE || el == E_TEMPLATE);
}

/**** tags ****/

static void start_tag(struct rd *rd, struct tag *t) {
    int el = t->el;
    unsigned int fl = FL(el);
    if(t->seen[A_ID]) add_anchor(rd, t->val[A_ID], t->len[A_ID], t->over[A_ID]);
    if(fl & F_CLOSEP) close_p(rd);
    switch(el) {
    case E_A:
        if(t->seen[A_NAME]) add_anchor(rd, t->val[A_NAME], t->len[A_NAME], t->over[A_NAME]);
        link_start(rd, t);
        return;
    case E_UL: case E_OL: case E_MENU: case E_DIR:
        list_start(rd, t);
        return;
    case E_LI:
        item_start(rd, t);
        return;
    case E_DT: case E_DD:
        close_item(rd, E_DT);
        want(rd, 1);
        push(rd, el);
        return;
    case E_H1: case E_H2: case E_H3: case E_H4: case E_H5: case E_H6:
        if(FL(rd->st[rd->depth - 1].el) & F_HEAD) pop(rd);        /* headings do not nest */
        want(rd, 2);
        if(push(rd, el)) rd->headings++;
        return;
    case E_PRE: case E_LISTING:
        want(rd, 2);
        if(push(rd, el)) rd->pre++;
        if(rd->pos < rd->len && rd->in[rd->pos] == '\r') rd->pos++;      /* a newline right after <pre> */
        if(rd->pos < rd->len && rd->in[rd->pos] == '\n') rd->pos++;
        return;
    case E_BR:
        newline(rd);
        return;
    case E_HR:
        want(rd, 1);
        atom(rd, RULE);
        want(rd, 1);
        return;
    case E_IMG: case E_IMAGE:
        image(rd, t);
        return;
    case E_INPUT:
        input(rd, t);
        return;
    case E_BUTTON:
        button_close(rd);
        rd->forms = 1;
        rd->button = 1;
        return;
    case E_SELECT:
        rd->forms = 1;
        rd->select = 1;
        rd->option = rd->select_shown = 0;
        return;
    case E_TEXTAREA:
        rd->forms = 1;
        atom(rd, "[text area]");
        return;
    case E_BASE:
        base_href(rd, t);
        return;
    case E_TABLE:
        want(rd, 1);
        if(push(rd, el)) rd->tables++;
        return;
    case E_TR:
        row_start(rd);
        return;
    case E_TD: case E_TH:
        cell_start(rd, el);
        return;
    case E_CAPTION:
        caption_start(rd);
        return;
    case E_TBODY: case E_THEAD: case E_TFOOT:
        if(table_at(rd)) pop_to(rd, table_at(rd) + 1);
        return;
    case E_FORM:
        rd->forms = 1;
        break;
    }
    if(fl & F_PARA) want(rd, 2);
    else if(fl & F_LINE) want(rd, 1);
    if(fl & F_PUSH) push(rd, el);
}

static void end_tag(struct rd *rd, struct tag *t) {
    int el = t->el, i;
    unsigned int fl = FL(el);
    if((fl & F_PUSH) && rd->over[el]) {             /* one that was too deep to keep */
        rd->over[el]--;
        return;
    }
    switch(el) {
    case E_A:
        link_close(rd);
        return;
    case E_BR:                                      /* </br> is <br>, as in browsers */
        newline(rd);
        return;
    case E_P:
        if((i = in_scope(rd, E_P, SCOPE))) pop_to(rd, i);
        else want(rd, 2);                           /* a lone </p> is an empty paragraph */
        return;
    case E_LI:
        if((i = in_scope(rd, E_LI, SCOPE_LIST))) pop_to(rd, i);
        return;
    case E_H1: case E_H2: case E_H3: case E_H4: case E_H5: case E_H6:
        if((i = heading_in_scope(rd))) pop_to(rd, i);
        return;
    case E_BUTTON:
        button_close(rd);
        return;
    case E_TABLE: case E_TR: case E_TD: case E_TH: case E_CAPTION:
        table_end(rd, el);
        return;
    case E_TBODY: case E_THEAD: case E_TFOOT:
        if(table_at(rd)) pop_to(rd, table_at(rd) + 1);
        return;
    }
    if((fl & F_PUSH) && (i = in_scope(rd, el, SCOPE))) pop_to(rd, i);
}

static void dispatch(struct rd *rd, struct tag *t);

/* A tag inside content that is not shown: only what ends that content matters. In svg and math, the
   HTML elements of the standard's list (p, div, b, ...) end them, as in browsers. */
static void hidden_tag(struct rd *rd, struct tag *t, int whole) {
    int el = t->el;
    unsigned int fl = FL(el);
    if((rd->hide[E_SVG] || rd->hide[E_MATH]) && (t->end ? el == E_P || el == E_BR : (fl & F_OUT) != 0)) {
        rd->skip -= (int)(rd->hide[E_SVG] + rd->hide[E_MATH]);
        rd->hide[E_SVG] = rd->hide[E_MATH] = 0;
        if(!rd->skip) dispatch(rd, t);
        return;
    }
    if(!(fl & F_HIDE)) return;
    if(t->end) {
        if(rd->hide[el]) {
            rd->hide[el]--;
            rd->skip--;
        }
    } else if(!whole) {
        rd->hide[el]++;
        rd->skip++;
    }
}

static void dispatch(struct rd *rd, struct tag *t) {
    int el = t->el;
    unsigned int fl = FL(el);
    int whole = t->selfclose && (rd->xhtml || el == E_SVG || el == E_MATH);     /* <x /> is all of x */
    if(!t->end && el != E_A && rd->link_open && rd->link_num && !rd->link_alt_len)
        link_alt(rd, t);                            /* the first element in a link with a label for it */
    if(rd->skip) {
        hidden_tag(rd, t, whole);
        return;
    }
    if(rd->select && select_tag(rd, t)) return;
    if(t->end) {
        end_tag(rd, t);
        return;
    }
    if(fl & F_HIDE) {
        if(!whole) {
            rd->hide[el]++;
            rd->skip++;
        }
        return;
    }
    start_tag(rd, t);
    if(whole && !(fl & F_VOID)) end_tag(rd, t);
}

/**** the tokenizer ****/

static int element_of(const char *name) {
    size_t lo = 0, hi = sizeof(elements) / sizeof(elements[0]);
    while(lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(elements[mid].name, name);
        if(!c) return elements[mid].id;
        if(c < 0) lo = mid + 1;
        else hi = mid;
    }
    return E_OTHER;
}

static int attr_of(const char *name) {
    int i;
    for(i = 0; i < A_COUNT; i++) if(!strcmp(name, attr_info[i].name)) return i;
    return -1;
}

/* An attribute's value from in[q] (quote: '"', '\'' or 0 for an unquoted one): decoded into the tag's
   buffer for it when which >= 0, cut at its size (over is set); returns where it stops, at the quote */
static size_t attr_value(struct rd *rd, struct tag *t, int which, size_t q, unsigned char quote) {
    const unsigned char *s = rd->in, *j;
    size_t n = rd->len, k;
    unsigned char b[4];
    unsigned int cp;
    while(q < n && (quote ? s[q] != quote : !ws(s[q]) && s[q] != '>')) {
        if(which < 0 || t->over[which]) {           /* nothing more is kept: find the end */
            if(!quote) {
                q++;
                continue;
            }
            j = memchr(s + q, quote, n - q);
            q = j ? (size_t)(j - s) : n;
            break;
        }
        cp = s[q] == '&' ? char_ref(rd, &q, n, 1) : next_char(rd, &q, n);
        if(!cp) continue;                           /* NUL */
        k = ut_utf8_encode(cp, b);
        if(t->len[which] + k > attr_info[which].cap) {
            t->over[which] = 1;
            continue;
        }
        memcpy(t->val[which] + t->len[which], b, k);
        t->len[which] += k;
    }
    if(which >= 0) t->val[which][t->len[which]] = 0;
    return q;
}

/* A tag whose name starts at in[q]: read it with its attributes and deal with it. 0 when the body ends
   inside it (it is dropped, and nothing after it is left) */
static int tag(struct rd *rd, size_t q, int end) {
    struct tag *t = &rd->tag;
    const unsigned char *s = rd->in;
    size_t n = rd->len, k = 0, an;
    char aname[16];
    unsigned char quote;
    int i, which, long_name = 0;
    for(i = 0; i < A_COUNT; i++) {
        t->seen[i] = t->over[i] = 0;
        t->len[i] = 0;
        t->val[i][0] = 0;
    }
    while(q < n && !ws(s[q]) && s[q] != '/' && s[q] != '>') {
        if(k + 1 < sizeof(t->name)) t->name[k++] = (char)lower(s[q]);
        else long_name = 1;
        q++;
    }
    t->name[k] = 0;
    t->el = long_name ? E_OTHER : element_of(t->name);
    t->end = end;
    t->selfclose = 0;
    for(;;) {
        while(q < n && (ws(s[q]) || s[q] == '/')) {
            if(s[q] == '/' && q + 1 < n && s[q + 1] == '>') t->selfclose = 1;
            q++;
        }
        if(q >= n) {
            rd->pos = n;
            return 0;
        }
        if(s[q] == '>') break;
        an = 0;
        long_name = 0;
        do {                                        /* an attribute's name; its first character may be '=' */
            if(an + 1 < sizeof(aname)) aname[an++] = (char)lower(s[q]);
            else long_name = 1;
            q++;
        } while(q < n && !ws(s[q]) && s[q] != '/' && s[q] != '>' && s[q] != '=');
        aname[an] = 0;
        which = long_name || end ? -1 : attr_of(aname);
        if(which >= 0 && t->seen[which]) which = -1;               /* the first of two counts */
        while(q < n && ws(s[q])) q++;
        if(q < n && s[q] == '=') {
            q++;
            while(q < n && ws(s[q])) q++;
            quote = q < n && (s[q] == '"' || s[q] == '\'') ? s[q] : 0;
            if(quote) q++;
            q = attr_value(rd, t, which, q, quote);
            if(quote) {
                if(q >= n) {                        /* a quote that never closes: the rest is this tag */
                    rd->pos = n;
                    return 0;
                }
                q++;
            }
        }
        if(which >= 0) t->seen[which] = 1;
    }
    rd->pos = q + 1;
    dispatch(rd, t);
    return 1;
}

/* Where the raw text that started at rd->pos ends: at "</name" and whitespace, '/' or '>' */
static size_t raw_end(const struct rd *rd, const char *name) {
    const unsigned char *s = rd->in, *j;
    size_t n = rd->len, q = rd->pos, k = strlen(name);
    while(q < n && (j = memchr(s + q, '<', n - q)) != NULL) {
        q = (size_t)(j - s);
        if(q + 2 + k < n && s[q + 1] == '/' && ci_prefix(s + q + 2, k, name) &&
           (ws(s[q + 2 + k]) || s[q + 2 + k] == '/' || s[q + 2 + k] == '>')) return q;
        q++;
    }
    return n;
}

/* "</script" or "<script" at in[q], followed by whitespace, '/' or '>' */
static int script_tag_at(const unsigned char *s, size_t n, size_t q, int end) {
    size_t k = q + 1 + (end ? 1 : 0);
    if(end && (q + 1 >= n || s[q + 1] != '/')) return 0;
    return k + 6 < n && ci_prefix(s + k, 6, "script") && (ws(s[k + 6]) || s[k + 6] == '/' || s[k + 6] == '>');
}

/* Where a script's text ends, with the HTML standard's escaped states: after "<!--" a script may hold
   "<script>...</script>" (as document.write() strings do) and goes on to its real end tag. Linear. */
static size_t script_end(const struct rd *rd) {
    const unsigned char *s = rd->in, *j;
    size_t n = rd->len, q = rd->pos;
    int state = 0, dashes = 0;                      /* 0: script data, 1: escaped, 2: double escaped */
    while(q < n) {
        if(!state) {
            j = memchr(s + q, '<', n - q);
            if(!j) return n;
            q = (size_t)(j - s);
            if(script_tag_at(s, n, q, 1)) return q;
            if(n - q >= 4 && s[q + 1] == '!' && s[q + 2] == '-' && s[q + 3] == '-') {
                state = 1;
                dashes = 2;
                q += 4;
            } else q++;
            continue;
        }
        if(s[q] == '-') {
            dashes++;
            q++;
            continue;
        }
        if(s[q] == '>' && dashes >= 2) state = 0;   /* "-->" ends the escape */
        else if(s[q] == '<' && state == 1) {
            if(script_tag_at(s, n, q, 1)) return q;
            if(script_tag_at(s, n, q, 0)) {
                state = 2;                          /* "<script": the next "</script" is its own */
                q += 7;
                dashes = 0;
                continue;
            }
        } else if(s[q] == '<' && state == 2 && script_tag_at(s, n, q, 1)) {
            state = 1;
            q += 8;
            dashes = 0;
            continue;
        }
        dashes = 0;
        q++;
    }
    return n;
}

/* A title's text: references decoded, whitespace collapsed, unsafe characters dropped, cut at UT_FIELD_MAX */
static void title_text(struct rd *rd, size_t q, size_t end) {
    unsigned char b[4];
    unsigned int cp;
    size_t k;
    int gap = 0;
    while(q < end) {
        cp = rd->in[q] == '&' ? char_ref(rd, &q, end, 0) : next_char(rd, &q, end);
        if(space_char(cp) || cp == 0xa0) {
            gap = 1;
            continue;
        }
        if(hidden_char(cp)) continue;
        k = ut_utf8_encode(cp, b);
        if(rd->title_len + (gap && rd->title_len) + k > UT_FIELD_MAX) return;
        if(gap && rd->title_len) rd->title[rd->title_len++] = ' ';
        gap = 0;
        memcpy(rd->title + rd->title_len, b, k);
        rd->title_len += k;
    }
}

/* The content of a raw text element, up to its own end tag (which the next markup() reads). Only a title
   (the first one) and an xmp's text are kept, and only when nothing hides them. */
static void raw(struct rd *rd, int el, const char *name) {
    size_t start = rd->pos, end = el == E_SCRIPT ? script_end(rd) : raw_end(rd, name), q;
    rd->pos = end;
    if(rd->skip) return;
    if(el == E_TITLE && !rd->title_seen) {
        rd->title_seen = 1;
        title_text(rd, start, end);
    } else if(el == E_XMP && !rd->select) {
        rd->pre++;
        for(q = start; q < end && !rd->stopped && !rd->failed; ) text_char(rd, next_char(rd, &q, end));
        rd->pre--;
        want(rd, 2);
    }
}

/* A comment, from just after "<!--": up to "-->" or "--!>"; "<!-->" and "<!--->" are empty ones */
static void comment(struct rd *rd, size_t q) {
    const unsigned char *s = rd->in, *j;
    size_t n = rd->len;
    if(q < n && s[q] == '>') {
        rd->pos = q + 1;
        return;
    }
    if(q + 1 < n && s[q] == '-' && s[q + 1] == '>') {
        rd->pos = q + 2;
        return;
    }
    while(q < n && (j = memchr(s + q, '-', n - q)) != NULL) {
        q = (size_t)(j - s);
        if(q + 2 < n && s[q + 1] == '-' && s[q + 2] == '>') {
            rd->pos = q + 3;
            return;
        }
        if(q + 3 < n && s[q + 1] == '-' && s[q + 2] == '!' && s[q + 3] == '>') {
            rd->pos = q + 4;
            return;
        }
        q++;
    }
    rd->pos = n;                                    /* an unterminated comment hides the rest */
}

static void skip_past(struct rd *rd, size_t q, const char *end) {
    const unsigned char *s = rd->in, *j;
    size_t n = rd->len, k = strlen(end);
    while(q < n && (j = memchr(s + q, end[0], n - q)) != NULL) {
        q = (size_t)(j - s);
        if(n - q >= k && !memcmp(s + q, end, k)) {
            rd->pos = q + k;
            return;
        }
        q++;
    }
    rd->pos = n;
}

/* At a '<' */
static void markup(struct rd *rd) {
    const unsigned char *s = rd->in;
    size_t p = rd->pos, n = rd->len;
    struct tag *t = &rd->tag;
    unsigned char c = p + 1 < n ? s[p + 1] : 0;
    if(c == '!') {
        if(p + 3 < n && s[p + 2] == '-' && s[p + 3] == '-') comment(rd, p + 4);
        else if((rd->hide[E_SVG] || rd->hide[E_MATH]) && n - p >= 9 && !memcmp(s + p + 2, "[CDATA[", 7))
            skip_past(rd, p + 9, "]]>");            /* CDATA exists only in svg and math */
        else skip_past(rd, p + 2, ">");             /* <!DOCTYPE ...>, a bogus comment */
    } else if(c == '?') skip_past(rd, p + 2, ">");  /* a processing instruction is a bogus comment */
    else if(c == '/') {
        if(p + 2 < n && alpha(s[p + 2])) tag(rd, p + 2, 1);
        else if(p + 2 < n && s[p + 2] == '>') rd->pos = p + 3;
        else if(p + 2 < n) skip_past(rd, p + 2, ">");
        else {
            text_char(rd, '<');
            text_char(rd, '/');
            rd->pos = n;
        }
    } else if(alpha(c)) {
        if(tag(rd, p + 1, 0) && (FL(t->el) & (F_RAW | F_RCDATA)) &&
           !(t->selfclose && (rd->xhtml || rd->hide[E_SVG] || rd->hide[E_MATH])))     /* <title/> in svg is whole */
            raw(rd, t->el, t->name);
    } else {
        text_char(rd, '<');
        rd->pos = p + 1;
    }
}

static void run_html(struct rd *rd) {
    const unsigned char *j;
    while(rd->pos < rd->len && !rd->failed) {
        unsigned char c = rd->in[rd->pos];
        if(c == '<') markup(rd);
        else if(rd->skip || rd->stopped || (rd->select && rd->option != 1)) {
            j = memchr(rd->in + rd->pos, '<', rd->len - rd->pos);      /* text nobody sees */
            rd->pos = j ? (size_t)(j - rd->in) : rd->len;
        } else if(c == '&') text_char(rd, char_ref(rd, &rd->pos, rd->len, 0));
        else text_char(rd, next_char(rd, &rd->pos, rd->len));
    }
}

static void run_plain(struct rd *rd) {
    rd->pre = 1;
    while(rd->pos < rd->len && !rd->failed && !rd->stopped) text_char(rd, next_char(rd, &rd->pos, rd->len));
}

/**** the document ****/

/* href (an address as written) starts with a scheme other than http and https */
static int other_scheme(const char *h) {
    size_t i = 0;
    if(!alpha((unsigned char)h[0])) return 0;
    while(alnum((unsigned char)h[i]) || h[i] == '+' || h[i] == '-' || h[i] == '.') i++;
    if(h[i] != ':') return 0;
    return !ci_eq((const unsigned char *)h, i, "http") && !ci_eq((const unsigned char *)h, i, "https");
}

/* in (UTF-8, or not) as one line of safe text in out (cap bytes, terminated): what ut_safe_text() does,
   but without anything hidden_char() drops. Returns its length. */
static size_t safe_copy(char *out, size_t cap, const char *in) {
    const unsigned char *s = (const unsigned char *)in;
    size_t n = strlen(in), i = 0, o = 0, k;
    unsigned int cp;
    while(i < n) {
        i += ut_utf8_decode(s + i, n - i, &cp);
        if(space_char(cp)) cp = ' ';
        else if(hidden_char(cp)) continue;
        if(o + 4 >= cap) break;
        k = ut_utf8_encode(cp, (unsigned char *)out + o);
        o += k;
    }
    out[o] = 0;
    return o;
}

/* Every link's address, now that the <base> is known: the absolute address when it resolves to a safe one
   (a fragment alone against the page itself), else the reference as written, as safe text. */
static int resolve_links(struct rd *rd) {
    struct work { struct ut_url u; char safe[UT_URL_MAX + 1]; } *w;
    const struct ut_url *base;
    struct ut_link *l;
    size_t i, n;
    int rc;
    if(!rd->nlinks) return 0;
    w = malloc(sizeof(*w));
    if(!w) return -1;
    for(i = 0; i < rd->nlinks; i++) {
        const char *h = rd->hrefs[i];
        l = &rd->links[i];
        base = h[0] == '#' || !rd->base ? &rd->r->url : rd->base;
        rc = rd->long_href[i] || other_scheme(h) ? 1 : resolve(base, h, &w->u);
        if(rc < 0) break;
        if(rc == 0) {
            n = strlen(w->u.text);
            l->url = malloc(n + 1);
            if(!l->url) break;
            memcpy(l->url, w->u.text, n + 1);
            l->usable = 1;
        } else {
            n = safe_copy(w->safe, sizeof(w->safe), h);
            l->url = malloc(n + 1);
            if(!l->url) break;
            memcpy(l->url, w->safe, n + 1);
        }
    }
    free(w);
    return i < rd->nlinks ? -1 : 0;
}

/* An array down to its count (kept as it is when that fails: its size is what is counted) */
static void *shrink(void *p, size_t count, size_t size, size_t *cap) {
    void *q;
    if(!p || count == *cap) return p;
    if(!count) {
        free(p);
        *cap = 0;
        return NULL;
    }
    q = realloc(p, count * size);
    if(!q) return p;
    *cap = count;
    return q;
}

static void add_notice(char *notice, size_t cap, const char *part) {
    size_t n = strlen(notice);
    if(n + (n ? 2 : 0) + strlen(part) >= cap) return;
    if(n) strcpy(notice + n, "; ");
    strcat(notice, part);
}

/* What is open closes, the cut gets its notice, links resolve, and the document takes what it owns */
static int finish(struct rd *rd, struct ut_document *out) {
    struct ut_document d;
    char part[160];
    size_t i;
    button_close(rd);
    select_close(rd);
    link_close(rd);
    close_span(rd);
    if(rd->failed) return -1;
    if(rd->text) {                                  /* no spaces or line breaks at the end */
        trim(rd);
        while(rd->tlen && rd->text[rd->tlen - 1] == '\n') rd->tlen--;
        for(i = 0; i < rd->nanchors; i++) if(rd->anchors[i].offset > rd->tlen) rd->anchors[i].offset = rd->tlen;
    }
    place_anchors(rd, rd->tlen);
    if(rd->stopped) {                               /* the text is full: a last line says so */
        rd->stopped = 0;
        rd->limit = UT_TEXT_MAX;
        if((rd->tlen && put(rd, "\n\n", 2)) || put(rd, cut_notice, sizeof(cut_notice) - 1)) return -1;
    }
    if(resolve_links(rd)) return -1;
    if(!rd->text) {                                 /* a page with no text at all */
        rd->text = malloc(1);
        if(!rd->text) return -1;
        rd->tcap = 1;
    }
    rd->text[rd->tlen] = 0;
    if(rd->tcap > rd->tlen + 1) {
        char *t = realloc(rd->text, rd->tlen + 1);
        if(t) {
            rd->text = t;
            rd->tcap = rd->tlen + 1;
        }
    }
    memset(&d, 0, sizeof(d));
    d.title = malloc(rd->title_len + 1);
    if(!d.title) return -1;
    memcpy(d.title, rd->title, rd->title_len);
    d.title[rd->title_len] = 0;
    d.text = rd->text;
    d.text_len = rd->tlen;
    d.links = shrink(rd->links, rd->nlinks, sizeof(*d.links), &rd->links_cap);
    d.link_count = rd->nlinks;
    d.anchors = shrink(rd->anchors, rd->nanchors, sizeof(*d.anchors), &rd->anchors_cap);
    d.anchor_count = rd->nanchors;
    d.spans = shrink(rd->spans, rd->nspans, sizeof(*d.spans), &rd->spans_cap);
    d.span_count = rd->nspans;
    d.truncated = rd->truncated;
    d.bytes = rd->tcap + rd->title_len + 1 + rd->links_cap * sizeof(*d.links) +
              rd->anchors_cap * sizeof(*d.anchors) + rd->spans_cap * sizeof(*d.spans);
    for(i = 0; i < d.link_count; i++) d.bytes += strlen(d.links[i].label) + 1 + strlen(d.links[i].url) + 1;
    for(i = 0; i < d.anchor_count; i++) d.bytes += strlen(d.anchors[i].name) + 1;
    if(rd->truncated & UT_TRUNC_LINKS) add_notice(d.notice, sizeof(d.notice), "only its first 256 links have numbers");
    if(rd->cs == CS_ASCII) {                        /* a long name is cut, so that every part fits */
        char name[25];
        copy_cut(name, sizeof(name), rd->charset);
        if(strlen(rd->charset) > 24) strcpy(name + 21, "...");
        snprintf(part, sizeof(part), "the page says it is in %s; only its ASCII is shown", name);
        add_notice(d.notice, sizeof(d.notice), part);
    }
    if(rd->forms) add_notice(d.notice, sizeof(d.notice), "forms are not supported");
    rd->given = 1;                                  /* the document's now */
    memcpy(out, &d, sizeof(d));
    return 0;
}

static void cleanup_parts(struct rd *rd);

/* Free what the document did not take, and the renderer */
static void cleanup(struct rd *rd) {
    size_t i;
    for(i = 0; rd->hrefs && i < rd->nlinks; i++) free(rd->hrefs[i]);
    free(rd->hrefs);
    if(!rd->given) cleanup_parts(rd);
    free(rd->base);
    free(rd->decoded);
    free(rd);
}

static void cleanup_parts(struct rd *rd) {
    size_t i;
    for(i = 0; rd->links && i < rd->nlinks; i++) {
        free(rd->links[i].label);
        free(rd->links[i].url);
    }
    free(rd->links);
    for(i = 0; rd->anchors && i < rd->nanchors; i++) free(rd->anchors[i].name);
    free(rd->anchors);
    free(rd->spans);
    free(rd->text);
}

int ut_render(const struct ut_response *r, struct ut_document *out, char *err, size_t cap) {
    struct rd *rd;
    int html;
    if(!r || !out || (!r->body && r->len)) return ut_error(err, cap, "there is no page to show");
    rd = calloc(1, sizeof(*rd));
    if(!rd) return ut_error(err, cap, "out of memory showing the page");
    rd->r = r;
    rd->in = r->body ? r->body : (const unsigned char *)"";
    rd->len = r->len;
    rd->limit = UT_TEXT_MAX - (sizeof(cut_notice) - 1) - 2;     /* room is kept for the notice of a cut */
    rd->depth = 1;
    rd->st[0].el = E_HTML;
    rd->st[0].lst = -1;
    rd->tag.val[A_HREF] = rd->tag.href;
    rd->tag.val[A_ID] = rd->tag.id;
    rd->tag.val[A_NAME] = rd->tag.name_;
    rd->tag.val[A_ALT] = rd->tag.alt;
    rd->tag.val[A_VALUE] = rd->tag.value;
    rd->tag.val[A_TYPE] = rd->tag.type;
    rd->tag.val[A_START] = rd->tag.start;
    rd->tag.val[A_ARIA] = rd->tag.aria;
    rd->tag.val[A_TITLE] = rd->tag.title;
    if(decode_utf16(rd)) {
        cleanup(rd);
        return ut_error(err, cap, "out of memory showing the page");
    }
    rd->xhtml = !strcmp(r->content_type, "application/xhtml+xml");
    html = rd->xhtml || !strcmp(r->content_type, "text/html") || (!r->content_type[0] && looks_like_html(rd->in, rd->len));
    if(!rd->decoded) choose_charset(rd, html);
    if(html) run_html(rd);
    else run_plain(rd);
    if(finish(rd, out)) {
        cleanup(rd);
        return ut_error(err, cap, "out of memory showing the page");
    }
    cleanup(rd);
    return 0;
}

void ut_document_free(struct ut_document *d) {
    size_t i;
    if(!d) return;
    for(i = 0; d->links && i < d->link_count; i++) {
        free(d->links[i].label);
        free(d->links[i].url);
    }
    for(i = 0; d->anchors && i < d->anchor_count; i++) free(d->anchors[i].name);
    free(d->text);
    free(d->title);
    free(d->links);
    free(d->anchors);
    free(d->spans);
    memset(d, 0, sizeof(*d));
}

/* name is fragment once fragment's %XX escapes are decoded */
static int same_decoded(const char *name, const char *fragment) {
    const unsigned char *f = (const unsigned char *)fragment;
    int hi, lo;
    while(*f) {
        unsigned char c = *f++;
        if(c == '%' && (hi = hexval(f[0])) >= 0 && (lo = hexval(f[1])) >= 0) {
            c = (unsigned char)(hi * 16 + lo);
            f += 2;
        }
        if(!c || (unsigned char)*name++ != c) return 0;
    }
    return !*name;
}

int ut_anchor_offset(const struct ut_document *d, const char *fragment, size_t *offset) {
    size_t i;
    if(!d || !fragment || !offset) return -1;
    for(i = 0; i < d->anchor_count; i++)
        if(!strcmp(d->anchors[i].name, fragment)) {
            *offset = d->anchors[i].offset;
            return 0;
        }
    if(strchr(fragment, '%'))                       /* as a browser does, with its escapes decoded */
        for(i = 0; i < d->anchor_count; i++)
            if(same_decoded(d->anchors[i].name, fragment)) {
                *offset = d->anchors[i].offset;
                return 0;
            }
    if(!fragment[0] || ci_eq((const unsigned char *)fragment, strlen(fragment), "top")) {
        *offset = 0;
        return 0;
    }
    return -1;
}
