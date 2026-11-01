/*
SurfOS Undertow's window: a page's CSS on its boxes and runs
----------------------
File: docstyle.c  Date: 11/01/26
----------------------
docstyle.h's cascade: the sheets of a page gathered first (collect()), then each element as it opens: its id,
classes and style attribute read again from its tag (this keeps its own attributes out of doc.c's list), the
selectors filed under what it has tried right to left against the open elements, and for each property the
declaration that wins (consider()) put on its frame (apply()). Whole pixels and integers only.
*/

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "undertow.h"
#include "html_scan.h"
#include "css.h"
#include "doc.h"
#include "docstyle.h"

#define CLASSES_MAX 16                  /* classes of an element that count */
#define STYLE_DECLS 64                  /* declarations of a style attribute that count */
#define ATTR_MAX    4096                /* bytes of a class or style attribute: past them a style attribute goes whole */
#define MARGIN_MAX  4096                /* pixels */

enum { A_ID, A_CLASS, A_STYLE, A_HREF, A_COUNT };
static const struct ut_html_attr tag_attrs[A_COUNT] = {
    { "id", UT_FIELD_MAX + 1 }, { "class", ATTR_MAX }, { "style", ATTR_MAX }, { "href", 8 }
};

enum { C_TYPE, C_MEDIA, C_COUNT };
static const struct ut_html_attr sheet_attrs[C_COUNT] = { { "type", 32 }, { "media", 1024 } };

struct frame {
    /* who it is */
    unsigned int name, id;              /* css_type(), css_name(); id 0: none */
    unsigned int cls[CLASSES_MAX];
    unsigned char ncls;
    unsigned char link;                 /* an <a href> */
    /* what the page's CSS says of the text in it, inherited by what opens inside it */
    signed char bold, italic;           /* -1: nothing, HTML's word stands */
    unsigned char add, off, noline;     /* lines it adds (WS_UNDER, WS_STRIKE), its element's own it takes away, a
                                           link's underline it takes away */
    unsigned char align;                /* text-align as WA_*, WA_INHERIT: nothing */
    unsigned short size;                /* the font's size in pixels here, always known */
    unsigned short px;                  /* the size when CSS is what gave it, else 0 */
    unsigned int color;                 /* WD_NONE: nothing, HTML's word stands; WD_TEXT: the page's text colour */
    unsigned int bg;                    /* an inline element's background on its text, WD_NONE: none */
};

/* what the cascade said of the element that opened last that only a box can show */
struct pending {
    unsigned int bg;
    int bg_given, align_given;
    short margin[4], padding[4];
    unsigned char given;
};

struct winner {
    const struct css_decl *d;           /* NULL: nothing yet */
    unsigned int spec;
    unsigned int order;
};

struct ds {
    struct css css;
    struct frame f[DS_DEPTH];
    int n;
    int base;                           /* the page's base font size, in pixels */
    unsigned int work;                  /* steps of matching so far */
    int spent;                          /* past DS_WORK_MAX */
    struct pending box;
    struct ut_html_tag tag;             /* an element's own attributes */
    char id[UT_FIELD_MAX + 2], cls[ATTR_MAX + 1], sty[ATTR_MAX + 1], href[9];
    struct css_decl attr[STYLE_DECLS];
    /* the page's root elements */
    struct pending root[2];
    unsigned int fg[2];
    int hidden[2];                      /* it is display: none */
    int failed;
};

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

/**** the sheets of a page ****/

/* a <style> element's text as the sheet takes it: UTF-8 */
static void add_style(struct ds *s, const unsigned char *in, size_t start, size_t end, int cs) {
    size_t i, o = 0;
    char *u;
    for(i = start; i < end && (cs == UT_CS_UTF8 || in[i] < 0x80); i++) ;
    if(i == end) {
        if(css_add(&s->css, (const char *)in + start, end - start)) s->failed = 1;
        return;
    }
    if(end - start > CSS_BYTES_MAX) {               /* (as css_add() would have it) */
        s->css.truncated = 1;
        return;
    }
    u = malloc(3 * (end - start) + 1);
    if(!u) {
        s->failed = 1;
        return;
    }
    for(i = start; i < end; ) o += ut_utf8_encode(ut_html_char(in, cs, &i, end), (unsigned char *)u + o);
    if(css_add(&s->css, u, o)) s->failed = 1;
    free(u);
}

