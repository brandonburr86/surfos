/*
SurfOS user program: wcode
--------------------
File: window.c  Date: 11/05/26
--------------------
The Code Editor's window (window.h). From the top: a strip of tabs, one for each file open (its x
closes it); the text, with the line numbers in a gutter at its left and scroll bars when it does
not fit; the find bar and the replacement's line under it when they are open; find in files, with
its list; the status bar, a message at the left and where the caret is at the right. The menus
are in the desktop's bar while the window has the focus, and their shortcuts work in any case.

The text is drawn by the program in cells of the console's font (an area of the toolkit's): each
event that changes anything renders the rows shown (view.c), compares them with the rows as last
drawn, and damages only those that differ, so a key typed presents a row and not the window. The
draw function paints the rows the toolkit asks for, from what was rendered.

A file's characters are the console's: UTF-8 as code page 437 ('?' for one it lacks), a byte that
is no UTF-8 as the 437 character it is and a control character as the console's symbol for it (a
lone CR as ♪), as Notepad shows them; a tab is blank to the next multiple of 8.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <surfos/cp437.h>
#include "window.h"

#define ELLIPSIS "\xe2\x88\x99"         /* the desktop's menus end an item that asks more with ∙ */

/**** colours ****/

static unsigned class_color(const struct gk_theme *t, int cls) {
    switch(cls) {
    case WC_C_COMMENT: return t->muted;
    case WC_C_STRING: case WC_C_CODE: return 0x167366;          /* a dark sea green */
    case WC_C_NUMBER: return 0xA44A22;                          /* rust */
    case WC_C_KEYWORD: case WC_C_EMPH: return 0x6B3A8E;         /* plum */
    case WC_C_TYPE: case WC_C_LINK: case WC_C_KEY: return 0x1E5C96;     /* a deep blue */
    case WC_C_PREPROC: case WC_C_LABEL: return 0x86581A;        /* a dark sand */
    case WC_C_VAR: return 0x96365E;                             /* a dark rose */
    case WC_C_HEADING: return GW_NAVY;
    case WC_C_BAD: return 0xC0392B;
    }
    return t->ink;
}

static unsigned bg_color(const struct gk_theme *t, int bg) {
    switch(bg) {
    case WC_BG_SEL: return t->pick;
    case WC_BG_BRACKET: return t->foam;
    case WC_BG_NOMATCH: return 0xF5C6B5;
    }
    return t->paper;
}

/* the console's character for a code point (cp437.h) */
static unsigned glyph_of(unsigned cp) {
    int c;
    if(cp & WC_BAD) return cp & 0xFF;
    if(cp < 0x20 || cp == 0x7F) return cp;
    c = cp437_from_unicode(cp);
    return c > 0 ? (unsigned)c : '?';
}

static unsigned gutter_cells(const struct wc_doc *d) {
    unsigned n = wc_view_digits(d);
    return (n < 3 ? 3 : n) + 2;
}

/**** drawing ****/

static void render(struct wc_window *w) {
    struct wc_doc *d = wc_app_doc(&w->app);
    unsigned row, col;
    w->top = d->top;
    w->left = d->left;
    if(!w->rows || !w->cols) {
        w->lines = 0;
        w->crow = -1;
        return;
    }
    wc_view_render(d, &w->app.view, w->cells, w->rows, w->cols, &w->lines);
    w->cline = wc_doc_line(d);
    if(wc_view_caret(d, w->rows, w->cols, &row, &col)) {
        w->crow = (int)row;
        w->ccol = (int)col;
    } else w->crow = -1;
}

static void draw_row(struct wc_window *w, struct gw *win, const struct gk_theme *t, int r) {
    const struct wc_cell *c = w->cells + (size_t)r * w->cols;
    int y = r * WC_CH, gx = (int)(w->gutter * WC_CW), k, from, n;
    unsigned line = w->top + (unsigned)r;
    char num[16];
    gw_rect(win, 0, y, gx, WC_CH, t->band);
    if((unsigned)r < w->lines) {
        n = snprintf(num, sizeof(num), "%u", line + 1);
        gw_text(win, gx - WC_CW - n * WC_CW, y, num, line == w->cline ? t->ink : t->muted);
    }
    for(k = 0; k < (int)w->cols; k = from) {            /* the backgrounds, a run at a time */
        for(from = k + 1; from < (int)w->cols && c[from].bg == c[k].bg; from++);
        if(c[k].bg != WC_BG_NONE) gw_rect(win, gx + k * WC_CW, y, (from - k) * WC_CW, WC_CH, bg_color(t, c[k].bg));
    }
    for(k = 0; k < (int)w->cols; k++)
        if(c[k].cp && c[k].cp != ' ') gw_glyph(win, gx + k * WC_CW, y, glyph_of(c[k].cp), class_color(t, c[k].cls), 1);
    if(r == w->crow) gw_rect(win, gx + w->ccol * WC_CW, y, 2, WC_CH, w->focused ? t->ink : t->muted);
}

