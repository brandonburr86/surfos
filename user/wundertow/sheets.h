/*
SurfOS Undertow's window: a page's style sheets from the network
----------------------
File: sheets.h  Date: 11/01/26
----------------------
Stage 3.3: what wd_build() lists of a page (doc.h's wd_sheets) loaded by the helpers (loader.h), and the page built again
with them. A page shows at once, as it is without them; once the last sheet has answered it is built again from its
response, kept until then, and laid out again where it was. So a sheet that is slow, or never comes, holds nothing up,
and one that fails is left out. Every address goes through the browser's wb_fetch() and its rules, may_load(): an
https page loads nothing over http, an http(s) page reads no file. A sheet's @imports are looked for when the
page's sheets have answered, one level: wd_sheets_imports(). An @import is never a file, on a file page neither: only
a file page's own <link> reaches one. A sheet is taken as text/css, or from a file page by its place, or with no
type at all: an error page is no sheet. @import's base is the address asked for: wb_text() is not told of a redirect.
*/

#ifndef WUNDERTOW_SHEETS_H
#define WUNDERTOW_SHEETS_H

#include "browser.h"

/* A page that was just made from r: what it needs of r is kept when it lists sheets. Without memory it goes on as it is. */
void ws_keep(struct wb_page *p, const struct ut_response *r);

/* The page is the browser's now: its sheets are asked for */
void ws_ask(struct wb *b, struct wb_page *p);

/* The answer to wb_fetch()'s tag: body and its length, or NULL when it failed; type its media type */
void ws_answer(struct wb *b, struct wb_page *p, int tag, const char *body, size_t len, const char *type);

/* The page goes */
void ws_free(struct wb_page *p);

#endif
