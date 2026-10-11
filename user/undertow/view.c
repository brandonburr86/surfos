/*
SurfOS Undertow: the screen: wrapped text and one screen of a page at a time
----------------------
File: view.c  Date: 10/11/26
----------------------
view.h describes the screen. Text is cut into rows by one greedy pass per line
(struct rows): a row takes characters while they fit, and ends at the last
space that fits, or inside a word that is longer than the row; the spaces at a
break belong to the row before, so every byte of the text is in exactly one
row, and the row that holds a reading position is well defined. Rows are
worked out again for every view, from the start of the line that holds the
position (and from the top for the line numbers): a page has at most
UT_TEXT_MAX bytes, and the terminal may have changed its size since.
*/

#include <stdio.h>
#include <string.h>
#include "undertow.h"
#include "view.h"

/* the SurfOS palette as ANSI SGR (the subset include/surfos/tty.h lists) */
#define C_RESET   "\033[0m"
#define C_BRAND   "\033[0;1;34m"        /* light blue: "Undertow", as the SurfOS prompt is */
#define C_TITLE   "\033[0;1;37m"        /* bright white: the page's title, its headings */
#define C_LINK    "\033[0;1;34m"        /* light blue: the link markers [N] */
#define C_TYPED   "\033[0;36m"          /* teal: what to type, as surfsh shows typed text */
#define C_SECURE  "\033[0;32m"          /* green: HTTPS verified */
#define C_PLAIN   "\033[0;33m"          /* orange: HTTP, unencrypted */
#define C_ERROR   "\033[0;1;31m"        /* light red: an HTTP status that is not 200 */
#define C_RULE    "\033[0;34m"          /* blue: the rules */

#define RULE_U     "\xe2\x94\x80"       /* U+2500, code page 437's 0xC4 */
#define ELLIPSIS_U "\xe2\x80\xa6"       /* U+2026, one of SurfOS's own glyphs */

#define COLS_MAX   1024u                /* PTY_SETSIZE's limits */
#define ROWS_MAX   1024u
#define PREFIX_COLS 11u                 /* "Undertow - " */
#define ROW_SPANS  64                   /* spans that colour one row; more take the slow way */

/* the terminal with its size kept in bounds */
static struct ut_term fixed(const struct ut_term *t) {
    struct ut_term f = *t;
    if(f.cols < 1) f.cols = 1;
    if(f.cols > COLS_MAX) f.cols = COLS_MAX;
    if(f.rows < 1) f.rows = 1;
    if(f.rows > ROWS_MAX) f.rows = ROWS_MAX;
    return f;
}

unsigned int ut_view_text_rows(const struct ut_term *t0) {
    struct ut_term t = fixed(t0);
    if(t.rows >= 8) return t.rows - 6;
    return t.rows > 4 ? t.rows - 3 : 1;     /* the compact view: the title line, the rows, the position */
}

/**** characters as the terminal shows them ****/

struct ch {
    size_t len;                 /* bytes */
    unsigned int cp;            /* what is shown: ' ' for a space, 0 for nothing */
    unsigned int cols;          /* columns */
    int space;                  /* a space (a break may come here) */
};

/* the character at s (n > 0 bytes): what ut_safe_text() would make of it, and its width */
static void get_ch(const struct ut_term *t, const unsigned char *s, size_t n, struct ch *c) {
    unsigned int cp;
    if(s[0] < 0x80) {
        c->len = 1;
        cp = s[0];
    } else {
        c->len = ut_utf8_decode(s, n, &cp);
    }
    if(cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\v' || cp == '\f' ||
       cp == 0x85 || cp == 0x2028 || cp == 0x2029) {
        c->cp = ' ';
        c->cols = 1;
        c->space = 1;
        return;
    }
    c->space = 0;
    if(ut_unsafe_char(cp)) {
        c->cp = 0;
        c->cols = 0;
        return;
    }
    c->cp = cp;
    c->cols = t->utf8 || cp < 0x80 ? 1 : (unsigned int)strlen(ut_ascii_for(cp));
}

