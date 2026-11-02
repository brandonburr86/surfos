/*
SurfOS Undertow's window: a page as a tree of boxes
----------------------
File: doc.c  Date: 10/25/26
----------------------
doc.h's builder, in one pass over the body as html.c reads it, over the same scanner (html_scan.c):

- the open elements on a small stack with html.c's rules for closing them: a block closes an open <p>, an
  item the open item of its list, a cell the open cell, headings do not nest, end tags reach no further than
  the standard's scopes; deeper than STACK_MAX elements are only counted;
- every element on that stack opens a box; inline content goes into the innermost box, or into an anonymous
  one when that box already holds boxes (the CSS rule);
- inline elements are counted, not kept: a run's style is what is open when its text comes (b, i, code, u,
  s, sub, sup, nobr, mark, big, small, the nearest <font color>, the open link). A table's cell starts with
  the style around its table and gives it back at its end, as browsers' formatting elements stop at a cell;
- whitespace collapses into one space between the texts of a box, kept with the style it came in; none starts
  a box or follows a line break; in pre, listing, xmp and plain text it is kept (tabs to the next column of 8);
- what html.c hides, this hides (script, style, template, svg and math, object, ...), and so the content of
  an element with the hidden attribute; links, anchors, the title, the base and the limits are html.c's.
*/

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "undertow.h"
#include "url.h"
#include "html_scan.h"
#include "doc.h"
#include "form.h"
#include "css.h"
#include "docstyle.h"

#define STACK_MAX    128                /* open elements kept; deeper ones are only counted */
#define HREF_MAX     4096               /* bytes of an address kept from an attribute (url.c takes 2048) */
#define AHASH        (2 * UT_ANCHORS_MAX)   /* the anchor names' hash table: never more than half full */
#define COLORS_MAX   16                 /* nested <font color> kept; deeper ones are only counted */
#define LABEL_MAX    UT_FIELD_MAX       /* a field's or a link's label */

static const char cut_notice[] = "the rest of this page is not shown: it is over 512 KB";

int wd_base = 16;

/**** the attributes kept ****/

enum {
    A_HREF, A_ID, A_NAME, A_ALT, A_VALUE, A_TYPE, A_START, A_SRC, A_WIDTH, A_HEIGHT, A_ALIGN, A_COLOR, A_BGCOLOR,
    A_TEXT, A_LINK, A_VLINK, A_BORDER, A_COLSPAN, A_CELLPADDING, A_CHECKED, A_PLACEHOLDER, A_HIDDEN, A_ARIA,
    A_TITLE, A_SELECTED,
    A_ACTION, A_METHOD, A_FORM, A_FORMACTION, A_FORMMETHOD, A_DISABLED, A_READONLY, A_MAXLENGTH, A_ROWS, A_COLS, A_SIZE,
    A_MULTIPLE, A_LABEL, A_ACCEPT, A_DIRNAME, A_WRAP, A_COUNT
};

static const struct ut_html_attr attr_info[A_COUNT] = {
    { "href", HREF_MAX }, { "id", UT_FIELD_MAX + 1 }, { "name", UT_FIELD_MAX + 1 }, { "alt", UT_FIELD_MAX },
    { "value", UT_FIELD_MAX }, { "type", 32 }, { "start", 32 }, { "src", HREF_MAX }, { "width", 32 },
    { "height", 32 }, { "align", 32 }, { "color", 64 }, { "bgcolor", 64 }, { "text", 64 }, { "link", 64 },
    { "vlink", 64 }, { "border", 32 }, { "colspan", 32 }, { "cellpadding", 32 }, { "checked", 32 },
    { "placeholder", UT_FIELD_MAX }, { "hidden", 32 }, { "aria-label", UT_FIELD_MAX }, { "title", UT_FIELD_MAX },
    { "selected", 32 },
    { "action", HREF_MAX }, { "method", 32 }, { "form", UT_FIELD_MAX }, { "formaction", HREF_MAX }, { "formmethod", 32 },
    { "disabled", 32 }, { "readonly", 32 }, { "maxlength", 32 }, { "rows", 32 }, { "cols", 32 }, { "size", 32 },
    { "multiple", 32 }, { "label", UT_FIELD_MAX }, { "accept-charset", 64 }, { "dirname", 32 }, { "wrap", 32 }
};

typedef char attrs_fit[A_COUNT <= UT_HTML_ATTRS_MAX ? 1 : -1];

struct tag_bufs {                       /* the buffers of the tag's attributes: attr_info's cap + 1 each */
    char href[HREF_MAX + 1], id[UT_FIELD_MAX + 2], name[UT_FIELD_MAX + 2], alt[UT_FIELD_MAX + 1],
         value[UT_FIELD_MAX + 1], type[33], start[33], src[HREF_MAX + 1], width[33], height[33], align[33],
         color[65], bgcolor[65], text[65], link[65], vlink[65], border[33], colspan[33], cellpadding[33],
         checked[33], placeholder[UT_FIELD_MAX + 1], hidden[33], aria[UT_FIELD_MAX + 1], title[UT_FIELD_MAX + 1],
         selected[33], action[HREF_MAX + 1], method[33], form[UT_FIELD_MAX + 1], formaction[HREF_MAX + 1], formmethod[33],
         disabled[33], readonly[33], maxlength[33], rows[33], cols[33], size[33], multiple[33], label[UT_FIELD_MAX + 1], accept[65], dirname[33], wrap[33];
};

/**** the builder's state ****/

enum { S_BOLD, S_ITALIC, S_MONO, S_UNDER, S_STRIKE, S_SUB, S_SUP, S_NOWRAP, S_MARK, S_BIG, S_SMALL, S_COUNT };

struct inline_state {                   /* what a cell saves of the style around its table */
    unsigned short sty[S_COUNT];
    int ncolors;
};

struct frame {                          /* an open element that has a box */
    unsigned char el;
    int box;                            /* its box (blocks[]), or the box it is in when it has none */
    long next;                          /* ol: its next item's number */
    unsigned char marker;               /* a list: its items' marker (WMK_*) */
    unsigned char depth;                /* a list: how many lists it is in, itself too */
    unsigned char saved_on;             /* a cell or caption: it saved the inline state */
    struct inline_state saved;
    unsigned char off;                  /* a disabled fieldset */
    unsigned char legend;               /* a legend in a disabled fieldset */
    int css;                            /* the elements docstyle had open before it */
    unsigned char own;                  /* the box is its own */
    unsigned char hides;                /* display: none */
    unsigned char made;                 /* it is here for its CSS: HTML gives this element no box */
};

struct bd {
    const struct ut_response *r;
    const unsigned char *in;
    size_t len, pos;
    int cs, xhtml;
    char charset[UT_HTML_LABEL_MAX];    /* the name of a charset Undertow cannot read */
    unsigned char *decoded;             /* a UTF-16 body as UTF-8 */

    struct wd_document d;               /* what is made: given to the caller at the end */
    size_t text_cap, blocks_cap, runs_cap, links_cap, anchors_cap, images_cap;
    char **hrefs;                       /* each link's address as written, until the end (nhrefs of them) */
    size_t nhrefs, nsrcs;
    unsigned char *long_href;           /* it was longer than HREF_MAX: unusable */
    char **srcs;                        /* each image's address as written */
    unsigned short ahash[AHASH];
    size_t link_bytes;                  /* the anchors' names (the links' strings are counted at the end) */
    int link_bytes_full;

    /* open elements */
    struct frame st[STACK_MAX];         /* st[0] is the root, whose box is blocks[0] */
    int depth;
    unsigned short open[E_COUNT];
    unsigned int over[E_COUNT];
    int lists;                          /* open lists */
    int tables;                         /* open tables */
    int headings;                       /* open headings */

    /* what is hidden */
    int skip_el;                        /* the element whose content is not shown (E_OTHER: none) */
    unsigned int skip_n;                /* how many of it are open */
    int skip_foreign;                   /* that is svg or math: the standard's elements end it */
    int skip_live;                      /* the hidden attribute hid it: its forms are still the page's */
    unsigned int skip_inert;            /* elements open in it whose content is in no form */
    int inert_el;                       /* of which */
    int base_seen;                      /* a <base href> has come: for forms, the first is the only one that counts */
    int skip_off;                       /* in what is hidden, a disabled fieldset or a legend of one, not followed */
    int skip_fields;                    /* fieldsets open in there, from the first disabled one on */
    int legends;                        /* legends of disabled fieldsets open: b->fb.off is not all that says what is in them */
    int undeclared;                     /* HTML, and no response's label for its charset */

    /* inline content */
    int target;                         /* the box that takes inline content now, -1: none yet */
    int space;                          /* a collapsible space waits for the next text */
    unsigned short space_style;         /* with the style it came in */
    unsigned char space_size;
    unsigned int space_color;
    int space_link;
    int pre;                            /* open pre, listing, xmp (or plain text) */
    unsigned int col;                   /* in pre: characters since the line's start (tabs) */
    struct inline_state is;
    unsigned int colors[COLORS_MAX];
    int link;                           /* the open link (links[]), or -1 */
    int link_open;                      /* an <a href> is open, numbered or not */
    char label[LABEL_MAX + 1];          /* the open link's text, collapsed */
    size_t label_len;
    int label_gap;
    char link_alt[LABEL_MAX + 1];       /* its aria-label or title, for a link that shows no text */
    size_t link_alt_len;
    int button;                         /* a <button> is open: its text is its label */
    int select, option;                 /* a <select> is open; option 1: in the option it shows, 2: past it */
    int selected_seen;                  /* that is an option with the selected attribute */
    char flabel[LABEL_MAX + 1];         /* the open button's or select's label */
    size_t flabel_len;
    int flabel_gap;
    int fcontrol;                       /* the open button's or select's control, -1: none */
    int opt_open;                       /* an <option> is open: its text is its label */
    int opt_selected, opt_disabled, group_disabled;
    int opt_has_value;
    int opt_cut;                        /* its value attribute was longer than was kept */
    char opt_value[UT_FIELD_MAX + 1];
    char opt_label[LABEL_MAX + 1];      /* its label attribute */
    char olabel[LABEL_MAX + 1];         /* its text */
    size_t olabel_len;
    int olabel_gap;
    int olabel_cut;
    struct fm_build fb;                 /* the page's forms */

    /* the rest */
    int title_seen;
    struct ut_url *base;                /* the first <base href> that resolved */
    int failed;                         /* out of memory */
    int stopped;                        /* a limit: nothing more is shown */
    struct ut_html_tag tag;
    struct tag_bufs tagbuf;
    char scratch[LABEL_MAX + 16];

    /* the page's CSS (docstyle.c) */
    struct ds *ds;
    size_t tag_q;                       /* where the tag's name starts */
    int none;                           /* open elements that are display: none: what they hold makes nothing */
    int page_none;                      /* one of those is the page's root */
    unsigned char root_seen[2];         /* the tag of <html>, of <body> came */
    int css_box;                        /* the box this tag made, -1: none */
    int css_at;                         /* docstyle's elements before this tag */
    int display;                        /* what the cascade said of this tag's element */
    struct wd_block no_box;             /* what box_of() gives an element without a box of its own */
    unsigned int space_bg;              /* a waiting space's look, from the CSS around it */
    unsigned short space_px;
    int space_now;                      /* that space is what is being written */

    /* 11/02/26 (the integration of milestones 2 and 3): what display: none hides is still the page's forms */
    int select_none;                    /* the <select> that opens has display: none of its own */
    int field_none;                     /* the open select is hidden: its control has no box */
};

/**** small helpers ****/

static int ws(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; }
static int alpha(unsigned char c) { return (c | 0x20) >= 'a' && (c | 0x20) <= 'z'; }
static int digit(unsigned char c) { return c >= '0' && c <= '9'; }
static int alnum(unsigned char c) { return alpha(c) || digit(c); }
static unsigned char lower(unsigned char c) { return c >= 'A' && c <= 'Z' ? (unsigned char)(c + 32) : c; }

/* s (n bytes) is the lowercase word w in any case */
static int ci_eq(const char *s, size_t n, const char *w) {
    size_t i;
    for(i = 0; i < n; i++) if(!w[i] || lower((unsigned char)s[i]) != (unsigned char)w[i]) return 0;
    return !w[n];
}

static int attr_is(const struct ut_html_tag *t, int a, const char *w) {
    return t->seen[a] && ci_eq(t->val[a], t->len[a], w);
}

/* A whole number in an attribute ("12", " 12px", "+3"), kept within a billion: 0 and *out, or -1 for none */
static int parse_int(const char *s, long *out) {
    long v = 0;
    int neg = 0, digits = 0;
    while(ws((unsigned char)*s)) s++;
    if(*s == '-' || *s == '+') neg = *s++ == '-';
    while(digit((unsigned char)*s)) {
        if(v < 100000000L) v = v * 10 + (*s - '0');
        else v = 999999999L;
        s++;
        digits++;
    }
    if(!digits) return -1;
    *out = neg ? -v : v;
    return 0;
}

/* A width or height attribute: pixels (> 0), percent (< 0) when pct, 0 when none or not one */
static int dimension(const struct ut_html_tag *t, int a, int pct) {
    long v;
    const char *s = t->val[a];
    if(!t->seen[a] || parse_int(s, &v) || v <= 0) return 0;
    while(*s && *s != '%') s++;
    if(*s == '%') return pct ? -(int)(v > 100 ? 100 : v) : 0;
    return (int)(v > WD_SIZE_MAX ? WD_SIZE_MAX : v);
}

static int clamp_attr(const struct ut_html_tag *t, int a, int lo, int hi, int none) {
    long v;
    if(!t->seen[a] || parse_int(t->val[a], &v)) return none;
    return v < lo ? lo : v > hi ? hi : (int)v;
}

/**** colours ****/

static const struct { const char *name; unsigned int rgb; } colors[] = {    /* sorted */
    { "aqua", 0x00ffff }, { "black", 0x000000 }, { "blue", 0x0000ff }, { "brown", 0xa52a2a },
    { "crimson", 0xdc143c }, { "cyan", 0x00ffff }, { "darkblue", 0x00008b }, { "darkgray", 0xa9a9a9 },
    { "darkgreen", 0x006400 }, { "darkgrey", 0xa9a9a9 }, { "darkred", 0x8b0000 }, { "fuchsia", 0xff00ff },
    { "gold", 0xffd700 }, { "gray", 0x808080 }, { "green", 0x008000 }, { "grey", 0x808080 },
    { "indigo", 0x4b0082 }, { "lightblue", 0xadd8e6 }, { "lightgray", 0xd3d3d3 }, { "lightgreen", 0x90ee90 },
    { "lightgrey", 0xd3d3d3 }, { "lightyellow", 0xffffe0 }, { "lime", 0x00ff00 }, { "magenta", 0xff00ff },
    { "maroon", 0x800000 }, { "navy", 0x000080 }, { "olive", 0x808000 }, { "orange", 0xffa500 },
    { "pink", 0xffc0cb }, { "purple", 0x800080 }, { "red", 0xff0000 }, { "silver", 0xc0c0c0 },
    { "tan", 0xd2b48c }, { "teal", 0x008080 }, { "violet", 0xee82ee }, { "white", 0xffffff },
    { "yellow", 0xffff00 }
};

