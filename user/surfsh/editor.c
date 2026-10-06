/*
SurfOS shell: surfsh
--------------------
File: editor.c  Date: 10/6/26
--------------------
The line editor. The console is in raw mode (ttyctl(TTY_RAW, 1)) only while a line
is typed, and back in line mode before anything runs, so the kernel's commands can
ask their questions and the pager can stop at a screenful. The terminal is used only
through raw mode, TTY_GETSIZE and the ANSI subset of include/surfos/tty.h, the same
on the screen and over the serial line, where the terminal's own key sequences
arrive as they were sent.

Keys: Left, Right, Home, End (also ^A, ^E), Backspace, Delete; ^U clears the line,
^K deletes to its end, ^W the word before the cursor; Up and Down go through the
history (HIST_MAX lines, no line twice in a row); Tab completes (complete.c), and a
second Tab in a row lists the choices (asking first when they would not fit on the
screen: the list is written in raw mode, where the pager does not stop); ^L clears
the screen, ^C drops the line for a fresh prompt, ^D on an empty line leaves the
shell (otherwise it deletes). A terminal's escape sequences are read to their final
byte, so one the editor does not know leaves nothing in the line. Enter is
'\r' from a keyboard or a terminal and '\n' from a test harness (CR LF counts once).
Typed text is teal. A line longer than LINE_LIMIT bytes takes no more characters and
is refused with Enter, so a cut-off command never runs; once something is deleted,
the line on the screen is what runs again. Keys that arrive together
(a paste) are all handled before the screen is written, so the editor keeps up with
the console's input queue (256 bytes; a faster paste over the serial line loses
bytes in the kernel). Keys typed after Enter are dropped, as the console drops what
it holds when raw mode ends: there is no type-ahead.

Long lines wrap. The console wraps the VT100 way: writing the last column leaves the
cursor there until the next character comes. So the editor keeps the cursor out of
that state: whenever the prompt or the line ends exactly at the end of a row it
writes CR LF, and the cursor is at the start of the next row. Its position is kept as
a count of columns from the prompt's start (the row is that over the width), and it
moves only with CR, ESC[nA, ESC[nB and ESC[nC. A change in the middle of the line
moves back to the first changed character and writes the rest of the line again;
when the line got shorter, blanks go over the cells it no longer needs, and over
nothing else: what lies below the line (a program's drawing, ansi's corner) stays,
unless the line grows over it, as on any terminal. The prompt itself is written once
per line (again only after ^L, ^C or a list of choices). Before a prompt the cursor
goes to the start of a fresh row without leaving an empty one: a row of spaces up to
the edge, a CR and ESC[K do that wherever the last command left the cursor, and clear
the prompt's own row. The colours are reset and the cursor shown first: a kernel
command runs in this task, and what it leaves (inverse video, a hidden cursor) is not
undone by the end of a task.
*/

#include "surfsh.h"
#include <surfos/prompt.h>

#define CTRL(c) ((c) & 0x1f)
#define BLANK(c) ((c) == ' ' || (c) == '\t')

enum {
    KEY_NONE = 0x100, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END, KEY_DEL, KEY_EOF
};

/**** output: collected while keys are waiting, then one write ****/

static char obuf[4096];
static int olen;

static void flush_out(void) {
    int done = 0;
    while(done < olen) {
        ssize_t n = write(1, obuf + done, (size_t)(olen - done));
        if(n <= 0) break;
        done += (int)n;
    }
    olen = 0;
}

static void put(const char *s, int n) {
    while(n > 0) {
        int k = (int)sizeof(obuf) - olen;
        if(!k) {
            flush_out();
            continue;
        }
        if(k > n) k = n;
        memcpy(obuf + olen, s, (size_t)k);
        olen += k;
        s += k;
        n -= k;
    }
}

static void puts_(const char *s) {
    put(s, (int)strlen(s));
}

static void seq(int n, char final) {           /* ESC [ n final, for n > 0 */
    char s[16];
    if(n <= 0) return;
    snprintf(s, sizeof(s), "\033[%d%c", n, final);
    puts_(s);
}

/**** the state of the line on the screen ****/

static char *line;                      /* the caller's buffer */
static int len, pos, limit;             /* bytes: the line's, the cursor's, the most it may hold */
static int too_long;                    /* characters were refused: Enter refuses the line */
static int width = 80;                  /* the terminal's columns */
static int colour;                      /* prompt.color, where the console has colours */
static char prompt[PROMPT_MAX];
static int pcols;                       /* the columns of the prompt */
static int cur;                         /* the cursor: columns from the prompt's start */
static int drawn;                       /* where the prompt and the line on the screen end, likewise */

static int is_cont(int c) {
    return ((unsigned char)c & 0xC0) == 0x80;
}