static void put_ch(FILE *out, const struct ut_term *t, const struct ch *c) {
    unsigned char b[4];
    if(!c->cp) return;
    if(c->cp < 0x80) putc((int)c->cp, out);
    else if(t->utf8) fwrite(b, 1, ut_utf8_encode(c->cp, b), out);
    else fputs(ut_ascii_for(c->cp), out);
}

/* the columns of a one-line text */
static unsigned int text_cols(const struct ut_term *t, const char *s, size_t len) {
    const unsigned char *u = (const unsigned char *)s;
    unsigned int cols = 0;
    struct ch c;
    size_t i;
    for(i = 0; i < len; i += c.len) {
        get_ch(t, u + i, len - i, &c);
        cols += c.cols;
    }
    return cols;
}

/* A one-line text (a title, an address, a message: anything a server may have sent) in at most
   cols columns: through ut_safe_text() first, then cut short with an ellipsis when it is wider.
   Returns the columns written. More than UT_ERR_MAX bytes never fit on a row: they are cut. */
static char safe_line[UT_ERR_MAX * 4 + 1];   /* the safe copy: an ASCII stand-in has up to 4 bytes */

static unsigned int put_cut(FILE *out, const struct ut_term *t, const char *s, size_t len, unsigned int cols) {
    unsigned int used = 0, ell = t->utf8 ? 1 : 3, room, total = 0;
    size_t n, i, end;
    int longer = len > UT_ERR_MAX;
    n = ut_safe_text(safe_line, sizeof(safe_line), s, longer ? UT_ERR_MAX : len, t->utf8);
    for(i = 0; i < n; i++) if(((unsigned char)safe_line[i] & 0xc0) != 0x80) total++;
    room = !longer && total <= cols ? cols : cols > ell ? cols - ell : 0;
    for(end = 0; end < n; end++) {
        if(((unsigned char)safe_line[end] & 0xc0) == 0x80) continue;
        if(used == room) break;
        used++;
    }
    fwrite(safe_line, 1, end, out);
    if((end < n || longer) && cols >= ell) {
        fputs(t->utf8 ? ELLIPSIS_U : "...", out);
        used += ell;
    }
    return used;
}

static void sgr(FILE *out, const struct ut_term *t, const char *seq) {
    if(t->color) fputs(seq, out);
}

/* the end of a row: \r\n when it filled the width (the console wraps the VT100 way: a full
   row's \n alone would leave an empty row), \n otherwise */
static void end_row(FILE *out, const struct ut_term *t, unsigned int width) {
    fputs(width >= t->cols ? "\r\n" : "\n", out);
}

static void spaces(FILE *out, unsigned int n) {
    while(n--) putc(' ', out);
}

/**** rows ****/

struct rows {
    const struct ut_term *t;
    const unsigned char *s;
    size_t len;
    size_t line, eol;           /* the line: [line, eol), eol its '\n' or len */
    unsigned int hang;          /* the indent of its continuation rows */
    size_t start, end, next;    /* the row shows [start, end); the next row starts at next (len: none) */
    int cont;                   /* the row continues its line: hang spaces come first */
};

/* a line's leading spaces and list marker ("* ", "12. "), when 20 columns are left after them */
static unsigned int hang_of(const struct rows *r) {
    const unsigned char *s = r->s;
    size_t i = r->line, lead, mark = 0, d;
    while(i < r->eol && s[i] == ' ') i++;
    lead = i - r->line;
    if(i + 1 < r->eol && s[i] == '*' && s[i + 1] == ' ') {
        mark = 2;
    } else {
        for(d = i; d < r->eol && d - i < 9 && s[d] >= '0' && s[d] <= '9'; d++);
        if(d > i && d + 1 < r->eol && s[d] == '.' && s[d + 1] == ' ') mark = d - i + 2;
    }
    if(lead + mark + 20 > r->t->cols) return 0;
    return (unsigned int)(lead + mark);
}

/* past the spaces from i; past the line when only spaces are left */
static size_t skip_spaces(const struct rows *r, size_t i) {
    while(i < r->eol && r->s[i] == ' ') i++;
    if(i == r->eol) return r->eol < r->len ? r->eol + 1 : r->len;
    return i;
}

