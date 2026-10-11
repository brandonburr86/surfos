/*
SurfOS Undertow: tests of the screen and of the line at the prompt
----------------------
File: test_undertow_view.c  Date: 10/11/26
----------------------
user/undertow/view.c (wrapping, page views, the welcome screen) and
user/undertow/edit.c (the undertow> prompt) with the test's own terminal:
views are written into memory (open_memstream) and read back row by row, the
editor reads keys from a script and its drawing is kept.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "surf.h"
#include "alloc.h"
#include "undertow.h"
#include "view.h"
#include "edit.h"

static int checks, failures;
static unsigned int sim_ms = 5000;

unsigned int uptime(void) { return sim_ms; }
time_t ut_sim_time(time_t *t) { if(t) *t = 0; return 0; }

/* the terminal: raw mode and what the editor asked of it */
static int raw_on, raw_works = 1, skips, raw_offs, raw_ons;
static char raw_at_draw;        /* 'r' when the prompt was drawn in raw mode, 'l' in line mode */

int ttyctl(int op, unsigned int arg) {
    if(op == TTY_RAW) {
        if(!raw_works) return -1;
        raw_on = (int)arg;
        if(arg) raw_ons++; else raw_offs++;
        return 0;
    }
    if(op == TTY_SKIP) {
        if(arg) skips++;
        return 0;
    }
    return -1;
}

static void check(int ok, const char *what) {
    checks++;
    if(!ok) { failures++; printf("FAIL: %s\n", what); }
}

/**** output read back: rows without their colours ****/

#define SHOT_ROWS 1200

struct shot {
    char *raw;                  /* the bytes as written */
    size_t rawlen;
    int n;                      /* rows: lines ended by a line break */
    char *row[SHOT_ROWS];       /* each without its line break or colours */
    int cols[SHOT_ROWS];        /* its display columns (code points) */
    int crlf[SHOT_ROWS];        /* it ended with \r\n */
    int sgr;                    /* colour sequences seen */
    int bad_esc;                /* an ESC that did not start ESC [ digits ; m */
    int high;                   /* a byte >= 0x80 */
    int bad_utf8;               /* a row that is not well-formed UTF-8 */
    int other_ctl;              /* a control byte other than ESC, \r before \n, \n */
};

static int utf8_ok(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    unsigned int cp;
    size_t len = strlen(s), i = 0, n;
    while(i < len) {
        n = ut_utf8_decode(p + i, len - i, &cp);
        if(cp == UT_REPLACEMENT && !(n == 3 && !memcmp(p + i, "\xef\xbf\xbd", 3))) return 0;
        i += n;
    }
    return 1;
}

static void shot_take(struct shot *s, char *buf, size_t len) {
    size_t i = 0, start;
    memset(s, 0, sizeof(*s));
    s->raw = buf;
    s->rawlen = len;
    while(i < len && s->n < SHOT_ROWS) {
        char *row = malloc(len - i + 1);
        size_t o = 0;
        int cols = 0;
        start = i;
        while(i < len && buf[i] != '\n') {
            unsigned char c = (unsigned char)buf[i];
            if(c == 0x1b) {
                size_t j = i + 1;
                if(j < len && buf[j] == '[') {
                    j++;
                    while(j < len && ((buf[j] >= '0' && buf[j] <= '9') || buf[j] == ';')) j++;
                    if(j < len && buf[j] == 'm') { s->sgr++; i = j + 1; continue; }
                }
                s->bad_esc++;
                i++;
                continue;
            }
            if(c == '\r' && i + 1 < len && buf[i + 1] == '\n') { s->crlf[s->n] = 1; i++; continue; }
            if(c < 0x20 || c == 0x7f) { s->other_ctl++; i++; continue; }
            if(c >= 0x80) s->high = 1;
            if((c & 0xc0) != 0x80) cols++;
            row[o++] = (char)c;
            i++;
        }
        row[o] = 0;
        if(i == len) {                  /* no line break after it: not a row (the prompt's line) */
            free(row);
            break;
        }
        i++;
        if(!utf8_ok(row)) s->bad_utf8++;
        s->row[s->n] = row;
        s->cols[s->n] = cols;
        s->n++;
        (void)start;
    }
}

static void shot_free(struct shot *s) {
    int i;
    for(i = 0; i < s->n; i++) free(s->row[i]);
    free(s->raw);
    memset(s, 0, sizeof(*s));
}

static int widest(const struct shot *s) {
    int i, w = 0;
    for(i = 0; i < s->n; i++) if(s->cols[i] > w) w = s->cols[i];
    return w;
}

/* \r\n exactly where a row fills the width */
static int crlf_right(const struct shot *s, unsigned int cols) {
    int i;
    for(i = 0; i < s->n; i++) if(s->crlf[i] != (s->cols[i] == (int)cols)) return 0;
    return 1;
}

static int starts(const char *s, const char *prefix) {
    return !strncmp(s, prefix, strlen(prefix));
}

static int ends(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && !strcmp(s + a - b, suffix);
}

/* the SurfOS console's: code page 437 drawn (remote 0) */
static struct ut_term term(unsigned int cols, unsigned int rows, int utf8, int color) {
    struct ut_term t;
    t.cols = cols;
    t.rows = rows;
    t.utf8 = utf8;
    t.color = color;
    t.remote = 0;
    return t;
}

/* ut_view_text() of text into a shot */
static void text_shot(struct shot *s, const struct ut_term *t, const char *text, const struct ut_span *spans, size_t n) {
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    ut_view_text(f, t, text, strlen(text), spans, n);
    fclose(f);
    shot_take(s, buf, len);
}

/* the words of text, joined by single spaces */
static char *words(const char *text) {
    char *out = malloc(strlen(text) + 1);
    size_t o = 0;
    const char *p = text;
    while(*p) {
        while(*p == ' ' || *p == '\n') p++;
        if(!*p) break;
        if(o) out[o++] = ' ';
        while(*p && *p != ' ' && *p != '\n') out[o++] = *p++;
    }
    out[o] = 0;
    return out;
}

/* the rows of a shot joined as words: what was shown */
static char *shot_words(const struct shot *s, int from, int to) {
    size_t total = 1;
    char *all, *w;
    int i;
    for(i = from; i < to; i++) total += strlen(s->row[i]) + 1;
    all = malloc(total);
    all[0] = 0;
    for(i = from; i < to; i++) {
        strcat(all, s->row[i]);
        strcat(all, " ");
    }
    w = words(all);
    free(all);
    return w;
}

/**** wrapping ****/

