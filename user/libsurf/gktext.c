/*
SurfOS window toolkit: a text of several lines
--------------------
File: gktext.c  Date: 11/05/26
--------------------
gk_text_area() (gk.h): a small text of several lines (a note, an address list, a page of About), edited as
Notepad edits (shell/guinote.c): lines end at \n and do not wrap, the view follows the cursor sideways and down, a tab
reaches the next multiple of 8 columns; typing replaces the selection; the arrows, Home and End, Page Up and Page
Down, with Shift to select, and Ctrl with Home and End for the text's ends; Backspace and Delete; Ctrl+A, Ctrl+C,
Ctrl+X and Ctrl+V through the desktop's clipboard (all of a paste, each line end as \n, other control characters as
spaces); a click places the cursor, a drag selects (scrolling past an edge), a double-click takes a word and a third
click the line; the wheel and the scroll bar move the view. Enter starts a new line; Tab moves the focus on, unless
the area takes it (GK_TAKES_TAB: a tab). GK_READONLY shows the text, selectable and copied, but never changed.

It is for small texts: it keeps one buffer of at most its max bytes and finds lines by scanning it, so its work is in
proportion to the text's size; a program with megabytes (an editor) draws an area of its own. As in the line of text
(gkfield.c), the cursor stays on whole characters by the console's rules, bytes typed one at a time make a character
before it goes in, and what is deleted is zeroed.
*/

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "gkpriv.h"

#define AREA_MAX (1 << 20)              /* a text's bytes at most */
#define TAB 8                           /* a tab reaches the next multiple of 8 columns, as in Notepad and edit */
#define LINE_H 16

struct text {
    char *buf;                          /* the text and its NUL: max + 1 bytes */
    int len, max;
    int cursor, anchor;                 /* the selection is between them */
    int top, left;                      /* the first line and column shown */
    int goal;                           /* the column Up and Down keep (-1: the cursor's) */
    int grab, dragging, click_line;
    char part[4];
    int nparts, need;
};

static int min(int a, int b) { return a < b ? a : b; }
static int max(int a, int b) { return a > b ? a : b; }

static int next_char(const struct text *t, int at) {
    int len;
    if(at >= t->len) return t->len;
    if(t->buf[at] == '\n') return at + 1;
    gw_char(t->buf + at, t->len - at, &len);
    return at + len;
}

static int prev_char(const struct text *t, int at) {
    int k, last;
    if(at <= 0) return 0;
    k = at - 1;
    while(k > 0 && t->buf[k - 1] != '\n' && at - k < 4) k--;   /* a character is 4 bytes at most */
    for(last = k; k < at; k = next_char(t, k)) last = k;
    return last;
}

static int line_start(const struct text *t, int at) {
    while(at > 0 && t->buf[at - 1] != '\n') at--;
    return at;
}

static int line_end(const struct text *t, int at) {
    while(at < t->len && t->buf[at] != '\n') at++;
    return at;
}

static int line_of(const struct text *t, int at) {
    int n = 0, k;
    for(k = 0; k < at && k < t->len; k++) n += t->buf[k] == '\n';
    return n;
}

static int lines(const struct text *t) {
    return line_of(t, t->len) + 1;
}

static int start_of_line(const struct text *t, int line) {
    int k = 0;
    while(line > 0 && k < t->len) {
        if(t->buf[k++] == '\n') line--;
    }
    return line > 0 ? t->len : k;
}

/* the column (cells, tabs counted to their stop) of at in its line */
static int column(const struct text *t, int at) {
    int k = line_start(t, at), col = 0;
    for(; k < at; k = next_char(t, k)) col = t->buf[k] == '\t' ? (col / TAB + 1) * TAB : col + 1;
    return col;
}

/* the character boundary of a line nearest a column */
static int at_column(const struct text *t, int from, int col) {
    int k = from, c = 0, w;
    while(k < t->len && t->buf[k] != '\n') {
        w = t->buf[k] == '\t' ? (c / TAB + 1) * TAB - c : 1;
        if(c + w > col) return col - c > w / 2 ? next_char(t, k) : k;
        c += w;
        k = next_char(t, k);
    }
    return k;
}