static void draw(struct gk_widget *area, struct gw *win, const struct gk_view *v, void *data) {
    struct wc_window *w = data;
    const struct gk_theme *t = gk_theme(w->ui);
    struct wc_doc *d = wc_app_doc(&w->app);
    int r, r1;
    (void)area;
    if((unsigned)v->sy != w->top || (unsigned)v->sx != w->left) {  /* moved by its bars before the program heard */
        d->top = (unsigned)v->sy;
        d->left = (unsigned)v->sx;
        render(w);
    }
    gw_rect(win, v->cx, v->cy, v->cw, v->ch, t->paper);
    gw_rect(win, 0, v->cy, (int)(w->gutter * WC_CW), v->ch, t->band);    /* the gutter to the foot, past the rows */
    r1 = (v->cy + v->ch - 1) / WC_CH;
    for(r = v->cy / WC_CH; r <= r1 && r < (int)w->rows; r++) draw_row(w, win, t, r);
}

/* the text's cells for its view now: 1 when they are not those drawn (a new size, a new gutter) */
static int geometry(struct wc_window *w) {
    struct gk_view v;
    unsigned gutter = gutter_cells(wc_app_doc(&w->app)), rows, cols, gpx;
    struct wc_cell *a, *b;
    gk_area_view(w->text, &v);
    gpx = gutter * WC_CW;
    rows = v.h > 0 ? (unsigned)v.h / WC_CH : 0;
    cols = v.w > (int)gpx ? ((unsigned)v.w - gpx) / WC_CW : 0;
    if(cols > WC_ROW_CELLS) cols = WC_ROW_CELLS;
    if(w->cells && rows == w->rows && cols == w->cols && gutter == w->gutter) return 0;
    a = malloc((size_t)(rows ? rows : 1) * (cols ? cols : 1) * sizeof(struct wc_cell));
    b = malloc((size_t)(rows ? rows : 1) * (cols ? cols : 1) * sizeof(struct wc_cell));
    if(!a || !b) {                              /* the old ones stay, at their size */
        free(a);
        free(b);
        return 0;
    }
    free(w->cells);
    free(w->drawn);
    w->cells = a;
    w->drawn = b;
    w->rows = rows;
    w->cols = cols;
    w->gutter = gutter;
    wc_app_size(&w->app, rows ? rows : 1, cols ? cols : 1);
    return 1;
}

/* the extent of the scroll bars, and the view where the model has it (within what the toolkit allows) */
static void place(struct wc_window *w) {
    struct wc_doc *d = wc_app_doc(&w->app);
    struct gk_view v;
    unsigned width = wc_view_width(d, w->rows ? w->rows : 1) + 1, col = wc_doc_col(d) + 1;
    if(width < col) width = col;                /* the widest line shown, a cell for the caret after it */
    if(d->left && width < d->left + w->cols) width = d->left + w->cols;     /* and room for where the view is */
    gk_area_extent(w->text, (int)(w->gutter + width), WC_CW, (int)d->t.nl + 1, WC_CH);
    gk_scroll_to(w->text, (int)d->left, (int)d->top);
    gk_area_view(w->text, &v);
    d->left = (unsigned)v.sx;
    d->top = (unsigned)v.sy;
}

/**** the bars, the tabs, the menus ****/

static void status(struct wc_window *w) {
    struct wc_doc *d = wc_app_doc(&w->app);
    char left[WC_MSG + 64];
    if(w->app.status[0]) snprintf(left, sizeof(left), "%s", w->app.status);
    else if(d->msg[0]) snprintf(left, sizeof(left), "%s", d->msg);
    else if(d->unsure)
        snprintf(left, sizeof(left), "%.160s may share this file's data: check the disk (fsck) first",
                 d->twin[0] ? d->twin : d->copied[0] ? d->copied : "A file beside it");
    else left[0] = 0;
    gk_set_text(w->status, left);
    wc_view_status(d, w->right, sizeof(w->right));
    gk_status_right(w->status, w->right);
}

static void tabs(struct wc_window *w) {
    char label[WC_PATH + 4];
    int i;
    while(gk_tab_count(w->tabs) > w->app.n) gk_tab_remove(w->tabs, gk_tab_count(w->tabs) - 1);
    while(gk_tab_count(w->tabs) < w->app.n) {
        wc_app_tab_label(&w->app, gk_tab_count(w->tabs), label, sizeof(label));
        if(!gk_tab(w->tabs, label)) break;
    }
    for(i = 0; i < w->app.n && i < gk_tab_count(w->tabs); i++) {
        wc_app_tab_label(&w->app, i, label, sizeof(label));
        gk_tab_label(w->tabs, i, label);
    }
    gk_set_value(w->tabs, w->app.cur);
}

