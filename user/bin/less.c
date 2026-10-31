/*
SurfOS user program: less
--------------------
File: less.c    Date: 10/25/26
--------------------
less [file]: a file, or the standard input, a screenful at a time (track A, A9).
The text is read whole first (8 MB at most: the rest is left unread, and the
bottom row says so). The keys come from /dev/tty, so less can end a pipeline
(dmesg | less). less shows the text on the alternate screen in raw mode and gives
the shell's screen back when it ends (TTY_CAP_ALTSCREEN, include/surfos/tty.h;
the console gives it back for a less that is killed too). The bottom row says
where the screen is in the text.

Lines longer than the screen are cut, a '>' in the last column saying so; Right
and Left move sideways by half a screen. A TAB moves to the next multiple of 8,
UTF-8 shows as characters, one cell each ('?' where TTY_CAPS has no
TTY_CAP_UTF8), other control characters show as ^X in inverse video, and bytes
that are not UTF-8 as an inverse '?'. A CR before a line's newline is dropped.

Keys: Space, f, PgDn, ^F, ^V the next screenful; b, PgUp, ^B the one before;
Enter, j, e, Down, ^N, ^E one line on; k, y, Up, ^P, ^Y one line back; d, ^D and
u, ^U half a screen on and back; g, <, Home the start; G, >, End the end. / and
? search forward from the top line and backward from the line above it for
text (not a pattern: case counts only when the text has capitals; Enter alone
searches for the last text again); the line found goes to the top, and every
match on the screen shows in inverse video. n finds the next match, N the one
the other way. Right and Left move sideways, ^L draws the screen again, h shows
the keys, q, Q and ^C end.

With no terminal on its standard output (a pipe, a file), less copies the text
there, as cat does.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <surf.h>

#define MAX_TEXT   (8 * 1024 * 1024)
#define MAX_COLS   512
#define MAX_ROWS   256
#define TAB        8
#define CTRL(c)    ((c) & 0x1f)

enum {
    KEY_NONE = 0x100, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END,
    KEY_PGUP, KEY_PGDN, KEY_ESC, KEY_EOF
};

static char *text;              /* the whole text */
static size_t text_len;
static size_t *starts;          /* where each line starts */
static int nlines;
static int cut_short;           /* MAX_TEXT reached, or no memory for more: the rest was not read */
static const char *name;        /* the file, or "standard input" */

static int rows, cols, text_rows, utf8;
static int top, left;           /* the first line and the first column shown */
static char pattern[128];       /* the last search: its text (in lower case when case does not count) */
static size_t plen;
static int folded, backward;    /* case does not count; the last search went backward */
static int found = -1;          /* the line the last search found */
static char msg[160];           /* for the bottom row, until the next key */
static const char *asking;      /* while a search is typed: "/" or "?" and the text so far */
static int keys = -1;           /* /dev/tty */

/**** output: one buffer per screen, one write ****/

static char *out;
static size_t out_len, out_cap;

static void out_add(const char *s, size_t n) {
    if(out_len + n > out_cap) {
        size_t cap = out_cap ? out_cap * 2 : 16384;
        char *p;
        while(cap < out_len + n) cap *= 2;
        p = realloc(out, cap);
        if(!p) return;                  /* this screen is incomplete; ^L draws again */
        out = p;
        out_cap = cap;
    }
    memcpy(out + out_len, s, n);
    out_len += n;
}

static void out_str(const char *s) {
    out_add(s, strlen(s));
}

static void out_flush(void) {
    size_t done = 0;
    while(done < out_len) {
        ssize_t n = write(1, out + done, out_len - done);
        if(n < 0 && errno == EINTR) continue;
        if(n <= 0) break;
        done += (size_t)n;
    }
    out_len = 0;
}

static void move_to(int row, int col) {         /* 0-based */
    char seq[24];
    snprintf(seq, sizeof(seq), "\x1b[%d;%dH", row + 1, col + 1);
    out_str(seq);
}