/* where byte i of the line is on the screen, in columns from the prompt's start */
static int col_of(int i) {
    return pcols + display_cols(line, i);
}

static int prev_char(int p) {
    if(p > 0) p--;
    while(p > 0 && is_cont(line[p])) p--;
    return p;
}

static int next_char(int p) {
    if(p < len) p++;
    while(p < len && is_cont(line[p])) p++;
    return p;
}

static int rows = 25;                   /* the terminal's rows */

/* the terminal's size and whether to colour, once per line */
static void screen_setup(void) {
    int size = ttyctl(TTY_GETSIZE, 0);
    width = size > 0 ? (size >> 16) & 0xffff : 80;
    rows = size > 0 ? size & 0xffff : 25;
    if(width < 8) width = 8;
    if(width > 1024) width = 1024;
    if(rows < 2) rows = 2;
    colour = use_color();
}

/* the prompt for this line (screen_setup() first) */
static void make_prompt(void) {
    struct prompt_ctx c;
    char cwd[PATH_MAX];
    memset(&c, 0, sizeof(c));
    c.cwd = getcwd(cwd, sizeof(cwd)) ? cwd : NULL;
    c.last_status = last_status;
    c.width = width;
    c.unicode = use_unicode();
    c.color = colour;
    c.max_depth = settings.max_depth;
    prompt_render(&c, prompt, sizeof(prompt));
    pcols = prompt_columns(prompt);
}

/* the cursor from where it is to column `to` (from the prompt's start) */
static void move_to(int to) {
    int r0 = cur / width, r1 = to / width;
    if(to == cur) return;
    if(r1 < r0) seq(r0 - r1, 'A');
    else if(r1 > r0) seq(r1 - r0, 'B');
    put("\r", 1);
    seq(to % width, 'C');
    cur = to;
}

/* after writing up to column end: at the end of a row, go to the start of the next one */
static void past_edge(int end) {
    cur = end;
    if(end % width == 0) put("\r\n", 2);
}

/* Blanks over the cells the line had from the cursor on before, up to column old (where it
   ended): those only. A blank in the last column of a row leaves the console's pending wrap,
   so a CR takes the cursor back to the start of that row. */
static void blank_to(int old) {
    int n;
    if(old <= cur) return;
    for(n = old - cur; n > 0; n--) put(" ", 1);
    cur = old;
    if(old % width == 0) {
        put("\r", 1);
        cur = old - width;
    }
}

/* the line from byte from on is written again, the cells it no longer needs blanked, the cursor put at pos */
static void redraw(int from) {
    int end = col_of(len);
    move_to(col_of(from));
    if(colour) puts_(SGR_TYPED);
    put(line + from, len - from);
    if(len > from) past_edge(end);
    blank_to(drawn);
    drawn = end;
    move_to(col_of(pos));
}

/* the prompt (made already) and the line, from the start of a row that fresh_row() cleared */
static void draw_all(void) {
    puts_(prompt);
    past_edge(pcols);
    if(colour) puts_(SGR_TYPED);
    put(line, len);
    drawn = col_of(len);
    if(len) past_edge(drawn);
    move_to(col_of(pos));
}

/* the start of a fresh, empty row, wherever the last output left the cursor (see the top) */
static void fresh_row(void) {
    int i;
    puts_("\033[0m\033[?25h");
    for(i = 0; i < width; i++) put(" ", 1);
    puts_("\r\033[K");
}

void edit_fresh_row(void) {
    screen_setup();
    fresh_row();
    flush_out();
}

/* the cursor after the line, and the next row: the line is done */
static void leave_line(const char *say) {
    move_to(col_of(len));
    if(colour) puts_(SGR_RESET);
    if(say) {
        puts_(say);
        put("\r\n", 2);
    } else if(cur % width) {
        put("\r\n", 2);
    }
}

/**** keys ****/

static unsigned char inbuf[256];         /* the console's whole input queue (TTY_INPUT_SIZE) at once */
static int inpos, inlen;
static unsigned int lone_esc_at;        /* when a lone ESC came: "ESC" and "[A" may arrive apart */

static int next_byte(void) {
    if(inpos == inlen) {
        ssize_t n;
        flush_out();                            /* the screen is brought up to date before waiting */
        n = read(0, inbuf, sizeof(inbuf));
        if(n <= 0) return -1;
        inpos = 0;
        inlen = (int)n;
    }
    return inbuf[inpos++];
}

/* The rest of ESC [ or ESC O: parameter and intermediate bytes (0x20-0x3F) up to the
   final byte (0x40-0x7E). The first parameter is the key's number (ESC[3~, and ESC[3;5~
   with Ctrl held: the key is what counts). A private sequence (ESC[?...) or one with
   intermediate bytes is no key; another byte ends the sequence and is a key again. */
