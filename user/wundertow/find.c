/*
SurfOS Undertow's window: finding words on a page
----------------------
File: find.c  Date: 11/1/26
----------------------
find.h: a box at a time, each stretch of its text runs that follow one another in the document's text. No index and no
allocation: a search goes over the text once, which a person's keys can afford.
*/

#include <stddef.h>
#include "doc.h"
#include "find.h"

static int low(char c) { return c >= 'A' && c <= 'Z' ? c + 32 : (unsigned char)c; }

static int here(const char *s, const char *word, size_t n) {
    size_t i;
    for(i = 0; i < n; i++) if(low(s[i]) != low(word[i])) return 0;
    return 1;
}

struct span {                           /* the walk over the document's stretches of text */
    const struct wd_document *d;
    size_t box;
    unsigned int run;
};

/* The next stretch: its text is [*start, *stop); 0 when there are no more */
static int next_span(struct span *w, size_t *start, size_t *stop) {
    const struct wd_document *d = w->d;
    while(w->box < d->block_count) {
        const struct wd_block *b = &d->blocks[w->box];
        unsigned int end = b->run + b->nruns;
        if(w->run < b->run) w->run = b->run;
        while(w->run < end && d->runs[w->run].kind != WR_TEXT) w->run++;
        if(w->run >= end) {
            w->box++;
            w->run = 0;
            continue;
        }
        *start = d->runs[w->run].off;
        *stop = *start + d->runs[w->run].len;
        for(w->run++; w->run < end && d->runs[w->run].kind == WR_TEXT && d->runs[w->run].off == *stop; w->run++)
            *stop += d->runs[w->run].len;
        return 1;
    }
    return 0;
}

int fi_search(const struct wd_document *d, const char *word, size_t n, size_t from, int back, size_t *at) {
    struct span w;
    size_t start, stop, i;
    int found = 0;
    w.d = d;
    w.box = 0;
    w.run = 0;
    if(!n) return 0;
    while(next_span(&w, &start, &stop)) {
        if(stop - start < n) continue;
        for(i = start; i + n <= stop; i++) {
            if(back ? i >= from : i < from) continue;
            if(!here(d->text + i, word, n)) continue;
            if(!found || (back ? i > *at : i < *at)) *at = i;
            found = 1;
            if(!back) break;                    /* the first of this stretch */
        }
    }
    return found;
}

int fi_count(const struct wd_document *d, const char *word, size_t n, size_t at, int *index) {
    struct span w;
    size_t start, stop, i;
    int total = 0, before = 0;
    w.d = d;
    w.box = 0;
    w.run = 0;
    *index = 0;
    if(!n) return 0;
    while(next_span(&w, &start, &stop)) {
        if(stop - start < n) continue;
        for(i = start; i + n <= stop; i++)
            if(here(d->text + i, word, n)) {
                total++;
                if(i < at) before++;
                else if(i == at) *index = before + 1;
            }
    }
    return total;
}
