/*
SurfOS Undertow: the pages read, back and forward, the reading positions, find
----------------------
File: session.h  Date: 10/11/26
----------------------
A session keeps the pages read for back and forward: at most UT_HISTORY_MAX of
them, owning at most UT_HISTORY_BYTES together (their documents' bytes and the
pages' own blocks). The current page is the one on the screen; it is never
evicted. A load fetches and renders into new objects first and changes the
session only once both have worked, so a failed load (or reload) leaves every
page, the current one and the reading positions as they were; the pages after
the current one (forward) are dropped only when a new page has loaded. back and
forward only move between the pages kept: they never fetch. Every page keeps
its own reading position, a byte offset in its text (view.c shows the row that
holds it first, so a position survives a change of the terminal's size).

Only main.c uses this; view.c draws a struct ut_page. The functions follow
undertow.h's conventions: 0 or -1 with a message in err (cap bytes), nothing
changed on failure.
*/

#ifndef UNDERTOW_SESSION_H
#define UNDERTOW_SESSION_H

#include <stddef.h>
#include "undertow.h"
#include "url.h"
#include "config.h"
#include "html.h"

struct ut_page {
    struct ut_url url;          /* where the text came from, after redirects; its fragment is the
                                   part of the page asked for last */
    struct ut_document doc;     /* the text, title, links, anchors, spans */
    int status;                 /* the HTTP status (200, 404, ...) */
    int verified;               /* 1: HTTPS with a verified certificate */
    int redirects;              /* redirects followed to get here */
    char tls[16];               /* "TLSv1.3", "TLSv1.2"; "" for http: */
    char reason[64];            /* the status line's reason phrase, safe text */
    char content_type[64];      /* "text/html", "text/plain", ... */
    size_t position;            /* the reading position: a byte offset in doc.text */
    size_t bytes;               /* what the session counts for this page: this block and doc.bytes */
};

struct ut_session {
    struct ut_page *pages[UT_HISTORY_MAX];  /* the oldest first */
    unsigned int count;                     /* pages kept */
    unsigned int current;                   /* pages[current] is on the screen (when count > 0) */
    size_t bytes;                           /* the pages' bytes together */
    char search[UT_SEARCH_MAX + 1];         /* the text of the last find; "" before the first */
    const struct ut_page *match_page;       /* where find found it last (NULL: nowhere, or a page gone) */
    size_t match_at;                        /* the offset of that match */
    size_t match_position;                  /* the reading position find left (findnext goes on from the
                                               match itself while the reader stays there) */
};

/* An empty session; ut_session_free() gives back everything it holds and leaves it empty. */
void ut_session_init(struct ut_session *s);
void ut_session_free(struct ut_session *s);

/* The page on the screen, NULL before the first one loads. */
struct ut_page *ut_session_current(const struct ut_session *s);

/* Load url as a new page (ut_fetch(), then ut_render()). On success it becomes the current
   page, after the current one: the pages that were after it are dropped, then the oldest
   pages go while there are more than UT_HISTORY_MAX or more than UT_HISTORY_BYTES (never the
   current one). Its reading position is the part url's fragment names, or the top; *found
   (when found is not NULL) is 0 when url has a fragment that names no part of the page, 1
   otherwise. On failure -1 and the message; the session is as it was. */
int ut_session_load(struct ut_session *s, const struct ut_url *url, const struct ut_config *cfg,
                    int *found, char *err, size_t cap);

/* Load the current page's address again and put the new page in its place. The reading
   position stays where it was when the new text is longer than it, else it is the top. On
   failure -1 and the message; the current page, its text and position are as they were. */
int ut_session_reload(struct ut_session *s, const struct ut_config *cfg, char *err, size_t cap);

/* back (-1) and forward (+1): 0, or -1 when there is no page there (nothing changes). */
int ut_session_move(struct ut_session *s, int direction);

/* The address of link number n (1, 2, ...) of the current page: 0 and *out; -1 and why: no
   page, no link with that number (the message says how many there are), or a link Undertow
   cannot open (its reference is named, as safe text). */
int ut_session_link(const struct ut_session *s, unsigned long n, struct ut_url *out, char *err, size_t cap);

/* Is url an address in the current page itself, a fragment of it (only the part after '#'
   differs, and url has one)? Following such a link moves on the page without fetching. */
int ut_session_same_page(const struct ut_session *s, const struct ut_url *url);

/* Move to url's fragment in the current page (ut_session_same_page() said yes): 0, the
   position at that part and the page's address with that fragment; -1 and why when the page
   has no part of that name (nothing changes). */
int ut_session_jump(struct ut_session *s, const struct ut_url *url, char *err, size_t cap);

/* Find text in the current page: ASCII letters in either case, every other byte exactly as it
   is. next 0: text is new (it is kept for findnext) and the search starts at the reading
   position; next 1: text is NULL, the kept text is searched for again, from just after the
   last match while the reader has not moved since, else from the reading position. Either
   way the search goes on from the top once when it reaches the end. On a match the reading
   position is the match: 0, or 1 when the search went round to the top first. -1 and why
   when there is no match (the position stays), no page, or no text to find. */
int ut_session_find(struct ut_session *s, const char *text, int next, char *err, size_t cap);

#endif
