/*
SurfOS Desktop: Log
--------------------
File: guilog.c Date: 10/17/26
--------------------
The kernel log (dmesg) in a window. struct gui_sys's log gives a copy of the log's last 16 KB and how
many bytes the kernel ever wrote; the window reads it again twice a second while it shows. A place in
the copy is counted from the first byte ever written (as `written` is), so the view, the selection and
a match stay on their text when the log wraps around (more than 16 KB written) and its oldest lines
drop off the front: what dropped off is gone, and a place before the copy's start is that start.
Long lines wrap at the window's edge, their later rows two cells in. Characters are drawn as on the
console: UTF-8 as code page 437 (a byte that is not UTF-8 as the 437 character it is), a tab to the
next multiple of 8 columns, other control characters and colour sequences not at all.
Follow keeps the newest lines in view; scrolling up and selecting lines stop it, and End, Follow in
the toolbar, or scrolling back down to the end start it again. Find is Notepad's find bar; Copy puts
the selected lines on the desktop's clipboard.
Track TOOLS of docs/superpowers/plans/2026-10-17-gui-suite.md; the window manager reaches it
through gui_app_log (struct gui_app, shell/guipriv.h).
*/
#include "gui_tools.h"
#include <string.h>
#include <stdio.h>

#define SHEET 0xFFFDF5                  /* the text's background, as Notepad's */
#define MATCH 0xE8D08A                  /* the match found: the folders' yellow */
#define INDENT 2                        /* a long line's later rows start two cells in */
#define READ_MS 500                     /* the log is read again twice a second */
#define DRAG_LINES 1                    /* the held button selects lines */
#define DRAG_THUMB 2                    /* or holds the scroll bar's thumb */
#define DRAG_MS 50                      /* 10/18/26: held past the text's edge, the selection scrolls a row this often */

enum { T_FOLLOW = 1, T_FIND, T_COPY, T_ALL };
static const struct gui_tool tools[] = {
    { "Follow", T_FOLLOW }, { "Find", T_FIND }, { "Copy", T_COPY }, { "Select all", T_ALL },
};
#define TOOLS (int)(sizeof(tools) / sizeof(tools[0]))

static const char *const help[] = {
    "#Log",
    "The kernel log, as dmesg shows it, read again twice a",
    "second. Follow (End) keeps the newest lines in view;",
    "scrolling up or selecting lines stops it. Up, Down,",
    "Page Up, Page Down, Home, End and the wheel scroll, and",
    "long lines wrap. Click a line to select it, drag for",
    "more: Ctrl+C copies them, Ctrl+A selects all. Ctrl+F",
    "finds (Enter: the next one; Esc closes the bar).",
};

struct log_layout { int ty, rows, cols, bar_x, find_y, status_y; };

static void layout(const struct gui_state *g, struct log_layout *l) {
    const struct gui_window *w = &g->win[GUI_LOG];
    l->ty = TITLE_H + BAR_H + 4;
    l->status_y = w->h - STATUS_H - 1;
    l->find_y = g->logview.finding ? l->status_y - 28 : l->status_y;
    l->rows = (l->find_y - l->ty - 2) / 16;
    if(l->rows < 1) l->rows = 1;
    l->bar_x = w->w - 2 - SCROLL_W;
    l->cols = (l->bar_x - 12) / 8;                      /* from 8 pixels in, clear of the scroll bar */
    if(l->cols < INDENT + 8) l->cols = INDENT + 8;
}

static void status(struct gui_state *g, const char *msg) {
    strlcpy(g->logview.status, msg, sizeof(g->logview.status));
    g->dirty = true;
}

/**** places in the copy ****/

static u32 base(const struct gui_logview *v) { return v->written - v->len; }

/* a place as an index into the copy: one that dropped off the front is its start, one past its end its end */
static u32 index_of(const struct gui_logview *v, u32 pos) {
    i32 d = (i32)(pos - base(v));
    return d < 0 ? 0 : (u32)d > v->len ? v->len : (u32)d;
}

static bool dropped(const struct gui_logview *v, u32 pos) { return (i32)(pos - base(v)) < 0; }

static u32 place(const struct gui_logview *v, u32 i) { return base(v) + i; }

static u32 line_begin(const struct gui_logview *v, u32 i) {
    while(i > 0 && v->text[i - 1] != '\n') i--;
    return i;
}