static int is_css(const struct ut_html_tag *t) {
    size_t i;
    static const char want[] = "text/css";
    if(!t->seen[C_TYPE] || !t->len[C_TYPE]) return 1;
    if(t->len[C_TYPE] != sizeof(want) - 1) return 0;
    for(i = 0; i < sizeof(want) - 1; i++) {
        char c = t->val[C_TYPE][i];
        if((c >= 'A' && c <= 'Z' ? c + 32 : c) != want[i]) return 0;
    }
    return 1;
}

/* Every <style> of the page, as doc.c will read the page: comments and raw text are no elements, what is in a
   <template> is not the page's. That a hidden element's style counts is as in a browser. */
static void collect(struct ds *s, const unsigned char *in, size_t len, int cs) {
    static struct ut_html_tag t;
    static char type[33], media[1025];
    size_t p = 0, next;
    unsigned int templates = 0;
    t.attrs = sheet_attrs;
    t.nattrs = C_COUNT;
    t.val[C_TYPE] = type;
    t.val[C_MEDIA] = media;
    while(p < len && !s->failed) {
        const unsigned char *j = memchr(in + p, '<', len - p);
        unsigned char c;
        if(!j) break;
        p = (size_t)(j - in);
        c = p + 1 < len ? in[p + 1] : 0;
        if(c == '!') {
            if(p + 3 < len && in[p + 2] == '-' && in[p + 3] == '-') p = ut_html_comment_end(in, len, p + 4);
            else p = ut_html_skip_past(in, len, p + 2, ">");
        } else if(c == '?') p = ut_html_skip_past(in, len, p + 2, ">");
        else if(c == '/' && p + 2 < len && (in[p + 2] | 0x20) >= 'a' && (in[p + 2] | 0x20) <= 'z') {
            if(!ut_html_tag(in, len, cs, p + 2, 1, &t, &next)) break;
            p = next;
            if(t.el == E_TEMPLATE && templates) templates--;
        } else if((c | 0x20) >= 'a' && (c | 0x20) <= 'z') {
            if(!ut_html_tag(in, len, cs, p + 1, 0, &t, &next)) break;
            p = next;
            if(t.el == E_TEMPLATE && !t.selfclose) templates++;
            if(ut_html_flags(t.el) & (F_RAW | F_RCDATA)) {
                size_t end = t.el == E_SCRIPT ? ut_html_script_end(in, len, p) : ut_html_raw_end(in, len, p, t.name);
                if(t.el == E_STYLE && !templates && is_css(&t) &&
                   (!t.seen[C_MEDIA] || css_media(t.val[C_MEDIA], t.len[C_MEDIA]))) add_style(s, in, p, end, cs);
                p = end;
            }
        } else p++;
    }
}

/**** selectors against the open elements ****/

/* The compound at parts[*i] (up to its combinator) against frame f; *i past it */
static int compound(struct ds *s, const struct css_part *parts, unsigned int n, unsigned int *i, const struct frame *f) {
    unsigned int k = *i, c;
    int ok = 1;
    s->work++;
    for(; k < n && parts[k].kind < CSS_DESCENDANT; k++) {
        if(!ok) continue;
        switch(parts[k].kind) {
        case CSS_TYPE: ok = parts[k].hash == f->name; break;
        case CSS_ID: ok = parts[k].hash == f->id; break;
        case CSS_LINK: ok = f->link; break;
        case CSS_CLASS:
            ok = 0;
            for(c = 0; c < f->ncls; c++) if(f->cls[c] == parts[k].hash) ok = 1;
            s->work += f->ncls;
            break;
        }
    }
    *i = k;
    return ok;
}

/* Does the selector hold for the top frame? Right to left, in runs of compounds joined by '>', which must hold on
   frames in a row; between runs anything may stand, so the nearest frame a run holds on leaves the most for the rest.
   So it is at worst a selector's parts times the depth, with no backing up. */