static void menus(struct wc_window *w) {
    struct wc_doc *d = wc_app_doc(&w->app);
    unsigned from, to;
    int sel = wc_doc_sel(d, &from, &to) > 0, l;
    gk_menu_enable(w->ui, WC_CMD_UNDO, wc_undo_can(&d->u, 0) > 0);
    gk_menu_enable(w->ui, WC_CMD_REDO, wc_undo_can(&d->u, 1) > 0);
    gk_menu_enable(w->ui, WC_CMD_CUT, sel);
    gk_menu_enable(w->ui, WC_CMD_COPY, sel);
    gk_menu_enable(w->ui, WC_CMD_COMMENT, wc_lang_comment(d->lang) != NULL);
    for(l = 0; l < WC_LANGS; l++) gk_menu_check(w->ui, WC_CMD_LANG + l, l == d->lang);
}

/* find in files' list: the lines found since the last look, and what it says of itself */
static void files_list(struct wc_window *w) {
    const struct wc_grep *g = &w->app.grep;
    char line[WC_PATH + WC_GREP_SHOW + 32], *p;
    const char *path, *root = g->root;
    size_t rn = strlen(root);
    for(; w->listed < g->nhits; w->listed++) {
        const struct wc_hit *h = &g->hits[w->listed];
        path = wc_grep_path(g, h);
        if(rn > 1 && !strncmp(path, root, rn) && path[rn] == '/') path += rn + 1;   /* from the folder searched */
        snprintf(line, sizeof(line), "%s:%u: %s", path, h->line, h->text);
        for(p = line; *p; p++)
            if(*p == '\t') *p = ' ';            /* a tab in a list starts a column */
        if(gk_list_add(w->flist, line) < 0) break;
    }
    if(g->state == WC_GREP_RUN)
        snprintf(line, sizeof(line), "Searching" ELLIPSIS " %u line%s found in %u file%s so far (Stop ends it)", g->nhits,
                 g->nhits == 1 ? "" : "s", g->files, g->files == 1 ? "" : "s");
    else if(g->state == WC_GREP_DONE) {
        char notes[160];
        wc_grep_notes(g, notes, sizeof(notes));     /* 11/09/26: what was left out, and why */
        snprintf(line, sizeof(line), "%u line%s found in %u file%s read%s%s%s%s", g->nhits, g->nhits == 1 ? "" : "s",
                 g->files, g->files == 1 ? "" : "s", g->why[0] ? ". " : "", g->why, notes[0] ? "; " : "", notes);
    } else line[0] = 0;
    gk_set_text(w->fsum, line);
    gk_set_text(w->fgo, g->state == WC_GREP_RUN ? "Stop" : "Search");
}

/* everything shown, after anything changed: the rows that differ damaged, the bars, the tabs, the title */
static void refresh(struct wc_window *w) {
    struct wc_doc *d = wc_app_doc(&w->app);
    struct gk_view v;
    char title[WC_PATH + 24];
    int full, r, crow = w->crow, ccol = w->ccol;
    unsigned cline = w->cline;
    full = geometry(w);
    place(w);
    full |= geometry(w);                        /* a scroll bar came or went */
    if(full) place(w);
    full |= d->top != w->top || d->left != w->left || w->app.cur != w->doc_drawn;
    render(w);
    gk_area_view(w->text, &v);
    if(full) gk_redraw(w->text);
    else
        for(r = 0; r < (int)w->rows; r++)
            if(memcmp(w->cells + (size_t)r * w->cols, w->drawn + (size_t)r * w->cols, w->cols * sizeof(struct wc_cell)) ||
               ((r == crow) != (r == w->crow)) || (r == w->crow && ccol != w->ccol) ||
               (w->top + (unsigned)r == cline) != (w->top + (unsigned)r == w->cline))
                gk_damage(w->text, 0, r * WC_CH, v.w, WC_CH);
    memcpy(w->drawn, w->cells, (size_t)w->rows * w->cols * sizeof(struct wc_cell));
    w->doc_drawn = w->app.cur;
    status(w);
    tabs(w);
    menus(w);
    if(gk_shown(w->files)) files_list(w);
    wc_app_title(&w->app, title, sizeof(title));
    if(strcmp(title, w->title)) {
        snprintf(w->title, sizeof(w->title), "%s", title);
        gw_title(gk_gw(w->ui), title);
    }
}

/**** what app.c asks for ****/

/* the folder of a path, for a chooser to start in; "" for none (the desktop's choice) */
static void folder_of(const char *path, char *out, unsigned size) {
    const char *slash = strrchr(path, '/');
    if(path[0] != '/' || !slash) {
        out[0] = 0;
        return;
    }
    snprintf(out, size, "%.*s", slash == path ? 1 : (int)(slash - path), path);
}

static void start_dir(struct wc_window *w, char *out, unsigned size) {
    folder_of(wc_app_doc(&w->app)->path, out, size);
    if(!out[0]) snprintf(out, size, "%s", w->dir);
}

static void say(struct wc_window *w, const char *s) {
    snprintf(w->app.status, sizeof(w->app.status), "%s", s);
}

