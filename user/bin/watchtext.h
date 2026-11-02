/*
SurfOS user programs: watch's text
----------------------
File: watchtext.h  Date: 11/01/26
----------------------
What a command printed, turned into the rows of a screen: a small terminal that holds only what
a person reads. Characters go into cells, one a cell; a long line goes on in the next row, as it
would at the prompt; \n, \r, \b and TAB move as they do on a console. What would move or colour
the real screen does not get there: escape sequences are read and dropped, other control
characters are dropped, and what is no character shows as a '?', one for each byte. Past the
last row the text is read and goes nowhere, so a command that prints a lot still ends.

The bytes come from a pipe in reads of any size, so a character or an escape sequence may end in
a later read than it began: everything here keeps its place between calls of ws_feed(). No
system call is made here (watch.c makes them), so tests/host/test_watch.c runs this as it is,
on damaged and random input too.
*/

#ifndef _WATCHTEXT_H
#define _WATCHTEXT_H

#include "termview.h"

#define WATCH_TEXT_MAX 255              /* the longest argument a program takes (surfos/spawn.h) */
#define WATCH_TAB 8

struct ws_cell {
    char b[4];                          /* what it shows, a character's bytes */
    unsigned char n;                    /* how many; 0: nothing */
};

struct wscreen {
    struct ws_cell *cells;              /* rows by cols, the top row first */
    int rows, cols;
    int row, col;                       /* where the next character goes: col == cols when the row has just filled */
    int utf8;                           /* the terminal draws UTF-8 */
    int esc;                            /* 0 text, then the WS_* of the sequence it is inside */
    unsigned char pend[4];              /* the start of a character whose end has not come */
    int npend;
};

enum { WS_ESC = 1, WS_CSI, WS_STRING };

/* a screen of that size, empty: 0, or -1 without memory (the old one is then gone) */
static TV_UNUSED int ws_size(struct wscreen *w, int rows, int cols) {
    free(w->cells);
    w->cells = calloc((size_t)rows * (size_t)cols, sizeof(w->cells[0]));
    w->rows = w->cells ? rows : 0;
    w->cols = w->cells ? cols : 0;
    w->row = w->col = w->esc = w->npend = 0;
    return w->cells ? 0 : -1;
}

static TV_UNUSED void ws_clear(struct wscreen *w) {
    if(w->cells) memset(w->cells, 0, (size_t)w->rows * (size_t)w->cols * sizeof(w->cells[0]));
    w->row = w->col = w->esc = w->npend = 0;
}

/* the next row, no further than the one past the last: a pipe carries 2^31 line ends in minutes, and an int that counted
   them all would come round to below 0, and the next character go outside the cells */
static void ws_down(struct wscreen *w) {
    if(w->row < w->rows) w->row++;
}

/* one character into the cell under the cursor; a row that has filled goes on in the next */
static void ws_put(struct wscreen *w, const void *b, int n) {
    struct ws_cell *c;
    if(w->col >= w->cols) {
        w->col = 0;
        ws_down(w);
    }
    if(w->row >= w->rows) return;
    c = &w->cells[w->row * w->cols + w->col];
    memcpy(c->b, b, (size_t)n);
    c->n = (unsigned char)n;
    w->col++;
}

/* a byte below 0x80 outside any sequence */
static void ws_ascii(struct wscreen *w, unsigned char c) {
    switch(c) {
    case '\n':
        ws_down(w);
        w->col = 0;
        break;
    case '\r':
        w->col = 0;
        break;
    case '\b':
        if(w->col) w->col--;
        break;
    case '\t':
        w->col = (w->col / WATCH_TAB + 1) * WATCH_TAB;
        if(w->col > w->cols) w->col = w->cols;
        break;
    case 0x1b:
        w->esc = WS_ESC;
        break;
    default:
        if(c >= 0x20 && c != 0x7f) ws_put(w, &c, 1);
    }
}

static void ws_drop_pend(struct wscreen *w, int n) {
    w->npend -= n;
    memmove(w->pend, w->pend + n, (size_t)w->npend);
}

/* what is held of a character, once there is enough of it to say what it is. A byte that turns out to begin no
   character is a '?', and those after it are looked at again. Only the last byte held can be below 0x80, as every
   byte before it had to continue a character: it is plain text, or the Escape of a sequence. */