static size_t trim(const struct rows *r, size_t end) {
    while(end > r->start && r->s[end - 1] == ' ') end--;
    return end;
}

/* the row from r->start: where it ends and where the next one starts */
static void fill(struct rows *r) {
    unsigned int avail = r->t->cols - (r->cont ? r->hang : 0), col = 0;
    size_t i = r->start, brk = 0;
    int have_brk = 0, seen = 0;
    struct ch c;
    while(i < r->eol) {
        get_ch(r->t, r->s + i, r->eol - i, &c);
        if(c.space && seen) {
            brk = i;
            have_brk = 1;
        }
        if(col + c.cols > avail) {
            if(have_brk) {                          /* at the last space that fits */
                r->end = trim(r, brk);
                r->next = skip_spaces(r, brk);
            } else if(i > r->start) {               /* inside a word longer than the row */
                r->end = i;
                r->next = skip_spaces(r, i);
            } else {                                /* not one character fits: it goes all the same */
                r->end = i + c.len;
                r->next = skip_spaces(r, i + c.len);
            }
            return;
        }
        col += c.cols;
        if(!c.space) seen = 1;
        i += c.len;
    }
    r->end = trim(r, r->eol);                       /* the rest of the line fits */
    r->next = r->eol < r->len ? r->eol + 1 : r->len;
}

static void rows_line(struct rows *r, size_t line) {
    const unsigned char *nl = memchr(r->s + line, '\n', r->len - line);
    r->line = line;
    r->eol = nl ? (size_t)(nl - r->s) : r->len;
    r->hang = hang_of(r);
    r->start = line;
    r->cont = 0;
    fill(r);
}

/* the next row: 0 at the end of the text */
static int rows_next(struct rows *r) {
    if(r->next >= r->len) return 0;
    if(r->next > r->eol) {
        rows_line(r, r->next);
    } else {
        r->start = r->next;
        r->cont = 1;
        fill(r);
    }
    return 1;
}

/* the row that holds offset (offsets past the text: its last row); 0 when the text has none */
static int rows_at(struct rows *r, const struct ut_term *t, const char *text, size_t len, size_t offset) {
    size_t line;
    r->t = t;
    r->s = (const unsigned char *)text;
    r->len = len;
    if(!len || !text) return 0;
    if(offset >= len) offset = len - 1;
    for(line = offset; line > 0 && text[line - 1] != '\n'; line--);
    rows_line(r, line);
    while(r->next <= offset && rows_next(r));
    return 1;
}

/* the index of the row that holds offset, and how many rows there are */
static void count_rows(const struct ut_term *t, const char *text, size_t len, size_t offset,
                       unsigned long *index, unsigned long *total) {
    struct rows r;
    unsigned long n = 0, at = 0;
    int found = 0;
    if(rows_at(&r, t, text, len, 0)) {
        do {
            if(!found && r.next > offset) {
                at = n;
                found = 1;
            }
            n++;
        } while(rows_next(&r));
        if(!found) at = n - 1;
    }
    *index = at;
    *total = n;
}

size_t ut_view_row_start(const struct ut_term *t0, const char *text, size_t len, size_t offset) {
    struct ut_term t = fixed(t0);
    struct rows r;
    return rows_at(&r, &t, text, len, offset) ? r.start : 0;
}

size_t ut_view_next(const struct ut_term *t0, const char *text, size_t len, size_t from) {
    struct ut_term t = fixed(t0);
    unsigned int n = ut_view_text_rows(&t);
    struct rows r;
    if(!rows_at(&r, &t, text, len, from)) return len;
    while(n--) if(!rows_next(&r)) return len;
    return r.start;
}

size_t ut_view_prev(const struct ut_term *t0, const char *text, size_t len, size_t from) {
    struct ut_term t = fixed(t0);
    unsigned long index, total, n = ut_view_text_rows(&t);
    struct rows r;
    count_rows(&t, text, len, from, &index, &total);
    if(index <= n || !rows_at(&r, &t, text, len, 0)) return 0;
    for(index -= n; index; index--) rows_next(&r);
    return r.start;
}

/**** a row on the terminal, with the colours of the spans ****/

