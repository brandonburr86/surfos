/*
SurfOS Desktop: Terminal
--------------------
File: guiterm.c Date: 10/17/26
--------------------
The SurfOS shell (surfsh) in a window: a session on a pseudo-terminal, through struct gui_sys's term_* calls.
Track TERM of docs/superpowers/plans/2026-10-17-gui-suite.md; the window manager reaches it
through gui_app_terminal (struct gui_app, shell/guipriv.h).
10/17/26: the Terminal. The window opens a session when it opens (term_open: /initrd/bin/surfsh -l
on a free pseudo-terminal, shell/guisys_term.c) at the size its content has in 8x16 cells, and
every frame (the tick) reads what the session printed, never waiting, into the screen of
shell/guivt.c, which draws it as the console does; a new size of the window is the terminal's
(term_resize, once per change, when the corner is let go and everything printed at the old size
has been read). Every key but Ctrl+G is the session's, as a terminal sends it (raw mode's
sequences, tty.h): the arrows, Home, End, Page Up and Down, Delete and Insert as escape sequences,
Enter as CR, the rest as the bytes they are (Ctrl+C is 3: the pty interrupts the program). A write
never waits: what the session does not take now (a paste, keys while a program does not read)
waits in pend and goes on the next frames; a Ctrl+C drops what waited and goes first, as telnetd
does it. The scrollback (about 1,000 lines) is shown with the scroll bar and the wheel; output
leaves a view that was scrolled back where it is (the status bar says how much came below), and a
key brings it back to the screen. The mouse selects (a drag, past the top or bottom edge it
scrolls; a double-click takes a word), and the Edit menu copies the selection (or, with none, the
screen as shown), pastes and selects everything, through the desktop's clipboard. When the session
ends (exit), a line in the window and the status bar say so, and Enter or the New session button
starts another below; while one runs, busy() makes closing the window and leaving the desktop ask
first, and closing it hangs the session up. Nothing here keeps or reports what is typed or shown
anywhere but in the window and the clipboard.
10/18/26 (the second review): a new size waits for what was printed at the old one SIZE_WAIT_MS at
most: while output pours in for longer, what was read is drawn, and the size goes without waiting for
the pty to be empty. A copy of the screen takes it as it shows, also lines kept from a wider window
(vt_shown()); a paste goes into the queue in chunks that end between characters, and once a byte did
not fit, nothing after it does, so what the session gets is the clipboard's start.
*/
#include "guipriv.h"
#include "guivt.h"
#include <string.h>
#include <stdio.h>

/* the console's sixteen colours (VGA order) in the desktop's palette: black is the Terminal's own background */
static const u32 palette[16] = {
    NAVY, 0x3C6EB4, 0x4C9F5C, 0x2B9E9C, 0xC2534B, 0x9B5FA7, 0xC08A3E, SHELL,
    MUTED, SKY, 0x7ED48C, FOAM, CORAL, 0xD891D8, 0xF0D27A, 0xFFFDF5,
};
#define READ_MAX 8192                   /* bytes drawn in one frame at most (a pty's whole buffer), and */
#define WORK_MAX 32768                  /* 10/18/26: cells written: a flood of erases never holds the desktop up */
#define SIZE_WAIT_MS 250                /* 10/18/26: a new size waits this long at most for the output before it */
#ifndef EMFILE
#define EMFILE 24                       /* fs/vfs.h's: the desktop's task has no free descriptor */
#endif
#define EDGE_MS 50                      /* a drag past the edge scrolls a line this often */
#define BUTTON_W 104                    /* New session, in the status bar */

static const char *const help[] = {
    "#Terminal",
    "surfsh, the SurfOS shell, on a pseudo-terminal: every",
    "command works as on F1-F4. Every key is the shell's,",
    "Ctrl+C, Ctrl+Q, Ctrl+W, Tab and Esc too, but Ctrl+G",
    "and Ctrl+Space: the menus, whose Edit menu copies (the",
    "selection, or the screen), pastes and selects it all.",
    "Drag to select; double-click selects a word. The wheel",
    "and the scroll bar go back about 1,000 lines. Output",
    "leaves a view that was scrolled back where it is; a",
    "key returns to the end. The window's size is the",
    "terminal's. When the shell exits, its session ends",
    "with what it left running; Enter or New session starts",
    "another, and Esc or Ctrl+W closes the window. Closing",
    "it while a session runs ends the session, asking first.",
};