/* why gk_choose() gave no chooser, from its errno */
static void no_chooser(struct wc_window *w) {
    /* EBUSY: a chooser open, or an answer not yet taken; EAGAIN: the desktop's queue of events is full */
    say(w, errno == EACCES ? "No chooser: the window must have the focus" :
        errno == EBUSY || errno == EAGAIN ? "No chooser yet: the desktop is busy; try again" :
        "No chooser: the desktop refused it");
}

static void choose(struct wc_window *w, int id, int kind) {
    struct wc_doc *d = wc_app_doc(&w->app);
    char dir[WC_PATH], name[WC_PATH];
    const char *base;
    start_dir(w, dir, sizeof(dir));
    base = strrchr(d->path, '/');
    snprintf(name, sizeof(name), "%s", !d->path[0] ? "untitled.txt" : base ? base + 1 : d->path);
    if(strlen(name) >= WIN_NAME_MAX) name[0] = 0;
    if(gk_choose(w->ui, id, kind, dir, kind == WIN_DLG_SAVE ? name : "", "") == 0) return;
    no_chooser(w);
    if(id == CHOOSE_SAVE && w->app.after_save_as) wc_app_save_as(&w->app, NULL);    /* the guard's question ends */
}

static void copy(struct wc_window *w) {
    unsigned n = wc_app_clip(&w->app, w->clip, WIN_CLIP_MAX + 1);
    if(n) wc_app_copied(&w->app, gk_copy(w->ui, w->clip, n) == 0);
}

static void paste(struct wc_window *w) {
    int len = 0;
    char *text = gk_paste_all(w->ui, &len);
    if(!text) {
        say(w, errno == EACCES ? "Not pasted: the window must have the focus" : "Not pasted: the clipboard cannot be read");
        return;
    }
    wc_app_paste(&w->app, text, (unsigned)len);
    free(text);
}

static void show(struct wc_window *w, struct gk_widget *wd, int on) {
    if(gk_shown(wd) == on) return;
    gk_show(wd, on);
    w->relayout = 1;
}

static void show_find(struct wc_window *w, int replace) {
    show(w, w->findbar, 1);
    if(replace) show(w, w->replace_row, 1);
    gk_set_text(w->find, w->app.find.what);
    gk_set_text(w->with, w->app.find.with);
    gk_field_select(w->find, 0, -1);
    gk_focus(w->find);
}

static void show_files(struct wc_window *w) {
    char dir[WC_PATH];
    show(w, w->files, 1);
    gk_set_text(w->fwhat, w->app.grep.f.what);
    if(!gk_text(w->fin)[0]) {
        start_dir(w, dir, sizeof(dir));
        gk_set_text(w->fin, dir[0] ? dir : "/");
    }
    gk_field_select(w->fwhat, 0, -1);
    gk_focus(w->fwhat);
}

static int start_files(struct wc_window *w) {
    struct wc_grep *g = &w->app.grep;
    if(g->state == WC_GREP_RUN) {               /* the button says Stop meanwhile */
        wc_grep_stop(g);
        return WC_DO_DRAW;
    }
    snprintf(g->f.what, sizeof(g->f.what), "%s", gk_text(w->fwhat));
    snprintf(g->filter, sizeof(g->filter), "%s", gk_text(w->fnames));
    g->f.icase = w->app.find.icase;
    g->f.word = w->app.find.word;
    g->f.regex = w->app.find.regex;
    gk_list_clear(w->flist);
    w->listed = 0;
    w->idle = 1;
    return wc_app_files(&w->app, gk_text(w->fin)) | WC_DO_IDLE;
}

/* Esc where the text has not the keys: the bar that has them closes, and the keys go back to the text */
static void close_bar(struct wc_window *w) {
    struct gk_widget *f = gk_focused(w->ui);
    if(f == w->fwhat || f == w->fin || f == w->fnames || f == w->flist || f == w->fgo) {
        wc_grep_stop(&w->app.grep);
        show(w, w->files, 0);
    } else {
        show(w, w->findbar, 0);
        show(w, w->replace_row, 0);
    }
    gk_focus(w->text);
}

