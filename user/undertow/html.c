/*
SurfOS Undertow: HTML (and plain text) as readable text with numbered links
----------------------
File: html.c  Date: 10/11/26
----------------------
html.h's renderer, in one pass over the body with no recursion and no tree:

- a tokenizer after the HTML standard's: comments and bogus comments, raw
  text elements (script, style, title, textarea, ...) that end only at their
  own end tag, attributes quoted or not, character references;
- a small stack of the open elements that shape the text (paragraphs, lists,
  tables, headings, ...) with the standard's rules for closing them, so that
  unclosed and mismatched tags still give sensible text; inline elements are
  not kept at all;
- a writer that collapses whitespace, starts lines, indents lists and quotes,
  joins table cells with " | ", numbers links after their labels, and lets no
  unsafe character (ut_unsafe_char()) into anything it makes.

Everything is bounded: the stack (deeper elements are only counted), each
attribute that is kept, the text, the links, the anchors, the spans, the
title. Whatever is past a bound is skipped in time linear in the input.

10/25/26 (the browser's window, wundertow): the tokenizer's parts that do
not depend on the text (the elements and their flags, the character
references, the charsets, a tag's attributes, the ends of raw text and
comments) moved to html_scan.c, which the window's reader shares; the loop
over them, the stack and the writer stay here, unchanged.
*/

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "undertow.h"
#include "url.h"
#include "html.h"
#include "html_scan.h"

#define STACK_MAX    128                /* open elements kept; deeper ones are only counted */
#define INDENT_MAX   32                 /* columns of indentation at most, however deep the lists */
#define HREF_MAX     4096               /* bytes of an address kept from an attribute (url.c takes 2048) */
#define LABEL_MAX    UT_HTML_LABEL_MAX  /* bytes of a charset's name kept for the notice */
#define HEADING_SPANS (UT_SPANS_MAX - UT_LINKS_MAX)     /* every link keeps room for its [N] */
#define AHASH        (2 * UT_ANCHORS_MAX)   /* slots of the anchor names' hash table: never more than half full */

typedef char heading_room[UT_SPANS_MAX > UT_LINKS_MAX ? 1 : -1];    /* headings keep spans of their own */
#define RULE         "----------------------------------------"         /* <hr>: 40 of them */

static const char cut_notice[] = "[Undertow: the rest of this page is not shown: it is over 512 KB]";

enum { CS_UTF8 = UT_CS_UTF8, CS_1252 = UT_CS_1252, CS_ASCII = UT_CS_ASCII };

/* the elements and their flags (F_*, E_*), the character references and the charsets: html_scan.c */

/**** the renderer's state ****/

struct frame {                          /* an open element that shapes the text */
    unsigned char el;
    unsigned char nested;               /* a list inside a list: no blank lines around it */
    unsigned char cell_pending;         /* td, th: it has shown nothing yet */
    unsigned char row_shown;            /* tr: one of its cells has shown something */
    unsigned char data;                 /* table: it has had a <th>, so every cell keeps its column */
    unsigned short text;                /* the indentation of the lines of its text */
    unsigned short mcol;                /* lists and items: the column of the markers */
    short lst;                          /* the nearest list or item around it (no quote between), or -1 */
    long next;                          /* ol: the number of its next item */
};

enum { A_HREF, A_ID, A_NAME, A_ALT, A_VALUE, A_TYPE, A_START, A_ARIA, A_TITLE, A_COUNT };

static const struct ut_html_attr attr_info[A_COUNT] = {
    { "href", HREF_MAX }, { "id", UT_FIELD_MAX + 1 }, { "name", UT_FIELD_MAX + 1 }, { "alt", UT_FIELD_MAX },
    { "value", UT_FIELD_MAX }, { "type", 32 }, { "start", 32 }, { "aria-label", UT_FIELD_MAX }, { "title", UT_FIELD_MAX }
};

struct tag_bufs {                       /* the buffers of the tag's attributes (attr_info) */
    char href[HREF_MAX + 1], id[UT_FIELD_MAX + 2], name_[UT_FIELD_MAX + 2], alt[UT_FIELD_MAX + 1],
         value[UT_FIELD_MAX + 1], type[33], start[33], aria[UT_FIELD_MAX + 1], title[UT_FIELD_MAX + 1];
};

struct rd {
    const struct ut_response *r;
    const unsigned char *in;
    size_t len, pos;
    int cs;                             /* CS_UTF8, CS_1252, CS_ASCII (a charset Undertow cannot read) */
    int xhtml;                          /* <tag /> is a whole element */
    char charset[LABEL_MAX];            /* the name of a charset Undertow cannot read */

    /* the text */
    char *text;
    size_t tlen, tcap, limit;
    size_t line_start;                  /* where the last line starts */
    unsigned int col;                   /* characters on it after the indentation */
    int breaks;                         /* line breaks wanted before the next text: 1 a new line, 2 a blank line too */
    int space;                          /* a collapsible space is wanted before the next text */
    int stopped;                        /* the text is full: nothing more is shown */
    int failed;                         /* out of memory */
    int truncated;                      /* UT_TRUNC_* */
    int pre;                            /* inside pre, listing or xmp (or plain text) */
    int headings;                       /* open h1-h6 */
    int tables;                         /* open tables */
    int cells_pending;                  /* open cells that have shown nothing yet: no line breaks for them */
    int sep;                            /* the next text continues a row: " | " first */
    char marker[16];                    /* a list item's marker, waiting for its first text */
    int marker_on;
    unsigned int marker_col;
    int marker_frame;

    /* open elements */
    struct frame st[STACK_MAX];         /* st[0] is the root */
    int depth;
    unsigned short open[E_COUNT];       /* how many of each kind are on the stack */
    unsigned int over[E_COUNT];         /* and how many more were too deep to be kept */
    unsigned int hide[E_COUNT];         /* open elements whose content is not shown */
    int skip;                           /* their sum */

    /* links, buttons, selects */
    int link_open, link_num, link_text; /* link_num 0: a link without a number */
    char label[UT_FIELD_MAX + 1];
    size_t label_len;
    char link_alt[UT_FIELD_MAX + 1];    /* its text if it shows none: aria-label or title, its own or inside it */
    size_t link_alt_len;
    int label_gap, label_full;
    int button;                         /* 1: open, nothing shown yet; 2: "[button: " shown */
    int select, option, select_shown;   /* option 1: in the first one; 2: past it */
    int forms;

    /* what the document gets */
    struct ut_link *links;
    char **hrefs;                       /* each link's address as written, until the end */
    unsigned char long_href[UT_LINKS_MAX];  /* it was longer than HREF_MAX: unusable */
    size_t nlinks, links_cap;
    size_t link_bytes;                  /* the links' labels and addresses and the anchors' names, as allocated */
    int link_bytes_full;                /* they reached UT_LINK_BYTES_MAX: no more numbers, no more anchors */
    struct work { struct ut_url u; char safe[UT_URL_MAX + 1]; } *work;     /* for resolving addresses */
    struct ut_anchor *anchors;
    size_t nanchors, anchors_cap, aplaced;  /* anchors[aplaced..] wait for the next text */
    unsigned short ahash[AHASH];        /* index + 1 into anchors, by name */
    struct ut_span *spans;
    size_t nspans, spans_cap, heading_spans;
    int span_on;
    size_t span_start;
    char title[UT_FIELD_MAX + 1];
    size_t title_len;
    int title_seen;
    struct ut_url *base;                /* the first <base href> that resolved */
    int base_done;                      /* that one was seen, or was known before the page was read */
    int again;                          /* that one came late and was too big for the links before it: the page is
                                           read again with it known from the start (failed is set too, to stop) */
    unsigned char *decoded;             /* a UTF-16 body as UTF-8 */

    int given;                          /* the document has taken the text, links, anchors and spans */

    struct ut_html_tag tag;             /* the tag being read */
    struct tag_bufs tagbuf;
    char scratch[UT_FIELD_MAX + 16];
};

/**** small helpers ****/

static int ws(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; }
static int alpha(unsigned char c) { return (c | 0x20) >= 'a' && (c | 0x20) <= 'z'; }
static int digit(unsigned char c) { return c >= '0' && c <= '9'; }
static int alnum(unsigned char c) { return alpha(c) || digit(c); }
static unsigned char lower(unsigned char c) { return c >= 'A' && c <= 'Z' ? (unsigned char)(c + 32) : c; }

