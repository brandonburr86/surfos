/*
SurfOS user program: wcode
--------------------
File: find.h    Date: 11/05/26
--------------------
Find and replace in the Code Editor (find.c): text as it is, or a regular expression (libsurf's,
POSIX extended syntax, user/include/regex.h), either case or only the same, and whole words only
if asked. A match never spans lines and is never empty (a pattern that matches nothing at a
place, such as x*, finds only where it matches something). Find next goes on from the selection's
end and Find previous back from its start, round past either end of the file.

A replacement is the text as it is; with a regular expression, & or \0 is the match, \1 to \9 its
groups, \n a line end (the file's kind), \t a tab, and a backslash takes the character after it
(sed's way, POSIX). Replace replaces the selection when it is a match and finds the next; Replace
all replaces every match in the file as one step of the history.
*/
#ifndef _WCODE_FIND_H
#define _WCODE_FIND_H

#include <regex.h>
#include "doc.h"

#define WC_FIND_MAX 256                 /* what to find, and what to put instead, with the NUL */

struct wc_find {
    char what[WC_FIND_MAX];
    char with[WC_FIND_MAX];
    int icase, word, regex;
    char err[96];                       /* why a pattern is none */
    /* the compiled pattern, for what it was compiled from */
    regex_t re;
    int compiled;
    char cwhat[WC_FIND_MAX];
    int cicase;
};

void wc_find_init(struct wc_find *f);
void wc_find_free(struct wc_find *f);
/* the next match at or after from (backward: the last that starts before it), round the end: 1 with [*a, *b) and
   *wrapped when it came from the other end; 0 for none; -1 for a pattern that is none (f->err) */
int wc_find_next(struct wc_find *f, struct wc_text *t, unsigned from, int backward, unsigned *a, unsigned *b,
                 int *wrapped);
/* Find next or previous on the document: the match selected; its result as above, and d->msg says it */
int wc_find_doc(struct wc_find *f, struct wc_doc *d, int backward);
int wc_find_replace(struct wc_find *f, struct wc_doc *d);         /* 1 replaced, 0 not; -1 a bad pattern */
/* for find in files (grep.c): the pattern made ready (0, or -1 with f->err), then a match in the line s[0..n) at or
   after from: 1 with [*a, *b) */
int wc_find_prepare(struct wc_find *f);
int wc_find_in(struct wc_find *f, const char *s, unsigned n, unsigned from, unsigned *a, unsigned *b);
int wc_find_replace_all(struct wc_find *f, struct wc_doc *d);     /* how many, or -1 */

#endif