/**** the terminal ****/

static int raw_mode;

/* the alternate screen cleared, then left: the shell's screen back, or on a terminal without one a clear screen */
static void restore_terminal(void) {
    static const char bye[] = "\x1b[0m\x1b[2J\x1b[H\x1b[?25h\x1b[?1049l";
    if(!raw_mode) return;
    raw_mode = 0;
    write(1, bye, sizeof(bye) - 1);
    ttyctl(TTY_RAW, 0);
}

static char *shown[MAX_ROWS];   /* what each row shows, as it was written; NULL: not known */
static size_t shown_len[MAX_ROWS];

static void forget_screen(void) {
    int y;
    for(y = 0; y < MAX_ROWS; y++) {
        free(shown[y]);
        shown[y] = NULL;
    }
}

static void screen_size(void) {
    int r = ttyctl(TTY_GETSIZE, 0), c, n;
    c = r > 0 ? (r >> 16) & 0xffff : 80;
    n = r > 0 ? r & 0xffff : 25;
    if(c < 16) c = 16;
    if(c > MAX_COLS) c = MAX_COLS;
    if(n < 3) n = 3;
    if(n > MAX_ROWS) n = MAX_ROWS;
    if(c != cols || n != rows) {                /* a new size (a pty's): everything again */
        forget_screen();
        out_str("\x1b[0m\x1b[2J");
    }
    cols = c;
    rows = n;
    text_rows = rows - 1;
}

/**** keys ****/

static unsigned char inbuf[64];
static int inpos, inlen;
static unsigned int lone_esc_at;    /* uptime of the last lone ESC, to mend a sequence split across reads */

static int next_byte(void) {
    if(inpos == inlen) {
        ssize_t n = read(keys, inbuf, sizeof(inbuf));
        if(n < 0 && errno == EINTR) return next_byte();
        if(n <= 0) return -1;
        inpos = 0;
        inlen = (int)n;
    }
    return inbuf[inpos++];
}

/* the rest of ESC [ or ESC O: the parameter digits and the final letter */
static int sequence(void) {
    int c, num = 0;
    for(;;) {
        c = next_byte();
        if(c < 0) return KEY_EOF;
        if(c >= '0' && c <= '9') { num = num * 10 + (c - '0'); continue; }
        if(c == ';') { num = 0; continue; }     /* modifiers (ESC[1;5C): the key is what counts */
        break;
    }
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
        case 5: return KEY_PGUP;
        case 6: return KEY_PGDN;
        }
    }
    return KEY_NONE;
}

static int get_key(void) {
    int c = next_byte();
    if(c < 0) return KEY_EOF;
    if((c == '[' || c == 'O') && lone_esc_at && uptime() - lone_esc_at < 250) {
        lone_esc_at = 0;                        /* "ESC" and "[A" came in two reads: still an arrow */
        return sequence();
    }
    lone_esc_at = 0;
    if(c != 0x1b) return c;
    if(inpos == inlen) {                        /* nothing came with it: the Escape key */
        lone_esc_at = uptime();
        if(!lone_esc_at) lone_esc_at = 1;
        return KEY_ESC;
    }
    c = next_byte();
    if(c == '[' || c == 'O') return sequence();
    return KEY_NONE;                            /* Alt and a key (A4): nothing of less's */
}

/**** the text ****/

static int read_text(int fd) {
    size_t cap = 0;
    for(;;) {
        ssize_t n;
        if(text_len == cap) {
            size_t more = cap ? cap * 2 : 65536;
            char *p;
            if(more > MAX_TEXT) more = MAX_TEXT;
            if(more == cap || !(p = realloc(text, more))) {
                cut_short = 1;
                return 0;
            }
            text = p;
            cap = more;
        }
        n = read(fd, text + text_len, cap - text_len);
        if(n < 0 && errno == EINTR) continue;
        if(n < 0) return -1;
        if(n == 0) return 0;
        text_len += (size_t)n;
    }
}

