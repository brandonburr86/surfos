/*
SurfOS user program: wcode
--------------------
File: doc.c     Date: 11/05/26
--------------------
A file open in the Code Editor: moving, selecting and editing (doc.h says what each does). Every
change goes through wc_doc_put() and wc_doc_cut(), which change the text, keep the history's ops
and carry the selection along, inside a step begun by wc_doc_begin() and ended by wc_doc_end();
nothing else writes the text, so the history and the text never disagree. Undo replays a step's
ops backwards, redo forwards, without recording them again.

The rules for indentation are the ones most code editors share; they are no one program's code.
The auto-indent's questions (does this line open a block? where is the brace this one closes?)
are asked of the colouring's tokenizers, so a brace in a comment or a string opens nothing.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "doc.h"

#define BYTE(d, p) wc_text_byte(&(d)->t, (p))
#define INDENT_MAX 512                  /* an indentation copied to a new line, at most */

static int blank(int c) {
    return c == ' ' || c == '\t';
}

int wc_doc_init(struct wc_doc *d, struct wc_pool *pool) {
    memset(d, 0, sizeof(*d));
    if(wc_text_init(&d->t, pool) < 0) return -1;
    wc_undo_init(&d->u);
    d->want_col = -1;
    d->indent = 4;
    d->saved = wc_undo_serial(&d->u);
    return 0;
}

void wc_doc_free(struct wc_doc *d) {
    wc_text_free(&d->t);
    wc_undo_free(&d->u);
}

/* how the file is indented: by the most of its first lines, tabs or spaces, and of spaces the commonest step */
static void detect_indent(struct wc_doc *d) {
    struct wc_text *t = &d->t;
    struct wc_line l;
    unsigned tabs = 0, spaces = 0, steps[9] = { 0 }, prev = 0, pos = 0, lines, k, n, step;
    for(lines = 0; lines < 10000 && pos <= t->len; lines++) {
        wc_text_line(t, pos, &l);
        for(k = l.start, n = 0; k < l.end && BYTE(d, k) == ' '; k++) n++;
        if(k < l.end && BYTE(d, k) == '\t' && !n) tabs++;
        else if(k < l.end && !blank(BYTE(d, k))) {             /* a line with text: its spaces count */
            if(n) spaces++;
            step = n > prev ? n - prev : prev - n;
            if(step == 2 || step == 4 || step == 8) steps[step]++;
            prev = n;
        }
        if(!l.eol) break;
        pos = l.end + l.eol;
    }
    d->indent_tab = d->lang == WC_LANG_MAKE || tabs > spaces;
    d->indent = 4;
    if(spaces && steps[2] + steps[4] + steps[8]) {
        d->indent = steps[2] > steps[4] && steps[2] > steps[8] ? 2 : steps[8] > steps[4] ? 8 : 4;
    }
}

int wc_doc_loaded(struct wc_doc *d, const char *path) {
    struct wc_text *t = &d->t;
    struct wc_line l;
    const char *a, *b;
    unsigned na, nb, lf = 0, crlf = 0, q, pos;
    wc_text_parts(t, &a, &na, &b, &nb);
    if((na && memchr(a, 0, na)) || (nb && memchr(b, 0, nb))) return WC_NOT_TEXT;
    snprintf(d->path, sizeof(d->path), "%s", path ? path : "");
    for(pos = 0; (q = wc_text_find_nl(t, pos, t->len)) < t->len; pos = q + 1) {
        if(q > 0 && BYTE(d, q - 1) == '\r') crlf++;
        else lf++;
    }
    d->eols = !lf && !crlf ? WC_EOL_NONE : lf && crlf ? WC_EOL_MIXED : crlf ? WC_EOL_CRLF : WC_EOL_LF;
    d->crlf = crlf > lf;
    wc_text_line(t, 0, &l);
    q = l.end < 256 ? l.end : 256;
    d->lang = wc_lang_guess(d->path, wc_text_span(t, 0, q), q);
    detect_indent(d);
    wc_undo_free(&d->u);
    wc_undo_init(&d->u);
    d->saved = wc_undo_serial(&d->u);
    wc_syn_reset(t);
    d->anchor = d->caret = 0;
    d->want_col = -1;
    d->top = d->left = 0;
    d->unsure = 0;
    d->copied[0] = d->twin[0] = 0;
    return 0;
}