static int sequence(void) {
    int c, num = 0, first = 1, other = 0;
    for(;;) {
        c = next_byte();
        if(c < 0) return KEY_EOF;
        if(c < 0x20 || c > 0x7E) {
            inpos--;
            return KEY_NONE;
        }
        if(c >= 0x40) break;                        /* the final byte */
        if(c >= '0' && c <= '9') {
            if(first && num < 1000) num = num * 10 + (c - '0');
        } else if(c == ';' || c == ':') {
            first = 0;
        } else {
            other = 1;                              /* < = > ? or an intermediate byte */
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
        }
    }
    return KEY_NONE;                            /* Insert, PgUp, PgDn, F keys: nothing here */
}

static int get_key(void) {
    int c = next_byte();
    if(c < 0) return KEY_EOF;
    if((c == '[' || c == 'O') && lone_esc_at && uptime() - lone_esc_at < 250) {
        lone_esc_at = 0;
        return sequence();
    }
    lone_esc_at = 0;
    if(c != 0x1b) return c;
    if(inpos == inlen) {                        /* nothing came with it: Esc, or a split sequence */
        lone_esc_at = uptime();
        if(!lone_esc_at) lone_esc_at = 1;
        return KEY_NONE;
    }
    c = next_byte();
    if(c == '[' || c == 'O') return sequence();
    inpos--;                                    /* ESC and another key: Esc (nothing), then that key */
    return KEY_NONE;
}

/**** changing the line ****/

static void insert(const char *s, int n) {
    if(len + n > limit) {
        too_long = 1;
        return;
    }
    memmove(line + pos + n, line + pos, (size_t)(len - pos));
    memcpy(line + pos, s, (size_t)n);
    len += n;
    pos += n;
    if(pos == len) {                            /* at the end: just write it */
        put(s, n);
        past_edge(cur + display_cols(s, n));
        drawn = cur;
    } else {
        redraw(pos - n);
    }
}

/* a character of more than one byte from a terminal: the lead byte and what follows it */
static void insert_utf8(int lead) {
    char ch[4];
    int need = lead >= 0xF0 ? 3 : lead >= 0xE0 ? 2 : 1, n = 1;
    ch[0] = (char)lead;
    while(n <= need) {
        int c = next_byte();
        if(c < 0) return;
        if(!is_cont(c)) {                       /* broken: dropped, and that byte is a key again */
            inpos--;
            return;
        }
        ch[n++] = (char)c;
    }
    insert(ch, n);
}

static void delete_range(int a, int b) {
    if(b <= a) return;
    memmove(line + a, line + b, (size_t)(len - b));
    len -= b - a;
    pos = a;
    too_long = 0;                               /* what runs is the line as it shows now */
    redraw(a);
}

/**** the history ****/

static char *hist[HIST_MAX];
static int hist_count;                  /* lines ever added; hist[i % HIST_MAX] */
static int hist_pos;                    /* the line shown: hist_count is the one being typed */
static char typed[LINE_LIMIT + 1];      /* the line being typed, while the history is shown */
static int typed_len, typed_too_long;

static int hist_oldest(void) {
    return hist_count > HIST_MAX ? hist_count - HIST_MAX : 0;
}

void history_add(const char *s) {
    char *copy;
    if(hist_count && !strcmp(hist[(hist_count - 1) % HIST_MAX], s)) return;   /* the same again */
    if(!(copy = strdup(s))) return;
    free(hist[hist_count % HIST_MAX]);          /* full: the oldest goes */
    hist[hist_count % HIST_MAX] = copy;
    hist_count++;
}

int history_print(void) {
    int i;
    for(i = hist_oldest(); i < hist_count; i++) printf("%5d  %s\n", i + 1, hist[i % HIST_MAX]);
    return 0;
}

static void history_go(int to) {
    if(to < hist_oldest() || to > hist_count || to == hist_pos) return;
    if(hist_pos == hist_count) {                /* keep what was being typed */
        memcpy(typed, line, (size_t)len);
        typed_len = len;
        typed_too_long = too_long;
    }
    hist_pos = to;
    if(to == hist_count) {
        memcpy(line, typed, (size_t)typed_len);
        len = typed_len;
        too_long = typed_too_long;
    } else {
        const char *s = hist[to % HIST_MAX];
        len = (int)strlen(s);
        if(len > limit) len = limit;
        memcpy(line, s, (size_t)len);
        too_long = 0;
    }
    pos = len;
    redraw(0);
}

/**** Tab ****/