static int index_lines(void) {
    size_t i, cap = 1024;
    if(!text_len) return 0;
    starts = malloc(cap * sizeof(*starts));
    if(!starts) return -1;
    starts[nlines++] = 0;
    for(i = 0; i + 1 < text_len; i++) {
        if(text[i] != '\n') continue;
        if((size_t)nlines == cap) {
            size_t *p = realloc(starts, cap * 2 * sizeof(*starts));
            if(!p) return -1;
            starts = p;
            cap *= 2;
        }
        starts[nlines++] = i + 1;
    }
    return 0;
}

/* where line k's text ends: its newline, and a CR before that, left out */
static size_t line_end(int k) {
    size_t e = k + 1 < nlines ? starts[k + 1] - 1 : text_len;
    if(k + 1 >= nlines && e > starts[k] && text[e - 1] == '\n') e--;
    if(e > starts[k] && text[e - 1] == '\r') e--;
    return e;
}

/* the length of the UTF-8 character at s (n bytes there); 0 for bytes that are not one, and for the C1
   controls U+0080-U+009F, which the console never draws */
static int utf8_len(const unsigned char *s, size_t n) {
    int len, i;
    if(s[0] < 0xC2 || s[0] > 0xF4) return 0;
    len = s[0] < 0xE0 ? 2 : s[0] < 0xF0 ? 3 : 4;
    if((size_t)len > n) return 0;
    for(i = 1; i < len; i++)
        if((s[i] & 0xC0) != 0x80) return 0;
    if((s[0] == 0xE0 && s[1] < 0xA0) || (s[0] == 0xED && s[1] >= 0xA0) ||    /* too long a form; a surrogate */
       (s[0] == 0xF0 && s[1] < 0x90) || (s[0] == 0xF4 && s[1] >= 0x90) ||    /* too long; past U+10FFFF */
       (s[0] == 0xC2 && s[1] < 0xA0)) return 0;
    return len;
}

/* the search's text at s[p] (n bytes there) */
static int match_at(const unsigned char *s, size_t n, size_t p) {
    size_t i;
    if(!plen || n - p < plen) return 0;
    for(i = 0; i < plen; i++) {
        unsigned char c = s[p + i];
        if(folded && c < 0x80) c = (unsigned char)tolower(c);
        if(c != (unsigned char)pattern[i]) return 0;
    }
    return 1;
}

static int line_has(int k) {
    const unsigned char *s = (const unsigned char *)text + starts[k];
    size_t n = line_end(k) - starts[k], p;
    for(p = 0; p + plen <= n; p++)
        if(match_at(s, n, p)) return 1;
    return 0;
}

/**** the screen ****/

struct cell {
    char b[4];                  /* what it shows: a character's bytes */
    unsigned char n, inverse;
};

static struct cell cells[MAX_COLS];

static void blank_cells(void) {
    int x;
    for(x = 0; x < cols; x++) {
        cells[x].b[0] = ' ';
        cells[x].n = 1;
        cells[x].inverse = 0;
    }
}