static int rows_of(const struct gk_widget *w) {
    return max(1, (w->h - 4) / LINE_H);
}

static int cols_of(const struct gk_widget *w) {
    return max(1, (w->w - 2 - GK_SCROLL_W - 8) / 8);
}

/* the view follows the cursor */
static void reveal(struct gk_widget *w) {
    struct text *t = w->k;
    int line = line_of(t, t->cursor), col = column(t, t->cursor), rows = rows_of(w), cols = cols_of(w);
    if(line < t->top) t->top = line;
    if(line >= t->top + rows) t->top = line - rows + 1;
    if(col < t->left) t->left = col;
    if(col > t->left + cols - 1) t->left = col - cols + 1;
    if(t->top > max(0, lines(t) - 1)) t->top = max(0, lines(t) - 1);
}

static void changed(struct gk_widget *w, int text) {
    reveal(w);
    gk_dirty(w);
    if(text) gk_post(w->ui, GK_EV_CHANGE, w, ((struct text *)w->k)->len);
}

static int has_selection(const struct text *t) {
    return t->anchor != t->cursor;
}

static void delete_selection(struct text *t) {
    int from = min(t->anchor, t->cursor), to = max(t->anchor, t->cursor);
    if(to <= from) return;
    memmove(t->buf + from, t->buf + to, (size_t)(t->len - to));
    gk_wipe(t->buf + t->len - (to - from), (size_t)(to - from));
    t->len -= to - from;
    if(from < t->len && from > 0 && next_char(t, prev_char(t, from)) > from)
        from = prev_char(t, from);                      /* (the re-review, N4) on the start of a character it joined */
    t->cursor = t->anchor = from;
}

/* n bytes at the cursor over the selection: a CR LF or a lone CR as \n, other control characters but \t as spaces, no
   more than whole characters that fit; 1 when the text changed */
static int insert(struct text *t, const char *s, int n) {
    int had = has_selection(t), i, k, len, out;
    char c;
    delete_selection(t);
    for(i = k = 0; i < n; i += len) {                   /* what fits, on whole characters */
        len = s[i] == '\r' && i + 1 < n && s[i + 1] == '\n' ? 2 : 1;
        if(len == 1 && (unsigned char)s[i] >= 0x80) gw_char(s + i, n - i, &len);
        out = len == 2 && s[i] == '\r' ? 1 : len;
        if(t->len + k + out > t->max) break;
        k += out;
    }
    if(!k) return had;
    memmove(t->buf + t->cursor + k, t->buf + t->cursor, (size_t)(t->len - t->cursor));
    for(i = 0, out = t->cursor; out < t->cursor + k; i++) {
        c = s[i];
        if(c == '\r') {
            if(i + 1 < n && s[i + 1] == '\n') i++;
            c = '\n';
        } else if((unsigned char)c < 32 && c != '\n' && c != '\t') c = ' ';
        t->buf[out++] = c;
    }
    t->len += k;
    t->buf[t->len] = 0;
    t->cursor = t->anchor = t->cursor + k;
    if(t->cursor < t->len && t->cursor > 0 && next_char(t, prev_char(t, t->cursor)) > t->cursor)
        t->cursor = t->anchor = next_char(t, prev_char(t, t->cursor));   /* (the review, F13) never inside one */
    return 1;
}

static int flush_part(struct text *t) {
    int i, n = t->nparts, did = 0;
    t->nparts = 0;
    for(i = 0; i < n; i++) did |= insert(t, t->part + i, 1);
    gk_wipe(t->part, sizeof(t->part));
    return did;
}

