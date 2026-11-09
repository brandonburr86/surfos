/*
SurfOS user program: wcode
--------------------
File: find.h    Date: 11/05/26
--------------------
Find and replace in the Code Editor (find.c): text as it is, or a regular expression (libsurf's,
POSIX extended syntax, user/include/regex.h), either case or only the same, and whole words only
if asked. A match never spans lines and is never empty: a pattern that matches an empty text (x*,
^, $) is refused (11/09/26), and one that matches nothing at a place (\<) finds only where it
matches something. Find next goes on from the selection's end and Find previous back from its
start, round past either end of the file.
A replacement is the text as it is; with a regular expression, & or \0 is the match, \1 to \9 its
groups, \n a line end (the file's kind), \t a tab, and a backslash takes the character after it
(sed's way, POSIX). Replace replaces the selection when it is a match and finds the next; Replace
all replaces every match in the file as one step of the history, or none of them.
11/09/26 (the review, C3): a search's work is counted, a regexec() charged the text it may read
(from where it starts to the line's end), a plain search what it reads; with a budget, a search
stops where it is when the budget is spent and goes on from there at the next call, so that the
window answers meanwhile and Esc stops it (wc_find_begin() and wc_find_go(), Replace all's
wc_find_all_begin() and wc_find_all_go()).
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
    /* 11/09/26: the work done (bytes of text a search may have read), where a search stops (0: nowhere), and where
       in its line it goes on from then */
    unsigned long work, limit;
    unsigned resume;
    /* a search of a text going on (wc_find_begin()): forward or back, round the end yet, the position and line it
       began at, the line it is in and where in that line; backward, the last match found in that line so far */
    int job, back, phase, have;
    unsigned start, first, ls, at, ma, mb;
    /* Replace all going on (wc_find_all_begin()): counting, then changing; what it comes to, and the line
       being built */
    int all;
    unsigned count, changed, maxline, linelen, lines;
    long grow, maxgrow;
    int began;
    char *ob;
    unsigned on, ocap;
};

#define WC_FIND_STEP (1u << 20)         /* a search's work at a time between the window's events */

void wc_find_init(struct wc_find *f);
void wc_find_free(struct wc_find *f);
/* the next match at or after from (backward: the last that starts before it), round the end: 1 with [*a, *b) and
   *wrapped when it came from the other end; 0 for none; -1 for a pattern that is none (f->err), or one that took too
   long (a back-reference: f->err) */
int wc_find_next(struct wc_find *f, struct wc_text *t, unsigned from, int backward, unsigned *a, unsigned *b,
                 int *wrapped);
/* the same in steps: wc_find_begin() (0, or -1 with f->err), then wc_find_go() with about budget bytes of work at a
   time (0: all of it) until it gives 1 or 0 (or -1) rather than 2 (more to do); wc_find_stop() ends it */
int wc_find_begin(struct wc_find *f, struct wc_text *t, unsigned from, int backward);
int wc_find_go(struct wc_find *f, struct wc_text *t, unsigned long budget, unsigned *a, unsigned *b, int *wrapped);
/* Find next or previous on the document, from its selection: the match selected and d->msg saying what came of it;
   wc_find_doc() all at once (1, 0, -1), or begun (0, -1) and then gone on with (as wc_find_go()) */
int wc_find_doc(struct wc_find *f, struct wc_doc *d, int backward);
int wc_find_doc_begin(struct wc_find *f, struct wc_doc *d, int backward);
int wc_find_doc_go(struct wc_find *f, struct wc_doc *d, unsigned long budget);
/* Replace: the selection, when it is a match (1 replaced; 0 not a match; -1 a bad pattern, -2 a match that could not
   be replaced: d->msg); wc_find_replace() then finds the next at once, but after -2 */
int wc_find_replace_sel(struct wc_find *f, struct wc_doc *d);
int wc_find_replace(struct wc_find *f, struct wc_doc *d);         /* 1 replaced, 0 not; -1 a bad pattern */
/* for find in files (grep.c): the pattern made ready (0, or -1 with f->err), then a match in the line s[0..n) at or
   after from: 1 with [*a, *b), 0 none; -1 when f->limit came first, -2 when the pattern took too long */
int wc_find_prepare(struct wc_find *f);
int wc_find_in(struct wc_find *f, const char *s, unsigned n, unsigned from, unsigned *a, unsigned *b);
/* Replace all: how many, or -1 (d->msg says why: nothing was changed); all at once, or begun (0, or -1) and gone on
   with about budget bytes of work at a time, 2 meaning more to do. Stopping it (wc_find_all_stop()) while it counts
   changes nothing; once it changes the file, it takes back what it did (1), or says it cannot (0: a change too large
   to undo, which goes on to its end) */
int wc_find_replace_all(struct wc_find *f, struct wc_doc *d);
int wc_find_all_begin(struct wc_find *f, struct wc_doc *d);
int wc_find_all_go(struct wc_find *f, struct wc_doc *d, unsigned long budget);
int wc_find_all_stop(struct wc_find *f, struct wc_doc *d);
int wc_find_all_changing(const struct wc_find *f);                /* it has begun to change the file */
void wc_find_stop(struct wc_find *f);                             /* the find going on ends */

#endif