static void ws_settle(struct wscreen *w) {
    while(w->npend) {
        unsigned char c = w->pend[0];
        int len = tv_utf8(w->pend, (size_t)w->npend);
        if(len < 0) return;
        if(c < 0x80) {
            ws_drop_pend(w, 1);
            ws_ascii(w, c);
        } else if(len) {
            if(w->utf8) ws_put(w, w->pend, len);
            else ws_put(w, "?", 1);
            ws_drop_pend(w, len);
        } else {
            ws_put(w, "?", 1);
            ws_drop_pend(w, 1);
        }
    }
}

/* the next byte of what the command printed */
static TV_UNUSED void ws_byte(struct wscreen *w, unsigned char c) {
    if(w->esc == WS_ESC) {
        if(c == '[') w->esc = WS_CSI;
        else if(c == ']' || c == 'P' || c == '^' || c == '_' || c == 'X') w->esc = WS_STRING;
        else if(c < 0x20) {             /* a control character ends it, and is what it is */
            w->esc = 0;
            ws_ascii(w, c);
        } else if(c > 0x2f) w->esc = 0; /* ESC 7, ESC c: done; ESC ( B: an intermediate, one more byte to come */
        return;
    }
    if(w->esc == WS_CSI) {
        if(c >= 0x20 && c <= 0x3f) return;
        w->esc = 0;
        if(c < 0x20) ws_ascii(w, c);
        return;
    }
    if(w->esc == WS_STRING) {           /* a title, say: to BEL or ESC \ , or a control character when that never comes */
        if(c >= 0x20) return;
        w->esc = 0;
        if(c != 7) ws_ascii(w, c);
        return;
    }
    if(!w->npend && c < 0x80) {
        ws_ascii(w, c);
        return;
    }
    w->pend[w->npend++] = c;            /* room: ws_settle() leaves at most three */
    ws_settle(w);
}

static TV_UNUSED void ws_feed(struct wscreen *w, const void *p, size_t n) {
    const unsigned char *s = p;
    size_t i;
    if(w->cells) for(i = 0; i < n; i++) ws_byte(w, s[i]);
}

/* the end of the text: a character still waiting for its end never comes whole. That is a '?' a byte. */
static TV_UNUSED void ws_finish(struct wscreen *w) {
    while(w->npend) {
        ws_put(w, "?", 1);
        ws_drop_pend(w, 1);
    }
    w->esc = 0;
}

/* row y's text, at most width characters, without the blanks at its end: its length. out holds width * 4 bytes. */
static TV_UNUSED size_t ws_text(const struct wscreen *w, int y, int width, char *out) {
    const struct ws_cell *c = &w->cells[y * w->cols];
    size_t len = 0;
    int x, end = width < w->cols ? width : w->cols;
    while(end > 0 && !c[end - 1].n) end--;
    for(x = 0; x < end; x++) {
        if(c[x].n) {
            memcpy(out + len, c[x].b, c[x].n);
            len += c[x].n;
        } else out[len++] = ' ';        /* a TAB, or \r and a shorter line, went over it */
    }
    return len;
}

/* the words of a command as one text, as surfsh -c takes it: 0, or -1 when it does not fit in a program's argument */
static TV_UNUSED int ws_join(int n, char **words, char *out, size_t size) {
    size_t len = 0;
    int i;
    out[0] = 0;
    for(i = 0; i < n; i++) {
        size_t k = strlen(words[i]);
        if(len + k + (i ? 1 : 0) >= size) return -1;
        if(i) out[len++] = ' ';
        memcpy(out + len, words[i], k + 1);
        len += k;
    }
    return 0;
}

/* The title row: "Every 2s: command" at the left, the time of this run at the right, when both fit; a narrow screen
   keeps the command, which is what says what this is. The length. */
static TV_UNUSED size_t ws_title(unsigned int ms, const char *text, const char *when, int width, int utf8, char *out, size_t size) {
    char every[32], left[WATCH_TEXT_MAX * 4 + 64], clean[WATCH_TEXT_MAX * 4 + 8];
    int room = width - (int)strlen(when) - 1;
    tv_every(ms, every, sizeof(every));
    tv_clean(text, strlen(text), clean, sizeof(clean), utf8);
    snprintf(left, sizeof(left), "Every %s: %s", every, clean);
    if(room < 16) {
        tv_cut(left, width);
        snprintf(out, size, "%s", left);
    } else {
        size_t n = tv_cut(left, room);
        snprintf(out, size, "%s%*s%s", left, room + 1 - tv_chars(left, n), "", when);
    }
    return strlen(out);
}

#endif