static const char *para =
    "Undertow reads pages one screen at a time. Each link has a number after its label, and typing the "
    "number follows it. This paragraph is long enough to wrap several times on a narrow terminal, which "
    "is what these tests look at: no row may be wider than the terminal, and no word may be lost.";

static void wrapping(void) {
    static const unsigned int widths[] = { 40, 80, 120 };
    struct shot s;
    struct ut_term t;
    char *want = words(para), *got, *long_word, *text;
    size_t i;
    int k, ok;
    for(i = 0; i < 3; i++) {
        char what[96];
        t = term(widths[i], 25, 1, 0);
        text_shot(&s, &t, para, NULL, 0);
        got = shot_words(&s, 0, s.n);
        snprintf(what, sizeof(what), "%u columns: every word in order, no row wider, \\r\\n only after full rows", widths[i]);
        check(!strcmp(got, want) && widest(&s) <= (int)widths[i] && crlf_right(&s, widths[i]) &&
              s.n == (widths[i] == 40 ? 8 : widths[i] == 80 ? 4 : 3), what);
        ok = 1;
        for(k = 0; k < s.n; k++) if(s.row[k][0] == ' ' || ends(s.row[k], " ")) ok = 0;
        snprintf(what, sizeof(what), "%u columns: rows neither start nor end with a space", widths[i]);
        check(ok, what);
        free(got);
        shot_free(&s);
    }
    free(want);

    /* a word longer than a row */
    long_word = malloc(200);
    memset(long_word, 'x', 100);
    long_word[100] = 0;
    t = term(40, 25, 1, 0);
    text_shot(&s, &t, long_word, NULL, 0);
    check(s.n == 3 && s.cols[0] == 40 && s.cols[1] == 40 && s.cols[2] == 20 && s.crlf[0] && !s.crlf[2],
          "a 100-letter word at 40 columns: 40, 40 and 20");
    shot_free(&s);
    strcpy(long_word, "abc ");
    memset(long_word + 4, 'y', 50);
    long_word[54] = 0;
    text_shot(&s, &t, long_word, NULL, 0);
    check(s.n == 3 && !strcmp(s.row[0], "abc") && s.cols[1] == 40 && s.cols[2] == 10, "a long word after a short one starts a row");
    shot_free(&s);
    free(long_word);

    /* multibyte text */
    text = malloc(400);
    text[0] = 0;
    for(k = 0; k < 100; k++) strcat(text, "\xc3\xa9");          /* e with an acute accent */
    text_shot(&s, &t, text, NULL, 0);
    check(s.n == 3 && s.cols[0] == 40 && s.cols[2] == 20 && !s.bad_utf8 && strlen(s.row[0]) == 80,
          "100 two-byte characters at 40 columns: one column each, no sequence split");
    shot_free(&s);
    text[0] = 0;
    for(k = 0; k < 50; k++) strcat(text, "\xe4\xb8\xad");       /* U+4E2D, a CJK character */
    strcat(text, " \xf0\x9f\x98\x80 end");
    t.remote = 1;                                                   /* a telnet client draws them */
    text_shot(&s, &t, text, NULL, 0);
    check(s.n == 2 && s.cols[0] == 40 && !strcmp(s.row[1], "\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad"
          "\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad \xf0\x9f\x98\x80 end") && !s.bad_utf8,
          "three- and four-byte characters");
    shot_free(&s);
    text_shot(&s, &t, "bad \xff\xc3 bytes \xe2\x82", NULL, 0);
    check(s.n == 1 && !strcmp(s.row[0], "bad \xef\xbf\xbd\xef\xbf\xbd bytes \xef\xbf\xbd\xef\xbf\xbd") && !s.bad_utf8,
          "ill-formed UTF-8 shows as U+FFFD, a byte at a time as ut_utf8_decode() reads it");
    shot_free(&s);
    t.remote = 0;
    text_shot(&s, &t, "bad \xff\xc3 bytes \xe2\x82", NULL, 0);
    check(s.n == 1 && !strcmp(s.row[0], "bad ?? bytes ??"), "on the console, which has no U+FFFD, a ?");
    shot_free(&s);

    /* the console draws code page 437; for the rest the ASCII stand-ins, and their widths */
    text_shot(&s, &t, "thin\xe2\x80\x89space zero\xe2\x80\x8bwidth \xe2\x82\xac" "5 \xe2\x80\xb0 \xe2\x80\xa0"
                      " \xc2\xbe caf\xc3\xa9 \xe2\x80\x9cq\xe2\x80\x9d \xe2\x96\xb8 \xe2\x80\xa6 \xe4\xb8\xad", NULL, 0);
    check(s.n == 2 && !strcmp(s.row[0], "thin space zerowidth EUR5 %o + 3/4 caf\xc3\xa9") &&
          !strcmp(s.row[1], "\xe2\x80\x9cq\xe2\x80\x9d \xe2\x96\xb8 \xe2\x80\xa6 ?"),
          "console: code page 437 as itself (and its aliases), the rest as stand-ins, ? last");
    shot_free(&s);
    text[0] = 0;
    for(k = 0; k < 20; k++) strcat(text, "\xe2\x82\xac");       /* U+20AC: EUR, three columns */
    text_shot(&s, &t, text, NULL, 0);
    check(s.n == 2 && s.cols[0] == 39 && s.cols[1] == 21 && !s.high, "console: rows count what is drawn");
    shot_free(&s);
    check(ut_view_stand_in(&t, 'a') == NULL && ut_view_stand_in(&t, 0xe9) == NULL && ut_view_stand_in(&t, 0x2026) == NULL &&
          ut_view_stand_in(&t, 0x201c) == NULL && !strcmp(ut_view_stand_in(&t, 0x20ac), "EUR") &&
          !strcmp(ut_view_stand_in(&t, 0x200b), "") && !strcmp(ut_view_stand_in(&t, 0x4e2d), "?"),
          "the console's rule: code page 437 and its aliases as themselves");
    t.utf8 = 0;
    check(!strcmp(ut_view_stand_in(&t, 0xe9), "e") && ut_view_stand_in(&t, 'a') == NULL, "an ASCII terminal's rule");
    t.utf8 = 1;
    t.remote = 1;
    check(ut_view_stand_in(&t, 0x20ac) == NULL && ut_view_stand_in(&t, 0x4e2d) == NULL, "a telnet terminal's rule");
    text_shot(&s, &t, "\xe2\x82\xac" "5 \xe2\x80\x89\xe4\xb8\xad", NULL, 0);
    check(s.n == 1 && !strcmp(s.row[0], "\xe2\x82\xac" "5 \xe2\x80\x89\xe4\xb8\xad"), "telnet: every printable character as itself");
    shot_free(&s);
    t.remote = 0;
    text_shot(&s, &t, "esc\033[2J bel\a c1\xc2\x9b" "31m bidi\xe2\x80\xae" "x tab\tz", NULL, 0);
    check(s.n == 1 && !s.bad_esc && !s.other_ctl && !strcmp(s.row[0], "esc[2J bel c131m bidix tab z"),
          "unsafe characters in text are left out, a tab is a space");
    shot_free(&s);
    free(text);

    /* list items and indented lines: continuation rows under the text */
    {
        static const struct { const char *line, *first; int indent; } items[] = {
            { "* The first item of a list, long enough to wrap twice at forty columns.", "* The first item", 2 },
            { "  12. A numbered item inside another list, long enough to wrap as well.", "  12. A numbered", 6 },
            { "  A quoted paragraph is indented by two spaces and so are its rows.", "  A quoted", 2 }
        };
        static const char *const what[] = { "a \"* \" item's rows go under its text",
                                            "a \"12. \" item's rows go under its text, two spaces in",
                                            "a paragraph indented by two spaces keeps its rows there" };
        t = term(40, 25, 1, 0);
        for(i = 0; i < 3; i++) {
            text_shot(&s, &t, items[i].line, NULL, 0);
            ok = s.n == 2 && starts(s.row[0], items[i].first) && widest(&s) <= 40;
            for(k = 1; k < s.n && ok; k++) {
                int sp = 0;
                while(s.row[k][sp] == ' ') sp++;
                if(sp != items[i].indent) ok = 0;
            }
            check(ok, what[i]);
            shot_free(&s);
        }
    }
    t = term(25, 25, 1, 0);
    text_shot(&s, &t, "    * nested deeply, so that an indent would leave too little room", NULL, 0);
    check(s.n >= 3 && s.row[1][0] != ' ' && widest(&s) <= 25, "no indent where it would leave fewer than 20 columns");
    shot_free(&s);

    /* blank lines and the end of the text */
    t = term(80, 25, 1, 0);
    text_shot(&s, &t, "one\n\ntwo\n", NULL, 0);
    check(s.n == 3 && !strcmp(s.row[0], "one") && !strcmp(s.row[1], "") && !strcmp(s.row[2], "two"),
          "a blank line is a row; a line break at the end adds none");
    shot_free(&s);

    /* ASCII terminals */
    t = term(40, 25, 0, 0);
    text_shot(&s, &t, "caf\xc3\xa9 \xe2\x80\x9cquoted\xe2\x80\x9d \xe2\x80\x94 done\xe2\x80\xa6 \xe4\xb8\xad", NULL, 0);
    check(s.n == 1 && !s.high && !strcmp(s.row[0], "cafe \"quoted\" -- done... ?"), "ASCII: the stand-ins of ut_ascii_for()");
    shot_free(&s);
    text = malloc(200);
    text[0] = 0;
    for(k = 0; k < 30; k++) strcat(text, "\xe2\x80\xa6");      /* U+2026 is "...": three columns in ASCII */
    text_shot(&s, &t, text, NULL, 0);
    check(s.n == 3 && widest(&s) <= 40 && !s.high, "ASCII: rows count the stand-ins' width");
    shot_free(&s);
    free(text);

    /* narrow terminals do not loop */
    t = term(1, 2, 1, 0);
    text_shot(&s, &t, "ab cd", NULL, 0);
    check(s.n == 4 && widest(&s) == 1, "one column: a character per row");
    shot_free(&s);
}