void wc_doc_set_lang(struct wc_doc *d, int lang) {
    if(lang < 0 || lang >= WC_LANGS) lang = WC_LANG_TEXT;
    d->lang = lang;
    if(lang == WC_LANG_MAKE) d->indent_tab = 1;
    wc_syn_reset(&d->t);
}

int wc_doc_modified(const struct wc_doc *d) {
    return wc_undo_serial(&d->u) != d->saved;
}

int wc_doc_safe(const struct wc_doc *d) {
    return !wc_doc_modified(d) || (d->copied[0] && d->copied_serial == wc_undo_serial(&d->u));
}

void wc_doc_saved(struct wc_doc *d) {
    d->saved = wc_undo_serial(&d->u);
    wc_undo_close(&d->u);                       /* typing on starts a step of its own, so undo comes back here */
}

void wc_doc_title(const struct wc_doc *d, char *out, unsigned size) {
    const char *base = strrchr(d->path, '/');
    if(!d->path[0]) snprintf(out, size, "Untitled %u", d->untitled ? d->untitled : 1);
    else snprintf(out, size, "%s", base ? base + 1 : d->path);
}

/**** the selection ****/

unsigned wc_doc_sel(const struct wc_doc *d, unsigned *from, unsigned *to) {
    *from = d->anchor < d->caret ? d->anchor : d->caret;
    *to = d->anchor < d->caret ? d->caret : d->anchor;
    return *to - *from;
}

void wc_doc_select(struct wc_doc *d, unsigned anchor, unsigned caret) {
    d->anchor = wc_text_snap(&d->t, anchor);
    d->caret = wc_text_snap(&d->t, caret);
    d->want_col = -1;
    wc_undo_close(&d->u);
}

void wc_doc_select_all(struct wc_doc *d) {
    wc_doc_select(d, 0, d->t.len);
}

/* what a character is to words: 0 blank, 1 a word's (letters, digits, _ and every other script's), 2 punctuation and
   bytes that are no UTF-8, 3 a line end */
