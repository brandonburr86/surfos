/*
SurfOS Undertow: the line typed at the undertow> prompt
----------------------
File: edit.c  Date: 10/11/26
----------------------
edit.h gives the keys. The line is kept on the prompt's row: the prompt, then a
window onto the line that moves sideways to keep the cursor in it, never
reaching the row's last column (the console wraps the VT100 way: a character
there would leave a wrap pending, and the next one would start a new row). So
the cursor moves only with CR and ESC[nC, and nothing the editor draws ever
scrolls the screen or touches the view above. The row is drawn again whole
(CR, the prompt, the window, ESC[K) after a change, unless the change is a
character typed at the end of the line, which is just written.

As surfsh's editor does, keys that arrive together (a paste) are all handled
before the row is drawn, so the editor keeps up with the console's 256-byte
input queue; Enter is '\r' from a keyboard or a terminal and '\n' from a test
harness, and a CR's LF (CR LF) or NUL (a telnet client's CR NUL) is the same
Enter, whenever it comes: with the CR or at the start of the next line. A lone Esc followed within 250 ms by '[' or 'O'
is a key's escape sequence split by the terminal; later, the keys are typed.
Typed text is teal and the prompt light blue, as in surfsh, where the console
has colours.
*/

#include <stdlib.h>
#include <string.h>
#include <surf.h>
#include "undertow.h"
#include "edit.h"

#define CTRL(c)   ((c) & 0x1f)
#define C_PROMPT  "\033[0;1;34m"        /* light blue: the SurfOS prompt's colour */
#define C_TYPED   "\033[0;36m"          /* teal: what is typed */
#define C_RESET   "\033[0m"
#define SPLIT_MS  250                   /* an escape sequence split in two: its pieces this close */

enum {
    KEY_NONE = 0x100, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END, KEY_DEL,
    KEY_PGUP, KEY_PGDN, KEY_EOF
};

struct ut_editor {
    struct ut_edit_io io;
    /* the line */
    char line[UT_COMMAND_MAX + 1];
    size_t len, pos;                    /* bytes: the line's, the cursor's */
    int too_long;                       /* characters were refused: Enter refuses the line */
    /* the row */
    unsigned int cols;                  /* the terminal's columns */
    int utf8, colour;
    const char *prompt;
    unsigned int pcols;                 /* the prompt's columns */
    size_t off;                         /* the first byte of the line in the window */
    int dirty;                          /* the row on the screen is not the line any more */
    /* output, written at once */
    char obuf[2048];
    size_t olen;
    /* input */
    unsigned char ibuf[256];            /* the console's input queue (TTY_INPUT_SIZE) at once */
    size_t ipos, ilen;
    unsigned int lone_esc_at;           /* uptime when a lone Esc came, 0: none */
    int after_cr;                       /* the last key was CR: an LF or NUL now is the same Enter,
                                           in this line or at the start of the next */
    /* the lines typed before */
    char *hist[UT_EDIT_HISTORY];
    int hist_count;                     /* lines ever added: hist[i % UT_EDIT_HISTORY] */
    int hist_pos;                       /* the one shown; hist_count: the line being typed */
    char typed[UT_COMMAND_MAX + 1];     /* the line being typed, while one from before is shown */
    size_t typed_len;
    int typed_too_long;
};

struct ut_editor *ut_edit_new(const struct ut_edit_io *io) {
    struct ut_editor *e = calloc(1, sizeof(*e));
    if(!e) return NULL;
    e->io = *io;
    return e;
}

void ut_edit_free(struct ut_editor *e) {
    int i;
    if(!e) return;
    for(i = 0; i < UT_EDIT_HISTORY; i++) free(e->hist[i]);
    free(e);
}

/**** output ****/

static void flush_out(struct ut_editor *e) {
    if(e->olen) e->io.write(e->io.ctx, e->obuf, e->olen);
    e->olen = 0;
}

static void put(struct ut_editor *e, const char *s, size_t n) {
    while(n) {
        size_t k = sizeof(e->obuf) - e->olen;
        if(!k) {
            flush_out(e);
            continue;
        }
        if(k > n) k = n;
        memcpy(e->obuf + e->olen, s, k);
        e->olen += k;
        s += k;
        n -= k;
    }
}

static void puts_(struct ut_editor *e, const char *s) {
    put(e, s, strlen(s));
}

static void colour(struct ut_editor *e, const char *seq) {
    if(e->colour) puts_(e, seq);
}

/**** the window ****/

static int is_cont(int c) {
    return ((unsigned char)c & 0xc0) == 0x80;
}

static size_t prev_char(const struct ut_editor *e, size_t p) {
    if(p > 0) p--;
    while(p > 0 && is_cont(e->line[p])) p--;
    return p;
}

