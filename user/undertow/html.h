/*
SurfOS Undertow: HTML (and plain text) as readable text with numbered links
----------------------
File: html.h  Date: 10/11/26
----------------------
A response becomes a document: its text in UTF-8, one paragraph after another,
with every link's number after its label ("More information...[1]"), plus the
title, the links' addresses, the fragment targets (id= and <a name=>) and the
spans a colour terminal may show differently. The text is safe to print: no
character of ut_unsafe_char() is in it, except the line breaks; there are no
tabs, no trailing spaces, no two blank lines in a row and none at either end.
*/

#ifndef UNDERTOW_HTML_H
#define UNDERTOW_HTML_H

#include <stddef.h>
#include "http.h"

struct ut_link {
    char *label;                /* safe text, one line, at most UT_FIELD_MAX bytes; may be "" */
    char *url;                  /* usable: the absolute address (ut_url.text, fragment included);
                                   otherwise the reference as written, as safe text */
    int usable;                 /* 1: an http: or https: address ut_url_resolve() accepted */
};

struct ut_anchor {
    char *name;                 /* the id or name, as written (at most UT_FIELD_MAX bytes) */
    size_t offset;              /* where its element's text starts in text */
};

#define UT_SPAN_HEADING 1       /* the text of h1-h6 */
#define UT_SPAN_LINK    2       /* a link's "[N]" */
#define UT_SPANS_MAX    4096u   /* more are dropped: they are only looks */

struct ut_span {
    size_t offset, len;         /* bytes of text */
    int kind;
};

#define UT_TRUNC_TEXT    1      /* the text stopped at UT_TEXT_MAX (a notice says so at its end) */
#define UT_TRUNC_LINKS   2      /* links after UT_LINKS_MAX, or past UT_LINK_BYTES_MAX, have no number */
#define UT_TRUNC_ANCHORS 4      /* fragment targets after UT_ANCHORS_MAX, or past UT_LINK_BYTES_MAX, are not known */

struct ut_document {
    char *text;                 /* text_len bytes and a NUL */
    size_t text_len;
    char *title;                /* safe text, one line, at most UT_FIELD_MAX bytes; "" when there is none */
    struct ut_link *links;      /* link [N] is links[N - 1] */
    size_t link_count;
    struct ut_anchor *anchors;  /* in the order of the text; for a name given twice the first counts */
    size_t anchor_count;
    struct ut_span *spans;
    size_t span_count;
    size_t bytes;               /* everything this document owns, allocation by allocation */
    int truncated;              /* UT_TRUNC_* */
    char notice[160];           /* safe text for the status lines ("the page says it is in
                                   Shift_JIS; only its ASCII is shown"), "" when there is nothing */
};

/* Render r: text/html and application/xhtml+xml as HTML, any other text as it is. Links
   resolve against the first <base href> that resolves, wherever it is in the page, or else
   r->url. The charset is a byte order mark's (UTF-8, UTF-16LE or UTF-16BE), else r->charset,
   else a <meta charset> (or http-equiv Content-Type) in the first 1024 bytes, else UTF-8 when
   the bytes are valid UTF-8, else windows-1252 (iso-8859-1 and us-ascii are read as
   windows-1252 too, as browsers do); a charset Undertow cannot read gives its ASCII and a
   notice. 0 and *out (free it with ut_document_free()), or -1 with a message and *out
   unchanged (out of memory). */
int ut_render(const struct ut_response *r, struct ut_document *out, char *err, size_t cap);

/* Free everything a document owns and zero it; a zeroed document, or NULL, is fine. */
void ut_document_free(struct ut_document *d);

/* The text offset of the fragment target name: compared exactly as written first, then once
   with the fragment's %XX escapes decoded (so a fragment is decoded once in all; callers pass
   it as the URL has it); "" and "top" are the start of the text. 0 and *offset, or -1 when
   the page has none. */
int ut_anchor_offset(const struct ut_document *d, const char *fragment, size_t *offset);

#endif