/* line k's cells from column left on, matches of the search in inverse video */
static void line_cells(int k) {
    const unsigned char *s = (const unsigned char *)text + starts[k];
    size_t n = line_end(k) - starts[k], p = 0, match_end = 0;
    int col = 0, more = 0;
    blank_cells();
    while(p < n && !more) {
        char b[4];
        int w = 1, len = 1, bn = 1, inv, i, x;
        if(p >= match_end && match_at(s, n, p)) match_end = p + plen;
        inv = p < match_end;
        if(s[p] == '\t') {
            w = TAB - col % TAB;
            b[0] = ' ';
        } else if(s[p] < 0x20 || s[p] == 0x7f) {            /* ^X */
            w = 2;
            b[0] = (char)(s[p] ^ 0x40);
            inv = 1;
        } else if(s[p] < 0x80) {
            b[0] = (char)s[p];
        } else if((len = utf8_len(s + p, n - p)) > 0) {
            if(utf8) {
                memcpy(b, s + p, (size_t)len);
                bn = len;
            } else b[0] = '?';
        } else {                                            /* a byte that is not UTF-8 */
            len = 1;
            b[0] = '?';
            inv = 1;
        }
        for(i = 0; i < w; i++, col++) {
            x = col - left;
            if(x >= cols) {
                more = 1;
                break;
            }
            if(x < 0) continue;
            if(w == 2) {                                    /* ^ and the letter */
                cells[x].b[0] = i ? b[0] : '^';
                cells[x].n = 1;
            } else if(i) {                                  /* the rest of a TAB */
                cells[x].b[0] = ' ';
                cells[x].n = 1;
            } else {
                memcpy(cells[x].b, b, (size_t)bn);
                cells[x].n = (unsigned char)bn;
            }
            cells[x].inverse = (unsigned char)inv;
        }
        p += (size_t)len;
        if(!more && col >= left + cols && p < n) more = 1;
    }
    if(more) {                                              /* it goes on */
        cells[cols - 1].b[0] = '>';
        cells[cols - 1].n = 1;
        cells[cols - 1].inverse = 0;
    }
}

/* the cells of row y to the terminal when the row does not show them already; the bottom row's last column is
   never written (no scroll, whatever the terminal) */
static void put_row(int y) {
    char line[MAX_COLS * 8 + 32];
    size_t len = 0;
    int x, end = y == rows - 1 ? cols - 1 : cols, inv = 0, used = 0;
    for(x = 0; x < end; x++)
        if(cells[x].inverse || cells[x].n != 1 || cells[x].b[0] != ' ') used = x + 1;
    for(x = 0; x < used; x++) {
        if(cells[x].inverse != inv) {
            inv = cells[x].inverse;
            memcpy(line + len, inv ? "\x1b[7m" : "\x1b[0m", 4);
            len += 4;
        }
        memcpy(line + len, cells[x].b, cells[x].n);
        len += cells[x].n;
    }
    if(inv) {
        memcpy(line + len, "\x1b[0m", 4);
        len += 4;
    }
    if(used < end) {
        memcpy(line + len, "\x1b[K", 3);
        len += 3;
    }
    if(shown[y] && shown_len[y] == len && !memcmp(shown[y], line, len)) return;
    move_to(y, 0);
    out_add(line, len);
    free(shown[y]);
    shown[y] = malloc(len ? len : 1);
    if(shown[y]) memcpy(shown[y], line, len);
    shown_len[y] = len;
}

/* text into the cells from column x on, in inverse video or not (ASCII, as less writes it) */
static int put_text(int x, const char *s, int inverse) {
    for(; *s && x < cols - 1; s++, x++) {
        cells[x].b[0] = *s;
        cells[x].n = 1;
        cells[x].inverse = (unsigned char)inverse;
    }
    return x;
}

static int max_top(void) {
    return nlines > text_rows ? nlines - text_rows : 0;
}

static void draw(void) {
    char status[200];
    int y, x, last;
    screen_size();
    if(top > max_top()) top = max_top();
    if(top < 0) top = 0;
    if(left < 0) left = 0;
    for(y = 0; y < text_rows; y++) {
        if(top + y < nlines) line_cells(top + y);
        else {
            blank_cells();
            cells[0].b[0] = '~';                        /* past the end */
        }
        put_row(y);
    }
    last = top + text_rows < nlines ? top + text_rows : nlines;
    blank_cells();
    if(asking) x = put_text(0, asking, 0);
    else {
        if(msg[0]) snprintf(status, sizeof(status), "%s", msg);
        else if(!nlines) snprintf(status, sizeof(status), "%s (empty)%s", name, cut_short ? ", not read whole" : "");
        else snprintf(status, sizeof(status), "%s  lines %d-%d of %d%s  %s%s", name, top + 1, last, nlines,
                      cut_short ? "+" : "", last == nlines ? "(END)" : "", cut_short && last == nlines ?
                      " of the first 8 MB" : "");
        if(!msg[0] && last < nlines) {
            size_t n = strlen(status);
            snprintf(status + n, sizeof(status) - n, "%d%%  h: keys", last * 100 / nlines);  /* 8 MB: 8M lines at most */
        }
        x = put_text(0, status, 1);
    }
    put_row(rows - 1);
    move_to(rows - 1, x);
    out_flush();
}