static void typed(struct gk_widget *w, unsigned char c) {
    struct text *t = w->k;
    int did = 0, n, len;
    if(t->nparts && (c & 0xC0) == 0x80) {
        t->part[t->nparts++] = (char)c;
        if(t->nparts < t->need) return;
        n = t->nparts;
        gw_char(t->part, n, &len);
        if(len == n) {
            t->nparts = 0;
            did = insert(t, t->part, n);
            gk_wipe(t->part, sizeof(t->part));
        } else did = flush_part(t);
        if(did) changed(w, 1);
        return;
    }
    did = flush_part(t);
    if(c >= 0xC2 && c <= 0xF4) {
        t->part[0] = (char)c;
        t->nparts = 1;
        t->need = c <= 0xDF ? 2 : c <= 0xEF ? 3 : 4;
    } else {
        char b = (char)c;
        did |= insert(t, &b, 1);
    }
    if(did) changed(w, 1);
}

static int wordy(unsigned char c) {
    return c >= 0x80 || c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* a word's start before at, its end after it (the field's words: letters, digits, '_', anything past ASCII) */
static int word_left(const struct text *t, int at) {
    while(at > 0 && !wordy((unsigned char)t->buf[at - 1])) at = prev_char(t, at);
    while(at > 0 && wordy((unsigned char)t->buf[at - 1])) at--;
    return at;
}

static int word_right(const struct text *t, int at) {
    while(at < t->len && !wordy((unsigned char)t->buf[at])) at = next_char(t, at);
    while(at < t->len && wordy((unsigned char)t->buf[at])) at++;
    return at;
}

/* the selection to the clipboard: 0, or -1 when there is none or the clipboard refused it (GK_EV_UNCOPIED) */
static int copy(struct gk_widget *w) {
    struct text *t = w->k;
    int from = min(t->anchor, t->cursor), to = max(t->anchor, t->cursor);
    if(to <= from) return -1;
    if(gk_copy(w->ui, t->buf + from, (unsigned)(to - from)) == 0) return 0;
    gk_post(w->ui, GK_EV_UNCOPIED, w, errno);
    return -1;
}

static void paste(struct gk_widget *w) {
    struct text *t = w->k;
    char *clip;
    int n;
    if(!(clip = malloc((size_t)t->max + 1))) return;
    n = gk_paste(w->ui, clip, (unsigned)t->max + 1);
    if(n > 0 && insert(t, clip, n)) changed(w, 1);
    gk_wipe(clip, (size_t)t->max + 1);
    free(clip);
}

/* the cursor to at (Shift: the selection grows to it); Up and Down keep their column */
static void move(struct gk_widget *w, int at, int shift, int keep_goal) {
    struct text *t = w->k;
    t->cursor = at;
    if(!shift) t->anchor = at;
    if(!keep_goal) t->goal = -1;
    changed(w, 0);
}

static void vertical(struct gk_widget *w, int by, int shift) {
    struct text *t = w->k;
    int line = line_of(t, t->cursor), last = lines(t) - 1, to;
    if(t->goal < 0) t->goal = column(t, t->cursor);
    to = line + by < 0 ? 0 : line + by > last ? last : line + by;
    if(to == line) {
        move(w, by < 0 ? line_start(t, t->cursor) : line_end(t, t->cursor), shift, 1);
        return;
    }
    move(w, at_column(t, start_of_line(t, to), t->goal), shift, 1);
}