static int matches(struct ds *s, const struct css_selector *o) {
    const struct css_part *parts;
    unsigned int n = o->nparts, i = 0;
    int at = s->n - 1, first = 1;
    if(!n) return 1;                                /* '*' */
    parts = s->css.parts + o->first;
    for(;;) {
        unsigned int j = i;
        int k = at, ok = 1;
        for(;;) {
            if(!compound(s, parts, n, &j, &s->f[k])) {
                ok = 0;
                break;
            }
            if(j >= n) return 1;
            if(parts[j].kind != CSS_CHILD) break;
            j++;
            if(--k < 0) return 0;
        }
        if(ok) {
            i = j + 1;                              /* past the descendant combinator */
            at = k - 1;
            first = 0;
        } else {
            if(first) return 0;
            at--;
        }
        if(at < 0) return 0;
    }
}

static void consider(struct winner *w, const struct css_decl *d, unsigned int spec, unsigned int order) {
    struct winner *x = &w[d->prop];
    if(x->d) {
        if(x->d->important != d->important) {
            if(x->d->important) return;
        } else if(spec < x->spec || (spec == x->spec && order < x->order)) return;
    }
    x->d = d;
    x->spec = spec;
    x->order = order;
}

/* The selectors of a bucket for this key */
static void chain(struct ds *s, int i, unsigned int key, struct winner *w) {
    for(; i >= 0 && !s->spent; i = s->css.sels[i].next) {
        const struct css_selector *o = &s->css.sels[i];
        s->work++;
        if(s->work > DS_WORK_MAX) s->spent = 1;
        if(o->key == key && matches(s, o)) {
            unsigned int d;
            for(d = o->decl; d < o->decl + o->ndecl; d++) consider(w, &s->css.decls[d], o->spec, (unsigned int)i);
        }
    }
}

static void gather(struct ds *s, const struct frame *f, struct winner *w) {
    int i;
    if(!s->css.nsels) return;
    chain(s, s->css.any, 0, w);
    chain(s, s->css.head[f->name % CSS_BUCKETS], f->name, w);
    if(f->id) chain(s, s->css.head[f->id % CSS_BUCKETS], f->id, w);
    for(i = 0; i < f->ncls; i++) chain(s, s->css.head[f->cls[i] % CSS_BUCKETS], f->cls[i], w);
}

/**** one element ****/

static void read_classes(struct frame *f, const char *c) {
    while(*c && f->ncls < CLASSES_MAX) {
        size_t n = 0;
        unsigned int h;
        int k, dup = 0;
        while(ws(*c)) c++;
        while(c[n] && !ws(c[n])) n++;
        if(!n) break;
        h = css_name(c, n);
        for(k = 0; k < f->ncls; k++) dup |= f->cls[k] == h;
        if(!dup) f->cls[f->ncls++] = h;
        c += n;
    }
}

/* a margin's or padding's size in pixels, or percent as a negative number; in the 16-bit fields of a box */
static int to_px(const struct css_decl *d, int em, int base) {
    long v;
    switch(d->unit) {
    case CU_PX: v = clamp(d->value, -(MARGIN_MAX << 8), MARGIN_MAX << 8) / 256; break;
    case CU_EM: v = (long)clamp(d->value, -(1 << 20), 1 << 20) * em / 256; break;
    case CU_REM: v = (long)clamp(d->value, -(1 << 20), 1 << 20) * base / 256; break;
    case CU_PCT: return d->value <= 0 ? 0 : -clamp(d->value / 256, 0, 100);
    default: return 0;                              /* auto: 0 here, boxes have no width of their own to centre */
    }
    return clamp((int)v, 0, MARGIN_MAX);            /* what a negative margin would pull in, it leaves */
}

/* a heading's or big's or small's own size against the parent's, in 1/256ths; 0: none */
static int own_ratio(int el) {
    static const unsigned short ratio[] = { 512, 384, 300, 256, 212, 171 };
    if(el >= E_H1 && el <= E_H6) return ratio[el - E_H1];
    return el == E_BIG ? 307 : el == E_SMALL ? 213 : 0;
}