/**** positions: row starts, next, prev ****/

static char *numbered_lines(int n, const char *fmt) {
    char *text = malloc((size_t)n * 64 + 1);
    int i;
    text[0] = 0;
    for(i = 1; i <= n; i++) {
        snprintf(text + strlen(text), 64, fmt, i);
        if(i < n) strcat(text, "\n");
    }
    return text;
}

static void positions(void) {
    struct ut_term t80 = term(80, 25, 1, 0), t40 = term(40, 25, 1, 0);
    char *text = numbered_lines(100, "line %03d, which is long enough to wrap at forty columns");
    size_t len = strlen(text), starts80[200] = { 0 }, starts40[400] = { 0 }, at, p;
    int n80 = 0, n40 = 0, i, ok;
    /* the row starts at 80 columns are the lines' starts */
    for(at = 0; at < len; ) {
        starts80[n80++] = at;
        p = at;
        while(p < len && text[p] != '\n') p++;
        at = p + 1;
    }
    ok = n80 == 100;
    for(i = 0; i < n80 && ok; i++) {
        if(ut_view_row_start(&t80, text, len, starts80[i]) != starts80[i]) ok = 0;
        if(ut_view_row_start(&t80, text, len, starts80[i] + 5) != starts80[i]) ok = 0;
    }
    check(ok, "80 columns: a row starts each line; an offset inside a row gives its start");
    check(ut_view_row_start(&t80, text, len, len) == starts80[99] && ut_view_row_start(&t80, text, len, len + 50) == starts80[99],
          "the end of the text, or past it: the last row");
    /* 40 columns: two rows a line */
    for(i = 0; i < n80; i++) {
        starts40[n40++] = starts80[i];
        p = ut_view_row_start(&t40, text, len, starts80[i] + 45);
        if(p > starts80[i]) starts40[n40++] = p;
    }
    check(n40 == 200, "40 columns: two rows a line");
    /* next: a screen of 19 rows at a time; the last screen gives len */
    ok = 1;
    for(i = 0; i + 19 < n80; i += 19) if(ut_view_next(&t80, text, len, starts80[i]) != starts80[i + 19]) ok = 0;
    check(ok && ut_view_next(&t80, text, len, starts80[95]) == len && ut_view_next(&t80, text, len, starts80[81]) == len,
          "next: 19 rows on at 80x25, the end on the last screen");
    ok = 1;
    for(i = 0; i < n40; i++) {
        size_t want = starts40[i < 19 ? 0 : i - 19];
        if(ut_view_prev(&t40, text, len, starts40[i]) != want) ok = 0;
        if(ut_view_prev(&t40, text, len, starts40[i] + 3) != want) ok = 0;
    }
    check(ok, "prev from any position: 19 rows before the row that holds it, the top at most");
    check(ut_view_prev(&t40, text, len, 0) == 0, "prev at the top stays there");
    /* a resize keeps the reading position: the row that holds it comes first */
    at = starts40[123];
    check(ut_view_row_start(&t80, text, len, at) == starts80[61] && ut_view_row_start(&t40, text, len, at) == at,
          "a position at 40 columns falls in its line's row at 80, and is its own row's start again at 40");
    check(ut_view_next(&t40, text, len, len) == len && ut_view_prev(&t80, text, len, len) == starts80[99 - 19],
          "next and prev from the end");
    check(ut_view_next(&t80, "", 0, 0) == 0 && ut_view_prev(&t80, "", 0, 0) == 0 && ut_view_row_start(&t80, "", 0, 0) == 0,
          "an empty text");
    free(text);
}