static int word_class(unsigned cp) {
    if(cp == ' ' || cp == '\t') return 0;
    if(cp == '\n') return 3;
    if(cp == '_' || (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
       (cp >= 0x80 && !(cp & WC_BAD))) return 1;
    return 2;
}

static unsigned class_at(struct wc_doc *d, unsigned pos) {
    unsigned cp;
    if(!wc_text_char(&d->t, pos, &cp)) return 3;
    return (unsigned)word_class(cp);
}

void wc_doc_select_word(struct wc_doc *d, unsigned pos) {
    struct wc_text *t = &d->t;
    unsigned a, b, k, p;
    pos = wc_text_snap(t, pos);
    if(class_at(d, pos) == 3 && pos > 0 && class_at(d, wc_text_prev(t, pos)) != 3) pos = wc_text_prev(t, pos);
    k = class_at(d, pos);
    if(k == 3) {
        wc_doc_select(d, pos, pos);
        return;
    }
    for(a = pos; a > 0 && class_at(d, p = wc_text_prev(t, a)) == k; a = p);
    for(b = pos; b < t->len && class_at(d, b) == k; b = wc_text_next(t, b));
    wc_doc_select(d, a, b);
}

void wc_doc_select_line(struct wc_doc *d, unsigned line, int extend) {
    struct wc_text *t = &d->t;
    struct wc_line l;
    unsigned a, s = wc_text_line_start(t, line);
    wc_text_line(t, s, &l);
    if(!extend) {
        wc_doc_select(d, s, l.end + l.eol);
        return;
    }
    a = wc_text_line_begin(t, d->anchor);       /* from the anchor's line to this one, whole */
    if(s >= a) wc_doc_select(d, a, l.end + l.eol);
    else {
        wc_text_line(t, a, &l);
        wc_doc_select(d, l.end + l.eol, s);
    }
}

/**** moving ****/

static unsigned word_right(struct wc_doc *d, unsigned pos) {
    struct wc_text *t = &d->t;
    unsigned cp, n = wc_text_char(t, pos, &cp);
    int k;
    if(!n) return pos;
    if(cp == '\n') return pos + n;              /* a line end: to the next line's start */
    while(n && (cp == ' ' || cp == '\t')) {
        pos += n;
        n = wc_text_char(t, pos, &cp);
    }
    if(!n || cp == '\n') return pos;
    k = word_class(cp);
    while(n && word_class(cp) == k) {
        pos += n;
        n = wc_text_char(t, pos, &cp);
    }
    return pos;
}

static unsigned word_left(struct wc_doc *d, unsigned pos) {
    struct wc_text *t = &d->t;
    unsigned p, cp;
    int k;
    if(!pos) return 0;
    p = wc_text_prev(t, pos);
    wc_text_char(t, p, &cp);
    if(cp == '\n') return p;                    /* a line's start: to the end of the line before */
    for(; pos > 0; pos = p) {
        p = wc_text_prev(t, pos);
        wc_text_char(t, p, &cp);
        if(cp != ' ' && cp != '\t') break;
    }
    if(!pos || cp == '\n') return pos;
    k = word_class(cp);
    for(; pos > 0; pos = p) {
        p = wc_text_prev(t, pos);
        wc_text_char(t, p, &cp);
        if(word_class(cp) != k) break;
    }
    return pos;
}

void wc_doc_move(struct wc_doc *d, int how, int extend, unsigned page) {
    struct wc_text *t = &d->t;
    struct wc_line l;
    unsigned from, to, c = d->caret, line, n, k;
    int sel = wc_doc_sel(d, &from, &to) > 0, keep = 0;
    switch(how) {
    case WC_LEFT: c = sel && !extend ? from : wc_text_prev(t, c); break;
    case WC_RIGHT: c = sel && !extend ? to : wc_text_next(t, c); break;
    case WC_WORD_LEFT: c = word_left(d, c); break;
    case WC_WORD_RIGHT: c = word_right(d, c); break;
    case WC_UP: case WC_DOWN: case WC_PGUP: case WC_PGDN:
        n = how == WC_UP || how == WC_DOWN ? 1 : page > 1 ? page - 1 : 1;
        line = wc_text_line_of(t, c);
        if(d->want_col < 0) d->want_col = (int)wc_text_col(t, wc_text_line_start(t, line), c, WC_TABW);
        if(how == WC_UP || how == WC_PGUP) {
            if(!line) {
                c = 0;
                break;
            }
            line = line > n ? line - n : 0;
        } else {
            if(line >= t->nl) {
                c = t->len;
                break;
            }
            line = n > t->nl - line ? t->nl : line + n;
        }
        wc_text_line(t, wc_text_line_start(t, line), &l);
        c = wc_text_at_col(t, &l, (unsigned)d->want_col, WC_TABW);
        keep = 1;
        break;
    case WC_HOME:                               /* the indentation's end, and from there the line's start */
        wc_text_line(t, wc_text_line_begin(t, c), &l);
        for(k = l.start; k < l.end && blank(BYTE(d, k)); k++);
        c = c == k ? l.start : k;
        break;
    case WC_END:
        wc_text_line(t, wc_text_line_begin(t, c), &l);
        c = l.end;
        break;
    case WC_DOC_START: c = 0; break;
    case WC_DOC_END: c = t->len; break;
    }
    d->caret = c;
    if(!extend) d->anchor = c;
    if(!keep) d->want_col = -1;
    wc_undo_close(&d->u);
}

unsigned wc_doc_line(struct wc_doc *d) {
    return wc_text_line_of(&d->t, d->caret);
}

unsigned wc_doc_col(struct wc_doc *d) {
    return wc_text_col(&d->t, wc_text_line_begin(&d->t, d->caret), d->caret, WC_TABW);
}

int wc_doc_goto(struct wc_doc *d, unsigned line, unsigned col) {
    struct wc_text *t = &d->t;
    struct wc_line l;
    unsigned c;
    if(!line) line = 1;
    if(line > t->nl + 1) {
        snprintf(d->msg, sizeof(d->msg), "There are %u lines: the last one", t->nl + 1);
        line = t->nl + 1;
    }
    wc_text_line(t, wc_text_line_start(t, line - 1), &l);
    c = col > 1 ? wc_text_at_col(t, &l, col - 1, WC_TABW) : l.start;
    wc_doc_select(d, c, c);
    return 0;
}

int wc_doc_bracket(struct wc_doc *d, int extend) {
    unsigned at, other;
    if(!wc_syn_match(&d->t, d->lang, d->caret, &at, &other, 100000, 16u << 20)) {
        snprintf(d->msg, sizeof(d->msg), at == WC_NONE ? "No bracket at the cursor" : "No matching bracket");
        return 0;
    }
    /* to the far side of the other: after a closing one, before an opening one, as the caret stood at this */
    if(other > at) other++;
    d->caret = other;
    if(!extend) d->anchor = other;
    d->want_col = -1;
    wc_undo_close(&d->u);
    return 1;
}

/**** changes ****/

void wc_doc_begin(struct wc_doc *d, int kind) {
    wc_undo_begin(&d->u, kind, d->anchor, d->caret);
}

/* the selection moves with the text: a position after pos by what went in */
int wc_doc_put(struct wc_doc *d, unsigned pos, const char *s, unsigned n) {
    if(!n) return 0;
    if(wc_text_insert(&d->t, pos, s, n) < 0) {
        snprintf(d->msg, sizeof(d->msg), errno == EFBIG ? "Not changed: a file may have 16 MB at most" :
                 "Not changed: there is not enough memory");
        return -1;
    }
    wc_undo_op(&d->u, 1, pos, s, n);
    if(d->anchor > pos) d->anchor += n;
    if(d->caret > pos) d->caret += n;
    return 0;
}

void wc_doc_cut(struct wc_doc *d, unsigned pos, unsigned n) {
    if(pos >= d->t.len || !n) return;
    if(n > d->t.len - pos) n = d->t.len - pos;
    if(n > WC_UNDO_BYTES) wc_undo_op(&d->u, 0, pos, NULL, n);      /* too large: the history goes, nothing copied */
    else wc_undo_op(&d->u, 0, pos, wc_text_span(&d->t, pos, n), n);
    wc_text_delete(&d->t, pos, n);
    if(d->anchor > pos) d->anchor = d->anchor > pos + n ? d->anchor - n : pos;
    if(d->caret > pos) d->caret = d->caret > pos + n ? d->caret - n : pos;
}

void wc_doc_end(struct wc_doc *d) {
    d->anchor = wc_text_snap(&d->t, d->anchor);     /* a lone CR that met a line feed */
    d->caret = wc_text_snap(&d->t, d->caret);
    wc_undo_end(&d->u, d->anchor, d->caret);
    if(d->u.forgot) snprintf(d->msg, sizeof(d->msg), "Too large a change to undo: the history is forgotten");
}

/* the selection goes, inside a step begun */
static void cut_sel(struct wc_doc *d) {
    unsigned from, to;
    if(!wc_doc_sel(d, &from, &to)) return;
    wc_doc_cut(d, from, to - from);
    d->anchor = d->caret = from;
}

/* the indentation of the line that starts at ls: its bytes into out (at most size), *n of them; its end */
static unsigned indentation(struct wc_doc *d, unsigned ls, unsigned upto, char *out, unsigned size, unsigned *n) {
    unsigned k;
    *n = 0;
    for(k = ls; k < upto && blank(BYTE(d, k)); k++)
        if(*n < size) out[(*n)++] = (char)BYTE(d, k);
    return k;
}

/* do the bytes at pos say s? */
static int same(struct wc_doc *d, unsigned pos, unsigned n, const char *s, unsigned m) {
    unsigned k;
    if(n != m) return 0;
    for(k = 0; k < n; k++)
        if(BYTE(d, pos + k) != (unsigned char)s[k]) return 0;
    return 1;
}

/* the line's classes, from its start to upto, into cls: how many there are, 0 for a line too long to look at */
static unsigned classes(struct wc_doc *d, unsigned ls, unsigned upto, unsigned char *cls, unsigned size) {
    struct wc_line l;
    unsigned st;
    if(upto - ls > size) return 0;
    st = wc_syn_state(&d->t, d->lang, wc_text_line_of(&d->t, ls), 4u << 20, NULL);
    wc_text_line(&d->t, ls, &l);
    wc_syn_classes(&d->t, d->lang, &l, st, cls, size);
    return upto - ls;
}

/* the last character of code before upto on the line (blanks and comments left out): its position, or WC_NONE */
static unsigned last_code(struct wc_doc *d, unsigned ls, unsigned upto, const unsigned char *cls, unsigned ncls) {
    unsigned k = upto - ls < ncls ? upto - ls : ncls;
    while(k > 0) {
        k--;
        if(!blank(BYTE(d, ls + k)) && cls[k] != WC_C_COMMENT) return ls + k;
    }
    return WC_NONE;
}

/* does the line before upto open a block, so that the next line takes a step more? */
static int opens(struct wc_doc *d, unsigned ls, unsigned upto) {
    static unsigned char cls[WC_LINE_MAX];
    static const char *const words[] = { "do", "else", "then" };
    unsigned n, at, k, w;
    int c;
    if(d->lang == WC_LANG_TEXT || d->lang == WC_LANG_MD || d->lang == WC_LANG_NASM || d->lang == WC_LANG_GAS)
        return 0;
    n = classes(d, ls, upto, cls, sizeof(cls));
    if(d->lang == WC_LANG_MAKE)                 /* a rule's line: its recipe follows */
        return n && BYTE(d, ls) != '\t' && cls[0] == WC_C_LABEL;
    at = last_code(d, ls, upto, cls, n);
    if(at == WC_NONE || cls[at - ls] == WC_C_STRING) return 0;
    c = BYTE(d, at);
    if(c == '{' || c == '(' || c == '[') return 1;
    if(d->lang == WC_LANG_PY) return c == ':';
    if(d->lang == WC_LANG_SH && cls[at - ls] == WC_C_KEYWORD) {
        for(k = at; k > ls && cls[k - 1 - ls] == WC_C_KEYWORD && !blank(BYTE(d, k - 1)); k--);
        for(w = 0; w < sizeof(words) / sizeof(words[0]); w++) {
            n = (unsigned)strlen(words[w]);
            if(at + 1 - k == n) {
                char b[8];
                wc_text_get(&d->t, k, b, n);
                if(!memcmp(b, words[w], n)) return 1;
            }
        }
    }
    return 0;
}

/* one step of indentation: a tab, or the file's spaces */
static unsigned unit(struct wc_doc *d, char *out) {
    unsigned k;
    if(d->indent_tab) {
        out[0] = '\t';
        return 1;
    }
    for(k = 0; k < (unsigned)d->indent && k < 8; k++) out[k] = ' ';
    return k;
}

int wc_doc_type(struct wc_doc *d, const char *s, unsigned n) {
    char ind[INDENT_MAX];
    unsigned at, other, ls, k, m, close;
    int r;
    if(!n) return 0;
    wc_doc_begin(d, WC_K_TYPE);
    cut_sel(d);
    r = wc_doc_put(d, d->caret, s, n);
    if(!r) {
        d->caret += n;
        d->anchor = d->caret;
    }
    /* a closing bracket alone on its line: the line takes the indentation of its opening one's */
    if(!r && n == 1 && (s[0] == '}' || s[0] == ']' || s[0] == ')') && (wc_lang_braces(d->lang) || d->lang == WC_LANG_PY)) {
        close = d->caret - 1;
        ls = wc_text_line_begin(&d->t, close);
        for(k = ls; k < close && blank(BYTE(d, k)); k++);
        if(k == close && wc_syn_match(&d->t, d->lang, close, &at, &other, 2000, 1u << 20) && at == close) {
            indentation(d, wc_text_line_begin(&d->t, other), other, ind, sizeof(ind), &m);
            if(m < sizeof(ind) && !same(d, ls, close - ls, ind, m)) {
                wc_doc_cut(d, ls, close - ls);
                wc_doc_put(d, ls, ind, m);
                d->anchor = d->caret = ls + m + 1;
            }
        }
    }
    d->want_col = -1;
    wc_doc_end(d);
    return r;
}

int wc_doc_enter(struct wc_doc *d) {
    char buf[2 * INDENT_MAX + 16];
    unsigned ls, k, m, n = 0, first, end;
    int open = 0, c, r;
    wc_doc_begin(d, WC_K_OTHER);
    cut_sel(d);
    ls = wc_text_line_begin(&d->t, d->caret);
    end = indentation(d, ls, d->caret, buf + 2, INDENT_MAX, &m);
    if(end == d->caret && d->caret > ls) {      /* nothing but indentation before the caret: it moves down with it */
        wc_doc_cut(d, ls, d->caret - ls);
        d->anchor = d->caret = ls;
    } else if(end < d->caret) open = opens(d, ls, d->caret);
    if(m >= INDENT_MAX) m = INDENT_MAX - 1;
    if(d->crlf) buf[n++] = '\r';
    buf[n++] = '\n';
    memmove(buf + n, buf + 2, m);               /* the line end, then the indentation */
    n += m;
    if(open) n += unit(d, buf + n);
    first = n;
    if(open && d->lang != WC_LANG_MAKE) {       /* between brackets: the closing one on a line of its own */
        for(k = d->caret; k < d->t.len && blank(BYTE(d, k)); k++);
        c = k < d->t.len ? BYTE(d, k) : 0;
        if(c == '}' || c == ']' || c == ')') {
            unsigned at, other;
            if(wc_syn_match(&d->t, d->lang, k, &at, &other, 2, 1u << 16) && at == k && other < d->caret) {
                if(d->crlf) buf[n++] = '\r';
                buf[n++] = '\n';
                memcpy(buf + n, buf + (d->crlf ? 2 : 1), m);
                n += m;
            }
        }
    }
    r = wc_doc_put(d, d->caret, buf, n);
    if(!r) d->anchor = d->caret = d->caret + first;
    d->want_col = -1;
    wc_doc_end(d);
    return r;
}

/* the lines a selection takes (one ending at a line's start leaves that line out) */
static void sel_lines(struct wc_doc *d, unsigned *l0, unsigned *l1) {
    unsigned from, to;
    wc_doc_sel(d, &from, &to);
    *l0 = wc_text_line_of(&d->t, from);
    *l1 = wc_text_line_of(&d->t, to);
    if(*l1 > *l0 && wc_text_line_start(&d->t, *l1) == to) (*l1)--;
}

/* the lines l0 to l1 selected whole (their text, not the last line end), the way the selection ran */
static void select_lines(struct wc_doc *d, unsigned l0, unsigned l1, int backwards) {
    struct wc_line l;
    unsigned a = wc_text_line_start(&d->t, l0);
    wc_text_line(&d->t, wc_text_line_start(&d->t, l1), &l);
    if(backwards) {
        d->anchor = l.end;
        d->caret = a;
    } else {
        d->anchor = a;
        d->caret = l.end;
    }
}

int wc_doc_tab(struct wc_doc *d, int back) {
    unsigned from, to, n, col;
    int r = 0;
    wc_doc_sel(d, &from, &to);
    if(!back && (from == to || wc_text_line_of(&d->t, from) == wc_text_line_of(&d->t, to))) {
        wc_doc_begin(d, WC_K_TYPE);             /* a tab, or spaces to the next step */
        cut_sel(d);
        if(d->indent_tab) {
            n = 1;
            r = wc_doc_put(d, d->caret, "\t", 1);
        } else {
            col = wc_doc_col(d);
            n = (unsigned)d->indent - col % (unsigned)d->indent;
            r = wc_doc_put(d, d->caret, "        ", n);
        }
        if(!r) d->anchor = d->caret = d->caret + n;
        d->want_col = -1;
        wc_doc_end(d);
        return r;
    }
    return wc_doc_indent(d, back);
}

int wc_doc_indent(struct wc_doc *d, int back) {
    struct wc_line l;
    char u[8];
    unsigned l0, l1, line, ls, k, n, m;
    int backwards = d->caret < d->anchor, r = 0, had = d->caret != d->anchor;
    sel_lines(d, &l0, &l1);
    wc_doc_begin(d, WC_K_OTHER);
    n = unit(d, u);
    for(line = l1 + 1; line-- > l0 && !r;) {
        ls = wc_text_line_start(&d->t, line);
        wc_text_line(&d->t, ls, &l);
        if(!back) {
            if(l.end > l.start) r = wc_doc_put(d, ls, u, n);   /* empty lines stay empty */
        } else if(ls < l.end && BYTE(d, ls) == '\t') wc_doc_cut(d, ls, 1);
        else {
            for(k = ls, m = 0; k < l.end && m < (unsigned)d->indent && BYTE(d, k) == ' '; k++) m++;
            wc_doc_cut(d, ls, m);
        }
    }
    if(had) select_lines(d, l0, l1, backwards);     /* the lines stay selected; a caret alone stays a caret */
    d->want_col = -1;
    wc_doc_end(d);
    return r;
}

int wc_doc_backspace(struct wc_doc *d, int word) {
    unsigned from, to, c = d->caret, p, ls, k;
    if(wc_doc_sel(d, &from, &to)) return wc_doc_erase(d);
    if(!c) return 0;
    if(word) p = word_left(d, c);
    else {
        p = wc_text_prev(&d->t, c);
        if(!d->indent_tab && d->indent > 1) {   /* in an indentation of spaces: a step back */
            ls = wc_text_line_begin(&d->t, c);
            for(k = ls; k < c && BYTE(d, k) == ' '; k++);
            if(k == c && c > ls) p = ls + (c - ls - 1) / (unsigned)d->indent * (unsigned)d->indent;
        }
    }
    wc_doc_begin(d, WC_K_BACK);
    wc_doc_cut(d, p, c - p);
    d->anchor = d->caret = p;
    d->want_col = -1;
    wc_doc_end(d);
    return 0;
}

int wc_doc_delete(struct wc_doc *d, int word) {
    unsigned from, to, c = d->caret, p;
    if(wc_doc_sel(d, &from, &to)) return wc_doc_erase(d);
    if(c >= d->t.len) return 0;
    p = word ? word_right(d, c) : wc_text_next(&d->t, c);
    wc_doc_begin(d, WC_K_DEL);
    wc_doc_cut(d, c, p - c);
    d->anchor = d->caret = c;
    d->want_col = -1;
    wc_doc_end(d);
    return 0;
}

int wc_doc_erase(struct wc_doc *d) {
    unsigned from, to;
    if(!wc_doc_sel(d, &from, &to)) return 0;
    wc_doc_begin(d, WC_K_OTHER);
    cut_sel(d);
    d->want_col = -1;
    wc_doc_end(d);
    return 0;
}

int wc_doc_paste(struct wc_doc *d, const char *s, unsigned n) {
    unsigned k, m = 0, len;
    char *buf;
    int r;
    for(len = 0; len < n && s[len]; len++);
    if(!len) return 0;
    buf = malloc((size_t)len * 2);
    if(!buf) {
        snprintf(d->msg, sizeof(d->msg), "Not pasted: there is not enough memory");
        return -1;
    }
    for(k = 0; k < len; k++) {                  /* every line end the file's kind; a lone CR stays one */
        if(s[k] == '\r' && k + 1 < len && s[k + 1] == '\n') continue;
        if(s[k] == '\n' && d->crlf) buf[m++] = '\r';
        buf[m++] = s[k];
    }
    wc_doc_begin(d, WC_K_OTHER);
    cut_sel(d);
    r = wc_doc_put(d, d->caret, buf, m);
    if(!r) d->anchor = d->caret = d->caret + m;
    d->want_col = -1;
    wc_doc_end(d);
    free(buf);
    return r;
}

unsigned wc_doc_copy(struct wc_doc *d, char *out, unsigned size) {
    unsigned from, to, k, n = 0;
    int c;
    if(!wc_doc_sel(d, &from, &to) || !size) {
        snprintf(d->msg, sizeof(d->msg), "Nothing is selected");
        return 0;
    }
    for(k = from; k < to; k++) {
        c = BYTE(d, k);
        if(c == '\r' && k + 1 < to && BYTE(d, k + 1) == '\n') continue;     /* the clipboard's line ends are LF */
        if(n + 1 >= size) {
            snprintf(d->msg, sizeof(d->msg), "Not copied: the clipboard holds %u KB at most", size / 1024);
            return 0;
        }
        out[n++] = (char)c;
    }
    out[n] = 0;
    return n;
}

int wc_doc_comment(struct wc_doc *d) {
    const char *tok = wc_lang_comment(d->lang);
    struct wc_line l;
    char b[8];
    unsigned l0, l1, line, ls, k, n, mind = WC_NONE, all = 1, any = 0;
    int backwards = d->caret < d->anchor, r = 0;
    if(!tok) {
        snprintf(d->msg, sizeof(d->msg), "%s has no line comments", wc_lang_name(d->lang));
        return -1;
    }
    n = (unsigned)strlen(tok);
    sel_lines(d, &l0, &l1);
    for(line = l0; line <= l1; line++) {        /* are they all comments already? where do they start? */
        wc_text_line(&d->t, wc_text_line_start(&d->t, line), &l);
        for(k = l.start; k < l.end && blank(BYTE(d, k)); k++);
        if(k == l.end) continue;
        any = 1;
        if(k - l.start < mind) mind = k - l.start;
        if(l.end - k < n) all = 0;
        else {
            wc_text_get(&d->t, k, b, n);
            if(memcmp(b, tok, n)) all = 0;
        }
    }
    if(!any) return 0;
    wc_doc_begin(d, WC_K_OTHER);
    for(line = l1 + 1; line-- > l0 && !r;) {
        ls = wc_text_line_start(&d->t, line);
        wc_text_line(&d->t, ls, &l);
        for(k = l.start; k < l.end && blank(BYTE(d, k)); k++);
        if(k == l.end) continue;
        if(all) wc_doc_cut(d, k, n + (k + n < l.end && BYTE(d, k + n) == ' '));
        else {
            memcpy(b, tok, n);
            b[n] = ' ';
            r = wc_doc_put(d, ls + mind, b, n + 1);
        }
    }
    select_lines(d, l0, l1, backwards);
    d->want_col = -1;
    wc_doc_end(d);
    return r;
}

int wc_doc_replace(struct wc_doc *d, unsigned from, unsigned to, const char *s, unsigned n, int select) {
    int r;
    if(to > d->t.len) to = d->t.len;
    if(from > to) from = to;
    wc_doc_begin(d, WC_K_OTHER);
    wc_doc_cut(d, from, to - from);
    r = wc_doc_put(d, from, s, n);
    d->anchor = from;
    d->caret = r ? from : from + n;
    if(!select) d->anchor = d->caret;
    d->want_col = -1;
    wc_doc_end(d);
    return r;
}

/* an op done again (undo 0) or undone (undo 1) */
static int replay(struct wc_doc *d, const struct wc_step *s, const struct wc_op *o, int undo) {
    if(o->ins != undo) return wc_text_insert(&d->t, o->pos, s->blob + o->off, o->n);
    wc_text_delete(&d->t, o->pos, o->n);
    return 0;
}

int wc_doc_undo(struct wc_doc *d, int redo) {
    const struct wc_step *s = redo ? wc_undo_forward(&d->u) : wc_undo_back(&d->u);
    unsigned i;
    int failed = 0;
    if(!s) {
        snprintf(d->msg, sizeof(d->msg), redo ? "Nothing to redo" : "Nothing to undo");
        return 0;
    }
    if(redo)
        for(i = 0; i < s->nops && !failed; i++) failed = replay(d, s, &s->ops[i], 0) < 0;
    else
        for(i = s->nops; i-- > 0 && !failed;) failed = replay(d, s, &s->ops[i], 1) < 0;
    if(failed) {                                /* the text is no longer what the history says */
        wc_undo_forget(&d->u);
        snprintf(d->msg, sizeof(d->msg), "Out of memory: the history is forgotten");
        d->anchor = d->caret = wc_text_snap(&d->t, d->caret);
        return -1;
    }
    d->anchor = wc_text_snap(&d->t, redo ? s->a1 : s->a0);
    d->caret = wc_text_snap(&d->t, redo ? s->c1 : s->c0);
    d->want_col = -1;
    return 0;
}