static int px_of(int v) { return clamp((v + 128) / 256, 1, DS_PX_MAX); }

/* (parent and the base are DS_PX_MAX at most, so these stay within 32 bits) */
static void set_size(struct ds *s, struct frame *f, int parent, const struct css_decl *d) {
    int v = clamp(d->value, 0, 1 << 22);
    switch(d->unit) {
    case CU_PX: f->px = (unsigned short)px_of(v); break;
    case CU_EM: f->px = (unsigned short)px_of(parent * v); break;
    case CU_REM: f->px = (unsigned short)px_of(s->base * v); break;
    case CU_PCT: f->px = (unsigned short)px_of(parent * v / 100); break;
    case CU_INHERIT: f->px = (unsigned short)parent; break;
    default: return;
    }
    f->size = f->px;
}

static int display_of(const struct css_decl *d) {
    if(!d || d->unit != CU_NONE) return DS_ASIS;
    switch(d->value) {
    case CSS_D_NONE: return DS_NONE;
    case CSS_D_BLOCK: return DS_BLOCK;
    case CSS_D_INLINE: return DS_INLINE;
    }
    return DS_SHOWN;
}

/* What won goes on the frame, and for a box in s->box */
static int apply(struct ds *s, struct frame *f, const struct frame *p, const struct ds_element *e, const struct winner *w) {
    struct pending *b = &s->box;
    const struct css_decl *d;
    int parent = p ? p->size : s->base, i, r = own_ratio(e->el);
    if(r) {                                         /* an element of a size of its own */
        f->size = (unsigned short)clamp((parent * r + 128) / 256, 1, DS_PX_MAX);
        f->px = p && p->px ? f->size : 0;
    }
    if(e->words & WS_BOLD) f->bold = -1;
    if(e->words & WS_ITALIC) f->italic = -1;
    if(e->words & (WS_UNDER | WS_STRIKE)) f->off &= (unsigned char)~e->words;
    if((e->words & DS_W_COLOR) || f->link) f->color = WD_NONE;
    if(f->link) f->noline = 0;
    if((d = w[CP_SIZE].d) != NULL) set_size(s, f, parent, d);
    if((d = w[CP_COLOR].d) != NULL) {
        if(d->unit == CU_RGB) f->color = (unsigned int)d->value;
        else if(d->unit == CU_INITIAL) f->color = WD_TEXT;
        else if(d->unit == CU_INHERIT) f->color = e->color != WD_NONE ? e->color : WD_TEXT;
    }
    if((d = w[CP_WEIGHT].d) != NULL && d->unit != CU_REVERT)
        f->bold = (signed char)(d->unit == CU_INHERIT ? (e->style & WS_BOLD) != 0 : d->value);
    if((d = w[CP_STYLE].d) != NULL && d->unit != CU_REVERT)
        f->italic = (signed char)(d->unit == CU_INHERIT ? (e->style & WS_ITALIC) != 0 : d->value);
    if((d = w[CP_DECORATION].d) != NULL && d->unit != CU_REVERT) {
        unsigned int lines = (d->value & CSS_LINE_UNDER ? WS_UNDER : 0u) | (d->value & CSS_LINE_THROUGH ? WS_STRIKE : 0u);
        f->add |= (unsigned char)lines;             /* (inherit: none of its own, what is around it comes by itself) */
        f->off |= (unsigned char)(e->words & (WS_UNDER | WS_STRIKE) & ~lines);
        if(f->link) f->noline = !(lines & WS_UNDER);
    }
    if((d = w[CP_ALIGN].d) != NULL && d->unit != CU_REVERT) {
        static const unsigned char aligns[] = { WA_LEFT, WA_CENTER, WA_RIGHT };
        f->align = d->unit == CU_INHERIT ? (p ? p->align : WA_INHERIT) : aligns[clamp(d->value, 0, 2)];
        b->align_given = 1;
    }
    if((d = w[CP_BACKGROUND].d) != NULL && d->unit != CU_REVERT) {
        b->bg = d->unit == CU_RGB ? (unsigned int)d->value : WD_NONE;
        b->bg_given = 1;
        f->bg = b->bg;
    }
    for(i = 0; i < 4; i++) {
        if((d = w[CP_MARGIN_TOP + i].d) != NULL && d->unit != CU_REVERT) {
            b->margin[i] = (short)to_px(d, f->size, s->base);
            b->given |= (unsigned char)(1 << i);
        }
        if((d = w[CP_PADDING_TOP + i].d) != NULL && d->unit != CU_REVERT) {
            b->padding[i] = (short)to_px(d, f->size, s->base);
            b->given |= (unsigned char)(16 << i);
        }
    }
    return display_of(w[CP_DISPLAY].d);
}