/**** layout ****/

/* window-relative: the text's top left, its cells, the scroll bar, the status bar */
struct term_layout { int tx, ty, cols, rows, bar_x, status_y; };

static void layout(const struct gui_state *g, struct term_layout *l) {
    const struct gui_window *w = &g->win[GUI_TERMINAL];
    l->tx = 8;
    l->ty = TITLE_H + 2;
    l->status_y = w->h - STATUS_H - 1;
    l->bar_x = w->w - 2 - SCROLL_W;
    l->cols = gui_clamp((l->bar_x - l->tx - 2) / 8, 8, GUI_VT_COLS);     /* PTY_SETSIZE's least is 8 x 2 */
    l->rows = gui_clamp((l->status_y - l->ty - 2) / 16, 2, GUI_VT_ROWS);
}

static void say(struct gui_state *g, const char *msg) {
    strlcpy(g->term.status, msg, sizeof(g->term.status));
    g->dirty = true;
}

/* New session's place in the status bar (window-relative), left of the size and clear of the resize grip */
static int button_x(const struct gui_state *g) {
    char size[24];
    snprintf(size, sizeof(size), "%dx%d", g->term.vt.cols, g->term.vt.rows);
    return g->win[GUI_TERMINAL].w - 24 - (int)strlen(size) * 8 - 8 - BUTTON_W;
}

static bool button_shown(const struct gui_state *g) {
    return g->sys && g->term.handle < 0;
}

/**** the view ****/

/* the first line the window shows, of those kept */
static int view_top(const struct gui_term *t) {
    int bottom = vt_screen(&t->vt);
    u32 top;
    if(t->follow) return bottom;
    if(t->view < t->vt.dropped) return 0;               /* its lines went from the scrollback meanwhile */
    top = t->view - t->vt.dropped;
    return top > (u32)bottom ? bottom : (int)top;
}

/* show from line top (of those kept): at the screen, the view follows it again */
static void scroll_to(struct gui_state *g, int top) {
    struct gui_term *t = &g->term;
    int bottom = vt_screen(&t->vt);
    top = gui_clamp(top, 0, bottom);
    t->follow = top >= bottom;
    t->view = t->vt.dropped + (u32)top;
    if(t->follow) t->news = 0;
    g->dirty = true;
}

/* the selection in reading order, lines by number; false when there is none */
static bool sel_range(const struct gui_term *t, u32 *l0, int *c0, u32 *l1, int *c1) {
    bool ahead = t->sel_line < t->end_line || (t->sel_line == t->end_line && t->sel_col <= t->end_col);
    if(!t->selected) return false;
    *l0 = ahead ? t->sel_line : t->end_line;
    *c0 = ahead ? t->sel_col : t->end_col;
    *l1 = ahead ? t->end_line : t->sel_line;
    *c1 = ahead ? t->end_col : t->sel_col;
    return *l0 != *l1 || *c0 != *c1;
}

static bool in_sel(u32 l0, int c0, u32 l1, int c1, u32 line, int col) {
    if(line < l0 || line > l1) return false;
    if(line == l0 && col < c0) return false;
    return line != l1 || col < c1;
}

/* the line (of those kept) under y and the column under x: a boundary between cells (round), or a cell */
static void point(const struct gui_state *g, const struct term_layout *l, int x, int y, int *line, int *col, bool round) {
    const struct gui_vt *vt = &g->term.vt;
    int shown = vt->rows < l->rows ? vt->rows : l->rows, cols = vt->cols < l->cols ? vt->cols : l->cols;
    int row = y >= l->ty ? (y - l->ty) / 16 : 0;
    *line = view_top(&g->term) + gui_clamp(row, 0, shown - 1);
    if(*line >= vt->count) *line = vt->count - 1;
    *col = round ? gui_clamp((x - l->tx + 4) / 8, 0, cols) : gui_clamp((x - l->tx) / 8, 0, cols - 1);
    if(x < l->tx) *col = 0;
}