static u32 line_finish(const struct gui_logview *v, u32 i) {
    while(i < v->len && v->text[i] != '\n') i++;
    return i;
}

static u32 next_line(const struct gui_logview *v, u32 i) {
    i = line_finish(v, i);
    return i < v->len ? i + 1 : v->len;
}

/* the line a place is in (where it starts); the last line for one at the end, 0 when there is no text */
static u32 line_of(const struct gui_logview *v, u32 pos) {
    u32 i = index_of(v, pos);
    if(i >= v->len) i = v->len ? v->len - 1 : 0;
    return line_begin(v, i);
}

/* 10/18/26: the first line whose start is in the copy: once the log has wrapped round, its first bytes are usually what
   is left of a line (the copy's end if no line starts in it) */
static u32 first_whole(const struct gui_logview *v) {
    u32 i;
    if(v->written == v->len) return 0;
    for(i = 0; i < v->len && v->text[i] != '\n'; i++) ;
    return i < v->len ? i + 1 : v->len;
}

/* lines that start from index a up to index b (the copy's start counts as one) */
static int lines_between(const struct gui_logview *v, u32 a, u32 b) {
    int n = 0;
    u32 i;
    for(i = a; i < v->len && i <= b; i = next_line(v, i)) n++;
    return n;
}

/**** characters, cells and rows ****/

/* the character at i (before end): its bytes, and in *cells the cells it takes at column col */
static u32 step(const char *t, u32 i, u32 end, int col, int *cells) {
    u8 c = (u8)t[i];
    u32 k;
    if(c == '\t') { *cells = TAB_W - col % TAB_W; return 1; }
    if(c == 27 && i + 1 < end && t[i + 1] == '[') {    /* a colour sequence: ESC [, parameters, a final byte */
        for(k = i + 2; k < end && (u8)t[k] >= 0x20 && (u8)t[k] <= 0x3F; k++) ;
        *cells = 0;
        return (k < end ? k + 1 : end) - i;
    }
    *cells = c < 32 || c == 127 ? 0 : 1;
    return *cells ? gd_char_len(t + i, end - i) : 1;
}

/* the cells of the line from index i to end */
static int line_cells(const struct gui_logview *v, u32 i, u32 end) {
    int col = 0, cells;
    while(i < end) { i += step(v->text, i, end, col, &cells); col += cells; }
    return col;
}

static int rows_for(int cells, int cols) {
    return cells <= cols ? 1 : 1 + (cells - cols + cols - INDENT - 1) / (cols - INDENT);
}

static int line_rows(const struct gui_logview *v, u32 i, int cols) {
    return rows_for(line_cells(v, i, line_finish(v, i)), cols);
}

/* where column col of a line is drawn: its row in the line, and the cell in that row */
static void cell_at(int col, int cols, int *row, int *x) {
    if(col < cols) { *row = 0; *x = col; return; }
    *row = 1 + (col - cols) / (cols - INDENT);
    *x = INDENT + (col - cols) % (cols - INDENT);
}

/* the rows of all the lines at cols columns, and in *at the rows before the line that starts at index line */
static int count_rows(const struct gui_logview *v, int cols, u32 line, int *at) {
    u32 i;
    int n = 0;
    *at = 0;
    for(i = 0; i < v->len; i = next_line(v, i)) {
        if(i == line) *at = n;
        n += line_rows(v, i, cols);
    }
    return n;
}

/* the line that a row (from the top of all of them) is in, and in *sub that row's place in the line */
static u32 row_line(const struct gui_logview *v, int cols, int row, int *sub) {
    u32 i, next;
    int rows;
    if(row < 0) row = 0;
    for(i = 0; i < v->len; i = next) {
        next = next_line(v, i);
        rows = line_rows(v, i, cols);
        if(row < rows || next >= v->len) { *sub = row < rows ? row : rows - 1; return i; }
        row -= rows;
    }
    *sub = 0;
    return 0;
}

/**** the view ****/

/* the view's first row (Follow: the last page's), and in *total all the rows there are */
static int first_row(const struct gui_state *g, const struct log_layout *l, int *total) {
    const struct gui_logview *v = &g->logview;
    u32 top = line_of(v, v->top);
    int at, rows, first, max;
    *total = count_rows(v, l->cols, top, &at);
    max = *total > l->rows ? *total - l->rows : 0;
    if(v->follow || !v->len) return max;
    rows = line_rows(v, top, l->cols);
    first = at + (v->top_row < rows ? v->top_row : rows - 1);
    return first < 0 ? 0 : first > max ? max : first;
}