/* Who it is, and what it takes from around it */
static void begin(struct ds *s, struct frame *f, const struct frame *p, const struct ds_element *e) {
    if(p) *f = *p;
    else {
        memset(f, 0, sizeof(*f));
        f->bold = f->italic = -1;
        f->size = (unsigned short)s->base;
        f->color = f->bg = WD_NONE;
    }
    f->name = css_type(e->name, strlen(e->name));
    f->id = 0;
    f->ncls = 0;
    f->link = 0;
    memset(&s->box, 0, sizeof(s->box));
    s->box.bg = WD_NONE;
}

int ds_height(const struct ds *s) { return s->n; }

void ds_close(struct ds *s, int height) {
    if(height < 2) height = 2;                      /* html and body stay */
    if(height < s->n) s->n = height;
}

static int open_frame(struct ds *s, int at, const struct ds_element *e) {
    struct frame *f = &s->f[at];
    const struct frame *p = at ? &s->f[at - 1] : NULL;
    struct winner w[CP_COUNT];
    size_t next, k, nattr = 0;
    begin(s, f, p, e);
    s->n = at + 1;
    memset(w, 0, sizeof(w));
    if(e->in && ut_html_tag(e->in, e->len, e->cs, e->q, 0, &s->tag, &next)) {
        if(s->tag.seen[A_ID] && s->tag.len[A_ID] && !s->tag.over[A_ID]) f->id = css_name(s->tag.val[A_ID], s->tag.len[A_ID]);
        if(s->tag.seen[A_CLASS]) read_classes(f, s->tag.val[A_CLASS]);
        f->link = e->el == E_A && s->tag.seen[A_HREF];
        if(s->tag.seen[A_STYLE] && s->tag.len[A_STYLE] && !s->tag.over[A_STYLE])
            nattr = css_style(s->tag.val[A_STYLE], s->tag.len[A_STYLE], s->attr, STYLE_DECLS);
    }
    gather(s, f, w);
    for(k = 0; k < nattr; k++) consider(w, &s->attr[k], 0xffffffffu, (unsigned int)k);
    return apply(s, f, p, e, w);
}

int ds_open(struct ds *s, const struct ds_element *e) {
    if(s->n == DS_DEPTH) {
        memset(&s->box, 0, sizeof(s->box));
        return DS_ASIS;
    }
    return open_frame(s, s->n, e);
}

static void root_frame(struct ds *s, int which, const struct ds_element *e) {
    struct pending *b = &s->root[which];
    s->hidden[which] = open_frame(s, which, e) == DS_NONE;
    *b = s->box;
    s->fg[which] = s->f[which].color;               /* what is said of the page is said of its d->fg */
    s->f[which].color = WD_NONE;
    s->f[which].bg = WD_NONE;
}

void ds_root(struct ds *s, int which, const struct ds_element *e) {
    struct ds_element body;
    root_frame(s, which, e);
    if(which == DS_ROOT_BODY) return;
    memset(&body, 0, sizeof(body));                 /* the body it holds, as it stood */
    body.name = "body";
    body.el = E_BODY;
    body.color = WD_NONE;
    root_frame(s, DS_ROOT_BODY, &body);
}

int ds_hidden(const struct ds *s) { return s->hidden[0] || s->hidden[1]; }

void ds_end(struct ds *s, const char *name, int floor) {
    unsigned int h = css_type(name, strlen(name));
    int i;
    if(floor < 2) floor = 2;
    for(i = s->n - 1; i >= floor; i--)
        if(s->f[i].name == h) {
            s->n = i;
            return;
        }
}