/**** the session ****/

static const char *why_none(int err) {
    switch(err) {
    case -EAGAIN: return "every pseudo-terminal is in use";
    case -ENOENT: return "/initrd/bin/surfsh is not there";
    case -ENOMEM: return "there is not enough memory";
    case -ENOSYS: return "this system has no sessions";
    case -EMFILE: return "too many files are open";
    }
    return "it could not be started";
}

/* a line of the Terminal's own in the window, in light cyan, at the start of a row */
static void note(struct gui_state *g, const char *text) {
    char line[160];
    vt_fresh(&g->term.vt);
    snprintf(line, sizeof(line), "\033[1;36m%s\033[0m\n", text);
    vt_feed(&g->term.vt, line, (u32)strlen(line));
    g->dirty = true;
}

/* a session, at the window's size (the window opens, Enter after the end, New session) */
static void start(struct gui_state *g) {
    struct gui_term *t = &g->term;
    struct term_layout l;
    char line[GUI_STATUS];
    int h;
    if(!g->sys || t->handle >= 0) return;
    layout(g, &l);
    if(l.cols != t->vt.cols || l.rows != t->vt.rows) vt_resize(&t->vt, l.cols, l.rows);
    vt_fresh(&t->vt);
    t->pend_at = t->pend_len = t->carry_at = t->carry_len = 0;
    h = g->sys->term_open(g->sys->ctx, t->vt.cols, t->vt.rows);
    if(h < 0) {
        t->error = h;
        snprintf(line, sizeof(line), "[No session: %s. Enter tries again.]", why_none(h));
        note(g, line);
        snprintf(t->status, sizeof(t->status), "No session: %s.", why_none(h));
        return;
    }
    t->handle = h;
    t->ended = false;
    t->error = 0;
    t->status[0] = 0;
    scroll_to(g, vt_screen(&t->vt));
}

/* the session has ended and everything it printed was read (or it is lost): its pty goes */
static void ended(struct gui_state *g, int err) {
    struct gui_term *t = &g->term;
    g->sys->term_close(g->sys->ctx, t->handle);
    t->handle = -1;
    t->ended = true;
    t->pend_at = t->pend_len = t->carry_at = t->carry_len = 0;
    note(g, err == -EPIPE ? "[The session has ended. Enter starts a new one.]" : "[The session was lost. Enter starts a new one.]");
    say(g, "The session has ended. Enter starts a new one.");
}

/* bytes for the session behind those that wait; what does not fit is dropped: how many were. A key goes whole
   or not at all (10/18/26: never half an escape sequence); a paste as far as there is room, but not half a
   UTF-8 character. */
static u32 queue(struct gui_term *t, const char *s, u32 n, bool whole) {
    u32 room;
    if(t->pend_at && t->pend_len + n > GUI_TERM_PEND) {    /* what was taken makes room */
        memmove(t->pend, t->pend + t->pend_at, t->pend_len - t->pend_at);
        t->pend_len -= t->pend_at;
        t->pend_at = 0;
    }
    room = GUI_TERM_PEND - t->pend_len;
    if(n > room) {
        if(whole) return n;
        while(room && ((u8)s[room] & 0xC0) == 0x80) room--;
        memcpy(t->pend + t->pend_len, s, room);
        t->pend_len += room;
        return n - room;
    }
    memcpy(t->pend + t->pend_len, s, n);
    t->pend_len += n;
    return 0;
}

/* what waits, to the session: as much as its input queue takes now */
static void flush(struct gui_state *g) {
    struct gui_term *t = &g->term;
    int r, tries;
    for(tries = 0; tries < 4 && t->pend_at < t->pend_len; tries++) {
        r = g->sys->term_write(g->sys->ctx, t->handle, t->pend + t->pend_at, t->pend_len - t->pend_at);
        if(r <= 0) break;                               /* nothing fits now, or the session is gone (its read says so) */
        t->pend_at += (u32)r;
        g->dirty = true;
    }
    if(t->pend_at >= t->pend_len) t->pend_at = t->pend_len = 0;
}