/**** page views ****/

static struct ut_page *make_page(const char *text, const char *title, const char *url, int status, int verified,
                                 const char *tls, const char *reason) {
    struct ut_page *p = calloc(1, sizeof(*p));
    p->doc.text = strdup(text);
    p->doc.text_len = strlen(text);
    p->doc.title = strdup(title);
    strcpy(p->url.text, url);
    p->url.secure = !strncmp(url, "https:", 6);
    strcpy(p->url.host, "example.com");
    p->status = status;
    p->verified = verified;
    strcpy(p->tls, tls);
    strcpy(p->reason, reason);
    strcpy(p->content_type, "text/html");
    return p;
}

static void free_page(struct ut_page *p) {
    free(p->doc.text);
    free(p->doc.title);
    free(p->doc.spans);
    free(p);
}

static size_t view_shot(struct shot *s, const struct ut_term *t, const struct ut_page *p, size_t from, const char *message) {
    char *buf = NULL;
    size_t len = 0, next;
    FILE *f = open_memstream(&buf, &len);
    next = ut_view_page(f, t, p, from, message);
    fclose(f);
    shot_take(s, buf, len);
    return next;
}

static int is_rule(const char *row, unsigned int cols, int utf8) {
    unsigned int i;
    for(i = 0; i < cols; i++) {
        if(utf8 ? strncmp(row + 3 * i, "\xe2\x94\x80", 3) : row[i] != '-') return 0;
    }
    return row[utf8 ? 3 * cols : cols] == 0;
}

