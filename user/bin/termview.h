/*
SurfOS user programs: what top and watch share
----------------------
File: termview.h  Date: 11/01/26
----------------------
A program that keeps a screen up to date on the alternate screen, in the ANSI subset that
include/surfos/tty.h lists: seconds as an option gives them, the keys a terminal sends, one
buffer of output and one write a screen, and rows sent only when they changed, so that a redraw
costs little over a serial line or telnet and nothing flickers. Text from outside goes through
tv_clean() or tv_utf8() first, so nothing but what the program means ever reaches the terminal.

The only system call here is the write in tv_flush(), which keeps this file buildable on the
host as it is (tests/host/test_top.c, test_watch.c).
*/

#ifndef _TERMVIEW_H
#define _TERMVIEW_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#define TV_UNUSED __attribute__((unused))

#define TV_MAX_ROWS 256                 /* as less: a terminal claiming more is taken to be this big */
#define TV_MAX_COLS 512

/* the screen given over, and given back: without an alternate screen the second leaves a clear one */
#define TV_ENTER "\x1b[?1049h\x1b[0m\x1b[2J\x1b[?25l"
#define TV_LEAVE "\x1b[0m\x1b[2J\x1b[H\x1b[?25h\x1b[?1049l"

/**** seconds ****/

/* "2", "0.5", "1.25" in *ms, read without floating point (a program of integers need not use the FPU): 0, or -1 for
   anything else and for what top and watch cannot use. Below 0.1 s they would only keep the processor busy; a day is
   far enough, and keeps every sum of milliseconds inside poll()'s int. */
static TV_UNUSED int tv_seconds(const char *s, unsigned int *ms) {
    unsigned int whole = 0, part = 0, scale = 100;
    int digits = 0;
    for(; *s >= '0' && *s <= '9'; s++, digits++) {
        whole = whole * 10 + (unsigned int)(*s - '0');
        if(whole > 86400) return -1;
    }
    if(*s == '.') {
        for(s++; *s >= '0' && *s <= '9'; s++, digits++) {
            part += (unsigned int)(*s - '0') * scale;   /* past the third decimal scale is 0 */
            scale /= 10;
        }
    }
    if(*s || !digits) return -1;
    whole = whole * 1000 + part;
    if(whole < 100 || whole > 86400000) return -1;
    *ms = whole;
    return 0;
}

/* milliseconds as a person writes them: "2s", "0.5s", "1.25s" */
static TV_UNUSED void tv_every(unsigned int ms, char *out, size_t size) {
    if(ms % 1000 == 0) snprintf(out, size, "%us", ms / 1000);
    else if(ms % 100 == 0) snprintf(out, size, "%u.%us", ms / 1000, ms / 100 % 10);
    else if(ms % 10 == 0) snprintf(out, size, "%u.%02us", ms / 1000, ms / 10 % 100);
    else snprintf(out, size, "%u.%03us", ms / 1000, ms % 1000);
}

/**** keys ****/

enum { TV_NONE = 0x100, TV_UP, TV_DOWN, TV_PGUP, TV_PGDN, TV_HOME, TV_END };

struct tv_keys {
    int state;                          /* 0, 1 after ESC, 2 inside ESC [ or ESC O */
    int number, more;                   /* the sequence's first number; a ; came */
};

/* the next byte from the terminal: a key (a byte, or TV_UP ...), TV_NONE for a byte of a sequence, or of one that
   means nothing here. A sequence may come in pieces over a serial line, so this keeps its place between reads. Only
   a control character breaks one off, and is then the key it is: Ctrl+C gets through whatever came before it. */
static TV_UNUSED int tv_key(struct tv_keys *k, unsigned char c) {
    if(k->state == 1) {
        k->state = 0;
        if(c == '[' || c == 'O') {
            k->state = 2;
            k->number = k->more = 0;
            return TV_NONE;
        }
        /* a lone Escape, then a key: that key */
    } else if(k->state == 2) {
        if(c >= '0' && c <= '9') {
            if(!k->more && k->number < 1000) k->number = k->number * 10 + (c - '0');
            return TV_NONE;
        }
        if(c == ';') {
            k->more = 1;                /* the modifiers that follow (ESC[1;5A): the key is what counts */
            return TV_NONE;
        }
        if(c < 0x20) k->state = 0;      /* below */
        else {
            k->state = 0;
            switch(c) {
            case 'A': return TV_UP;
            case 'B': return TV_DOWN;
            case 'H': return TV_HOME;
            case 'F': return TV_END;
            case '~':
                switch(k->number) {
                case 1: case 7: return TV_HOME;
                case 4: case 8: return TV_END;
                case 5: return TV_PGUP;
                case 6: return TV_PGDN;
                }
            }
            return TV_NONE;
        }
    }
    if(c == 0x1b) {
        k->state = 1;
        return TV_NONE;
    }
    return c;
}

/**** text from outside ****/

/* the bytes at s (n of them, at least 1): the length of the character they begin; 0 when they begin none; -1 when
   they may, once more bytes come. Characters as less takes them: valid UTF-8, without the C1 controls, which the
   console never draws. */
static TV_UNUSED int tv_utf8(const unsigned char *s, size_t n) {
    int len, i;
    if(s[0] < 0x80) return 1;
    if(s[0] < 0xC2 || s[0] > 0xF4) return 0;
    len = s[0] < 0xE0 ? 2 : s[0] < 0xF0 ? 3 : 4;
    for(i = 1; i < len && (size_t)i < n; i++)
        if((s[i] & 0xC0) != 0x80) return 0;
    if(n >= 2 && ((s[0] == 0xE0 && s[1] < 0xA0) || (s[0] == 0xED && s[1] >= 0xA0) ||    /* too long a form; a surrogate */
                  (s[0] == 0xF0 && s[1] < 0x90) || (s[0] == 0xF4 && s[1] >= 0x90) ||    /* too long; past U+10FFFF */
                  (s[0] == 0xC2 && s[1] < 0xA0))) return 0;
    return (size_t)len > n ? -1 : len;
}

