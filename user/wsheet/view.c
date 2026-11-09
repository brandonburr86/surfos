/*
SurfOS user program: wsheet
--------------------
File: view.c    Date: 11/05/26
--------------------
The grid as the window shows it (view.h): its geometry, the active cell and the selection, and
what the keys, the pointer and the wheel do to them, as spreadsheets do it:
- the arrows move the active cell, and with Shift the selection's other corner; with Ctrl they go
  to the edge of the data: along filled cells to the last of them, or across empty ones to the
  next filled one, or to the sheet's edge;
- Page Up and Page Down move a screenful, the view with them; Home goes to column A (Ctrl+Home to
  A1), End to the row's last filled cell (Ctrl+End to the last row and column that hold anything);
- Enter moves down (Shift+Enter up), Tab right (Shift+Tab left), and Enter after Tabs comes back
  to the column the Tabs began in, so a table is typed a row at a time;
- a press selects a cell, a header a whole row or column (the corner everything), and a drag or
  Shift extends it; a press on a column header's right edge drags the column's width, which is
  set once, when it is let go (the selected whole columns all take it);
- the wheel scrolls rows, with Shift columns.
Nothing here draws or makes a system call.
*/

#include <stdio.h>
#include <string.h>
#include "view.h"
#include "sheetpriv.h"