int ds_named(const struct ds *s, int height, const char *name) {
    return height >= 0 && height < s->n && s->f[height].name == css_type(name, strlen(name));
}

void ds_box(struct ds *s, struct wd_block *x, int display) {
    const struct pending *b = &s->box;
    struct frame *f = &s->f[s->n - 1];
    int i;
    f->bg = WD_NONE;                                /* its background is its box's, not its text's */
    if(b->bg_given) x->bg = b->bg;
    if(x->kind != WB_TABLE) {                       /* a table's align is its place, not its text's */
        if(b->align_given) x->align = f->align;
        else if(x->kind == WB_CELL && x->align == WA_INHERIT) x->align = f->align;
    }
    if(x->kind == WB_ITEM && display == DS_BLOCK) x->marker = WMK_NONE;
    if(x->kind == WB_ROW) return;
    for(i = 0; i < 4; i++) {
        if(x->kind != WB_CELL) x->margin[i] = b->margin[i];
        x->pad[i] = b->padding[i];
    }
    x->given = x->kind == WB_CELL ? b->given & 0xf0 : b->given;
}

/**** text ****/

unsigned short ds_style(const struct ds *s, unsigned short style) {
    const struct frame *f = &s->f[s->n - 1];
    if(f->bold >= 0) style = (unsigned short)(f->bold ? style | WS_BOLD : style & ~WS_BOLD);
    if(f->italic >= 0) style = (unsigned short)(f->italic ? style | WS_ITALIC : style & ~WS_ITALIC);
    style = (unsigned short)((style & ~f->off) | f->add);
    return f->noline ? (unsigned short)(style | WS_NOLINE) : style;
}

unsigned int ds_color(const struct ds *s, unsigned int color) {
    const struct frame *f = &s->f[s->n - 1];
    return f->color != WD_NONE ? f->color : color;
}

unsigned int ds_bg(const struct ds *s) { return s->f[s->n - 1].bg; }
unsigned short ds_px(const struct ds *s) { return s->f[s->n - 1].px; }

/**** the page ****/

int ds_finish(struct ds *s, struct wd_document *d) {
    const struct pending *b = &s->root[s->root[1].bg_given ? 1 : 0], *m = &s->root[1];
    struct wd_block *x = &d->blocks[0];
    int i;
    if(b->bg_given) d->bg = b->bg;
    if(s->fg[1] != WD_NONE) d->fg = s->fg[1];
    else if(s->fg[0] != WD_NONE) d->fg = s->fg[0];
    if(d->fg == WD_TEXT) d->fg = WD_NONE;
    for(i = 0; i < 4; i++) {                        /* the body's margin is the page's; its padding adds to it */
        x->margin[i] = m->margin[i];
        x->pad[i] = m->padding[i];
    }
    x->given = m->given;
    if(m->align_given) x->align = s->f[DS_ROOT_BODY].align;
    return s->spent || s->css.truncated;
}

struct ds *ds_new(const unsigned char *in, size_t len, int cs) {
    struct ds *s = calloc(1, sizeof(*s));
    struct ds_element html;
    if(!s) return NULL;
    css_init(&s->css);
    s->base = clamp(wd_base, 8, DS_PX_MAX);
    s->tag.attrs = tag_attrs;
    s->tag.nattrs = A_COUNT;
    s->tag.val[A_ID] = s->id;
    s->tag.val[A_CLASS] = s->cls;
    s->tag.val[A_STYLE] = s->sty;
    s->tag.val[A_HREF] = s->href;
    s->fg[0] = s->fg[1] = WD_NONE;
    collect(s, in, len, cs);
    if(s->failed || s->css.failed) {
        ds_free(s);
        return NULL;
    }
    memset(&html, 0, sizeof(html));
    html.name = "html";
    html.el = E_HTML;
    html.color = WD_NONE;
    ds_root(s, DS_ROOT_HTML, &html);
    return s;
}

void ds_free(struct ds *s) {
    if(!s) return;
    css_free(&s->css);
    free(s);
}