/* text fit to show: every control byte, and every byte that is no character, a '?'; a character whole, or a '?' where
   the terminal has no UTF-8. A name cut short may end inside a character: that end is no character either. The
   length; out is always terminated (size at least 1). */
static TV_UNUSED size_t tv_clean(const char *s, size_t n, char *out, size_t size, int utf8) {
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0, used = 0;
    while(i < n && p[i] && used + 5 <= size) {
        int len = tv_utf8(p + i, n - i);
        if(len > 1 && utf8) {
            memcpy(out + used, p + i, (size_t)len);
            used += (size_t)len;
        } else out[used++] = (char)(len == 1 && p[i] >= 0x20 && p[i] != 0x7f ? p[i] : '?');
        i += len > 0 ? (size_t)len : 1;
    }
    out[used] = 0;
    return used;
}

/* the characters in n bytes of text that tv_clean() made */
static TV_UNUSED int tv_chars(const char *s, size_t n) {
    size_t i;
    int chars = 0;
    for(i = 0; i < n; i++)
        if(((unsigned char)s[i] & 0xC0) != 0x80) chars++;
    return chars;
}

/* s, text that tv_clean() made, cut to width characters: its length */
static TV_UNUSED size_t tv_cut(char *s, int width) {
    size_t i;
    int chars = 0;
    for(i = 0; s[i]; i++) {
        if(((unsigned char)s[i] & 0xC0) == 0x80) continue;
        if(chars == width) {
            s[i] = 0;
            return i;
        }
        chars++;
    }
    return i;
}

/**** the screen: one buffer, one write ****/

static int tv_rows, tv_cols;            /* the terminal's size, tv_resize() */
static char *tv_out;
static size_t tv_out_len, tv_out_cap;
static int tv_lost;                     /* no memory for some of a screen: all of it goes again next time */
static char *tv_shown[TV_MAX_ROWS];     /* what each row shows as it was written; NULL: not known */
static size_t tv_shown_len[TV_MAX_ROWS];

static TV_UNUSED void tv_add(const char *s, size_t n) {
    if(tv_out_len + n > tv_out_cap) {
        size_t cap = tv_out_cap ? tv_out_cap * 2 : 16384;
        char *p;
        while(cap < tv_out_len + n) cap *= 2;
        p = realloc(tv_out, cap);
        if(!p) {
            tv_lost = 1;
            return;
        }
        tv_out = p;
        tv_out_cap = cap;
    }
    memcpy(tv_out + tv_out_len, s, n);
    tv_out_len += n;
}

static TV_UNUSED void tv_str(const char *s) {
    tv_add(s, strlen(s));
}

static TV_UNUSED void tv_move(int row, int col) {       /* 0-based */
    char seq[24];
    snprintf(seq, sizeof(seq), "\x1b[%d;%dH", row + 1, col + 1);
    tv_str(seq);
}

static TV_UNUSED void tv_forget(void) {
    int y;
    for(y = 0; y < TV_MAX_ROWS; y++) {
        free(tv_shown[y]);
        tv_shown[y] = NULL;
        tv_shown_len[y] = 0;
    }
}

/* row y shows these bytes, which fit it (a row must not reach the last column of the bottom row, where a terminal may
   scroll) and leave the colours as they were: sent only when the row shows anything else. The erase comes first: after
   a text of the row's whole width the cursor is past it, and some terminals would erase its last character. */
static TV_UNUSED void tv_row(int y, const char *s, size_t n) {
    if(y < 0 || y >= TV_MAX_ROWS) return;
    if(tv_shown[y] && tv_shown_len[y] == n && !memcmp(tv_shown[y], s, n)) return;
    tv_move(y, 0);
    tv_str("\x1b[K");
    tv_add(s, n);
    free(tv_shown[y]);
    tv_shown[y] = malloc(n ? n : 1);    /* without memory the row is sent again next time */
    if(tv_shown[y]) memcpy(tv_shown[y], s, n);
    tv_shown_len[y] = n;
}

/* TTY_GETSIZE's answer (below 1: not known, 80x25): 1 when the size is new, when everything is drawn again */
static TV_UNUSED int tv_resize(int size) {
    int c = size > 0 ? (size >> 16) & 0xffff : 80, r = size > 0 ? size & 0xffff : 25;
    if(c < 16) c = 16;
    if(c > TV_MAX_COLS) c = TV_MAX_COLS;
    if(r < 3) r = 3;
    if(r > TV_MAX_ROWS) r = TV_MAX_ROWS;
    if(c == tv_cols && r == tv_rows) return 0;
    tv_cols = c;
    tv_rows = r;
    tv_forget();
    tv_str("\x1b[0m\x1b[2J");
    return 1;
}

static TV_UNUSED void tv_flush(void) {
    size_t done = 0;
    while(done < tv_out_len) {
        ssize_t n = write(1, tv_out + done, tv_out_len - done);
        if(n < 0 && errno == EINTR) continue;
        if(n <= 0) break;
        done += (size_t)n;
    }
    tv_out_len = 0;
    if(tv_lost) {
        tv_lost = 0;
        tv_forget();
    }
}

#endif