struct near {
    size_t from, to;
    int rank;
};

/* which colour wins where spans overlap: a link marker, then what to type, then a heading */
static int rank_of(int kind) {
    return kind == UT_SPAN_LINK ? 3 : kind == UT_SPAN_TYPED ? 2 : kind == UT_SPAN_HEADING ? 1 : 0;
}

static const char *colour_of(int rank) {
    return rank == 3 ? C_LINK : rank == 2 ? C_TYPED : rank == 1 ? C_TITLE : C_RESET;
}

static int rank_at(const struct near *near, int n, size_t at) {
    int i, best = 0;
    for(i = 0; i < n; i++) if(at >= near[i].from && at < near[i].to && near[i].rank > best) best = near[i].rank;
    return best;
}

/* the slow way, for a row with more than ROW_SPANS spans on it */
static int rank_all(const struct ut_span *spans, size_t nspans, size_t at) {
    size_t i;
    int best = 0;
    for(i = 0; i < nspans; i++)
        if(at >= spans[i].offset && at - spans[i].offset < spans[i].len && rank_of(spans[i].kind) > best)
            best = rank_of(spans[i].kind);
    return best;
}

static void put_row(FILE *out, const struct rows *r, const struct ut_span *spans, size_t nspans) {
    const struct ut_term *t = r->t;
    struct near near[ROW_SPANS];
    unsigned int width = 0;
    int n = 0, many = 0, now = 0;
    struct ch c;
    size_t i;
    if(r->cont) {
        spaces(out, r->hang);
        width = r->hang;
    }
    if(t->color && spans) {
        for(i = 0; i < nspans; i++) {
            size_t a = spans[i].offset, b = a + spans[i].len;
            if(b < a) b = (size_t)-1;
            if(b <= r->start || a >= r->end || !rank_of(spans[i].kind)) continue;
            if(n == ROW_SPANS) {
                many = 1;
                break;
            }
            near[n].from = a;
            near[n].to = b;
            near[n].rank = rank_of(spans[i].kind);
            n++;
        }
    }
    for(i = r->start; i < r->end; i += c.len) {
        int want = 0;
        get_ch(t, r->s + i, r->end - i, &c);
        if(n) want = many ? rank_all(spans, nspans, i) : rank_at(near, n, i);
        if(want != now && c.cols) {
            sgr(out, t, colour_of(want));
            now = want;
        }
        put_ch(out, t, &c);
        width += c.cols;
    }
    if(now) sgr(out, t, C_RESET);
    end_row(out, t, width);
}

static void put_rows(FILE *out, const struct ut_term *t, const char *text, size_t len,
                     const struct ut_span *spans, size_t nspans) {
    struct rows r;
    if(!rows_at(&r, t, text, len, 0)) return;
    do put_row(out, &r, spans, nspans);
    while(rows_next(&r));
}

void ut_view_text(FILE *out, const struct ut_term *t0, const char *text, size_t len,
                  const struct ut_span *spans, size_t nspans) {
    struct ut_term t = fixed(t0);
    put_rows(out, &t, text, len, spans, nspans);
}

/**** the chrome ****/

static void rule(FILE *out, const struct ut_term *t) {
    unsigned int i;
    sgr(out, t, C_RULE);
    for(i = 0; i < t->cols; i++) fputs(t->utf8 ? RULE_U : "-", out);
    sgr(out, t, C_RESET);
    end_row(out, t, t->cols);
}

/* the parts of the state at the right of the title line, from the longest form to the shortest */
struct state {
    char code[4][96];           /* [HTTP 404 Not Found], [HTTP 404], [404]; "" for 200 */
    char via[4][48];            /* [HTTPS verified TLSv1.3], [HTTPS verified], [HTTPS] */
    const char *via_colour;
};