static size_t next_char(const struct ut_editor *e, size_t p) {
    if(p < e->len) p++;
    while(p < e->len && is_cont(e->line[p])) p++;
    return p;
}

/* the columns of line[a..b): one a character */
static unsigned int cols_between(const struct ut_editor *e, size_t a, size_t b) {
    unsigned int n = 0;
    for(; a < b; a++) if(!is_cont(e->line[a])) n++;
    return n;
}

/* the window's columns: the row less the prompt and the last column */
static unsigned int window(const struct ut_editor *e) {
    return e->cols > e->pcols + 2 ? e->cols - e->pcols - 1 : 1;
}

/* the cursor in the window, at most at its last column but one */
static void keep_in_sight(struct ut_editor *e) {
    unsigned int w = window(e), n;
    if(cols_between(e, 0, e->len) < w) {
        e->off = 0;
        return;
    }
    if(e->pos < e->off) {
        n = w / 4;
        e->off = e->pos;
        while(n-- && e->off > 0) e->off = prev_char(e, e->off);
    } else if(cols_between(e, e->off, e->pos) + 1 > w) {
        n = w - w / 4;
        e->off = e->pos;
        while(n-- && e->off > 0) e->off = prev_char(e, e->off);
    }
}

/* the row again: CR, the prompt, the window onto the line, ESC[K, and the cursor in its column */
static void draw(struct ut_editor *e) {
    unsigned int w = window(e), shown = 0;
    size_t end = e->off;
    char move[16];
    keep_in_sight(e);
    end = e->off;
    while(end < e->len && shown < w) {
        end = next_char(e, end);
        shown++;
    }
    puts_(e, "\r");
    colour(e, C_PROMPT);
    puts_(e, e->prompt);
    colour(e, C_TYPED);
    put(e, e->line + e->off, end - e->off);
    puts_(e, "\033[K\r");
    shown = e->pcols + cols_between(e, e->off, e->pos);
    if(shown) {
        snprintf(move, sizeof(move), "\033[%uC", shown);
        puts_(e, move);
    }
    e->dirty = 0;
}

/* the start of a fresh, empty row, wherever the last output left the cursor (surfsh's way: a row
   of spaces to the edge, CR and ESC[K, so a half-written row above is kept) */
static void fresh_row(struct ut_editor *e) {
    unsigned int i;
    colour(e, C_RESET);
    puts_(e, "\033[?25h");
    for(i = 0; i < e->cols; i++) put(e, " ", 1);
    puts_(e, "\r\033[K");
}

/**** keys ****/

static int next_byte(struct ut_editor *e) {
    if(e->ipos == e->ilen) {
        long n;
        if(e->dirty) draw(e);                   /* the row is brought up to date before waiting */
        flush_out(e);
        n = e->io.read(e->io.ctx, e->ibuf, sizeof(e->ibuf));
        if(n <= 0) return -1;
        e->ipos = 0;
        e->ilen = (size_t)n;
    }
    return e->ibuf[e->ipos++];
}

/* The rest of ESC [ or ESC O: parameter and intermediate bytes (0x20-0x3F) up to the final byte
   (0x40-0x7E). The first parameter is the key's number (ESC[3~, and ESC[3;5~ with Ctrl held: the
   key is what counts). A private sequence (ESC[?...) or one with intermediate bytes is no key;
   another byte ends the sequence and is a key again. */
static int sequence(struct ut_editor *e) {
    int c, num = 0, first = 1, other = 0;
    for(;;) {
        c = next_byte(e);
        if(c < 0) return KEY_EOF;
        if(c < 0x20 || c > 0x7e) {
            e->ipos--;
            return KEY_NONE;
        }
        if(c >= 0x40) break;
        if(c >= '0' && c <= '9') {
            if(first && num < 1000) num = num * 10 + (c - '0');
        } else if(c == ';' || c == ':') {
            first = 0;
        } else {
            other = 1;
        }
    }
    if(other) return KEY_NONE;
    switch(c) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    case '~':
        switch(num) {
        case 1: case 7: return KEY_HOME;
        case 4: case 8: return KEY_END;
        case 3: return KEY_DEL;
        case 5: return KEY_PGUP;
        case 6: return KEY_PGDN;
        }
    }
    return KEY_NONE;
}

static int get_key(struct ut_editor *e) {
    int c = next_byte(e);
    if(c < 0) return KEY_EOF;
    if((c == '[' || c == 'O') && e->lone_esc_at && ut_now() - e->lone_esc_at < SPLIT_MS) {
        e->lone_esc_at = 0;
        return sequence(e);
    }
    e->lone_esc_at = 0;
    if(c != 0x1b) return c;
    if(e->ipos == e->ilen) {                    /* nothing came with it: Esc, or a split sequence */
        e->lone_esc_at = ut_now();
        if(!e->lone_esc_at) e->lone_esc_at = 1;
        return KEY_NONE;
    }
    c = next_byte(e);
    if(c == '[' || c == 'O') return sequence(e);
    e->ipos--;                                  /* Esc and another key: nothing, then that key */
    return KEY_NONE;
}

