/*
SurfOS window toolkit: a line of text
--------------------
File: gkfield.c Date: 11/05/26
--------------------
gk_field() (gk.h): one line of UTF-8, drawn as the desktop's text box (shell/guidialog.c's
field_render(): a teal border while it has the keys, the console's characters, a 2-pixel cursor)
and edited as its text boxes and Notepad are: typing replaces the selection; Backspace and Delete;
Left, Right, Home and End, with Shift to select; Ctrl+A selects everything, Ctrl+C and Ctrl+X copy
and cut through the desktop's clipboard (gk_copy(), WIN_CLIP), Ctrl+V pastes its first line
(gk_paste(), WIN_PASTE), each control character a space; a click places the cursor, a drag
selects, a double-click takes a word and a third click the whole line. Enter says GK_EV_ACTIVATE,
and every change GK_EV_CHANGE.

The cursor and the selection stay on whole characters, by the console's rules (gw_char(), the same
that draws them), so what is shown and what is edited always agree. A field holds at most its max
bytes; what would pass that is cut at a character's start. A password field (GK_PASSWORD) shows a
dot for each character, copies and cuts nothing, never remembers a line, and its bytes are zeroed
when they go: every field zeroes what was deleted, and gk_field_wipe() all of it.
11/05/26 (stage 3, the Settings review): zeroed through gk_wipe(), which no optimization drops, and
the bytes of a character typed partway (part[]) as well, once they go in and at a wipe; the state is
gkpriv.h's struct gk_line, so that test_gk looks that a wiped password leaves no byte.
*/

#include <stdlib.h>
#include <string.h>
#include "gkpriv.h"

#define FIELD_MAX (1 << 20)             /* a line's bytes at most: a field of more is made with this */
#define FIELD_HISTORY 256               /* remembered lines at most */


static int min(int a, int b) { return a < b ? a : b; }

/* the character before at, and after it (whole characters, gw_char()'s) */
static int prev_char(const struct gk_line *f, int at) {
    int k = 0, len, last = 0;
    while(k < at) {
        last = k;
        gw_char(f->buf + k, f->len - k, &len);
        k += len;
    }
    return last;
}

static int next_char(const struct gk_line *f, int at) {
    int len;
    if(at >= f->len) return f->len;
    gw_char(f->buf + at, f->len - at, &len);
    return at + len;
}

static int chars(const struct gk_line *f, int from, int to) {
    int n = 0, len;
    while(from < to) {
        gw_char(f->buf + from, f->len - from, &len);
        from += len;
        n++;
    }
    return n;
}

static int cells_of(const struct gk_widget *w) {
    return w->w > 8 ? (w->w - 8) / 8 : 0;
}

/* the first character shown so that the cursor is in view, from where the view was; and back while the text after it
   leaves room (a text set before the field had its width, a wider window) */
static int view_start(const struct gk_line *f, int cells) {
    int start = f->scroll;
    if(start > f->len) start = f->len;
    if(f->cursor < start) start = f->cursor;
    if(cells < 1) return start;
    while(chars(f, start, f->cursor) > cells - 1 && start < f->cursor) start = next_char(f, start);
    while(start > 0 && chars(f, prev_char(f, start), f->len) <= cells - 1) start = prev_char(f, start);
    return start;
}

static void changed(struct gk_widget *w, int text) {
    struct gk_line *f = w->k;
    f->scroll = view_start(f, cells_of(w));
    gk_dirty(w);
    if(text) {
        f->hpos = -1;                                   /* an edit leaves the history */
        gk_post(w->ui, GK_EV_CHANGE, w, f->len);
    }
}

static int has_selection(const struct gk_line *f) {
    return f->anchor != f->cursor;
}

/* the selection's bytes go (zeroed where the text ended) */
static void delete_range(struct gk_line *f, int from, int to) {
    if(to <= from) return;
    memmove(f->buf + from, f->buf + to, (size_t)(f->len - to));
    gk_wipe(f->buf + f->len - (to - from), (size_t)(to - from));
    f->len -= to - from;
    f->cursor = f->anchor = from;
}

static void delete_selection(struct gk_line *f) {
    delete_range(f, min(f->anchor, f->cursor), f->anchor > f->cursor ? f->anchor : f->cursor);
}

/* n bytes at the cursor, over the selection: control characters as spaces, and no more than room for whole
   characters */