static void make_state(const struct ut_page *p, struct state *st, int utf8) {
    char reason[64];
    memset(st, 0, sizeof(*st));
    if(p->status != 200) {
        ut_safe_text(reason, sizeof(reason), p->reason, strnlen(p->reason, sizeof(p->reason)), utf8);
        if(reason[0]) snprintf(st->code[0], sizeof(st->code[0]), "[HTTP %d %s]", p->status, reason);
        else snprintf(st->code[0], sizeof(st->code[0]), "[HTTP %d]", p->status);
        snprintf(st->code[1], sizeof(st->code[1]), "[HTTP %d]", p->status);
        snprintf(st->code[2], sizeof(st->code[2]), "[%d]", p->status);
    }
    if(p->url.secure && p->verified) {
        char tls[16];
        ut_safe_text(tls, sizeof(tls), p->tls, strnlen(p->tls, sizeof(p->tls)), 0);
        if(tls[0]) snprintf(st->via[0], sizeof(st->via[0]), "[HTTPS verified %s]", tls);
        else strcpy(st->via[0], "[HTTPS verified]");
        strcpy(st->via[1], "[HTTPS verified]");
        strcpy(st->via[2], "[HTTPS]");
        st->via_colour = C_SECURE;
    } else if(p->url.secure) {                      /* never claimed: transport.h has no such page */
        strcpy(st->via[0], "[HTTPS not verified]");
        strcpy(st->via[1], "[HTTPS not verified]");
        strcpy(st->via[2], "[HTTPS?]");
        st->via_colour = C_ERROR;
    } else {
        strcpy(st->via[0], "[HTTP unencrypted]");
        strcpy(st->via[1], "[HTTP unencrypted]");
        strcpy(st->via[2], "[HTTP]");
        st->via_colour = C_PLAIN;
    }
}

/* The title line: the title at the left, the state at the right edge. What gives way on a narrow
   terminal, in this order: the "Undertow - " prefix, the TLS version, the reason phrase (each
   while the title keeps 20 columns, or its whole width when it is shorter), then the title
   down to 8 columns, then the short forms ([404] [HTTPS]). */
static void title_line(FILE *out, const struct ut_term *t, const struct ut_page *p) {
    static const struct { int prefix, code, via, need; } ladder[] = {
        { 1, 0, 0, 20 }, { 0, 0, 0, 20 }, { 0, 0, 1, 20 }, { 0, 1, 1, 20 }, { 0, 1, 1, 8 }, { 0, 2, 2, 0 }
    };
    struct state st;
    const char *title = p->doc.title && p->doc.title[0] ? p->doc.title : p->url.host;
    size_t tlen = strlen(title);
    unsigned int twidth = text_cols(t, title, tlen), right, room = 0, used, k;
    int step, prefix = 0, code = 0, via = 0;
    make_state(p, &st, t->utf8);
    for(step = 0; step < 6; step++) {
        unsigned int need = twidth < (unsigned int)ladder[step].need ? twidth : (unsigned int)ladder[step].need;
        code = ladder[step].code;
        via = ladder[step].via;
        prefix = ladder[step].prefix;
        right = text_cols(t, st.code[code], strlen(st.code[code])) + (st.code[code][0] ? 1 : 0) +
                text_cols(t, st.via[via], strlen(st.via[via]));
        if(right + 1 > t->cols) continue;
        room = t->cols - right - 1;
        if(room >= need + (prefix ? PREFIX_COLS : 0)) break;
    }
    if(step == 6) {                                 /* not even [404] [HTTPS] fits: the state alone, cut */
        used = 0;
        if(st.code[2][0]) {
            sgr(out, t, C_ERROR);
            used = put_cut(out, t, st.code[2], strlen(st.code[2]), t->cols);
            sgr(out, t, C_RESET);
            if(used < t->cols) {
                putc(' ', out);
                used++;
            }
        }
        if(used < t->cols) {
            sgr(out, t, st.via_colour);
            used += put_cut(out, t, st.via[2], strlen(st.via[2]), t->cols - used);
            sgr(out, t, C_RESET);
        }
        end_row(out, t, used);
        return;
    }
    used = 0;
    if(prefix) {
        sgr(out, t, C_BRAND);
        fputs("Undertow", out);
        sgr(out, t, C_RESET);
        fputs(" - ", out);
        used = PREFIX_COLS;
    }
    sgr(out, t, C_TITLE);
    used += put_cut(out, t, title, tlen, room - used);
    sgr(out, t, C_RESET);
    spaces(out, t->cols - right - used);
    if(st.code[code][0]) {
        sgr(out, t, C_ERROR);
        fputs(st.code[code], out);
        sgr(out, t, C_RESET);
        putc(' ', out);
    }
    sgr(out, t, st.via_colour);
    k = (unsigned int)strlen(st.via[via]);
    fwrite(st.via[via], 1, k, out);
    sgr(out, t, C_RESET);
    end_row(out, t, t->cols);
}

