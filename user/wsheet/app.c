/*
SurfOS user program: wsheet
--------------------
File: app.c     Date: 11/05/26
--------------------
The Spreadsheet's window (app.h), on the toolkit (gk.h).

    [ B3     ][ =SUM(B1:B2)                                   ]    the formula bar
    [   ][  A  ][  B  ][  C  ] ...                                 the grid: headers and cells,
    [ 1 ][ ... ]                                                   the program's own drawing
    [ Sum 6  Average 2  Count 3                         Modified ] the status bar

Typing in the grid replaces the active cell's content as it goes (shown in the cell and in the formula
bar): Enter puts it in and moves down, Tab right (Shift back), an arrow puts it in and moves that
way, Esc leaves the cell as it was. Ctrl+E, a double-click or a click in the formula bar edits what the
cell holds there, where the arrows move in the text; Enter puts it in, Esc cancels, and leaving the
bar (a click on the grid, Tab) puts it in. The cell's name at the left takes a name or a range to go
to (Ctrl+L). A formula that does not parse goes in as typed and shows #ERROR!; the status bar says
why.

The menus, in the desktop's bar while the window has the focus and by their shortcuts in any case:
File (New, Open, Save, Save as, Close), Edit (Undo, Redo, Cut, Copy, Paste, Delete, Select all, Go
to, Edit the cell), Sheet (rows and columns inserted and deleted, column widths), Format (General,
fixed decimals, percent, more and fewer decimals) and the program's part of the desktop's Help.
Open and Save as are the desktop's choosers; a file is read a piece at a time between the window's
events, so the window answers meanwhile and Esc stops it. New, Open and Close ask first about
unsaved changes; saving as CSV asks first when formulas, formats or column widths would be lost,
and after such a save New, Open and Close ask again, unless a .sheet was saved since the sheet last
changed (11/09/26, the review).

Copy puts the cells' values on the desktop's clipboard as tab-separated text, and keeps the cells
themselves: a paste of what the clipboard still holds from this copy puts the cells back with their
formulas moved (a Cut's cells moved there, keeping their references), and other text is pasted as
typed, a field a cell. Only what changed is painted again: a move of the active cell paints the
cells it left and reached and the headers, a change of the sheet the grid.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "app.h"

static char chunk[APP_CHUNK];           /* a file's piece being read */