static int insert(struct gk_line *f, const char *s, int n) {
    int i, k, len;
    delete_selection(f);
    if(n > f->max - f->len) {                           /* cut at a character's start */
        for(k = 0; k < n; k += len) {
            gw_char(s + k, n - k, &len);
            if(k + len > f->max - f->len) break;
        }
        n = k;
    }
    if(n <= 0) return 0;
    memmove(f->buf + f->cursor + n, f->buf + f->cursor, (size_t)(f->len - f->cursor));
    for(i = 0; i < n; i++) f->buf[f->cursor + i] = (unsigned char)s[i] < 32 ? ' ' : s[i];
    f->len += n;
    f->buf[f->len] = 0;
    f->cursor = f->anchor = f->cursor + n;
    return n;
}

static void set_text(struct gk_line *f, const char *text) {
    int n = (int)strlen(text ? text : ""), old = f->len;
    f->len = f->cursor = f->anchor = 0;
    f->buf[0] = 0;
    insert(f, text ? text : "", n);
    if(old > f->len) gk_wipe(f->buf + f->len, (size_t)(old - f->len));
}

/* a word: letters, digits, _ and characters beyond ASCII; anything else alone */
static int wordy(unsigned char c) {
    return c >= 0x80 || c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static void select_word(struct gk_line *f, int at) {
    int from = at, to = at;
    if(at >= f->len) {
        f->anchor = f->cursor = f->len;
        return;
    }
    if(!wordy((unsigned char)f->buf[at])) to = next_char(f, at);
    else {
        while(from > 0 && wordy((unsigned char)f->buf[from - 1])) from--;
        while(to < f->len && wordy((unsigned char)f->buf[to])) to++;
    }
    f->anchor = from;
    f->cursor = to;
}

static int word_left(const struct gk_line *f, int at) {
    while(at > 0 && !wordy((unsigned char)f->buf[at - 1])) at = prev_char(f, at);
    while(at > 0 && wordy((unsigned char)f->buf[at - 1])) at--;
    return at;
}

static int word_right(const struct gk_line *f, int at) {
    while(at < f->len && !wordy((unsigned char)f->buf[at])) at = next_char(f, at);
    while(at < f->len && wordy((unsigned char)f->buf[at])) at++;
    return at;
}

/* the text between where the selection starts and ends, to the clipboard */
static void copy(struct gk_widget *w) {
    struct gk_line *f = w->k;
    int from = min(f->anchor, f->cursor), to = f->anchor > f->cursor ? f->anchor : f->cursor;
    if(to > from && !(w->opts & GK_PASSWORD)) gk_copy(w->ui, f->buf + from, (unsigned)(to - from));
}

static void paste(struct gk_widget *w) {
    struct gk_line *f = w->k;
    char *clip;
    int n, k;
    if(!(clip = malloc((size_t)f->max + 1))) return;
    n = gk_paste(w->ui, clip, (unsigned)f->max + 1);
    if(n > 0) {
        for(k = 0; k < n && clip[k] && clip[k] != '\r' && clip[k] != '\n'; k++) ;   /* its first line */
        n = has_selection(f);
        if(insert(f, clip, k) || n) changed(w, 1);
    }
    gk_wipe(clip, (size_t)f->max + 1);
    free(clip);
}

/* a copy of a line (the draft, a remembered one) zeroed before it is freed: it may hold a secret (a chat's IDENTIFY) */
static void drop_copy(char **s) {
    if(!*s) return;
    gk_wipe(*s, strlen(*s));
    free(*s);
    *s = NULL;
}

static void history(struct gk_widget *w, int back) {
    struct gk_line *f = w->k;
    char *keep;
    if(!f->nhist) return;
    if(back) {
        if(f->hpos == 0) return;
        if(f->hpos < 0) {                               /* what was being typed comes back after the newest */
            if(!(keep = gk_strdup(w->ui, f->buf))) return;
            drop_copy(&f->draft);
            f->draft = keep;
            f->hpos = f->nhist;
        }
        f->hpos--;
        set_text(f, f->hist[f->hpos]);
    } else {
        if(f->hpos < 0) return;
        if(++f->hpos >= f->nhist) {                     /* the line being typed back: its copy goes */
            f->hpos = -1;
            set_text(f, f->draft ? f->draft : "");
            drop_copy(&f->draft);
        } else set_text(f, f->hist[f->hpos]);
    }
    f->scroll = view_start(f, cells_of(w));
    gk_dirty(w);
    gk_post(w->ui, GK_EV_CHANGE, w, f->len);
}

/* n bytes over the selection: 1 when the text changed */
static int put(struct gk_line *f, const char *s, int n) {
    int had = has_selection(f);
    return insert(f, s, n) > 0 || had;
}

/* a character's first bytes typed, and something else came: they go in as they are */
static int flush_part(struct gk_line *f) {
    int i, n = f->nparts, did = 0;
    f->nparts = 0;
    for(i = 0; i < n; i++) did |= put(f, f->part + i, 1);
    gk_wipe(f->part, sizeof(f->part));                  /* (a password's bytes are in it) */
    return did;
}

/* bytes typed one at a time make a character before it goes in, so that a field never holds part of one: a lead byte
   waits for its continuation bytes (the console's rules, gw_char()'s), and bytes that make none go in as they are */
static void typed(struct gk_widget *w, unsigned char c) {
    struct gk_line *f = w->k;
    int did = 0, n, len;
    if(f->nparts && (c & 0xC0) == 0x80) {
        f->part[f->nparts++] = (char)c;
        if(f->nparts < f->need) return;
        n = f->nparts;
        gw_char(f->part, n, &len);
        if(len == n) {
            f->nparts = 0;
            did = put(f, f->part, n);
            gk_wipe(f->part, sizeof(f->part));
        } else did = flush_part(f);
        if(did) changed(w, 1);
        return;
    }
    did = flush_part(f);
    if(c >= 0xC2 && c <= 0xF4) {
        f->part[0] = (char)c;
        f->nparts = 1;
        f->need = c <= 0xDF ? 2 : c <= 0xEF ? 3 : 4;
    } else {
        char b = (char)c;
        did |= put(f, &b, 1);
    }
    if(did) changed(w, 1);
}

static int field_key(struct gk_widget *w, unsigned key, unsigned mods) {
    struct gk_line *f = w->k;
    int shift = gk_shift(mods), c = gk_ctrl(key, mods), ctrl = (mods & WIN_MOD_CTRL) != 0, to;
    struct gk_event *e;
    if(f->nparts && !(key >= 0x80 && key < 0x100) && flush_part(f)) changed(w, 1);
    if(c == 'a') {
        f->anchor = 0;
        f->cursor = f->len;
        changed(w, 0);
        return 1;
    }
    if(c == 'c' || c == 'x') {
        copy(w);
        if(c == 'x' && has_selection(f) && !(w->opts & GK_PASSWORD)) {
            delete_selection(f);
            changed(w, 1);
        }
        return 1;
    }
    if(c == 'v') {
        paste(w);
        return 1;
    }
    if(c) return 0;                                     /* Ctrl and another letter: the window's */
    switch(key) {
    case '\r': case '\n':                               /* (Ctrl+Enter is WIN_KEY_ENTER: the window's) */
        if((e = gk_post_ev(w->ui, GK_EV_ACTIVATE, w, f->len))) e->mods = mods;
        return 1;
    case 8: case 127: case WIN_KEY_BACKSPACE:           /* (Ctrl+Backspace: the word before) */
        if(!has_selection(f))
            f->anchor = ctrl || key == WIN_KEY_BACKSPACE ? word_left(f, f->cursor) : prev_char(f, f->cursor);
        if(!has_selection(f)) return 1;
        delete_selection(f);
        changed(w, 1);
        return 1;
    case WIN_KEY_DELETE:                                /* (Ctrl+Delete: the word after) */
        if(!has_selection(f)) f->anchor = ctrl ? word_right(f, f->cursor) : next_char(f, f->cursor);
        if(!has_selection(f)) return 1;
        delete_selection(f);
        changed(w, 1);
        return 1;
    case WIN_KEY_LEFT: case WIN_KEY_RIGHT: case WIN_KEY_HOME: case WIN_KEY_END:
        if(key == WIN_KEY_HOME) to = 0;
        else if(key == WIN_KEY_END) to = f->len;
        else if(!shift && has_selection(f)) to = key == WIN_KEY_LEFT ? min(f->anchor, f->cursor) :
                                                 f->anchor > f->cursor ? f->anchor : f->cursor;
        else if(ctrl) to = key == WIN_KEY_LEFT ? word_left(f, f->cursor) : word_right(f, f->cursor);
        else to = key == WIN_KEY_LEFT ? prev_char(f, f->cursor) : next_char(f, f->cursor);
        f->cursor = to;
        if(!shift) f->anchor = to;
        changed(w, 0);
        return 1;
    case WIN_KEY_UP: case WIN_KEY_DOWN:
        if(!f->maxhist) return 0;
        history(w, key == WIN_KEY_UP);
        return 1;
    }
    if(key < 32 || key >= 256 || key == 127 || (mods & (WIN_MOD_CTRL | WIN_MOD_ALT))) return 0;   /* Ctrl+0, Alt+X */
    typed(w, (unsigned char)key);
    return 1;
}

/* the character boundary nearest x */
static int at_x(const struct gk_widget *w, int x) {
    const struct gk_line *f = w->k;
    int at = view_start(f, cells_of(w)), col = (x - (w->x + 4) + 4) / 8, n;
    if(x < w->x + 4) return at > 0 ? prev_char(f, at) : 0;      /* past the left edge: one more */
    for(n = 0; n < col && at < f->len; n++) at = next_char(f, at);
    return at;
}

static void field_pointer(struct gk_widget *w, int x, int y, unsigned buttons, unsigned pressed, unsigned released) {
    struct gk_line *f = w->k;
    int at;
    (void)y;
    (void)released;
    if(pressed & WIN_BUTTON_LEFT) {
        at = at_x(w, x);
        if(w->ui->clicks == 2) select_word(f, at);
        else if(w->ui->clicks >= 3) {
            f->anchor = 0;
            f->cursor = f->len;
        } else {
            f->cursor = at;
            if(!gk_shift(w->ui->pmods)) f->anchor = at;     /* Shift and a click: the selection to there */
        }
        f->dragging = w->ui->clicks == 1;
        changed(w, 0);
        return;
    }
    if(!(buttons & WIN_BUTTON_LEFT)) {
        f->dragging = 0;
        return;
    }
    if(!f->dragging) return;
    at = at_x(w, x);
    if(at != f->cursor) {
        f->cursor = at;
        changed(w, 0);
    }
}

static void field_paint(struct gk_widget *w, struct gw *win) {
    const struct gk_line *f = w->k;
    const struct gk_theme *t = &w->ui->theme;
    int lit = gk_lit(w), off = (w->flags & GKW_DISABLED) != 0, cells = cells_of(w), col, at, len, code;
    int from = min(f->anchor, f->cursor), to = f->anchor > f->cursor ? f->anchor : f->cursor, y = w->y + (w->h - 16) / 2;
    gk_frame_in(win, t, w->x, w->y, w->w, w->h, lit);
    if(off) gw_rect(win, w->x + 1, w->y + 1, w->w - 2, w->h - 2, t->band);
    gw_clip_to(win, w->x + 2, w->y + 1, w->w - 4, w->h - 2);
    if(!f->len && f->hint) gw_text_cut(win, w->x + 4, y, f->hint, cells, t->muted);
    for(at = view_start(f, cells), col = 0; at < f->len && col < cells; at += len, col++) {
        code = gw_char(f->buf + at, f->len - at, &len);
        if(at >= from && at < to) gw_rect(win, w->x + 4 + col * 8, y, 8, 16, lit ? t->pick : t->band);
        gw_glyph(win, w->x + 4 + col * 8, y, (w->opts & GK_PASSWORD) ? 0x07 : (unsigned)code, off ? t->grey : t->ink, 1);
    }
    if(lit) {
        col = chars(f, view_start(f, cells), f->cursor);
        gw_rect(win, w->x + 4 + col * 8 - 1, y, 2, 16, t->ink);
    }
}

static void field_destroy(struct gk_widget *w) {
    struct gk_line *f = w->k;
    int i;
    if(f->buf) gk_wipe(f->buf, (size_t)f->max + 1);
    free(f->buf);
    free(f->hint);
    for(i = 0; i < f->nhist; i++) drop_copy(&f->hist[i]);
    free(f->hist);
    drop_copy(&f->draft);
    gk_wipe(f->part, sizeof(f->part));
}

static const char *field_text(const struct gk_widget *w) {
    return ((const struct gk_line *)w->k)->buf;
}

static void field_set_text(struct gk_widget *w, const char *text) {
    struct gk_line *f = w->k;
    set_text(f, text);
    f->scroll = 0;
    f->scroll = view_start(f, cells_of(w));
    f->hpos = -1;
    drop_copy(&f->draft);                               /* the program's text now: what was being typed goes */
    gk_dirty(w);
}

static void field_focus(struct gk_widget *w, int on) {
    struct gk_line *f = w->k;
    if(!on) f->dragging = 0;
}

static void field_measure(struct gk_widget *w) {
    w->base_w = 16 * 8 + 8;
    w->base_h = GK_LINE_H;
}

static const struct gk_class field_class = {
    .takes_focus = 1, .measure = field_measure, .paint = field_paint, .key = field_key, .pointer = field_pointer,
    .focus = field_focus, .destroy = field_destroy, .get_text = field_text, .set_text = field_set_text,
};

struct gk_widget *gk_field(struct gk_widget *parent, int id, int max, unsigned flags) {
    struct gk_widget *w = gk_make(parent, &field_class, sizeof(struct gk_line));
    struct gk_line *f;
    if(!w) return NULL;
    f = w->k;
    f->max = max > 0 && max <= FIELD_MAX ? max : max > FIELD_MAX ? FIELD_MAX : 256;
    f->hpos = -1;
    if(!(f->buf = calloc(1, (size_t)f->max + 1))) {
        w->ui->failed = 1;
        gk_remove(w);
        return NULL;
    }
    w->id = id;
    w->opts = flags & GK_PASSWORD;
    w->grow_x = 1;
    return w;
}

void gk_field_hint(struct gk_widget *w, const char *hint) {
    if(!w || w->cls != &field_class) return;
    if(gk_set_str(w, &((struct gk_line *)w->k)->hint, hint)) gk_dirty(w);
}

void gk_field_select(struct gk_widget *w, int from, int to) {
    struct gk_line *f;
    int at;
    if(!w || w->cls != &field_class) return;
    f = w->k;
    if(to < 0 || to > f->len) to = f->len;
    if(from < 0) from = 0;
    if(from > to) from = to;
    for(at = 0; at < from; at = next_char(f, at)) ;    /* on whole characters */
    f->anchor = at;
    for(; at < to; at = next_char(f, at)) ;
    f->cursor = at;
    changed(w, 0);
}

int gk_field_history(struct gk_widget *w, int n) {
    struct gk_line *f;
    char **h;
    if(!w || w->cls != &field_class || (w->opts & GK_PASSWORD)) return -1;
    f = w->k;
    if(n < 1 || n > FIELD_HISTORY) n = n < 1 ? 0 : FIELD_HISTORY;
    if(n < f->nhist) {                                  /* the oldest go */
        int drop = f->nhist - n, i;
        for(i = 0; i < drop; i++) drop_copy(&f->hist[i]);
        memmove(f->hist, f->hist + drop, (size_t)n * sizeof(*f->hist));
        f->nhist = n;
    }
    if(n && !(h = realloc(f->hist, (size_t)n * sizeof(*h)))) {
        w->ui->failed = 1;
        return -1;
    }
    if(n) f->hist = h;
    f->maxhist = n;
    f->hpos = -1;
    return 0;
}

void gk_field_remember(struct gk_widget *w) {
    struct gk_line *f;
    char *copy;
    if(!w || w->cls != &field_class || (w->opts & GK_PASSWORD)) return;
    f = w->k;
    f->hpos = -1;
    if(!f->maxhist || !f->len || (f->nhist && !strcmp(f->hist[f->nhist - 1], f->buf))) return;
    if(!(copy = gk_strdup(w->ui, f->buf))) return;
    if(f->nhist == f->maxhist) {
        drop_copy(&f->hist[0]);
        memmove(f->hist, f->hist + 1, (size_t)(f->nhist - 1) * sizeof(*f->hist));
        f->nhist--;
    }
    f->hist[f->nhist++] = copy;
}

void gk_field_wipe(struct gk_widget *w) {
    struct gk_line *f;
    if(!w || w->cls != &field_class) return;
    f = w->k;
    gk_wipe(f->buf, (size_t)f->max + 1);                /* all of it, and a character typed partway */
    gk_wipe(f->part, sizeof(f->part));
    f->len = f->cursor = f->anchor = f->scroll = f->nparts = f->need = 0;
    drop_copy(&f->draft);
    f->hpos = -1;
    gk_dirty(w);
}

/* for a box's answer: the text (the field keeps it, and wipes it when it goes) */
void gk_field_take(struct gk_widget *w, char **text, int *len) {
    struct gk_line *f = w->k;
    *text = f->buf;
    *len = f->len;
}