static int hexdigit(unsigned char c) {
    if(digit(c)) return c - '0';
    c |= 0x20;
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

unsigned int wd_color(const char *s) {
    char name[16];
    size_t n, i, lo = 0, hi = sizeof(colors) / sizeof(colors[0]);
    int v[6];
    while(ws((unsigned char)*s)) s++;
    n = strlen(s);
    while(n && ws((unsigned char)s[n - 1])) n--;
    if(n && s[0] == '#') {
        s++;
        n--;
    } else if(n && n < sizeof(name)) {                 /* a name, or six hex digits without the '#' */
        for(i = 0; i < n; i++) name[i] = (char)lower((unsigned char)s[i]);
        name[n] = 0;
        while(lo < hi) {
            size_t mid = (lo + hi) / 2;
            int c = strcmp(colors[mid].name, name);
            if(!c) return colors[mid].rgb;
            if(c < 0) lo = mid + 1;
            else hi = mid;
        }
        if(n != 6) return WD_NONE;
    }
    if(n != 3 && n != 6) return WD_NONE;
    for(i = 0; i < n; i++) if((v[i] = hexdigit((unsigned char)s[i])) < 0) return WD_NONE;
    if(n == 3) return (unsigned int)(v[0] * 17) << 16 | (unsigned int)(v[1] * 17) << 8 | (unsigned int)(v[2] * 17);
    return (unsigned int)(v[0] << 4 | v[1]) << 16 | (unsigned int)(v[2] << 4 | v[3]) << 8 | (unsigned int)(v[4] << 4 | v[5]);
}

static unsigned int attr_color(const struct ut_html_tag *t, int a) {
    return t->seen[a] ? wd_color(t->val[a]) : WD_NONE;
}

/**** growing the document ****/

/* Room for one more of an array (count of cap, each size bytes): 0, or -1 (failed set) */
static int grow(struct bd *b, void **arr, size_t count, size_t *cap, size_t size) {
    size_t n;
    void *p;
    if(count < *cap) return 0;
    n = *cap ? *cap * 2 : 16;
    p = realloc(*arr, n * size);
    if(!p) {
        b->failed = 1;
        return -1;
    }
    *arr = p;
    *cap = n;
    return 0;
}

/* Room for n more bytes of text: 0, or -1 when the text is full (the page stops there) or memory is out */
static int room(struct bd *b, size_t n) {
    size_t cap;
    char *t;
    if(b->stopped || b->failed) return -1;
    if(b->d.text_len + n > UT_TEXT_MAX) {
        b->stopped = 1;
        b->d.truncated |= UT_TRUNC_TEXT;
        return -1;
    }
    if(b->d.text_len + n < b->text_cap) return 0;
    cap = b->text_cap ? b->text_cap * 2 : 4096;
    while(cap <= b->d.text_len + n) cap *= 2;
    if(cap > UT_TEXT_MAX + 1) cap = UT_TEXT_MAX + 1;
    t = realloc(b->d.text, cap);
    if(!t) {
        b->failed = 1;
        return -1;
    }
    b->d.text = t;
    b->text_cap = cap;
    return 0;
}

static int full(struct bd *b) {                     /* boxes or runs past their limits: the page stops */
    b->stopped = 1;
    b->d.truncated |= WD_TRUNC_BOXES;
    return -1;
}

/* A new box of kind in parent: its index, or -1 (a limit, or out of memory). When parent holds inline
   content, that content moves into an anonymous box first, so that a box holds boxes or inline content. */
static int add_box(struct bd *b, int kind, int parent);

static int new_box(struct bd *b, int kind, int parent) {
    struct wd_block *x, *p;
    int i;
    if(b->stopped || b->failed) return -1;
    if(b->d.block_count >= WD_BLOCKS_MAX) return full(b);
    if(grow(b, (void **)&b->d.blocks, b->d.block_count, &b->blocks_cap, sizeof(*b->d.blocks))) return -1;
    i = (int)b->d.block_count++;
    x = &b->d.blocks[i];
    memset(x, 0, sizeof(*x));
    x->kind = (unsigned char)kind;
    x->colspan = 1;
    x->bg = WD_NONE;
    x->parent = parent;
    x->first = x->last = x->next = -1;
    x->run = (unsigned int)b->d.run_count;
    if(parent >= 0) {
        p = &b->d.blocks[parent];
        if(p->last >= 0) b->d.blocks[p->last].next = i;
        else p->first = i;
        p->last = i;
    }
    return i;
}

static int add_box(struct bd *b, int kind, int parent) {
    struct wd_block *p = &b->d.blocks[parent];
    int anon;
    if(b->none) return -1;
    if(p->nruns && p->first < 0) {                  /* its inline content goes into a box of its own */
        unsigned int run = p->run, nruns = p->nruns;
        anon = new_box(b, WB_BLOCK, parent);
        if(anon < 0) return -1;
        b->d.blocks[anon].flags = WBF_ANON;
        b->d.blocks[anon].run = run;
        b->d.blocks[anon].nruns = nruns;
        p = &b->d.blocks[parent];
        p->nruns = 0;
    }
    b->target = -1;
    b->space = 0;
    return new_box(b, kind, parent);
}

static int box_now(const struct bd *b) { return b->st[b->depth - 1].box; }

/* The box that takes inline content: the innermost box, or an anonymous one at its end when it holds boxes.
   A table or a row takes none: an implied cell does. -1 when none can be made. */
static int target(struct bd *b) {
    int box, t;
    struct wd_block *x;
    if(b->target >= 0) return b->target;
    box = box_now(b);
    x = &b->d.blocks[box];
    if(x->kind == WB_TABLE || x->kind == WB_ROW) {      /* text in a table but in no cell: a cell of its own */
        if(x->kind == WB_TABLE) {
            box = new_box(b, WB_ROW, box);
            if(box < 0) return -1;
        }
        box = new_box(b, WB_CELL, box);
        if(box < 0) return -1;
        x = &b->d.blocks[box];
    }
    if(x->first < 0) {
        if(x->nruns && x->run + x->nruns != b->d.run_count) {   /* (never: a box's runs are its last ones) */
            t = new_box(b, WB_BLOCK, box);
            if(t < 0) return -1;
            b->d.blocks[t].flags = WBF_ANON;
            box = t;
        }
        x = &b->d.blocks[box];
        if(!x->nruns) x->run = (unsigned int)b->d.run_count;
        b->target = box;
        return box;
    }
    t = new_box(b, WB_BLOCK, box);
    if(t < 0) return -1;
    b->d.blocks[t].flags = WBF_ANON;
    b->target = t;
    return t;
}

/**** inline content ****/

static unsigned short style_now(const struct bd *b) {
    unsigned short s = 0;
    if(b->is.sty[S_BOLD] || b->headings) s |= WS_BOLD;
    if(b->is.sty[S_ITALIC]) s |= WS_ITALIC;
    if(b->is.sty[S_MONO] || b->pre) s |= WS_MONO;
    if(b->is.sty[S_UNDER]) s |= WS_UNDER;
    if(b->is.sty[S_STRIKE]) s |= WS_STRIKE;
    if(b->is.sty[S_SUB]) s |= WS_SUB;
    else if(b->is.sty[S_SUP]) s |= WS_SUP;
    if(b->is.sty[S_NOWRAP]) s |= WS_NOWRAP;
    if(b->is.sty[S_MARK]) s |= WS_MARK;
    if(b->pre) s |= WS_PRE;
    return ds_style(b->ds, s);
}

static unsigned char size_now(const struct bd *b) {
    int i;
    for(i = b->depth - 1; i > 0; i--) {             /* the innermost heading's */
        int el = b->st[i].el;
        if(el >= E_H1 && el <= E_H6) return (unsigned char)(WZ_H1 + (el - E_H1));
    }
    if(b->is.sty[S_BIG]) return WZ_BIG;
    if(b->is.sty[S_SMALL]) return WZ_SMALL;
    return WZ_NORMAL;
}

static unsigned int color_now(const struct bd *b) {
    int n = b->is.ncolors < COLORS_MAX ? b->is.ncolors : COLORS_MAX;
    return ds_color(b->ds, n ? b->colors[n - 1] : WD_NONE);
}

/* The run that takes text of this kind and look: the target's last one when it matches and its text is the
   last written, else a new one. NULL when none can be made. */
static struct wd_run *run_for(struct bd *b, int kind, unsigned short style, unsigned char size, unsigned int color,
                              int link) {
    int t = target(b);
    struct wd_block *x;
    struct wd_run *r;
    unsigned int bg = b->space_now ? b->space_bg : ds_bg(b->ds);
    unsigned short px = b->space_now ? b->space_px : ds_px(b->ds);
    if(t < 0) return NULL;
    x = &b->d.blocks[t];
    if(kind == WR_TEXT && x->nruns) {
        r = &b->d.runs[x->run + x->nruns - 1];
        if(r->kind == WR_TEXT && r->style == style && r->size == size && r->color == color && r->link == link &&
           r->bg == bg && r->px == px && r->off + r->len == b->d.text_len) return r;
    }
    if(b->d.run_count >= WD_RUNS_MAX) {
        full(b);
        return NULL;
    }
    if(grow(b, (void **)&b->d.runs, b->d.run_count, &b->runs_cap, sizeof(*b->d.runs))) return NULL;
    x = &b->d.blocks[t];
    r = &b->d.runs[b->d.run_count++];
    memset(r, 0, sizeof(*r));
    r->kind = (unsigned char)kind;
    r->style = style;
    r->size = size;
    r->color = color;
    r->link = link;
    r->image = -1;
    r->control = -1;
    r->off = (unsigned int)b->d.text_len;
    r->bg = bg;
    r->px = px;
    r->src = (unsigned int)b->pos;
    x->nruns++;
    return r;
}

/* Bytes of text in the current look; 0, or -1 when they do not fit */
static int put_text(struct bd *b, const char *s, size_t n, unsigned short style, unsigned char size,
                    unsigned int color, int link) {
    struct wd_run *r;
    if(room(b, n)) return -1;
    r = run_for(b, WR_TEXT, style, size, color, link);
    if(!r) return -1;
    memcpy(b->d.text + b->d.text_len, s, n);
    b->d.text_len += n;
    b->d.text[b->d.text_len] = 0;
    r->len += (unsigned int)n;
    return 0;
}

/* The space that waits, in the look it came in; 0, or -1 when it does not fit */
static int put_space(struct bd *b) {
    int rc;
    b->space = 0;
    b->space_now = 1;
    rc = put_text(b, " ", 1, b->space_style, b->space_size, b->space_color, b->space_link);
    b->space_now = 0;
    return rc;
}

static int ascii_space(unsigned int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; }

/* One more character of a label, after what gap holds back: 1 when it does not fit */
static int label_put(char *label, size_t *len, int *gap, unsigned int cp) {
    unsigned char u[4];
    size_t k;
    if(ut_html_hidden(cp)) return 0;
    k = ut_utf8_encode(cp, u);
    if(*len + (*gap && *len) + k > LABEL_MAX) return 1;
    if(*gap && *len) label[(*len)++] = ' ';
    *gap = 0;
    memcpy(label + *len, u, k);
    *len += k;
    label[*len] = 0;
    return 0;
}

/* A label (a link's, a button's, a select's): collapsed, cut at LABEL_MAX */
static void label_add(char *label, size_t *len, int *gap, unsigned int cp) {
    if(ut_html_space(cp) || cp == 0xa0) *gap = 1;
    else label_put(label, len, gap, cp);
}

/* Is the target's content at the start of a line (nothing yet, or a line break last)? */
static int line_start(const struct bd *b) {
    const struct wd_block *x;
    if(b->target < 0) return 1;
    x = &b->d.blocks[b->target];
    return !x->nruns || b->d.runs[x->run + x->nruns - 1].kind == WR_BR;
}

static void line_break(struct bd *b) {              /* <br>, a line break in pre */
    if(b->stopped || b->failed || b->none) return;
    b->space = 0;
    b->col = 0;
    run_for(b, WR_BR, 0, size_now(b), WD_NONE, b->link);
}

/* One character of the page's text */
static void text_char(struct bd *b, unsigned int cp) {
    unsigned char u[4];
    size_t k;
    unsigned short style;
    unsigned char size;
    unsigned int color;
    if(b->stopped || b->failed) return;
    if(b->button || b->select) {                    /* a field's label */
        if(!b->none && (!b->select || b->option == 1)) label_add(b->flabel, &b->flabel_len, &b->flabel_gap, cp);
        if(b->opt_open) {                           /* what it sends when it has no value: only ASCII spaces collapse */
            if(ascii_space(cp)) b->olabel_gap = 1;  /* (display: none or not: an option's text is its form's) */
            else if(ut_html_hidden(cp) || label_put(b->olabel, &b->olabel_len, &b->olabel_gap, cp)) b->olabel_cut = 1;
        }
        return;
    }
    if(b->none) return;
    if(b->link_open) label_add(b->label, &b->label_len, &b->label_gap, cp);
    if(b->pre) {
        if(cp == '\n') {
            line_break(b);
            return;
        }
        if(cp == '\t') {
            do {
                if(put_text(b, " ", 1, style_now(b), size_now(b), color_now(b), b->link)) return;
            } while(++b->col % 8);
            return;
        }
        if(ut_html_space(cp)) cp = ' ';
        else if(ut_html_hidden(cp)) return;
        b->col++;
    } else if(ut_html_space(cp)) {
        if(!line_start(b) && !b->space) {
            b->space = 1;
            b->space_style = style_now(b);
            b->space_size = size_now(b);
            b->space_color = color_now(b);
            b->space_link = b->link;
            b->space_px = ds_px(b->ds);
            b->space_bg = ds_bg(b->ds);
        }
        return;
    } else if(ut_html_hidden(cp)) return;
    style = style_now(b);
    size = size_now(b);
    color = color_now(b);
    if(b->space && put_space(b)) return;
    k = ut_utf8_encode(cp, u);
    put_text(b, (const char *)u, k, style, size, color, b->link);
}

/* A field, image or other atom: a run of its own whose text is label (n bytes, already safe) */
static struct wd_run *atom(struct bd *b, int kind, const char *label, size_t n) {
    struct wd_run *r;
    if(b->stopped || b->failed || b->none) return NULL;
    if(b->space && put_space(b)) return NULL;
    if(room(b, n)) return NULL;
    r = run_for(b, kind, style_now(b), size_now(b), color_now(b), b->link);
    if(!r) return NULL;
    r->control = -1;
    memcpy(b->d.text + b->d.text_len, label, n);
    b->d.text_len += n;
    b->d.text[b->d.text_len] = 0;
    r->len = (unsigned int)n;
    return r;
}

/* An attribute's text as a label: whitespace collapsed, hidden characters out, cut at LABEL_MAX; in scratch */
static size_t collapse(struct bd *b, const char *s, size_t n) {
    size_t len = 0, i = 0, k;
    int gap = 0;
    unsigned int cp;
    while(i < n) {
        k = ut_utf8_decode((const unsigned char *)s + i, n - i, &cp);
        i += k;
        label_add(b->scratch, &len, &gap, cp);
    }
    b->scratch[len] = 0;
    return len;
}

/**** the open elements ****/

static int is_list(int el) { return el == E_UL || el == E_OL || el == E_MENU || el == E_DIR; }
static int is_cell(int el) { return el == E_TD || el == E_TH || el == E_CAPTION; }

static void link_close(struct bd *b);
static void field_close(struct bd *b);
static void option_end(struct bd *b);

static unsigned int words_of(const struct ut_html_tag *t);

/* The element opens in the page's CSS, with the elements around it right by now: how it is displayed. No tag says
   what it is when HTML implies it (a row, a text's pre). */
static int css_open(struct bd *b, int el, const struct ut_html_tag *t) {
    struct ds_element e;
    memset(&e, 0, sizeof(e));
    e.el = el;
    e.name = el == E_TR ? "tr" : el == E_TBODY ? "tbody" : "pre";
    if(t) {
        e.in = b->in;
        e.len = b->len;
        e.q = b->tag_q;
        e.cs = b->cs;
        e.name = t->name;
        e.words = words_of(t);
    }
    e.style = style_now(b);
    e.color = color_now(b);
    return ds_open(b->ds, &e);
}

/* Open el on the stack with a box of kind (kind < 0: no box of its own); NULL when the stack is full (it is
   only counted) or the box cannot be made */
static struct frame *frame_of(struct bd *b, int el, int kind, int hides, int css) {
    struct frame *f;
    int box;
    if(hides || b->none) kind = -1;
    box = kind >= 0 ? add_box(b, kind, box_now(b)) : box_now(b);
    if(box < 0) {
        b->over[el]++;
        ds_close(b->ds, css);
        return NULL;
    }
    f = &b->st[b->depth];
    memset(f, 0, sizeof(*f));
    f->el = (unsigned char)el;
    f->box = box;
    f->css = css;
    f->own = kind >= 0;
    f->hides = (unsigned char)hides;
    b->none += hides;
    b->css_box = kind >= 0 ? box : -1;
    b->open[el]++;
    b->depth++;
    if(kind >= 0) b->target = -1;
    if(is_cell(el)) {                               /* a cell starts with the style around its table */
        f->saved = b->is;
        f->saved_on = 1;
        link_close(b);
    }
    return f;
}

static struct frame *push(struct bd *b, int el, int kind) {
    int css = ds_height(b->ds), display;
    if(b->depth == STACK_MAX) {
        b->over[el]++;
        return NULL;
    }
    display = css_open(b, el, b->tag.el == el && !b->tag.end ? &b->tag : NULL);
    if(b->none) display = DS_ASIS;                  /* (it goes on the stack all the same, for what ends it) */
    if(el == E_TABLE || el == E_TR || is_cell(el)) {    /* (a table keeps its parts, but for none) */
        if(display != DS_NONE) display = DS_ASIS;
    } else if(display == DS_INLINE) kind = -1;
    b->display = display;
    return frame_of(b, el, kind, display == DS_NONE, css);
}

/* Close the top element */
static void pop(struct bd *b) {
    struct frame *f = &b->st[b->depth - 1];
    int el = f->el;
    if(is_cell(el)) {
        link_close(b);
        field_close(b);
    }
    if(f->saved_on) b->is = f->saved;
    if(f->off) b->fb.off--;
    if(f->legend) b->legends--;
    if(el == E_PRE || el == E_LISTING || el == E_XMP) b->pre--;
    if(is_list(el)) b->lists--;
    if(el == E_TABLE) b->tables--;
    if(el >= E_H1 && el <= E_H6) b->headings--;
    b->depth--;
    b->open[el]--;
    b->none -= f->hides;
    ds_close(b->ds, f->css);
    if(f->own) {
        b->target = -1;
        b->space = 0;
    }
}

static void pop_to(struct bd *b, int i) {           /* close st[i] and everything inside it */
    while(b->depth > i && b->depth > 1) pop(b);
}

enum { SCOPE, SCOPE_LIST };
static int in_scope(const struct bd *b, int el, int kind) {
    int i, e;
    if(!b->open[el]) return 0;
    for(i = b->depth - 1; i > 0; i--) {
        e = b->st[i].el;
        if(e == el) return i;
        if((ut_html_flags(e) & F_SCOPE) || (kind == SCOPE_LIST && (e == E_UL || e == E_OL))) return 0;
    }
    return 0;
}

static int heading_in_scope(const struct bd *b) {
    int i;
    if(!b->headings) return 0;
    for(i = b->depth - 1; i > 0; i--) {
        if(ut_html_flags(b->st[i].el) & F_HEAD) return i;
        if(ut_html_flags(b->st[i].el) & F_SCOPE) return 0;
    }
    return 0;
}

static void close_p(struct bd *b) {
    int i = in_scope(b, E_P, SCOPE);
    if(i) pop_to(b, i);
}

static void close_item(struct bd *b, int el) {      /* html.c's: <li> closes its list's open item, ... */
    int i, e;
    for(i = b->depth - 1; i > 0; i--) {
        e = b->st[i].el;
        if(el == E_LI ? e == E_LI : e == E_DT || e == E_DD) {
            pop_to(b, i);
            return;
        }
        if((ut_html_flags(e) & F_SPECIAL) && e != E_ADDRESS && e != E_DIV && e != E_P) return;
    }
}

static int table_at(const struct bd *b) {           /* the innermost open table, 0: none */
    int i;
    if(!b->tables) return 0;
    for(i = b->depth - 1; i > 0; i--) if(b->st[i].el == E_TABLE) return i;
    return 0;
}

static int above(const struct bd *b, int from, int el, int el2) {
    int i;
    for(i = b->depth - 1; i > from; i--) if(b->st[i].el == el || b->st[i].el == el2) return i;
    return 0;
}

static struct wd_block *box_of(struct bd *b, struct frame *f) {
    return f ? (f->own ? &b->d.blocks[f->box] : &b->no_box) : NULL;
}

static int align_of(const struct ut_html_tag *t) {
    if(attr_is(t, A_ALIGN, "center") || attr_is(t, A_ALIGN, "middle")) return WA_CENTER;
    if(attr_is(t, A_ALIGN, "right")) return WA_RIGHT;
    if(attr_is(t, A_ALIGN, "left") || attr_is(t, A_ALIGN, "justify")) return WA_LEFT;
    return WA_INHERIT;
}

/**** lists ****/

static int ol_marker(const struct ut_html_tag *t) {
    if(!t->seen[A_TYPE] || t->len[A_TYPE] != 1) return WMK_DECIMAL;
    switch(t->val[A_TYPE][0]) {
    case 'a': return WMK_LOWER_ALPHA;
    case 'A': return WMK_UPPER_ALPHA;
    case 'i': return WMK_LOWER_ROMAN;
    case 'I': return WMK_UPPER_ROMAN;
    }
    return WMK_DECIMAL;
}

static void list_start(struct bd *b, struct ut_html_tag *t) {
    int nested = b->lists > 0 || b->open[E_LI];
    struct frame *f = push(b, t->el, WB_BLOCK);
    struct wd_block *x = box_of(b, f);
    if(!f) return;
    x->flags = (unsigned short)(WBF_LIST | (nested ? 0 : WBF_PARA));
    x->align = (unsigned char)align_of(t);
    b->lists++;
    f->depth = (unsigned char)(b->lists > 255 ? 255 : b->lists);
    f->next = 1;
    if(t->el == E_OL) {
        f->marker = (unsigned char)ol_marker(t);
        if(t->seen[A_START]) parse_int(t->val[A_START], &f->next);
    } else if(attr_is(t, A_TYPE, "square")) f->marker = WMK_SQUARE;
    else if(attr_is(t, A_TYPE, "circle")) f->marker = WMK_CIRCLE;
    else if(attr_is(t, A_TYPE, "disc")) f->marker = WMK_DISC;
    else f->marker = f->depth == 1 ? WMK_DISC : f->depth == 2 ? WMK_CIRCLE : WMK_SQUARE;
}

static void item_start(struct bd *b, struct ut_html_tag *t) {
    struct frame *f, *list = NULL;
    struct wd_block *x;
    long num;
    int i;
    close_item(b, E_LI);
    for(i = b->depth - 1; i > 0; i--) {             /* its list: the nearest one, with no cell between */
        if(is_list(b->st[i].el)) {
            list = &b->st[i];
            break;
        }
        if(ut_html_flags(b->st[i].el) & F_SCOPE) break;
    }
    f = push(b, E_LI, WB_ITEM);
    x = box_of(b, f);
    if(!f) return;
    x->align = (unsigned char)align_of(t);
    if(!list) {
        x->marker = WMK_DISC;
        x->flags = WBF_INDENT;                      /* an item outside a list is indented as one */
        return;
    }
    x->marker = list->marker;
    if(list->el == E_OL && !b->none) {
        num = list->next;
        if(t->seen[A_VALUE]) parse_int(t->val[A_VALUE], &num);
        list->next = num + 1;
        x->number = num;
    }
}

/**** tables ****/

/* A table's section opens in the cascade (t NULL: the tbody HTML implies), and display: none hides what it holds */
static void section_open(struct bd *b, int el, const struct ut_html_tag *t) {
    int css = ds_height(b->ds), display = css_open(b, el, t);
    struct frame *f;
    if(display == DS_NONE && !b->none && b->depth < STACK_MAX && (f = frame_of(b, el, -1, 1, css)) != NULL) f->made = 1;
}

static int is_section(int el) { return el == E_TBODY || el == E_THEAD || el == E_TFOOT; }

static void row_start(struct bd *b, struct ut_html_tag *t) {
    int tb = table_at(b);
    struct frame *f;
    if(!tb) return;                                 /* a row outside a table is no row */
    pop_to(b, tb + 1 + (b->depth > tb + 1 && is_section(b->st[tb + 1].el)));    /* (a section hidden by its CSS stays) */
    if(ds_height(b->ds) == b->st[tb].css + 1) section_open(b, E_TBODY, NULL);
    f = push(b, E_TR, WB_ROW);
    if(f && t) {
        box_of(b, f)->bg = attr_color(t, A_BGCOLOR);
        box_of(b, f)->align = (unsigned char)align_of(t);
    }
}

static void cell_start(struct bd *b, struct ut_html_tag *t) {
    int tb = table_at(b), r, c;
    struct frame *f;
    struct wd_block *x;
    if(!tb) return;
    c = above(b, tb, E_TD, E_TH);
    if(c) pop_to(b, c);
    r = above(b, tb, E_TR, E_TR);
    if(!r) {                                        /* a cell without a row: the row is implied */
        row_start(b, NULL);
        if(!above(b, tb, E_TR, E_TR)) return;
    } else pop_to(b, r + 1);
    f = push(b, t->el, WB_CELL);
    x = box_of(b, f);
    if(!f) return;
    x->colspan = (unsigned short)clamp_attr(t, A_COLSPAN, 1, 64, 1);
    x->width = (short)dimension(t, A_WIDTH, 1);
    x->bg = attr_color(t, A_BGCOLOR);
    x->align = (unsigned char)align_of(t);
    if(t->el == E_TH) {
        x->flags |= WBF_HEADER;
        b->is.sty[S_BOLD]++;                        /* given back at the cell's end */
        if(x->align == WA_INHERIT) x->align = WA_CENTER;
    }
}

static void caption_start(struct bd *b, struct ut_html_tag *t) {
    int tb = table_at(b);
    struct frame *f;
    if(!tb) return;
    pop_to(b, tb + 1);
    f = push(b, E_CAPTION, WB_BLOCK);
    if(f) {
        box_of(b, f)->flags = WBF_CAPTION;
        box_of(b, f)->align = (unsigned char)(align_of(t) == WA_INHERIT ? WA_CENTER : align_of(t));
    }
}

static void table_start(struct bd *b, struct ut_html_tag *t) {
    struct frame *f = push(b, E_TABLE, WB_TABLE);
    struct wd_block *x = box_of(b, f);
    if(!f) return;
    b->tables++;
    x->border = (unsigned short)clamp_attr(t, A_BORDER, 0, 8, 0);
    if(t->seen[A_BORDER] && !t->len[A_BORDER]) x->border = 1;      /* <table border> */
    x->padding = (unsigned short)clamp_attr(t, A_CELLPADDING, 0, 32, 1);
    x->width = (short)dimension(t, A_WIDTH, 1);
    x->bg = attr_color(t, A_BGCOLOR);
    x->align = (unsigned char)align_of(t);
}

static void table_end(struct bd *b, int el) {       /* </table>, </tr>, </td>, </th>, </caption> */
    int tb = table_at(b), i;
    if(!tb) return;
    i = el == E_TABLE ? tb : above(b, tb, el, el);
    if(i) pop_to(b, i);
}

/**** anchors and links ****/

static void add_anchor(struct bd *b, const char *name, size_t len, int over) {
    unsigned int h = 2166136261u, cp;
    size_t i, slot, k;
    struct ut_anchor *a;
    if(!len || over || len > UT_FIELD_MAX || b->stopped || b->failed) return;
    for(i = 0; i < len; i += k) {
        k = ut_utf8_decode((const unsigned char *)name + i, len - i, &cp);
        if(ut_html_hidden(cp)) return;
    }
    for(i = 0; i < len; i++) h = (h ^ (unsigned char)name[i]) * 16777619u;
    for(slot = h % AHASH; b->ahash[slot]; slot = (slot + 1) % AHASH)
        if(!strcmp(b->d.anchors[b->ahash[slot] - 1].name, name)) return;
    if(b->d.anchor_count == UT_ANCHORS_MAX || b->link_bytes_full || b->link_bytes + len + 1 > UT_LINK_BYTES_MAX) {
        if(b->d.anchor_count < UT_ANCHORS_MAX) b->link_bytes_full = 1;
        b->d.truncated |= UT_TRUNC_ANCHORS;
        return;
    }
    if(grow(b, (void **)&b->d.anchors, b->d.anchor_count, &b->anchors_cap, sizeof(*b->d.anchors))) return;
    a = &b->d.anchors[b->d.anchor_count];
    a->name = malloc(len + 1);
    if(!a->name) {
        b->failed = 1;
        return;
    }
    memcpy(a->name, name, len + 1);
    a->offset = b->d.text_len;
    b->ahash[slot] = (unsigned short)(b->d.anchor_count + 1);
    b->d.anchor_count++;
    b->link_bytes += len + 1;
}

/* An address as written, without the ASCII whitespace and controls at its ends */
static const char *stripped(const char *s, size_t *n) {
    while(*n && (unsigned char)s[*n - 1] <= ' ') --*n;
    while(*n && (unsigned char)*s <= ' ') {
        s++;
        --*n;
    }
    return s;
}

/* The same of an attribute of a tag, in place */
static void strip_attr(struct ut_html_tag *t, int a) {
    size_t n = t->len[a];
    const char *s = stripped(t->val[a], &n);
    memmove(t->val[a], s, n);
    t->val[a][n] = 0;
    t->len[a] = n;
}

static void link_start(struct bd *b, struct ut_html_tag *t) {
    size_t n = t->len[A_HREF];
    const char *h = stripped(t->val[A_HREF], &n);
    void *p;
    link_close(b);
    if(!t->seen[A_HREF] || !n) return;              /* no address: no link */
    b->link_open = 1;
    b->label_len = 0;
    b->label_gap = 0;
    b->link_alt_len = 0;
    if(t->seen[A_ARIA]) b->link_alt_len = collapse(b, t->val[A_ARIA], t->len[A_ARIA]);
    if(!b->link_alt_len && t->seen[A_TITLE]) b->link_alt_len = collapse(b, t->val[A_TITLE], t->len[A_TITLE]);
    memcpy(b->link_alt, b->scratch, b->link_alt_len);
    b->link_alt[b->link_alt_len] = 0;
    if(b->stopped || b->failed) return;
    if(b->d.link_count == UT_LINKS_MAX) {           /* its text, but no link */
        b->d.truncated |= UT_TRUNC_LINKS;
        return;
    }
    if(b->d.link_count == b->links_cap) {
        size_t cap = b->links_cap ? b->links_cap * 2 : 16;
        p = realloc(b->hrefs, cap * sizeof(*b->hrefs));
        if(p) b->hrefs = p;
        p = p ? realloc(b->long_href, cap) : NULL;
        if(p) b->long_href = p;
        p = p ? realloc(b->d.links, cap * sizeof(*b->d.links)) : NULL;
        if(!p) {
            b->failed = 1;
            return;
        }
        b->d.links = p;
        b->links_cap = cap;
    }
    b->hrefs[b->d.link_count] = malloc(n + 1);
    if(!b->hrefs[b->d.link_count]) {
        b->failed = 1;
        return;
    }
    b->nhrefs++;
    memcpy(b->hrefs[b->d.link_count], h, n);
    b->hrefs[b->d.link_count][n] = 0;
    b->long_href[b->d.link_count] = t->over[A_HREF];
    memset(&b->d.links[b->d.link_count], 0, sizeof(b->d.links[0]));
    b->link = (int)b->d.link_count++;
}

/* A link ends: its label is its text (or its aria-label or title when it showed none) */
static void link_close(struct bd *b) {
    struct ut_link *l;
    const char *s;
    size_t n;
    if(!b->link_open) return;
    b->link_open = 0;
    if(b->link >= 0) {
        l = &b->d.links[b->link];
        s = b->label_len ? b->label : b->link_alt;
        n = b->label_len ? b->label_len : b->link_alt_len;
        l->label = malloc(n + 1);
        if(!l->label) b->failed = 1;
        else {
            memcpy(l->label, s, n);
            l->label[n] = 0;
        }
    }
    b->link = -1;
}

/**** images and forms ****/

static void image(struct bd *b, struct ut_html_tag *t) {
    size_t n = t->seen[A_ALT] ? collapse(b, t->val[A_ALT], t->len[A_ALT]) : 0;
    int w = dimension(t, A_WIDTH, 0), h = dimension(t, A_HEIGHT, 0);
    struct wd_run *r;
    size_t sn = t->len[A_SRC];
    const char *src = stripped(t->val[A_SRC], &sn);
    if(b->link_open) {                              /* its alt text labels a link it is in, as in html.c */
        size_t i = 0, k;
        unsigned int cp;
        while(i < n) {
            k = ut_utf8_decode((const unsigned char *)b->scratch + i, n - i, &cp);
            label_add(b->label, &b->label_len, &b->label_gap, cp);
            i += k;
        }
    }
    if(!n && !(w && h) && !(t->seen[A_SRC] && sn)) return;      /* nothing to show, now or ever */
    r = atom(b, WR_IMAGE, b->scratch, n);
    if(!r) return;
    r->w = (unsigned short)w;
    r->h = (unsigned short)h;
    if(!t->seen[A_SRC] || !sn || t->over[A_SRC] || b->d.image_count == WD_IMAGES_MAX) return;
    if(grow(b, (void **)&b->srcs, b->d.image_count, &b->images_cap, sizeof(*b->srcs))) return;
    b->srcs[b->d.image_count] = malloc(sn + 1);
    if(!b->srcs[b->d.image_count]) {
        b->failed = 1;
        return;
    }
    b->nsrcs++;
    memcpy(b->srcs[b->d.image_count], src, sn);
    b->srcs[b->d.image_count][sn] = 0;
    b->d.runs[b->d.run_count - 1].image = (int)b->d.image_count++;
}

/* A field's box, showing control */
static void field(struct bd *b, int kind, const char *label, size_t n, int checked, int control) {
    struct wd_run *r = b->skip_n ? NULL : atom(b, WR_FIELD, label, n);     /* hidden: a control with no box */
    if(r) {
        r->field = (unsigned char)kind;
        r->checked = (unsigned char)checked;
        r->control = control;
        if(control >= 0) b->d.controls[control].run = (int)(r - b->d.runs);
    }
}

/* A number attribute, as a width in cells or a limit; none when it is no number or out of range */
static int count_attr(const struct ut_html_tag *t, int a, int lo, int hi, int none) {
    long v;
    if(!t->seen[a] || parse_int(t->val[a], &v) || v < lo) return none;
    return v > hi ? hi : (int)v;
}

/* A control as this tag gives it, of kind and role, checked its own, its value the tag's */
static int control_of(struct bd *b, struct ut_html_tag *t, int kind, int role, int checked) {
    struct fm_new n;
    int c;
    memset(&n, 0, sizeof(n));
    n.kind = kind;
    n.role = role;
    n.name = t->seen[A_NAME] ? t->val[A_NAME] : NULL;
    n.value = t->seen[A_VALUE] ? t->val[A_VALUE] : NULL;
    n.cut = (t->seen[A_VALUE] && t->over[A_VALUE]) || (t->seen[A_NAME] && t->over[A_NAME]) || t->seen[A_DIRNAME] ||
            (t->el == E_INPUT && (attr_is(t, A_TYPE, "range") || attr_is(t, A_TYPE, "color")) && !(t->seen[A_VALUE] && t->len[A_VALUE])) ||
            (t->el == E_INPUT && attr_is(t, A_TYPE, "email") && t->seen[A_MULTIPLE] && t->seen[A_VALUE] && strchr(t->val[A_VALUE], ',')) ||
            (t->el == E_TEXTAREA && attr_is(t, A_WRAP, "hard")) || b->legends || (b->skip_n && b->skip_off);
    n.shaped = t->el == E_INPUT && (attr_is(t, A_TYPE, "range") || attr_is(t, A_TYPE, "color"));
    n.checked = checked;
    n.disabled = t->seen[A_DISABLED];
    n.readonly = t->seen[A_READONLY];
    n.multiple = t->seen[A_MULTIPLE];
    n.image = t->el == E_INPUT && attr_is(t, A_TYPE, "image");
    n.maxlength = count_attr(t, A_MAXLENGTH, 0, (int)WD_VALUE_MAX, -1);
    n.rows = count_attr(t, A_ROWS, 1, 50, 0);
    n.cols = count_attr(t, A_COLS, 1, 200, 0);
    n.size = count_attr(t, A_SIZE, 1, 200, 0);
    n.owner = t->seen[A_FORM] ? t->val[A_FORM] : NULL;
    if(t->seen[A_FORM] && t->over[A_FORM]) b->fb.lost = 1;      /* of what it names, only the start is known */
    if(t->seen[A_FORMACTION]) strip_attr(t, A_FORMACTION);
    n.formaction = t->seen[A_FORMACTION] ? t->val[A_FORMACTION] : NULL;
    n.formaction_cut = t->seen[A_FORMACTION] && t->over[A_FORMACTION];
    n.formmethod = t->seen[A_FORMMETHOD] ? t->val[A_FORMMETHOD] : NULL;
    c = fm_control(&b->d, &b->fb, &n);
    if(c >= 0) b->d.controls[c].src = (unsigned int)b->tag_q;
    return c;
}

/* A button's or a select's label is the text inside it: the field is made at its end */
static void field_close(struct bd *b) {
    int kind = b->button ? WF_BUTTON : WF_SELECT, control = b->fcontrol;
    char label[LABEL_MAX + 1];
    size_t n;
    if(!b->button && !b->select) return;
    option_end(b);
    if(b->select) fm_select_end(&b->d, &b->fb);
    n = b->flabel_len;
    memcpy(label, b->flabel, n);
    b->button = b->select = b->option = b->selected_seen = b->group_disabled = 0;
    b->flabel_len = 0;
    b->flabel_gap = 0;
    b->fcontrol = -1;
    if(b->field_none) b->field_none = 0;            /* display: none: the control, with no box */
    else field(b, kind, label, n, 0, control);
}

/* An option's end: its text is its label (or its label attribute's), and its value, its value attribute or its text */
static void option_end(struct bd *b) {
    if(!b->opt_open) return;
    b->opt_open = 0;
    b->olabel[b->olabel_len] = 0;
    fm_option(&b->d, &b->fb, b->opt_has_value ? b->opt_value : NULL, b->olabel, b->opt_label, b->opt_selected, b->opt_disabled,
              b->opt_has_value ? b->opt_cut : b->olabel_cut);
    if(b->opt_label[0] && b->option == 1) {         /* the select's box shows what its list does */
        b->flabel_len = strlen(b->opt_label);
        memcpy(b->flabel, b->opt_label, b->flabel_len + 1);
        b->flabel_gap = 0;
    }
    b->olabel_len = 0;
    b->olabel_gap = 0;
    b->olabel_cut = 0;
}

static void option_start(struct bd *b, struct ut_html_tag *t) {
    option_end(b);
    b->opt_open = 1;
    b->opt_selected = t->seen[A_SELECTED];
    b->opt_disabled = t->seen[A_DISABLED] || b->group_disabled;
    b->opt_has_value = t->seen[A_VALUE];
    b->opt_cut = t->seen[A_VALUE] && t->over[A_VALUE];
    if(b->opt_has_value) memcpy(b->opt_value, t->val[A_VALUE], t->len[A_VALUE] + 1);
    b->opt_label[0] = 0;
    if(t->seen[A_LABEL]) memcpy(b->opt_label, b->scratch, collapse(b, t->val[A_LABEL], t->len[A_LABEL]) + 1);
}

static void input(struct bd *b, struct ut_html_tag *t) {
    size_t n;
    int image_button = attr_is(t, A_TYPE, "image");
    if(t->seen[A_VALUE] && (attr_is(t, A_TYPE, "url") || attr_is(t, A_TYPE, "email"))) {
        char *v = t->val[A_VALUE];              /* the standard's value sanitization for these */
        size_t from = 0;
        n = t->len[A_VALUE];
        while(n && ascii_space((unsigned char)v[n - 1])) n--;
        while(from < n && ascii_space((unsigned char)v[from])) from++;
        memmove(v, v + from, n - from);
        v[n - from] = 0;
        t->len[A_VALUE] = n - from;
    }
    if(attr_is(t, A_TYPE, "hidden")) {
        control_of(b, t, WC_HIDDEN, 0, 0);
        return;
    }
    if(attr_is(t, A_TYPE, "checkbox") || attr_is(t, A_TYPE, "radio")) {
        int kind = attr_is(t, A_TYPE, "checkbox") ? WF_CHECKBOX : WF_RADIO;
        field(b, kind, "", 0, t->seen[A_CHECKED], control_of(b, t, kind, 0, t->seen[A_CHECKED]));
    } else if(attr_is(t, A_TYPE, "submit") || attr_is(t, A_TYPE, "reset") || attr_is(t, A_TYPE, "button") ||
            image_button) {
        int a = image_button ? A_ALT : A_VALUE;
        int role = attr_is(t, A_TYPE, "reset") ? WC_RESET : attr_is(t, A_TYPE, "button") ? WC_PUSH : WC_SUBMIT;
        int control = control_of(b, t, WF_BUTTON, role, 0);
        n = t->seen[a] ? collapse(b, t->val[a], t->len[a]) : 0;
        if(!n && !t->seen[a]) {
            const char *dflt = attr_is(t, A_TYPE, "reset") ? "Reset" : attr_is(t, A_TYPE, "button") ? "" : "Submit";
            n = strlen(dflt);
            memcpy(b->scratch, dflt, n + 1);
        }
        field(b, WF_BUTTON, b->scratch, n, 0, control);
    } else if(attr_is(t, A_TYPE, "file")) field(b, WF_FILE, "Choose a file", 13, 0, control_of(b, t, WF_FILE, 0, 0));
    else if(attr_is(t, A_TYPE, "password")) {
        n = t->seen[A_VALUE] && t->len[A_VALUE] ? (t->len[A_VALUE] > 32 ? 32 : t->len[A_VALUE]) : 0;
        memset(b->scratch, '*', n);
        field(b, WF_PASSWORD, b->scratch, n, 0, control_of(b, t, WF_PASSWORD, 0, 0));
    } else {                                        /* text, search, email, url, number, ...: a text field */
        int control = control_of(b, t, WF_TEXT, 0, 0);
        n = t->seen[A_VALUE] ? collapse(b, t->val[A_VALUE], t->len[A_VALUE]) : 0;
        if(n) field(b, WF_TEXT, b->scratch, n, 0, control);
        else {
            n = t->seen[A_PLACEHOLDER] ? collapse(b, t->val[A_PLACEHOLDER], t->len[A_PLACEHOLDER]) : 0;
            field(b, WF_TEXT, b->scratch, n, n > 0, control);     /* checked: the label is a placeholder */
        }
    }
}

/* An input that display: none hides is a control of its form all the same, as in a browser: it has no box, as what
   display: none holds has none */
static void hidden_input(struct bd *b, struct ut_html_tag *t) {
    b->none++;
    input(b, t);
    b->none--;
}

/* A tag inside <select>: 1 when dealt with here (only options count; anything else is ignored) */
static int select_tag(struct bd *b, struct ut_html_tag *t) {
    int el = t->el;
    if(el == E_OPTION || el == E_OPTGROUP) {
        if(!t->end && el == E_OPTION) option_start(b, t);
        else {
            option_end(b);
            if(el == E_OPTGROUP) b->group_disabled = !t->end && t->seen[A_DISABLED];
        }
        if(!t->end && el == E_OPTION && t->seen[A_SELECTED] && !b->selected_seen) {   /* the one it shows */
            b->selected_seen = 1;
            b->flabel_len = 0;
            b->flabel_gap = 0;
            b->option = 1;
        } else if(!t->end && el == E_OPTION && !b->option) b->option = 1;
        else if(b->option == 1) b->option = 2;
        return 1;
    }
    if(el == E_SELECT || (!t->end && (el == E_INPUT || el == E_KEYGEN || el == E_TEXTAREA))) {
        field_close(b);
        return el == E_SELECT;
    }
    if(b->tables && (el == E_TABLE || el == E_CAPTION || el == E_TBODY || el == E_THEAD || el == E_TFOOT || el == E_TR ||
                     el == E_TD || el == E_TH)) {
        if(t->end && el != E_TBODY && el != E_THEAD && el != E_TFOOT && !b->open[el]) return 1;   /* none is open: nothing */
        field_close(b);
        return 0;
    }
    return !(el == E_SCRIPT || el == E_TEMPLATE);
}

/**** inline elements, counted ****/

/* The style an inline element gives: S_*, or -1 (the extra names are the HTML elements html.c has no use for) */
static int style_of(const struct ut_html_tag *t) {
    switch(t->el) {
    case E_B: case E_STRONG: return S_BOLD;
    case E_I: case E_EM: case E_VAR: return S_ITALIC;
    case E_CODE: case E_TT: return S_MONO;
    case E_U: return S_UNDER;
    case E_S: case E_STRIKE: return S_STRIKE;
    case E_SUB: return S_SUB;
    case E_SUP: return S_SUP;
    case E_NOBR: return S_NOWRAP;
    case E_BIG: return S_BIG;
    case E_SMALL: return S_SMALL;
    case E_OTHER:
        if(!strcmp(t->name, "cite") || !strcmp(t->name, "dfn")) return S_ITALIC;
        if(!strcmp(t->name, "kbd") || !strcmp(t->name, "samp")) return S_MONO;
        if(!strcmp(t->name, "ins")) return S_UNDER;
        if(!strcmp(t->name, "del")) return S_STRIKE;
        if(!strcmp(t->name, "mark")) return S_MARK;
        return -1;
    }
    return -1;
}

static void font_start(struct bd *b, struct ut_html_tag *t) {
    unsigned int c = attr_color(t, A_COLOR);
    if(c == WD_NONE) c = color_now(b);              /* a <font> without a colour keeps the one around it */
    if(b->is.ncolors < COLORS_MAX) b->colors[b->is.ncolors] = c;
    b->is.ncolors++;
}

/**** the page's CSS on a tag ****/

static unsigned int words_of(const struct ut_html_tag *t) {
    static const unsigned short given[S_COUNT] = { [S_BOLD] = WS_BOLD, [S_ITALIC] = WS_ITALIC, [S_UNDER] = WS_UNDER,
                                                   [S_STRIKE] = WS_STRIKE };
    int s = style_of(t);
    unsigned int w = s >= 0 ? given[s] : 0;
    if((t->el >= E_H1 && t->el <= E_H6) || t->el == E_TH) w |= WS_BOLD;
    if(t->el == E_ADDRESS) w |= WS_ITALIC;
    if(t->el == E_FONT && attr_color(t, A_COLOR) != WD_NONE) w |= DS_W_COLOR;
    return w;
}

/* Elements that are no part of what is shown, and those that have a place of their own in the cascade */
static int no_style(int el) {
    switch(el) {
    case E_HTML: case E_BODY: case E_HEAD: case E_TITLE: case E_STYLE: case E_SCRIPT: case E_META: case E_LINK:
    case E_BASE: case E_TBODY: case E_THEAD: case E_TFOOT: case E_XMP:
        return 1;
    }
    return 0;
}

/* An element HTML gives no box: CSS may give it one, or hide it. 1 when that is all there is to it. */
static int css_inline(struct bd *b, struct ut_html_tag *t, unsigned int fl) {
    int css = ds_height(b->ds), display;
    struct frame *f;
    if(no_style(t->el)) return 0;
    display = css_open(b, t->el, t);                /* (also where nothing shows: its end tag must find it) */
    if(b->none) return 0;
    if(display == DS_ASIS && t->seen[A_HIDDEN]) display = DS_NONE;
    if(t->el == E_SELECT && display == DS_BLOCK) display = DS_ASIS;    /* (select_tag() takes its end tag) */
    b->display = display;
    if(t->el == E_SELECT && display == DS_NONE) {   /* so no frame: nothing in it is shown but options anyway */
        b->select_none = 1;
        return 0;
    }
    if(display != DS_NONE && display != DS_BLOCK) return 0;
    if(display == DS_NONE && (fl & F_VOID)) {
        ds_close(b->ds, css);
        return 1;
    }
    if(b->depth < STACK_MAX && (f = frame_of(b, t->el, display == DS_BLOCK ? WB_BLOCK : -1, display == DS_NONE, css)) != NULL)
        f->made = 1;
    return 0;
}

/* The tag's CSS goes on the box it made */
static void css_box(struct bd *b) {
    if(b->css_box >= 0) ds_box(b->ds, &b->d.blocks[b->css_box], b->display);
    b->css_box = -1;
}

/* The tag has done what it does: so has a void element, and a <select> as far as text goes: its end tag is
   select_tag()'s */
static void css_done(struct bd *b, const struct ut_html_tag *t, unsigned int fl) {
    css_box(b);
    if(t->el == E_SELECT && b->select) ds_close(b->ds, b->css_at);
    if(!(fl & F_VOID)) return;
    if(b->depth > 1 && b->st[b->depth - 1].made && b->st[b->depth - 1].el == t->el) pop(b);
    ds_close(b->ds, b->css_at);
}

/* The end tag of an element HTML gives no box: one that CSS gave a box goes with it, else its place in the cascade */
static void css_end(struct bd *b, const struct ut_html_tag *t) {
    int i;
    if(ds_end(b->ds, t->name, b->st[b->depth - 1].css + 1) || !b->open[t->el]) return;
    for(i = b->depth - 1; i > 0; i--) {
        if(b->st[i].el == t->el && b->st[i].made && ds_named(b->ds, b->st[i].css, t->name)) {
            pop_to(b, i);
            return;
        }
        if(ut_html_flags(b->st[i].el) & (F_SCOPE | F_SPECIAL)) return;
    }
}

/* Does the page's CSS show this element, for all its hidden attribute says? */
static int css_shown(struct bd *b, const struct ut_html_tag *t) {
    int css = ds_height(b->ds), display = css_open(b, t->el, t);
    ds_close(b->ds, css);
    return !b->none && (display == DS_BLOCK || display == DS_INLINE || display == DS_SHOWN);
}

static void css_root(struct bd *b, const struct ut_html_tag *t) {
    struct ds_element e;
    memset(&e, 0, sizeof(e));
    e.in = b->in;
    e.len = b->len;
    e.q = b->tag_q;
    e.cs = b->cs;
    e.el = t->el;
    e.name = t->name;
    e.color = WD_NONE;
    if(b->root_seen[t->el == E_BODY]) return;       /* a second one adds nothing here */
    b->root_seen[t->el == E_BODY] = 1;
    ds_root(b->ds, t->el == E_BODY ? DS_ROOT_BODY : DS_ROOT_HTML, &e);
    if(b->depth > 1 && !ds_hidden(b->ds)) return;   /* (what opened hidden has no boxes to go on in) */
    b->none += ds_hidden(b->ds) - b->page_none;
    b->page_none = ds_hidden(b->ds);
}

/**** tags ****/

static int heading(int el) { return el >= E_H1 && el <= E_H6; }

/* The box kind of an element on the stack (-1: it has none) */
static int kind_of(int el) {
    switch(el) {
    case E_LI: return WB_ITEM;
    case E_TABLE: return WB_TABLE;
    case E_TR: return WB_ROW;
    case E_TD: case E_TH: return WB_CELL;
    }
    return (ut_html_flags(el) & F_PUSH) ? WB_BLOCK : -1;
}

static void start_hidden(struct bd *b, struct ut_html_tag *t) {
    b->skip_el = t->el;
    b->skip_n = 1;
    b->skip_foreign = t->el == E_SVG || t->el == E_MATH;
    b->skip_inert = 0;
    b->skip_off = 0;
    b->skip_fields = 0;
}

/* A form's start. Its address, as a link's, has no whitespace at its ends. */
static void form_start(struct bd *b, struct ut_html_tag *t) {
    if(t->seen[A_ACTION]) strip_attr(t, A_ACTION);
    if(t->seen[A_ID] && t->over[A_ID]) b->fb.lost = 1;          /* a form= could name it, or another of its start */
    fm_form(&b->d, &b->fb, t->seen[A_ID] ? t->val[A_ID] : NULL, t->seen[A_ACTION] ? t->val[A_ACTION] : NULL,
            t->seen[A_ACTION] && t->over[A_ACTION], t->seen[A_METHOD] ? t->val[A_METHOD] : NULL,
            !t->seen[A_ACCEPT] ? NULL : t->over[A_ACCEPT] ? "too-long" : t->val[A_ACCEPT]);
}

/* Elements whose content is in no form, even under the hidden attribute: not object and applet, whose content is what shows
   when they cannot, and a browser's form holds what is in them */
static int inert(int el) { return (ut_html_flags(el) & F_HIDE) && el != E_OBJECT && el != E_APPLET; }

/* A tag inside what the hidden attribute hides, which is still part of the page's forms: a form starts and ends there, an
   input is a control with no box, as a text area is (raw()). A select or a submit button would take more, and a disabled
   fieldset and a <base> are not followed in there, so a form that may hold what depends on them is not sent. What is
   inert (above) stays so. */
static void hidden_forms(struct bd *b, struct ut_html_tag *t, int whole) {
    int el = t->el;
    if(b->skip_inert) {
        if(el == b->inert_el) {
            if(t->end) b->skip_inert--;
            else if(!whole) b->skip_inert++;
        }
        if(el == E_FORM && t->end && b->inert_el != E_TEMPLATE) fm_form_end(&b->fb);
        return;
    }
    if(inert(el) && !t->end) {
        if(!whole) {
            b->skip_inert = 1;
            b->inert_el = el;
        }
        return;
    }
    if(el == E_FORM && t->end) fm_form_end(&b->fb);
    if(el == E_FIELDSET && t->end && b->skip_fields) b->skip_fields--;
    if(t->end) return;
    if(el == E_FORM) form_start(b, t);
    else if(el == E_INPUT) input(b, t);
    else if(el == E_SELECT || (el == E_BUTTON && !attr_is(t, A_TYPE, "button") && !attr_is(t, A_TYPE, "reset")))
        fm_lost(&b->d, &b->fb, t->seen[A_FORM] ? t->val[A_FORM] : NULL);
    else if(el == E_FIELDSET && (t->seen[A_DISABLED] || b->skip_fields)) {
        b->skip_off = 1;
        if(!whole) b->skip_fields++;
    } else if(el == E_BASE && t->seen[A_HREF] && !b->base_seen) b->fb.lost = 1;
}

static void start_tag(struct bd *b, struct ut_html_tag *t) {
    int el = t->el, s;
    unsigned int fl = ut_html_flags(el);
    struct frame *f;
    struct wd_block *x;
    b->css_at = ds_height(b->ds);
    if(el == E_FORM && b->fb.open >= 0) return;     /* the standard: a form in a form is no element at all */
    if(t->seen[A_ID]) add_anchor(b, t->val[A_ID], t->len[A_ID], t->over[A_ID]);
    if(t->seen[A_HIDDEN] && !(fl & F_VOID) && el != E_OTHER && !css_shown(b, t)) {   /* <div hidden>: not shown, nor what is in it */
        start_hidden(b, t);
        b->skip_live = 1;
        if(el == E_LEGEND && b->fb.off) b->skip_off = 1;
        hidden_forms(b, t, 0);
        return;
    }
    if(fl & F_CLOSEP) close_p(b);
    if(el == E_HTML || el == E_BODY) css_root(b, t);
    if(!(fl & F_PUSH) && css_inline(b, t, fl)) {
        if(el == E_INPUT) hidden_input(b, t);
        return;
    }
    if((s = style_of(t)) >= 0) {
        b->is.sty[s]++;
        return;
    }
    if(el == E_FORM) form_start(b, t);
    if(el == E_FIELDSET && t->seen[A_DISABLED]) b->fb.off++;   /* given back at its end (pop()), when it has a box */
    switch(el) {
    case E_A:
        if(t->seen[A_NAME]) add_anchor(b, t->val[A_NAME], t->len[A_NAME], t->over[A_NAME]);
        link_start(b, t);
        return;
    case E_FONT:
        font_start(b, t);
        return;
    case E_UL: case E_OL: case E_MENU: case E_DIR:
        list_start(b, t);
        return;
    case E_LI:
        item_start(b, t);
        return;
    case E_DT: case E_DD:
        close_item(b, E_DT);
        f = push(b, el, WB_BLOCK);
        if(f && el == E_DD) box_of(b, f)->flags = WBF_INDENT;
        return;
    case E_H1: case E_H2: case E_H3: case E_H4: case E_H5: case E_H6:
        if(heading(b->st[b->depth - 1].el)) pop(b);     /* headings do not nest */
        f = push(b, el, WB_BLOCK);
        if(!f) return;
        b->headings++;
        x = box_of(b, f);
        x->flags = WBF_PARA;
        x->level = (unsigned char)(el - E_H1 + 1);
        x->align = (unsigned char)align_of(t);
        return;
    case E_PRE: case E_LISTING:
        f = push(b, el, WB_BLOCK);
        if(f) {
            box_of(b, f)->flags = WBF_PARA | WBF_PRE;
            b->pre++;
            b->col = 0;
        }
        if(b->pos < b->len && b->in[b->pos] == '\r') b->pos++;     /* a newline right after <pre> */
        if(b->pos < b->len && b->in[b->pos] == '\n') b->pos++;
        return;
    case E_BR:
        line_break(b);
        return;
    case E_HR:
        b->css_box = add_box(b, WB_HR, box_now(b));
        return;
    case E_IMG: case E_IMAGE:
        image(b, t);
        return;
    case E_INPUT:
        input(b, t);
        return;
    case E_BUTTON:
        field_close(b);
        b->button = !b->none;
        b->fcontrol = control_of(b, t, WF_BUTTON, attr_is(t, A_TYPE, "reset") ? WC_RESET : attr_is(t, A_TYPE, "button") ? WC_PUSH : WC_SUBMIT, 0);
        return;
    case E_SELECT:
        field_close(b);
        b->select = 1;                              /* display: none or not: its options are its form's */
        b->field_none = b->none || b->select_none;
        b->select_none = 0;
        b->option = 0;
        b->fcontrol = control_of(b, t, WF_SELECT, 0, 0);
        return;
    case E_BASE:
        if(t->seen[A_HREF]) {                       /* links take the first that resolves, as html.c's do; of a */
            size_t n = t->len[A_HREF];              /* browser's forms the first there is: when they differ, none goes */
            const char *h = stripped(t->val[A_HREF], &n);
            int first = !b->base_seen;
            b->base_seen = 1;
            if(first && (t->over[A_HREF] || (b->cs == UT_CS_ASCII && memchr(h, '?', n)))) b->fb.lost = 1;
            if(!b->base && !t->over[A_HREF]) {
                char *copy = n ? malloc(n + 1) : NULL;
                struct ut_url *u = copy ? malloc(sizeof(*u)) : NULL;
                char err[64];
                if(n && !u) b->failed = 1;
                if(u) {
                    memcpy(copy, h, n);
                    copy[n] = 0;
                    err[0] = 0;
                    if(!ut_url_resolve(&b->r->url, copy, u, err, sizeof(err))) {
                        b->base = u;
                        if(!first) b->fb.lost = 1;
                    } else {
                        free(u);
                        if(strstr(err, "out of memory")) b->failed = 1;
                        if(first) b->fb.lost = 1;   /* a browser's rules for one are wider than url.c's */
                    }
                }
                free(copy);
            }
        }
        return;
    case E_BODY:
        b->d.bg = attr_color(t, A_BGCOLOR);
        b->d.fg = attr_color(t, A_TEXT);
        b->d.link_color = attr_color(t, A_LINK);
        b->d.vlink_color = attr_color(t, A_VLINK);
        return;
    case E_TABLE:
        table_start(b, t);
        return;
    case E_TR:
        row_start(b, t);
        return;
    case E_TD: case E_TH:
        cell_start(b, t);
        return;
    case E_CAPTION:
        caption_start(b, t);
        return;
    case E_TBODY: case E_THEAD: case E_TFOOT:
        if(table_at(b)) {
            pop_to(b, table_at(b) + 1);
            ds_close(b->ds, b->st[table_at(b)].css + 1);       /* the section before it ends with it */
            section_open(b, el, t);
        }
        return;
    }
    if(fl & F_PUSH) {
        f = push(b, el, kind_of(el));
        if(!f) {
            if(el == E_FIELDSET && t->seen[A_DISABLED]) {           /* no frame to give it back, and nothing to end it */
                b->fb.off--;
                b->fb.lost = 1;
            }
            return;
        }
        if(el == E_FIELDSET && t->seen[A_DISABLED]) f->off = 1;
        if(el == E_LEGEND && b->fb.off) {           /* a browser's first legend of a fieldset is not disabled by it */
            f->legend = 1;
            b->legends++;
        }
        x = box_of(b, f);
        x->align = (unsigned char)(el == E_CENTER ? WA_CENTER : align_of(t));
        if(el == E_BLOCKQUOTE) x->flags = WBF_PARA | WBF_QUOTE;
        else if(fl & F_PARA) x->flags = WBF_PARA;
        if(el == E_ADDRESS) b->is.sty[S_ITALIC]++;     /* given back at its end (end_tag) */
    }
}

static void end_tag(struct bd *b, struct ut_html_tag *t) {
    int el = t->el, i, s;
    unsigned int fl = ut_html_flags(el);
    if(el == E_FORM) fm_form_end(&b->fb);
    if((fl & F_PUSH) && b->over[el]) {             /* one that was too deep to keep */
        b->over[el]--;
        return;
    }
    if(el == E_BUTTON) field_close(b);              /* (in a box of its own, if its CSS gave it one) */
    if(!(fl & F_PUSH)) css_end(b, t);
    if((s = style_of(t)) >= 0) {
        if(b->is.sty[s]) b->is.sty[s]--;
        return;
    }
    switch(el) {
    case E_A:
        link_close(b);
        return;
    case E_FONT:
        if(b->is.ncolors) b->is.ncolors--;
        return;
    case E_BR:                                      /* </br> is <br>, as in browsers */
        line_break(b);
        return;
    case E_P:
        if((i = in_scope(b, E_P, SCOPE))) pop_to(b, i);
        return;
    case E_LI:
        if((i = in_scope(b, E_LI, SCOPE_LIST))) pop_to(b, i);
        return;
    case E_H1: case E_H2: case E_H3: case E_H4: case E_H5: case E_H6:
        if((i = heading_in_scope(b))) pop_to(b, i);
        return;
    case E_BUTTON:
        field_close(b);
        return;
    case E_TABLE: case E_TR: case E_TD: case E_TH: case E_CAPTION:
        table_end(b, el);
        return;
    case E_TBODY: case E_THEAD: case E_TFOOT:
        if(table_at(b)) {
            pop_to(b, table_at(b) + 1);
            ds_close(b->ds, b->st[table_at(b)].css + 1);
        }
        return;
    case E_ADDRESS:
        if((i = in_scope(b, el, SCOPE))) {
            pop_to(b, i);
            if(b->is.sty[S_ITALIC]) b->is.sty[S_ITALIC]--;
        }
        return;
    }
    if((fl & F_PUSH) && (i = in_scope(b, el, SCOPE))) pop_to(b, i);
}

static void dispatch(struct bd *b, struct ut_html_tag *t);

/* A tag inside content that is not shown: only what ends that content matters. In svg and math, the HTML
   elements of the standard's list (p, div, b, ...) end them, as in browsers (html.c's rule). */
static void hidden_tag(struct bd *b, struct ut_html_tag *t, int whole) {
    int el = t->el;
    if(b->skip_foreign && (t->end ? el == E_P || el == E_BR : (ut_html_flags(el) & F_OUT) != 0)) {
        b->skip_el = E_OTHER;
        b->skip_n = 0;
        b->skip_foreign = 0;
        dispatch(b, t);
        return;
    }
    if(b->skip_live) hidden_forms(b, t, whole);
    else if(el == E_FORM && t->end && b->skip_el != E_TEMPLATE) fm_form_end(&b->fb);
    if(b->skip_inert && b->inert_el == E_TEMPLATE) return;     /* a template holds what ends what is around it */
    if(el != b->skip_el) return;
    if(t->end) {
        if(!--b->skip_n) {
            b->skip_el = E_OTHER;
            b->skip_foreign = 0;
            if(b->skip_fields) b->fb.lost = 1;      /* an end tag a browser need not take here: that fieldset may go on */
        }
    } else if(!whole && el != E_FORM) b->skip_n++;             /* (a form in a form is no element) */
}

static void dispatch(struct bd *b, struct ut_html_tag *t) {
    int el = t->el;
    unsigned int fl = ut_html_flags(el);
    int whole = t->selfclose && (b->xhtml || el == E_SVG || el == E_MATH);     /* <x /> is all of x */
    if(b->skip_n) {
        hidden_tag(b, t, whole);
        return;
    }
    if(b->select && select_tag(b, t)) return;
    if(t->end) {
        end_tag(b, t);
        return;
    }
    if(fl & F_HIDE) {
        if(!whole) {
            start_hidden(b, t);
            b->skip_live = !inert(el);
        }
        return;
    }
    start_tag(b, t);
    css_done(b, t, fl);
    if(whole && !(fl & F_VOID)) end_tag(b, t);
}

/**** the loop over the page ****/

static unsigned int next_char(const struct bd *b, size_t *pos, size_t end) { return ut_html_char(b->in, b->cs, pos, end); }

static int tag(struct bd *b, size_t q, int end) {
    size_t next;
    int whole = ut_html_tag(b->in, b->len, b->cs, q, end, &b->tag, &next);
    b->pos = next;
    b->tag_q = q;
    if(whole) dispatch(b, &b->tag);
    return whole;
}

/* A title's text: references decoded, whitespace collapsed, unsafe characters dropped, cut at UT_FIELD_MAX */
static void title_text(struct bd *b, size_t q, size_t end) {
    char t[UT_FIELD_MAX + 1];
    size_t n = 0;
    int gap = 0;
    unsigned char u[4];
    size_t k;
    unsigned int cp;
    while(q < end) {
        cp = b->in[q] == '&' ? ut_html_ref(b->in, &q, end, 0) : next_char(b, &q, end);
        if(ut_html_space(cp) || cp == 0xa0) {
            gap = 1;
            continue;
        }
        if(ut_html_hidden(cp)) continue;
        k = ut_utf8_encode(cp, u);
        if(n + (gap && n) + k > UT_FIELD_MAX) break;
        if(gap && n) t[n++] = ' ';
        gap = 0;
        memcpy(t + n, u, k);
        n += k;
    }
    t[n] = 0;
    free(b->d.title);
    b->d.title = malloc(n + 1);
    if(!b->d.title) b->failed = 1;
    else memcpy(b->d.title, t, n + 1);
}

/* A text area's control, of the tag still in b->tag: its value is its text, characters decoded, in UTF-8, without the
   line feed that may follow the start tag; cut at WD_VALUE_MAX, which marks its form */
static int area_control(struct bd *b, size_t q, size_t end) {
    struct ut_html_tag *t = &b->tag;
    char *v = malloc(WD_VALUE_MAX + 1), *tag_value = t->val[A_VALUE];
    size_t n = 0;
    unsigned char u[4];
    int control, first = 1, cr = 0;
    if(!v) {
        b->failed = 1;
        return -1;
    }
    while(q < end) {
        unsigned int cp = b->in[q] == '&' ? ut_html_ref(b->in, &q, end, 0) : next_char(b, &q, end);
        size_t k;
        if(first && cp == '\n') {
            first = 0;
            continue;
        }
        first = 0;
        if(cr && cp == '\n') {                      /* a CR, which only a reference can be, and its LF: one line end */
            cr = 0;
            continue;
        }
        cr = cp == '\r';
        if(cr) cp = '\n';
        if(!cp) cp = 0xfffd;
        k = ut_utf8_encode(cp, u);
        if(n + k > WD_VALUE_MAX) {
            t->over[A_VALUE] = 1;
            break;
        }
        memcpy(v + n, u, k);
        n += k;
    }
    v[n] = 0;
    t->val[A_VALUE] = v;                        /* the tag's value for this one call: the next tag sets it afresh */
    t->seen[A_VALUE] = 1;
    control = control_of(b, t, WF_AREA, 0, 0);
    t->val[A_VALUE] = tag_value;
    free(v);
    return control;
}

/* The content of a raw text element up to its own end tag: a title (the first one), an xmp's text (as pre)
   and a textarea's (as its field's label) are kept, when nothing hides them */
static void raw(struct bd *b, int el, const char *name) {
    size_t start = b->pos, q;
    size_t end = el == E_SCRIPT ? ut_html_script_end(b->in, b->len, b->pos) : ut_html_raw_end(b->in, b->len, b->pos, name);
    struct frame *f;
    b->pos = end;
    if(b->skip_n) {
        if(el == E_TEXTAREA && b->skip_live && !b->skip_inert) area_control(b, start, end);
        return;
    }
    if(el == E_TITLE && !b->title_seen) {
        b->title_seen = 1;
        title_text(b, start, end);
    } else if(el == E_XMP && !b->select) {
        close_p(b);
        f = push(b, E_XMP, WB_BLOCK);
        if(!f) return;
        box_of(b, f)->flags = WBF_PARA | WBF_PRE;
        css_box(b);
        b->pre++;
        b->col = 0;
        for(q = start; q < end && !b->stopped && !b->failed; ) text_char(b, next_char(b, &q, end));
        pop_to(b, b->depth - 1);
    } else if(el == E_TEXTAREA && !b->select) {
        size_t n = 0;
        int gap = 0;
        char label[LABEL_MAX + 1];
        unsigned int cp;
        int control = area_control(b, start, end);
        for(q = start; q < end && n < 40; ) {
            cp = b->in[q] == '&' ? ut_html_ref(b->in, &q, end, 0) : next_char(b, &q, end);
            label_add(label, &n, &gap, cp);
        }
        label[n] = 0;
        field(b, WF_AREA, label, n, 0, control);
    }
}

static void markup(struct bd *b) {
    const unsigned char *s = b->in;
    size_t p = b->pos, n = b->len;
    struct ut_html_tag *t = &b->tag;
    unsigned char c = p + 1 < n ? s[p + 1] : 0;
    if(c == '!') {
        if(p + 3 < n && s[p + 2] == '-' && s[p + 3] == '-') b->pos = ut_html_comment_end(s, n, p + 4);
        else if(b->skip_foreign && n - p >= 9 && !memcmp(s + p + 2, "[CDATA[", 7))
            b->pos = ut_html_skip_past(s, n, p + 9, "]]>");     /* CDATA exists only in svg and math */
        else b->pos = ut_html_skip_past(s, n, p + 2, ">");     /* <!DOCTYPE ...>, a bogus comment */
    } else if(c == '?') b->pos = ut_html_skip_past(s, n, p + 2, ">");
    else if(c == '/') {
        if(p + 2 < n && ((s[p + 2] | 0x20) >= 'a' && (s[p + 2] | 0x20) <= 'z')) tag(b, p + 2, 1);
        else if(p + 2 < n && s[p + 2] == '>') b->pos = p + 3;
        else if(p + 2 < n) b->pos = ut_html_skip_past(s, n, p + 2, ">");
        else {
            text_char(b, '<');
            text_char(b, '/');
            b->pos = n;
        }
    } else if((c | 0x20) >= 'a' && (c | 0x20) <= 'z') {
        if(tag(b, p + 1, 0) && (ut_html_flags(t->el) & (F_RAW | F_RCDATA)) &&
           !(t->selfclose && (b->xhtml || b->skip_foreign)) && (!b->select || b->skip_n || t->el == E_SCRIPT))
            raw(b, t->el, t->name);
        else if(t->el == E_TEXTAREA && t->selfclose && b->xhtml && (!b->skip_n || (b->skip_live && !b->skip_inert)))
            field(b, WF_AREA, "", 0, 0, area_control(b, b->pos, b->pos));
    } else {
        text_char(b, '<');
        b->pos = p + 1;
    }
}

static void run_html(struct bd *b) {
    const unsigned char *j;
    while(b->pos < b->len && !b->failed) {
        unsigned char c = b->in[b->pos];
        if(c == '<') markup(b);
        else if(b->skip_n || b->stopped || (b->none && !b->opt_open) || (b->select && b->option != 1 && !b->opt_open)) {
            j = memchr(b->in + b->pos, '<', b->len - b->pos);      /* text nobody sees */
            b->pos = j ? (size_t)(j - b->in) : b->len;
        } else if(c == '&') text_char(b, ut_html_ref(b->in, &b->pos, b->len, 0));
        else text_char(b, next_char(b, &b->pos, b->len));
    }
}

static void run_plain(struct bd *b) {
    struct frame *f = push(b, E_PRE, WB_BLOCK);
    if(f) box_of(b, f)->flags = WBF_PRE;
    b->pre++;
    while(b->pos < b->len && !b->failed && !b->stopped) text_char(b, next_char(b, &b->pos, b->len));
}

/**** the document ****/

/* Is an address of another scheme than http and https (html.c's rule, a scheme as RFC 3986 spells one): it is kept
   as written, not usable */
static int other_scheme(const char *h) {
    size_t i = 0;
    if(!alpha((unsigned char)h[0])) return 0;
    while(alnum((unsigned char)h[i]) || h[i] == '+' || h[i] == '-' || h[i] == '.') i++;
    if(h[i] != ':') return 0;
    return !ci_eq(h, i, "http") && !ci_eq(h, i, "https");
}

/* in (UTF-8, or not) as one line of safe text in out (cap bytes, terminated), as html.c's: spaces as ' ', what
   ut_html_hidden() drops left out, a byte that is no UTF-8 as U+FFFD. Returns its length. */
static size_t safe_copy(char *out, size_t cap, const char *in) {
    const unsigned char *s = (const unsigned char *)in;
    size_t n = strlen(in), i = 0, o = 0;
    unsigned int cp;
    while(i < n) {
        i += ut_utf8_decode(s + i, n - i, &cp);
        if(ut_html_space(cp)) cp = ' ';
        else if(ut_html_hidden(cp)) continue;
        if(o + 4 >= cap) break;
        o += ut_utf8_encode(cp, (unsigned char *)out + o);
    }
    out[o] = 0;
    return o;
}

/* Remove tabs and line breaks, and write spaces as %20, as browsers do before they resolve an address */
static char *clean_href(const char *h) {
    size_t n = 0, i;
    char *c;
    for(i = 0; h[i]; i++) n += h[i] == ' ' ? 3 : h[i] == '\t' || h[i] == '\n' || h[i] == '\r' ? 0 : 1;
    c = malloc(n + 1);
    if(!c) return NULL;
    for(n = 0, i = 0; h[i]; i++) {
        if(h[i] == '\t' || h[i] == '\n' || h[i] == '\r') continue;
        if(h[i] == ' ') {
            memcpy(c + n, "%20", 3);
            n += 3;
        } else c[n++] = h[i];
    }
    c[n] = 0;
    return c;
}

static int safe_utf8(const char *s) {
    size_t n = strlen(s), i = 0, k;
    unsigned int cp;
    while(i < n) {
        k = ut_utf8_decode((const unsigned char *)s + i, n - i, &cp);
        if((k == 1 && (unsigned char)s[i] >= 0x80) || ut_html_hidden(cp)) return 0;
        i += k;
    }
    return 1;
}

/* An address as written, against base (a fragment alone against the page itself): the absolute address when it
   resolves to safe text (*usable), else the reference as written, as safe text. malloc'ed; NULL out of memory. */
static char *address(struct bd *b, const char *h, int too_long, int *usable, struct ut_url *u, char *safe) {
    const struct ut_url *base = h[0] == '#' || !b->base ? &b->r->url : b->base;
    char err[128], *c, *out;
    const char *src;
    size_t n;
    int rc = 1;
    if(!too_long && !other_scheme(h)) {
        c = clean_href(h);
        if(!c) return NULL;
        err[0] = 0;
        rc = ut_url_resolve(base, c, u, err, sizeof(err));
        free(c);
        if(rc && strstr(err, "out of memory")) return NULL;
        if(!rc && !safe_utf8(u->text)) rc = 1;
    }
    if(!rc) src = u->text;
    else {
        safe_copy(safe, UT_URL_MAX + 1, h);
        src = safe;
    }
    n = strlen(src);
    out = malloc(n + 1);
    if(!out) return NULL;
    memcpy(out, src, n + 1);
    *usable = !rc;
    return out;
}

static void *shrink(void *p, size_t count, size_t size) {
    void *q;
    if(!p || !count) return p;
    q = realloc(p, count * size);
    return q ? q : p;
}

static void add_notice(char *notice, size_t cap, const char *part) {
    size_t n = strlen(notice);
    snprintf(notice + n, cap - n, "%s%s", n ? "; " : "", part);
}

/* A form's address as written becomes absolute, or NULL when it is none that can be used: what *x points to */
static int form_address(struct bd *b, char **x, int cut, char *safe, struct ut_url *u) {
    int usable = 0;
    size_t n;
    char *a = address(b, *x, cut, &usable, u, safe);
    if(!a) return -1;
    if(**x == '#' && b->base) usable = 0;           /* a place on the base's page: links' rule is not a form's */
    b->fb.bytes -= strlen(*x) + 1;
    free(*x);
    *x = a;
    n = strlen(a) + 1;
    if(usable && n <= WD_FORM_BYTES - b->fb.bytes) b->fb.bytes += n;   /* an address grows here: it stays in the budget */
    else {
        free(a);
        *x = NULL;
    }
    return 0;
}

/* The page's own address, for a form that gives none: *x stays NULL past the budget */
static int page_address(struct bd *b, char **x) {
    size_t n = strlen(b->r->url.text) + 1;
    if(n > WD_FORM_BYTES - b->fb.bytes) return 0;
    *x = malloc(n);
    if(!*x) return -1;
    memcpy(*x, b->r->url.text, n);
    b->fb.bytes += n;
    return 0;
}

/* The forms' addresses, by the base the page ended with, and what fm_finish() does; 0, or -1 out of memory */
static int forms_finish(struct bd *b, char *safe, struct ut_url *u) {
    struct wd_document *d = &b->d;
    size_t i;
    d->form_charset = b->cs;
    if(b->cs == UT_CS_UTF8 && b->undeclared && !b->xhtml) { /* ASCII that says nothing where it was looked for */
        for(i = 0; i < b->len && b->in[i] < 0x80; i++) ;
        if(i == b->len) {
            int cs = ut_html_declared(b->in, b->len);
            d->form_charset = cs < 0 ? UT_CS_1252 : cs;     /* a browser's default, or what it says further on */
        }
    }
    for(i = 0; i < d->form_count; i++) {
        struct wd_form *f = &d->forms[i];
        if(fm_unsure(d, f->action)) f->too_much = 1;
        if(f->here) {                               /* the page's own address, as the standard has it, not the base's */
            if(page_address(b, &f->action)) return -1;
        } else if(f->action && form_address(b, &f->action, f->action_cut, safe, u)) return -1;
    }
    for(i = 0; i < d->control_count; i++) {
        struct wd_control *c = &d->controls[i];
        int given = c->here || c->formaction;
        if(fm_unsure(d, c->formaction)) c->cut = 1;
        if(c->here) {
            if(page_address(b, &c->formaction)) return -1;
        } else if(c->formaction && form_address(b, &c->formaction, c->action_cut, safe, u)) return -1;
        if(given && !c->formaction) c->action_cut = 1;  /* what it cannot use is refused, not the form's address taken */
    }
    fm_finish(d, &b->fb, b->stopped);
    return b->fb.failed ? -1 : 0;
}

static void sheet_free(struct wd_sheet *o) {
    free(o->url);
    free(o->text);
}

void wd_sheets_free(struct wd_sheets *s) {
    size_t i;
    if(!s) return;
    for(i = 0; i < s->count; i++) sheet_free(&s->sheet[i]);
    free(s);
}

/* One more sheet: 0, or -1 without memory; it is left out past the limit, and over http from what came by https */
static int sheet_add(struct wd_sheets *q, const char *url, int source, int imported, int https) {
    struct wd_sheet *o;
    if(q->count == WD_SHEETS_MAX || (https && strncmp(url, "https:", 6))) return 0;
    o = &q->sheet[q->count];
    memset(o, 0, sizeof(*o));
    o->url = malloc(strlen(url) + 1);
    if(!o->url) return -1;
    strcpy(o->url, url);
    o->source = source;
    o->imported = imported;
    q->count++;
    return 0;
}

/* What the page links to, resolved as its links are: in d->sheets when there is any. 0, or -1 out of memory */
static int find_sheets(struct bd *b, struct ut_url *u, char *safe) {
    const char *h;
    int source, imported, usable = 0;
    size_t i;
    for(i = 0; ds_found(b->ds, i, &h, &source, &imported); i++) {
        char *url = address(b, h, 0, &usable, u, safe);
        int rc;
        if(!url) return -1;
        if(!b->d.sheets && !(b->d.sheets = calloc(1, sizeof(*b->d.sheets)))) {
            free(url);
            return -1;
        }
        b->d.sheets->https = b->r->verified;
        rc = usable ? sheet_add(b->d.sheets, url, source, imported, b->r->verified) : 0;
        free(url);
        if(rc) return -1;
    }
    return 0;
}

int wd_sheets_imports(struct wd_sheets *s) {
    struct { struct ut_url base, u; char urls[CSS_IMPORTS_MAX][CSS_URL_MAX + 1], err[128]; } *w;
    size_t i, n = s ? s->count : 0, k;
    int added = 0;
    if(!s) return 0;
    w = malloc(sizeof(*w));
    if(!w) return -1;
    for(i = 0; i < n; i++) {
        struct wd_sheet *o = &s->sheet[i];
        size_t count;
        if(!o->text || o->imported || ut_url_parse(o->url, &w->base, w->err, sizeof(w->err))) continue;
        count = css_imports(o->text, o->len, w->urls, CSS_IMPORTS_MAX);
        for(k = 0; k < count; k++) {
            char *c = clean_href(w->urls[k]);
            int rc;
            if(!c) {
                free(w);
                return -1;
            }
            rc = other_scheme(c) || ut_url_resolve(&w->base, c, &w->u, w->err, sizeof(w->err)) || !safe_utf8(w->u.text);
            free(c);
            if(rc) continue;
            {
                size_t before = s->count;
                if(sheet_add(s, w->u.text, o->source, 1, s->https || !strncmp(o->url, "https:", 6))) {
                    free(w);
                    return -1;
                }
                added += s->count > before;
            }
        }
    }
    free(w);
    return added;
}

/* The links' and images' addresses, the budget of the link strings, the notice; 0, or -1 out of memory */
static int finish(struct bd *b) {
    struct wd_document *d = &b->d;
    struct { struct ut_url u; char safe[UT_URL_MAX + 1]; } *work = malloc(sizeof(*work));
    size_t i, keep, bytes = b->link_bytes;
    if(!work) return -1;
    link_close(b);
    field_close(b);
    for(i = 0; i < d->link_count && !b->failed; i++) {
        struct ut_link *l = &d->links[i];
        if(!l->label && !(l->label = calloc(1, 1))) break;
        l->url = address(b, b->hrefs[i], b->long_href[i], &l->usable, &work->u, work->safe);
        if(!l->url) break;
    }
    if(i < d->link_count) {
        free(work);
        return -1;
    }
    for(keep = 0; keep < d->link_count; keep++) {   /* the link strings within UT_LINK_BYTES_MAX, in order */
        size_t need = strlen(d->links[keep].label) + 1 + strlen(d->links[keep].url) + 1;
        if(b->link_bytes_full || bytes + need > UT_LINK_BYTES_MAX) break;
        bytes += need;
    }
    if(keep < d->link_count) {                      /* the rest are no links: their text stays */
        d->truncated |= UT_TRUNC_LINKS;
        for(i = keep; i < d->link_count; i++) {
            free(d->links[i].label);
            free(d->links[i].url);
        }
        for(i = 0; i < d->run_count; i++) if(d->runs[i].link >= (int)keep) d->runs[i].link = -1;
        d->link_count = keep;
    }
    if(find_sheets(b, &work->u, work->safe)) {
        free(work);
        return -1;
    }
    d->images = calloc(d->image_count ? d->image_count : 1, sizeof(*d->images));
    if(!d->images) {
        free(work);
        return -1;
    }
    if(forms_finish(b, work->safe, &work->u)) {
        free(work);
        return -1;
    }
    d->pics = calloc(d->image_count ? d->image_count : 1, sizeof(*d->pics));
    if(!d->pics) {
        free(work);
        return -1;
    }
    for(i = 0; i < d->image_count; i++) {
        int usable = 0;
        char *u = address(b, b->srcs[i], 0, &usable, &work->u, work->safe);
        if(!u) {
            free(work);
            return -1;
        }
        if(usable) d->images[i] = u;
        else {
            free(u);
            d->pics[i].state = WP_REFUSED;
        }
    }
    free(work);
    if(!d->title && !(d->title = calloc(1, 1))) return -1;
    if(!d->text && !(d->text = calloc(1, 1))) return -1;
    d->notice[0] = 0;
    if(b->cs == UT_CS_ASCII) {
        char part[UT_HTML_LABEL_MAX + 64];
        snprintf(part, sizeof(part), "the page says it is in %s; only its ASCII is shown", b->charset);
        add_notice(d->notice, sizeof(d->notice), part);
    }
    if(d->truncated & (UT_TRUNC_TEXT | WD_TRUNC_BOXES)) add_notice(d->notice, sizeof(d->notice), cut_notice);
    if(d->truncated & UT_TRUNC_LINKS) add_notice(d->notice, sizeof(d->notice), "some links are not links");
    if(ds_finish(b->ds, d)) add_notice(d->notice, sizeof(d->notice), "some of the page's style is left out");
    d->blocks = shrink(d->blocks, d->block_count, sizeof(*d->blocks));
    d->runs = shrink(d->runs, d->run_count, sizeof(*d->runs));
    d->text = shrink(d->text, d->text_len + 1, 1);
    d->bytes = d->block_count * sizeof(*d->blocks) + d->run_count * sizeof(*d->runs) + d->text_len + 1 +
               strlen(d->title) + 1 + (d->image_count ? d->image_count : 1) * (sizeof(*d->images) + sizeof(*d->pics));
    if(d->link_count) d->bytes += b->links_cap * sizeof(*d->links);
    for(i = 0; i < d->link_count; i++) d->bytes += strlen(d->links[i].label) + 1 + strlen(d->links[i].url) + 1;
    if(d->anchor_count) d->bytes += b->anchors_cap * sizeof(*d->anchors);
    for(i = 0; i < d->anchor_count; i++) d->bytes += strlen(d->anchors[i].name) + 1;
    for(i = 0; i < d->image_count; i++) if(d->images[i]) d->bytes += strlen(d->images[i]) + 1;
    d->bytes += d->form_bytes;
    if(d->sheets) d->bytes += sizeof(*d->sheets);
    for(i = 0; d->sheets && i < d->sheets->count; i++) d->bytes += strlen(d->sheets->sheet[i].url) + 1;
    return b->failed ? -1 : 0;
}

static void cleanup(struct bd *b, int given) {
    size_t i;
    for(i = 0; i < b->nhrefs; i++) free(b->hrefs[i]);
    free(b->hrefs);
    free(b->long_href);
    for(i = 0; i < b->nsrcs; i++) free(b->srcs[i]);
    free(b->srcs);
    free(b->decoded);
    free(b->base);
    ds_free(b->ds);
    if(!given) wd_free(&b->d);
    free(b);
}

int wd_build_with(const struct ut_response *r, const struct wd_sheets *sheets, struct wd_document *out, char *err, size_t cap) {
    struct bd *b;
    int html, rc;
    unsigned char *dec;
    size_t declen;
    if(!r || !out || (!r->body && r->len)) return ut_error(err, cap, "there is no page to show");
    b = calloc(1, sizeof(*b));
    if(!b) return ut_error(err, cap, "out of memory showing the page");
    b->r = r;
    b->in = r->body ? r->body : (const unsigned char *)"";
    b->len = r->len;
    b->depth = 1;
    b->st[0].el = E_HTML;
    b->target = -1;
    b->link = -1;
    b->skip_el = E_OTHER;
    b->fcontrol = -1;
    fm_begin(&b->fb);
    b->d.bg = b->d.fg = b->d.link_color = b->d.vlink_color = WD_NONE;
    b->tag.attrs = attr_info;
    b->tag.nattrs = A_COUNT;
    b->tag.val[A_HREF] = b->tagbuf.href;
    b->tag.val[A_ID] = b->tagbuf.id;
    b->tag.val[A_NAME] = b->tagbuf.name;
    b->tag.val[A_ALT] = b->tagbuf.alt;
    b->tag.val[A_VALUE] = b->tagbuf.value;
    b->tag.val[A_TYPE] = b->tagbuf.type;
    b->tag.val[A_START] = b->tagbuf.start;
    b->tag.val[A_SRC] = b->tagbuf.src;
    b->tag.val[A_WIDTH] = b->tagbuf.width;
    b->tag.val[A_HEIGHT] = b->tagbuf.height;
    b->tag.val[A_ALIGN] = b->tagbuf.align;
    b->tag.val[A_COLOR] = b->tagbuf.color;
    b->tag.val[A_BGCOLOR] = b->tagbuf.bgcolor;
    b->tag.val[A_TEXT] = b->tagbuf.text;
    b->tag.val[A_LINK] = b->tagbuf.link;
    b->tag.val[A_VLINK] = b->tagbuf.vlink;
    b->tag.val[A_BORDER] = b->tagbuf.border;
    b->tag.val[A_COLSPAN] = b->tagbuf.colspan;
    b->tag.val[A_CELLPADDING] = b->tagbuf.cellpadding;
    b->tag.val[A_CHECKED] = b->tagbuf.checked;
    b->tag.val[A_PLACEHOLDER] = b->tagbuf.placeholder;
    b->tag.val[A_HIDDEN] = b->tagbuf.hidden;
    b->tag.val[A_ARIA] = b->tagbuf.aria;
    b->tag.val[A_TITLE] = b->tagbuf.title;
    b->tag.val[A_SELECTED] = b->tagbuf.selected;
    b->tag.val[A_ACTION] = b->tagbuf.action;
    b->tag.val[A_METHOD] = b->tagbuf.method;
    b->tag.val[A_FORM] = b->tagbuf.form;
    b->tag.val[A_FORMACTION] = b->tagbuf.formaction;
    b->tag.val[A_FORMMETHOD] = b->tagbuf.formmethod;
    b->tag.val[A_DISABLED] = b->tagbuf.disabled;
    b->tag.val[A_READONLY] = b->tagbuf.readonly;
    b->tag.val[A_MAXLENGTH] = b->tagbuf.maxlength;
    b->tag.val[A_ROWS] = b->tagbuf.rows;
    b->tag.val[A_COLS] = b->tagbuf.cols;
    b->tag.val[A_SIZE] = b->tagbuf.size;
    b->tag.val[A_MULTIPLE] = b->tagbuf.multiple;
    b->tag.val[A_LABEL] = b->tagbuf.label;
    b->tag.val[A_ACCEPT] = b->tagbuf.accept;
    b->tag.val[A_DIRNAME] = b->tagbuf.dirname;
    b->tag.val[A_WRAP] = b->tagbuf.wrap;
    if(new_box(b, WB_BLOCK, -1) != 0) {             /* the root */
        cleanup(b, 0);
        return ut_error(err, cap, "out of memory showing the page");
    }
    b->st[0].box = 0;
    rc = ut_html_utf16(b->in, b->len, r->charset, &dec, &declen);
    if(rc < 0) {
        cleanup(b, 0);
        return ut_error(err, cap, "out of memory showing the page");
    }
    if(rc) {
        b->decoded = dec;
        b->in = dec;
        b->len = declen;
        b->cs = UT_CS_UTF8;
    }
    b->xhtml = !strcmp(r->content_type, "application/xhtml+xml");
    html = b->xhtml || !strcmp(r->content_type, "text/html") || (!r->content_type[0] && ut_html_sniff(b->in, b->len));
    if(!b->decoded) b->cs = ut_html_charset(b->in, b->len, r->charset, html, &b->pos, b->charset, sizeof(b->charset));
    b->undeclared = !b->decoded && !r->charset[0] && html;
    b->css_box = -1;
    b->ds = ds_new(b->in, html ? b->len : 0, b->cs, sheets);
    if(!b->ds) {
        cleanup(b, 0);
        return ut_error(err, cap, "out of memory showing the page");
    }
    b->none = b->page_none = ds_hidden(b->ds);
    if(html) run_html(b);
    else run_plain(b);
    if(b->failed || finish(b)) {
        cleanup(b, 0);
        return ut_error(err, cap, "out of memory showing the page");
    }
    if(sheets) {                                    /* (it has what it asked for) */
        wd_sheets_free(b->d.sheets);
        b->d.sheets = NULL;
    }
    *out = b->d;
    memset(&b->d, 0, sizeof(b->d));
    cleanup(b, 1);
    return 0;
}

int wd_build(const struct ut_response *r, struct wd_document *out, char *err, size_t cap) {
    return wd_build_with(r, NULL, out, err, cap);
}

void wd_free(struct wd_document *d) {
    size_t i;
    if(!d) return;
    wd_sheets_free(d->sheets);
    for(i = 0; d->links && i < d->link_count; i++) {
        free(d->links[i].label);
        free(d->links[i].url);
    }
    free(d->links);
    for(i = 0; d->anchors && i < d->anchor_count; i++) free(d->anchors[i].name);
    free(d->anchors);
    for(i = 0; d->images && i < d->image_count; i++) free(d->images[i]);
    free(d->images);
    for(i = 0; d->pics && i < d->image_count; i++) {
        free(d->pics[i].px);
        free(d->pics[i].scaled);
    }
    free(d->pics);
    free(d->visited);
    fm_free(d);
    free(d->blocks);
    free(d->runs);
    free(d->text);
    free(d->title);
    memset(d, 0, sizeof(*d));
}

static int hexv(char c) { return hexdigit((unsigned char)c); }

/* name and fragment are the same once fragment's %XX escapes are decoded (once); a %00 matches nothing, so the
   name's end is never passed */
static int same_decoded(const char *name, const char *fragment) {
    while(*fragment) {
        char c = *fragment;
        if(c == '%' && hexv(fragment[1]) >= 0 && hexv(fragment[2]) >= 0) {
            c = (char)(hexv(fragment[1]) << 4 | hexv(fragment[2]));
            fragment += 3;
        } else fragment++;
        if(!c || *name++ != c) return 0;
    }
    return !*name;
}

/* as Undertow's ut_anchor_offset (html.c) and a browser: an anchor of the name, then of the name with its escapes
   decoded, and only then "top" (or nothing) for the top */
int wd_anchor_offset(const struct wd_document *d, const char *fragment, size_t *offset) {
    size_t i;
    if(!d || !fragment || !offset) return -1;
    for(i = 0; i < d->anchor_count; i++) if(!strcmp(d->anchors[i].name, fragment)) {
        *offset = d->anchors[i].offset;
        return 0;
    }
    if(strchr(fragment, '%'))
        for(i = 0; i < d->anchor_count; i++) if(same_decoded(d->anchors[i].name, fragment)) {
            *offset = d->anchors[i].offset;
            return 0;
        }
    if(!fragment[0] || ci_eq(fragment, strlen(fragment), "top")) {
        *offset = 0;
        return 0;
    }
    return -1;
}

/**** the dump (the host tests' fixtures) ****/

struct out { char *s; size_t n, cap; int failed; };

static void outf(struct out *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#include <stdarg.h>
static void outf(struct out *o, const char *fmt, ...) {
    va_list ap;
    int k;
    if(o->failed) return;
    for(;;) {
        va_start(ap, fmt);
        k = vsnprintf(o->s + o->n, o->cap - o->n, fmt, ap);
        va_end(ap);
        if(k < 0) {
            o->failed = 1;
            return;
        }
        if(o->n + (size_t)k < o->cap) {
            o->n += (size_t)k;
            return;
        }
        {
            size_t cap = o->cap * 2 + (size_t)k + 64;
            char *p = realloc(o->s, cap);
            if(!p) {
                o->failed = 1;
                return;
            }
            o->s = p;
            o->cap = cap;
        }
    }
}

static void out_text(struct out *o, const char *s, size_t n) {
    size_t i;
    outf(o, "\"");
    for(i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if(c == '"' || c == '\\') outf(o, "\\%c", c);
        else if(c < 0x20) outf(o, "\\x%02x", c);
        else outf(o, "%c", c);
    }
    outf(o, "\"");
}

static void dump_box(struct out *o, const struct wd_document *d, int i, int depth) {
    static const char *const kinds[] = { "block", "item", "table", "row", "cell", "hr" };
    static const char *const aligns[] = { "", " left", " center", " right" };
    static const char *const markers[] = { "", " disc", " circle", " square", " decimal", " lower-alpha",
                                           " upper-alpha", " lower-roman", " upper-roman" };
    static const char *const fields[] = { "text", "password", "button", "checkbox", "radio", "select", "area", "file" };
    const struct wd_block *x = &d->blocks[i];
    unsigned int r;
    int c;
    outf(o, "%*s%s", depth * 2, "", kinds[x->kind]);
    if(x->level) outf(o, " h%d", x->level);
    if(x->flags & WBF_PARA) outf(o, " para");
    if(x->flags & WBF_INDENT) outf(o, " indent");
    if(x->flags & WBF_QUOTE) outf(o, " quote");
    if(x->flags & WBF_PRE) outf(o, " pre");
    if(x->flags & WBF_ANON) outf(o, " anon");
    if(x->flags & WBF_LIST) outf(o, " list");
    if(x->flags & WBF_HEADER) outf(o, " header");
    if(x->flags & WBF_CAPTION) outf(o, " caption");
    outf(o, "%s%s", aligns[x->align], markers[x->marker]);
    if(x->marker >= WMK_DECIMAL) outf(o, " %ld", x->number);
    if(x->colspan > 1) outf(o, " colspan=%u", x->colspan);
    if(x->kind == WB_TABLE) outf(o, " border=%u padding=%u", x->border, x->padding);
    if(x->width) outf(o, " width=%d%s", x->width < 0 ? -x->width : x->width, x->width < 0 ? "%" : "");
    if(x->bg != WD_NONE) outf(o, " bg=%06x", x->bg);
    if(x->given) {
        int k;
        for(k = 0; k < 4; k++) if(x->given & (1 << k)) outf(o, " m%c=%d", "trbl"[k], x->margin[k]);
        for(k = 0; k < 4; k++) if(x->given & (16 << k)) outf(o, " p%c=%d", "trbl"[k], x->pad[k]);
    }
    outf(o, "\n");
    for(r = x->run; r < x->run + x->nruns; r++) {
        const struct wd_run *u = &d->runs[r];
        outf(o, "%*s", depth * 2 + 2, "");
        if(u->kind == WR_BR) outf(o, "br");
        else {
            outf(o, "%s", u->kind == WR_TEXT ? "text" : u->kind == WR_IMAGE ? "image" : fields[u->field]);
            if(u->kind == WR_FIELD) outf(o, " field");
            if(u->style & WS_BOLD) outf(o, " b");
            if(u->style & WS_ITALIC) outf(o, " i");
            if(u->style & WS_MONO) outf(o, " mono");
            if(u->style & WS_UNDER) outf(o, " u");
            if(u->style & WS_STRIKE) outf(o, " s");
            if(u->style & WS_SUB) outf(o, " sub");
            if(u->style & WS_SUP) outf(o, " sup");
            if(u->style & WS_PRE) outf(o, " pre");
            if(u->style & WS_NOWRAP) outf(o, " nowrap");
            if(u->style & WS_MARK) outf(o, " mark");
            if(u->style & WS_NOLINE) outf(o, " noline");
            if(u->size >= WZ_H1 && u->size <= WZ_H6) outf(o, " h%d", u->size);
            if(u->size == WZ_BIG) outf(o, " big");
            if(u->size == WZ_SMALL) outf(o, " small");
            if(u->px) outf(o, " px=%u", u->px);
            if(u->color == WD_TEXT) outf(o, " color=text");
            else if(u->color != WD_NONE) outf(o, " color=%06x", u->color);
            if(u->bg != WD_NONE) outf(o, " bg=%06x", u->bg);
            if(u->checked) outf(o, " checked");
            if(u->w || u->h) outf(o, " %ux%u", u->w, u->h);
            if(u->image >= 0) outf(o, " src=%s", d->images[u->image] ? d->images[u->image] : "(unusable)");
            outf(o, " ");
            out_text(o, d->text + u->off, u->len);
        }
        if(u->link >= 0) outf(o, " link=%d", u->link);
        outf(o, "\n");
    }
    for(c = x->first; c >= 0; c = d->blocks[c].next) dump_box(o, d, c, depth + 1);
}

char *wd_dump(const struct wd_document *d) {
    struct out o = { NULL, 0, 0, 0 };
    size_t i;
    o.cap = 256;
    o.s = malloc(o.cap);
    if(!o.s) return NULL;
    o.s[0] = 0;
    outf(&o, "title ");
    out_text(&o, d->title, strlen(d->title));
    outf(&o, "\n");
    if(d->bg != WD_NONE || d->fg != WD_NONE || d->link_color != WD_NONE)
        outf(&o, "colors bg=%06x fg=%06x link=%06x\n", d->bg & 0xffffff, d->fg & 0xffffff, d->link_color & 0xffffff);
    if(d->block_count) dump_box(&o, d, 0, 0);
    for(i = 0; i < d->link_count; i++) {
        outf(&o, "link %u%s ", (unsigned int)i, d->links[i].usable ? "" : " unusable");
        out_text(&o, d->links[i].url, strlen(d->links[i].url));
        outf(&o, " ");
        out_text(&o, d->links[i].label, strlen(d->links[i].label));
        outf(&o, "\n");
    }
    for(i = 0; i < d->anchor_count; i++) outf(&o, "anchor %s %u\n", d->anchors[i].name, (unsigned int)d->anchors[i].offset);
    if(d->notice[0]) outf(&o, "notice %s\n", d->notice);
    if(o.failed) {
        free(o.s);
        return NULL;
    }
    return o.s;
}