static void say(struct app *a, const char *msg) {
    snprintf(a->msg, sizeof(a->msg), "%s", msg);
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static const char *sheet_name(const struct app *a) {
    return a->path[0] ? base_name(a->path) : "Untitled";
}

static void range_name(struct sh_range r, char *out, int size) {
    char a[16], b[16];
    sh_cellname(r.r1, r.c1, a);
    if(r.r1 == r.r2 && r.c1 == r.c2) snprintf(out, (size_t)size, "%s", a);
    else {
        sh_cellname(r.r2, r.c2, b);
        snprintf(out, (size_t)size, "%s:%s", a, b);
    }
}

/**** drawing the grid ****/

/* text in the cell from x0 to x1 (pixels in the area), cut there, as align says */
static void put_text(struct gw *win, int x0, int x1, int y, const char *s, int align, unsigned color) {
    struct gw_view keep;
    int cells = gw_text_width(s) / 8, tx = x0 + V_PAD;
    if(align == SH_RIGHT) tx = x1 - V_PAD - cells * 8;
    else if(align == SH_CENTER) tx = x0 + (x1 - x0 - cells * 8) / 2;
    gw_get_view(win, &keep);
    gw_clip_to(win, x0, y, x1 - x0 - 1, V_ROW_H - 1);
    gw_text(win, tx, y + (V_ROW_H - 16) / 2, s, color);
    gw_set_view(win, &keep);
}

static void draw_headers(struct app *a, struct gw *win, const struct gk_theme *t, struct sh_range sel, int clip_y0,
                         int clip_y1) {
    struct view *v = &a->v;
    char name[16];
    int c, r, x, y, w, in;
    gw_rect(win, 0, 0, V_ROWHEAD_W, V_HEAD_H, t->band);
    gw_rect(win, V_ROWHEAD_W - 1, 0, 1, V_HEAD_H, t->line);
    for(c = v->left, x = V_ROWHEAD_W; c < SH_COLS && x < v->w; c++, x += w) {
        w = view_width(v, c);
        in = c >= sel.c1 && c <= sel.c2;
        gw_rect(win, x, 0, w, V_HEAD_H, in ? t->pick : t->band);
        gw_rect(win, x + w - 1, 0, 1, V_HEAD_H, t->line);
        sh_colname(c, name);
        put_text(win, x, x + w, 0, name, SH_CENTER, c == v->col ? t->teal : t->ink);
    }
    gw_rect(win, 0, V_HEAD_H - 1, v->w, 1, t->line);
    for(r = v->top, y = V_HEAD_H; r < SH_ROWS && y < v->h; r++, y += V_ROW_H) {
        if(y + V_ROW_H <= clip_y0 || y >= clip_y1) continue;
        in = r >= sel.r1 && r <= sel.r2;
        gw_rect(win, 0, y, V_ROWHEAD_W - 1, V_ROW_H, in ? t->pick : t->band);
        gw_rect(win, 0, y + V_ROW_H - 1, V_ROWHEAD_W - 1, 1, t->line);
        gw_rect(win, V_ROWHEAD_W - 1, y, 1, V_ROW_H, t->line);
        snprintf(name, sizeof(name), "%d", r + 1);
        put_text(win, 0, V_ROWHEAD_W - 1, y, name, SH_RIGHT, r == v->row ? t->teal : t->ink);
    }
}

static void draw_row(struct app *a, struct gw *win, const struct gk_theme *t, int r, int y) {
    static char text[1024];
    struct view *v = &a->v;
    struct sh_value val;
    int c, x, w, align, end, k, kx;
    unsigned bg;
    for(c = v->left, x = V_ROWHEAD_W; c < SH_COLS && x < v->w; c++, x += w) {      /* the backgrounds and lines */
        w = view_width(v, c);
        bg = view_selected(v, r, c) && !(r == v->row && c == v->col) ? t->pick : t->sheet;
        gw_rect(win, x, y, w - 1, V_ROW_H - 1, bg);
        gw_rect(win, x + w - 1, y, 1, V_ROW_H, t->line);
        gw_rect(win, x, y + V_ROW_H - 1, w, 1, t->line);
    }
    for(c = v->left, x = V_ROWHEAD_W; c < SH_COLS && x < v->w; c++, x += w) {      /* the values */
        w = view_width(v, c);
        if(a->editing && r == a->edit_row && c == a->edit_col) continue;          /* the edit is drawn over it */
        sh_value(a->s, r, c, &val);
        if(val.type == SH_EMPTY) continue;
        sh_display(a->s, r, c, view_chars(v, c), text, sizeof(text), &align);
        end = x + w;
        if(val.type == SH_TEXT && gw_text_width(text) > w - 2 * V_PAD) {          /* on over empty cells to the right */
            end = view_text_end(v, r, c);
            for(k = c + 1, kx = x + w; kx < end; kx += view_width(v, k), k++)
                gw_rect(win, kx - 1, y, view_width(v, k), V_ROW_H - 1,
                        view_selected(v, r, k) && !(r == v->row && k == v->col) ? t->pick : t->sheet);
        }
        put_text(win, x, end, y, text, align, val.type == SH_ERROR ? t->coral : t->ink);
    }
}

/* the pixels of range r as the grid shows it (within its cells), 0 when none of it shows */
static int range_box(const struct view *v, struct sh_range r, int *x0, int *y0, int *x1, int *y1) {
    int c, x;
    if(r.r2 < v->top || r.c2 < v->left) return 0;
    if(r.r1 < v->top) r.r1 = v->top;
    if(r.c1 < v->left) r.c1 = v->left;
    *x0 = view_col_x(v, r.c1);
    *y0 = view_row_y(v, r.r1);
    if(*x0 < 0 || *y0 < 0) return 0;
    for(c = r.c1, x = *x0; c <= r.c2 && x < v->x + v->w; c++) x += view_width(v, c);
    *x1 = x < v->x + v->w ? x : v->x + v->w;
    *y1 = *y0 + (r.r2 - r.r1 + 1) * V_ROW_H;
    if(*y1 > v->y + v->h || r.r2 - r.r1 > v->h / V_ROW_H) *y1 = v->y + v->h;
    return 1;
}

static void draw(struct gk_widget *area, struct gw *win, const struct gk_view *gv, void *data) {
    static char text[SH_INPUT_MAX + 64];
    struct app *a = data;
    const struct gk_theme *t = gk_theme(a->ui);
    struct view *v = &a->v;
    struct sh_range sel, one;
    int r, y, x0, y0, x1, y1, w;
    (void)area;
    v->x = v->y = 0;
    v->w = gv->w;
    v->h = gv->h;
    view_selection(v, &sel);
    for(r = v->top, y = V_HEAD_H; r < SH_ROWS && y < v->h; r++, y += V_ROW_H)
        if(y + V_ROW_H > gv->cy && y < gv->cy + gv->ch) draw_row(a, win, t, r, y);
    if(y < v->h) gw_rect(win, 0, y, v->w, v->h - y, t->paper);
    draw_headers(a, win, t, sel, gv->cy, gv->cy + gv->ch);
    if((sel.r1 != sel.r2 || sel.c1 != sel.c2) && range_box(v, sel, &x0, &y0, &x1, &y1))
        gw_frame(win, x0 - 1, y0 - 1, x1 - x0 + 1, y1 - y0 + 1, t->teal);
    one.r1 = one.r2 = v->row;
    one.c1 = one.c2 = v->col;
    if(range_box(v, one, &x0, &y0, &x1, &y1)) {                 /* the active cell: two pixels of the accent */
        gw_frame(win, x0 - 1, y0 - 1, x1 - x0 + 1, y1 - y0 + 1, t->teal);
        gw_frame(win, x0, y0, x1 - x0 - 1, y1 - y0 - 1, t->teal);
    }
    one.r1 = one.r2 = a->edit_row;
    one.c1 = one.c2 = a->edit_col;
    if(a->editing && range_box(v, one, &x0, &y0, &x1, &y1)) {   /* the edit, over the cell and on to the right */
        snprintf(text, sizeof(text), "%s", a->editing == E_FIELD ? gk_text(a->formula) : a->edit);
        w = gw_text_width(text) + 2 * V_PAD + (a->editing == E_TYPING ? 2 : 0);
        if(x1 - x0 < w) x1 = x0 + w < v->w ? x0 + w : v->w;
        gw_rect(win, x0, y0, x1 - x0 - 1, V_ROW_H - 1, t->sheet);
        put_text(win, x0, x1, y0, text, SH_LEFT, t->ink);
        if(a->editing == E_TYPING && x0 + V_PAD + gw_text_width(text) < v->w)    /* the caret */
            gw_rect(win, x0 + V_PAD + gw_text_width(text), y0 + 2, 1, 16, t->ink);
        gw_frame(win, x0 - 1, y0 - 1, x1 - x0 + 1, V_ROW_H + 1, t->teal);
        gw_frame(win, x0, y0, x1 - x0 - 1, V_ROW_H - 1, t->teal);
    }
}

/**** what changed, painted again ****/

struct snap {
    int top, left, row, col, erow, ecol, editing, edit_row, edit_col, edit_len, width, drag;
    unsigned edit_hash;
};

static unsigned hash(const char *s, int n) {   /* FNV-1a */
    unsigned h = 2166136261u;
    int i;
    for(i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h;
}

static void snapshot(const struct app *a, struct snap *b) {
    const char *t = a->editing == E_FIELD ? gk_text(a->formula) : a->edit;
    int n = a->editing == E_FIELD ? (int)strlen(t) : a->edit_len;
    b->top = a->v.top;
    b->left = a->v.left;
    b->row = a->v.row;
    b->col = a->v.col;
    b->erow = a->v.erow;
    b->ecol = a->v.ecol;
    b->editing = a->editing;
    b->edit_row = a->edit_row;
    b->edit_col = a->edit_col;
    b->edit_len = n;
    b->edit_hash = a->editing ? hash(t, n) : 0;
    b->drag = a->v.drag;
    b->width = a->v.drag == VD_WIDTH ? a->v.width_now : 0;
}

/* a range as the grid shows it, painted again, with a pixel round it for the frames */
static void damage_range(struct app *a, struct sh_range r) {
    int x0, y0, x1, y1;
    if(range_box(&a->v, r, &x0, &y0, &x1, &y1)) gk_damage(a->grid, x0 - 2, y0 - 2, x1 - x0 + 4, y1 - y0 + 4);
}

static void damage_row_on(struct app *a, int r, int c) {    /* a cell and the row to the right of it: an edit's room */
    int x = view_col_x(&a->v, c), y = view_row_y(&a->v, r);
    if(x >= 0 && y >= 0) gk_damage(a->grid, x - 2, y - 2, a->v.w - x + 2, V_ROW_H + 4);
}

static void damage(struct app *a, const struct snap *b) {
    struct sh_range was, now;
    if(a->all || b->top != a->v.top || b->left != a->v.left ||
       b->width != (a->v.drag == VD_WIDTH ? a->v.width_now : 0) ||
       (b->drag == VD_WIDTH && a->v.drag != VD_WIDTH)) {
        gk_redraw(a->grid);
        return;
    }
    if(b->row != a->v.row || b->col != a->v.col || b->erow != a->v.erow || b->ecol != a->v.ecol) {
        was.r1 = b->row < b->erow ? b->row : b->erow;
        was.r2 = b->row < b->erow ? b->erow : b->row;
        was.c1 = b->col < b->ecol ? b->col : b->ecol;
        was.c2 = b->col < b->ecol ? b->ecol : b->col;
        view_selection(&a->v, &now);
        damage_range(a, was);
        damage_range(a, now);
        gk_damage(a->grid, 0, 0, a->v.w, V_HEAD_H);             /* the headers that light the selection */
        gk_damage(a->grid, 0, 0, V_ROWHEAD_W, a->v.h);
    }
    if(b->editing || a->editing) {
        struct snap n;
        snapshot(a, &n);
        if(n.editing != b->editing || n.edit_len != b->edit_len || n.edit_hash != b->edit_hash ||
           n.edit_row != b->edit_row || n.edit_col != b->edit_col) {
            if(b->editing) damage_row_on(a, b->edit_row, b->edit_col);
            if(a->editing) damage_row_on(a, a->edit_row, a->edit_col);
        }
    }
}

/* the formula bar, the cell's name, the status bar, the title and the menus as the sheet is now */
static void show(struct app *a) {
    static char input[SH_INPUT_MAX + 64];
    char line[APP_MSG + 64], name[40], why[SH_WHY];
    struct sh_range r;
    int fmt = sh_format(a->s, a->v.row, a->v.col), kind = SH_FMT_KIND(fmt);
    view_selection(&a->v, &r);
    range_name(r, name, sizeof(name));
    if(gk_focused(a->ui) != a->name && strcmp(gk_text(a->name), name)) gk_set_text(a->name, name);
    if(a->editing == E_TYPING) snprintf(input, sizeof(input), "%s", a->edit);
    else if(a->editing == E_NONE) sh_input(a->s, a->v.row, a->v.col, input, sizeof(input));
    if(a->editing != E_FIELD && strcmp(gk_text(a->formula), input)) gk_set_text(a->formula, input);
    if(a->msg[0]) snprintf(line, sizeof(line), "%s", a->msg);
    else if(a->rd) snprintf(line, sizeof(line), "Reading %s: Esc stops it", base_name(a->rd_path));
    else if(a->editing) {
        sh_cellname(a->edit_row, a->edit_col, name);
        snprintf(line, sizeof(line), a->editing == E_TYPING ? "Typing into %s: Enter or Tab puts it in, Esc cancels" :
                 "Editing %s: Enter puts it in, Esc cancels", name);
    } else {
        if(a->said.s != a->s || a->said.state != sh_state(a->s) || a->said.row != a->v.row ||
           a->said.col != a->v.col || a->said.erow != a->v.erow || a->said.ecol != a->v.ecol) {
            view_status(&a->v, a->said.text, sizeof(a->said.text));     /* a whole sheet's sum: not at every move */
            a->said.s = a->s;
            a->said.state = sh_state(a->s);
            a->said.row = a->v.row;
            a->said.col = a->v.col;
            a->said.erow = a->v.erow;
            a->said.ecol = a->v.ecol;
        }
        snprintf(why, sizeof(why), "%s", a->said.text);
        if(why[0]) snprintf(line, sizeof(line), "%s", why);
        else if(sh_limited(a->s))
            snprintf(line, sizeof(line), "Cells marked #LIMIT! were not worked out: too much for SurfOS");
        else line[0] = 0;
    }
    if(strcmp(gk_text(a->status), line)) gk_set_text(a->status, line);
    gk_status_right(a->status, sh_modified(a->s) ? "Modified" : "");
    snprintf(line, sizeof(line), "%s - Spreadsheet", sheet_name(a));
    if(strcmp(line, a->title)) {               /* the title is a call to the desktop: only when it changes */
        snprintf(a->title, sizeof(a->title), "%s", line);
        gw_title(gk_gw(a->ui), line);
    }
    gk_menu_enable(a->ui, MENU_UNDO, sh_undo_label(a->s) != NULL || a->editing);
    gk_menu_enable(a->ui, MENU_REDO, sh_redo_label(a->s) != NULL && !a->editing);
    gk_menu_enable(a->ui, MENU_PASTE, a->editing != E_FIELD);
    gk_menu_check(a->ui, MENU_GENERAL, kind == SH_GENERAL);
    gk_menu_check(a->ui, MENU_FIXED, kind == SH_FIXED(0));
    gk_menu_check(a->ui, MENU_PERCENT, kind == SH_PERCENT(0));
}

/**** editing ****/

static void type_start(struct app *a) {
    a->editing = E_TYPING;
    a->edit_row = a->v.row;
    a->edit_col = a->v.col;
    a->edit_len = 0;
    a->edit[0] = 0;
    a->v.erow = a->v.row;                       /* the active cell alone, Tab's column kept for Enter */
    a->v.ecol = a->v.col;
    view_show(&a->v, a->v.row, a->v.col);
}

static void type_add(struct app *a, const char *s, int n) {
    if(a->edit_len + n > SH_INPUT_MAX + ((a->edit_len ? a->edit[0] : n ? s[0] : 0) == '\'')) {
        say(a, "A cell holds 8,192 bytes at most");
        return;
    }
    memcpy(a->edit + a->edit_len, s, (size_t)n);
    a->edit_len += n;
    a->edit[a->edit_len] = 0;
}

static void type_back(struct app *a) {          /* the last character, a UTF-8 sequence whole */
    while(a->edit_len > 0 && ((unsigned char)a->edit[a->edit_len - 1] & 0xC0) == 0x80) a->edit_len--;
    if(a->edit_len > 0) a->edit_len--;
    a->edit[a->edit_len] = 0;
}

/* the edit put into its cell: 0, or -1 when the sheet refused it (it stays, and the status bar says why) */
static int commit(struct app *a) {
    static char was[SH_INPUT_MAX + 64];
    const char *text = a->editing == E_FIELD ? gk_text(a->formula) : a->edit;
    char why[SH_WHY], name[16], line[APP_MSG];
    if(!a->editing) return 0;
    if(a->editing == E_FIELD) {                 /* the bar as the cell is: nothing changes (11/09/26, the review) */
        sh_input(a->s, a->edit_row, a->edit_col, was, sizeof(was));
        if(!strcmp(was, text)) {
            a->editing = E_NONE;
            a->all = 1;
            return 0;
        }
    }
    if(sh_set(a->s, a->edit_row, a->edit_col, text, (int)strlen(text)) < 0) {
        say(a, sh_why(a->s));
        return -1;
    }
    a->editing = E_NONE;
    a->all = 1;
    if(sh_problem(a->s, a->edit_row, a->edit_col, why, sizeof(why))) {
        sh_cellname(a->edit_row, a->edit_col, name);
        snprintf(line, sizeof(line), "%s: %s", name, why);
        say(a, line);
    }
    return 0;
}

static void cancel(struct app *a) {
    if(!a->editing) return;
    a->editing = E_NONE;
    a->all = 1;
    if(gk_focused(a->ui) == a->formula) gk_focus(a->grid);
}

/* the active cell, when the formula bar cannot edit what it holds: kit's field makes a control character
   a space, so a line break or a tab would be lost by an edit that changed nothing else. 1, saying so */
static int bar_refuses(struct app *a) {
    static char input[SH_INPUT_MAX + 64];
    char name[16], line[APP_MSG];
    const char *p;
    sh_input(a->s, a->v.row, a->v.col, input, sizeof(input));
    for(p = input; *p && (unsigned char)*p >= 32; p++) ;
    if(!*p) return 0;
    sh_cellname(a->v.row, a->v.col, name);
    snprintf(line, sizeof(line), "%s holds a line break or a tab, which the formula bar cannot edit: type to replace it",
             name);
    say(a, line);
    return 1;
}

/* the active cell edited in the formula bar: what it holds, the cursor at its end */
static void edit_in_bar(struct app *a) {
    static char input[SH_INPUT_MAX + 64];
    if(a->editing == E_FIELD) {                 /* edited there already: what is typed stays */
        gk_focus(a->formula);
        return;
    }
    if(a->editing != E_TYPING && bar_refuses(a)) return;
    if(a->editing == E_TYPING) snprintf(input, sizeof(input), "%s", a->edit);
    else sh_input(a->s, a->v.row, a->v.col, input, sizeof(input));
    if(a->editing != E_TYPING) {
        a->edit_row = a->v.row;
        a->edit_col = a->v.col;
    }
    a->editing = E_FIELD;
    gk_set_text(a->formula, input);
    gk_focus(a->formula);
}

static int typing_key(unsigned key, unsigned mods) {
    return !(mods & (WIN_MOD_CTRL | WIN_MOD_ALT)) && ((key >= 32 && key < 127) || (key >= 128 && key < 256));
}

static void grid_key(struct app *a, unsigned key, unsigned mods) {
    int vm = ((mods & WIN_MOD_SHIFT) ? VM_SHIFT : 0) | ((mods & WIN_MOD_CTRL) ? VM_CTRL : 0);
    int back = (mods & WIN_MOD_SHIFT) != 0;
    char c = (char)key;
    if(a->editing == E_TYPING) {
        if(typing_key(key, mods)) {
            type_add(a, &c, 1);
            return;
        }
        switch(key) {
        case 127: type_back(a); return;
        case 27: cancel(a); return;
        case '\r': if(!commit(a)) view_enter(&a->v, back); return;
        case '\t': if(!commit(a)) view_tab(&a->v, 0); return;
        case WIN_KEY_BACKTAB: if(!commit(a)) view_tab(&a->v, 1); return;
        case WIN_KEY_UP: case WIN_KEY_DOWN: case WIN_KEY_LEFT: case WIN_KEY_RIGHT:
        case WIN_KEY_PGUP: case WIN_KEY_PGDN: case WIN_KEY_HOME: case WIN_KEY_END:
            if(!commit(a)) view_key(&a->v, (int)key, vm);
            return;
        }
        return;
    }
    if(typing_key(key, mods)) {                 /* a key that types begins the cell anew */
        type_start(a);
        type_add(a, &c, 1);
        return;
    }
    switch(key) {
    case '\r': view_enter(&a->v, back); break;
    case '\t': view_tab(&a->v, 0); break;
    case WIN_KEY_BACKTAB: view_tab(&a->v, 1); break;
    case 127: type_start(a); break;             /* Backspace: the cell emptied, as typing begins */
    default: view_key(&a->v, (int)key, vm); break;
    }
}

static void grid_pointer(struct app *a, const struct gk_event *ev) {
    struct view_at at;
    int vm = (ev->mods & WIN_MOD_SHIFT) ? VM_SHIFT : 0;
    if(ev->value & WIN_BUTTON_LEFT) {           /* pressed now */
        if(a->editing && commit(a) < 0) return;
        view_at(&a->v, ev->x, ev->y, &at);
        view_press(&a->v, ev->x, ev->y, vm);
        if(ev->clicks == 2 && at.what == VA_CELL) {     /* a double-click on a cell edits it */
            a->v.drag = VD_NONE;
            edit_in_bar(a);
        }
    } else if(ev->buttons & WIN_BUTTON_LEFT) view_move(&a->v, ev->x, ev->y);
    else if(a->v.drag) {
        if(a->v.drag == VD_WIDTH) a->all = 1;
        view_release(&a->v);
    }
}

/**** files ****/

static void title_path(struct app *a, const char *path) {
    if(path != a->path) snprintf(a->path, sizeof(a->path), "%s", path);    /* never onto itself: undefined */
}

static void failed(struct app *a, const char *title, const char *line1, const char *line2) {
    gk_message(a->ui, BOX_FAILED, title, line1, line2);
}

/* the window's sheet now s (the one it had is freed): the view on its top left, nothing being edited */
static void take_sheet(struct app *a, struct sheet *s) {
    struct view keep = a->v;
    sh_free(a->s);
    a->s = s;
    a->said.s = NULL;                           /* a new sheet may have the old one's address */
    a->editing = E_NONE;
    a->lossy = 0;
    a->whole_known = 0;
    view_init(&a->v, s);
    view_place(&a->v, keep.x, keep.y, keep.w, keep.h);
    gk_scroll_to(a->grid, 0, 0);
    a->all = 1;
}

static void start_read(struct app *a, const char *path) {
    char line[APP_MSG];
    long size = 0;
    struct sheet *s;
    int h = a->io.open(a->io.ctx, path, &size);
    if(h < 0) {
        snprintf(line, sizeof(line), "%s: %s", base_name(path), strerror(errno));
        failed(a, "Not opened", line, "");
        return;
    }
    /* the sheet the file replaces goes first (its question answered, if it had changes): a reader may need the
       whole budget, so a file the window saved at its budget opens again in it (11/09/26, the second review);
       a file that cannot be read then leaves a new, empty sheet */
    sh_free(a->s);
    a->s = NULL;
    s = sh_new();
    if(!s) {                                    /* nothing left of a budget just freed: no sheet to show */
        a->io.close(a->io.ctx, h);
        a->quit = 1;
        return;
    }
    take_sheet(a, s);
    a->path[0] = 0;
    a->rd = sh_read_begin(sh_kind_of(path));
    if(!a->rd) {
        a->io.close(a->io.ctx, h);
        failed(a, "Not opened", "Not enough memory to read a sheet.", "");
        return;
    }
    a->rd_h = h;
    a->rd_size = size;
    a->rd_done = 0;
    snprintf(a->rd_path, sizeof(a->rd_path), "%s", path);
}

static void stop_read(struct app *a) {
    if(!a->rd) return;
    sh_read_cancel(a->rd);
    a->rd = NULL;
    a->io.close(a->io.ctx, a->rd_h);
    a->rd_h = -1;
}

static void read_done(struct app *a) {
    char why[SH_WHY], line[APP_MSG];
    struct sheet *old = a->s;                   /* start_read()'s empty sheet: freed once the new one is built */
    struct sheet *s = sh_read_finish(a->rd, why, sizeof(why), &old);
    a->rd = NULL;
    a->io.close(a->io.ctx, a->rd_h);
    a->rd_h = -1;
    if(!s) {
        snprintf(line, sizeof(line), "%s could not be read:", base_name(a->rd_path));
        failed(a, "Not opened", line, why);
        return;
    }
    a->s = NULL;                                /* freed by sh_read_finish() */
    take_sheet(a, s);
    a->whole = sh_state(s);                     /* a .sheet opened holds all of it */
    a->whole_known = sh_kind_of(a->rd_path) == SH_NATIVE;
    title_path(a, a->rd_path);
    snprintf(line, sizeof(line), "Opened %s: %d cells%s", base_name(a->path), sh_cells(s),
             sh_kind_of(a->path) == SH_CSV ? " (CSV: values only)" : "");
    say(a, line);
    a->all = 1;
}

/* done of size, in whole percent below 100: in 32 bits, multiplied first only where that cannot overflow (the
   review's L10: a file past 21 MB did), since a long long's division would need libgcc, which programs lack */
static int percent(long done, long size) {
    long p;
    if(done >= size) return 99;
    p = size < 0x7FFFFFFFL / 100 ? done * 100 / size : done / (size / 100);
    return p > 99 ? 99 : (int)p;                /* dividing the size first can reach 100 before the end */
}

int app_reading(const struct app *a) {
    return a->rd != NULL;
}

void app_read(struct app *a) {
    char line[APP_MSG];
    int i, n;
    for(i = 0; i < APP_CHUNKS && a->rd; i++) {
        n = a->io.read(a->io.ctx, a->rd_h, chunk, sizeof(chunk));
        if(n < 0) {
            snprintf(line, sizeof(line), "%s: %s", base_name(a->rd_path), strerror(errno));
            stop_read(a);
            failed(a, "Not opened", line, "");
            show(a);
            return;
        }
        if(n == 0 || sh_read(a->rd, chunk, n) < 0) {
            read_done(a);
            if(a->all) gk_redraw(a->grid);
            a->all = 0;
            show(a);
            return;
        }
        a->rd_done += n;
    }
    if(a->rd && a->rd_size > 0) {
        snprintf(line, sizeof(line), "Reading %s: %d%%", base_name(a->rd_path), percent(a->rd_done, a->rd_size));
        say(a, line);
        show(a);
    }
}

/* the sheet written to path: 0, or -1 with a message box saying why */
static int write_to(struct app *a, const char *path) {
    char why[SH_WHY], line[APP_MSG];
    int kind = sh_kind_of(path);
    long bytes = sh_write_size(a->s, kind);
    if(bytes > sh_file_max) {                   /* what no reader would open is not written (11/09/26, the review) */
        snprintf(line, sizeof(line), "%s would be %ld MB as a file,", base_name(path), (bytes + 1048575) / 1048576);
        failed(a, "Not saved", line, "more than the 32 MB a file may be: nothing written.");
        return -1;
    }
    if(a->io.save(a->io.ctx, path, a->s, kind, why, sizeof(why)) < 0) {
        snprintf(line, sizeof(line), "%s could not be saved:", base_name(path));
        failed(a, "Not saved", line, why);
        return -1;
    }
    title_path(a, path);
    sh_saved(a->s);
    if(kind == SH_NATIVE) {                     /* all of it in a file */
        a->whole = sh_state(a->s);
        a->whole_known = 1;
    }
    a->lossy = kind == SH_NATIVE || (a->whole_known && a->whole == sh_state(a->s)) ? 0 : sh_csv_loss(a->s);
    snprintf(line, sizeof(line), kind == SH_CSV ? "Saved the values in %s" : "Saved %s", base_name(path));
    say(a, line);
    return 0;
}

static void choose_failed(struct app *a) {
    say(a, errno == EACCES ? "The desktop's chooser comes only for a key or a click: try again" :
           errno == EBUSY ? "A chooser is open already" : errno == EAGAIN ? "The desktop is busy: try again" :
           "No chooser: the desktop refused it");
}

/* Save as: the chooser in the file's folder, offering its name, or with whole set, the name as a .sheet */
static void save_as(struct app *a, int whole) {
    char dir[WIN_PATH_MAX], name[WIN_PATH_MAX], *slash, *dot;
    snprintf(dir, sizeof(dir), "%s", a->path);
    slash = strrchr(dir, '/');
    if(slash && slash != dir) *slash = 0;
    else if(slash) dir[1] = 0;
    else dir[0] = 0;
    snprintf(name, sizeof(name), "%s", a->path[0] ? base_name(a->path) : "Untitled.sheet");
    if(whole && sh_kind_of(name) == SH_CSV && (dot = strrchr(name, '.')))
        snprintf(dot, sizeof(name) - (size_t)(dot - name), ".sheet");
    if(gk_choose(a->ui, CHOOSE_SAVE, WIN_DLG_SAVE, dir, name, "sheet csv") < 0) {
        choose_failed(a);
        a->after = AFTER_NONE;
    }
}

static void choose_open(struct app *a) {
    char dir[WIN_PATH_MAX], *slash;
    snprintf(dir, sizeof(dir), "%s", a->path);
    slash = strrchr(dir, '/');
    if(slash && slash != dir) *slash = 0;
    else dir[0] = 0;
    if(gk_choose(a->ui, CHOOSE_OPEN, WIN_DLG_OPEN, dir, "", "sheet csv") < 0) choose_failed(a);
}

static void do_new(struct app *a) {
    struct sheet *s = sh_new();
    if(!s) {
        say(a, "Not enough memory for a new sheet");
        return;
    }
    take_sheet(a, s);
    a->path[0] = 0;
    say(a, "A new sheet");
}

/* what waited for a save, or for the question about unsaved changes */
static void proceed(struct app *a) {
    int after = a->after;
    a->after = AFTER_NONE;
    switch(after) {
    case AFTER_NEW: do_new(a); break;
    case AFTER_OPEN: choose_open(a); break;
    case AFTER_CLOSE: a->quit = 1; break;
    }
}

/* what a CSV save leaves out, in words: "formulas, formats and column widths" */
static void lost_words(unsigned lose, char *out, int size) {
    const char *w[3];
    int n = 0;
    if(lose & (SH_LOSE_FORMULAS | SH_LOSE_LIMIT)) w[n++] = "formulas";
    if(lose & SH_LOSE_FORMATS) w[n++] = "formats";
    if(lose & SH_LOSE_WIDTHS) w[n++] = "column widths";
    if(n == 3) snprintf(out, (size_t)size, "%s, %s and %s", w[0], w[1], w[2]);
    else if(n == 2) snprintf(out, (size_t)size, "%s and %s", w[0], w[1]);
    else snprintf(out, (size_t)size, "%s", n ? w[0] : "");
}

/* a CSV save of path, which keeps the values: asked first when the sheet has more (11/09/26, the review: formats,
   widths and cells not worked out too). 1 when it asks, 0 when it may go on */
static int csv_asks(struct app *a, const char *path) {
    unsigned lose = sh_csv_loss(a->s);
    char what[64], line[APP_MSG];
    if(!lose) return 0;
    snprintf(a->save_path, sizeof(a->save_path), "%s", path);
    lost_words(lose, what, sizeof(what));
    snprintf(line, sizeof(line), "In CSV, %s are lost:", what);
    gk_confirm(a->ui, BOX_CSV, "Save the values only?", line, lose & SH_LOSE_LIMIT ?
               "cells saying #LIMIT! go as that. .sheet keeps all." : "the file holds the values only. .sheet keeps all.",
               "Save", NULL, "Cancel");
    return 1;
}

/* Save: 0 done (or nothing to do), 1 a chooser or a question asked (its answer goes on), -1 failed */
static int save(struct app *a) {
    if(!a->path[0]) {
        save_as(a, 0);
        return 1;
    }
    if(sh_kind_of(a->path) == SH_CSV && csv_asks(a, a->path)) return 1;
    return write_to(a, a->path);
}

/* New, Open and Close: 1 when it asks about unsaved changes first (the answer goes on), 0 when it can go on now */
static int unsaved(struct app *a, int after) {
    char line[APP_MSG], line2[APP_MSG], what[64];
    a->after = after;
    if(sh_modified(a->s)) {
        snprintf(line, sizeof(line), "%s has changes that are not saved.", sheet_name(a));
        gk_confirm(a->ui, BOX_SAVE, "Save the changes?", line, "", "Save", "Don't save", "Cancel");
        return 1;
    }
    if(!a->lossy) return 0;
    lost_words(a->lossy, what, sizeof(what));   /* saved, but as CSV: what it left out is in this window alone */
    snprintf(line, sizeof(line), "%s keeps only the values:", sheet_name(a));
    snprintf(line2, sizeof(line2), "the %s are here only.", what);
    gk_confirm(a->ui, BOX_SAVE, "Save the whole sheet first?", line, line2, "Save as", "Don't save", "Cancel");
    return 1;
}

static void close_asked(struct app *a) {
    if(gk_box_open(a->ui) && gk_asking(a->ui) == BOX_SAVE && a->after == AFTER_CLOSE) return;   /* asked already */
    stop_read(a);
    if(a->editing && commit(a) < 0) cancel(a);
    if(!unsaved(a, AFTER_CLOSE)) proceed(a);
}

static void chosen(struct app *a, const struct gk_event *ev) {
    if(!ev->value) {
        a->after = AFTER_NONE;
        say(a, ev->id == CHOOSE_SAVE ? "Not saved" : "Not opened");
        return;
    }
    if(ev->id == CHOOSE_OPEN) {
        start_read(a, ev->text);
        return;
    }
    if(sh_kind_of(ev->text) == SH_CSV && csv_asks(a, ev->text)) return;
    if(write_to(a, ev->text) == 0) proceed(a);
    else a->after = AFTER_NONE;
}

static void width_answered(struct app *a, const char *text) {
    struct sh_range r;
    char *end;
    long n = strtol(text, &end, 10);
    while(*end == ' ') end++;
    if(end == text || *end || n < 1 || n > 99) {
        say(a, "A width is 1 to 99 characters");
        return;
    }
    view_selection(&a->v, &r);
    if(sh_set_width(a->s, r.c1, r.c2, (int)n * 8 + 2 * V_PAD) < 0) say(a, sh_why(a->s));
    a->all = 1;
}

static void answered(struct app *a, const struct gk_event *ev) {
    switch(ev->id) {
    case BOX_SAVE:
        if(ev->value == 0) {                    /* Save, then go on: when Save as asks, its answer goes on */
            int r = !sh_modified(a->s) && a->lossy ? (save_as(a, 1), 1) : save(a);
            if(r == 0) proceed(a);
            else if(r < 0) a->after = AFTER_NONE;
        } else if(ev->value == 1) proceed(a);   /* Don't save */
        else if(!gk_box_open(a->ui)) {          /* Cancel (not a box put in its place, which asks on) */
            a->after = AFTER_NONE;
            say(a, "Nothing changed");
        }
        break;
    case BOX_CSV:
        if(ev->value == 0) {
            if(write_to(a, a->save_path) == 0) proceed(a);
            else a->after = AFTER_NONE;
        } else if(!gk_box_open(a->ui)) {        /* not a box put in its place, which asks on (the review's L8) */
            a->after = AFTER_NONE;
            say(a, "Not saved");
        }
        break;
    case BOX_WIDTH:
        if(ev->value == 0) width_answered(a, ev->text);
        break;
    }
}

/**** the clipboard ****/

static void copy(struct app *a, int cut) {
    struct sh_clip *c;
    struct sh_range r;
    char line[APP_MSG], name[40];
    const char *text;
    char *now;
    int len, n;
    view_selection(&a->v, &r);
    c = sh_copy(a->s, r);
    if(!c) {
        say(a, sh_why(a->s));
        return;
    }
    if(cut) sh_clip_cut(c);                     /* pasted, the cells move */
    sh_clip_free(a->clip);
    a->clip = c;
    a->clip_known = 0;
    text = sh_clip_text(c, &len);
    range_name(r, name, sizeof(name));
    if(text && len > 0 && gk_copy(a->ui, text, (unsigned)len) == 0)
        snprintf(line, sizeof(line), "%s %s", cut ? "Cut" : "Copied", name);
    else {                                      /* kept here only: a paste of the clipboard as it is now is this copy */
        now = gk_paste_all(a->ui, &n);
        if(now) {
            a->clip_seen = hash(now, n);
            a->clip_known = 1;
            free(now);
        }
        snprintf(line, sizeof(line), "%s %s%s", cut ? "Cut" : "Copied", name,
                 text && len > 0 ? " (here only: the desktop's clipboard refused it)" : !text ?
                 " (here only: too much for the desktop's clipboard)" : "");
    }
    say(a, line);
    if(cut) {
        if(sh_clear(a->s, r) < 0) say(a, sh_why(a->s));
        a->all = 1;
    }
}

static void paste(struct app *a) {
    struct sh_range where;
    const char *own = NULL;
    char *text, *nl, name[40], line[APP_MSG];
    int len = 0, olen = 0, r, use_clip;
    text = gk_paste_all(a->ui, &len);
    if(a->editing == E_FIELD) {                 /* kit's field pastes at its cursor, by its Ctrl+V: the menu's is off */
        free(text);
        say(a, "Ctrl+V pastes into the formula bar, at the cursor");
        return;
    }
    if(a->editing == E_TYPING) {                /* into what is typed: the clipboard's first line, as the field takes */
        if(text) {                              /* it: control characters (a NUL, a tab) as spaces */
            int i;
            if((nl = memchr(text, '\n', (size_t)len))) len = (int)(nl - text);
            if(len && text[len - 1] == '\r') len--;
            for(i = 0; i < len; i++)
                if((unsigned char)text[i] < 32) text[i] = ' ';
            type_add(a, text, len);
            free(text);
        }
        return;
    }
    if(a->clip) own = sh_clip_text(a->clip, &olen);
    use_clip = a->clip && (!text || !len || (own && olen == len && !memcmp(own, text, (size_t)len)) ||
                           (!own && a->clip_known && hash(text, len) == a->clip_seen));
    if(use_clip) r = sh_paste(a->s, a->clip, a->v.row, a->v.col, &where);
    else if(text && len) r = sh_paste_text(a->s, text, len, a->v.row, a->v.col, &where);
    else {
        free(text);
        say(a, "Nothing to paste");
        return;
    }
    free(text);
    if(r < 0) {
        say(a, sh_why(a->s));
        return;
    }
    view_select(&a->v, where);
    view_show(&a->v, where.r1, where.c1);
    range_name(where, name, sizeof(name));
    snprintf(line, sizeof(line), "Pasted into %s", name);
    say(a, line);
    a->all = 1;
}

/**** the menus ****/

static void set_format(struct app *a, int fmt) {
    struct sh_range r;
    view_selection(&a->v, &r);
    if(sh_set_format(a->s, r, fmt) < 0) say(a, sh_why(a->s));
    a->all = 1;
}

static void decimals(struct app *a, int more) {
    int fmt = sh_format(a->s, a->v.row, a->v.col), kind = SH_FMT_KIND(fmt), d = SH_FMT_DECIMALS(fmt);
    if(kind == SH_GENERAL) {
        kind = SH_FIXED(0);
        d = 2;
    }
    d += more ? 1 : -1;
    if(d < 0 || d > SH_DECIMALS_MAX) {
        say(a, more ? "Ten decimals at most" : "No decimals left");
        return;
    }
    set_format(a, kind | d);
}

static void rows_cols(struct app *a, int id) {
    struct sh_range r;
    int k;
    view_selection(&a->v, &r);
    switch(id) {
    case MENU_INS_ROWS: k = sh_insert_rows(a->s, r.r1, r.r2 - r.r1 + 1); break;
    case MENU_INS_COLS: k = sh_insert_cols(a->s, r.c1, r.c2 - r.c1 + 1); break;
    case MENU_DEL_ROWS: k = sh_delete_rows(a->s, r.r1, r.r2 - r.r1 + 1); break;
    default: k = sh_delete_cols(a->s, r.c1, r.c2 - r.c1 + 1); break;
    }
    if(k < 0) say(a, sh_why(a->s));
    a->all = 1;
}

static void undo_redo(struct app *a, int redo) {
    struct sh_range where;
    char line[APP_MSG];
    const char *label = redo ? sh_redo_label(a->s) : sh_undo_label(a->s);
    if((redo ? sh_redo(a->s, &where) : sh_undo(a->s, &where)) < 0) {
        say(a, sh_why(a->s));
        return;
    }
    view_select(&a->v, where);
    view_show(&a->v, where.r1, where.c1);
    snprintf(line, sizeof(line), "%s: %s", redo ? "Redone" : "Undone", label);
    say(a, line);
    a->all = 1;
}

static void menu(struct app *a, int id) {
    struct sh_range r;
    char cols[8];
    if(id == MENU_UNDO && a->editing) {         /* Undo while typing: the cell as it was */
        cancel(a);
        say(a, "The cell is as it was");
        return;
    }
    if(id == MENU_PASTE) {
        paste(a);
        return;
    }
    if(id == MENU_DELETE && a->editing) return;     /* Delete is the text's while it is edited */
    if(a->editing && id != MENU_EDIT && commit(a) < 0) return;
    switch(id) {
    case MENU_NEW: if(!unsaved(a, AFTER_NEW)) proceed(a); break;
    case MENU_OPEN: if(!unsaved(a, AFTER_OPEN)) proceed(a); break;
    case MENU_SAVE: a->after = AFTER_NONE; save(a); break;
    case MENU_SAVE_AS: a->after = AFTER_NONE; save_as(a, 0); break;
    case MENU_CLOSE: close_asked(a); break;
    case MENU_UNDO: undo_redo(a, 0); break;
    case MENU_REDO: undo_redo(a, 1); break;
    case MENU_CUT: copy(a, 1); break;
    case MENU_COPY: copy(a, 0); break;
    case MENU_DELETE:
        view_selection(&a->v, &r);
        if(sh_clear(a->s, r) < 0) say(a, sh_why(a->s));
        a->all = 1;
        break;
    case MENU_SELECT_ALL: view_select_all(&a->v); break;
    case MENU_GOTO:
        gk_focus(a->name);
        gk_field_select(a->name, 0, -1);
        say(a, "Type a cell's name or a range, then Enter");
        break;
    case MENU_EDIT: edit_in_bar(a); break;
    case MENU_INS_ROWS: case MENU_INS_COLS: case MENU_DEL_ROWS: case MENU_DEL_COLS: rows_cols(a, id); break;
    case MENU_WIDTH:
        snprintf(cols, sizeof(cols), "%d", (sh_width(a->s, a->v.col) - 2 * V_PAD) / 8);
        gk_ask(a->ui, BOX_WIDTH, "Column width", "Characters wide, 1 to 99:", cols, 0);
        break;
    case MENU_WIDTH_DEFAULT:
        view_selection(&a->v, &r);
        sh_set_width(a->s, r.c1, r.c2, SH_WIDTH_DEFAULT);
        a->all = 1;
        break;
    case MENU_GENERAL: set_format(a, SH_GENERAL); break;
    case MENU_FIXED: {
        int fmt = sh_format(a->s, a->v.row, a->v.col);
        set_format(a, SH_FIXED(SH_FMT_KIND(fmt) == SH_GENERAL ? 2 : SH_FMT_DECIMALS(fmt)));
        break;
    }
    case MENU_PERCENT: {
        int fmt = sh_format(a->s, a->v.row, a->v.col);
        set_format(a, SH_PERCENT(SH_FMT_KIND(fmt) == SH_GENERAL ? 0 : SH_FMT_DECIMALS(fmt)));
        break;
    }
    case MENU_MORE: decimals(a, 1); break;
    case MENU_FEWER: decimals(a, 0); break;
    case MENU_KEYS:
        gk_message(a->ui, BOX_KEYS, "Spreadsheet keys", "Type into a cell; = begins a formula. Enter and Tab",
                   "move on, Esc cancels, Ctrl+E edits, Ctrl+L goes to.");
        break;
    case MENU_ABOUT:
        gk_message(a->ui, BOX_ABOUT, "About Spreadsheet", "wsheet: 256 columns, 65,536 rows, formulas, CSV.",
                   "Its page in the manual: man wsheet");
        break;
    }
}

static void menus(struct app *a) {
    struct gk *ui = a->ui;
    int m = gk_menu(ui, "File");
    gk_menu_item(ui, m, MENU_NEW, "New", GK_CTRL | 'N', 0);
    gk_menu_item(ui, m, MENU_OPEN, "Open\xe2\x88\x99", GK_CTRL | 'O', 0);
    gk_menu_item(ui, m, MENU_SAVE, "Save", GK_CTRL | 'S', 0);
    gk_menu_item(ui, m, MENU_SAVE_AS, "Save as\xe2\x88\x99", GK_CTRL | GK_SHIFT | 'S', 0);
    gk_menu_line(ui, m);
    gk_menu_item(ui, m, MENU_CLOSE, "Close", GK_CTRL | 'W', 0);
    m = gk_menu(ui, "Edit");
    gk_menu_item(ui, m, MENU_UNDO, "Undo", GK_CTRL | 'Z', GK_ITEM_OFF);
    gk_menu_item(ui, m, MENU_REDO, "Redo", GK_CTRL | 'Y', GK_ITEM_OFF);
    gk_menu_line(ui, m);
    gk_menu_item(ui, m, MENU_CUT, "Cut", GK_CTRL | 'X', 0);
    gk_menu_item(ui, m, MENU_COPY, "Copy", GK_CTRL | 'C', 0);
    gk_menu_item(ui, m, MENU_PASTE, "Paste", GK_CTRL | 'V', 0);
    gk_menu_item(ui, m, MENU_DELETE, "Delete", WIN_KEY_DELETE, 0);
    gk_menu_line(ui, m);
    gk_menu_item(ui, m, MENU_SELECT_ALL, "Select all", GK_CTRL | 'A', 0);
    gk_menu_item(ui, m, MENU_GOTO, "Go to a cell\xe2\x88\x99", GK_CTRL | 'L', 0);
    gk_menu_item(ui, m, MENU_EDIT, "Edit the cell", GK_CTRL | 'E', 0);
    m = gk_menu(ui, "Sheet");
    gk_menu_item(ui, m, MENU_INS_ROWS, "Insert rows", GK_CTRL | '+', 0);
    gk_menu_item(ui, m, MENU_INS_COLS, "Insert columns", 0, 0);
    gk_menu_item(ui, m, MENU_DEL_ROWS, "Delete rows", GK_CTRL | '-', 0);
    gk_menu_item(ui, m, MENU_DEL_COLS, "Delete columns", 0, 0);
    gk_menu_line(ui, m);
    gk_menu_item(ui, m, MENU_WIDTH, "Column width\xe2\x88\x99", 0, 0);
    gk_menu_item(ui, m, MENU_WIDTH_DEFAULT, "Default width", 0, 0);
    m = gk_menu(ui, "Format");
    gk_menu_item(ui, m, MENU_GENERAL, "General", GK_CTRL | '~', GK_ITEM_RADIO | GK_ITEM_ON);
    gk_menu_item(ui, m, MENU_FIXED, "Fixed decimals", GK_CTRL | '!', GK_ITEM_RADIO);
    gk_menu_item(ui, m, MENU_PERCENT, "Percent", GK_CTRL | '%', GK_ITEM_RADIO);
    gk_menu_line(ui, m);
    gk_menu_item(ui, m, MENU_MORE, "More decimals", 0, 0);
    gk_menu_item(ui, m, MENU_FEWER, "Fewer decimals", 0, 0);
    m = gk_menu(ui, "Help");
    gk_menu_item(ui, m, MENU_KEYS, "Spreadsheet keys", 0, 0);
    gk_menu_item(ui, m, MENU_ABOUT, "About Spreadsheet", 0, 0);
}

/**** the window ****/

static void name_entered(struct app *a) {
    struct sh_range r;
    char line[APP_MSG];
    if(sh_parse_range(gk_text(a->name), &r) < 0) {
        snprintf(line, sizeof(line), "No such cell or range: %.60s", gk_text(a->name));
        say(a, line);
        return;
    }
    view_select(&a->v, r);
    view_show(&a->v, r.r1, r.c1);
    gk_focus(a->grid);
}

static void resized(struct app *a) {
    struct gk_view gv;
    gk_area_view(a->grid, &gv);
    view_place(&a->v, 0, 0, gv.w, gv.h);
    a->all = 1;
}

/* a key no widget took: from the formula bar (Esc, Up, Down) or the cell's name (Esc) */
static void window_key(struct app *a, unsigned key, unsigned mods) {
    if(gk_focused(a->ui) == a->formula) {
        if(key == 27) {
            cancel(a);
            gk_focus(a->grid);
        } else if((key == WIN_KEY_UP || key == WIN_KEY_DOWN) && (!a->editing || !commit(a))) {
            view_key(&a->v, (int)key, (mods & WIN_MOD_SHIFT) ? VM_SHIFT : 0);
            gk_focus(a->grid);
        }
    } else if(gk_focused(a->ui) == a->name && key == 27) gk_focus(a->grid);
}

int app_event(struct app *a, const struct gk_event *ev) {
    struct snap before;
    snapshot(a, &before);
    if(ev->type == GK_EV_KEY || ev->type == GK_EV_MENU || ev->type == GK_EV_CHANGE ||
       (ev->type == GK_EV_POINTER && ev->value)) a->msg[0] = 0;     /* a new action: the last message goes */
    if(a->rd && ev->type != GK_EV_CLOSE && ev->type != GK_EV_SCROLL && ev->type != GK_EV_RESIZE &&
       ev->type != GK_EV_THEME) {               /* while a file is read: Esc stops it, and nothing else changes */
        if(ev->type == GK_EV_KEY && ev->value == 27) {
            stop_read(a);
            say(a, "Not opened");
        } else if(ev->type == GK_EV_KEY || ev->type == GK_EV_MENU || (ev->type == GK_EV_POINTER && ev->value))
            say(a, "A file is being read: Esc stops it");
        show(a);
        return 1;
    }
    switch(ev->type) {
    case GK_EV_CLOSE: close_asked(a); break;
    case GK_EV_KEY:
        if(ev->w == a->grid) grid_key(a, (unsigned)ev->value, ev->mods);
        else window_key(a, (unsigned)ev->value, ev->mods);
        break;
    case GK_EV_POINTER:
        if(ev->w == a->grid) grid_pointer(a, ev);
        break;
    case GK_EV_SCROLL:
        if(ev->w == a->grid) {
            a->v.left = ev->x;
            a->v.top = ev->y;
            a->all = 1;
        }
        break;
    case GK_EV_FOCUS:                           /* the grid has the keys: an edit in the formula bar goes in */
        if(ev->w == a->grid && ev->value && a->editing == E_FIELD) commit(a);
        else if(ev->w == a->grid && !ev->value && !a->editing && gk_focused(a->ui) == a->formula && bar_refuses(a))
            gk_focus(a->grid);                  /* a click in the bar, for a cell it cannot edit */
        break;
    case GK_EV_CHANGE:
        if(ev->id == ID_FORMULA && a->editing != E_FIELD) {
            if(a->editing != E_TYPING && bar_refuses(a)) {
                gk_focus(a->grid);              /* and show() puts the cell back in the bar */
                break;
            }
            if(a->editing != E_TYPING) {
                a->edit_row = a->v.row;
                a->edit_col = a->v.col;
            }
            a->editing = E_FIELD;
        }
        break;
    case GK_EV_ACTIVATE:
        if(ev->id == ID_NAME) name_entered(a);
        else if(ev->id == ID_FORMULA && (!a->editing || !commit(a))) {
            view_enter(&a->v, 0);
            gk_focus(a->grid);
        }
        break;
    case GK_EV_MENU: menu(a, ev->id); break;
    case GK_EV_CHOSEN: chosen(a, ev); break;
    case GK_EV_ANSWER: answered(a, ev); break;
    case GK_EV_RESIZE: resized(a); break;
    case GK_EV_THEME: a->all = 1; break;
    }
    damage(a, &before);
    a->all = 0;
    if(before.top != a->v.top || before.left != a->v.left) gk_scroll_to(a->grid, a->v.left, a->v.top);
    show(a);
    return !a->quit;
}

int app_open(struct app *a, const struct sheet_io *io, const char *path) {
    struct gk_widget *root, *bar;
    memset(a, 0, sizeof(*a));
    a->io = *io;
    a->rd_h = -1;
    a->s = sh_new();
    if(!a->s) {
        errno = ENOMEM;
        return -1;
    }
    view_init(&a->v, a->s);
    a->ui = gk_open_limits("Spreadsheet", APP_W, APP_H, APP_MIN_W, APP_MIN_H, 0, 0, WIN_RESIZABLE);
    if(!a->ui) {
        sh_free(a->s);
        a->s = NULL;
        return -1;
    }
    root = gk_root(a->ui);
    bar = gk_hbox(root, 6);
    gk_pad(bar, 4);
    a->name = gk_field(bar, ID_NAME, 24, 0);
    gk_size(a->name, 96, 0);
    gk_grow(a->name, 0, 0);                     /* its own width; the formula takes the rest */
    gk_field_hint(a->name, "Cell");
    a->formula = gk_field(bar, ID_FORMULA, SH_INPUT_MAX + 1, 0);   /* a text's ' and SH_INPUT_MAX bytes */
    gk_grow(a->formula, 1, 0);
    gk_field_hint(a->formula, "A number, text, or = and a formula");
    a->grid = gk_area(root, ID_GRID, GK_FOCUSABLE | GK_TAKES_TAB, draw, a);
    gk_area_extent(a->grid, SH_COLS, SH_WIDTH_DEFAULT, SH_ROWS + 1, V_ROW_H);   /* the headers' row, the +1 */
    a->status = gk_status(root);
    menus(a);
    gk_focus(a->grid);
    if(!gk_ok(a->ui)) {
        app_close(a);
        errno = ENOMEM;
        return -1;
    }
    gk_flush(a->ui);                            /* laid out: the grid's size */
    resized(a);
    a->all = 0;
    show(a);
    if(path && *path) start_read(a, path);
    return 0;
}

void app_close(struct app *a) {
    stop_read(a);
    sh_clip_free(a->clip);
    a->clip = NULL;
    gk_close(a->ui);
    a->ui = NULL;
    sh_free(a->s);
    a->s = NULL;
}