/**** changing the line ****/

static void insert(struct ut_editor *e, const char *s, size_t n) {
    unsigned int w = window(e);
    if(e->len + n > UT_COMMAND_MAX) {
        e->too_long = 1;
        return;
    }
    memmove(e->line + e->pos + n, e->line + e->pos, e->len - e->pos);
    memcpy(e->line + e->pos, s, n);
    e->len += n;
    e->pos += n;
    if(!e->dirty && e->pos == e->len && cols_between(e, e->off, e->pos) < w) put(e, s, n);  /* at the end: just write it */
    else e->dirty = 1;
}

/* a character of more than one byte from a terminal: the lead byte and what follows it; only a
   whole, safe character, and only where the terminal draws UTF-8 */
static void insert_utf8(struct ut_editor *e, int lead) {
    unsigned char ch[4];
    unsigned int cp;
    size_t need = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : 2, n = 1;
    ch[0] = (unsigned char)lead;
    while(n < need) {
        int c = next_byte(e);
        if(c < 0) return;
        if(!is_cont(c)) {                       /* broken: dropped, and that byte is a key again */
            e->ipos--;
            return;
        }
        ch[n++] = (unsigned char)c;
    }
    if(ut_utf8_decode(ch, n, &cp) != n || ut_unsafe_char(cp) || !e->utf8) return;
    insert(e, (const char *)ch, n);
}

static void delete_range(struct ut_editor *e, size_t a, size_t b) {
    if(b <= a) return;
    memmove(e->line + a, e->line + b, e->len - b);
    e->len -= b - a;
    e->pos = a;
    if(!e->len) e->too_long = 0;                /* all of it is gone: the line starts again */
    e->dirty = 1;
}

static void move_to(struct ut_editor *e, size_t p) {
    if(p == e->pos) return;
    e->pos = p;
    e->dirty = 1;
}

/**** the lines typed before ****/

static int hist_oldest(const struct ut_editor *e) {
    return e->hist_count > UT_EDIT_HISTORY ? e->hist_count - UT_EDIT_HISTORY : 0;
}

static void history_add(struct ut_editor *e) {
    char *copy;
    if(!e->len || e->too_long) return;
    if(e->hist_count && !strcmp(e->hist[(e->hist_count - 1) % UT_EDIT_HISTORY], e->line)) return;
    copy = malloc(e->len + 1);
    if(!copy) return;                           /* no memory: just not kept */
    memcpy(copy, e->line, e->len + 1);
    free(e->hist[e->hist_count % UT_EDIT_HISTORY]);
    e->hist[e->hist_count % UT_EDIT_HISTORY] = copy;
    e->hist_count++;
}

static void history_go(struct ut_editor *e, int to) {
    if(to < hist_oldest(e) || to > e->hist_count || to == e->hist_pos) return;
    if(e->hist_pos == e->hist_count) {          /* keep what was being typed */
        memcpy(e->typed, e->line, e->len);
        e->typed_len = e->len;
        e->typed_too_long = e->too_long;
    }
    e->hist_pos = to;
    if(to == e->hist_count) {
        memcpy(e->line, e->typed, e->typed_len);
        e->len = e->typed_len;
        e->too_long = e->typed_too_long;
    } else {
        const char *s = e->hist[to % UT_EDIT_HISTORY];
        e->len = strlen(s);
        memcpy(e->line, s, e->len);
        e->too_long = 0;
    }
    e->line[e->len] = 0;
    e->pos = e->len;
    e->dirty = 1;
}

/**** a line ****/

/* the row is done: the cursor after the line (its end in sight), say, and the next row */
static void leave(struct ut_editor *e, const char *say) {
    if(e->pos != e->len || e->dirty) {
        e->pos = e->len;
        draw(e);
    }
    colour(e, C_RESET);
    if(say) puts_(e, say);
    puts_(e, "\r\n");
}

/* without raw mode: the prompt, and a line as the console's own line editing gives it */
static int plain_line(struct ut_editor *e, char *buf) {
    size_t n = 0;
    int too_long = 0, c;
    puts_(e, e->prompt);
    flush_out(e);
    e->ipos = e->ilen = 0;
    for(;;) {
        c = next_byte(e);
        if(c < 0) {
            if(!n && !too_long) return UT_EDIT_EOF;
            break;
        }
        if(c == '\n') break;
        if(n == UT_COMMAND_MAX) too_long = 1;
        else buf[n++] = (char)c;
    }
    if(n && buf[n - 1] == '\r') n--;
    buf[n] = 0;
    e->ipos = e->ilen = 0;
    return too_long ? UT_EDIT_LONG : (int)n;
}

