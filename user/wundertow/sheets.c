/*
SurfOS Undertow's window: a page's style sheets from the network
----------------------
File: sheets.c  Date: 11/01/26
----------------------
sheets.h's calls. A page with sheets on their way has a struct ws_work; with its last answer the page is built again,
or stays as it is when none came, and the work goes.
*/

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "undertow.h"
#include "url.h"
#include "http.h"
#include "doc.h"
#include "layout.h"
#include "browser.h"
#include "sheets.h"

struct ws_work {
    struct ut_response r;               /* the page's response, its body its own */
    size_t asked;                       /* of doc.sheets, how many have been through wb_fetch() */
    int waiting;                        /* answers to come */
    int imports;                        /* the sheets' @imports were looked for */
    size_t held;                        /* the bytes of the sheets that came */
};

void ws_keep(struct wb_page *p, const struct ut_response *r) {
    struct ws_work *w;
    if(!p->doc.sheets || !p->doc.sheets->count) return;
    w = calloc(1, sizeof(*w));
    if(!w) return;
    w->r = *r;
    w->r.body = malloc(r->len + 1);
    if(!w->r.body) {
        free(w);
        return;
    }
    if(r->len) memcpy(w->r.body, r->body, r->len);
    w->r.body[r->len] = 0;
    p->style = w;
    p->bytes += sizeof(*w) + r->len;
}

void ws_free(struct wb_page *p) {
    struct ws_work *w = p->style;
    if(!w) return;
    p->bytes -= sizeof(*w) + w->r.len + w->held;
    free(w->r.body);
    free(w);
    p->style = NULL;
}

/* Is the page where its fragment target put it? It is laid out, and nobody has moved it since. */
static int at_target(const struct wb *b, const struct wb_page *p, const char *fragment) {
    int ch = b->h - WB_TOOLBAR - WB_STATUS, most = p->lay.height > ch ? p->lay.height - ch : 0, top;
    size_t off;
    if(!p->lay_w || !fragment[0] || wd_anchor_offset(&p->doc, fragment, &off)) return 0;
    top = wl_offset_y(&p->lay, off) - 4;
    return p->scroll == (top < 0 ? 0 : top > most ? most : top);
}

/* What the page has of the sheets is all it will get: built again with them, if there is any */
static void restyle(struct wb *b, struct wb_page *p) {
    struct ws_work *w = p->style;
    struct wd_sheets *q = p->doc.sheets;
    struct wd_document d;
    char err[UT_ERR_MAX];
    size_t i, old = p->doc.bytes;
    int there = at_target(b, p, w->r.url.fragment);
    for(i = 0; i < q->count && !q->sheet[i].text; i++) ;
    if(i == q->count) ;
    else if(!wd_build_with(&w->r, q, &d, err, sizeof(err))) {
        wd_free(&p->doc);
        p->doc = d;
        p->bytes += d.bytes - old;
        wl_free(&p->lay);
        p->lay_w = 0;                           /* laid out again, at the same place; a fragment target at its own */
        if(there) snprintf(p->fragment, sizeof(p->fragment), "%s", w->r.url.fragment);
    } else for(i = 0; i < q->count; i++) {      /* (without memory, what came is given back) */
        free(q->sheet[i].text);
        q->sheet[i].text = NULL;
    }
    ws_free(p);
}

/* Every address goes through wb_fetch()'s rules. An @import is a sheet's word, and a sheet never gets at a file, on a
   file page neither: what a page's own <link> names may be one, what a sheet names may not. */
static void ask(struct wb *b, struct wb_page *p) {
    static const char file_host[] = "https://" WB_FILE_HOST "/";
    struct ws_work *w = p->style;
    const struct wd_sheets *q = p->doc.sheets;
    for(; w->asked < q->count; w->asked++) {
        const struct wd_sheet *o = &q->sheet[w->asked];
        if(o->imported && !strncmp(o->url, file_host, sizeof(file_host) - 1)) continue;
        if(!wb_fetch(b, p, o->url, (int)w->asked)) w->waiting++;
    }
}

/* Nothing is on its way: one more round for the @imports, once, and then the page */
static void settle(struct wb *b, struct wb_page *p) {
    struct ws_work *w = p->style;
    while(!w->waiting) {
        if(w->imports) {
            restyle(b, p);
            return;
        }
        w->imports = 1;
        wd_sheets_imports(p->doc.sheets);       /* (without memory, the sheets that came are what there is) */
        ask(b, p);
    }
}

void ws_ask(struct wb *b, struct wb_page *p) {
    if(!p->style) return;
    ask(b, p);
    settle(b, p);
}

void ws_stop(struct wb *b, struct wb_page *p) {
    if(p->style) restyle(b, p);
}

void ws_answer(struct wb *b, struct wb_page *p, int tag, const char *body, size_t len, const char *type) {
    struct ws_work *w = p->style;
    struct wd_sheets *q = p->doc.sheets;
    if(!w || tag < 0 || (size_t)tag >= q->count || q->sheet[tag].text) return;
    if(body && (p->file || !type[0] || !strcmp(type, "text/css"))) {
        char *t = malloc(len + 1);
        if(t) {
            memcpy(t, body, len);
            t[len] = 0;
            q->sheet[tag].text = t;
            q->sheet[tag].len = len;
            w->held += len;
            p->bytes += len;
        }
    }
    if(w->waiting) w->waiting--;
    settle(b, p);
}