/* a hint on the position line: its long and short forms; they go on the line in the order of
   the table, and when room is short the one of the lowest rank gets it first */
struct hint {
    const char *full, *brief;
    int rank;
    int on;
};

static void position_line(FILE *out, const struct ut_term *t, const struct ut_page *p, unsigned long index,
                          unsigned int shown, unsigned long total, int at_end, const char *message) {
    struct hint hints[4] = {
        { "Enter: next screen", "Enter: next", 0, 0 },
        { "top: back to the start", "top", 3, 0 },
        { "NUMBER: follow a link", "NUMBER: link", 2, 0 },
        { "help: all commands", "help", 1, 0 }
    };
    const char *forms[4] = { NULL, NULL, NULL, NULL };
    char pos[96];
    unsigned int used, rank, i;
    if(!total) strcpy(pos, "empty page");
    else snprintf(pos, sizeof(pos), "lines %lu-%lu of %lu (%lu%%)", index + 1, index + shown, total,
                  (index + shown) * 100 / total);
    used = put_cut(out, t, pos, strlen(pos), t->cols);
    if(!message || !message[0]) message = index == 0 && p->doc.notice[0] ? p->doc.notice : NULL;
    if(message) {
        if(used + 2 < t->cols) {
            fputs("  ", out);
            used += 2 + put_cut(out, t, message, strlen(message), t->cols - used - 2);
        }
        end_row(out, t, used);
        return;
    }
    if(at_end && total) {
        hints[0].full = "end of the page";
        hints[0].brief = "end";
        hints[1].on = index > 0;
    } else {
        hints[1].on = 0;
    }
    hints[0].on = total > 0;
    hints[2].on = p->doc.link_count > 0;
    hints[3].on = 1;
    for(rank = 0; rank < 4; rank++)                 /* the most useful first, the long form if it fits */
        for(i = 0; i < 4; i++) {
            unsigned int full, brief;
            if(hints[i].rank != (int)rank || !hints[i].on) continue;
            full = (unsigned int)strlen(hints[i].full);
            brief = (unsigned int)strlen(hints[i].brief);
            if(used + 2 + full <= t->cols) {
                forms[i] = hints[i].full;
                used += 2 + full;
            } else if(used + 2 + brief <= t->cols) {
                forms[i] = hints[i].brief;
                used += 2 + brief;
            }
        }
    for(i = 0; i < 4; i++) {
        if(!forms[i]) continue;
        fputs("  ", out);
        fputs(forms[i], out);
    }
    end_row(out, t, used);
}

size_t ut_view_page(FILE *out, const struct ut_term *t0, const struct ut_page *p, size_t from, const char *message) {
    struct ut_term t = fixed(t0);
    const struct ut_document *d = &p->doc;
    unsigned int n = ut_view_text_rows(&t), shown = 0, k;
    unsigned long index, total;
    size_t next = d->text_len;
    struct rows r;
    title_line(out, &t, p);
    if(t.rows >= 8) {
        end_row(out, &t, put_cut(out, &t, p->url.text, strnlen(p->url.text, sizeof(p->url.text)), t.cols));
        rule(out, &t);
    }
    if(rows_at(&r, &t, d->text, d->text_len, from)) {
        for(;;) {
            put_row(out, &r, d->spans, d->span_count);
            shown++;
            if(!rows_next(&r)) break;
            if(shown == n) {
                next = r.start;
                break;
            }
        }
    }
    for(k = shown; k < n; k++) end_row(out, &t, 0);
    if(t.rows >= 8) rule(out, &t);
    count_rows(&t, d->text, d->text_len, from, &index, &total);
    position_line(out, &t, p, index, shown, total, next == d->text_len, message);
    return next;
}