/* what a terminal sends for a key: raw mode's sequences for the keys that are not bytes (tty.h) */
static u32 key_bytes(int key, char *out) {
    static const char *const seqs[] = {                 /* GUI_UP .. GUI_INSERT */
        "\033[A", "\033[B", "\033[D", "\033[C", "\033[H", "\033[F", "\033[5~", "\033[6~", "\033[3~", "\033[2~",
    };
    if(key >= GUI_UP && key <= GUI_INSERT) {
        strlcpy(out, seqs[key - GUI_UP], 8);
        return (u32)strlen(out);
    }
    if(key < 0 || key > 255) return 0;
    out[0] = (char)key;                                 /* Enter is CR, Backspace DEL, Tab, Esc, Ctrl+letters 1-26 */
    return 1;
}

/**** the Edit menu ****/

static void copy(struct gui_state *g) {
    struct gui_term *t = &g->term;
    const struct gui_vt *vt = &t->vt;
    char size[24];
    u32 l0, l1, n;
    int c0, c1, a, b;
    bool cut, sel = sel_range(t, &l0, &c0, &l1, &c1);
    if(sel) {
        if(l1 < vt->dropped) { say(g, "The selection has left the scrollback: nothing copied."); return; }
        a = l0 < vt->dropped ? -1 : (int)(l0 - vt->dropped);   /* -1: from the oldest line kept */
        b = (int)(l1 - vt->dropped);
    } else {                                            /* nothing selected: the screen as shown (10/18/26: */
        a = view_top(t);                                /* lines kept from a wider window too, cut at its width) */
        b = a + vt->rows - 1;
    }
    n = sel ? vt_text(vt, a, c0, b, c1, g->clip, GUI_CLIP_MAX, &cut) : vt_shown(vt, a, b, g->clip, GUI_CLIP_MAX, &cut);
    if(!n) {                                            /* 10/18/26: only blanks: the clipboard keeps what it had */
        say(g, sel ? "Nothing copied: the selection is blank." : "Nothing copied: the screen is blank.");
        return;
    }
    gui_clipped(g, n);
    gui_bytes(n, size, sizeof(size));
    snprintf(t->status, sizeof(t->status), "%s %s%s", sel ? "Copied" : "Copied the screen,", size,
             cut ? " (as much as the clipboard holds)" : "");
    g->dirty = true;
}

/* the clipboard, typed: a line end goes as Enter (CR), as a terminal sends it. 10/18/26 (the second review): in
   chunks that end between characters (a UTF-8 character is 4 bytes at most), and once a byte did not fit behind
   what waits, nothing after it goes either: the session gets the clipboard's start, never one with a hole */
static void paste(struct gui_state *g) {
    struct gui_term *t = &g->term;
    char chunk[256], msg[GUI_STATUS];
    u32 i, n = 0, lost = 0;
    if(!g->sys || t->handle < 0 || !g->clip_len) return;
    for(i = 0; i < g->clip_len; i++) {
        char c = g->clip[i];
        if(c == '\n' && i && g->clip[i - 1] == '\r') continue;     /* CR LF is one Enter */
        if(n == sizeof(chunk) || (n > sizeof(chunk) - 4 && ((u8)c & 0xC0) != 0x80)) {    /* c starts a character */
            lost += lost ? n : queue(t, chunk, n, false);
            n = 0;
        }
        chunk[n++] = c == '\n' ? '\r' : c;
    }
    lost += lost ? n : queue(t, chunk, n, false);
    flush(g);
    if(!t->follow) scroll_to(g, vt_screen(&t->vt));
    if(lost) {
        snprintf(msg, sizeof(msg), "Pasted all but %u bytes: they did not fit behind what waits.", lost);
        say(g, msg);
    } else t->status[0] = 0;
    g->dirty = true;
}