void log_view(const struct gui_state *g, int *first, int *rows, int *total) {
    struct log_layout l;
    layout(g, &l);
    *first = first_row(g, &l, total);
    *rows = l.rows;
}

int log_line_text(const struct gui_state *g, int row, char *out, u32 n) {
    const struct gui_logview *v = &g->logview;
    struct log_layout l;
    int first, total, sub;
    u32 line, end;
    layout(g, &l);
    first = first_row(g, &l, &total);
    if(row < 0 || first + row >= total) return -1;
    line = row_line(v, l.cols, first + row, &sub);
    end = line_finish(v, line);
    if(end - line >= n) end = line + n - 1;
    memcpy(out, v->text + line, end - line);
    out[end - line] = 0;
    return (int)(end - line);
}

static void set_first(struct gui_state *g, const struct log_layout *l, int first) {
    struct gui_logview *v = &g->logview;
    int sub;
    u32 i = row_line(v, l->cols, first, &sub);
    v->top = place(v, i);
    v->top_row = sub;
}

/* the view to a first row: Follow stops when the view moves up, and starts again when it is scrolled down to the end
   (10/18/26: also when it was there already) */
static void scroll_to(struct gui_state *g, int first) {
    struct gui_logview *v = &g->logview;
    struct log_layout l;
    int total, old, max, want = first;
    layout(g, &l);
    old = first_row(g, &l, &total);
    max = total > l.rows ? total - l.rows : 0;
    first = gui_clamp(first, 0, max);
    if(first < old) v->follow = false;
    else if(want > old && first == max) v->follow = true;
    set_first(g, &l, first);
    g->dirty = true;
}

/* Follow on (the view goes to the end) or off (it stays where it is) */
static void follow(struct gui_state *g, bool on) {
    struct gui_logview *v = &g->logview;
    struct log_layout l;
    int total, first;
    layout(g, &l);
    first = first_row(g, &l, &total);
    v->follow = on;
    if(!on) set_first(g, &l, first);
    g->dirty = true;
}

/**** reading ****/

/* the log again: what came since the last read, and what dropped off the front meanwhile */
static void read_log(struct gui_state *g) {
    struct gui_logview *v = &g->logview;
    u32 len = 0, written = 0, old_len = v->len, old_written = v->written, after = 0;
    int r, old_error = v->error;
    bool up = (i32)(v->anchor - v->cursor) <= 0;
    v->read_ms = g->now;
    if(!g->sys || !g->sys->log) return;
    if(v->any && !dropped(v, up ? v->anchor : v->cursor))   /* 10/18/26: where the line after the selection's first */
        after = place(v, next_line(v, index_of(v, up ? v->anchor : v->cursor)));   /* starts, before the copy changes */
    r = g->sys->log(g->sys->ctx, v->text, GUI_LOG_MAX, &len, &written);
    if(r < 0) { len = 0; written = old_written; }
    if(len > GUI_LOG_MAX) len = GUI_LOG_MAX;            /* 10/18/26: written is counted modulo 2^32, never clamped */
    v->len = len;
    v->written = written;
    v->error = r < 0 ? r : 0;
    if(len == old_len && written == old_written && v->error == old_error) return;
    if(dropped(v, v->top)) { v->top = base(v); v->top_row = 0; }   /* nothing stale: the view, the match, the selection */
    if(v->match_len && dropped(v, v->match)) v->match_len = 0;
    if(v->any) {                                        /* 10/18/26: it keeps the lines it still has whole: */
        u32 lo = up ? v->anchor : v->cursor, hi = up ? v->cursor : v->anchor, whole;
        if(dropped(v, hi)) v->any = false;
        else if(dropped(v, lo)) {                       /* from the line after its first, when that is still there */
            whole = after && !dropped(v, after) ? after : place(v, first_whole(v));
            lo = (i32)(whole - hi) <= 0 ? whole : hi;
            if(up) v->anchor = lo;
            else v->cursor = lo;
        }
    }
    if(!g->win[GUI_LOG].min) g->dirty = true;
}

/**** what the toolbar, the keys and the Edit menu do ****/