/**** the welcome screen ****/

struct build {
    char text[3072];
    size_t len;
    struct ut_span spans[16];
    size_t nspans;
};

static void add(struct build *b, const char *s, int kind) {
    size_t n = strlen(s);
    if(n > sizeof(b->text) - 1 - b->len) n = sizeof(b->text) - 1 - b->len;
    if(kind && b->nspans < sizeof(b->spans) / sizeof(b->spans[0])) {
        b->spans[b->nspans].offset = b->len;
        b->spans[b->nspans].len = n;
        b->spans[b->nspans].kind = kind;
        b->nspans++;
    }
    memcpy(b->text + b->len, s, n);
    b->len += n;
    b->text[b->len] = 0;
}

/* lines of the caller's: each made safe on its own, the line breaks kept */
static void add_lines(struct build *b, const char *s) {
    char line[600];
    const char *nl;
    if(!s) return;
    for(;;) {
        nl = strchr(s, '\n');
        ut_safe_text(line, sizeof(line), s, nl ? (size_t)(nl - s) : strlen(s), 1);
        add(b, line, 0);
        if(!nl) break;
        add(b, "\n", 0);
        s = nl + 1;
    }
}

void ut_view_welcome(FILE *out, const struct ut_term *t0, const char *clock, const char *https) {
    static struct build b;      /* 3 KB: not on the stack */
    struct ut_term t = fixed(t0);
    unsigned int n, shown = 0, used;
    int full = t.rows >= 8;
    struct rows r;
    b.len = b.nspans = 0;
    b.text[0] = 0;
    add(&b, "To read a page, type open and its address:\n  ", 0);
    add(&b, "open https://example.com/", UT_SPAN_TYPED);
    add(&b, "\n  ", 0);
    add(&b, "open example.com", UT_SPAN_TYPED);
    add(&b, "     (a name alone means HTTPS)\n\nOn a page, a link's number follows it, ", 0);
    add(&b, "Enter", UT_SPAN_TYPED);
    add(&b, " shows the next screen, ", 0);
    add(&b, "back", UT_SPAN_TYPED);
    add(&b, " goes to the page before and ", 0);
    add(&b, "find WORDS", UT_SPAN_TYPED);
    add(&b, " searches it. ", 0);
    add(&b, "help", UT_SPAN_TYPED);
    add(&b, " lists all the commands; ", 0);
    add(&b, "quit", UT_SPAN_TYPED);
    add(&b, " leaves.\n\n", 0);
    add_lines(&b, clock);
    add(&b, "\n", 0);
    add_lines(&b, https);

    n = full ? t.rows - 5 : ut_view_text_rows(&t);
    sgr(out, &t, C_BRAND);
    used = put_cut(out, &t, "Undertow " UT_VERSION, strlen("Undertow " UT_VERSION), t.cols);
    sgr(out, &t, C_RESET);
    if(used + 25 <= t.cols) {
        fputs(" - the SurfOS web browser", out);
        used += 25;
    }
    end_row(out, &t, used);
    if(full) rule(out, &t);
    if(rows_at(&r, &t, b.text, b.len, 0)) {
        do {
            put_row(out, &r, b.spans, b.nspans);
            shown++;
        } while(shown < n && rows_next(&r));
    }
    for(; shown < n; shown++) end_row(out, &t, 0);
    if(full) rule(out, &t);
    if(t.cols >= 40) {
        fputs("help: all commands  quit: leave", out);
        end_row(out, &t, 31);
    } else {
        end_row(out, &t, put_cut(out, &t, "help, quit", 10, t.cols));
    }
}

void ut_view_line(FILE *out, const struct ut_term *t0, const char *before, const char *s, const char *after) {
    struct ut_term t = fixed(t0);
    unsigned int used = text_cols(&t, before, strlen(before)), tail = text_cols(&t, after, strlen(after)), room;
    fputs(before, out);
    room = used + tail < t.cols ? t.cols - used - tail : 0;
    used += put_cut(out, &t, s, strlen(s), room);
    fputs(after, out);
    end_row(out, &t, used + tail);
}