/* keys typed before the prompt (during a load): read and dropped, and the rest of a paste skipped */
static void drop_typed(struct ut_editor *e) {
    long n, dropped = 0;
    while(e->io.ready(e->io.ctx) && (n = e->io.read(e->io.ctx, e->ibuf, sizeof(e->ibuf))) > 0) dropped += n;
    if(dropped) ttyctl(TTY_SKIP, 1);
    e->ipos = e->ilen = 0;
}

int ut_edit_line(struct ut_editor *e, const struct ut_term *t, int drop, char *buf) {
    int key, result;
    e->cols = t->cols ? t->cols : 80;
    if(e->cols > 1024) e->cols = 1024;
    e->utf8 = t->utf8;
    e->colour = t->color;
    e->prompt = e->cols >= 24 ? "undertow> " : "> ";
    e->pcols = (unsigned int)strlen(e->prompt);
    e->olen = 0;
    if(ttyctl(TTY_RAW, 1) < 0) return plain_line(e, buf);
    e->ipos = e->ilen = 0;
    e->lone_esc_at = 0;
    if(drop) drop_typed(e);
    e->len = e->pos = e->off = 0;
    e->line[0] = 0;
    e->too_long = 0;
    e->hist_pos = e->hist_count;
    fresh_row(e);
    draw(e);
    for(;;) {
        key = get_key(e);
        if((key == '\n' || key == 0) && e->after_cr) {     /* the end of a CR LF or CR NUL */
            e->after_cr = 0;
            continue;
        }
        e->after_cr = key == '\r';
        switch(key) {
        case '\r':
        case '\n':
            leave(e, NULL);
            result = e->too_long ? UT_EDIT_LONG : (int)e->len;
            e->line[e->len] = 0;
            history_add(e);
            goto done;
        case KEY_EOF:
            leave(e, NULL);
            result = UT_EDIT_EOF;
            goto done;
        case CTRL('c'):
            leave(e, "^C");
            result = UT_EDIT_QUIT;
            goto done;
        case CTRL('d'):
            if(e->len) {
                delete_range(e, e->pos, next_char(e, e->pos));
                break;
            }
            leave(e, "quit");
            result = UT_EDIT_EOF;
            goto done;
        case KEY_PGDN:
        case KEY_PGUP:
            if(e->len) break;
            leave(e, key == KEY_PGDN ? "next" : "prev");
            result = key == KEY_PGDN ? UT_EDIT_NEXT : UT_EDIT_PREV;
            goto done;
        case CTRL('a'):
        case KEY_HOME:
            move_to(e, 0);
            break;
        case CTRL('e'):
        case KEY_END:
            move_to(e, e->len);
            break;
        case KEY_LEFT:
            move_to(e, prev_char(e, e->pos));
            break;
        case KEY_RIGHT:
            move_to(e, next_char(e, e->pos));
            break;
        case 0x7f:
        case 0x08:
            delete_range(e, prev_char(e, e->pos), e->pos);
            break;
        case KEY_DEL:
            delete_range(e, e->pos, next_char(e, e->pos));
            break;
        case CTRL('u'):
            e->too_long = 0;
            delete_range(e, 0, e->len);
            break;
        case CTRL('k'):
            delete_range(e, e->pos, e->len);
            break;
        case CTRL('w'): {
            size_t p = e->pos;
            while(p > 0 && e->line[p - 1] == ' ') p--;
            while(p > 0 && e->line[p - 1] != ' ') p--;
            delete_range(e, p, e->pos);
            break;
        }
        case KEY_UP:
            history_go(e, e->hist_pos - 1);
            break;
        case KEY_DOWN:
            history_go(e, e->hist_pos + 1);
            break;
        default:
            if(key >= 0x20 && key < 0x7f) {
                char ch = (char)key;
                insert(e, &ch, 1);
            } else if(key >= 0xc2 && key <= 0xf4) {
                insert_utf8(e, key);
            }
            break;                              /* other control characters and keys: nothing */
        }
    }
done:
    flush_out(e);
    if(e->after_cr && e->ipos < e->ilen && (e->ibuf[e->ipos] == '\n' || !e->ibuf[e->ipos])) {
        e->ipos++;                              /* the Enter's own LF or NUL: no paste after it */
        e->after_cr = 0;
    }
    if(e->ipos < e->ilen) ttyctl(TTY_SKIP, 1);  /* keys after the Enter: the rest of a paste is skipped */
    ttyctl(TTY_RAW, 0);                         /* the console drops what it still holds, */
    e->ipos = e->ilen = 0;                      /* and so does the editor: no type-ahead */
    memcpy(buf, e->line, e->len + 1);
    if(result == UT_EDIT_LONG) buf[0] = 0;
    return result;
}
