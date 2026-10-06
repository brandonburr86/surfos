/*
SurfOS user program: edit
--------------------
File: edit.c    Date: 10/5/26
--------------------
edit <file>: a small full-screen text editor in the manner of nano. The whole
file lives in memory as an array of lines (files up to 1 MB). The screen is a
title line, the text, a status line and a help line. Lines do not wrap: the
line with the cursor scrolls sideways to keep it in view, the others show their
start, and a '>' in the last column says a line goes on.

Keys: arrows, Home, End, PgUp, PgDn, Backspace, Delete, Enter, Tab (a tab
character, shown as spaces up to the next multiple of 8); ^S save, ^Q quit
(asking first when there are unsaved changes), ^F find (Enter alone repeats the
last search), ^K cut the line (several in a row go together), ^U paste them,
^G go to a line, ^C where the cursor is, ^L draw the screen again.

Saving writes <file>.tmp, fsyncs it and renames it over the file; where the
file system cannot rename (ENOSYS) the file is truncated and written in place.
The file keeps its line ends (LF, or CRLF when its first line has one) and
ends with a line end. The terminal is used only through ttyctl(TTY_RAW),
ttyctl(TTY_GETSIZE), the key sequences of raw mode and the ANSI subset that
include/surfos/tty.h lists, so the editor works on the screen and over the
serial line alike. Each refresh rewrites only the rows that changed.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <surf.h>

#define MAX_FILE   (1024 * 1024)
#define TAB        8
#define CTRL(c)    ((c) & 0x1f)
#define HELP       "^S Save  ^Q Quit  ^F Find  ^K Cut line  ^U Paste  ^G Go to line"

enum {
    KEY_NONE = 0x100, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END,
    KEY_PGUP, KEY_PGDN, KEY_INS, KEY_DEL, KEY_ESC, KEY_EOF
};

struct line {
    char *s;                    /* not terminated */
    int len, cap;
};

static struct line *lines;      /* the text: nlines >= 1 */
static int nlines, lines_cap;
static struct line *cut;        /* ^K collects lines here, ^U inserts them */
static int ncut, cut_cap, cutting;

static const char *filename;
static int crlf;                /* the file's line ends are CR LF */
static int modified;
static int no_rename;           /* rename said ENOSYS once: write in place from then on */

static int cy, cx;              /* cursor: line, byte in the line */
static int want_col = -1;       /* the screen column up and down aim for */
static int top;                 /* first line on the screen */
static int left, left_line = -1;    /* first column shown of the cursor's line, and that line */
static int rows, cols, text_rows;

static char msg[200];           /* for the status line until the next key */
static const char *prompt_label;    /* while asking: the question and the answer so far */
static char prompt_buf[128];
static char last_find[128];

/**** output: one buffer per refresh, one write ****/

static char *out;
static size_t out_len, out_cap;

