/*
SurfOS user program: wcode
--------------------
File: text.c    Date: 11/05/26
--------------------
The Code Editor's text: a gap buffer and a sparse index of line starts (text.h says what they
hold). A gap buffer keeps the bytes in one block with a hole at the place being edited, so typing
moves no more than the bytes between the last edit and this one, and a file is read straight
into it. The index is a short sorted list of marks, roughly one in WC_MARK_EVERY lines, made by
the scans that pass through text without one and moved by every change: a lookup costs a search
of the list and a scan of at most the lines after one mark, and a 16 MB file of empty lines
needs no more of them than one of long lines needs. No line counts or lengths are kept per line,
so nothing limits the number of lines but the bytes.
*/
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "text.h"

#define PHYS(t, p) ((p) < (t)->gs ? (p) : (p) + ((t)->ge - (t)->gs))

/**** UTF-8 (RFC 3629: a sequence is the shortest form of a code point up to U+10FFFF, never a surrogate) ****/

int wc_utf8_len(const unsigned char *s, unsigned n) {
    unsigned char c = s[0];
    int len, k;
    if(c < 0x80 || n < 2 || c < 0xC2 || c > 0xF4) return 1;
    len = c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    if((unsigned)len > n) return 1;
    for(k = 1; k < len; k++)
        if((s[k] & 0xC0) != 0x80) return 1;
    if((c == 0xE0 && s[1] < 0xA0) || (c == 0xED && s[1] >= 0xA0) ||     /* too long a form; a surrogate */
       (c == 0xF0 && s[1] < 0x90) || (c == 0xF4 && s[1] >= 0x90)) return 1;   /* too long; past U+10FFFF */
    return len;
}

unsigned wc_utf8_decode(const unsigned char *s, unsigned n, unsigned *cp) {
    int len = wc_utf8_len(s, n);
    switch(len) {
    case 1: *cp = s[0] < 0x80 ? s[0] : WC_BAD | s[0]; break;
    case 2: *cp = (unsigned)(s[0] & 0x1F) << 6 | (s[1] & 0x3F); break;
    case 3: *cp = (unsigned)(s[0] & 0x0F) << 12 | (unsigned)(s[1] & 0x3F) << 6 | (s[2] & 0x3F); break;
    default: *cp = (unsigned)(s[0] & 0x07) << 18 | (unsigned)(s[1] & 0x3F) << 12 | (unsigned)(s[2] & 0x3F) << 6 |
                   (s[3] & 0x3F); break;
    }
    return (unsigned)len;
}