static int clamp(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

void view_init(struct view *v, struct sheet *s) {
    memset(v, 0, sizeof(*v));
    v->s = s;
    v->tab_col = -1;
    v->w = V_ROWHEAD_W + SH_WIDTH_DEFAULT;
    v->h = V_HEAD_H + V_ROW_H;
}

int view_width(const struct view *v, int col) {
    if(v->drag == VD_WIDTH && col == v->drag_col) return v->width_now;
    return sh_width(v->s, col);
}

int view_rows_shown(const struct view *v) {
    int n = (v->h - V_HEAD_H) / V_ROW_H;
    return n < 1 ? 1 : n;
}

int view_cols_shown(const struct view *v) {
    int c, n = 0, room = v->w - V_ROWHEAD_W;
    for(c = v->left; c < SH_COLS; c++) {
        room -= view_width(v, c);
        if(room < 0) break;
        n++;
    }
    return n < 1 ? 1 : n;
}

void view_place(struct view *v, int x, int y, int w, int h) {
    v->x = x;
    v->y = y;
    v->w = w;
    v->h = h;
    view_show(v, v->row, v->col);
}

int view_col_x(const struct view *v, int col) {
    int c, x = v->x + V_ROWHEAD_W;
    if(col < v->left || col >= SH_COLS) return -1;
    for(c = v->left; c < col; c++) {
        x += view_width(v, c);
        if(x >= v->x + v->w) return -1;
    }
    return x;
}

int view_row_y(const struct view *v, int row) {
    int y;
    if(row < v->top || row >= SH_ROWS) return -1;
    if(row - v->top > v->h / V_ROW_H) return -1;
    y = v->y + V_HEAD_H + (row - v->top) * V_ROW_H;
    return y < v->y + v->h ? y : -1;
}

int view_chars(const struct view *v, int col) {
    int n = (view_width(v, col) - 2 * V_PAD) / 8;
    return n < 1 ? 1 : n;
}

/* the column under x, -1 when there is none; *edge: x is within V_BORDER of its right edge */
static int col_at(const struct view *v, int x, int *edge) {
    int c, cx = v->x + V_ROWHEAD_W, w;
    *edge = 0;
    if(x < cx) return -1;
    for(c = v->left; c < SH_COLS && cx < v->x + v->w; c++) {
        w = view_width(v, c);
        if(x < cx + w) {
            *edge = x >= cx + w - V_BORDER;
            return c;
        }
        cx += w;
    }
    return -1;
}

void view_at(const struct view *v, int x, int y, struct view_at *a) {
    int edge, c;
    a->what = VA_NONE;
    a->row = a->col = -1;
    if(x < v->x || y < v->y || x >= v->x + v->w || y >= v->y + v->h) return;
    if(y < v->y + V_HEAD_H) {
        if(x < v->x + V_ROWHEAD_W) {
            a->what = VA_CORNER;
            return;
        }
        c = col_at(v, x, &edge);
        if(c < 0) return;
        if(!edge && c > v->left && x < view_col_x(v, c) + V_BORDER) {     /* the left edge is the one before */
            c--;
            edge = 1;
        }
        a->what = edge ? VA_EDGE : VA_COLHEAD;
        a->col = c;
        return;
    }
    a->row = v->top + (y - v->y - V_HEAD_H) / V_ROW_H;
    if(a->row >= SH_ROWS) {
        a->row = -1;
        return;
    }
    if(x < v->x + V_ROWHEAD_W) {
        a->what = VA_ROWHEAD;
        return;
    }
    a->col = col_at(v, x, &edge);
    if(a->col >= 0) a->what = VA_CELL;
}

void view_selection(const struct view *v, struct sh_range *r) {
    r->r1 = v->row < v->erow ? v->row : v->erow;
    r->r2 = v->row < v->erow ? v->erow : v->row;
    r->c1 = v->col < v->ecol ? v->col : v->ecol;
    r->c2 = v->col < v->ecol ? v->ecol : v->col;
}

int view_selected(const struct view *v, int row, int col) {
    struct sh_range r;
    view_selection(v, &r);
    return row >= r.r1 && row <= r.r2 && col >= r.c1 && col <= r.c2;
}

static int empty(const struct view *v, int row, int col) {
    struct sh_value x;
    sh_value(v->s, row, col, &x);
    return x.type == SH_EMPTY;
}

int view_text_end(const struct view *v, int row, int col) {
    int x = view_col_x(v, col), end = v->x + v->w, c;
    if(x < 0) return -1;
    x += view_width(v, col);
    for(c = col + 1; c < SH_COLS && x < end && empty(v, row, c); c++) x += view_width(v, c);
    return x < end ? x : end;
}

/**** moving ****/

int view_scroll(struct view *v, int rows, int cols) {
    int top = clamp(v->top + rows, 0, SH_ROWS - view_rows_shown(v)), left = clamp(v->left + cols, 0, SH_COLS - 1);
    if(top == v->top && left == v->left) return 0;
    v->top = top;
    v->left = left;
    return 1;
}

int view_show(struct view *v, int row, int col) {
    int top = v->top, left = v->left, rows = view_rows_shown(v), x, end = v->x + v->w;
    if(row < v->top) v->top = row;
    else if(row >= v->top + rows) v->top = row - rows + 1;
    if(col < v->left) v->left = col;
    else
        while(v->left < col) {                  /* until the column shows whole, or is the first shown */
            x = view_col_x(v, col);
            if(x >= 0 && x + view_width(v, col) <= end) break;
            v->left++;
        }
    return top != v->top || left != v->left;
}

int view_go(struct view *v, int row, int col, int extend) {
    int was[4] = { v->row, v->col, v->erow, v->ecol }, moved;
    row = clamp(row, 0, SH_ROWS - 1);
    col = clamp(col, 0, SH_COLS - 1);
    v->erow = row;
    v->ecol = col;
    if(!extend) {
        v->row = row;
        v->col = col;
    }
    v->tab_col = -1;
    moved = view_show(v, row, col);
    return moved || was[0] != v->row || was[1] != v->col || was[2] != v->erow || was[3] != v->ecol;
}

void view_select(struct view *v, struct sh_range r) {
    v->row = r.r1;
    v->col = r.c1;
    v->erow = r.r2;
    v->ecol = r.c2;
    v->tab_col = -1;
}

void view_select_all(struct view *v) {
    struct sh_range all = { 0, 0, SH_ROWS - 1, SH_COLS - 1 };
    view_select(v, all);
}

/* Ctrl with an arrow: from row, col by dr, dc to the edge of the data */
static void data_edge(const struct view *v, int *row, int *col, int dr, int dc) {
    int r = *row, c = *col;
    if(r + dr < 0 || r + dr >= SH_ROWS || c + dc < 0 || c + dc >= SH_COLS) return;
    if(!empty(v, r, c) && !empty(v, r + dr, c + dc)) {         /* along the filled cells to their last */
        while(r + dr >= 0 && r + dr < SH_ROWS && c + dc >= 0 && c + dc < SH_COLS && !empty(v, r + dr, c + dc)) {
            r += dr;
            c += dc;
        }
    } else {                                    /* across the empty ones to the next filled, or the sheet's edge */
        do {
            r += dr;
            c += dc;
        } while(r + dr >= 0 && r + dr < SH_ROWS && c + dc >= 0 && c + dc < SH_COLS && empty(v, r, c));
    }
    *row = r;
    *col = c;
}

/* the last column of row that holds something, or col */
static int row_end(const struct view *v, int row, int col) {
    int rows, cols, c;
    sh_extent(v->s, &rows, &cols);
    for(c = cols - 1; c >= 0; c--) if(!empty(v, row, c)) return c;
    return col;
}

int view_key(struct view *v, int key, int mods) {
    int extend = (mods & VM_SHIFT) != 0, ctrl = (mods & VM_CTRL) != 0;
    int row = extend ? v->erow : v->row, col = extend ? v->ecol : v->col, n = view_rows_shown(v), rows, cols;
    switch(key) {
    case V_UP: case V_DOWN: case V_LEFT: case V_RIGHT: {
        int dr = key == V_UP ? -1 : key == V_DOWN ? 1 : 0, dc = key == V_LEFT ? -1 : key == V_RIGHT ? 1 : 0;
        if(ctrl) data_edge(v, &row, &col, dr, dc);
        else {
            row += dr;
            col += dc;
        }
        break;
    }
    case V_PGUP: case V_PGDN:                   /* a screenful, and the view with it */
        row += key == V_PGUP ? -n : n;
        view_scroll(v, key == V_PGUP ? -n : n, 0);
        break;
    case V_HOME:
        col = 0;
        if(ctrl) row = 0;
        break;
    case V_END:
        if(ctrl) {
            sh_extent(v->s, &rows, &cols);
            row = rows ? rows - 1 : 0;
            col = cols ? cols - 1 : 0;
        } else col = row_end(v, row, col);
        break;
    default:
        return 0;
    }
    view_go(v, row, col, extend);
    return 1;
}

int view_enter(struct view *v, int back) {
    int col = v->tab_col >= 0 ? v->tab_col : v->col;
    view_go(v, v->row + (back ? -1 : 1), col, 0);
    return 1;
}

int view_tab(struct view *v, int back) {
    int from = v->tab_col >= 0 ? v->tab_col : v->col;
    view_go(v, v->row, v->col + (back ? -1 : 1), 0);
    v->tab_col = from;
    return 1;
}

/**** the pointer ****/

int view_press(struct view *v, int x, int y, int mods) {
    struct view_at a;
    int extend = (mods & VM_SHIFT) != 0;
    view_at(v, x, y, &a);
    switch(a.what) {
    case VA_CELL:
        if(extend) {
            v->erow = a.row;
            v->ecol = a.col;
        } else {
            v->row = v->erow = a.row;
            v->col = v->ecol = a.col;
        }
        v->tab_col = -1;
        v->drag = VD_CELLS;
        view_show(v, a.row, a.col);
        return 1;
    case VA_COLHEAD:
        if(!extend) v->col = a.col;
        v->row = 0;
        v->erow = SH_ROWS - 1;
        v->ecol = a.col;
        v->tab_col = -1;
        v->drag = VD_COLS;
        return 1;
    case VA_ROWHEAD:
        if(!extend) v->row = a.row;
        v->col = 0;
        v->ecol = SH_COLS - 1;
        v->erow = a.row;
        v->tab_col = -1;
        v->drag = VD_ROWS;
        return 1;
    case VA_CORNER:
        view_select_all(v);
        return 1;
    case VA_EDGE:
        v->drag = VD_WIDTH;
        v->drag_col = a.col;
        v->drag_x = x;
        v->drag_w = v->width_now = sh_width(v->s, a.col);
        return 1;
    }
    return 0;
}

int view_move(struct view *v, int x, int y) {
    struct view_at a;
    int was[4] = { v->erow, v->ecol, v->top, v->left }, w, edge, bottom = v->y + v->h, right = v->x + v->w;
    switch(v->drag) {
    case VD_WIDTH:
        w = clamp(v->drag_w + x - v->drag_x, SH_WIDTH_MIN, SH_WIDTH_MAX);
        if(w == v->width_now) return 0;
        v->width_now = w;
        return 1;
    case VD_CELLS: case VD_ROWS: case VD_COLS:
        /* past an edge of the grid: a row or a column further each move, the view following */
        if(v->drag != VD_COLS) {
            if(y < v->y + V_HEAD_H) {
                view_scroll(v, -1, 0);
                v->erow = v->top;
            } else if(y >= bottom) {
                v->erow = clamp(v->top + view_rows_shown(v), 0, SH_ROWS - 1);
                view_show(v, v->erow, v->ecol);
            } else {
                view_at(v, x < v->x + V_ROWHEAD_W ? v->x + V_ROWHEAD_W : x >= right ? right - 1 : x, y, &a);
                if(a.row >= 0) v->erow = a.row;
            }
        }
        if(v->drag != VD_ROWS) {
            if(x < v->x + V_ROWHEAD_W) {
                view_scroll(v, 0, -1);
                v->ecol = v->left;
            } else if(x >= right) {
                v->ecol = clamp(v->left + view_cols_shown(v), 0, SH_COLS - 1);
                view_show(v, v->drag == VD_COLS ? v->top : v->erow, v->ecol);
            } else {
                int c = col_at(v, x, &edge);
                if(c >= 0) v->ecol = c;
            }
        }
        break;
    default:
        return 0;
    }
    return was[0] != v->erow || was[1] != v->ecol || was[2] != v->top || was[3] != v->left;
}

int view_release(struct view *v) {
    struct sh_range r;
    int c1, c2, drag = v->drag;
    v->drag = VD_NONE;
    if(drag != VD_WIDTH) return drag != VD_NONE;
    if(v->width_now == v->drag_w) return 1;
    view_selection(v, &r);
    c1 = c2 = v->drag_col;
    if(r.r1 == 0 && r.r2 == SH_ROWS - 1 && v->drag_col >= r.c1 && v->drag_col <= r.c2) {   /* whole columns: all */
        c1 = r.c1;
        c2 = r.c2;
    }
    sh_set_width(v->s, c1, c2, v->width_now);
    return 1;
}

int view_wheel(struct view *v, int rows, int mods) {
    if(mods & VM_SHIFT) return view_scroll(v, 0, rows > 0 ? (rows + 2) / 3 : -((2 - rows) / 3));
    return view_scroll(v, rows, 0);
}

/**** the status line ****/

void view_status(const struct view *v, char *out, int size) {
    struct sh_range r;
    char sum[40], avg[40];
    double total;
    int numbers, values;
    out[0] = 0;
    if(sh_problem(v->s, v->row, v->col, out, size)) return;
    view_selection(v, &r);
    if(r.r1 == r.r2 && r.c1 == r.c2) return;
    sh_stats(v->s, r, &total, &numbers, &values);
    if(numbers) {
        num_general(total, 0, sum, sizeof(sum));
        num_general(total / numbers, 0, avg, sizeof(avg));
        snprintf(out, (size_t)size, "Sum %s  Average %s  Count %d", sum, avg, values);
    } else if(values) snprintf(out, (size_t)size, "Count %d", values);
}
