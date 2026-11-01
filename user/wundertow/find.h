/*
SurfOS Undertow's window: finding words on a page
----------------------
File: find.h  Date: 11/1/26
----------------------
Words in a document's text, as Undertow's find takes them: ASCII letters in either case, everything else as it is. A
match lies in the text of one box and goes over its styles (<b>or</b>d is a word); it does not go from one box into the
next, nor into what is no text on the page (alt text, a field's label). Offsets are the document's text's.
*/

#ifndef WUNDERTOW_FIND_H
#define WUNDERTOW_FIND_H

#include <stddef.h>
#include "doc.h"

/* The first match at or after from, or the last one before from when back: 1 and *at, or 0 */
int fi_search(const struct wd_document *d, const char *word, size_t n, size_t from, int back, size_t *at);

/* How many matches there are, and which of them (from 1) is the one at at */
int fi_count(const struct wd_document *d, const char *word, size_t n, size_t at, int *index);

#endif