static void views(void) {
    char *text = numbered_lines(240, "line %d");
    struct ut_page *p = make_page(text, "Example Domain", "https://example.com/", 200, 1, "TLSv1.3", "OK");
    struct ut_term t;
    struct shot s;
    size_t next;
    int i, ok;

    t = term(120, 33, 1, 0);
    next = view_shot(&s, &t, p, 0, NULL);
    check(s.n == 32, "120x33: a view is 32 lines");
    check(starts(s.row[0], "Undertow - Example Domain ") && ends(s.row[0], "[HTTPS verified TLSv1.3]") && s.cols[0] == 120,
          "120x33: the title line, the state at the right edge");
    check(!strcmp(s.row[1], "https://example.com/") && is_rule(s.row[2], 120, 1) && is_rule(s.row[30], 120, 1),
          "120x33: the address and the rules");
    ok = 1;
    for(i = 0; i < 27; i++) {
        char want[32];
        snprintf(want, sizeof(want), "line %d", i + 1);
        if(strcmp(s.row[3 + i], want)) ok = 0;
    }
    check(ok, "120x33: 27 rows of the page");
    check(starts(s.row[31], "lines 1-27 of 240 (11%)") && s.cols[31] <= 120, "120x33: the position line");
    check(next == (size_t)(strstr(text, "line 28") - text), "120x33: the next screen starts at line 28");
    check(crlf_right(&s, 120) && !s.sgr && !s.bad_esc && !s.other_ctl, "120x33: no colours without the capability");
    shot_free(&s);

    t = term(80, 25, 1, 0);
    next = view_shot(&s, &t, p, 0, NULL);
    check(s.n == 24 && starts(s.row[0], "Undertow - Example Domain") && ends(s.row[0], "[HTTPS verified TLSv1.3]") &&
          s.cols[0] == 80 && !strcmp(s.row[3], "line 1") && !strcmp(s.row[21], "line 19") &&
          starts(s.row[23], "lines 1-19 of 240 (7%)") && widest(&s) <= 80, "80x25: 24 lines, 19 rows of the page");
    shot_free(&s);
    /* the last screen: 240 = 12 * 19 + 12 rows */
    next = view_shot(&s, &t, p, (size_t)(strstr(text, "line 229") - text), NULL);
    check(next == strlen(text) && s.n == 24 && !strcmp(s.row[3], "line 229") && !strcmp(s.row[14], "line 240") &&
          !strcmp(s.row[15], "") && !strcmp(s.row[21], "") && starts(s.row[23], "lines 229-240 of 240 (100%)"),
          "the last screen: the rest, blank rows, 100%");
    shot_free(&s);
    /* a position inside a row: the row that holds it */
    view_shot(&s, &t, p, (size_t)(strstr(text, "line 50") - text) + 3, NULL);
    check(!strcmp(s.row[3], "line 50") && starts(s.row[23], "lines 50-68 of 240 (28%)"), "from inside a row: that row first");
    shot_free(&s);
    /* the message takes the place of the keys */
    view_shot(&s, &t, p, 0, "found \"line 1\"");
    check(ends(s.row[23], "found \"line 1\"") && !strstr(s.row[23], "help"), "a message on the position line");
    shot_free(&s);

    /* 40 columns: the title gives way, then the status's details */
    t = term(40, 25, 1, 0);
    view_shot(&s, &t, p, 0, NULL);
    check(s.n == 24 && !strcmp(s.row[0], "Example Domain  [HTTPS verified TLSv1.3]") && widest(&s) <= 40,
          "40 columns: the title without the prefix, the whole state");
    shot_free(&s);
    free(p->doc.title);
    p->doc.title = strdup("A title much longer than any narrow terminal could ever show");
    view_shot(&s, &t, p, 0, NULL);
    check(ends(s.row[0], " [HTTPS verified]") && starts(s.row[0], "A title much longer") && strstr(s.row[0], "\xe2\x80\xa6") &&
          s.cols[0] == 40, "40 columns, a long title: cut short, the state kept");
    shot_free(&s);
    t = term(80, 25, 1, 0);
    view_shot(&s, &t, p, 0, NULL);
    check(starts(s.row[0], "Undertow - A title much longer") && ends(s.row[0], "[HTTPS verified TLSv1.3]") && s.cols[0] == 80,
          "80 columns, a long title: the prefix and the whole state, the title cut");
    shot_free(&s);

    /* HTTP, an error status, no title */
    free(p->doc.title);
    p->doc.title = strdup("");
    strcpy(p->url.text, "http://example.com/missing");
    p->url.secure = 0;
    p->verified = 0;
    p->tls[0] = 0;
    p->status = 404;
    strcpy(p->reason, "Not Found");
    view_shot(&s, &t, p, 0, NULL);
    check(starts(s.row[0], "Undertow - example.com ") && ends(s.row[0], "[HTTP 404 Not Found] [HTTP unencrypted]"),
          "HTTP 404: the status and the transport; no title: the host");
    shot_free(&s);
    t = term(40, 25, 1, 0);
    view_shot(&s, &t, p, 0, NULL);
    check(ends(s.row[0], "[HTTP 404] [HTTP unencrypted]") && s.cols[0] == 40 && starts(s.row[0], "example.c"),
          "HTTP 404 at 40 columns: the reason and the title give way, the state stays");
    shot_free(&s);
    p->status = 200;

    /* colours: only with the capability, only for the chrome and the spans */
    t = term(80, 25, 1, 1);
    p->doc.spans = calloc(2, sizeof(struct ut_span));
    p->doc.spans[0].offset = 0;                 /* "line 1" a heading, its "1" a link marker */
    p->doc.spans[0].len = 6;
    p->doc.spans[0].kind = UT_SPAN_HEADING;
    p->doc.spans[1].offset = 5;
    p->doc.spans[1].len = 1;
    p->doc.spans[1].kind = UT_SPAN_LINK;
    p->doc.span_count = 2;
    view_shot(&s, &t, p, 0, NULL);
    check(s.sgr > 0 && !s.bad_esc && s.n == 24 && !strcmp(s.row[3], "line 1"), "colours with the capability, the text unchanged");
    check(strstr(s.raw, "\033[0;1;37mline \033[0;1;34m1\033[0m") != NULL,
          "a heading in bright white, a link marker in it light blue, back to plain at the end of the row");
    shot_free(&s);
    t = term(80, 25, 1, 0);
    view_shot(&s, &t, p, 0, NULL);
    check(!s.sgr && !s.bad_esc, "the same page without colours: no escape sequence at all");
    shot_free(&s);

    /* the console: a title's characters outside code page 437 as stand-ins too */
    free(p->doc.title);
    p->doc.title = strdup("Price \xe2\x82\xac and \xe2\x80\xb0");
    t = term(80, 25, 1, 0);
    view_shot(&s, &t, p, 0, NULL);
    check(starts(s.row[0], "Undertow - Price EUR and %o ") && s.cols[0] == 80, "console: the title line's stand-ins");
    shot_free(&s);
    t.remote = 1;
    view_shot(&s, &t, p, 0, NULL);
    check(starts(s.row[0], "Undertow - Price \xe2\x82\xac and \xe2\x80\xb0 ") && s.cols[0] == 80, "telnet: the title as it is");
    shot_free(&s);

    /* ASCII */
    free(p->doc.text);
    p->doc.text = strdup("caf\xc3\xa9 \xe2\x80\x94 na\xc3\xafve\n\xe2\x80\x9cquoted\xe2\x80\x9d");
    p->doc.text_len = strlen(p->doc.text);
    p->doc.span_count = 0;
    free(p->doc.title);
    p->doc.title = strdup("Caf\xc3\xa9");
    t = term(80, 25, 0, 0);
    view_shot(&s, &t, p, 0, NULL);
    check(!s.high && !strcmp(s.row[3], "cafe -- naive") && !strcmp(s.row[4], "\"quoted\"") &&
          starts(s.row[0], "Undertow - Cafe") && is_rule(s.row[2], 80, 0), "ASCII: text, title and rules");
    shot_free(&s);

    /* a reason phrase beyond ASCII: an ASCII terminal gets its stand-ins */
    p->status = 404;
    strcpy(p->reason, "N\xc3\xa3o Encontrado");
    view_shot(&s, &t, p, 0, NULL);
    check(!s.high && ends(s.row[0], "[HTTP 404 Nao Encontrado] [HTTP unencrypted]"), "ASCII: the reason phrase too");
    shot_free(&s);
    /* the console draws UTF-8 as code page 437 cells: a reason phrase's zero-width characters
       (stand-in: nothing) and its characters outside the set (U+00E3) are drawn as they were
       measured, so the title line is one row and the view never reaches --More-- */
    t = term(80, 25, 1, 0);
    strcpy(p->reason, "Not\xe2\x80\x8b\xe2\x80\x8b\xe2\x80\x8b" "Found");
    view_shot(&s, &t, p, 0, NULL);
    check(s.n == 24 && s.cols[0] == 80 && ends(s.row[0], "[HTTP 404 NotFound] [HTTP unencrypted]"),
          "console: a reason phrase with zero-width spaces draws as wide as it was measured");
    shot_free(&s);
    strcpy(p->reason, "N\xc3\xa3o\xc2\xad Encontrado\xe2\x81\xa0!");
    view_shot(&s, &t, p, 0, NULL);
    check(s.n == 24 && s.cols[0] == 80 && ends(s.row[0], "[HTTP 404 Nao Encontrado!] [HTTP unencrypted]"),
          "console: a reason phrase beyond code page 437 (U+00E3, U+00AD, U+2060) as its stand-ins");
    shot_free(&s);
    t = term(40, 25, 1, 0);
    view_shot(&s, &t, p, 0, NULL);
    check(s.n == 24 && s.cols[0] == 40 && widest(&s) <= 40, "console, 40 columns: the title line still one row");
    shot_free(&s);
    t = term(80, 25, 1, 0);
    t.remote = 1;
    strcpy(p->reason, "N\xc3\xa3o Encontrado");
    view_shot(&s, &t, p, 0, NULL);
    check(s.n == 24 && s.cols[0] == 80 && ends(s.row[0], "[HTTP 404 N\xc3\xa3o Encontrado] [HTTP unencrypted]"),
          "telnet: the reason phrase as it is");
    shot_free(&s);
    t = term(80, 25, 0, 0);
    p->status = 200;
    strcpy(p->reason, "OK");

    /* an empty page */
    free(p->doc.text);
    p->doc.text = strdup("");
    p->doc.text_len = 0;
    t = term(80, 25, 1, 0);
    next = view_shot(&s, &t, p, 0, NULL);
    check(s.n == 24 && next == 0 && !strcmp(s.row[3], "") && starts(s.row[23], "empty page"), "an empty page");
    shot_free(&s);

    /* the page's notice on its first screen */
    free(p->doc.text);
    p->doc.text = strdup("text");
    p->doc.text_len = 4;
    strcpy(p->doc.notice, "forms are not supported");
    view_shot(&s, &t, p, 0, NULL);
    check(strstr(s.row[23], "forms are not supported") != NULL, "the page's notice on the position line");
    shot_free(&s);
    p->doc.notice[0] = 0;

    /* escape bytes in the page's fields never reach the terminal */
    free(p->doc.title);
    p->doc.title = strdup("evil\033[2Jtitle\xc2\x9b\033]0;x\a");
    free(p->doc.text);
    p->doc.text = strdup("text\033]0;owned\a and \033[31mred");
    p->doc.text_len = strlen(p->doc.text);
    strcpy(p->url.text, "https://example.com/\033[2J\033[31m\x9b");
    strcpy(p->reason, "Bad\033[1K\033[1m");
    p->status = 500;
    t = term(80, 25, 1, 0);
    view_shot(&s, &t, p, 0, "message\033[2J\033[1m");
    check(!s.sgr && !s.bad_esc && !s.other_ctl && !s.bad_utf8, "no colours: no escape byte at all, whatever the page sends");
    shot_free(&s);
    t = term(80, 25, 1, 1);
    view_shot(&s, &t, p, 0, "message\033[2J");
    check(!s.bad_esc && !s.other_ctl && !s.bad_utf8, "colours: only the view's own colour sequences");
    shot_free(&s);
    free(text);
    free_page(p);
}