static void copy(struct gui_state *g) {
    struct gui_logview *v = &g->logview;
    char msg[GUI_STATUS];
    u32 a, b, end;
    int n;
    if(!v->any || !v->len) { status(g, "Select some lines first: click one, or drag"); return; }
    a = line_of(v, v->anchor);
    b = line_of(v, v->cursor);
    if(a > b) { end = a; a = b; b = end; }
    end = next_line(v, b);
    if(end - a > GUI_CLIP_MAX) end = a + GUI_CLIP_MAX;  /* never: the log is 16 KB */
    memcpy(g->clip, v->text + a, end - a);
    g->clip_len = end - a;
    n = lines_between(v, a, b);
    snprintf(msg, sizeof(msg), "Copied %d line%s (%u bytes)", n, n == 1 ? "" : "s", end - a);
    status(g, msg);
}

static void select_all(struct gui_state *g) {
    struct gui_logview *v = &g->logview;
    if(!v->len) return;
    follow(g, false);                                   /* 10/18/26: as any selecting does */
    v->anchor = place(v, 0);
    v->cursor = place(v, line_begin(v, v->len - 1));
    v->any = true;
    v->status[0] = 0;
    g->dirty = true;
}

static void find_open(struct gui_state *g) {
    struct gui_logview *v = &g->logview;
    field_set(&v->find, v->find.text, true);            /* the last search, selected: typing replaces it */
    v->finding = true;
    g->dirty = true;
}