static int text_key(struct gk_widget *w, unsigned key, unsigned mods) {
    struct text *t = w->k;
    int shift = gk_shift(mods), c = gk_ctrl(key, mods), ctrl = (mods & WIN_MOD_CTRL) != 0, ro = (w->opts & GK_READONLY) != 0;
    if(t->nparts && !(key >= 0x80 && key < 0x100) && flush_part(t)) changed(w, 1);
    switch(c) {
    case 'a':
        t->anchor = 0;
        t->cursor = t->len;
        changed(w, 0);
        return 1;
    case 'c':
        copy(w);
        return 1;
    case 'x':                                           /* a cut takes only what the clipboard has */
        if(copy(w) == 0 && !ro) {
            delete_selection(t);
            changed(w, 1);
        }
        return 1;
    case 'v':
        if(!ro) paste(w);
        return 1;
    }
    if(c) return 0;
    switch(key) {
    case WIN_KEY_LEFT:                                  /* (Ctrl: by words, as in a field) */
        move(w, !shift && has_selection(t) ? min(t->anchor, t->cursor) : ctrl ? word_left(t, t->cursor) :
             prev_char(t, t->cursor), shift, 0);
        return 1;
    case WIN_KEY_RIGHT:
        move(w, !shift && has_selection(t) ? max(t->anchor, t->cursor) : ctrl ? word_right(t, t->cursor) :
             next_char(t, t->cursor), shift, 0);
        return 1;
    case WIN_KEY_UP: vertical(w, -1, shift); return 1;
    case WIN_KEY_DOWN: vertical(w, 1, shift); return 1;
    case WIN_KEY_PGUP: vertical(w, -(rows_of(w) - 1 > 0 ? rows_of(w) - 1 : 1), shift); return 1;
    case WIN_KEY_PGDN: vertical(w, rows_of(w) - 1 > 0 ? rows_of(w) - 1 : 1, shift); return 1;
    case WIN_KEY_HOME: move(w, ctrl ? 0 : line_start(t, t->cursor), shift, 0); return 1;
    case WIN_KEY_END: move(w, ctrl ? t->len : line_end(t, t->cursor), shift, 0); return 1;
    }
    if(ro) return 0;
    switch(key) {
    case '\r': case '\n':                               /* (Ctrl+Enter is WIN_KEY_ENTER, Ctrl+M 13 with Ctrl: the window's) */
        typed(w, '\n');
        return 1;
    case 8: case 127: case WIN_KEY_BACKSPACE:           /* (Ctrl+Backspace: the word before) */
        if(!has_selection(t)) t->anchor = ctrl || key == WIN_KEY_BACKSPACE ? word_left(t, t->cursor) : prev_char(t, t->cursor);
        if(has_selection(t)) {
            delete_selection(t);
            changed(w, 1);
        }
        return 1;
    case WIN_KEY_DELETE:                                /* (Ctrl+Delete: the word after) */
        if(!has_selection(t)) t->anchor = ctrl ? word_right(t, t->cursor) : next_char(t, t->cursor);
        if(has_selection(t)) {
            delete_selection(t);
            changed(w, 1);
        }
        return 1;
    case 9:                                             /* Tab, only when it takes Tab (gkcore.c moves the focus
                                                           else; Ctrl+I, 9 with Ctrl, never comes here) */
        typed(w, '\t');
        return 1;
    }
    if(key < 32 || key >= 256 || key == 127 || (mods & (WIN_MOD_CTRL | WIN_MOD_ALT))) return 0;   /* Ctrl+0, Alt+X */
    typed(w, (unsigned char)key);
    return 1;
}

/* the character boundary under x, y (in the window) */
static int at_point(const struct gk_widget *w, int x, int y) {
    const struct text *t = w->k;
    int line = t->top + (y - w->y - 2) / LINE_H, col = t->left + (x - w->x - 5 + 4) / 8;
    if(y < w->y + 2) line = t->top - 1;
    if(line < 0) return 0;
    if(line >= lines(t)) return t->len;
    if(col < 0) col = 0;
    return at_column(t, start_of_line(t, line), col);
}