static void select_all(struct gui_state *g) {
    struct gui_term *t = &g->term;
    t->selected = true;
    t->selecting = false;
    t->sel_line = t->vt.dropped;
    t->sel_col = 0;
    t->end_line = t->vt.dropped + (u32)t->vt.count - 1;
    t->end_col = t->vt.cols;
    say(g, "All the lines are selected: Edit, Copy takes them.");
}

static bool can_edit(const struct gui_state *g, int act) {
    switch(act) {
    case ACT_COPY: case ACT_SELECT_ALL: return true;
    case ACT_PASTE: return g->sys && g->term.handle >= 0 && g->clip_len > 0;
    }
    return false;
}

static void edit(struct gui_state *g, int act) {
    if(act == ACT_COPY) copy(g);
    else if(act == ACT_PASTE) paste(g);
    else if(act == ACT_SELECT_ALL) select_all(g);
}

/**** the window manager's hooks ****/

static void reset(struct gui_state *g) {
    struct gui_term *t = &g->term;
    struct term_layout l;
    memset(t, 0, sizeof(*t));
    t->handle = -1;
    t->follow = true;
    t->click_line = (u32)-1;
    t->size_ms = g->now;
    layout(g, &l);
    vt_reset(&t->vt, l.cols, l.rows);
}

static void opened(struct gui_state *g) {
    reset(g);
    start(g);
}

/* the window closes, or the desktop does: the session hangs up (its programs are killed) */
static void closed(struct gui_state *g) {
    struct gui_term *t = &g->term;
    if(g->sys && t->handle >= 0) g->sys->term_close(g->sys->ctx, t->handle);
    t->handle = -1;
    t->ended = false;
    t->selecting = t->thumb = false;
    t->pend_at = t->pend_len = t->carry_at = t->carry_len = 0;
}

static bool key(struct gui_state *g, int key) {
    struct gui_term *t = &g->term;
    char b[9];
    u32 n;
    bool alt = (key & GUI_KEY_ALT) != 0;                /* 11/05/26: an Alt key goes as Esc and the key, as a */
    key &= GUI_KEY_MASK;                                /* terminal sends it (surfsh's Alt+. and the rest) */
    if(!g->sys) return true;
    if(t->handle < 0) {                                 /* no session: Enter starts one, and the window's keys are back */
        if(key == '\r' || key == '\n') start(g);
        else if(key == 27 || key == 23) gui_close(g, GUI_TERMINAL);    /* Esc, Ctrl+W */
        else if(key == 17) gui_request_quit(g);         /* Ctrl+Q */
        return true;
    }
    n = key_bytes(key, b + alt);
    if(!n) return true;
    if(alt) {
        b[0] = 27;
        n++;
    }
    if(key == 3) t->pend_at = t->pend_len = 0;          /* Ctrl+C: what waited is dropped, and it goes first */
    t->status[0] = 0;
    if(queue(t, b, n, true)) say(g, "A key was dropped: the session takes nothing now.");
    flush(g);
    if(!t->follow) scroll_to(g, vt_screen(&t->vt));     /* a key brings the view back to the screen */
    g->dirty = true;
    return true;
}

/* a drag past the top or bottom edge scrolls, a line every EDGE_MS (the tick goes on while the pointer rests there) */
static void edge_scroll(struct gui_state *g, const struct term_layout *l) {
    struct gui_term *t = &g->term;
    int shown = t->vt.rows < l->rows ? t->vt.rows : l->rows, line, col;
    if(t->drag_y >= l->ty && t->drag_y < l->ty + shown * 16) return;
    if(g->now - t->drag_ms < EDGE_MS) return;
    t->drag_ms = g->now;
    scroll_to(g, view_top(t) + (t->drag_y < l->ty ? -1 : 1));
    point(g, l, t->drag_x, t->drag_y, &line, &col, true);
    t->end_line = t->vt.dropped + (u32)line;
    t->end_col = col;
}