static void out_add(const char *s, size_t n) {
    if(out_len + n > out_cap) {
        size_t cap = out_cap ? out_cap * 2 : 16384;
        char *p;
        while(cap < out_len + n) cap *= 2;
        p = realloc(out, cap);
        if(!p) return;                  /* this refresh is incomplete; ^L draws again */
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

static void restore_terminal(void) {
    static const char bye[] = "\x1b[0m\x1b[2J\x1b[H\x1b[?25h";
    if(!raw_mode) return;
    raw_mode = 0;
    write(1, bye, sizeof(bye) - 1);
    ttyctl(TTY_RAW, 0);
}

static void screen_size(void) {
    int r = ttyctl(TTY_GETSIZE, 0);
    cols = r > 0 ? (r >> 16) & 0xffff : 80;
    rows = r > 0 ? r & 0xffff : 25;
    if(cols < 16) cols = 16;
    if(cols > 512) cols = 512;
    if(rows < 4) rows = 4;
    if(rows > 256) rows = 256;
    text_rows = rows - 3;
}

/**** keys ****/

static unsigned char inbuf[64];
static int inpos, inlen;
static unsigned int lone_esc_at;    /* uptime of the last lone ESC, to mend a sequence split across reads */

static int next_byte(void) {
    if(inpos == inlen) {
        ssize_t n = read(0, inbuf, sizeof(inbuf));
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
        case 2: return KEY_INS;
        case 3: return KEY_DEL;
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
    inpos--;                                    /* ESC and then another key: Escape, then that key */
    return KEY_ESC;
}

/**** lines ****/

static int line_room(struct line *l, int need) {
    if(need <= l->cap) return 0;
    {
        int cap = l->cap ? l->cap : 16;
        char *p;
        while(cap < need) cap *= 2;
        p = realloc(l->s, (size_t)cap);
        if(!p) { snprintf(msg, sizeof(msg), "Out of memory"); return -1; }
        l->s = p;
        l->cap = cap;
    }
    return 0;
}

static int lines_room(int need) {
    if(need <= lines_cap) return 0;
    {
        int cap = lines_cap ? lines_cap : 64;
        struct line *p;
        while(cap < need) cap *= 2;
        p = realloc(lines, (size_t)cap * sizeof(struct line));
        if(!p) { snprintf(msg, sizeof(msg), "Out of memory"); return -1; }
        lines = p;
        lines_cap = cap;
    }
    return 0;
}

/* a new line at index at with the text s[0..len) */
static int insert_line(int at, const char *s, int len) {
    struct line l = { NULL, 0, 0 };
    if(lines_room(nlines + 1) || line_room(&l, len > 0 ? len : 1)) return -1;
    memcpy(l.s, s, (size_t)len);
    l.len = len;
    memmove(lines + at + 1, lines + at, (size_t)(nlines - at) * sizeof(struct line));
    lines[at] = l;
    nlines++;
    return 0;
}

static void delete_line(int at) {
    free(lines[at].s);
    memmove(lines + at, lines + at + 1, (size_t)(nlines - at - 1) * sizeof(struct line));
    nlines--;
}

/* the screen column where byte i of line l starts */
static int col_of(const struct line *l, int i) {
    int k, col = 0;
    for(k = 0; k < i && k < l->len; k++) col = l->s[k] == '\t' ? (col / TAB + 1) * TAB : col + 1;
    return col;
}

/* the byte whose cell covers screen column col (or the end of the line) */
static int index_of(const struct line *l, int col) {
    int k, c = 0;
    for(k = 0; k < l->len; k++) {
        int next = l->s[k] == '\t' ? (c / TAB + 1) * TAB : c + 1;
        if(next > col) return k;
        c = next;
    }
    return l->len;
}

/**** loading and saving ****/

/* the lines of the file: the empty line after a final line end does not count */
static int text_lines(void) {
    return nlines - (nlines > 1 && !lines[nlines - 1].len);
}

static int load(const char *path) {
    struct stat st;
    char *buf, *p, *end;
    int fd, n = 0, total = 0;
    fd = open(path, O_RDONLY);
    if(fd < 0) {
        if(errno != ENOENT) return -1;
        snprintf(msg, sizeof(msg), "New file");
        return insert_line(0, "", 0);
    }
    if(fstat(fd, &st) < 0) { close(fd); return -1; }
    if(S_ISDIR(st.type)) { close(fd); errno = EISDIR; return -1; }
    if(st.size > MAX_FILE) { close(fd); errno = EFBIG; return -1; }
    buf = malloc(st.size + 1);
    if(!buf) { close(fd); errno = ENOMEM; return -1; }
    while(total <= MAX_FILE && (n = (int)read(fd, buf + total, st.size + 1 - total)) > 0) {
        total += n;
        if(total == (int)st.size + 1) break;    /* it grew while we read: too big now, or changing */
    }
    close(fd);
    if(n < 0) { free(buf); return -1; }
    if(total > (int)st.size) { free(buf); errno = EFBIG; return -1; }
    p = memchr(buf, '\n', (size_t)total);
    crlf = p && p > buf && p[-1] == '\r';
    for(p = buf, end = buf + total;;) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        int len = nl ? (int)(nl - p) : (int)(end - p);
        if(nl && crlf && len && p[len - 1] == '\r') len--;
        if(insert_line(nlines, p, len)) { free(buf); errno = ENOMEM; return -1; }
        if(!nl) break;
        p = nl + 1;
    }
    free(buf);
    n = text_lines();
    snprintf(msg, sizeof(msg), "Read %d line%s%s", n, n == 1 ? "" : "s", crlf ? " (CRLF line ends)" : "");
    return 0;
}

static int write_chunk(int fd, const char *p, int n) {
    ssize_t w = write(fd, p, (size_t)n);
    if(w == n) return 0;
    if(w >= 0) errno = ENOSPC;                  /* a short write: the disk is full */
    return -1;
}

/* every line and its line end; the last line gets one unless it is empty */
static int write_text(int fd) {
    static char chunk[8192];
    int used = 0, i;
    for(i = 0; i < nlines; i++) {
        const struct line *l = &lines[i];
        int k = 0, eol = (i < nlines - 1 || l->len > 0);
        while(k < l->len + (eol ? (crlf ? 2 : 1) : 0)) {
            if(used == (int)sizeof(chunk)) {
                if(write_chunk(fd, chunk, used)) return -1;
                used = 0;
            }
            if(k < l->len) chunk[used++] = l->s[k];
            else chunk[used++] = (crlf && k == l->len) ? '\r' : '\n';
            k++;
        }
    }
    return used ? write_chunk(fd, chunk, used) : 0;
}

static int write_file(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC), e;
    if(fd < 0) return -1;
    if(write_text(fd) < 0 || fsync(fd) < 0) {
        e = errno ? errno : EIO;
        close(fd);
        errno = e;
        return -1;
    }
    return close(fd);
}

static int save(void) {
    char tmp[PATH_MAX + 8];
    const char *what = filename;
    snprintf(tmp, sizeof(tmp), "%s.tmp", filename);
    if(!no_rename) {
        what = tmp;
        if(write_file(tmp) < 0) { int e = errno; unlink(tmp); errno = e; goto fail; }
        if(rename(tmp, filename) == 0) goto done;
        what = "rename";
        if(errno != ENOSYS) { int e = errno; unlink(tmp); errno = e; goto fail; }
        unlink(tmp);
        no_rename = 1;
    }
    what = filename;                            /* no rename here: truncate the file and write it */
    if(write_file(filename) < 0) goto fail;
done:
    modified = 0;
    snprintf(msg, sizeof(msg), "Wrote %d line%s", text_lines(), text_lines() == 1 ? "" : "s");
    return 0;
fail:
    snprintf(msg, sizeof(msg), "Not saved: %s: %s", what, strerror(errno));
    return -1;
}

/**** the screen ****/

/* what each row shows now: its text and whether it is drawn inverse */
static char *shadow;            /* rows * (cols + 1) */
static unsigned char *shadow_inv;
static int shadow_ok;

static void draw_row(int row, const char *text, int inverse) {
    char *old = shadow + row * (cols + 1);
    int len = (int)strlen(text), width = row == rows - 1 ? cols - 1 : cols;    /* never the last cell: no scrolling */
    if(len > width) len = width;
    if(shadow_ok && shadow_inv[row] == inverse && (int)strlen(old) == len && !memcmp(old, text, (size_t)len)) return;
    move_to(row, 0);
    if(inverse) {                                       /* a bar across the screen */
        int k;
        out_str("\x1b[7m");
        out_add(text, (size_t)len);
        for(k = len; k < width; k++) out_add(" ", 1);
        out_str("\x1b[0m");
    } else {
        out_add(text, (size_t)len);
        if(len < width) out_str("\x1b[K");
    }
    memcpy(old, text, (size_t)len);
    old[len] = 0;
    shadow_inv[row] = (unsigned char)inverse;
}

/* the cells of line l from screen column from on; '>' in the last one when it goes on */
static void render_line(const struct line *l, char *dst, int from) {
    int k, col = 0, n = 0, width = cols - 1;
    for(k = 0; k < l->len && col < from + width; k++) {
        unsigned char c = (unsigned char)l->s[k];
        int next = c == '\t' ? (col / TAB + 1) * TAB : col + 1;
        for(; col < next; col++) {
            if(col < from || col >= from + width) continue;
            dst[n++] = c == '\t' ? ' ' : (c >= 32 && c < 127) ? (char)c : '?';
        }
    }
    if(k < l->len || col > from + width) {      /* more to the right */
        while(n < width) dst[n++] = ' ';
        dst[n++] = '>';
    }
    while(n > 0 && dst[n - 1] == ' ') n--;      /* trailing blanks are what ESC[K leaves anyway */
    dst[n] = 0;
}

static void scroll_into_view(void) {
    int col = col_of(&lines[cy], cx);
    if(cy != left_line) { left = 0; left_line = cy; }  /* another line: it starts at its start */
    if(cy < top) top = cy;
    if(cy >= top + text_rows) top = cy - text_rows + 1;
    if(col < left) left = col > cols / 2 ? col - cols / 2 : 0;
    if(col >= left + cols - 1) left = col - cols / 2;
}

static void refresh(void) {
    char *row = malloc((size_t)cols + 2), info[64];
    int r, n, crow, ccol;
    if(!row) return;
    scroll_into_view();
    out_str("\x1b[?25l");
    if(!shadow_ok) out_str("\x1b[0m\x1b[2J");

    n = snprintf(row, (size_t)cols + 1, " SurfOS edit  %s%s", filename, crlf ? "  (CRLF)" : "");
    if(modified) {                                      /* "Modified" at the right, over a long name if need be */
        if(n > cols - 10) n = cols - 10;
        while(n < cols - 10) row[n++] = ' ';
        strcpy(row + n, "Modified");
    }
    draw_row(0, row, 1);

    for(r = 0; r < text_rows; r++) {
        if(top + r < nlines) render_line(&lines[top + r], row, top + r == cy ? left : 0);
        else row[0] = 0;
        draw_row(1 + r, row, 0);
    }

    if(prompt_label) {
        snprintf(row, (size_t)cols + 1, "%s%s", prompt_label, prompt_buf);
        draw_row(rows - 2, row, 0);
    } else {
        snprintf(info, sizeof(info), "Ln %d/%d  Col %d", cy + 1, nlines, col_of(&lines[cy], cx) + 1);
        n = snprintf(row, (size_t)cols + 1, " %s", msg);
        if(n > cols) n = cols;
        if(n + (int)strlen(info) + 2 <= cols - 1) {
            while(n < cols - 1 - (int)strlen(info)) row[n++] = ' ';
            strcpy(row + n, info);
        }
        draw_row(rows - 2, row, 0);
    }
    snprintf(row, (size_t)cols + 1, "%s", HELP);
    draw_row(rows - 1, row, 0);
    shadow_ok = 1;

    if(prompt_label) {
        crow = rows - 2;
        ccol = (int)(strlen(prompt_label) + strlen(prompt_buf));
        if(ccol > cols - 1) ccol = cols - 1;
    } else {
        crow = 1 + cy - top;
        ccol = col_of(&lines[cy], cx) - left;
    }
    move_to(crow, ccol);
    out_str("\x1b[?25h");
    out_flush();
    free(row);
}

/**** editing ****/

static void set_modified(void) {
    modified = 1;
}

static void insert_char(int c) {
    struct line *l = &lines[cy];
    if(line_room(l, l->len + 1)) return;
    memmove(l->s + cx + 1, l->s + cx, (size_t)(l->len - cx));
    l->s[cx++] = (char)c;
    l->len++;
    set_modified();
}

static void enter(void) {
    struct line *l = &lines[cy];
    if(insert_line(cy + 1, l->s + cx, l->len - cx)) return;
    lines[cy].len = cx;
    cy++;
    cx = 0;
    set_modified();
}

/* line y gets line y + 1 appended */
static void join(int y) {
    struct line *a = &lines[y], *b = &lines[y + 1];
    if(line_room(a, a->len + b->len)) return;
    memcpy(a->s + a->len, b->s, (size_t)b->len);
    a->len += b->len;
    delete_line(y + 1);
    set_modified();
}

static void backspace(void) {
    if(cx > 0) {
        struct line *l = &lines[cy];
        memmove(l->s + cx - 1, l->s + cx, (size_t)(l->len - cx));
        l->len--;
        cx--;
        set_modified();
    } else if(cy > 0) {
        int at = lines[cy - 1].len;
        join(cy - 1);
        if(lines[cy - 1].len >= at) { cy--; cx = at; }
    }
}

static void delete_char(void) {
    struct line *l = &lines[cy];
    if(cx < l->len) {
        memmove(l->s + cx, l->s + cx + 1, (size_t)(l->len - cx - 1));
        l->len--;
        set_modified();
    } else if(cy < nlines - 1) {
        join(cy);
    }
}

static void cut_line(void) {
    struct line *l = &lines[cy];
    if(!cutting) {                              /* a new cut replaces what was collected before */
        while(ncut > 0) free(cut[--ncut].s);
    }
    if(ncut == cut_cap) {
        int cap = cut_cap ? cut_cap * 2 : 16;
        struct line *p = realloc(cut, (size_t)cap * sizeof(struct line));
        if(!p) { snprintf(msg, sizeof(msg), "Out of memory"); return; }
        cut = p;
        cut_cap = cap;
    }
    cut[ncut].s = malloc(l->len > 0 ? (size_t)l->len : 1);
    if(!cut[ncut].s) { snprintf(msg, sizeof(msg), "Out of memory"); return; }
    memcpy(cut[ncut].s, l->s, (size_t)l->len);
    cut[ncut].len = cut[ncut].cap = l->len;
    ncut++;
    if(nlines > 1) delete_line(cy);
    else lines[0].len = 0;
    if(cy >= nlines) cy = nlines - 1;
    cx = 0;
    set_modified();
    snprintf(msg, sizeof(msg), "Cut %d line%s", ncut, ncut == 1 ? "" : "s");
}

static void paste(void) {
    int i;
    if(!ncut) { snprintf(msg, sizeof(msg), "Nothing cut yet (^K cuts a line)"); return; }
    for(i = 0; i < ncut; i++) {
        if(insert_line(cy, cut[i].s, cut[i].len)) break;
        cy++;
    }
    cx = 0;
    set_modified();
    snprintf(msg, sizeof(msg), "Pasted %d line%s", i, i == 1 ? "" : "s");
}

/**** questions on the status line ****/

/* an answer typed on the status line: 1 with Enter, 0 when cancelled (^C or Esc) */
static int ask(const char *label) {
    int k, n = 0;
    prompt_label = label;
    prompt_buf[0] = 0;
    for(;;) {
        refresh();
        k = get_key();
        if(k == '\r' || k == '\n') break;
        if(k == CTRL('c') || k == KEY_ESC || k == KEY_EOF) { prompt_label = NULL; snprintf(msg, sizeof(msg), "Cancelled"); return 0; }
        if((k == 0x7f || k == 0x08) && n > 0) prompt_buf[--n] = 0;
        else if(k >= 32 && k < 127 && n < (int)sizeof(prompt_buf) - 1) { prompt_buf[n++] = (char)k; prompt_buf[n] = 0; }
    }
    prompt_label = NULL;
    return 1;
}

/* y, n or cancel: 'y', 'n' or 0 */
static int ask_yes_no(const char *label) {
    int k;
    prompt_label = label;
    prompt_buf[0] = 0;
    for(;;) {
        refresh();
        k = get_key();
        if(k == 'y' || k == 'Y' || k == 'n' || k == 'N') { prompt_label = NULL; return tolower(k); }
        if(k == CTRL('c') || k == KEY_ESC || k == KEY_EOF) { prompt_label = NULL; snprintf(msg, sizeof(msg), "Cancelled"); return 0; }
    }
}

static int match_at(const struct line *l, int i, const char *s, int n) {
    int k;
    if(i + n > l->len) return 0;
    for(k = 0; k < n; k++) if(tolower((unsigned char)l->s[i + k]) != tolower((unsigned char)s[k])) return 0;
    return 1;
}

static void find(void) {
    char label[160];
    int n, y, i, step;
    if(last_find[0]) snprintf(label, sizeof(label), "Find [%s]: ", last_find);
    else snprintf(label, sizeof(label), "Find: ");
    if(!ask(label)) return;
    if(prompt_buf[0]) snprintf(last_find, sizeof(last_find), "%s", prompt_buf);
    if(!last_find[0]) { snprintf(msg, sizeof(msg), "Cancelled"); return; }
    n = (int)strlen(last_find);
    y = cy;
    i = cx + 1;
    for(step = 0; step <= nlines; step++) {     /* every line once, then the start of this one */
        for(; i <= lines[y].len - n; i++) {
            if(match_at(&lines[y], i, last_find, n)) {
                if(step > 0 && y <= cy) snprintf(msg, sizeof(msg), "Search wrapped");
                cy = y;
                cx = i;
                want_col = -1;
                return;
            }
        }
        y = (y + 1) % nlines;
        i = 0;
    }
    snprintf(msg, sizeof(msg), "\"%s\" not found", last_find);
}

static void go_to_line(void) {
    long n;
    char *end;
    if(!ask("Go to line: ") || !prompt_buf[0]) return;
    n = strtol(prompt_buf, &end, 10);
    if(*end || n < 1) { snprintf(msg, sizeof(msg), "Not a line number: %s", prompt_buf); return; }
    if(n > nlines) n = nlines;
    cy = (int)n - 1;
    cx = 0;
    want_col = -1;
}

/* 1 when the editor should end */
static int quit(void) {
    int a;
    if(!modified) return 1;
    a = ask_yes_no("Save changes? (y/n/^C) ");
    if(a == 'n') return 1;
    if(a == 'y') return save() == 0;
    return 0;
}

/**** moving ****/

static void vertical(int dy) {
    if(want_col < 0) want_col = col_of(&lines[cy], cx);
    cy += dy;
    if(cy < 0) cy = 0;
    if(cy >= nlines) cy = nlines - 1;
    cx = index_of(&lines[cy], want_col);
}

static int handle(int k) {
    int keep_col = 0, was_cut = 0;
    switch(k) {
    case KEY_UP:    vertical(-1); keep_col = 1; break;
    case KEY_DOWN:  vertical(1); keep_col = 1; break;
    case KEY_PGUP:  vertical(-(text_rows > 1 ? text_rows - 1 : 1)); keep_col = 1; break;
    case KEY_PGDN:  vertical(text_rows > 1 ? text_rows - 1 : 1); keep_col = 1; break;
    case KEY_LEFT:
        if(cx > 0) cx--;
        else if(cy > 0) { cy--; cx = lines[cy].len; }
        break;
    case KEY_RIGHT:
        if(cx < lines[cy].len) cx++;
        else if(cy < nlines - 1) { cy++; cx = 0; }
        break;
    case KEY_HOME:  cx = 0; break;
    case KEY_END:   cx = lines[cy].len; break;
    case KEY_DEL:   delete_char(); break;
    case 0x7f:
    case 0x08:      backspace(); break;
    case '\r':
    case '\n':      enter(); break;
    case CTRL('s'): save(); break;
    case CTRL('q'): if(quit()) return 1; break;
    case CTRL('f'): find(); break;
    case CTRL('k'): cut_line(); was_cut = 1; break;
    case CTRL('u'): paste(); break;
    case CTRL('g'): go_to_line(); break;
    case CTRL('c'):
        snprintf(msg, sizeof(msg), "Line %d of %d, column %d", cy + 1, nlines, col_of(&lines[cy], cx) + 1);
        break;
    case CTRL('l'): screen_size(); free(shadow); free(shadow_inv); shadow = NULL; shadow_ok = 0; break;
    case KEY_EOF:                                       /* no more input: nothing to ask with */
        return 2;
    default:
        if(k == '\t' || (k >= 32 && k < 0x100 && k != 0x7f)) insert_char(k);
        break;
    }
    if(!keep_col) want_col = -1;
    cutting = was_cut;
    return 0;
}

int main(int argc, char **argv) {
    int r;
    if(argc != 2) { fprintf(stderr, "usage: edit <file>\n"); return 2; }
    filename = argv[1];
    if(!isatty(0) || !isatty(1)) { fprintf(stderr, "edit: needs the console for input and output\n"); return 1; }
    if(load(filename) < 0) {
        fprintf(stderr, "edit: %s: %s%s\n", filename, strerror(errno), errno == EFBIG ? " (the limit is 1 MB)" : "");
        return 1;
    }
    screen_size();
    r = ttyctl(TTY_RAW, 1);
    if(r < 0) {
        fprintf(stderr, "edit: the console has no raw mode (%s)\n", strerror(errno));
        return 1;
    }
    raw_mode = 1;
    atexit(restore_terminal);               /* every way out goes through exit() */
    for(;;) {
        if(!shadow) {
            shadow = calloc((size_t)rows, (size_t)cols + 1);
            shadow_inv = calloc((size_t)rows, 1);
            if(!shadow || !shadow_inv) {
                free(shadow);
                free(shadow_inv);
                restore_terminal();
                fprintf(stderr, "edit: out of memory\n");
                _exit(1);
            }
            shadow_ok = 0;
        }
        refresh();
        r = get_key();
        msg[0] = 0;
        r = handle(r);
        if(r) break;
    }
    if(r == 2) {
        restore_terminal();
        fprintf(stderr, "edit: the input ended%s\n", modified ? "; the changes were not saved" : "");
        return 1;
    }
    return 0;
}