static void text_pointer(struct gk_widget *w, int x, int y, unsigned buttons, unsigned pressed, unsigned released) {
    struct text *t = w->k;
    struct gk_bar b;
    int at, rx = w->x + w->w - 1 - GK_SCROLL_W, from, to;
    (void)released;
    b.total = lines(t);
    b.shown = rows_of(w);
    b.first = t->top;
    if(pressed & WIN_BUTTON_LEFT) {
        t->dragging = 0;
        if(x >= rx) {
            if(gk_bar_press(w->h - 2, y - w->y - 1, &b, &t->grab)) {
                t->top = b.first;
                gk_dirty(w);
            }
            return;
        }
        t->grab = -1;
        at = at_point(w, x, y);
        if(w->ui->clicks == 2) {                        /* a word */
            from = to = at;
            if(at < t->len && wordy((unsigned char)t->buf[at])) {
                while(from > 0 && wordy((unsigned char)t->buf[from - 1])) from--;
                while(to < t->len && wordy((unsigned char)t->buf[to])) to++;
            } else to = next_char(t, at);
            t->anchor = from;
            t->cursor = to;
        } else if(w->ui->clicks >= 3) {                 /* the line */
            t->anchor = line_start(t, at);
            t->cursor = line_end(t, at) < t->len ? line_end(t, at) + 1 : t->len;
        } else {
            t->cursor = at;
            if(!gk_shift(w->ui->pmods)) t->anchor = at;
            t->dragging = 1;
        }
        t->goal = -1;
        changed(w, 0);
        return;
    }
    if(!(buttons & WIN_BUTTON_LEFT)) {
        t->dragging = 0;
        t->grab = -1;
        return;
    }
    if(t->grab >= 0) {
        if(gk_bar_drag(w->h - 2, y - w->y - 1, &b, t->grab)) {
            t->top = b.first;
            gk_dirty(w);
        }
        return;
    }
    if(t->dragging && (at = at_point(w, x, y)) != t->cursor) {   /* past an edge, the view follows */
        t->cursor = at;
        changed(w, 0);
    }
}

static int text_wheel(struct gk_widget *w, int x, int y, int rows) {
    struct text *t = w->k;
    int was = t->top, last = max(0, lines(t) - rows_of(w));
    (void)x;
    (void)y;
    long long to = (long long)t->top + rows;            /* (the re-review, N7) in 64 bits */
    t->top = to < 0 ? 0 : to > last ? last : (int)to;
    if(t->top != was) gk_dirty(w);
    return 1;
}

static void text_paint(struct gk_widget *w, struct gw *win) {
    const struct text *t = w->k;
    const struct gk_theme *th = &w->ui->theme;
    struct gk_bar b;
    struct gw_view keep;
    int lit = gk_lit(w), off = (w->flags & GKW_DISABLED) != 0, rows = rows_of(w), cols = cols_of(w);
    int from = min(t->anchor, t->cursor), to = max(t->anchor, t->cursor), k, line, col, x, y, len, code;
    int rx = w->x + w->w - 1 - GK_SCROLL_W;
    gk_frame_in(win, th, w->x, w->y, w->w, w->h, lit);
    if(off || (w->opts & GK_READONLY)) gw_rect(win, w->x + 1, w->y + 1, w->w - 2, w->h - 2, off ? th->band : th->paper);
    gw_get_view(win, &keep);
    gw_clip_to(win, w->x + 1, w->y + 1, rx - w->x - 1, w->h - 2);
    k = start_of_line(t, t->top);
    for(line = t->top; line < t->top + rows + 1 && k <= t->len; line++) {
        y = w->y + 2 + (line - t->top) * LINE_H;
        for(col = 0; k < t->len && t->buf[k] != '\n'; k += len) {
            code = t->buf[k] == '\t' ? ' ' : gw_char(t->buf + k, t->len - k, &len);
            if(t->buf[k] == '\t') len = 1;
            x = w->x + 5 + (col - t->left) * 8;
            if(t->buf[k] == '\t') {
                int stop = (col / TAB + 1) * TAB;
                if(k >= from && k < to) gw_rect(win, x, y, (stop - col) * 8, LINE_H, lit ? th->pick : th->band);
                col = stop;
                continue;
            }
            if(col >= t->left && col < t->left + cols + 1) {
                if(k >= from && k < to) gw_rect(win, x, y, 8, LINE_H, lit ? th->pick : th->band);
                gw_glyph(win, x, y, (unsigned)code, off ? th->muted : th->ink, 1);
            }
            col++;
        }
        if(k < t->len && k >= from && k < to)          /* a selected line end: a cell of the colour */
            gw_rect(win, w->x + 5 + (col - t->left) * 8, y, 8, LINE_H, lit ? th->pick : th->band);
        if(k >= t->len) break;
        k++;
    }
    if(lit && !(w->opts & GK_READONLY)) {
        line = line_of(t, t->cursor);
        col = column(t, t->cursor);
        if(line >= t->top && line < t->top + rows + 1)
            gw_rect(win, w->x + 5 + (col - t->left) * 8 - 1, w->y + 2 + (line - t->top) * LINE_H, 2, LINE_H, th->ink);
    }
    gw_set_view(win, &keep);
    b.total = lines(t);
    b.shown = rows;
    b.first = t->top;
    gk_bar_paint(win, th, rx, w->y + 1, w->h - 2, 0, &b);
}