/* every size: never more than rows - 1 lines (or the compact layout), never wider */
static void sizes(void) {
    static const unsigned int cols[] = { 8, 20, 39, 40, 41, 80, 120, 300 };
    static const unsigned int rows[] = { 2, 3, 7, 8, 24, 25, 33, 60 };
    char *text = numbered_lines(300, "row %d with a few words to wrap on the narrower terminals here");
    struct ut_page *p = make_page(text, "Sizes", "https://example.com/a/very/long/address/that/does/not/fit/on/a/narrow/terminal/at/all",
                                  404, 1, "TLSv1.2", "Not Found");
    struct shot s;
    size_t i, j, from;
    int ok = 1;
    for(i = 0; i < sizeof(cols) / sizeof(cols[0]); i++)
        for(j = 0; j < sizeof(rows) / sizeof(rows[0]); j++) {
            struct ut_term t = term(cols[i], rows[j], (int)(i & 1), 1);
            int want = rows[j] >= 8 ? (int)rows[j] - 1 : (rows[j] - 1 >= 3 ? (int)rows[j] - 1 : 3);
            for(from = 0; from < strlen(text); from += strlen(text) / 3) {
                view_shot(&s, &t, p, from, NULL);
                if(s.n != want || widest(&s) > (int)cols[i] || s.bad_esc || s.other_ctl || s.bad_utf8) {
                    printf("  %ux%u from %u: %d lines (want %d), widest %d\n", cols[i], rows[j], (unsigned)from, s.n,
                           want, widest(&s));
                    ok = 0;
                }
                shot_free(&s);
            }
        }
    check(ok, "8-300 columns, 2-60 rows: exactly the lines a view has, none wider than the terminal");
    free(text);
    free_page(p);
}

static void welcome_and_lines(void) {
    static const unsigned int sizes[][2] = { { 40, 25 }, { 80, 25 }, { 120, 33 }, { 80, 24 } };
    const char *clock = "Clock  2026-10-11 14:05:09 UTC, from the hardware clock";
    const char *https = "HTTPS  ready: strong randomness and 121 trusted roots";
    struct shot s;
    size_t i;
    for(i = 0; i < 4; i++) {
        struct ut_term t = term(sizes[i][0], sizes[i][1], 1, 1);
        char *buf = NULL, what[80];
        size_t len = 0;
        FILE *f = open_memstream(&buf, &len);
        char *all;
        ut_view_welcome(f, &t, clock, https);
        fclose(f);
        shot_take(&s, buf, len);
        all = shot_words(&s, 0, s.n);
        snprintf(what, sizeof(what), "the welcome screen at %ux%u: rows - 1 lines, all of it", sizes[i][0], sizes[i][1]);
        check(s.n == (int)sizes[i][1] - 1 && widest(&s) <= (int)sizes[i][0] && strstr(all, "open https://example.com/") &&
              strstr(all, "2026-10-11 14:05:09 UTC, from the hardware clock") && strstr(all, "121 trusted roots") &&
              starts(s.row[0], "Undertow 1.0") && !s.bad_esc, what);
        free(all);
        shot_free(&s);
    }
    {
        struct ut_term t = term(80, 25, 1, 0);
        char *buf = NULL;
        size_t len = 0;
        FILE *f = open_memstream(&buf, &len);
        int k, two = 0;
        ut_view_welcome(f, &t, "Clock  2004-01-01 00:00:12 UTC: too early\n       set it with clock\033[2J",
                        "HTTPS  unavailable: no RDRAND\n       http:// pages still work");
        fclose(f);
        shot_take(&s, buf, len);
        for(k = 0; k < s.n; k++)
            if(!strcmp(s.row[k], "       http:// pages still work") || !strcmp(s.row[k], "       set it with clock[2J")) two++;
        check(two == 2 && !s.bad_esc && s.n == 24, "the state's lines keep their own breaks and indents, made safe");
        shot_free(&s);
    }
    {
        struct ut_term t = term(40, 25, 1, 0);
        char *buf = NULL;
        size_t len = 0;
        FILE *f = open_memstream(&buf, &len);
        ut_view_line(f, &t, "Loading ", "https://example.com/a/very/long/address/that/needs/cutting", " ...");
        fclose(f);
        shot_take(&s, buf, len);
        check(s.n == 1 && s.cols[0] == 40 && starts(s.row[0], "Loading https://example.com/") && ends(s.row[0], "\xe2\x80\xa6 ...") &&
              s.crlf[0], "one line: the address cut short to fit");
        shot_free(&s);
        f = open_memstream(&buf, &len);
        ut_view_line(f, &t, "Loading ", "http://a/", " ...");
        fclose(f);
        shot_take(&s, buf, len);
        check(s.n == 1 && !strcmp(s.row[0], "Loading http://a/ ...") && !s.crlf[0], "one line: a short address whole");
        shot_free(&s);
    }
}

/**** the line editor ****/

struct chunk {
    const char *bytes;
    size_t len;
    int early;                  /* there before the prompt (typed during a load) */
    unsigned int delay;         /* milliseconds that pass before it comes */
};