static char lower(char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static bool cont(u8 c) { return (c & 0xC0) == 0x80; }

/* a match at index i, n bytes: its line is selected and comes into view; Follow stops */
static void found(struct gui_state *g, u32 i, u32 n, bool again) {
    struct gui_logview *v = &g->logview;
    struct log_layout l;
    u32 line = line_begin(v, i);
    int total, first, at, row, x;
    layout(g, &l);
    first = first_row(g, &l, &total);
    v->match = place(v, i);
    v->match_len = n;
    v->anchor = v->cursor = place(v, line);
    v->any = true;
    count_rows(v, l.cols, line, &at);
    cell_at(line_cells(v, line, i), l.cols, &row, &x);
    row += at;
    if(row < first) first = row;
    else if(row >= first + l.rows) first = row - l.rows + 1;
    v->follow = false;
    set_first(g, &l, first);
    status(g, again ? "Found it again from the top" : "");
}

/* the next match (letters in either case): after the last one, or from the selection or the view's top, and then
   round from the top */
static bool find_next(struct gui_state *g) {
    struct gui_logview *v = &g->logview;
    struct log_layout l;
    char msg[GUI_STATUS];
    u32 n = v->find.len, from, i, k, pass, a, b;
    int total, sub;
    if(!n) return false;
    layout(g, &l);
    if(v->match_len && !dropped(v, v->match) && index_of(v, v->match) < v->len) from = index_of(v, v->match) + v->match_len;
    else if(v->any) {
        a = line_of(v, v->anchor);
        b = line_of(v, v->cursor);
        from = a < b ? a : b;
    } else from = row_line(v, l.cols, first_row(g, &l, &total), &sub);
    if(from > v->len) from = v->len;
    for(pass = 0; pass < 2; pass++) {
        u32 start = pass ? 0 : from, limit = pass ? from : v->len;
        for(i = start; i < limit && i + n <= v->len; i++) {
            for(k = 0; k < n && lower(v->text[i + k]) == lower(v->find.text[k]); k++) ;
            if(k < n || cont((u8)v->text[i]) || (i + n < v->len && cont((u8)v->text[i + n]))) continue;  /* whole characters */
            found(g, i, n, pass);
            return true;
        }
    }
    snprintf(msg, sizeof(msg), "Not found: %s", v->find.text);
    status(g, msg);
    return false;
}

static bool tool_on(const struct gui_state *g, int act) {
    const struct gui_logview *v = &g->logview;
    switch(act) {
    case T_COPY: return v->any && v->len;
    case T_ALL: return v->len > 0;
    }
    return true;
}

static void tool(struct gui_state *g, int act) {
    switch(act) {
    case T_FOLLOW: follow(g, !g->logview.follow); break;
    case T_FIND: find_open(g); break;
    case T_COPY: copy(g); break;
    case T_ALL: select_all(g); break;
    }
}

static bool can_edit(const struct gui_state *g, int act) {
    const struct gui_logview *v = &g->logview;
    switch(act) {
    case ACT_COPY: return v->any && v->len;
    case ACT_SELECT_ALL: return v->len > 0;
    case ACT_FIND: return true;
    case ACT_FIND_NEXT: return v->find.len > 0;
    }
    return false;
}

static void edit(struct gui_state *g, int act) {
    switch(act) {
    case ACT_COPY: copy(g); break;
    case ACT_SELECT_ALL: select_all(g); break;
    case ACT_FIND: find_open(g); break;
    case ACT_FIND_NEXT: find_next(g); break;
    }
}

/**** the window ****/

static void select_to(struct gui_state *g, int y);

static void opened(struct gui_state *g) {
    struct gui_logview *v = &g->logview;
    v->len = v->written = 0;
    v->error = 0;
    v->follow = true;
    v->finding = v->any = false;
    v->top = 0;
    v->top_row = 0;
    v->match_len = 0;
    v->drag = 0;
    v->status[0] = 0;
    read_log(g);
}

static void closed(struct gui_state *g) {
    g->logview.drag = 0;
    g->logview.finding = false;
}

/* read again about twice a second while it shows (minimized, it is read again when it comes back) */
static void tick(struct gui_state *g) {
    struct gui_logview *v = &g->logview;
    struct log_layout l;
    if(g->app_drag != GUI_LOG || g->win[GUI_LOG].min) v->drag = 0;   /* 10/18/26: a drag the desktop ended (the screen
                                                                        came back from another console) or tucked away */
    if(v->drag == DRAG_LINES && g->now - v->drag_ms >= DRAG_MS) {   /* 10/18/26: held past an edge, it scrolls on */
        layout(g, &l);
        if(v->drag_y < l.ty || v->drag_y >= l.ty + l.rows * 16) {
            v->drag_ms = g->now;
            select_to(g, v->drag_y);
        }
    }
    if(g->win[GUI_LOG].min || !g->sys) return;
    if(g->now - v->read_ms >= READ_MS) read_log(g);
}

static bool key(struct gui_state *g, int key) {
    struct gui_logview *v = &g->logview;
    struct log_layout l;
    int first, total, page;
    if(v->finding) {                                    /* the find bar's keys; the view still scrolls */
        if(key == 27) { v->finding = false; g->dirty = true; return true; }
        if(key == '\r' || key == '\n') { find_next(g); return true; }
        if(key != GUI_UP && key != GUI_DOWN && key != GUI_PGUP && key != GUI_PGDN &&
           field_key(&v->find, key, g->clip, g->clip_len)) {
            g->dirty = true;
            return true;
        }
    }
    layout(g, &l);
    first = first_row(g, &l, &total);
    page = l.rows > 1 ? l.rows - 1 : 1;
    switch(key) {
    case GUI_UP: scroll_to(g, first - 1); break;
    case GUI_DOWN: scroll_to(g, first + 1); break;
    case GUI_PGUP: scroll_to(g, first - page); break;
    case GUI_PGDN: scroll_to(g, first + page); break;
    case GUI_HOME: scroll_to(g, 0); break;
    case GUI_END: follow(g, true); break;
    default: return false;                              /* Esc closes the window, Ctrl+C copies (the Edit menu) */
    }
    v->status[0] = 0;
    return true;
}

static void press(struct gui_state *g, int x, int y) {
    struct gui_logview *v = &g->logview;
    const struct gui_window *w = &g->win[GUI_LOG];
    struct log_layout l;
    int i, first, total, top, size, row, sub;
    layout(g, &l);
    v->drag = 0;
    i = tool_hit(tools, TOOLS, x, y);
    if(i >= 0) { if(tool_on(g, tools[i].act)) tool(g, tools[i].act); return; }
    if(v->finding && y >= l.find_y && y < l.status_y) {
        if(x >= w->w - 34) v->finding = false;
        else if(x >= w->w - 100) find_next(g);
        g->dirty = true;
        return;
    }
    if(y < l.ty - 2 || y >= l.find_y) return;
    first = first_row(g, &l, &total);
    if(x >= l.bar_x) {                                  /* the scroll bar: hold the thumb, or page */
        gd_thumb(l.find_y - l.ty + 2, first, l.rows, total, &top, &size);
        if(y - (l.ty - 2) < top) scroll_to(g, first - l.rows);
        else if(y - (l.ty - 2) >= top + size) scroll_to(g, first + l.rows);
        else { v->drag = DRAG_THUMB; v->thumb_at = y - (l.ty - 2) - top; }
        return;
    }
    v->finding = false;                                 /* a click in the text: the keys are the view's again */
    v->status[0] = 0;
    g->dirty = true;
    row = y >= l.ty ? (y - l.ty) / 16 : 0;
    if(row >= l.rows) return;                           /* 10/18/26: the strip below the last whole row shown */
    if(first + row >= total) { v->any = false; return; }   /* below the last line: nothing is selected */
    v->follow = false;                                  /* the lines stay put while they are chosen */
    set_first(g, &l, first);
    v->anchor = v->cursor = place(v, row_line(v, l.cols, first + row, &sub));
    v->any = true;
    v->drag = DRAG_LINES;
    v->drag_y = y;
    v->drag_ms = g->now;
}

/* the held button's lines reach the row at y (window-relative); past the text's top or bottom edge the view scrolls a
   row first */
static void select_to(struct gui_state *g, int y) {
    struct gui_logview *v = &g->logview;
    struct log_layout l;
    u32 top = v->top, cursor = v->cursor;
    int first, total, row, sub, max, top_row = v->top_row;
    layout(g, &l);
    first = first_row(g, &l, &total);
    max = total > l.rows ? total - l.rows : 0;
    if(y < l.ty && first > 0) set_first(g, &l, --first);                         /* past the edge: the view scrolls */
    else if(y >= l.ty + l.rows * 16 && first < max) set_first(g, &l, ++first);
    row = y < l.ty ? 0 : (y - l.ty) / 16;
    if(row >= l.rows) row = l.rows - 1;
    row += first;
    if(row >= total) row = total - 1;
    if(row >= 0) v->cursor = place(v, row_line(v, l.cols, row, &sub));
    if(v->top != top || v->top_row != top_row || v->cursor != cursor) g->dirty = true;   /* 10/18/26: only a change */
}

static void drag(struct gui_state *g, int x, int y) {
    struct gui_logview *v = &g->logview;
    struct log_layout l;
    int total;
    (void)x;
    if(v->drag == DRAG_THUMB) {
        layout(g, &l);
        first_row(g, &l, &total);
        scroll_to(g, gd_scroll_to(l.find_y - l.ty + 2, y - (l.ty - 2) - v->thumb_at, l.rows, total));
        return;
    }
    if(v->drag != DRAG_LINES) return;
    v->drag_y = y;
    v->drag_ms = g->now;
    select_to(g, y);
}

static void release(struct gui_state *g) {
    g->logview.drag = 0;
}

static void wheel(struct gui_state *g, int rows) {
    int first, shown, total;
    log_view(g, &first, &shown, &total);
    scroll_to(g, first + rows);
}

/**** drawing ****/

static void render(const struct gui_state *g, const struct gui_draw *d) {
    const struct gui_window *w = &g->win[GUI_LOG];
    const struct gui_logview *v = &g->logview;
    struct log_layout l;
    struct gui_draw text;
    bool on[TOOLS], focused = g->focus == GUI_LOG && !g->dialog.kind && !g->menu;
    char b[GUI_STATUS + 48];
    const char *right = v->follow ? "Follow on" : "Follow off: End";
    int x0 = w->x, y0 = w->y, first, total, row, sub, rows, i, col, c, r, cx, x, lines;
    u32 line, end, k, n, len, a = 0, z = 0, m0 = 0, m1 = 0;
    layout(g, &l);
    for(i = 0; i < TOOLS; i++) on[i] = tool_on(g, tools[i].act);
    gd_rect(d, x0 + 1, y0 + TITLE_H, w->w - 2, BAR_H + 2, BAND);
    tool_render(d, x0, y0, tools, TOOLS, on);
    if(v->follow)                                       /* Follow is the toolbar's first button: lit while on */
        gd_button(d, x0 + 8, y0 + TITLE_H + 4, (int)strlen(tools[0].label) * 8 + 14, 22, tools[0].label, TEAL, PAPER);
    gd_rect(d, x0 + 1, y0 + TITLE_H + BAR_H + 2, w->w - 2, 1, LINE);
    gd_rect(d, x0 + 1, y0 + l.ty - 1, l.bar_x - 1, l.find_y - l.ty + 1, SHEET);
    text = gd_clip(d, x0 + 1, y0 + l.ty, l.bar_x - 1, l.rows * 16);
    first = first_row(g, &l, &total);
    if(!g->sys) gd_text(&text, x0 + 8, y0 + l.ty, "No kernel log: this desktop has no system.", MUTED);
    else if(v->error) gd_text(&text, x0 + 8, y0 + l.ty, "The kernel log cannot be read.", CORAL);
    else if(!v->len) gd_text(&text, x0 + 8, y0 + l.ty, "The kernel log is empty.", MUTED);
    if(v->any) {
        a = line_of(v, v->anchor);
        z = line_of(v, v->cursor);
        if(a > z) { line = a; a = z; z = line; }
    }
    if(v->match_len && !dropped(v, v->match)) { m0 = index_of(v, v->match); m1 = m0 + v->match_len; }
    line = row_line(v, l.cols, first, &sub);
    for(row = 0; row < l.rows && line < v->len; line = next_line(v, line)) {
        end = line_finish(v, line);
        rows = rows_for(line_cells(v, line, end), l.cols);
        if(v->any && line >= a && line <= z)
            gd_rect(&text, x0 + 1, y0 + l.ty + row * 16, l.bar_x - 1, (rows - sub) * 16, focused ? PICK : BAND);
        for(k = line, col = 0; k < end; k += n, col += c) {
            n = step(v->text, k, end, col, &c);
            if(!c || v->text[k] == '\t') continue;
            cell_at(col, l.cols, &r, &cx);
            if(r < sub || r - sub + row >= l.rows) continue;
            if(k >= m0 && k < m1) gd_rect(&text, x0 + 8 + cx * 8, y0 + l.ty + (row + r - sub) * 16, 8, 16, MATCH);
            gd_glyph(&text, x0 + 8 + cx * 8, y0 + l.ty + (row + r - sub) * 16, gd_char(v->text + k, end - k, &len), INK, 1);
        }
        row += rows - sub;
        sub = 0;
    }
    gd_scrollbar(d, x0 + l.bar_x, y0 + l.ty - 2, l.find_y - l.ty + 2, first, l.rows, total);
    if(v->finding) {
        gd_rect(d, x0 + 1, y0 + l.find_y, w->w - 2, 28, BAND);
        gd_rect(d, x0 + 1, y0 + l.find_y, w->w - 2, 1, LINE);
        gd_text(d, x0 + 10, y0 + l.find_y + 6, "Find:", INK);
        field_render(d, &v->find, x0 + 56, y0 + l.find_y + 3, w->w - 56 - 108, !g->dialog.kind && g->focus == GUI_LOG);
        gd_button(d, x0 + w->w - 100, y0 + l.find_y + 3, 60, 22, "Next", TEAL, PAPER);
        gd_button(d, x0 + w->w - 34, y0 + l.find_y + 3, 24, 22, "x", SHELL, INK);
    }
    gd_rect(d, x0 + 1, y0 + l.status_y, w->w - 2, STATUS_H, BAND);
    gd_rect(d, x0 + 1, y0 + l.status_y, w->w - 2, 1, LINE);
    lines = lines_between(v, 0, v->len);
    if(v->status[0]) strlcpy(b, v->status, sizeof(b));
    else if(v->any) snprintf(b, sizeof(b), "%d line%s, %d selected", lines, lines == 1 ? "" : "s", lines_between(v, a, z));
    else snprintf(b, sizeof(b), "%d line%s%s", lines, lines == 1 ? "" : "s", v->written != v->len ? ", the newest 16 KB" : "");
    x = w->w - 24 - (int)strlen(right) * 8;             /* clear of the resize grip */
    gd_name(d, x0 + 10, y0 + l.status_y + 3, b, (x - 18) / 8, INK);
    gd_text(d, x0 + x, y0 + l.status_y + 3, right, v->follow ? TEAL : MUTED);
}

const struct gui_app gui_app_log = {
    .title = "Log", .icon = ICON_LOG, .shortcut = "", .w = 600, .h = 380,
    .resizable = true, .arrows = true, .keys = false,
    .opened = opened, .closed = closed, .render = render, .key = key,
    .press = press, .drag = drag, .release = release, .wheel = wheel, .tick = tick,
    .help = help, .help_lines = (int)(sizeof(help) / sizeof(help[0])),
    .can_edit = can_edit, .edit = edit,
};