static void press(struct gui_state *g, int x, int y) {
    struct gui_term *t = &g->term;
    struct term_layout l;
    int line, col, top, size, l0, c0, l1, c1, first;
    layout(g, &l);
    t->selecting = t->thumb = false;
    g->dirty = true;
    if(y >= l.status_y) {
        if(button_shown(g) && gui_inside(x, y, button_x(g), l.status_y + 2, BUTTON_W, 18)) start(g);
        return;
    }
    if(x >= l.bar_x) {                                  /* the scroll bar: page, or hold the thumb */
        first = view_top(t);
        gd_thumb(l.status_y - TITLE_H, first, t->vt.rows, t->vt.count, &top, &size);
        if(y - TITLE_H < top) scroll_to(g, first - t->vt.rows);
        else if(y - TITLE_H >= top + size) scroll_to(g, first + t->vt.rows);
        else { t->thumb = true; t->thumb_at = y - TITLE_H - top; }
        return;
    }
    point(g, &l, x, y, &line, &col, false);
    if(t->vt.dropped + (u32)line == t->click_line && col == t->click_col && g->now - t->click_ms <= DOUBLE_MS) {
        vt_word(&t->vt, line, col, &l0, &c0, &l1, &c1); /* a double-click: the word, across a wrap too */
        t->selected = l0 != l1 || c0 != c1;
        t->sel_line = t->vt.dropped + (u32)l0;
        t->sel_col = c0;
        t->end_line = t->vt.dropped + (u32)l1;
        t->end_col = c1;
        t->click_ms = g->now - DOUBLE_MS - 1;
        return;
    }
    t->click_line = t->vt.dropped + (u32)line;
    t->click_col = col;
    t->click_ms = g->now;
    point(g, &l, x, y, &line, &col, true);
    t->selected = t->selecting = true;
    t->sel_line = t->end_line = t->vt.dropped + (u32)line;
    t->sel_col = t->end_col = col;
    t->drag_x = x;
    t->drag_y = y;
    t->drag_ms = g->now - EDGE_MS;                      /* the first move past the edge scrolls at once */
}

static void drag(struct gui_state *g, int x, int y) {
    struct gui_term *t = &g->term;
    struct term_layout l;
    int line, col;
    layout(g, &l);
    if(t->thumb) {
        scroll_to(g, gd_scroll_to(l.status_y - TITLE_H, y - TITLE_H - t->thumb_at, t->vt.rows, t->vt.count));
        return;
    }
    if(!t->selecting) return;
    t->drag_x = x;
    t->drag_y = y;
    edge_scroll(g, &l);
    point(g, &l, x, y, &line, &col, true);
    t->end_line = t->vt.dropped + (u32)line;
    t->end_col = col;
    g->dirty = true;
}

static void release(struct gui_state *g) {
    struct gui_term *t = &g->term;
    if(t->selecting && t->sel_line == t->end_line && t->sel_col == t->end_col) t->selected = false;   /* a click */
    t->selecting = t->thumb = false;
    g->dirty = true;
}

static void wheel(struct gui_state *g, int rows) {
    scroll_to(g, view_top(&g->term) + rows);
}

/* Every frame: what waits goes, what the session printed is read and drawn, a new size is passed on. 10/18/26:
   what is read waits in carry and is drawn as far as a frame's budget goes, READ_MAX bytes and WORK_MAX cells
   written (and the last byte's own: a screenful at most), so that a flood of erases costs a frame no more than
   a screenful of text does; the rest waits for the next frame. A new size waits until what the session printed
   before it is drawn, at the old size; 10/18/26 (the second review): SIZE_WAIT_MS at most, as output that never
   pauses (a program in the background, say) would hold it up for good. Then the frame reads nothing more, draws
   what is left of what was read as far as its budget goes (all of it, but for a flood of erases), and the size
   goes at its end, though more waits in the pty: that is drawn at the new size. */