static struct chunk script[64];
static int nscript, at_script;
static char *drawn;             /* everything the editor wrote */
static size_t drawn_len, drawn_cap;

static long fake_read(void *ctx, unsigned char *buf, size_t cap) {
    struct chunk *c;
    size_t n;
    (void)ctx;
    if(at_script == nscript) return 0;
    c = &script[at_script];
    sim_ms += c->delay;
    c->delay = 0;
    n = c->len < cap ? c->len : cap;
    memcpy(buf, c->bytes, n);
    c->bytes += n;
    c->len -= n;
    if(!c->len) at_script++;
    return (long)n;
}

static int fake_ready(void *ctx) {
    (void)ctx;
    return at_script < nscript && script[at_script].early;
}

static void fake_write(void *ctx, const char *buf, size_t len) {
    (void)ctx;
    if(!drawn_len && !raw_at_draw) raw_at_draw = raw_on ? 'r' : 'l';
    if(drawn_len + len + 1 > drawn_cap) {
        drawn_cap = (drawn_len + len + 1) * 2;
        drawn = realloc(drawn, drawn_cap);
    }
    memcpy(drawn + drawn_len, buf, len);
    drawn_len += len;
    drawn[drawn_len] = 0;
}

static void keys(const char *bytes, size_t len, int early, unsigned int delay) {
    script[nscript].bytes = bytes;
    script[nscript].len = len;
    script[nscript].early = early;
    script[nscript].delay = delay;
    nscript++;
}
#define KEYS(s) keys(s, sizeof(s) - 1, 0, 0)

static void reset_terminal(void) {
    nscript = at_script = 0;
    drawn_len = 0;
    if(drawn) drawn[0] = 0;
    raw_at_draw = 0;
    skips = raw_ons = raw_offs = 0;
}

/* the prompt's row as the terminal shows it at the end: the text after the last \r, colours out */
static int row_fits(unsigned int cols) {
    size_t i;
    int col = 0, worst = 0;
    for(i = 0; i < drawn_len; i++) {
        unsigned char c = (unsigned char)drawn[i];
        if(c == '\r' || c == '\n') { col = 0; continue; }
        if(c == 0x1b) {
            i++;
            if(i < drawn_len && drawn[i] == '[') {
                int n = 0;
                i++;
                while(i < drawn_len && ((drawn[i] >= '0' && drawn[i] <= '9') || drawn[i] == ';' || drawn[i] == '?'))
                    n = n * 10 + (drawn[i] >= '0' && drawn[i] <= '9' ? drawn[i] - '0' : 0), i++;
                if(i < drawn_len && drawn[i] == 'C') col += n ? n : 1;
            }
            continue;
        }
        if((c & 0xc0) != 0x80) col++;
        if(col > worst) worst = col;
    }
    return worst <= (int)cols;
}