static int hexval(unsigned char c) {
    if(digit(c)) return c - '0';
    c |= 0x20;
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* s (n bytes) is the lowercase word w in any case */
static int ci_eq(const unsigned char *s, size_t n, const char *w) {
    size_t i;
    for(i = 0; i < n; i++) if(!w[i] || lower(s[i]) != (unsigned char)w[i]) return 0;
    return !w[n];
}

static void copy_cut(char *dst, size_t cap, const char *src) {    /* src, cut to cap - 1 bytes */
    size_t n = strlen(src);
    if(n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static int hidden_char(unsigned int cp) { return ut_html_hidden(cp); }     /* html_scan.c */

static int space_char(unsigned int cp) { return ut_html_space(cp); }

/* The bytes of s (n of them) that are characters' first bytes: its width on the console */
static unsigned int width(const char *s, size_t n) {
    unsigned int w = 0;
    size_t i;
    for(i = 0; i < n; i++) if(((unsigned char)s[i] & 0xc0) != 0x80) w++;
    return w;
}

/**** the page's characters ****/

/* The character at in[*pos] (before end), and a character reference there: html_scan.c's */
static unsigned int next_char(const struct rd *rd, size_t *pos, size_t end) {
    return ut_html_char(rd->in, rd->cs, pos, end);
}

static unsigned int char_ref(const struct rd *rd, size_t *pos, size_t end, int attr) {
    return ut_html_ref(rd->in, pos, end, attr);
}

/**** which charset (html_scan.c) ****/

/* A UTF-16 body as UTF-8, which the renderer then reads: 0, or -1 when memory is out */
static int decode_utf16(struct rd *rd) {
    unsigned char *out;
    size_t n;
    int r = ut_html_utf16(rd->in, rd->len, rd->r->charset, &out, &n);
    if(r < 0) return -1;
    if(r) {
        rd->decoded = out;
        rd->in = out;
        rd->len = n;
        rd->cs = CS_UTF8;
    }
    return 0;
}

static void choose_charset(struct rd *rd, int html) {
    rd->cs = ut_html_charset(rd->in, rd->len, rd->r->charset, html, &rd->pos, rd->charset, sizeof(rd->charset));
}

static int looks_like_html(const unsigned char *s, size_t n) { return ut_html_sniff(s, n); }

/**** the text ****/

#define FL(el) ut_html_flags(el)

static void link_close(struct rd *rd);

/* Room for n more bytes of text: 0, or -1 when the text is full (the page is cut there) or memory is out */
static int room(struct rd *rd, size_t n) {
    size_t cap;
    char *t;
    if(rd->stopped || rd->failed) return -1;
    if(rd->tlen + n > rd->limit) {
        rd->stopped = 1;
        rd->truncated |= UT_TRUNC_TEXT;
        return -1;
    }
    if(rd->tlen + n < rd->tcap) return 0;
    cap = rd->tcap ? rd->tcap * 2 : 4096;
    while(cap <= rd->tlen + n) cap *= 2;
    if(cap > UT_TEXT_MAX + 1) cap = UT_TEXT_MAX + 1;
    t = realloc(rd->text, cap);
    if(!t) {
        rd->failed = 1;
        return -1;
    }
    rd->text = t;
    rd->tcap = cap;
    return 0;
}

static int put(struct rd *rd, const char *s, size_t n) {
    if(room(rd, n)) return -1;
    memcpy(rd->text + rd->tlen, s, n);
    rd->tlen += n;
    return 0;
}

static int pad(struct rd *rd, unsigned int n) {
    if(room(rd, n)) return -1;
    memset(rd->text + rd->tlen, ' ', n);
    rd->tlen += n;
    return 0;
}

static void add_span(struct rd *rd, size_t offset, size_t len, int kind) {
    struct ut_span *s;
    size_t cap;
    if(kind == UT_SPAN_HEADING && rd->heading_spans >= HEADING_SPANS) return;
    if(rd->nspans == rd->spans_cap) {
        cap = rd->spans_cap ? rd->spans_cap * 2 : 16;
        s = realloc(rd->spans, cap * sizeof(*s));
        if(!s) {
            rd->failed = 1;
            return;
        }
        rd->spans = s;
        rd->spans_cap = cap;
    }
    if(kind == UT_SPAN_HEADING) rd->heading_spans++;
    s = &rd->spans[rd->nspans++];
    s->offset = offset;
    s->len = len;
    s->kind = kind;
}

/* A heading's span ends here (spans never hold a line break, a link's [N] or spaces at their ends) */
static void close_span(struct rd *rd) {
    size_t end = rd->tlen;
    if(!rd->span_on) return;
    rd->span_on = 0;
    while(end > rd->span_start && rd->text[end - 1] == ' ') end--;
    if(end > rd->span_start) add_span(rd, rd->span_start, end - rd->span_start, UT_SPAN_HEADING);
}

/* Anchors waiting for their element's text get this offset */
static void place_anchors(struct rd *rd, size_t offset) {
    while(rd->aplaced < rd->nanchors) rd->anchors[rd->aplaced++].offset = offset;
}

/* The line's trailing spaces go; anchors that pointed at them point where they ended (each anchor moves
   once at most: the text never shrinks below a line already ended, so this stays linear) */
static void trim(struct rd *rd) {
    size_t old = rd->tlen, i;
    while(rd->tlen > rd->line_start && rd->text[rd->tlen - 1] == ' ') rd->tlen--;
    if(rd->tlen == old) return;
    for(i = rd->aplaced; i > 0 && rd->anchors[i - 1].offset > rd->tlen; i--) rd->anchors[i - 1].offset = rd->tlen;
}

/* The line breaks wanted, before the next text: the line ends, and a blank line follows if one was
   wanted; never two blank lines, and none at the start */
static int line_breaks(struct rd *rd) {
    int n = rd->breaks;
    rd->breaks = 0;
    rd->space = 0;
    close_span(rd);
    trim(rd);
    rd->col = 0;
    if(!rd->tlen) return 0;
    if(rd->tlen > rd->line_start) {
        if(put(rd, "\n", 1)) return -1;
        rd->line_start = rd->tlen;
        n--;
    }
    if(n > 0 && rd->text[rd->line_start - 2] != '\n') {
        if(put(rd, "\n", 1)) return -1;
        rd->line_start = rd->tlen;
    }
    return 0;
}

static void want(struct rd *rd, int n) {            /* 1: a new line, 2: a blank line too */
    if(!rd->cells_pending && rd->breaks < n) rd->breaks = n;
}

static void newline(struct rd *rd) {                /* <br>, a newline in pre */
    if(!rd->cells_pending && rd->breaks < 2) rd->breaks++;
}

/* The open cells that had shown nothing have now, and so have their rows */
static void cells_shown(struct rd *rd) {
    int i, row = 0;
    for(i = rd->depth - 1; i > 0 && (rd->cells_pending || row); i--) {
        struct frame *f = &rd->st[i];
        if((f->el == E_TD || f->el == E_TH) && f->cell_pending) {
            f->cell_pending = 0;
            rd->cells_pending--;
            row = 1;
        } else if(f->el == E_TR && row) {
            f->row_shown = 1;
            row = 0;
        }
    }
}

/* What comes before any text: the line breaks wanted; at the start of a line its indentation and a list
   item's marker, else the space wanted or a row's " | ". Anchors waiting for text get its place. Returns
   1 when something stands between this text and the text before (for a link's label), 0 when nothing
   does, -1 when the text is full. */
static int begin(struct rd *rd) {
    int gap = 0;
    size_t n;
    if(rd->stopped || rd->failed) return -1;
    if(rd->breaks) {
        if(line_breaks(rd)) return -1;
        gap = rd->tlen > 0;
    }
    if(rd->tlen == rd->line_start) {
        place_anchors(rd, rd->tlen);
        if(pad(rd, rd->marker_on ? rd->marker_col : rd->st[rd->depth - 1].text)) return -1;
    } else if(rd->sep) {
        if(put(rd, " | ", 3)) return -1;
        rd->col += 3;
        gap = 1;
        place_anchors(rd, rd->tlen);
    } else {
        if(rd->space) {
            if(put(rd, " ", 1)) return -1;
            rd->col++;
            gap = 1;
        }
        place_anchors(rd, rd->tlen);
    }
    rd->space = 0;
    rd->sep = 0;
    if(rd->marker_on) {
        n = strlen(rd->marker);
        if(put(rd, rd->marker, n)) return -1;
        rd->col += (unsigned int)n;
        rd->marker_on = 0;
    }
    if(rd->cells_pending) cells_shown(rd);
    return gap;
}

/* A link's label: its text on one line, spaces collapsed, cut between characters at UT_FIELD_MAX */
static void label_add(struct rd *rd, const char *s, size_t n, int gap) {
    if(!rd->link_num || rd->label_full) return;
    if(n == 1 && *s == ' ') {
        rd->label_gap = 1;
        return;
    }
    if((gap || rd->label_gap) && rd->label_len) {
        if(rd->label_len + 1 + n > UT_FIELD_MAX) {
            rd->label_full = 1;
            return;
        }
        rd->label[rd->label_len++] = ' ';
    }
    rd->label_gap = 0;
    if(rd->label_len + n > UT_FIELD_MAX) {
        rd->label_full = 1;
        return;
    }
    memcpy(rd->label + rd->label_len, s, n);
    rd->label_len += n;
}

/* Text: s is n bytes of safe UTF-8 (one character, or a placeholder); " " is a space that does not collapse */
static void emit(struct rd *rd, const char *s, size_t n) {
    int gap;
    if(rd->button == 1) {                   /* a button's first text */
        rd->button = 2;
        emit(rd, "[button: ", 9);
    }
    if(rd->select && rd->option == 1 && !rd->select_shown) {
        rd->select_shown = 1;
        emit(rd, "[select: ", 9);
    }
    gap = begin(rd);
    if(gap < 0) return;
    if(rd->headings && !rd->span_on && *s != ' ') {
        rd->span_on = 1;
        rd->span_start = rd->tlen;
    }
    if(put(rd, s, n)) return;
    rd->col += width(s, n);
    if(rd->link_open) {
        rd->link_text = 1;
        label_add(rd, s, n, gap);
    }
}

static void atom(struct rd *rd, const char *s) {
    emit(rd, s, strlen(s));
}

/* s right after the text before it, with the space or line break that waits still waiting: the "]"
   that ends a placeholder */
static void closer(struct rd *rd, const char *s) {
    size_t n = strlen(s);
    if(rd->stopped || rd->failed || put(rd, s, n)) return;
    rd->col += (unsigned int)n;
    if(rd->link_open) {
        rd->link_text = 1;
        rd->label_gap = 0;
        label_add(rd, s, n, 0);
    }
}

/* Words already collapsed and safe (an alt text, a button's value): their spaces collapse with the text's */
static void words(struct rd *rd, const char *s, size_t n) {
    size_t i = 0, k;
    unsigned int cp;
    while(i < n) {
        if(s[i] == ' ') {
            rd->space = 1;
            i++;
            continue;
        }
        k = ut_utf8_decode((const unsigned char *)s + i, n - i, &cp);
        emit(rd, s + i, k);
        i += k;
    }
}

/* A tab in pre or plain text: spaces to the next 8-column stop */
static void tab(struct rd *rd) {
    emit(rd, " ", 1);
    while(rd->col % 8 && !rd->stopped && !rd->failed) emit(rd, " ", 1);
}

/* One character of the page's text */
static void text_char(struct rd *rd, unsigned int cp) {
    unsigned char b[4];
    if(rd->skip || (rd->select && rd->option != 1) || rd->stopped || rd->failed) return;
    if(rd->pre) {
        if(cp == '\n') {
            newline(rd);
            return;
        }
        if(cp == '\t') {
            tab(rd);
            return;
        }
        if(space_char(cp) || cp == 0xa0) {
            emit(rd, " ", 1);
            return;
        }
    } else {
        if(space_char(cp)) {
            rd->space = 1;
            return;
        }
        if(cp == 0xa0) {                    /* a no-break space is a space that does not collapse, */
            if(rd->cells_pending) rd->space = 1;    /* except where a cell starts: <td>&nbsp;</td> is empty */
            else emit(rd, " ", 1);
            return;
        }
    }
    if(hidden_char(cp)) return;
    emit(rd, (const char *)b, ut_utf8_encode(cp, b));
}

/* s (n bytes of UTF-8 from an attribute) into rd->scratch: unsafe characters dropped, whitespace collapsed,
   none at the ends. Returns its length. */
static size_t collapse(struct rd *rd, const char *s, size_t n) {
    size_t i = 0, k, len = 0;
    unsigned char b[4];
    unsigned int cp;
    int gap = 0;
    while(i < n) {
        k = ut_utf8_decode((const unsigned char *)s + i, n - i, &cp);
        i += k;
        if(space_char(cp) || cp == 0xa0) {
            gap = 1;
            continue;
        }
        if(hidden_char(cp)) continue;
        k = ut_utf8_encode(cp, b);
        if(len + (gap && len) + k > UT_FIELD_MAX) break;
        if(gap && len) rd->scratch[len++] = ' ';
        gap = 0;
        memcpy(rd->scratch + len, b, k);
        len += k;
    }
    rd->scratch[len] = 0;
    return len;
}

/**** open elements ****/

static int is_list(int el) { return el == E_UL || el == E_OL || el == E_MENU || el == E_DIR; }

static unsigned short capped(unsigned int n, unsigned int max) { return (unsigned short)(n > max ? max : n); }

/* Open el on the stack with its parent's indentation; NULL when the stack is full (it is only counted) */
static struct frame *push(struct rd *rd, int el) {
    struct frame *p = &rd->st[rd->depth - 1], *f;
    if(rd->depth == STACK_MAX) {
        rd->over[el]++;
        return NULL;
    }
    f = &rd->st[rd->depth];
    memset(f, 0, sizeof(*f));
    f->el = (unsigned char)el;
    f->text = p->text;
    f->mcol = p->mcol;
    f->lst = (short)(is_list(p->el) || p->el == E_LI ? rd->depth - 1 : p->lst);
    if(el == E_BLOCKQUOTE || el == E_DD) {
        f->text = capped(p->text + 2u, INDENT_MAX);
        f->lst = -1;
    }
    rd->open[el]++;
    rd->depth++;
    return f;
}

static void button_close(struct rd *rd);
static void select_close(struct rd *rd);
static int table_at(const struct rd *rd);

/* Close the top element, with what its end does to the text */
static void pop(struct rd *rd) {
    struct frame *f = &rd->st[rd->depth - 1];
    int el = f->el, t;
    unsigned int fl = FL(el);
    if(el == E_TD || el == E_TH) {                  /* a cell's end closes what is open inside it */
        link_close(rd);
        button_close(rd);
        select_close(rd);
        t = table_at(rd);                           /* its own table: the cell is on top */
        if(f->cell_pending && t && rd->st[t].data) emit(rd, "-", 1);   /* an empty cell of a data table */
    }
    rd->depth--;
    rd->open[el]--;
    switch(el) {
    case E_LI:
        if(rd->marker_on && rd->marker_frame == rd->depth) rd->marker_on = 0;   /* an item that showed nothing */
        want(rd, 1);
        return;
    case E_TD: case E_TH:
        if(f->cell_pending && !--rd->cells_pending) rd->sep = 0;
        return;
    case E_CAPTION:
        link_close(rd);
        want(rd, 1);
        return;
    case E_TABLE:
        rd->tables--;
        want(rd, 1);
        return;
    case E_PRE: case E_LISTING:
        rd->pre--;
        want(rd, 2);
        return;
    }
    if(fl & F_HEAD) {
        rd->headings--;
        close_span(rd);
    }
    if(fl & F_LIST) want(rd, f->nested ? 1 : 2);
    else if(fl & F_PARA) want(rd, 2);
    else if(fl & F_LINE) want(rd, 1);
}

static void pop_to(struct rd *rd, int i) {          /* close st[i] and everything inside it */
    while(rd->depth > i && rd->depth > 1) pop(rd);
}

/* The nearest open el that an end tag reaches: the standard's scopes (an end tag in a cell, a caption,
   a table or a list item's list cannot close what is outside it). 0: none. */
enum { SCOPE, SCOPE_LIST };
static int in_scope(const struct rd *rd, int el, int kind) {
    int i, e;
    if(!rd->open[el]) return 0;
    for(i = rd->depth - 1; i > 0; i--) {
        e = rd->st[i].el;
        if(e == el) return i;
        if((FL(e) & F_SCOPE) || (kind == SCOPE_LIST && (e == E_UL || e == E_OL))) return 0;
    }
    return 0;
}

static int heading_in_scope(const struct rd *rd) {
    int i;
    if(!rd->headings) return 0;
    for(i = rd->depth - 1; i > 0; i--) {
        if(FL(rd->st[i].el) & F_HEAD) return i;
        if(FL(rd->st[i].el) & F_SCOPE) return 0;
    }
    return 0;
}

static void close_p(struct rd *rd) {
    int i = in_scope(rd, E_P, SCOPE);
    if(i) pop_to(rd, i);
}

/* <li> closes the open item of its list; <dt> and <dd> the open term or definition (the standard's walk) */
static void close_item(struct rd *rd, int el) {
    int i, e;
    for(i = rd->depth - 1; i > 0; i--) {
        e = rd->st[i].el;
        if(el == E_LI ? e == E_LI : e == E_DT || e == E_DD) {
            pop_to(rd, i);
            return;
        }
        if((FL(e) & F_SPECIAL) && e != E_ADDRESS && e != E_DIV && e != E_P) return;
    }
}

/* An integer attribute (ol start, li value) as the standard reads it, kept within a billion */
static int parse_int(const char *s, long *out) {
    long v = 0;
    int neg = 0, digits = 0;
    while(ws((unsigned char)*s)) s++;
    if(*s == '-' || *s == '+') neg = *s++ == '-';
    while(digit((unsigned char)*s)) {
        if(v < 100000000L) v = v * 10 + (*s - '0');     /* at most 999999999: no overflow, even in 32 bits */
        else v = 999999999L;
        s++;
        digits++;
    }
    if(!digits) return -1;
    *out = neg ? -v : v;
    return 0;
}

static void list_start(struct rd *rd, struct ut_html_tag *t) {
    struct frame *p = &rd->st[rd->depth - 1], *f;
    int nested = p->lst >= 0 || is_list(p->el) || p->el == E_LI;
    want(rd, nested ? 1 : 2);
    f = push(rd, t->el);
    if(!f) return;
    f->nested = (unsigned char)nested;
    f->mcol = f->lst >= 0 ? capped(rd->st[f->lst].mcol + 2u, INDENT_MAX) : p->text;    /* two more per level */
    f->text = capped(f->mcol + 2u, INDENT_MAX);
    f->next = 1;
    if(t->el == E_OL && t->seen[A_START]) parse_int(t->val[A_START], &f->next);
}

static void item_start(struct rd *rd, struct ut_html_tag *t) {
    struct frame *f, *list;
    long num;
    size_t n;
    close_item(rd, E_LI);
    want(rd, 1);
    f = push(rd, E_LI);
    if(!f) return;
    list = f->lst >= 0 && is_list(rd->st[f->lst].el) ? &rd->st[f->lst] : NULL;
    f->mcol = list ? list->mcol : rd->st[rd->depth - 2].text;
    if(list && list->el == E_OL) {
        num = list->next;
        if(t->seen[A_VALUE]) parse_int(t->val[A_VALUE], &num);
        list->next = num + 1;
        snprintf(rd->marker, sizeof(rd->marker), "%ld. ", num);
    } else snprintf(rd->marker, sizeof(rd->marker), "%s", "* ");
    n = strlen(rd->marker);
    f->text = capped(f->mcol + (unsigned int)n, INDENT_MAX + 8);
    rd->marker_on = 1;
    rd->marker_col = f->mcol;
    rd->marker_frame = rd->depth - 1;
}

/**** tables: a line per row, " | " between the cells that show something ****/

static int table_at(const struct rd *rd) {          /* the innermost open table, 0: none */
    int i;
    if(!rd->tables) return 0;
    for(i = rd->depth - 1; i > 0; i--) if(rd->st[i].el == E_TABLE) return i;
    return 0;
}

static int above(const struct rd *rd, int from, int el, int el2) {     /* the nearest el or el2 above st[from] */
    int i;
    for(i = rd->depth - 1; i > from; i--) if(rd->st[i].el == el || rd->st[i].el == el2) return i;
    return 0;
}

static void row_start(struct rd *rd) {
    int t = table_at(rd);
    if(!t) return;                                  /* a row outside a table is no row */
    pop_to(rd, t + 1);
    want(rd, 1);
    push(rd, E_TR);
}

static void cell_start(struct rd *rd, int el) {
    int t = table_at(rd), r, c;
    struct frame *f;
    if(!t) return;
    if(el == E_TH) rd->st[t].data = 1;              /* a data table from its first header cell on */
    link_close(rd);
    c = above(rd, t, E_TD, E_TH);
    if(c) pop_to(rd, c);
    r = above(rd, t, E_TR, E_TR);
    if(!r) {                                        /* a cell without a row: the row is implied */
        pop_to(rd, t + 1);
        want(rd, 1);
        if(!push(rd, E_TR)) return;
        r = rd->depth - 1;
    } else pop_to(rd, r + 1);
    if(rd->st[r].row_shown) {                       /* the row's line goes on */
        rd->breaks = 0;
        rd->space = 0;
        rd->sep = 1;
    }
    f = push(rd, el);
    if(!f) {
        rd->sep = 0;
        return;
    }
    f->cell_pending = 1;
    rd->cells_pending++;
}

static void caption_start(struct rd *rd) {
    int t = table_at(rd);
    if(!t) return;
    pop_to(rd, t + 1);
    link_close(rd);
    want(rd, 1);
    push(rd, E_CAPTION);
}

static void table_end(struct rd *rd, int el) {      /* </table>, </tr>, </td>, </th>, </caption> */
    int t = table_at(rd), i;
    if(!t) return;
    if(el == E_TABLE) i = t;
    else i = above(rd, t, el, el);
    if(i) pop_to(rd, i);
}

/**** anchors ****/

/* An id, or an <a name>: kept with the offset of its element's text, which is not known yet. The first of
   two equal names counts; a name over UT_FIELD_MAX bytes, or with a character that is not safe, is not
   kept (it could not be named exactly anyway). */
static void add_anchor(struct rd *rd, const char *name, size_t len, int over) {
    unsigned int h = 2166136261u;
    size_t i, slot, k;
    unsigned int cp;
    struct ut_anchor *a;
    if(!len || over || len > UT_FIELD_MAX || rd->stopped || rd->failed) return;
    for(i = 0; i < len; i += k) {
        k = ut_utf8_decode((const unsigned char *)name + i, len - i, &cp);
        if(hidden_char(cp)) return;
    }
    for(i = 0; i < len; i++) h = (h ^ (unsigned char)name[i]) * 16777619u;
    for(slot = h % AHASH; rd->ahash[slot]; slot = (slot + 1) % AHASH)
        if(!strcmp(rd->anchors[rd->ahash[slot] - 1].name, name)) return;
    if(rd->nanchors == UT_ANCHORS_MAX || rd->link_bytes_full || rd->link_bytes + len + 1 > UT_LINK_BYTES_MAX) {
        if(rd->nanchors < UT_ANCHORS_MAX) rd->link_bytes_full = 1;
        rd->truncated |= UT_TRUNC_ANCHORS;
        return;
    }
    if(rd->nanchors == rd->anchors_cap) {
        size_t cap = rd->anchors_cap ? rd->anchors_cap * 2 : 8;
        a = realloc(rd->anchors, cap * sizeof(*a));
        if(!a) {
            rd->failed = 1;
            return;
        }
        rd->anchors = a;
        rd->anchors_cap = cap;
    }
    a = &rd->anchors[rd->nanchors];
    a->name = malloc(len + 1);
    if(!a->name) {
        rd->failed = 1;
        return;
    }
    memcpy(a->name, name, len + 1);
    a->offset = 0;
    rd->ahash[slot] = (unsigned short)(rd->nanchors + 1);
    rd->nanchors++;
    rd->link_bytes += len + 1;
}

/**** links ****/

/* An address as written, without the ASCII whitespace and controls at its ends */
static const char *stripped(const char *s, size_t *n) {
    while(*n && (unsigned char)s[*n - 1] <= ' ') --*n;
    while(*n && (unsigned char)*s <= ' ') {
        s++;
        --*n;
    }
    return s;
}

/* A label for a link that shows no text: an element's aria-label, else its title (the first that has one:
   the <a> itself, else an element inside it) */
static void link_alt(struct rd *rd, struct ut_html_tag *t) {
    size_t n = 0;
    if(t->seen[A_ARIA]) n = collapse(rd, t->val[A_ARIA], t->len[A_ARIA]);
    if(!n && t->seen[A_TITLE]) n = collapse(rd, t->val[A_TITLE], t->len[A_TITLE]);
    memcpy(rd->link_alt, rd->scratch, n);
    rd->link_alt_len = n;
}

static void link_start(struct rd *rd, struct ut_html_tag *t) {
    size_t n = t->len[A_HREF], cap;
    const char *h = stripped(t->val[A_HREF], &n);
    void *p;
    link_close(rd);
    if(!t->seen[A_HREF] || !n) return;              /* no address: no link */
    rd->link_open = 1;
    rd->link_text = 0;
    rd->link_num = 0;
    rd->label_len = 0;
    rd->label_gap = 0;
    rd->label_full = 0;
    rd->link_alt_len = 0;
    if(rd->stopped || rd->failed) return;
    link_alt(rd, t);
    if(rd->nlinks == UT_LINKS_MAX || rd->link_bytes_full) {    /* its text, but no number */
        rd->truncated |= UT_TRUNC_LINKS;
        return;
    }
    if(rd->nlinks == rd->links_cap) {
        cap = rd->links_cap ? rd->links_cap * 2 : 8;
        p = realloc(rd->hrefs, cap * sizeof(*rd->hrefs));
        if(p) rd->hrefs = p;
        p = p ? realloc(rd->links, cap * sizeof(*rd->links)) : NULL;
        if(!p) {
            rd->failed = 1;
            return;
        }
        rd->links = p;
        rd->links_cap = cap;
    }
    rd->hrefs[rd->nlinks] = malloc(n + 1);
    if(!rd->hrefs[rd->nlinks]) {
        rd->failed = 1;
        return;
    }
    memcpy(rd->hrefs[rd->nlinks], h, n);
    rd->hrefs[rd->nlinks][n] = 0;
    rd->long_href[rd->nlinks] = t->over[A_HREF];
    memset(&rd->links[rd->nlinks], 0, sizeof(rd->links[0]));
    rd->link_num = (int)++rd->nlinks;
}

/* A link's "[N]": right after its label, with the space or line break that waits still waiting; where the
   link is when it showed nothing. 0, or -1 when it does not fit. */
static int link_marker(struct rd *rd) {
    char m[16];
    size_t n = (size_t)snprintf(m, sizeof(m), "[%d]", rd->link_num);
    if(rd->link_text) {
        if(rd->stopped || rd->failed) return -1;
    } else if(begin(rd) < 0) return -1;
    close_span(rd);
    if(put(rd, m, n)) return -1;
    rd->col += (unsigned int)n;
    add_span(rd, rd->tlen - n, n, UT_SPAN_LINK);
    return rd->failed ? -1 : 0;
}

static int other_scheme(const char *h);
static size_t safe_copy(char *out, size_t cap, const char *in);
static int resolve(const struct ut_url *base, const char *href, struct ut_url *u);

/* Link i's address against base (a fragment alone against the page itself): the absolute address when it
   resolves to safe text, else the reference as written, as safe text. *url is malloc'ed (exactly its size).
   0, or -1 when memory is out. */
static int link_url(struct rd *rd, size_t i, const struct ut_url *base, char **url, int *usable) {
    const char *h = rd->hrefs[i], *src;
    size_t n;
    int rc = 1;
    if(!rd->work && !(rd->work = malloc(sizeof(*rd->work)))) return -1;
    if(h[0] == '#') base = &rd->r->url;
    if(!rd->long_href[i] && !other_scheme(h)) rc = resolve(base, h, &rd->work->u);
    if(rc < 0) return -1;
    if(rc == 0) src = rd->work->u.text;
    else {
        safe_copy(rd->work->safe, sizeof(rd->work->safe), h);
        src = rd->work->safe;
    }
    n = strlen(src);
    *url = malloc(n + 1);
    if(!*url) return -1;
    memcpy(*url, src, n + 1);
    *usable = rc == 0;
    return 0;
}

/* A numbered link ends: its label and address are made now, so that their size is known exactly; it keeps
   its number only while the page's link and anchor strings stay within UT_LINK_BYTES_MAX (and its [N] fits
   in the text). Otherwise it is no link: its text stays, without a number, and so for every link after it. */
static void link_close(struct rd *rd) {
    struct ut_link *l;
    char *label, *url = NULL;
    size_t i, need;
    int usable = 0;
    if(!rd->link_open) return;
    if(rd->link_num && !rd->label_len && rd->link_alt_len)
        words(rd, rd->link_alt, rd->link_alt_len);     /* no text: its aria-label or title shows as its label */
    rd->link_open = 0;
    if(!rd->link_num || rd->failed) return;
    i = (size_t)rd->link_num - 1;
    if(link_url(rd, i, rd->base ? rd->base : &rd->r->url, &url, &usable)) {
        free(url);
        rd->failed = 1;
        return;
    }
    need = rd->label_len + 1 + strlen(url) + 1;
    if(rd->link_bytes_full || rd->link_bytes + need > UT_LINK_BYTES_MAX) {
        rd->link_bytes_full = 1;
        rd->truncated |= UT_TRUNC_LINKS;
    } else if(!link_marker(rd)) {
        label = malloc(rd->label_len + 1);
        if(!label) {
            free(url);
            rd->failed = 1;
            return;
        }
        memcpy(label, rd->label, rd->label_len);
        label[rd->label_len] = 0;
        l = &rd->links[i];
        l->label = label;
        l->url = url;
        l->usable = usable;
        rd->link_bytes += need;
        return;
    }
    free(url);
    if(rd->failed) return;
    rd->nlinks--;                                   /* no room for it: no number, so no link */
    free(rd->hrefs[rd->nlinks]);
    rd->hrefs[rd->nlinks] = NULL;
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

/* Well-formed UTF-8 without a character that is never shown: an address that is safe to print */
static int safe_utf8(const char *s) {
    size_t n = strlen(s), i = 0, k;
    unsigned int cp;
    while(i < n) {
        k = ut_utf8_decode((const unsigned char *)s + i, n - i, &cp);
        if((k == 1 && (unsigned char)s[i] >= 0x80) || hidden_char(cp)) return 0;
        i += k;
    }
    return 1;
}

/* Resolve href against base into u: 0, 1 when it does not resolve (or not to a safe address), -1 out of memory */
static int resolve(const struct ut_url *base, const char *href, struct ut_url *u) {
    char err[128], *c = clean_href(href);
    int rc;
    if(!c) return -1;
    err[0] = 0;
    rc = ut_url_resolve(base, c, u, err, sizeof(err));
    free(c);
    if(!rc) return safe_utf8(u->text) ? 0 : 1;
    return strstr(err, "out of memory") ? -1 : 1;
}

/* A <base> after links: their addresses again, against it. 1 when done; 0 when they would pass
   UT_LINK_BYTES_MAX, or when that limit has already stopped the numbers, which then depend on the base from
   the first link on (nothing changes); -1 when memory is out. */
static int rebase(struct rd *rd, const struct ut_url *base) {
    struct { char *url; int usable; } *again;
    size_t i, k, n = rd->nlinks - (rd->link_open && rd->link_num ? 1 : 0), total = rd->link_bytes;
    if(!n) return 1;
    if(rd->link_bytes_full) return 0;
    again = malloc(n * sizeof(*again));
    if(!again) return -1;
    for(i = 0; i < n; i++) {
        if(link_url(rd, i, base, &again[i].url, &again[i].usable)) {
            for(k = 0; k < i; k++) free(again[k].url);
            free(again);
            return -1;
        }
        total = total - strlen(rd->links[i].url) + strlen(again[i].url);
    }
    if(total > UT_LINK_BYTES_MAX) {
        for(i = 0; i < n; i++) free(again[i].url);
        free(again);
        return 0;
    }
    for(i = 0; i < n; i++) {
        free(rd->links[i].url);
        rd->links[i].url = again[i].url;
        rd->links[i].usable = again[i].usable;
    }
    rd->link_bytes = total;
    free(again);
    return 1;
}

/* The first <base href> that resolves counts for every link, those before it too (no later one counts). When
   it would make their addresses pass UT_LINK_BYTES_MAX, which of them keep their numbers depends on it from the
   first link on: this reading stops, and the page is read again with the base known from the start, so that
   the links are numbered in order while their strings fit, as on any page. No link is ever kept at an address
   against the page instead of its base. */
static void base_href(struct rd *rd, struct ut_html_tag *t) {
    size_t n = t->len[A_HREF];
    const char *h;
    char *copy;
    struct ut_url *u;
    int rc;
    if(rd->base_done || rd->failed || !t->seen[A_HREF] || t->over[A_HREF]) return;
    h = stripped(t->val[A_HREF], &n);
    if(!n) return;
    copy = malloc(n + 1);
    u = malloc(sizeof(*u));
    if(!copy || !u) {
        free(copy);
        free(u);
        rd->failed = 1;
        return;
    }
    memcpy(copy, h, n);
    copy[n] = 0;
    rc = resolve(&rd->r->url, copy, u);
    free(copy);
    if(!rc) {
        rd->base_done = 1;
        rc = rebase(rd, u);                         /* 1: used, 0: too big, -1: out of memory */
        if(rc == 0) rd->again = rd->failed = 1;     /* ut_render() reads the page again with u */
        rc = rc >= 0 ? 0 : -1;
    }
    if(rc < 0) rd->failed = 1;
    if(rc) free(u);
    else rd->base = u;
}

/**** images and forms ****/

static void image(struct rd *rd, struct ut_html_tag *t) {
    size_t n;
    if(!t->seen[A_ALT] || !(n = collapse(rd, t->val[A_ALT], t->len[A_ALT]))) return;
    if(rd->link_open || rd->button) words(rd, rd->scratch, n);         /* a label: no brackets */
    else {
        emit(rd, "[", 1);
        words(rd, rd->scratch, n);
        closer(rd, "]");
    }
}

/* A button or a select is a box: the space at the end of its text stays inside it */
static void button_close(struct rd *rd) {
    int b = rd->button;
    rd->button = 0;
    if(b == 2) {
        closer(rd, "]");
        rd->space = 0;
    } else if(b == 1) atom(rd, "[button]");
}

static void select_close(struct rd *rd) {
    int shown = rd->select_shown;
    if(!rd->select) return;
    rd->select = rd->option = rd->select_shown = 0;
    if(shown) {
        closer(rd, "]");
        rd->space = 0;
    } else atom(rd, "[select]");
}

static int is_type(struct ut_html_tag *t, const char *type) {
    return t->seen[A_TYPE] && ci_eq((const unsigned char *)t->val[A_TYPE], t->len[A_TYPE], type);
}

static void input(struct rd *rd, struct ut_html_tag *t) {
    const char *label = NULL;
    size_t n;
    if(is_type(t, "hidden")) return;
    rd->forms = 1;
    if(is_type(t, "checkbox")) atom(rd, "[checkbox]");
    else if(is_type(t, "radio")) atom(rd, "[radio button]");
    else if(is_type(t, "file")) atom(rd, "[file field]");
    else if(is_type(t, "password")) atom(rd, "[password field]");
    else if(is_type(t, "submit") || is_type(t, "reset") || is_type(t, "button") || is_type(t, "image")) {
        if(is_type(t, "image")) n = t->seen[A_ALT] ? collapse(rd, t->val[A_ALT], t->len[A_ALT]) : 0;
        else n = t->seen[A_VALUE] ? collapse(rd, t->val[A_VALUE], t->len[A_VALUE]) : 0;
        if(!n && !t->seen[is_type(t, "image") ? A_ALT : A_VALUE])
            label = is_type(t, "reset") ? "Reset" : is_type(t, "button") ? NULL : "Submit";
        if(n) {
            emit(rd, "[button: ", 9);
            words(rd, rd->scratch, n);
            closer(rd, "]");
        } else if(label) {
            emit(rd, "[button: ", 9);
            atom(rd, label);
            closer(rd, "]");
        } else atom(rd, "[button]");
    } else atom(rd, "[text field]");
}

/* A tag inside <select>: 1 when it was dealt with here (only options count; anything else is ignored) */
static int select_tag(struct rd *rd, struct ut_html_tag *t) {
    int el = t->el;
    if(el == E_OPTION || el == E_OPTGROUP) {
        if(!t->end && el == E_OPTION && !rd->option) rd->option = 1;
        else if(rd->option == 1) rd->option = 2;
        return 1;
    }
    if(el == E_SELECT || (!t->end && (el == E_INPUT || el == E_KEYGEN || el == E_TEXTAREA))) {
        select_close(rd);
        return el == E_SELECT;
    }
    if(el == E_TABLE || el == E_CAPTION || el == E_TBODY || el == E_THEAD || el == E_TFOOT || el == E_TR ||
       el == E_TD || el == E_TH) {                  /* a table's structure ends a select inside it */
        select_close(rd);
        return 0;
    }
    return !(el == E_SCRIPT || el == E_STYLE || el == E_TEMPLATE);
}

/**** tags ****/

static void start_tag(struct rd *rd, struct ut_html_tag *t) {
    int el = t->el;
    unsigned int fl = FL(el);
    if(t->seen[A_ID]) add_anchor(rd, t->val[A_ID], t->len[A_ID], t->over[A_ID]);
    if(fl & F_CLOSEP) close_p(rd);
    switch(el) {
    case E_A:
        if(t->seen[A_NAME]) add_anchor(rd, t->val[A_NAME], t->len[A_NAME], t->over[A_NAME]);
        link_start(rd, t);
        return;
    case E_UL: case E_OL: case E_MENU: case E_DIR:
        list_start(rd, t);
        return;
    case E_LI:
        item_start(rd, t);
        return;
    case E_DT: case E_DD:
        close_item(rd, E_DT);
        want(rd, 1);
        push(rd, el);
        return;
    case E_H1: case E_H2: case E_H3: case E_H4: case E_H5: case E_H6:
        if(FL(rd->st[rd->depth - 1].el) & F_HEAD) pop(rd);        /* headings do not nest */
        want(rd, 2);
        if(push(rd, el)) rd->headings++;
        return;
    case E_PRE: case E_LISTING:
        want(rd, 2);
        if(push(rd, el)) rd->pre++;
        if(rd->pos < rd->len && rd->in[rd->pos] == '\r') rd->pos++;      /* a newline right after <pre> */
        if(rd->pos < rd->len && rd->in[rd->pos] == '\n') rd->pos++;
        return;
    case E_BR:
        newline(rd);
        return;
    case E_HR:
        want(rd, 1);
        atom(rd, RULE);
        want(rd, 1);
        return;
    case E_IMG: case E_IMAGE:
        image(rd, t);
        return;
    case E_INPUT:
        input(rd, t);
        return;
    case E_BUTTON:
        button_close(rd);
        rd->forms = 1;
        rd->button = 1;
        return;
    case E_SELECT:
        rd->forms = 1;
        rd->select = 1;
        rd->option = rd->select_shown = 0;
        return;
    case E_TEXTAREA:
        rd->forms = 1;
        atom(rd, "[text area]");
        return;
    case E_BASE:
        base_href(rd, t);
        return;
    case E_TABLE:
        want(rd, 1);
        if(push(rd, el)) rd->tables++;
        return;
    case E_TR:
        row_start(rd);
        return;
    case E_TD: case E_TH:
        cell_start(rd, el);
        return;
    case E_CAPTION:
        caption_start(rd);
        return;
    case E_TBODY: case E_THEAD: case E_TFOOT:
        if(table_at(rd)) pop_to(rd, table_at(rd) + 1);
        return;
    case E_FORM:
        rd->forms = 1;
        break;
    }
    if(fl & F_PARA) want(rd, 2);
    else if(fl & F_LINE) want(rd, 1);
    if(fl & F_PUSH) push(rd, el);
}

static void end_tag(struct rd *rd, struct ut_html_tag *t) {
    int el = t->el, i;
    unsigned int fl = FL(el);
    if((fl & F_PUSH) && rd->over[el]) {             /* one that was too deep to keep */
        rd->over[el]--;
        return;
    }
    switch(el) {
    case E_A:
        link_close(rd);
        return;
    case E_BR:                                      /* </br> is <br>, as in browsers */
        newline(rd);
        return;
    case E_P:
        if((i = in_scope(rd, E_P, SCOPE))) pop_to(rd, i);
        else want(rd, 2);                           /* a lone </p> is an empty paragraph */
        return;
    case E_LI:
        if((i = in_scope(rd, E_LI, SCOPE_LIST))) pop_to(rd, i);
        return;
    case E_H1: case E_H2: case E_H3: case E_H4: case E_H5: case E_H6:
        if((i = heading_in_scope(rd))) pop_to(rd, i);
        return;
    case E_BUTTON:
        button_close(rd);
        return;
    case E_TABLE: case E_TR: case E_TD: case E_TH: case E_CAPTION:
        table_end(rd, el);
        return;
    case E_TBODY: case E_THEAD: case E_TFOOT:
        if(table_at(rd)) pop_to(rd, table_at(rd) + 1);
        return;
    }
    if((fl & F_PUSH) && (i = in_scope(rd, el, SCOPE))) pop_to(rd, i);
}

static void dispatch(struct rd *rd, struct ut_html_tag *t);

/* A tag inside content that is not shown: only what ends that content matters. In svg and math, the
   HTML elements of the standard's list (p, div, b, ...) end them, as in browsers. */
static void hidden_tag(struct rd *rd, struct ut_html_tag *t, int whole) {
    int el = t->el;
    unsigned int fl = FL(el);
    if((rd->hide[E_SVG] || rd->hide[E_MATH]) && (t->end ? el == E_P || el == E_BR : (fl & F_OUT) != 0)) {
        rd->skip -= (int)(rd->hide[E_SVG] + rd->hide[E_MATH]);
        rd->hide[E_SVG] = rd->hide[E_MATH] = 0;
        if(!rd->skip) dispatch(rd, t);
        return;
    }
    if(!(fl & F_HIDE)) return;
    if(t->end) {
        if(rd->hide[el]) {
            rd->hide[el]--;
            rd->skip--;
        }
    } else if(!whole) {
        rd->hide[el]++;
        rd->skip++;
    }
}

static void dispatch(struct rd *rd, struct ut_html_tag *t) {
    int el = t->el;
    unsigned int fl = FL(el);
    int whole = t->selfclose && (rd->xhtml || el == E_SVG || el == E_MATH);     /* <x /> is all of x */
    if(!t->end && el != E_A && rd->link_open && rd->link_num && !rd->link_alt_len)
        link_alt(rd, t);                            /* the first element in a link with a label for it */
    if(rd->skip) {
        hidden_tag(rd, t, whole);
        return;
    }
    if(rd->select && select_tag(rd, t)) return;
    if(t->end) {
        end_tag(rd, t);
        return;
    }
    if(fl & F_HIDE) {
        if(!whole) {
            rd->hide[el]++;
            rd->skip++;
        }
        return;
    }
    start_tag(rd, t);
    if(whole && !(fl & F_VOID)) end_tag(rd, t);
}

/**** the tokenizer (its scanning is html_scan.c's) ****/

/* A tag whose name starts at in[q]: read it with its attributes (html_scan.c) and deal with it. 0 when the
   body ends inside it (it is dropped, and nothing after it is left) */
static int tag(struct rd *rd, size_t q, int end) {
    size_t next;
    int whole = ut_html_tag(rd->in, rd->len, rd->cs, q, end, &rd->tag, &next);
    rd->pos = next;
    if(whole) dispatch(rd, &rd->tag);
    return whole;
}

/* Where the raw text that started at rd->pos ends (html_scan.c) */
static size_t raw_end(const struct rd *rd, const char *name) { return ut_html_raw_end(rd->in, rd->len, rd->pos, name); }
static size_t script_end(const struct rd *rd) { return ut_html_script_end(rd->in, rd->len, rd->pos); }

/* A title's text: references decoded, whitespace collapsed, unsafe characters dropped, cut at UT_FIELD_MAX */
static void title_text(struct rd *rd, size_t q, size_t end) {
    unsigned char b[4];
    unsigned int cp;
    size_t k;
    int gap = 0;
    while(q < end) {
        cp = rd->in[q] == '&' ? char_ref(rd, &q, end, 0) : next_char(rd, &q, end);
        if(space_char(cp) || cp == 0xa0) {
            gap = 1;
            continue;
        }
        if(hidden_char(cp)) continue;
        k = ut_utf8_encode(cp, b);
        if(rd->title_len + (gap && rd->title_len) + k > UT_FIELD_MAX) return;
        if(gap && rd->title_len) rd->title[rd->title_len++] = ' ';
        gap = 0;
        memcpy(rd->title + rd->title_len, b, k);
        rd->title_len += k;
    }
}

/* The content of a raw text element, up to its own end tag (which the next markup() reads). Only a title
   (the first one) and an xmp's text are kept, and only when nothing hides them. */
static void raw(struct rd *rd, int el, const char *name) {
    size_t start = rd->pos, end = el == E_SCRIPT ? script_end(rd) : raw_end(rd, name), q;
    rd->pos = end;
    if(rd->skip) return;
    if(el == E_TITLE && !rd->title_seen) {
        rd->title_seen = 1;
        title_text(rd, start, end);
    } else if(el == E_XMP && !rd->select) {
        rd->pre++;
        for(q = start; q < end && !rd->stopped && !rd->failed; ) text_char(rd, next_char(rd, &q, end));
        rd->pre--;
        want(rd, 2);
    }
}

/* A comment, from just after "<!--", and a bogus one (html_scan.c) */
static void comment(struct rd *rd, size_t q) { rd->pos = ut_html_comment_end(rd->in, rd->len, q); }

static void skip_past(struct rd *rd, size_t q, const char *end) { rd->pos = ut_html_skip_past(rd->in, rd->len, q, end); }

/* At a '<' */
static void markup(struct rd *rd) {
    const unsigned char *s = rd->in;
    size_t p = rd->pos, n = rd->len;
    struct ut_html_tag *t = &rd->tag;
    unsigned char c = p + 1 < n ? s[p + 1] : 0;
    if(c == '!') {
        if(p + 3 < n && s[p + 2] == '-' && s[p + 3] == '-') comment(rd, p + 4);
        else if((rd->hide[E_SVG] || rd->hide[E_MATH]) && n - p >= 9 && !memcmp(s + p + 2, "[CDATA[", 7))
            skip_past(rd, p + 9, "]]>");            /* CDATA exists only in svg and math */
        else skip_past(rd, p + 2, ">");             /* <!DOCTYPE ...>, a bogus comment */
    } else if(c == '?') skip_past(rd, p + 2, ">");  /* a processing instruction is a bogus comment */
    else if(c == '/') {
        if(p + 2 < n && alpha(s[p + 2])) tag(rd, p + 2, 1);
        else if(p + 2 < n && s[p + 2] == '>') rd->pos = p + 3;
        else if(p + 2 < n) skip_past(rd, p + 2, ">");
        else {
            text_char(rd, '<');
            text_char(rd, '/');
            rd->pos = n;
        }
    } else if(alpha(c)) {
        if(tag(rd, p + 1, 0) && (FL(t->el) & (F_RAW | F_RCDATA)) &&
           !(t->selfclose && (rd->xhtml || rd->hide[E_SVG] || rd->hide[E_MATH])))     /* <title/> in svg is whole */
            raw(rd, t->el, t->name);
    } else {
        text_char(rd, '<');
        rd->pos = p + 1;
    }
}

static void run_html(struct rd *rd) {
    const unsigned char *j;
    while(rd->pos < rd->len && !rd->failed) {
        unsigned char c = rd->in[rd->pos];
        if(c == '<') markup(rd);
        else if(rd->skip || rd->stopped || (rd->select && rd->option != 1)) {
            j = memchr(rd->in + rd->pos, '<', rd->len - rd->pos);      /* text nobody sees */
            rd->pos = j ? (size_t)(j - rd->in) : rd->len;
        } else if(c == '&') text_char(rd, char_ref(rd, &rd->pos, rd->len, 0));
        else text_char(rd, next_char(rd, &rd->pos, rd->len));
    }
}

static void run_plain(struct rd *rd) {
    rd->pre = 1;
    while(rd->pos < rd->len && !rd->failed && !rd->stopped) text_char(rd, next_char(rd, &rd->pos, rd->len));
}

/**** the document ****/

/* href (an address as written) starts with a scheme other than http and https */
static int other_scheme(const char *h) {
    size_t i = 0;
    if(!alpha((unsigned char)h[0])) return 0;
    while(alnum((unsigned char)h[i]) || h[i] == '+' || h[i] == '-' || h[i] == '.') i++;
    if(h[i] != ':') return 0;
    return !ci_eq((const unsigned char *)h, i, "http") && !ci_eq((const unsigned char *)h, i, "https");
}

/* in (UTF-8, or not) as one line of safe text in out (cap bytes, terminated): what ut_safe_text() does,
   but without anything hidden_char() drops. Returns its length. */
static size_t safe_copy(char *out, size_t cap, const char *in) {
    const unsigned char *s = (const unsigned char *)in;
    size_t n = strlen(in), i = 0, o = 0, k;
    unsigned int cp;
    while(i < n) {
        i += ut_utf8_decode(s + i, n - i, &cp);
        if(space_char(cp)) cp = ' ';
        else if(hidden_char(cp)) continue;
        if(o + 4 >= cap) break;
        k = ut_utf8_encode(cp, (unsigned char *)out + o);
        o += k;
    }
    out[o] = 0;
    return o;
}

/* An array down to its count (kept as it is when that fails: its size is what is counted) */
static void *shrink(void *p, size_t count, size_t size, size_t *cap) {
    void *q;
    if(!p || count == *cap) return p;
    if(!count) {
        free(p);
        *cap = 0;
        return NULL;
    }
    q = realloc(p, count * size);
    if(!q) return p;
    *cap = count;
    return q;
}

static void add_notice(char *notice, size_t cap, const char *part) {
    size_t n = strlen(notice);
    if(n + (n ? 2 : 0) + strlen(part) >= cap) return;
    if(n) strcpy(notice + n, "; ");
    strcat(notice, part);
}

/* What is open closes, the cut gets its notice, links resolve, and the document takes what it owns */
static int finish(struct rd *rd, struct ut_document *out) {
    struct ut_document d;
    char part[160];
    size_t i;
    button_close(rd);
    select_close(rd);
    link_close(rd);
    close_span(rd);
    if(rd->failed) return -1;
    if(rd->text) {                                  /* no spaces or line breaks at the end */
        trim(rd);
        while(rd->tlen && rd->text[rd->tlen - 1] == '\n') rd->tlen--;
        for(i = 0; i < rd->nanchors; i++) if(rd->anchors[i].offset > rd->tlen) rd->anchors[i].offset = rd->tlen;
    }
    place_anchors(rd, rd->tlen);
    if(rd->stopped) {                               /* the text is full: a last line says so */
        rd->stopped = 0;
        rd->limit = UT_TEXT_MAX;
        if((rd->tlen && put(rd, "\n\n", 2)) || put(rd, cut_notice, sizeof(cut_notice) - 1)) return -1;
    }
    if(!rd->text) {                                 /* a page with no text at all */
        rd->text = malloc(1);
        if(!rd->text) return -1;
        rd->tcap = 1;
    }
    rd->text[rd->tlen] = 0;
    if(rd->tcap > rd->tlen + 1) {
        char *t = realloc(rd->text, rd->tlen + 1);
        if(t) {
            rd->text = t;
            rd->tcap = rd->tlen + 1;
        }
    }
    memset(&d, 0, sizeof(d));
    d.title = malloc(rd->title_len + 1);
    if(!d.title) return -1;
    memcpy(d.title, rd->title, rd->title_len);
    d.title[rd->title_len] = 0;
    d.text = rd->text;
    d.text_len = rd->tlen;
    d.links = shrink(rd->links, rd->nlinks, sizeof(*d.links), &rd->links_cap);
    d.link_count = rd->nlinks;
    d.anchors = shrink(rd->anchors, rd->nanchors, sizeof(*d.anchors), &rd->anchors_cap);
    d.anchor_count = rd->nanchors;
    d.spans = shrink(rd->spans, rd->nspans, sizeof(*d.spans), &rd->spans_cap);
    d.span_count = rd->nspans;
    d.truncated = rd->truncated;
    d.bytes = rd->tcap + rd->title_len + 1 + rd->links_cap * sizeof(*d.links) +
              rd->anchors_cap * sizeof(*d.anchors) + rd->spans_cap * sizeof(*d.spans);
    for(i = 0; i < d.link_count; i++) d.bytes += strlen(d.links[i].label) + 1 + strlen(d.links[i].url) + 1;
    for(i = 0; i < d.anchor_count; i++) d.bytes += strlen(d.anchors[i].name) + 1;
    if(rd->truncated & UT_TRUNC_LINKS) {             /* the count or the bytes, whichever applied */
        if(rd->nlinks) snprintf(part, sizeof(part), "only its first %u links have numbers", (unsigned)rd->nlinks);
        else snprintf(part, sizeof(part), "%s", "its links have no numbers");
        add_notice(d.notice, sizeof(d.notice), part);
    }
    if(rd->cs == CS_ASCII) {                        /* a long name is cut, so that every part fits */
        char name[25];
        copy_cut(name, sizeof(name), rd->charset);
        if(strlen(rd->charset) > 24) strcpy(name + 21, "...");
        snprintf(part, sizeof(part), "the page says it is in %s; only its ASCII is shown", name);
        add_notice(d.notice, sizeof(d.notice), part);
    }
    if(rd->forms) add_notice(d.notice, sizeof(d.notice), "forms are not supported");
    rd->given = 1;                                  /* the document's now */
    memcpy(out, &d, sizeof(d));
    return 0;
}

static void cleanup_parts(struct rd *rd);

/* Free what the document did not take, and the renderer */
static void cleanup(struct rd *rd) {
    size_t i;
    for(i = 0; rd->hrefs && i < rd->nlinks; i++) free(rd->hrefs[i]);
    free(rd->hrefs);
    if(!rd->given) cleanup_parts(rd);
    free(rd->base);
    free(rd->decoded);
    free(rd->work);
    free(rd);
}

static void cleanup_parts(struct rd *rd) {
    size_t i;
    for(i = 0; rd->links && i < rd->nlinks; i++) {
        free(rd->links[i].label);
        free(rd->links[i].url);
    }
    free(rd->links);
    for(i = 0; rd->anchors && i < rd->nanchors; i++) free(rd->anchors[i].name);
    free(rd->anchors);
    free(rd->spans);
    free(rd->text);
}

/* One reading of r, as far as it goes (rd->failed: out of memory, or rd->again). base: a <base> known from the start,
   which it takes (NULL: none yet). NULL when memory is out. */
static struct rd *read_page(const struct ut_response *r, struct ut_url *base) {
    struct rd *rd = calloc(1, sizeof(*rd));
    int html;
    if(!rd) {
        free(base);
        return NULL;
    }
    rd->r = r;
    rd->in = r->body ? r->body : (const unsigned char *)"";
    rd->len = r->len;
    rd->limit = UT_TEXT_MAX - (sizeof(cut_notice) - 1) - 2;     /* room is kept for the notice of a cut */
    rd->depth = 1;
    rd->st[0].el = E_HTML;
    rd->st[0].lst = -1;
    rd->base = base;
    rd->base_done = base != NULL;
    rd->tag.attrs = attr_info;
    rd->tag.nattrs = A_COUNT;
    rd->tag.val[A_HREF] = rd->tagbuf.href;
    rd->tag.val[A_ID] = rd->tagbuf.id;
    rd->tag.val[A_NAME] = rd->tagbuf.name_;
    rd->tag.val[A_ALT] = rd->tagbuf.alt;
    rd->tag.val[A_VALUE] = rd->tagbuf.value;
    rd->tag.val[A_TYPE] = rd->tagbuf.type;
    rd->tag.val[A_START] = rd->tagbuf.start;
    rd->tag.val[A_ARIA] = rd->tagbuf.aria;
    rd->tag.val[A_TITLE] = rd->tagbuf.title;
    if(decode_utf16(rd)) {
        cleanup(rd);
        return NULL;
    }
    rd->xhtml = !strcmp(r->content_type, "application/xhtml+xml");
    html = rd->xhtml || !strcmp(r->content_type, "text/html") || (!r->content_type[0] && looks_like_html(rd->in, rd->len));
    if(!rd->decoded) choose_charset(rd, html);
    if(html) run_html(rd);
    else run_plain(rd);
    return rd;
}

int ut_render(const struct ut_response *r, struct ut_document *out, char *err, size_t cap) {
    struct rd *rd;
    struct ut_url *base;
    if(!r || !out || (!r->body && r->len)) return ut_error(err, cap, "there is no page to show");
    rd = read_page(r, NULL);
    if(rd && rd->again) {                           /* a late <base> too big for the links before it: from the start */
        base = rd->base;
        rd->base = NULL;
        cleanup(rd);
        rd = read_page(r, base);                    /* with base_done set, at most once */
    }
    if(!rd || finish(rd, out)) {
        if(rd) cleanup(rd);
        return ut_error(err, cap, "out of memory showing the page");
    }
    cleanup(rd);
    return 0;
}

void ut_document_free(struct ut_document *d) {
    size_t i;
    if(!d) return;
    for(i = 0; d->links && i < d->link_count; i++) {
        free(d->links[i].label);
        free(d->links[i].url);
    }
    for(i = 0; d->anchors && i < d->anchor_count; i++) free(d->anchors[i].name);
    free(d->text);
    free(d->title);
    free(d->links);
    free(d->anchors);
    free(d->spans);
    memset(d, 0, sizeof(*d));
}

/* name is fragment once fragment's %XX escapes are decoded */
static int same_decoded(const char *name, const char *fragment) {
    const unsigned char *f = (const unsigned char *)fragment;
    int hi, lo;
    while(*f) {
        unsigned char c = *f++;
        if(c == '%' && (hi = hexval(f[0])) >= 0 && (lo = hexval(f[1])) >= 0) {
            c = (unsigned char)(hi * 16 + lo);
            f += 2;
        }
        if(!c || (unsigned char)*name++ != c) return 0;
    }
    return !*name;
}

int ut_anchor_offset(const struct ut_document *d, const char *fragment, size_t *offset) {
    size_t i;
    if(!d || !fragment || !offset) return -1;
    for(i = 0; i < d->anchor_count; i++)
        if(!strcmp(d->anchors[i].name, fragment)) {
            *offset = d->anchors[i].offset;
            return 0;
        }
    if(strchr(fragment, '%'))                       /* as a browser does, with its escapes decoded */
        for(i = 0; i < d->anchor_count; i++)
            if(same_decoded(d->anchors[i].name, fragment)) {
                *offset = d->anchors[i].offset;
                return 0;
            }
    if(!fragment[0] || ci_eq((const unsigned char *)fragment, strlen(fragment), "top")) {
        *offset = 0;
        return 0;
    }
    return -1;
}