static void tick(struct gui_state *g) {
    struct gui_term *t = &g->term;
    struct term_layout l;
    u32 before, start, got = 0, k;
    int r = 0;
    bool spent = false, sized, due;
    if(!g->sys) return;
    layout(g, &l);
    if(g->app_drag != GUI_TERMINAL) t->selecting = t->thumb = false;   /* the drag ended elsewhere (a console switch) */
    if(t->selecting) edge_scroll(g, &l);
    sized = (l.cols != t->vt.cols || l.rows != t->vt.rows) && g->resize != GUI_TERMINAL;   /* a new size to pass on */
    if(!sized) t->size_ms = g->now;                     /* so a new one waits from the frame it came in */
    due = sized && g->now - t->size_ms >= SIZE_WAIT_MS;
    if(t->handle >= 0) {
        flush(g);
        before = t->vt.dropped + (u32)t->vt.count;
        start = t->vt.written;
        while(!(spent = got >= READ_MAX || t->vt.written - start >= WORK_MAX)) {
            if(t->carry_at == t->carry_len) {           /* all of it drawn: read on, unless the size is due */
                if(due) break;
                r = g->sys->term_read(g->sys->ctx, t->handle, t->carry, sizeof(t->carry));
                if(r <= 0) break;
                t->carry_at = 0;
                t->carry_len = (u32)r;
            }
            k = vt_take(&t->vt, t->carry + t->carry_at, t->carry_len - t->carry_at, WORK_MAX - (t->vt.written - start));
            t->carry_at += k;
            got += k;
        }
        if(got) {
            if(!t->follow) t->news += t->vt.dropped + (u32)t->vt.count - before;
            g->dirty = true;
        }
        if(r < 0) { ended(g, r); return; }
        if(!due && (spent || t->carry_at < t->carry_len)) return;  /* more next frame: the size waits until all is */
    }                                                   /* drawn, unless it is due (what is left of carry follows) */
    if(sized) {
        vt_resize(&t->vt, l.cols, l.rows);
        if(t->handle >= 0) g->sys->term_resize(g->sys->ctx, t->handle, t->vt.cols, t->vt.rows);
        g->dirty = true;
    }
}

static bool busy(const struct gui_state *g, char *why, size_t n) {
    if(!g->sys || g->term.handle < 0) return false;
    strlcpy(why, "A session of surfsh runs in the Terminal.", n);
    return true;
}

/**** drawing ****/

static void status_text(const struct gui_state *g, char *out, size_t n) {
    const struct gui_term *t = &g->term;
    char size[24];
    int back = vt_screen(&t->vt) - view_top(t);
    if(!g->sys) strlcpy(out, "No system: no session can run here.", n);
    else if(t->handle < 0) strlcpy(out, t->status, n);
    else if(t->pend_len - t->pend_at > 64) {
        gui_bytes(t->pend_len - t->pend_at, size, sizeof(size));
        snprintf(out, n, "Pasting: %s to go", size);
    } else if(back > 0 && t->news) snprintf(out, n, "Scrolled back %d lines, %u new below. A key returns.", back, t->news);
    else if(back > 0) snprintf(out, n, "Scrolled back %d line%s. A key returns.", back, back == 1 ? "" : "s");
    else if(t->status[0]) strlcpy(out, t->status, n);
    else strlcpy(out, "Ctrl+G: the menus. Edit copies and pastes.", n);
}