static void editor(void) {
    struct ut_edit_io io = { NULL, fake_read, fake_ready, fake_write };
    struct ut_editor *e = ut_edit_new(&io);
    struct ut_term t = term(80, 25, 1, 1), ta = term(40, 25, 0, 0);
    char *buf = malloc(UT_COMMAND_MAX + 1), *big;
    int r;

    check(e != NULL, "an editor");
    reset_terminal();
    KEYS("open example.com\r");
    r = ut_edit_line(e, &t, 0, buf);
    check(r == 16 && !strcmp(buf, "open example.com"), "a line and Enter");
    check(raw_at_draw == 'r' && raw_ons == 1 && raw_offs == 1 && !raw_on, "the prompt is drawn in raw mode, line mode after");
    check(strstr(drawn, "undertow>") != NULL && row_fits(80), "the prompt, on one row");
    check(ends(drawn, "\033[0m\r\n"), "the line ends with the colours reset (the typed text is teal)");
    {
        static const struct { const char *keys; const char *end; } ends_of[] = {
            { "abc\003", "\033[0m^C\r\n" }, { "\004", "\033[0mquit\r\n" }, { "\033[6~", "\033[0mnext\r\n" },
            { "\033[5~", "\033[0mprev\r\n" }, { "abc", "\033[0m\r\n" }
        };
        size_t k;
        int all = 1;
        for(k = 0; k < sizeof(ends_of) / sizeof(ends_of[0]); k++) {
            reset_terminal();
            keys(ends_of[k].keys, strlen(ends_of[k].keys), 0, 0);
            ut_edit_line(e, &t, 0, buf);
            if(!ends(drawn, ends_of[k].end)) {
                printf("  after %s: no reset at the end\n", ends_of[k].end + 4);
                all = 0;
            }
        }
        check(all, "^C, ^D, PgDn, PgUp and the end of the input reset the colours too");
    }

    reset_terminal();
    KEYS("helo\x7f\x7flp\033[D\033[D\033[3~X\001\033[C\005!\r");
    r = ut_edit_line(e, &t, 0, buf);
    check(r == 5 && !strcmp(buf, "heXp!"), "Backspace, Left, Delete, Home (^A), Right, End (^E)");

    reset_terminal();
    KEYS("garbage\025back\r");
    r = ut_edit_line(e, &t, 0, buf);
    check(r == 4 && !strcmp(buf, "back"), "^U clears the line");

    reset_terminal();
    KEYS("one two three\027\027four\013\r");
    r = ut_edit_line(e, &t, 0, buf);
    check(r == 8 && !strcmp(buf, "one four"), "^W deletes words, ^K the rest");

    reset_terminal();
    KEYS("\033[A\033[A\033[A\033[B\r");
    r = ut_edit_line(e, &t, 0, buf);
    check(r == 4 && !strcmp(buf, "back"), "Up and Down go through the lines typed before");
    reset_terminal();
    KEYS("partial\033[A\033[B\r");
    r = ut_edit_line(e, &t, 0, buf);
    check(r == 7 && !strcmp(buf, "partial"), "Down after Up gives back what was being typed");

    reset_terminal();
    KEYS("\033[1;5C\033[200~x\033[201~\033OH\033[1~y\033[4~\033[?1;2c\r");
    r = ut_edit_line(e, &t, 0, buf);
    check(r == 2 && !strcmp(buf, "yx"), "escape sequences are read to their end; unknown ones leave nothing");

    reset_terminal();
    keys("ab\033", 3, 0, 0);
    keys("[D", 2, 0, 100);
    KEYS("c\r");
    r = ut_edit_line(e, &t, 0, buf);
    check(r == 3 && !strcmp(buf, "acb"), "an escape sequence that arrives in two pieces");
    reset_terminal();
    keys("ab\033", 3, 0, 0);
    keys("[D", 2, 0, 1000);
    KEYS("\r");
    r = ut_edit_line(e, &t, 0, buf);
    check(r == 4 && !strcmp(buf, "ab[D"), "a lone Esc long before [D: the keys are typed");

    reset_terminal();
    KEYS("caf\xc3\xa9 \xc2\x9b\xe2\x80\xae\xe4\xb8\xad\033[Dx\033[D\033[D\x7f\r");
    r = ut_edit_line(e, &t, 0, buf);
    check(r == 8 && !strcmp(buf, "caf x\xe4\xb8\xad"),
          "UTF-8 where the terminal draws it, a character at a time (Left, Backspace); unsafe characters left out");
    reset_terminal();
    KEYS("caf\xc3\xa9!\r");
    r = ut_edit_line(e, &ta, 0, buf);
    check(r == 4 && !strcmp(buf, "caf!"), "an ASCII terminal takes ASCII only");

    reset_terminal();
    KEYS("next\rquit\r");
    r = ut_edit_line(e, &t, 0, buf);
    check(r == 4 && !strcmp(buf, "next") && skips == 1, "keys after the Enter: dropped, and the rest of the paste skipped");
    check(at_script == nscript, "the editor read them all");
    reset_terminal();
    KEYS("next\r\n");
    r = ut_edit_line(e, &t, 0, buf);
    check(r == 4 && skips == 0, "CR LF is one Enter, nothing skipped");
    reset_terminal();
    keys("next\r\0", 6, 0, 0);
    r = ut_edit_line(e, &t, 0, buf);
    check(r == 4 && skips == 0, "CR NUL (a telnet client's Enter) is one Enter, nothing skipped");
    reset_terminal();
    KEYS("top\r");
    r = ut_edit_line(e, &t, 0, buf);
    KEYS("\nurl\r");
    check(r == 3 && ut_edit_line(e, &t, 0, buf) == 3 && !strcmp(buf, "url"),
          "the LF of a CR LF that comes after the line ended is no empty line");
    reset_terminal();
    KEYS("\0top\r");
    r = ut_edit_line(e, &t, 0, buf);
    check(r == 3 && !strcmp(buf, "top"), "a NUL that comes late is nothing either");
    reset_terminal();
    KEYS("\r");
    r = ut_edit_line(e, &t, 0, buf);
    check(r == 0 && buf[0] == 0, "Enter alone: an empty line");

    reset_terminal();
    keys("typed during the load\r", 22, 1, 0);
    KEYS("top\r");
    r = ut_edit_line(e, &t, 1, buf);
    check(r == 3 && !strcmp(buf, "top") && skips == 1, "keys typed during a load are dropped");

    reset_terminal();
    big = malloc(5001);
    memset(big, 'a', 5000);
    big[5000] = '\r';
    keys(big, 5001, 0, 0);
    KEYS("quit\r");
    r = ut_edit_line(e, &t, 0, buf);
    check(r == UT_EDIT_LONG && row_fits(80), "a line over 4096 bytes is refused whole; the row stays one row");
    r = ut_edit_line(e, &t, 0, buf);
    check(r == 4 && !strcmp(buf, "quit"), "the next line is read as usual");
    reset_terminal();
    memset(big, 'b', 4096);
    big[4096] = '\r';
    keys(big, 4097, 0, 0);
    r = ut_edit_line(e, &t, 0, buf);
    check(r == 4096 && strlen(buf) == 4096, "a line of exactly 4096 bytes is taken");
    reset_terminal();
    memset(big, 'c', 4097);
    big[4097] = '\025';
    keys(big, 4098, 0, 0);
    KEYS("ok\r");
    r = ut_edit_line(e, &t, 0, buf);
    check(r == 2 && !strcmp(buf, "ok"), "^U after a line too long starts again");
    free(big);

    reset_terminal();
    KEYS("abc\003");
    r = ut_edit_line(e, &t, 0, buf);
    check(r == UT_EDIT_QUIT && !raw_on, "Ctrl+C quits, line mode after");
    reset_terminal();
    KEYS("\004");
    r = ut_edit_line(e, &t, 0, buf);
    check(r == UT_EDIT_EOF, "Ctrl+D on an empty line: the end");
    reset_terminal();
    KEYS("ab\033[D\004\r");
    r = ut_edit_line(e, &t, 0, buf);
    check(r == 1 && !strcmp(buf, "a"), "Ctrl+D on a line deletes");
    reset_terminal();
    r = ut_edit_line(e, &t, 0, buf);
    check(r == UT_EDIT_EOF && !raw_on, "the end of the input");
    reset_terminal();
    KEYS("\033[6~");
    check(ut_edit_line(e, &t, 0, buf) == UT_EDIT_NEXT, "PgDn on an empty line: next");
    reset_terminal();
    KEYS("\033[5~");
    check(ut_edit_line(e, &t, 0, buf) == UT_EDIT_PREV, "PgUp on an empty line: prev");
    reset_terminal();
    KEYS("x\033[6~\r");
    check(ut_edit_line(e, &t, 0, buf) == 1, "PgDn on a line: nothing");

    /* a long line on a narrow terminal scrolls sideways */
    reset_terminal();
    KEYS("open https://example.com/a/path/long/enough/to/scroll/sideways/on/forty/columns\001\005\r");
    r = ut_edit_line(e, &ta, 0, buf);
    check(r > 40 && row_fits(40), "a line longer than the row: one row, the cursor in sight");

    /* without raw mode: the console's line editing */
    reset_terminal();
    raw_works = 0;
    KEYS("url\n");
    r = ut_edit_line(e, &t, 0, buf);
    check(r == 3 && !strcmp(buf, "url") && strstr(drawn, "undertow>") != NULL, "no raw mode: a line as the console reads it");
    raw_works = 1;

    ut_edit_free(e);
    free(buf);
    free(drawn);
    drawn = NULL;
    drawn_cap = 0;
    check(ut_alloc_live == 0, "the editor gives back its memory");

    /* out of memory */
    ut_alloc_fail_after = 0;
    check(ut_edit_new(&io) == NULL, "no memory: no editor");
    ut_alloc_fail_after = -1;
    check(ut_alloc_live == 0, "nothing kept");
}

int main(void) {
    wrapping();
    positions();
    views();
    sizes();
    welcome_and_lines();
    editor();
    check(ut_alloc_live == 0, "the view allocates nothing");
    printf("UNDERTOW VIEW TESTS %s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