static const char *const help_text[] = {
    "less: the keys",
    "",
    "  Space f PgDn ^F   the next screen     b PgUp ^B      the screen before",
    "  Enter j Down ^N   one line on         k y Up ^P      one line back",
    "  d ^D              half a screen on    u ^U           half a screen back",
    "  g < Home          the start           G > End        the end",
    "  /text             search forward      ?text          search backward",
    "  n                 the next match      N              the match the other way",
    "  Right Left        sideways            ^L             draw the screen again",
    "  q Q ^C            end",
    "",
    "A search finds text, not a pattern; case counts only when the text has capitals.",
    "",
    "Any key goes back to the text."
};

static void help(void) {
    unsigned int i;
    forget_screen();
    out_str("\x1b[0m\x1b[2J");
    for(i = 0; i < sizeof(help_text) / sizeof(help_text[0]) && (int)i < rows - 1; i++) {
        move_to((int)i, 0);
        out_add(help_text[i], strlen(help_text[i]) < (size_t)cols ? strlen(help_text[i]) : (size_t)cols - 1);
    }
    out_flush();
    get_key();
    out_str("\x1b[2J");
}

/**** searching ****/

static void search(int back, int again) {
    int k, step = back ? -1 : 1;
    if(!plen) {
        snprintf(msg, sizeof(msg), "No search yet: / or ? first");
        return;
    }
    if(again && found >= top && found < top + text_rows) k = found + step;  /* past the match on the screen */
    else k = back ? top - 1 : top;
    for(; k >= 0 && k < nlines; k += step)
        if(line_has(k)) {
            found = top = k;
            return;
        }
    snprintf(msg, sizeof(msg), "Not found: %s", pattern);
}

/* / or ?: the text to find, typed on the bottom row (Esc, ^C or ^G: none) */
static void ask(int kind) {
    char typed[sizeof(pattern) + 1];
    size_t n = 1, i;
    int c;
    typed[0] = (char)kind;
    typed[1] = 0;
    for(;;) {
        asking = typed;
        draw();
        asking = NULL;
        c = get_key();
        if(c == '\r' || c == '\n') break;
        if(c == KEY_ESC || c == CTRL('c') || c == CTRL('g') || c == KEY_EOF) return;
        if(c == 0x7f || c == CTRL('h')) {
            if(n == 1) return;
            do n--; while(n > 1 && ((unsigned char)typed[n] & 0xC0) == 0x80);    /* a whole UTF-8 character */
            typed[n] = 0;
            continue;
        }
        if(c >= 0x20 && c < 0x100 && c != 0x7f && n < sizeof(typed) - 1) {
            typed[n++] = (char)c;
            typed[n] = 0;
        }
    }
    if(n > 1) {                                 /* new text; Enter alone: the last again */
        memcpy(pattern, typed + 1, n);
        plen = n - 1;
        folded = 1;
        for(i = 0; i < plen; i++)
            if(isupper((unsigned char)pattern[i])) folded = 0;
        if(folded)
            for(i = 0; i < plen; i++)
                if((unsigned char)pattern[i] < 0x80) pattern[i] = (char)tolower((unsigned char)pattern[i]);
    }
    backward = kind == '?';
    search(backward, 0);
}

/**** the program ****/