static void render(const struct gui_state *g, const struct gui_draw *d) {
    const struct gui_window *w = &g->win[GUI_TERMINAL];
    const struct gui_term *t = &g->term;
    const struct gui_vt *vt = &t->vt;
    struct term_layout l;
    struct gui_draw text;
    char b[GUI_STATUS + 64], size[24];
    int x0 = w->x, y0 = w->y, top, row, col, k, rows, cols, x, cy;
    u32 l0 = 0, l1 = 0, bg;
    int c0 = 0, c1 = 0;
    bool sel, focused = g->focus == GUI_TERMINAL && !g->menu && !g->dialog.kind;
    layout(g, &l);
    gd_rect(d, x0 + 1, y0 + TITLE_H, l.bar_x - 1, l.status_y - TITLE_H, palette[0]);
    rows = vt->rows < l.rows ? vt->rows : l.rows;
    cols = vt->cols < l.cols ? vt->cols : l.cols;
    top = view_top(t);
    sel = sel_range(t, &l0, &c0, &l1, &c1);
    text = gd_clip(d, x0 + l.tx, y0 + l.ty, cols * 8, rows * 16);
    for(row = 0; row < rows && top + row < vt->count; row++) {
        const struct gui_vt_cell *c = vt_line(vt, top + row);
        u32 line = vt->dropped + (u32)(top + row);
        int py = y0 + l.ty + row * 16;
        for(col = 0; col < cols; col = k) {             /* the backgrounds, a run at a time */
            bg = sel && in_sel(l0, c0, l1, c1, line, col) ? PICK : palette[(c[col].attr >> 4) & 7];
            for(k = col + 1; k < cols && (sel && in_sel(l0, c0, l1, c1, line, k) ? PICK : palette[(c[k].attr >> 4) & 7]) == bg; k++) ;
            if(bg != palette[0]) gd_rect(&text, x0 + l.tx + col * 8, py, (k - col) * 8, 16, bg);
        }
        for(col = 0; col < cols; col++) {
            if(c[col].ch == ' ' || !c[col].ch) continue;
            gd_glyph(&text, x0 + l.tx + col * 8, py, c[col].ch,
                     sel && in_sel(l0, c0, l1, c1, line, col) ? NAVY : palette[c[col].attr & 15], 1);
        }
    }
    cy = vt_screen(vt) + vt->y - top;                   /* the cursor's row in the view */
    if(t->handle >= 0 && !vt->cursor_off && cy >= 0 && cy < rows && vt->x < cols) {
        const struct gui_vt_cell *c = &vt_line(vt, vt_screen(vt) + vt->y)[vt->x];
        int px = x0 + l.tx + vt->x * 8, py = y0 + l.ty + cy * 16;
        if(focused) {                                   /* a block: the cell in inverse */
            gd_rect(&text, px, py, 8, 16, palette[c->attr & 15]);
            if(c->ch != ' ' && c->ch) gd_glyph(&text, px, py, c->ch, palette[(c->attr >> 4) & 7], 1);
        } else gd_frame(&text, px, py, 8, 16, palette[c->attr & 15]);
    }
    if(!g->sys) {
        gd_text(d, x0 + 16, y0 + TITLE_H + 16, "The Terminal runs a shell session, which", FOAM);
        gd_text(d, x0 + 16, y0 + TITLE_H + 34, "needs the system: this desktop has none.", FOAM);
    }
    gd_scrollbar(d, x0 + l.bar_x, y0 + TITLE_H, l.status_y - TITLE_H, top, vt->rows, vt->count);
    gd_rect(d, x0 + 1, y0 + l.status_y, w->w - 2, STATUS_H, BAND);
    gd_rect(d, x0 + 1, y0 + l.status_y, w->w - 2, 1, LINE);
    snprintf(size, sizeof(size), "%dx%d", vt->cols, vt->rows);
    x = w->w - 24 - (int)strlen(size) * 8;              /* clear of the resize grip */
    gd_text(d, x0 + x, y0 + l.status_y + 3, size, MUTED);
    if(button_shown(g)) {
        x = button_x(g);
        gd_button(d, x0 + x, y0 + l.status_y + 2, BUTTON_W, 18, "New session", TEAL, PAPER);
    }
    status_text(g, b, sizeof(b));
    gd_name(d, x0 + 10, y0 + l.status_y + 3, b, (x - 18) / 8, INK);
}

const struct gui_app gui_app_terminal = {
    .title = "Terminal", .icon = ICON_TERMINAL, .shortcut = "Ctrl+T", .w = 640, .h = 400,
    .resizable = true, .arrows = false, .keys = true,
    .opened = opened, .closed = closed, .render = render, .key = key,
    .press = press, .drag = drag, .release = release, .wheel = wheel, .tick = tick, .busy = busy,
    .help = help, .help_lines = (int)(sizeof(help) / sizeof(help[0])),
    .can_edit = can_edit, .edit = edit,
};