/* what app.c said to do; 0 once the program is to end */
static int act(struct wc_window *w, int r) {
    char question[WC_PATH + 32];
    if(r & WC_DO_QUIT) return 0;
    if(r & WC_DO_COPY) copy(w);
    if(r & WC_DO_PASTE) paste(w);
    if(r & WC_DO_OPEN) choose(w, CHOOSE_OPEN, WIN_DLG_OPEN);
    if(r & WC_DO_SAVE_AS) choose(w, CHOOSE_SAVE, WIN_DLG_SAVE);
    if((r & WC_DO_ASK) && wc_app_asking(&w->app, question, sizeof(question)) >= 0)
        gk_confirm(w->ui, BOX_SAVE, question, "If you don't save them, they are lost.", "", "Save", "Don't save",
                   "Cancel");
    if(r & WC_DO_DISK) {                        /* 11/09/26 (the review, H1) */
        char name[WC_PATH];
        wc_doc_title(wc_app_doc(&w->app), name, sizeof(name));
        snprintf(question, sizeof(question), "%.60s changed on the disk", name);
        gk_confirm(w->ui, BOX_DISK, question, "Something saved it after it was opened here.",
                   "Saving puts this text over what it has now.", "Save over it", "Save as" ELLIPSIS, "Cancel");
    }
    if(r & (WC_DO_FIND | WC_DO_REPLACE)) show_find(w, (r & WC_DO_REPLACE) != 0);
    if(r & WC_DO_GOTO) gk_ask(w->ui, BOX_GOTO, "Go to line", "A line, or line:column:", "", 0);
    if(r & WC_DO_FILES) show_files(w);
    if(r & WC_DO_IDLE) w->idle = 1;
    if(w->relayout) {                           /* the toolkit lays out at a paint: the text's size is known after it */
        w->relayout = 0;
        gk_flush(w->ui);
    }
    refresh(w);
    if(!w->app.view.exact) w->idle = 1;         /* 11/09/26: drawn with a guess (a jump): the colouring catches up */
    return 1;
}

/**** events ****/

static int cell_row(int y) {
    return y < 0 ? -1 : y / WC_CH;
}

static int cell_col(const struct wc_window *w, int x) {
    int gpx = (int)(w->gutter * WC_CW);
    return x < gpx ? -1 : (x - gpx + WC_CW / 2) / WC_CW;     /* the boundary nearest the pointer */
}

static int pointer(struct wc_window *w, const struct gk_event *ev) {
    if(ev->value & WIN_BUTTON_LEFT) {           /* a press */
        w->dragging = 1;
        w->drag_x = ev->x;
        w->drag_y = ev->y;
        gk_timer(w->ui, TIMER_DRAG, 60, 1);
        return wc_app_press(&w->app, cell_row(ev->y), cell_col(w, ev->x), ev->clicks > 0 ? (unsigned)ev->clicks : 1,
                            (ev->mods & WIN_MOD_SHIFT) != 0);
    }
    if(w->dragging && (ev->buttons & WIN_BUTTON_LEFT)) {
        w->drag_x = ev->x;
        w->drag_y = ev->y;
        return wc_app_drag(&w->app, cell_row(ev->y), ev->x < 0 ? -1 : cell_col(w, ev->x) < 0 ? 0 : cell_col(w, ev->x));
    }
    if(w->dragging && !(ev->buttons & WIN_BUTTON_LEFT)) {
        w->dragging = 0;
        gk_timer_stop(w->ui, TIMER_DRAG);
    }
    return 0;
}

/* the pointer held past the text's edges: the text goes on coming to it */
static int drag_tick(struct wc_window *w) {
    struct gk_view v;
    if(!w->dragging) {
        gk_timer_stop(w->ui, TIMER_DRAG);
        return 0;
    }
    gk_area_view(w->text, &v);
    if(w->drag_y >= 0 && w->drag_y < v.h && w->drag_x >= 0 && w->drag_x < v.w) return 0;
    return wc_app_drag(&w->app, cell_row(w->drag_y), w->drag_x < 0 ? -1 : cell_col(w, w->drag_x) < 0 ? 0 :
                       cell_col(w, w->drag_x));
}

static void field_into(struct gk_widget *f, char *out, unsigned size) {
    snprintf(out, size, "%s", gk_text(f));
}

/* a person's key, click or choice (not a pointer passing by, a size, the theme or a timer) */
static int input(const struct gk_event *ev) {
    switch(ev->type) {
    case GK_EV_KEY: case GK_EV_MENU: case GK_EV_ACTIVATE: case GK_EV_CLICK: case GK_EV_TAB_CLOSE: case GK_EV_CLOSE:
        return 1;
    case GK_EV_POINTER: return (ev->value & WIN_BUTTON_LEFT) != 0;
    }
    return 0;
}