/* standard output is no terminal: the text goes there as it is */
static int copy(int fd) {
    static char buf[16384];
    for(;;) {
        ssize_t n = read(fd, buf, sizeof(buf)), done = 0;
        if(n < 0 && errno == EINTR) continue;
        if(n < 0) {
            fprintf(stderr, "less: %s: %s\n", name, strerror(errno));
            return 1;
        }
        if(n == 0) return 0;
        while(done < n) {
            ssize_t w = write(1, buf + done, (size_t)(n - done));
            if(w < 0 && errno == EINTR) continue;
            if(w <= 0) return 1;
            done += w;
        }
    }
}

int main(int argc, char **argv) {
    int fd = 0, c, caps;
    if(argc > 2 || (argc == 2 && argv[1][0] == '-' && argv[1][1])) {
        fprintf(stderr, "usage: less [file]   (a pipeline's text too: dmesg | less)\n");
        return 2;
    }
    if(argc == 2 && strcmp(argv[1], "-")) {
        name = argv[1];
        fd = open(name, O_RDONLY);
        if(fd < 0) {
            fprintf(stderr, "less: %s: %s\n", name, strerror(errno));
            return 1;
        }
    } else {
        name = "standard input";
        if(isatty(0)) {
            fprintf(stderr, "less: no file, and the standard input is the terminal (usage: less [file])\n");
            return 2;
        }
    }
    if(!isatty(1)) return copy(fd);
    if(read_text(fd) < 0) {
        fprintf(stderr, "less: %s: %s\n", name, strerror(errno));
        return 1;
    }
    if(fd) close(fd);
    if(index_lines() < 0) {
        fprintf(stderr, "less: out of memory\n");
        return 1;
    }
    keys = open("/dev/tty", O_RDONLY);
    if(keys < 0) {
        fprintf(stderr, "less: /dev/tty: %s\n", strerror(errno));
        return 1;
    }
    caps = ttyctl(TTY_CAPS, 0);
    utf8 = caps > 0 && (caps & TTY_CAP_UTF8);
    if(ttyctl(TTY_RAW, 1) < 0) {
        fprintf(stderr, "less: raw mode: %s\n", strerror(errno));
        return 1;
    }
    raw_mode = 1;
    out_str("\x1b[?1049h\x1b[0m\x1b[2J");      /* without the alternate screen: a clear one */
    for(;;) {
        draw();
        c = get_key();
        msg[0] = 0;
        switch(c) {
        case 'q': case 'Q': case CTRL('c'): case KEY_EOF:
            restore_terminal();
            return 0;
        case ' ': case 'f': case KEY_PGDN: case CTRL('f'): case CTRL('v'):
            top += text_rows;
            break;
        case 'b': case KEY_PGUP: case CTRL('b'):
            top -= text_rows;
            break;
        case '\r': case '\n': case 'j': case 'e': case KEY_DOWN: case CTRL('n'): case CTRL('e'):
            top++;
            break;
        case 'k': case 'y': case KEY_UP: case CTRL('p'): case CTRL('y'):
            top--;
            break;
        case 'd': case CTRL('d'):
            top += text_rows / 2;
            break;
        case 'u': case CTRL('u'):
            top -= text_rows / 2;
            break;
        case 'g': case '<': case KEY_HOME:
            top = 0;
            break;
        case 'G': case '>': case KEY_END:
            top = max_top();
            break;
        case KEY_RIGHT:
            if(left < 1000000) left += cols / 2;
            break;
        case KEY_LEFT:
            left -= cols / 2;
            break;
        case '/': case '?':
            ask(c);
            break;
        case 'n':
            search(backward, 1);
            break;
        case 'N':
            search(!backward, 1);
            break;
        case CTRL('l'):
            forget_screen();
            out_str("\x1b[0m\x1b[2J");
            break;
        case 'h': case 'H':
            help();
            forget_screen();
            break;
        }
    }
}