int wc_utf8_put(unsigned cp, char *o) {
    if(cp < 0x80) { o[0] = (char)cp; return 1; }
    if(cp < 0x800) { o[0] = (char)(0xC0 | cp >> 6); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if(cp >= 0xD800 && cp < 0xE000) return 0;
    if(cp < 0x10000) {
        o[0] = (char)(0xE0 | cp >> 12);
        o[1] = (char)(0x80 | (cp >> 6 & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if(cp < 0x110000) {
        o[0] = (char)(0xF0 | cp >> 18);
        o[1] = (char)(0x80 | (cp >> 12 & 0x3F));
        o[2] = (char)(0x80 | (cp >> 6 & 0x3F));
        o[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

/**** the buffer ****/

static int pool_take(struct wc_pool *p, unsigned long more) {
    if(!p) return 0;
    if(p->used + more > p->limit) return -1;
    p->used += more;
    return 0;
}

static void pool_give(struct wc_pool *p, unsigned long less) {
    if(p) p->used -= less;
}

static unsigned count_in(const char *p, unsigned n) {
    const char *e, *q;
    unsigned k = 0;
    if(!n) return 0;
    for(e = p + n; p < e && (q = memchr(p, '\n', (size_t)(e - p))); p = q + 1) k++;
    return k;
}

int wc_text_init(struct wc_text *t, struct wc_pool *pool) {
    memset(t, 0, sizeof(*t));
    t->pool = pool;
    t->m = calloc(16, sizeof(struct wc_mark));
    if(!t->m) return -1;
    t->mcap = 16;
    t->nm = 1;
    t->m[0].ok = 1;
    t->dirty_lo = 1;                            /* nothing changed */
    return 0;
}

void wc_text_free(struct wc_text *t) {
    free(t->b);
    pool_give(t->pool, t->cap);
    free(t->m);
    memset(t, 0, sizeof(*t));
}

/* the gap at pos: what lies between moves across it */
static void move_gap(struct wc_text *t, unsigned pos) {
    unsigned n;
    if(pos < t->gs) {
        n = t->gs - pos;
        memmove(t->b + t->ge - n, t->b + pos, n);
        t->gs = pos;
        t->ge -= n;
    } else if(pos > t->gs) {
        n = pos - t->gs;
        memmove(t->b + t->gs, t->b + t->ge, n);
        t->gs = pos;
        t->ge += n;
    }
}

/* a gap of n bytes at least: a larger block, with room to spare (a sixteenth of the text, or WC_GAP), or just enough
   when the pool has no more */
static int room(struct wc_text *t, unsigned n) {
    unsigned long want, extra, cap;
    unsigned tail;
    char *nb;
    if((unsigned long)t->len + n > WC_TEXT_MAX) {      /* the limit is the text's, whatever room the gap has */
        errno = EFBIG;
        return -1;
    }
    if(t->ge - t->gs >= n) return 0;
    want = (unsigned long)t->len + n;
    extra = want / 16 > WC_GAP ? want / 16 : WC_GAP;
    cap = want + extra;
    if(pool_take(t->pool, cap - t->cap) < 0) {
        cap = want;
        if(pool_take(t->pool, cap - t->cap) < 0) {
            errno = ENOMEM;
            return -1;
        }
    }
    nb = realloc(t->b, cap);
    if(!nb) {
        pool_give(t->pool, cap - t->cap);
        errno = ENOMEM;
        return -1;
    }
    tail = t->cap - t->ge;
    if(tail) memmove(nb + cap - tail, nb + t->ge, tail);
    t->b = nb;
    t->ge = (unsigned)cap - tail;
    t->cap = (unsigned)cap;
    return 0;
}

/* after a large deletion the memory goes back: the gap to the end, then a smaller block */
static void shrink(struct wc_text *t) {
    unsigned cap = t->len + WC_GAP;
    char *nb;
    if(t->ge - t->gs <= (4u << 20) || t->ge - t->gs <= t->len) return;
    move_gap(t, t->len);
    nb = realloc(t->b, cap);
    if(!nb) return;                             /* it keeps the larger block */
    pool_give(t->pool, t->cap - cap);
    t->b = nb;
    t->cap = cap;
    t->ge = cap;
}

char *wc_text_load_begin(struct wc_text *t, unsigned n) {
    unsigned long extra = n / 16 > WC_GAP ? n / 16 : WC_GAP, cap = n + extra;
    if(n > WC_TEXT_MAX) {
        errno = EFBIG;
        return NULL;
    }
    free(t->b);
    pool_give(t->pool, t->cap);
    t->b = NULL;
    t->gs = t->ge = t->cap = t->len = t->nl = 0;
    if(pool_take(t->pool, cap) < 0) {
        errno = ENOMEM;
        return NULL;
    }
    t->b = malloc(cap);
    if(!t->b) {
        pool_give(t->pool, cap);
        errno = ENOMEM;
        return NULL;
    }
    t->cap = (unsigned)cap;
    t->ge = t->cap;
    return t->b;
}

void wc_text_load_end(struct wc_text *t, unsigned n) {
    if(n > t->cap) n = t->cap;
    t->gs = t->len = n;
    t->ge = t->cap;
    t->nl = count_in(t->b, n);
    t->nm = 1;                                  /* the index starts again */
    t->m[0].ok = 1;
    t->dirty_lo = 0;
    t->dirty_hi = n;
    t->edits++;
}

/* the first mark after position p (m[0] is at 0) */
static unsigned mark_after(const struct wc_text *t, unsigned p) {
    unsigned lo = 1, hi = t->nm, mid;
    while(lo < hi) {
        mid = lo + (hi - lo) / 2;
        if(t->m[mid].pos > p) hi = mid;
        else lo = mid + 1;
    }
    return lo;
}

/* what changed, for the colouring: [dirty_lo, dirty_hi] in the positions of the text as it is now */
static void dirty(struct wc_text *t, unsigned pos, unsigned n, int inserted) {
    if(t->dirty_lo > t->dirty_hi) {
        t->dirty_lo = pos;
        t->dirty_hi = inserted ? pos + n : pos;
    } else if(inserted) {
        t->dirty_hi = t->dirty_hi >= pos ? t->dirty_hi + n : pos + n;
        if(pos < t->dirty_lo) t->dirty_lo = pos;
    } else {
        if(t->dirty_hi > pos + n) t->dirty_hi -= n;
        else if(t->dirty_hi > pos) t->dirty_hi = pos;
        if(t->dirty_hi < pos) t->dirty_hi = pos;
        if(pos < t->dirty_lo) t->dirty_lo = pos;
    }
    t->edits++;
}

int wc_text_insert(struct wc_text *t, unsigned pos, const char *s, unsigned n) {
    unsigned k, i;
    if(!n) return 0;
    if(pos > t->len) pos = t->len;
    if(room(t, n) < 0) return -1;
    move_gap(t, pos);
    memcpy(t->b + t->gs, s, n);
    t->gs += n;
    t->len += n;
    k = count_in(s, n);
    t->nl += k;
    for(i = mark_after(t, pos); i < t->nm; i++) {      /* a mark at pos stays: its line starts there still */
        t->m[i].pos += n;
        t->m[i].line += k;
    }
    dirty(t, pos, n, 1);
    return 0;
}

void wc_text_delete(struct wc_text *t, unsigned pos, unsigned n) {
    unsigned k, i, j;
    if(pos >= t->len || !n) return;
    if(n > t->len - pos) n = t->len - pos;
    move_gap(t, pos);
    k = count_in(t->b + t->ge, n);
    t->ge += n;
    t->len -= n;
    t->nl -= k;
    i = mark_after(t, pos);                     /* marks in (pos, pos + n] were at lines that are gone or joined */
    j = mark_after(t, pos + n);
    if(j > i) {
        memmove(t->m + i, t->m + j, (t->nm - j) * sizeof(struct wc_mark));
        t->nm -= j - i;
    }
    for(; i < t->nm; i++) {
        t->m[i].pos -= n;
        t->m[i].line -= k;
    }
    dirty(t, pos, n, 0);
    shrink(t);
}

/**** reading ****/

unsigned char wc_text_byte(const struct wc_text *t, unsigned pos) {
    return (unsigned char)t->b[PHYS(t, pos)];
}

void wc_text_get(const struct wc_text *t, unsigned pos, char *out, unsigned n) {
    unsigned a;
    if(!n) return;
    if(pos < t->gs) {
        a = t->gs - pos < n ? t->gs - pos : n;
        memcpy(out, t->b + pos, a);
        out += a;
        pos += a;
        n -= a;
    }
    if(n) memcpy(out, t->b + pos + (t->ge - t->gs), n);
}

const char *wc_text_span(struct wc_text *t, unsigned pos, unsigned n) {
    if(!n || !t->b) return "";
    if(pos < t->gs && pos + n > t->gs) move_gap(t, t->gs - pos < pos + n - t->gs ? pos : pos + n);
    return t->b + PHYS(t, pos);
}

void wc_text_parts(const struct wc_text *t, const char **a, unsigned *na, const char **b, unsigned *nb) {
    *a = t->b ? t->b : "";
    *na = t->gs;
    *b = t->b ? t->b + t->ge : "";
    *nb = t->cap - t->ge;
}

unsigned wc_text_find_nl(const struct wc_text *t, unsigned from, unsigned to) {
    const char *p;
    unsigned e;
    if(to > t->len) to = t->len;
    if(from >= to) return to;
    if(from < t->gs) {
        e = to < t->gs ? to : t->gs;
        p = memchr(t->b + from, '\n', e - from);
        if(p) return (unsigned)(p - t->b);
        from = e;
    }
    if(from < to) {
        p = memchr(t->b + from + (t->ge - t->gs), '\n', to - from);
        if(p) return (unsigned)(p - t->b) - (t->ge - t->gs);
    }
    return to;
}

unsigned wc_text_count_nl(const struct wc_text *t, unsigned from, unsigned to) {
    unsigned k = 0, e;
    if(to > t->len) to = t->len;
    if(from >= to) return 0;
    if(from < t->gs) {
        e = to < t->gs ? to : t->gs;
        k = count_in(t->b + from, e - from);
        from = e;
    }
    if(from < to) k += count_in(t->b + from + (t->ge - t->gs), to - from);
    return k;
}

unsigned wc_text_line_begin(const struct wc_text *t, unsigned pos) {
    const char *b = t->b;
    unsigned off = t->ge - t->gs;
    if(pos > t->len) pos = t->len;
    while(pos > t->gs)                          /* after the gap */
        if(b[--pos + off] == '\n') return pos + 1;
    while(pos > 0)
        if(b[--pos] == '\n') return pos + 1;
    return 0;
}

void wc_text_line(const struct wc_text *t, unsigned start, struct wc_line *l) {
    unsigned q = wc_text_find_nl(t, start, t->len);
    l->start = start;
    l->end = q;
    l->eol = 0;
    if(q >= t->len) return;
    l->eol = 1;
    if(q > start && wc_text_byte(t, q - 1) == '\r') {
        l->end = q - 1;
        l->eol = 2;
    }
}

/**** the index ****/

unsigned wc_text_lines(const struct wc_text *t) {
    return t->nl + 1;
}

unsigned wc_text_mark_before(const struct wc_text *t, unsigned line) {
    unsigned lo = 1, hi = t->nm, mid;
    while(lo < hi) {
        mid = lo + (hi - lo) / 2;
        if(t->m[mid].line > line) hi = mid;
        else lo = mid + 1;
    }
    return lo - 1;
}

unsigned wc_text_mark_of(const struct wc_text *t, unsigned pos) {
    return mark_after(t, pos) - 1;
}

int wc_text_mark_add(struct wc_text *t, unsigned at, unsigned pos, unsigned line) {
    struct wc_mark *m;
    if(t->nm == t->mcap) {
        m = realloc(t->m, (size_t)t->mcap * 2 * sizeof(struct wc_mark));
        if(!m) return -1;
        t->m = m;
        t->mcap *= 2;
    }
    memmove(t->m + at + 1, t->m + at, (t->nm - at) * sizeof(struct wc_mark));
    t->nm++;
    memset(&t->m[at], 0, sizeof(t->m[at]));
    t->m[at].pos = pos;
    t->m[at].line = line;
    return 0;
}

/* Replace all changes a line after another: marks after the first would move at every change, so they go, and the
   lookups after it make them again */
void wc_text_drop_marks(struct wc_text *t, unsigned pos) {
    t->nm = mark_after(t, pos);
}

unsigned wc_text_line_start(struct wc_text *t, unsigned line) {
    unsigned i, pos, at, last, last_pos;
    if(line > t->nl) line = t->nl;
    i = wc_text_mark_before(t, line);
    pos = last_pos = t->m[i].pos;
    at = last = t->m[i].line;
    while(at < line) {
        pos = wc_text_find_nl(t, pos, t->len) + 1;
        at++;
        if(at - last >= WC_MARK_EVERY || pos - last_pos >= WC_MARK_BYTES) {   /* a mark, for the next lookup */
            if(wc_text_mark_add(t, i + 1, pos, at) == 0) i++;
            last = at;
            last_pos = pos;
        }
    }
    return pos;
}

unsigned wc_text_line_of(struct wc_text *t, unsigned pos) {
    unsigned i, p, at, last, last_pos, q;
    if(pos > t->len) pos = t->len;
    i = mark_after(t, pos) - 1;
    p = last_pos = t->m[i].pos;
    at = last = t->m[i].line;
    for(;;) {
        q = wc_text_find_nl(t, p, pos);
        if(q >= pos) break;
        p = q + 1;
        at++;
        if(at - last >= WC_MARK_EVERY || p - last_pos >= WC_MARK_BYTES) {
            if(wc_text_mark_add(t, i + 1, p, at) == 0) i++;
            last = at;
            last_pos = p;
        }
    }
    return at;
}

/**** characters and columns ****/

unsigned wc_text_char(const struct wc_text *t, unsigned pos, unsigned *cp) {
    unsigned char s[4];
    const unsigned char *p;
    unsigned n;
    if(pos >= t->len) {
        *cp = 0;
        return 0;
    }
    n = t->len - pos < 4 ? t->len - pos : 4;
    if(pos >= t->gs || pos + n <= t->gs) p = (const unsigned char *)t->b + PHYS(t, pos);
    else {
        wc_text_get(t, pos, (char *)s, n);
        p = s;
    }
    if(p[0] == '\r' && n > 1 && p[1] == '\n') {
        *cp = '\n';
        return 2;
    }
    return wc_utf8_decode(p, n, cp);
}

unsigned wc_text_next(const struct wc_text *t, unsigned pos) {
    unsigned cp;
    if(pos >= t->len) return t->len;
    return pos + wc_text_char(t, pos, &cp);
}

unsigned wc_text_prev(const struct wc_text *t, unsigned pos) {
    unsigned k, cp;
    if(pos == 0) return 0;
    if(pos > t->len) return t->len;
    if(pos >= 2 && wc_text_byte(t, pos - 1) == '\n' && wc_text_byte(t, pos - 2) == '\r') return pos - 2;
    k = pos - 1;
    while(k > 0 && pos - k < 4 && (wc_text_byte(t, k) & 0xC0) == 0x80) k--;
    if(k < pos - 1 && k + wc_text_char(t, k, &cp) == pos) return k;
    return pos - 1;
}

unsigned wc_text_snap(const struct wc_text *t, unsigned pos) {
    unsigned k, cp;
    if(pos >= t->len) return t->len;
    if(pos == 0) return 0;
    if(wc_text_byte(t, pos) == '\n' && wc_text_byte(t, pos - 1) == '\r') return pos - 1;
    if((wc_text_byte(t, pos) & 0xC0) != 0x80) return pos;
    for(k = pos; k > 0 && pos - k < 3;) {       /* the lead byte of the sequence pos may be inside */
        k--;
        if((wc_text_byte(t, k) & 0xC0) != 0x80) return k + wc_text_char(t, k, &cp) > pos ? k : pos;
    }
    return pos;
}

unsigned wc_text_col(const struct wc_text *t, unsigned start, unsigned pos, unsigned tabw) {
    const unsigned char *p;
    unsigned col = 0, avail, cp;
    if(pos > t->len) pos = t->len;
    if(!tabw) tabw = 1;
    while(start < pos) {
        if(start < t->gs) {
            p = (const unsigned char *)t->b + start;
            avail = (pos < t->gs ? pos : t->gs) - start;
        } else {
            p = (const unsigned char *)t->b + start + (t->ge - t->gs);
            avail = pos - start;
        }
        while(avail && *p < 0x80 && *p != '\t') {     /* a run of ASCII: a cell a byte */
            col++;
            p++;
            avail--;
            start++;
        }
        if(!avail) continue;
        if(*p == '\t') {
            col = (col / tabw + 1) * tabw;
            start++;
            continue;
        }
        start += wc_text_char(t, start, &cp);
        col++;
    }
    return col;
}

/* the character boundary of line l nearest to the boundary before cell col */
unsigned wc_text_at_col(const struct wc_text *t, const struct wc_line *l, unsigned col, unsigned tabw) {
    unsigned pos = l->start, c = 0, next, cp, n;
    if(!tabw) tabw = 1;
    while(pos < l->end) {
        n = wc_text_char(t, pos, &cp);
        next = cp == '\t' ? (c / tabw + 1) * tabw : c + 1;
        if(col < next) return col - c <= next - col ? pos : pos + n;
        c = next;
        pos += n;
    }
    return l->end;
}
