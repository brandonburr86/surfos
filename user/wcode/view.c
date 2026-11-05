/*
SurfOS user program: wcode
--------------------
File: view.c    Date: 11/05/26
--------------------
The text area's cells (view.h). A screenful is tokenized from the state its first line starts in
(syntax.c, within the view's budget) and each row's characters are put in cells from the first
column shown: a tab fills the cells to the next multiple of 8, any other character one, and a
row stops at the area's edge. A selection that takes a line's end shows one selected cell past
the line's text, so that a selection of empty lines can be seen.
*/
#include <stdio.h>
#include <string.h>
#include "view.h"

static unsigned char row_cls[WC_LEX_CLASSES];

void wc_view_init(struct wc_view *v) {
    memset(v, 0, sizeof(*v));
    v->budget = 512u << 10;
    v->br_a = v->br_b = WC_NONE;
}

void wc_view_follow(struct wc_doc *d, unsigned rows, unsigned cols) {
    unsigned line = wc_doc_line(d), col = wc_doc_col(d);
    if(!rows) rows = 1;
    if(!cols) cols = 1;
    if(line < d->top) d->top = line;
    else if(line >= d->top + rows) d->top = line - rows + 1;
    if(col < d->left) d->left = col > cols / 4 ? col - cols / 4 : 0;     /* a jump sideways shows some of what is there */
    else if(col >= d->left + cols) d->left = col - cols + 1 + cols / 4;
    if(d->left > col) d->left = col;
}

void wc_view_scroll(struct wc_doc *d, int rows, int cols, unsigned page) {
    long top = (long)d->top + rows, left = (long)d->left + cols;
    (void)page;
    if(top < 0) top = 0;
    if(top > (long)d->t.nl) top = d->t.nl;
    if(left < 0) left = 0;
    if(left > 1L << 24) left = 1L << 24;
    d->top = (unsigned)top;
    d->left = (unsigned)left;
}

void wc_view_render(struct wc_doc *d, struct wc_view *v, struct wc_cell *cells, unsigned rows, unsigned cols,
                    unsigned *lines) {
    struct wc_text *t = &d->t;
    struct wc_line l;
    struct wc_cell *row;
    unsigned from, to, r, k, pos, col, next, cp, n, st, line = d->top, end;
    int exact = 1, more = line <= t->nl;
    wc_doc_sel(d, &from, &to);
    if(cols > WC_ROW_CELLS) cols = WC_ROW_CELLS;
    v->br_a = v->br_b = WC_NONE;
    if(!wc_syn_match(t, d->lang, d->caret, &v->br_a, &v->br_b, 2000, 256u << 10) && v->br_a != WC_NONE)
        v->br_b = WC_NONE;                      /* a bracket with no partner near: shown so */
    st = more ? wc_syn_state(t, d->lang, line, v->budget, &exact) : 0;
    pos = more ? wc_text_line_start(t, line) : t->len;
    if(lines) *lines = 0;
    for(r = 0; r < rows; r++) {
        row = cells + (size_t)r * cols;
        for(k = 0; k < cols; k++) {
            row[k].cp = more ? ' ' : 0;
            row[k].cls = WC_C_TEXT;
            row[k].bg = WC_BG_NONE;
        }
        if(!more) continue;
        if(lines) (*lines)++;
        wc_text_line(t, pos, &l);
        st = wc_syn_classes(t, d->lang, &l, st, row_cls, sizeof(row_cls));
        end = d->left + cols;
        for(k = l.start, col = 0; k < l.end && col < end; k += n, col = next) {
            n = wc_text_char(t, k, &cp);
            next = cp == '\t' ? (col / WC_TABW + 1) * WC_TABW : col + 1;
            for(; col < next && col < end; col++) {
                if(col < d->left) continue;
                row[col - d->left].cp = cp == '\t' ? ' ' : cp;
                row[col - d->left].cls = k - l.start < sizeof(row_cls) ? row_cls[k - l.start] : WC_C_TEXT;
                if(k >= from && k < to) row[col - d->left].bg = WC_BG_SEL;
                else if(k == v->br_a || k == v->br_b) row[col - d->left].bg = v->br_b == WC_NONE ? WC_BG_NOMATCH : WC_BG_BRACKET;
            }
        }
        if(k >= l.end && col >= d->left && col < end && to > l.end && from <= l.end && l.eol)
            row[col - d->left].bg = WC_BG_SEL;  /* the line end is selected */
        if(!l.eol) more = 0;
        pos = l.end + l.eol;
    }
    v->exact = exact;
}

unsigned wc_view_hit(struct wc_doc *d, unsigned row, unsigned col) {
    struct wc_line l;
    unsigned line = d->top + row;
    if(line > d->t.nl || line < d->top) return d->t.len;
    wc_text_line(&d->t, wc_text_line_start(&d->t, line), &l);
    return wc_text_at_col(&d->t, &l, d->left + col, WC_TABW);
}

int wc_view_caret(struct wc_doc *d, unsigned rows, unsigned cols, unsigned *row, unsigned *col) {
    unsigned line = wc_doc_line(d), c = wc_doc_col(d);
    if(line < d->top || line >= d->top + rows || c < d->left || c >= d->left + cols) return 0;
    *row = line - d->top;
    *col = c - d->left;
    return 1;
}

unsigned wc_view_digits(const struct wc_doc *d) {
    unsigned n = d->t.nl + 1, k = 1;
    while(n >= 10) {
        n /= 10;
        k++;
    }
    return k;
}

unsigned wc_view_width(struct wc_doc *d, unsigned rows) {
    struct wc_line l;
    unsigned r, pos, w = 0, c;
    if(d->top > d->t.nl) return 0;
    pos = wc_text_line_start(&d->t, d->top);
    for(r = 0; r < rows; r++) {
        wc_text_line(&d->t, pos, &l);
        c = l.end - l.start > (1u << 20) ? l.end - l.start : wc_text_col(&d->t, l.start, l.end, WC_TABW);
        if(c > w) w = c;
        if(!l.eol) break;
        pos = l.end + l.eol;
    }
    return w;
}

void wc_view_status(struct wc_doc *d, char *out, unsigned size) {
    static const char *const eols[] = { "LF", "LF", "CRLF", "Mixed" };
    char sel[40] = "", ind[24];
    unsigned from, to, n = wc_doc_sel(d, &from, &to), lines;
    if(n) {
        lines = wc_text_count_nl(&d->t, from, to);
        if(lines) snprintf(sel, sizeof(sel), "  %u selected (%u lines)", n, lines + (to > from && wc_text_line_begin(&d->t, to) != to));
        else snprintf(sel, sizeof(sel), "  %u selected", n);
    }
    if(d->indent_tab) snprintf(ind, sizeof(ind), "Tabs");
    else snprintf(ind, sizeof(ind), "Spaces: %d", d->indent);
    snprintf(out, size, "Ln %u, Col %u%s  %s  %s%s  %s", wc_doc_line(d) + 1, wc_doc_col(d) + 1, sel,
             wc_lang_name(d->lang), d->eols == WC_EOL_MIXED ? (d->crlf ? "Mixed (CRLF)" : "Mixed (LF)") :
             eols[d->eols], "", ind);
}
