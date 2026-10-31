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
bytes in the kernel). Keys that come with the Enter (the rest of a paste) are dropped,
as the console drops what it holds when raw mode ends. 10/6/26: when the editor's own
buffer still held keys after the Enter (the rest of a paste; an LF right after the CR
of the Enter does not count), it asks the terminal to skip the rest of the paste until
the input pauses (TTY_SKIP), so a pasted block runs its first line and nothing else.
Keys typed while a command runs wait in the console's line-mode queue and are the next
prompt's first keys (type-ahead, as in bash). 10/25/26 (the programs' review, U3): only
the Wi-Fi password questions drop what was typed before them (shell_ask_secret(); wifi
join and wifi save on their way out too), so a password typed ahead of a question that
never came neither runs nor goes into the history.

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

10/6/26: on a pseudo-terminal (a telnet session) the prompt is in its remote state,
"≋@>" (surfos/prompt.h): the terminal is another computer's. The process list says which
terminal this shell has (procinfo), once.

10/24/26: a line that goes on with a command (more) has the continuation prompt, "… ≋> "
(shell/prompt.c); there ^C drops the whole command, and so does ^D on an empty line, which
does not leave the shell.

10/25/26: words. Ctrl+Left and Ctrl+Right (Alt+Left, Alt+Right, and Alt+B, Alt+F as bash has
them) move by words, letters and digits as bash counts them; Alt+D deletes to the end of the
word after the cursor, Alt+Backspace to the start of the one before it (^W still takes
everything back to a blank). Alt+. puts the last word of the line before at the cursor, and
again at once the last word of the line before that in its place. A terminal's sequences keep
their modifiers (xterm's ESC[1;5D, as the console sends them); ESC and another byte in the same
read are Alt and that key, and an ESC alone is Esc. Ctrl+R searches the history as text is
typed (search()), which keeps HIST_MAX lines (1,000). history_expand() does bash's history
expansion in part (!! !n !-n !str !$) on a line main.c was given, before it is kept; a line
that starts with a space is not kept (bash's ignorespace, main.c).
*/

#include "surfsh.h"
#include <surfos/prompt.h>

#define CTRL(c) ((c) & 0x1f)
#define BLANK(c) ((c) == ' ' || (c) == '\t')

enum {
    KEY_NONE = 0x100, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END, KEY_DEL, KEY_EOF,
    KEY_ESC, KEY_WORD_LEFT, KEY_WORD_RIGHT
};
#define KEY_ALT  0x1000                 /* 10/25/26: with Alt (ESC and the key in one read, a sequence's modifier) */
#define KEY_CTRL 0x2000                 /* with Ctrl: a sequence's modifier (ESC[1;5D) */

/**** output: collected while keys are waiting, then one write ****/

static char obuf[4096];
static int olen;

static void flush_out(void) {
    int done = 0;
    while(done < olen) {
        ssize_t n = write(term_fd, obuf + done, (size_t)(olen - done));   /* 10/25/26: the terminal, whatever 1 is */
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
static int continuing;                  /* 10/24/26: the line goes on with a command (the continuation prompt) */
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

/* is this shell's terminal a pseudo-terminal, a telnet session's? Asked once. The process
   list is read by index, and a task collected meanwhile shifts the rest: a search that does
   not find this task looks again. */
static int remote_session(void) {
    static int known = -1;
    struct procinfo pi;
    int i, k, me;
    if(known >= 0) return known;
    me = getpid();
    known = 0;
    for(k = 0; k < 3; k++)
        for(i = 0; procinfo(i, &pi) == 0; i++)
            if(pi.pid == me) return known = pi.tty >= PROC_TTY_PTY;
    return known;
}

/* the prompt for this line (screen_setup() first) */
static void make_prompt(void) {
    struct prompt_ctx c;
    char cwd[PATH_MAX];
    memset(&c, 0, sizeof(c));
    c.cwd = getcwd(cwd, sizeof(cwd)) ? cwd : NULL;
    c.last_status = last_status;
    c.remote = remote_session();
    c.hostname = var_get("HOSTNAME");                  /* 10/25/26 (A10): a telnet session's prompt names the host */
    c.width = width;
    c.unicode = use_unicode();
    c.color = colour;
    c.max_depth = settings.max_depth;
    c.continuation = continuing;
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
   final byte (0x40-0x7E). The first parameter is the key's number (ESC[3~), the second its
   modifiers, 1 + Shift + 2 Alt + 4 Ctrl as xterm sends them (ESC[1;5D, ESC[3;5~: with Ctrl;
   10/25/26: Alt and Ctrl are kept, Shift is not). A private sequence (ESC[?...) or one with
   intermediate bytes is no key; another byte ends the sequence and is a key again. */
static int sequence(void) {
    int c, num = 0, mods = 0, field = 0, other = 0, key = KEY_NONE;
    for(;;) {
        c = next_byte();
        if(c < 0) return KEY_EOF;
        if(c < 0x20 || c > 0x7E) {
            inpos--;
            return KEY_NONE;
        }
        if(c >= 0x40) break;                        /* the final byte */
        if(c >= '0' && c <= '9') {
            if(field == 0 && num < 1000) num = num * 10 + (c - '0');
            else if(field == 1 && mods < 1000) mods = mods * 10 + (c - '0');
        } else if(c == ';' || c == ':') {
            field++;
        } else {
            other = 1;                              /* < = > ? or an intermediate byte */
        }
    }
    if(other) return KEY_NONE;
    switch(c) {
    case 'A': key = KEY_UP; break;
    case 'B': key = KEY_DOWN; break;
    case 'C': key = KEY_RIGHT; break;
    case 'D': key = KEY_LEFT; break;
    case 'H': key = KEY_HOME; break;
    case 'F': key = KEY_END; break;
    case '~':
        switch(num) {
        case 1: case 7: key = KEY_HOME; break;
        case 4: case 8: key = KEY_END; break;
        case 3: key = KEY_DEL; break;
        }
    }
    if(key == KEY_NONE) return KEY_NONE;        /* Insert, PgUp, PgDn, F keys: nothing here */
    if(mods > 1 && ((mods - 1) & 2)) key |= KEY_ALT;
    if(mods > 1 && ((mods - 1) & 4)) key |= KEY_CTRL;
    return key;
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
        return KEY_ESC;
    }
    c = next_byte();
    if(c == '[' || c == 'O') return sequence();
    if(c == 0x1b) {                             /* Esc twice: the first is Esc */
        inpos--;
        return KEY_ESC;
    }
    return c | KEY_ALT;                         /* 10/25/26: ESC and a key in the same read: Alt and the key */
}

/* 10/25/26: what a key with Alt or Ctrl does here: with Left or Right (and Alt+B, Alt+F) it moves
   by a word; with another key of a sequence it is that key (Ctrl+Up is Up); Alt with a typed
   key stays as it is (Alt+D, Alt+Backspace, Alt+.) */
static int meaning(int key) {
    int k = key & ~(KEY_ALT | KEY_CTRL);
    if(k == key) return key;
    if(k == KEY_LEFT || ((key & KEY_ALT) && (k == 'b' || k == 'B'))) return KEY_WORD_LEFT;
    if(k == KEY_RIGHT || ((key & KEY_ALT) && (k == 'f' || k == 'F'))) return KEY_WORD_RIGHT;
    return k >= KEY_NONE ? k : key;
}

/* 10/25/26: words as bash's Alt+B, Alt+F, Alt+D and Alt+Backspace see them: letters and digits
   (and any character beyond ASCII); everything else parts them */
static int is_word(char c) {
    return isalnum((unsigned char)c) || (unsigned char)c >= 0x80;
}

/* the start of the word before p */
static int word_left(int p) {
    while(p > 0 && !is_word(line[p - 1])) p--;
    while(p > 0 && is_word(line[p - 1])) p--;
    return p;
}

/* the end of the word after p */
static int word_right(int p) {
    while(p < len && !is_word(line[p])) p++;
    while(p < len && is_word(line[p])) p++;
    return p;
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

/* a character of more than one byte from a terminal: the lead byte and what follows it, in ch;
   its bytes, 0 when it was broken */
static int read_utf8(int lead, char *ch) {
    int need = lead >= 0xF0 ? 3 : lead >= 0xE0 ? 2 : 1, n = 1;
    ch[0] = (char)lead;
    while(n <= need) {
        int c = next_byte();
        if(c < 0) return 0;
        if(!is_cont(c)) {                       /* broken: dropped, and that byte is a key again */
            inpos--;
            return 0;
        }
        ch[n++] = (char)c;
    }
    return n;
}

static void insert_utf8(int lead) {
    char ch[4];
    int n = read_utf8(lead, ch);
    if(n) insert(ch, n);
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

/* 10/24/26: the history as lines a new surfsh runs (history -s), for its history builtin (exec.c) */
void history_state(struct sbuf *out) {
    int i;
    for(i = hist_oldest(); i < hist_count; i++) {
        sb_puts(out, "history -s ");
        sb_quote(out, hist[i % HIST_MAX]);
        sb_putc(out, '\n');
    }
}

int history_print(void) {
    int i;
    for(i = hist_oldest(); i < hist_count; i++) printf("%5d  %s\n", i + 1, hist[i % HIST_MAX]);
    return 0;
}

/* 10/25/26 (track A, A10): for main.c's history on the stick */
int history_count(void) {
    return hist_count;
}

const char *history_line(int i) {
    return i >= hist_oldest() && i < hist_count ? hist[i % HIST_MAX] : NULL;
}

/**** 10/25/26: history expansion, and the last word of a line (!$, Alt+.) ****/

static int is_meta(char c) {                    /* a character that ends a word without a blank */
    return c && strchr(";&|<>()", c) != NULL;
}

/* The last word of a line as bash's history reads it: blanks part words, and so do ; & | < > ( ),
   which make words of their own; quotes and a backslash keep what they hold in the word. Its
   length, 0 for none; *start: where it begins */
static int last_word(const char *s, int *start) {
    int i = 0, n = (int)strlen(s), from = 0, wlen = 0;
    while(i < n) {
        int b = i;
        if(BLANK(s[i])) {
            i++;
            continue;
        }
        if(is_meta(s[i])) {
            while(i < n && is_meta(s[i])) i++;
        } else {
            while(i < n && !BLANK(s[i]) && !is_meta(s[i])) {
                char q = s[i++];
                if(q == '\\' && i < n) {
                    i++;
                } else if(q == '\'' || q == '"') {
                    while(i < n && s[i] != q) i += (q == '"' && s[i] == '\\' && i + 1 < n) ? 2 : 1;
                    if(i < n) i++;
                }
            }
        }
        from = b;
        wlen = i - b;
    }
    *start = from;
    return wlen;
}

/* The reference at in[i], a !: 0 when this ! stays itself; otherwise where the reference ends, with
   what it stands for added to b, or *missing set when the history has no such line */
static int reference(const char *in, int i, struct sbuf *b, int *missing) {
    int k = i + 1, at, word = 0, start, wlen;
    char c = in[k];
    if(!c || BLANK(c) || c == '=' || c == '(') return 0;          /* a ! alone, != and !( */
    if(i > 0 && (in[i - 1] == '$' || in[i - 1] == '[')) return 0;  /* $! and [!a] */
    if(i > 1 && in[i - 1] == '{' && in[i - 2] == '$') return 0;   /* ${!name} */
    if(c == '!' || c == '$') {                                     /* !! and !$: the line before */
        at = hist_count - 1;
        word = c == '$';
        k++;
    } else if(isdigit((unsigned char)c) || (c == '-' && isdigit((unsigned char)in[k + 1]))) {
        int back = c == '-', num = 0;                              /* !n, and !-n back from the end */
        if(back) k++;
        for(; isdigit((unsigned char)in[k]); k++) if(num < 1000000) num = num * 10 + (in[k] - '0');
        at = back ? hist_count - num : num - 1;
    } else {                                                       /* !str: the newest that starts so */
        int e = k;
        while(in[e] && !BLANK(in[e]) && !is_meta(in[e]) && !strchr("'\"\\", in[e])) e++;
        if(e == k) return 0;                                       /* !) !; !' and the like */
        for(at = hist_count - 1; at >= hist_oldest(); at--)
            if(!strncmp(hist[at % HIST_MAX], in + k, (size_t)(e - k))) break;
        k = e;
    }
    if(at < hist_oldest() || at >= hist_count) {
        *missing = 1;
        return k;
    }
    if(!word) {
        sb_puts(b, hist[at % HIST_MAX]);
    } else if((wlen = last_word(hist[at % HIST_MAX], &start))) {
        sb_put(b, hist[at % HIST_MAX] + start, wlen);
    } else {
        *missing = 1;
    }
    return k;
}

/* History expansion, bash's in part, of a line typed at the prompt: !! the line before, !n line n
   (as history numbers them), !-n the n-th line back, !str the newest line that starts with str
   (up to a blank, ; & | < > ( ), a quote or a backslash), !$ the last word of the line before. A !
   stays itself before a blank, =, ( or the end of the line, after a backslash, in $!, ${! and [!,
   inside quotes (double ones too, unlike bash: "#!/bin/sh" stays as typed) and in a comment (from
   a # that begins a word). Returns 1 with out holding the line expanded, 0 when nothing was
   (out untouched), -1 with out holding the reference no line answers, -2 when the line would be
   longer than size - 1 bytes. out may be in. 10/25/26 (the programs' review, U6): *quote_state is the
   quote a line before this one of the same command left open (0: none), and this line's end leaves it
   there, so a quote opened on one line goes on over the next (main.c starts each command with 0). */
int history_expand_in(const char *in, char *out, int size, int *quote_state) {
    struct sbuf b = { NULL, 0, 0 };
    int i = 0, n = (int)strlen(in), quote = quote_state ? *quote_state : 0, changed = 0, end, r = 0;
    while(i < n) {
        char c = in[i];
        int missing = 0;
        if(quote) {
            if(c == quote) quote = 0;
            else if(quote == '"' && c == '\\' && i + 1 < n) sb_putc(&b, in[i++]);
        } else if(c == '\'' || c == '"') {
            quote = c;
        } else if(c == '\\' && i + 1 < n) {
            sb_putc(&b, in[i++]);
        } else if(c == '#' && (i == 0 || BLANK(in[i - 1]) || is_meta(in[i - 1]))) {
            sb_put(&b, in + i, n - i);          /* a comment: the rest as it is */
            break;
        } else if(c == '!' && (end = reference(in, i, &b, &missing))) {
            if(missing) {
                struct sbuf ref = { NULL, 0, 0 };
                sb_put(&ref, in + i, end - i);
                snprintf(out, (size_t)size, "%s", sb_str(&ref));
                sb_free(&ref);
                r = -1;
                goto done;
            }
            i = end;
            changed = 1;
            continue;
        }
        sb_putc(&b, in[i++]);
    }
    if(quote_state) *quote_state = quote;    /* the quote this line leaves open, for the next */
    if(changed && b.len >= size) {
        r = -2;
    } else if(changed) {
        memcpy(out, sb_str(&b), (size_t)b.len + 1);
        r = 1;
    }
done:
    sb_free(&b);
    return r;
}

int history_expand(const char *in, char *out, int size) {
    return history_expand_in(in, out, size, NULL);
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
        too_long = len > limit;                 /* 10/25/26 (the review, U7): a longer line never runs cut off */
        if(len > limit) len = limit;
        memcpy(line, s, (size_t)len);
    }
    pos = len;
    redraw(0);
}

/**** 10/25/26: Ctrl+R and Alt+. ****/

#define SEARCH_MAX 64                   /* bytes of the text Ctrl+R looks for */

static int status_at = -1;              /* the search's row under the line: where it starts, in columns
                                           from the prompt's start; -1 while there is none */
static char last_search[SEARCH_MAX + 1];   /* the text of the last search, for Ctrl+R twice */
static int yank_from, yank_to, yank_back;   /* what Alt+. put in the line, from how many lines back */

/* The row under the line says s, in the plain colour and cut to its last width - 1 columns (no
   pending wrap there); the cursor goes back into the line. Where the line ends inside a row, a
   CR LF makes the row under it (the screen scrolls at its foot); where it ends at a row's end,
   past_edge() made that row already. */
static void status_show(const char *s) {
    int n = (int)strlen(s);
    while(display_cols(s, n) > width - 1) {
        do {
            s++;
            n--;
        } while(n > 0 && is_cont(*s));
    }
    move_to(drawn);
    if(drawn % width) {
        put("\r\n", 2);
        cur = (drawn / width + 1) * width;
    }
    status_at = cur;
    if(colour) puts_(SGR_RESET);
    put(s, n);
    puts_("\033[K");
    if(colour) puts_(SGR_TYPED);
    cur += display_cols(s, n);
    move_to(col_of(pos));
}

/* the search's row emptied (it stays under the line, empty) */
static void status_clear(void) {
    if(status_at < 0) return;
    move_to(status_at);
    puts_("\033[K");
    status_at = -1;
    move_to(col_of(pos));
}

/* the newest line of the history at index from or before it that holds text, other than skip:
   its index, -1 for none */
static int find_back(int from, const char *text, const char *skip) {
    int i;
    for(i = from; i >= hist_oldest(); i--) {
        const char *h = hist[i % HIST_MAX];
        if(strstr(h, text) && (!skip || strcmp(h, skip))) return i;
    }
    return -1;
}

/* the line becomes s (n bytes), the cursor at p; the search's row is emptied first, as the line
   may grow over it */
static void search_show(const char *s, int n, int p, int was_too_long) {
    status_clear();
    too_long = was_too_long || n > limit;       /* 10/25/26 (the review, U7): a line found that is longer than */
    if(n > limit) n = limit;                    /* the limit refuses Enter, rather than run cut off */
    if(p > n) p = n;
    memmove(line, s, (size_t)n);
    len = n;
    pos = p;
    redraw(0);
}

/* Ctrl+R: the history searched backwards as text is typed, as bash's reverse-i-search does. The
   line shows the newest line that holds the text, the cursor where the text begins, and the row
   under the line says "search: text" (the prompt stays as prompt.c made it). Typing adds to the
   text, Backspace takes from it, Ctrl+R again goes to an older line that holds it (another line,
   not the same one again; on an empty text, the last search's text). Esc or ^G puts back the line
   as it was. Any other key ends the search with the line found and is returned for the caller to
   do as usual (Enter runs the line); 0 when the search used the key. */
static int search(void) {
    static char saved[LINE_LIMIT + 1];
    char text[SEARCH_MAX + 1], say[SEARCH_MAX + 32];
    int tlen = 0, found = -1, failed = 0, saved_len = len, saved_pos = pos, saved_too_long = too_long, key;
    memcpy(saved, line, (size_t)len);
    if(hist_pos == hist_count) {                /* what was typed, for Down after the search */
        memcpy(typed, line, (size_t)len);
        typed_len = len;
        typed_too_long = too_long;
    }
    text[0] = 0;
    for(;;) {
        const char *h;
        int at = -1;
        snprintf(say, sizeof(say), failed ? "search: %s (not found)" : "search: %s", text);
        status_show(say);
        key = get_key();
        if(key == CTRL('r')) {
            if(!tlen && !last_search[0]) continue;
            if(!tlen) {                         /* Ctrl+R twice: the last search again */
                strcpy(text, last_search);
                tlen = (int)strlen(text);
                at = find_back(hist_count - 1, text, NULL);
            } else {
                at = find_back(found >= 0 ? found - 1 : hist_count - 1, text,
                               found >= 0 ? hist[found % HIST_MAX] : NULL);
            }
        } else if(key == 0x7f || key == 0x08) {
            if(!tlen) continue;
            do tlen--; while(tlen > 0 && is_cont(text[tlen]));
            text[tlen] = 0;
            if(!tlen) {                         /* nothing to look for: the line as it was */
                search_show(saved, saved_len, saved_pos, saved_too_long);
                found = -1;
                failed = 0;
                continue;
            }
            at = find_back(hist_count - 1, text, NULL);
        } else if((key >= 0x20 && key < 0x7f) || (key >= 0xC2 && key <= 0xF4)) {
            char ch[4];
            int k = 1;
            ch[0] = (char)key;
            if(key >= 0x80 && !(k = read_utf8(key, ch))) continue;
            if(tlen + k > SEARCH_MAX) continue;
            memcpy(text + tlen, ch, (size_t)k);
            tlen += k;
            text[tlen] = 0;
            at = find_back(found >= 0 ? found : hist_count - 1, text, NULL);
        } else if(key == KEY_ESC || key == CTRL('g')) {
            search_show(saved, saved_len, saved_pos, saved_too_long);
            return 0;
        } else {                                /* the line found stays; the key is the caller's */
            status_clear();
            if(found >= 0) hist_pos = found;
            if(tlen) strcpy(last_search, text);
            return key;
        }
        if(at < 0) {                            /* no such line: the one found before stays */
            failed = 1;
            continue;
        }
        found = at;
        failed = 0;
        h = hist[at % HIST_MAX];
        search_show(h, (int)strlen(h), (int)(strstr(h, text) - h), 0);
    }
}

/* Alt+.: the last word of the line before, at the cursor, as bash's yank-last-arg does; again at
   once, the last word of the line before that, in its place */
static void last_arg(int again) {
    int back = again ? yank_back + 1 : 1, at = hist_count - back, start, n;
    const char *h;
    if(at < hist_oldest()) return;
    h = hist[at % HIST_MAX];
    if(!(n = last_word(h, &start))) return;
    if(again) delete_range(yank_from, yank_to);
    yank_from = pos;
    insert(h + start, n);
    yank_to = pos;
    yank_back = back;
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
    fflush(stdout);
    dprintf(term_fd, "%s", prompt);
    n = readline(buf, (size_t)size);
    return n < 0 ? -1 : n;
}

int edit_line(char *buf, int size, int more) {
    int key, last_tab = 0, last_yank = 0, again = 0, result;
    continuing = more;
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
        int was_tab = 0, was_yank = 0;
        key = again ? again : get_key();        /* again: the key that ended a search */
        again = 0;
        if(key == '\n' && after_cr) {           /* the LF of a CR LF */
            after_cr = 0;
            continue;
        }
        after_cr = key == '\r';
        switch(meaning(key)) {
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
            if(continuing) {                    /* 10/24/26: the command is dropped; the shell goes on */
                leave_line("^D");
                result = -3;
                goto done;
            }
            leave_line("exit");
            result = -1;
            goto done;
        case CTRL('c'):
            if(continuing) {                    /* 10/24/26: the whole command is dropped */
                leave_line("^C");
                len = 0;
                result = -3;
                goto done;
            }
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
        case KEY_WORD_LEFT:                     /* 10/25/26: Ctrl+Left, Alt+B */
            pos = word_left(pos);
            move_to(col_of(pos));
            break;
        case KEY_WORD_RIGHT:                    /* Ctrl+Right, Alt+F */
            pos = word_right(pos);
            move_to(col_of(pos));
            break;
        case KEY_ALT | 'd':
        case KEY_ALT | 'D':
            delete_range(pos, word_right(pos));
            break;
        case KEY_ALT | 0x7f:
        case KEY_ALT | 0x08:
            delete_range(word_left(pos), pos);
            break;
        case KEY_ALT | '.':
        case KEY_ALT | '_':
            last_arg(last_yank);
            was_yank = 1;
            break;
        case CTRL('r'):
            again = search();
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
        last_yank = was_yank;
    }
done:
    flush_out();
    if(after_cr && inpos < inlen && inbuf[inpos] == '\n') {     /* the LF of the CR LF that ended the line */
        inpos++;
        after_cr = 0;
    }
    if(inpos < inlen) ttyctl(TTY_SKIP, 1);     /* keys after the Enter: the rest of a paste is skipped */
    ttyctl(TTY_RAW, 0);                         /* the console drops the keys it still holds, */
    inpos = inlen = 0;                          /* and so does the editor: no type-ahead either way */
    buf[len] = 0;
    return result;
}