static void text_measure(struct gk_widget *w) {
    w->base_w = 24 * 8 + 8 + GK_SCROLL_W;
    w->base_h = 4 * LINE_H + 4;
}

static void text_arrange(struct gk_widget *w) {
    struct text *t = w->k;
    int last = max(0, lines(t) - rows_of(w));
    if(t->top > last) t->top = last;                    /* a taller view shows more of the end */
}

static void text_destroy(struct gk_widget *w) {
    struct text *t = w->k;
    if(t->buf) gk_wipe(t->buf, (size_t)t->max + 1);
    free(t->buf);
    gk_wipe(t->part, sizeof(t->part));
}

static const char *text_get(const struct gk_widget *w) {
    return ((const struct text *)w->k)->buf;
}

static void text_set(struct gk_widget *w, const char *s) {
    struct text *t = w->k;
    int old = t->len;
    t->len = t->cursor = t->anchor = 0;
    t->buf[0] = 0;
    insert(t, s ? s : "", (int)strlen(s ? s : ""));
    if(old > t->len) gk_wipe(t->buf + t->len, (size_t)(old - t->len));
    t->cursor = t->anchor = 0;
    t->top = t->left = 0;
    t->goal = -1;
    gk_dirty(w);
}

static void text_focus(struct gk_widget *w, int on) {
    if(!on) ((struct text *)w->k)->dragging = 0;
}

static const struct gk_class text_class = {
    .takes_focus = 1, .measure = text_measure, .arrange = text_arrange, .paint = text_paint, .key = text_key,
    .pointer = text_pointer, .wheel = text_wheel, .focus = text_focus, .destroy = text_destroy, .get_text = text_get,
    .set_text = text_set,
};

struct gk_widget *gk_text_area(struct gk_widget *parent, int id, int max_bytes, unsigned flags) {
    struct gk_widget *w = gk_make(parent, &text_class, sizeof(struct text));
    struct text *t;
    if(!w) return NULL;
    t = w->k;
    t->max = max_bytes > 0 && max_bytes <= AREA_MAX ? max_bytes : max_bytes > AREA_MAX ? AREA_MAX : 4096;
    t->goal = t->grab = -1;
    if(!(t->buf = calloc(1, (size_t)t->max + 1))) {
        w->ui->failed = 1;
        gk_remove(w);
        return NULL;
    }
    w->id = id;
    w->opts = flags & (GK_READONLY | GK_TAKES_TAB);
    w->grow_x = w->grow_y = 1;
    return w;
}

void gk_text_select(struct gk_widget *w, int from, int to) {
    struct text *t;
    int at;
    if(!w || w->cls != &text_class) return;
    t = w->k;
    if(to < 0 || to > t->len) to = t->len;
    if(from < 0) from = 0;
    if(from > to) from = to;
    for(at = 0; at < from; at = next_char(t, at)) ;
    t->anchor = at;
    for(; at < to; at = next_char(t, at)) ;
    t->cursor = at;
    t->goal = -1;
    changed(w, 0);
}

void gk_text_cursor(const struct gk_widget *w, int *from, int *to) {
    const struct text *t = w && w->cls == &text_class ? w->k : NULL;
    *from = t ? min(t->anchor, t->cursor) : 0;
    *to = t ? max(t->anchor, t->cursor) : 0;
}
