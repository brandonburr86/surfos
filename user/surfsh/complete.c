/*
SurfOS shell: surfsh
--------------------
File: complete.c  Date: 10/6/26
--------------------
Lists of names: help shows the programs in /initrd/bin in columns that fit the
terminal, sorted down the columns as ls does.
*/

#include "surfsh.h"

void cands_free(struct cands *c) {
    int i;
    for(i = 0; i < c->n; i++) free(c->v[i].name);
    free(c->v);
    c->v = NULL;
    c->n = c->cap = 0;
}

void columns(char *const *names, int n, int width, const char *indent, const char *eol,
             void (*put)(const char *s, int len)) {
    int i, r, k, maxw = 0, colw, ncols, nrows, ind = (int)strlen(indent);
    for(i = 0; i < n; i++) {
        int w = display_cols(names[i], (int)strlen(names[i]));
        if(w > maxw) maxw = w;
    }
    colw = maxw + 2;
    /* the last column never reaches the edge of the terminal */
    ncols = width - 1 - ind - maxw >= 0 ? (width - 1 - ind - maxw) / colw + 1 : 1;
    nrows = (n + ncols - 1) / ncols;
    for(r = 0; r < nrows; r++) {
        put(indent, ind);
        for(k = 0; k < ncols; k++) {
            int at = k * nrows + r, pad;
            if(at >= n) break;
            put(names[at], (int)strlen(names[at]));
            if(at + nrows >= n) break;          /* the last in this row */
            for(pad = colw - display_cols(names[at], (int)strlen(names[at])); pad > 0; pad--) put(" ", 1);
        }
        put(eol, (int)strlen(eol));
    }
}