int wc_window_event(struct wc_window *w, const struct gk_event *ev) {
    struct wc_doc *d;
    int r = 0, esc;
    if(w->app.job && input(ev)) {               /* 11/09/26 (the review, C3): a search going on stops, or this waits */
        esc = ev->type == GK_EV_KEY && ev->value == 27 && !(ev->mods & WIN_MOD_CTRL);
        r = wc_app_interrupt(&w->app, esc);
        if(r < 0) return act(w, WC_DO_DRAW | WC_DO_IDLE);
        if(esc) return act(w, r);
    }
    d = wc_app_doc(&w->app);
    r = 0;
    switch(ev->type) {
    case GK_EV_KEY:
        if(ev->w == w->text) r = wc_app_key(&w->app, (unsigned)ev->value, ev->mods);
        else if(ev->value == 27) close_bar(w);
        break;
    case GK_EV_MENU:                            /* (the guard or the disk question asking: answered first) */
        if(w->app.asking >= 0 || gk_asking(w->ui) == BOX_DISK) break;
        if(ev->id == MENU_ABOUT)
            gk_message(w->ui, BOX_ABOUT, "Code Editor", "Text and code, coloured for C, assembly, Python, shell,",
                       "Makefiles, Markdown and JSON. Its keys: man wcode.");
        else r = wc_app_cmd(&w->app, ev->id);
        break;
    case GK_EV_CHANGE:
        switch(ev->id) {
        case ID_TABS:
            r = wc_app_cmd(&w->app, WC_CMD_TAB + ev->value);
            gk_focus(w->text);
            break;
        case ID_FIND: field_into(w->find, w->app.find.what, sizeof(w->app.find.what)); break;
        case ID_WITH: field_into(w->with, w->app.find.with, sizeof(w->app.find.with)); break;
        case ID_CASE: w->app.find.icase = !ev->value; break;
        case ID_WORD: w->app.find.word = ev->value; break;
        case ID_REGEX: w->app.find.regex = ev->value; break;
        }
        break;
    case GK_EV_ACTIVATE:
        switch(ev->id) {
        case ID_FIND: r = wc_app_cmd(&w->app, (ev->mods & WIN_MOD_SHIFT) ? WC_CMD_FIND_PREV : WC_CMD_FIND_NEXT); break;
        case ID_WITH: r = wc_app_cmd(&w->app, WC_CMD_REPLACE_ONE); break;
        case ID_FILES_WHAT: case ID_FILES_IN: case ID_FILES_NAMES: r = start_files(w); break;
        case ID_FILES_LIST:
            r = wc_app_hit(&w->app, (unsigned)ev->value);
            gk_focus(w->text);
            break;
        }
        break;
    case GK_EV_CLICK:
        switch(ev->id) {
        case ID_PREV: r = wc_app_cmd(&w->app, WC_CMD_FIND_PREV); break;
        case ID_NEXT: r = wc_app_cmd(&w->app, WC_CMD_FIND_NEXT); break;
        case ID_REPLACE_ONE: r = wc_app_cmd(&w->app, WC_CMD_REPLACE_ONE); break;
        case ID_REPLACE_ALL: r = wc_app_cmd(&w->app, WC_CMD_REPLACE_ALL); break;
        case ID_FIND_CLOSE:
            show(w, w->findbar, 0);
            show(w, w->replace_row, 0);
            gk_focus(w->text);
            break;
        case ID_FILES_GO: r = start_files(w); break;
        case ID_FILES_FOLDER:
            if(gk_choose(w->ui, CHOOSE_FOLDER, WIN_DLG_FOLDER, gk_text(w->fin)[0] == '/' ? gk_text(w->fin) : "", "", "") < 0)
                no_chooser(w);
            break;
        case ID_FILES_CLOSE:
            wc_grep_stop(&w->app.grep);
            show(w, w->files, 0);
            gk_focus(w->text);
            break;
        }
        break;
    case GK_EV_TAB_CLOSE:
        if(ev->value >= 0 && ev->value < w->app.n) {
            w->app.cur = ev->value;
            r = wc_app_cmd(&w->app, WC_CMD_CLOSE);
        }
        break;
    case GK_EV_POINTER:
        if(ev->w == w->text) r = pointer(w, ev);
        break;
    case GK_EV_SCROLL:
        if(ev->w == w->text) {
            d->top = (unsigned)ev->y;
            d->left = (unsigned)ev->x;
            r = WC_DO_DRAW;
        }
        break;
    case GK_EV_FOCUS:
        if(ev->w == w->text) {
            w->focused = ev->value;
            w->crow = -2;                       /* the caret's row drawn again, in its new colour */
        }
        break;
    case GK_EV_ANSWER:
        if(ev->id == BOX_DISK)
            r = wc_app_disk(&w->app, ev->value == 0 ? WC_DISK_OVER : ev->value == 1 ? WC_DISK_SAVE_AS : WC_DISK_CANCEL);
        else if(ev->id == BOX_SAVE)
            r = wc_app_answer(&w->app, ev->value == 0 ? WC_ANSWER_SAVE : ev->value == 1 ? WC_ANSWER_DISCARD :
                              WC_ANSWER_CANCEL);
        else if(ev->id == BOX_GOTO && ev->value == 0) r = wc_app_goto(&w->app, ev->text);
        break;
    case GK_EV_CHOSEN:
        if(ev->value && ev->id != CHOOSE_FOLDER) folder_of(ev->text, w->dir, sizeof(w->dir));
        if(ev->id == CHOOSE_OPEN && ev->value) r = wc_app_open(&w->app, ev->text);
        else if(ev->id == CHOOSE_SAVE) r = wc_app_save_as(&w->app, ev->value ? ev->text : NULL);
        else if(ev->id == CHOOSE_FOLDER && ev->value) gk_set_text(w->fin, ev->text);
        break;
    case GK_EV_CLOSE:                           /* pressed again while the guard asks: its question stands */
        if(w->app.asking < 0 && gk_asking(w->ui) != BOX_DISK) r = wc_app_cmd(&w->app, WC_CMD_QUIT);
        break;
    case GK_EV_TIMER:
        if(ev->id == TIMER_DRAG) r = drag_tick(w);
        break;
    case GK_EV_RESIZE: case GK_EV_THEME:
        w->doc_drawn = -1;                      /* all of it again */
        w->relayout = 1;
        break;
    }
    return act(w, r);
}