/* the choices of a second Tab, under the line; then the prompt and the line again */
static void list_choices(struct cands *c) {
    char **names = malloc((size_t)c->n * sizeof(char *));
    int i, n = 0, shown;
    if(!names) return;
    for(i = 0; i < c->n; i++) {
        char *s = malloc(strlen(c->v[i].name) + 2);
        if(!s) break;
        strcpy(s, c->v[i].name);
        if(c->v[i].dir) strcat(s, "/");
        names[n++] = s;
    }
    shown = n;
    leave_line(NULL);
    if(columns_rows(names, n, width, 0) > rows - 2) {   /* more than a screen: as bash, ask first */
        char ask[64];
        int key;
        snprintf(ask, sizeof(ask), "Display all %d possibilities? (y or n)", n);
        puts_(ask);
        key = get_key();
        put("\r\n", 2);
        if(key != 'y' && key != 'Y' && key != ' ') shown = 0;
    }
    columns(names, shown, width, "", "\r\n", put);
    for(i = 0; i < n; i++) free(names[i]);
    free(names);
    fresh_row();
    draw_all();
}

static void tab(int second) {
    struct cands list = { NULL, 0, 0 };
    int r = complete(line, &len, &pos, limit, &list);
    if(r == 1) redraw(0);
    else if(r == 2 && second) list_choices(&list);
    cands_free(&list);
}

/**** a line ****/

static int raw_failed;                  /* the console has no raw mode: the kernel's line editing */
static int after_cr;                    /* the last key was CR: an LF right after it is the same Enter */

/* without raw mode: the prompt, and a line as the kernel's line discipline edits it */
static int plain_line(char *buf, int size) {
    int n;
    screen_setup();
    make_prompt();
    fputs(prompt, stdout);
    n = readline(buf, (size_t)size);
    return n < 0 ? -1 : n;
}

int edit_line(char *buf, int size) {
    int key, last_tab = 0, result;
    if(raw_failed || ttyctl(TTY_RAW, 1) < 0) {
        raw_failed = 1;
        return plain_line(buf, size);
    }
    line = buf;
    limit = size - 1 < LINE_LIMIT ? size - 1 : LINE_LIMIT;
    len = pos = too_long = 0;
    hist_pos = hist_count;
    screen_setup();
    make_prompt();
    fresh_row();
    draw_all();
    flush_out();
    for(;;) {
        int was_tab = 0;
        key = get_key();
        if(key == '\n' && after_cr) {           /* the LF of a CR LF */
            after_cr = 0;
            continue;
        }
        after_cr = key == '\r';
        switch(key) {
        case '\r':
        case '\n':
            leave_line(NULL);
            result = too_long ? -2 : len;
            goto done;
        case KEY_EOF:
            leave_line(NULL);
            result = -1;
            goto done;
        case CTRL('d'):
            if(len) {
                if(pos < len) delete_range(pos, next_char(pos));
                break;
            }
            leave_line("exit");
            result = -1;
            goto done;
        case CTRL('c'):
            leave_line("^C");
            len = pos = too_long = 0;
            hist_pos = hist_count;
            fresh_row();
            draw_all();
            break;
        case CTRL('a'):
        case KEY_HOME:
            pos = 0;
            move_to(col_of(pos));
            break;
        case CTRL('e'):
        case KEY_END:
            pos = len;
            move_to(col_of(pos));
            break;
        case KEY_LEFT:
            pos = prev_char(pos);
            move_to(col_of(pos));
            break;
        case KEY_RIGHT:
            pos = next_char(pos);
            move_to(col_of(pos));
            break;
        case 0x7f:
        case 0x08:
            delete_range(prev_char(pos), pos);
            break;
        case KEY_DEL:
            delete_range(pos, next_char(pos));
            break;
        case CTRL('u'):
            too_long = 0;
            delete_range(0, len);
            break;
        case CTRL('k'):
            delete_range(pos, len);
            break;
        case CTRL('w'): {
            int p = pos;
            while(p > 0 && BLANK(line[p - 1])) p--;
            while(p > 0 && !BLANK(line[p - 1])) p--;
            delete_range(p, pos);
            break;
        }
        case CTRL('l'):
            puts_("\033[0m\033[2J\033[H");
            cur = 0;
            draw_all();
            break;
        case KEY_UP:
            history_go(hist_pos - 1);
            break;
        case KEY_DOWN:
            history_go(hist_pos + 1);
            break;
        case '\t':
            tab(last_tab);
            was_tab = 1;
            break;
        default:
            if(key >= 0x20 && key < 0x7f) {
                char ch = (char)key;
                insert(&ch, 1);
            } else if(key >= 0xC2 && key <= 0xF4) {
                insert_utf8(key);
            }
            break;                              /* other control characters and keys: nothing */
        }
        last_tab = was_tab;
    }
done:
    flush_out();
    ttyctl(TTY_RAW, 0);                         /* the console drops the keys it still holds, */
    inpos = inlen = 0;                          /* and so does the editor: no type-ahead either way */
    buf[len] = 0;
    return result;
}
