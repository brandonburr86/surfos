/*
SurfOS Undertow: the HTML scanner, shared by html.c and the browser's window
----------------------
File: html_scan.h  Date: 10/25/26
----------------------
The parts of reading HTML that do not depend on what is made of it: which
element a name is and what the HTML standard says of it (the flags below),
the page's charset and its characters, character references, a tag with the
attributes a reader wants, and where comments and raw text end. Each reader
(html.c's text, the window's box tree) drives its own loop over these, and
keeps its own open elements.

Everything here reads the bytes it is given and changes nothing but what it
is pointed at; nothing allocates but ut_html_utf16(). Bounded as html.c is:
a tag's name keeps 15 bytes, each attribute the bytes its reader asked for.
*/

#ifndef UNDERTOW_HTML_SCAN_H
#define UNDERTOW_HTML_SCAN_H

#include <stddef.h>

/* the charsets read: UTF-8, windows-1252, and ASCII alone for one Undertow cannot read (UTF-16 is turned into
   UTF-8 first, by ut_html_utf16()) */
enum { UT_CS_UTF8, UT_CS_1252, UT_CS_ASCII, UT_CS_UTF16 };

#define UT_HTML_LABEL_MAX 64            /* bytes of a charset's name kept for a notice */

/* What the standard says of an element (ut_html_flags()) */
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

/* An attribute a reader keeps, and how many bytes of its value (UTF-8) at most; its buffer has cap + 1 */
struct ut_html_attr {
    const char *name;                   /* lowercase */
    size_t cap;
};

#define UT_HTML_ATTRS_MAX 48

/* A tag as ut_html_tag() reads it. The reader sets attrs, nattrs and val[] (buffers of attrs[i].cap + 1
   bytes) once; each read sets the rest. */
struct ut_html_tag {
    char name[16];                      /* lowercase, cut at 15 bytes (an element is never that long) */
    int el;                             /* E_*, E_OTHER for any other name */
    int end;                            /* an end tag: its attributes are not kept */
    int selfclose;                      /* "/>" */
    const struct ut_html_attr *attrs;   /* the attributes kept, nattrs of them */
    int nattrs;
    char *val[UT_HTML_ATTRS_MAX];       /* each one's value, NUL-terminated UTF-8, "" when not given */
    size_t len[UT_HTML_ATTRS_MAX];      /* its bytes */
    unsigned char seen[UT_HTML_ATTRS_MAX];  /* it was given (the first of two counts) */
    unsigned char over[UT_HTML_ATTRS_MAX];  /* it was longer than cap: cut there */
};

/* E_*'s flags (F_*); 0 for E_OTHER */
unsigned int ut_html_flags(int el);

/* Characters the reader leaves out of what it shows: ut_unsafe_char()'s and those a console cannot show
   (the soft hyphen, zero-width spaces, variation selectors, ...); and HTML's whitespace with the line and
   paragraph separators */
int ut_html_hidden(unsigned int cp);
int ut_html_space(unsigned int cp);

/* A UTF-16 body (a byte order mark FF FE or FE FF, else a charset label naming UTF-16, when there is no UTF-8
   byte order mark): 1 and *out, the body in UTF-8 (malloc'ed, NUL-terminated, *outlen bytes; surrogate pairs
   joined, a lone surrogate or an odd last byte U+FFFD); 0 when the body is not UTF-16; -1 when memory is out. */
int ut_html_utf16(const unsigned char *in, size_t len, const char *label, unsigned char **out, size_t *outlen);

/* html.h's order: a byte order mark (*start becomes 3, past it), the response's charset label, a <meta> in the
   first 1024 bytes (html only), then UTF-8 if the bytes are UTF-8 and windows-1252 if not: UT_CS_UTF8,
   UT_CS_1252, or UT_CS_ASCII with the name to give in a notice in unknown (cap bytes). */
int ut_html_charset(const unsigned char *in, size_t len, const char *label, int html, size_t *start,
                    char *unknown, size_t cap);

/* Does a <meta> in the first 1024 bytes name a charset? (11/1/26: a form's bytes depend on whether anything said one) */
int ut_html_declared(const unsigned char *in, size_t len);

/* No Content-Type: a body is HTML when it starts like HTML (the MIME sniffing standard's list) */
int ut_html_sniff(const unsigned char *in, size_t len);

/* The character at in[*pos] (before end) in charset cs; *pos moves past it. CR LF and CR are LF. */
unsigned int ut_html_char(const unsigned char *in, int cs, size_t *pos, size_t end);

/* A character reference at in[*pos], which is '&' (before end): its character, with *pos past it; or '&' alone,
   with *pos past the '&' only, when it is none (unknown names stay as written). attr: in an attribute's value,
   where a legacy name without its semicolon counts only when neither '=' nor a letter or digit follows. */
unsigned int ut_html_ref(const unsigned char *in, size_t *pos, size_t end, int attr);

/* The tag whose name starts at in[q] (after "<" or "</": end), with the attributes t asks for: 1 with *next just
   past its '>', or 0 when the body ends inside it (an unclosed quote too: *next is len). */
int ut_html_tag(const unsigned char *in, size_t len, int cs, size_t q, int end, struct ut_html_tag *t, size_t *next);

/* Where raw text that starts at in[pos] ends: at "</name" followed by whitespace, '/' or '>' (len when never);
   a script's with the standard's escaped and double escaped states (linear) */
size_t ut_html_raw_end(const unsigned char *in, size_t len, size_t pos, const char *name);
size_t ut_html_script_end(const unsigned char *in, size_t len, size_t pos);

/* Just past a comment that starts at in[q], after "<!--": at "-->" or "--!>"; "<!-->" and "<!--->" are empty
   ones; an unterminated one is the rest of the body (len) */
size_t ut_html_comment_end(const unsigned char *in, size_t len, size_t q);

/* Just past the next end (a string) at or after in[q]; len when there is none */
size_t ut_html_skip_past(const unsigned char *in, size_t len, size_t q, const char *end);

#endif