int wc_window_idle(struct wc_window *w) {
    int r = wc_app_idle(&w->app);
    w->idle = (r & WC_DO_IDLE) != 0;
    if(r & (WC_DO_DRAW | WC_DO_FILES)) refresh(w);
    if(!w->app.view.exact) w->idle = 1;
    return w->idle;
}

int wc_window_path(struct wc_window *w, const char *path, int exists) {
    return act(w, exists ? wc_app_open(&w->app, path) : wc_app_new_file(&w->app, path));
}

/**** the window ****/

static struct gk_widget *labelled(struct gk_widget *row, const char *text) {
    struct gk_widget *l = gk_label(row, text);
    gk_size(l, 64, 0);
    return l;
}

int wc_window_open(struct wc_window *w, const struct wc_app_io *io, const struct wc_fsops *fs) {
    struct gk_widget *root, *row;
    int m, l, e;
    memset(w, 0, sizeof(*w));
    w->crow = -1;
    w->doc_drawn = -1;
    w->clip = malloc(WIN_CLIP_MAX + 1);
    if(!w->clip || wc_app_init(&w->app, io, fs) < 0) {
        free(w->clip);
        errno = ENOMEM;
        return -1;
    }
    w->app.find.icase = 1;
    w->ui = gk_open_limits("Code Editor", WC_WIN_W, WC_WIN_H, 400, 240, 0, 0, WIN_RESIZABLE);
    if(!w->ui) {
        e = errno;
        wc_app_free(&w->app);
        free(w->clip);
        errno = e;
        return -1;
    }
    root = gk_root(w->ui);
    w->tabs = gk_tabs(root, ID_TABS, GK_CLOSABLE);
    gk_grow(w->tabs, 1, 0);                     /* a strip: the text is under it, the same for every tab */
    gk_tab(w->tabs, "Untitled 1");
    w->text = gk_area(root, ID_TEXT, GK_FOCUSABLE | GK_TAKES_TAB, draw, w);
    /* the find bar, and the replacement's line */
    w->findbar = row = gk_hbox(root, 6);
    gk_pad(row, 4);
    labelled(row, "Find:");
    w->find = gk_field(row, ID_FIND, WC_FIND_MAX - 1, 0);
    gk_grow(w->find, 1, 0);
    w->icase = gk_check(row, "Case", ID_CASE, 0);
    w->word = gk_check(row, "Words", ID_WORD, 0);
    w->regex = gk_check(row, "Regex", ID_REGEX, 0);
    gk_button(row, "Previous", ID_PREV);
    gk_button(row, "Next", ID_NEXT);
    gk_button(row, "x", ID_FIND_CLOSE);
    w->replace_row = row = gk_hbox(root, 6);
    gk_pad(row, 4);
    labelled(row, "Replace:");
    w->with = gk_field(row, ID_WITH, WC_FIND_MAX - 1, 0);
    gk_grow(w->with, 1, 0);
    gk_button(row, "Replace", ID_REPLACE_ONE);
    gk_button(row, "Replace all", ID_REPLACE_ALL);
    /* find in files */
    w->files = gk_vbox(root, 4);
    gk_pad(w->files, 4);
    row = gk_hbox(w->files, 6);
    labelled(row, "Find:");
    w->fwhat = gk_field(row, ID_FILES_WHAT, WC_FIND_MAX - 1, 0);
    gk_grow(w->fwhat, 1, 0);
    gk_label(row, "Names:");
    w->fnames = gk_field(row, ID_FILES_NAMES, WC_GREP_FILTER - 1, 0);
    gk_field_hint(w->fnames, "*.c *.h");
    gk_size(w->fnames, 120, 0);
    w->fgo = gk_button(row, "Search", ID_FILES_GO);
    gk_button(row, "x", ID_FILES_CLOSE);
    row = gk_hbox(w->files, 6);
    labelled(row, "In:");
    w->fin = gk_field(row, ID_FILES_IN, WC_PATH - 1, 0);
    gk_grow(w->fin, 1, 0);
    gk_button(row, "Folder" ELLIPSIS, ID_FILES_FOLDER);
    w->flist = gk_list(w->files, ID_FILES_LIST);
    gk_size(w->flist, 0, 6 * GK_ROW_H + 4);
    w->fsum = gk_label(w->files, "");
    w->status = gk_status(root);
    gk_show(w->findbar, 0);
    gk_show(w->replace_row, 0);
    gk_show(w->files, 0);
    /* the menus: their shortcuts work whether or not the desktop shows them */
    m = gk_menu(w->ui, "File");
    gk_menu_item(w->ui, m, WC_CMD_NEW, "New", GK_CTRL | 'N', 0);
    gk_menu_item(w->ui, m, WC_CMD_OPEN, "Open" ELLIPSIS, GK_CTRL | 'O', 0);
    gk_menu_item(w->ui, m, WC_CMD_SAVE, "Save", GK_CTRL | 'S', 0);
    gk_menu_item(w->ui, m, WC_CMD_SAVE_AS, "Save as" ELLIPSIS, GK_CTRL | GK_SHIFT | 'S', 0);
    gk_menu_line(w->ui, m);
    gk_menu_item(w->ui, m, WC_CMD_CLOSE, "Close tab", GK_CTRL | 'W', 0);
    gk_menu_item(w->ui, m, WC_CMD_QUIT, "Quit", GK_CTRL | 'Q', 0);
    m = gk_menu(w->ui, "Edit");
    gk_menu_item(w->ui, m, WC_CMD_UNDO, "Undo", GK_CTRL | 'Z', 0);
    gk_menu_item(w->ui, m, WC_CMD_REDO, "Redo", GK_CTRL | 'Y', 0);
    gk_menu_line(w->ui, m);
    gk_menu_item(w->ui, m, WC_CMD_CUT, "Cut", GK_CTRL | 'X', 0);
    gk_menu_item(w->ui, m, WC_CMD_COPY, "Copy", GK_CTRL | 'C', 0);
    gk_menu_item(w->ui, m, WC_CMD_PASTE, "Paste", GK_CTRL | 'V', 0);
    gk_menu_item(w->ui, m, WC_CMD_DELETE, "Delete", WIN_KEY_DELETE, 0);
    gk_menu_item(w->ui, m, WC_CMD_SELECT_ALL, "Select all", GK_CTRL | 'A', 0);
    gk_menu_line(w->ui, m);
    gk_menu_item(w->ui, m, WC_CMD_INDENT, "Indent lines", 0, 0);     /* Ctrl+] and Tab: keys of the text's */
    gk_menu_item(w->ui, m, WC_CMD_OUTDENT, "Outdent lines", 0, 0);   /* Ctrl+[ and Shift+Tab */
    gk_menu_item(w->ui, m, WC_CMD_COMMENT, "Comment lines", 0, 0);   /* Ctrl+/ */
    m = gk_menu(w->ui, "Search");
    gk_menu_item(w->ui, m, WC_CMD_FIND, "Find" ELLIPSIS, GK_CTRL | 'F', 0);
    gk_menu_item(w->ui, m, WC_CMD_FIND_NEXT, "Find next", WIN_KEY_F(5), 0);
    gk_menu_item(w->ui, m, WC_CMD_FIND_PREV, "Find previous", GK_SHIFT | WIN_KEY_F(5), 0);
    gk_menu_item(w->ui, m, WC_CMD_REPLACE, "Replace" ELLIPSIS, GK_CTRL | 'H', 0);
    gk_menu_line(w->ui, m);
    gk_menu_item(w->ui, m, WC_CMD_FIND_FILES, "Find in files" ELLIPSIS, GK_CTRL | GK_SHIFT | 'F', 0);
    gk_menu_line(w->ui, m);
    gk_menu_item(w->ui, m, WC_CMD_GOTO, "Go to line" ELLIPSIS, GK_CTRL | 'L', 0);
    gk_menu_item(w->ui, m, WC_CMD_BRACKET, "Go to bracket", GK_CTRL | 'B', 0);
    m = gk_menu(w->ui, "View");
    l = gk_submenu(w->ui, m, MENU_LANGUAGE, "Language", 0);
    for(e = 0; e < WC_LANGS; e++) gk_menu_item(w->ui, l, WC_CMD_LANG + e, wc_lang_name(e), 0, GK_ITEM_RADIO);
    gk_menu_line(w->ui, m);
    gk_menu_item(w->ui, m, WC_CMD_NEXT_TAB, "Next tab", GK_CTRL | WIN_KEY_TAB, 0);
    gk_menu_item(w->ui, m, WC_CMD_PREV_TAB, "Previous tab", GK_CTRL | GK_SHIFT | WIN_KEY_TAB, 0);
    m = gk_menu(w->ui, "Help");
    gk_menu_item(w->ui, m, MENU_ABOUT, "About Code Editor", 0, 0);
    gk_focus(w->text);
    if(!gk_ok(w->ui)) {
        wc_window_close(w);
        errno = ENOMEM;
        return -1;
    }
    refresh(w);
    return 0;
}

void wc_window_close(struct wc_window *w) {
    if(w->ui) gk_close(w->ui);
    w->ui = NULL;
    wc_app_free(&w->app);
    free(w->cells);
    free(w->drawn);
    free(w->clip);
    w->cells = w->drawn = NULL;
    w->clip = NULL;
}
