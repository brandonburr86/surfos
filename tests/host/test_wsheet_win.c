/* Host tests for the Spreadsheet's window (user/wsheet/app.c, on view.c and the model; 11/05/26, the desktop
   applications, track sheet), on the fake desktop (tests/host/gk_fake.c) with files in memory in place of io.c's system
   calls, driven as a person drives it: the window and its menus in the desktop's bar; the grid drawn, headers lit for
   the active cell; typing into cells, Enter and Tab moving on and Enter coming back to Tab's column, an arrow putting
   the text in, Esc and Backspace; the status bar's sum of a selection and a formula's trouble; the formula bar (Ctrl+E,
   a double-click, Enter, Esc, leaving it); the cell's name going to a cell or a range; Copy and Paste of cells (their
   formulas moved) and of text from elsewhere, Cut; Undo and Redo; the formats; rows inserted; a column's width by its
   header's edge and by the Column width box; the wheel; keys scrolling the bars along; a move presenting only what
   changed; Save (the desktop's chooser, then without it), a save that fails, Save as CSV asking first about formulas;
   Open asking first about unsaved changes, a CSV read, a file that is not there or not a sheet, a file named on the
   command line, a big one read a piece at a time and stopped by Esc; the theme; a new size; closing, asking first.
   Built and run by `make unittest`; natively with the sanitizers as the account says. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <gk.h>
#include "gk_fake.h"
#include "app.h"

static int checks, failed;
#define CHECK(x) do { checks++; if(!(x)) { failed++; printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

static struct app A;

/**** files in memory ****/

#define FILES 16
static struct mfile {
    char path[WIN_PATH_MAX];
    char *data;
    int len;
} files[FILES];
static struct {
    int file, pos, open;
} handles[4];
static int save_fails, opens, reads;

static struct mfile *find(const char *path, int make) {
    int i;
    for(i = 0; i < FILES; i++) if(files[i].path[0] && !strcmp(files[i].path, path)) return &files[i];
    if(!make) return NULL;
    for(i = 0; i < FILES; i++)
        if(!files[i].path[0]) {
            snprintf(files[i].path, sizeof(files[i].path), "%s", path);
            return &files[i];
        }
    return NULL;
}

static void put_file(const char *path, const char *text, int len) {
    struct mfile *f = find(path, 1);
    free(f->data);
    f->data = malloc((size_t)len + 1);
    memcpy(f->data, text, (size_t)len);
    f->data[len] = 0;
    f->len = len;
}

static const char *file_text(const char *path) {
    struct mfile *f = find(path, 0);
    return f ? f->data : "";
}

static int m_open(void *ctx, const char *path, long *size) {
    struct mfile *f = find(path, 0);
    int h;
    (void)ctx;
    if(!f) {
        errno = ENOENT;
        return -1;
    }
    for(h = 0; h < 4 && handles[h].open; h++);
    if(h == 4) {
        errno = EMFILE;
        return -1;
    }
    handles[h].file = (int)(f - files);
    handles[h].pos = 0;
    handles[h].open = 1;
    *size = f->len;
    opens++;
    return h;
}

static int m_read(void *ctx, int h, char *buf, int n) {
    struct mfile *f = &files[handles[h].file];
    int k = f->len - handles[h].pos;
    (void)ctx;
    if(k > n) k = n;
    memcpy(buf, f->data + handles[h].pos, (size_t)k);
    handles[h].pos += k;
    reads++;
    return k;
}

static void m_close(void *ctx, int h) {
    (void)ctx;
    handles[h].open = 0;
}

struct mbuf {
    char *s;
    int len, cap;
};

static int m_put(void *ctx, const char *b, int n) {
    struct mbuf *m = ctx;
    if(m->len + n + 1 > m->cap) {
        m->cap = (m->len + n + 1) * 2;
        m->s = realloc(m->s, (size_t)m->cap);
    }
    memcpy(m->s + m->len, b, (size_t)n);
    m->len += n;
    return 0;
}

static int m_save(void *ctx, const char *path, const struct sheet *s, int kind, char *why, int size) {
    struct mbuf m = { NULL, 0, 0 };
    (void)ctx;
    if(save_fails) {
        save_fails = 0;
        snprintf(why, (size_t)size, "No space left on device");
        return -1;
    }
    m_put(&m, "", 0);
    sh_write(s, kind, m_put, &m);
    put_file(path, m.s, m.len);
    free(m.s);
    return 0;
}

static const struct sheet_io mem_io = { NULL, m_open, m_read, m_close, m_save };

/**** driving the window ****/

/* every event, as main.c's loop takes them, a file's pieces between: 1, or 0 once the window asked to close */
static int run(void) {
    struct gk_event ev;
    int r;
    for(;;) {
        r = gk_next(A.ui, &ev, 0);
        if(r < 0) return -1;
        if(r > 0) {
            if(!app_event(&A, &ev)) return 0;
            continue;
        }
        if(!app_reading(&A)) return 1;
        app_read(&A);
    }
}

static void grid_origin(int *x, int *y) {
    int w, h;
    gk_bounds(A.grid, x, y, &w, &h);
}

static void cell_xy(int row, int col, int *x, int *y) {
    int gx, gy;
    grid_origin(&gx, &gy);
    *x = gx + view_col_x(&A.v, col) + view_width(&A.v, col) / 2;
    *y = gy + view_row_y(&A.v, row) + V_ROW_H / 2;
}

static void click_cell(int row, int col) {
    int x, y;
    cell_xy(row, col, &x, &y);
    gkf_advance(1000);                          /* no double-click with the last click */
    gkf_click(x, y);
}

static void at(const char *name, int *r, int *c) {
    if(sh_parse_cell(name, r, c) < 0) exit(2);
}

static double num(const char *name) {
    struct sh_value v;
    int r, c;
    at(name, &r, &c);
    sh_value(A.s, r, c, &v);
    return v.type == SH_NUMBER ? v.num : -12345.678;
}

static int vtype(const char *name) {
    struct sh_value v;
    int r, c;
    at(name, &r, &c);
    sh_value(A.s, r, c, &v);
    return v.type;
}

static const char *inp(const char *name) {
    static char b[SH_INPUT_MAX + 64];
    int r, c;
    at(name, &r, &c);
    sh_input(A.s, r, c, b, sizeof(b));
    return b;
}

static int status_is(const char *s) {
    if(!strcmp(gk_text(A.status), s)) return 1;
    printf("  the status bar says \"%s\", not \"%s\"\n", gk_text(A.status), s);
    return 0;
}

static int status_starts(const char *s) {
    if(!strncmp(gk_text(A.status), s, strlen(s))) return 1;
    printf("  the status bar says \"%s\", not \"%s...\"\n", gk_text(A.status), s);
    return 0;
}

static void ctrl(char letter, unsigned more) {
    gkf_key((unsigned)(letter - 'A' + 1), WIN_MOD_CTRL | more);
}

static int starts(const char *s, const char *with) {
    return !strncmp(s, with, strlen(with));
}

static int box_is(int id) {
    return gk_box_open(A.ui) && gk_asking(A.ui) == id;
}

static void fresh(const char *path) {
    app_close(&A);
    gkf_reset(0, 0);
    if(app_open(&A, &mem_io, path) < 0) {
        printf("no window\n");
        exit(2);
    }
    gkf_focus(1);
    run();
}

int main(void) {
    static char big[400000];
    struct gk_view gv;
    int gx, gy, x, y, w, h, n, i;
    setvbuf(stdout, NULL, _IOLBF, 0);
    gkf_reset(0, 0);
    gkf_refuse(ENODEV);
    CHECK(app_open(&A, &mem_io, NULL) < 0 && errno == ENODEV);
    CHECK(app_open(&A, &mem_io, NULL) == 0 && gkf_w() == APP_W && gkf_h() == APP_H);
    gkf_focus(1);
    CHECK(run() && !strcmp(gkf_title(), "Untitled - Spreadsheet") && gkf_resizable());
    /* the menus in the desktop's bar */
    CHECK(gkf_menu_find("File") >= 0 && gkf_menu_find("Edit") >= 0 && gkf_menu_find("Sheet") >= 0 &&
          gkf_menu_find("Format") >= 0);
    CHECK(gkf_menu_entry(gkf_menu_find("Help"))->flags == (WIN_MENU_TITLE | WIN_MENU_HELP));
    CHECK(gkf_menu_entry(gkf_menu_find("Undo"))->flags & WIN_MENU_DISABLED);
    CHECK(!strcmp(gkf_menu_entry(gkf_menu_find("Save as\xe2\x88\x99"))->keys, "Ctrl+Shift+S"));
    CHECK(!strcmp(gkf_menu_entry(gkf_menu_find("Insert rows"))->keys, "Ctrl++"));
    CHECK(gkf_menu_entry(gkf_menu_find("General"))->flags & WIN_MENU_RADIO);
    /* the grid: the headers, the active cell's lit */
    grid_origin(&gx, &gy);
    CHECK(gkf_text(gx + 84, gy + 2, "A", GW_TEAL) && gkf_text(gx + 48 + 80 + 36, gy + 2, "B", GW_INK));
    CHECK(gkf_text(gx + 36, gy + 22, "1", GW_TEAL) && gkf_text(gx + 36, gy + 42, "2", GW_INK));
    CHECK(gkf_pixel(gx + 48, gy + 20) == GW_TEAL);  /* the active cell's frame */
    /* typing: Enter and Tab move on, and Enter comes back to the column the Tabs began in */
    gkf_type("12\n");
    CHECK(run() && num("A1") == 12 && A.v.row == 1 && A.v.col == 0);
    CHECK(gkf_text(gx + 128 - 3 - 16, gy + 22, "12", GW_INK));    /* a number at the right */
    gkf_type("=A1*2\t");
    CHECK(run() && num("A2") == 24 && A.v.row == 1 && A.v.col == 1);
    gkf_type("x\t\n");
    CHECK(run() && !strcmp(inp("B2"), "x") && A.v.row == 2 && A.v.col == 0);
    CHECK(gkf_text(gx + 128 + 3, gy + 42, "x", GW_INK));           /* text at the left */
    gkf_type("abc");
    CHECK(run() && A.editing == E_TYPING && !strcmp(gk_text(A.formula), "abc") && status_starts("Typing into A3"));
    CHECK(gkf_text(gx + 48 + 3, gy + 62, "abc", GW_INK));          /* in the cell as it is typed */
    gkf_key(27, 0);
    CHECK(run() && !A.editing && vtype("A3") == SH_EMPTY && !strcmp(gk_text(A.formula), ""));
    gkf_type("7");
    gkf_key(WIN_KEY_RIGHT, 0);                  /* an arrow puts it in and moves */
    CHECK(run() && num("A3") == 7 && A.v.col == 1 && A.v.row == 2);
    gkf_type("4x");
    gkf_key(127, 0);
    gkf_key('\r', 0);
    CHECK(run() && num("B3") == 4);
    click_cell(2, 1);
    gkf_key(127, 0);                            /* Backspace empties the cell as typing begins */
    gkf_key('\r', 0);
    CHECK(run() && vtype("B3") == SH_EMPTY);
    /* the status bar: a selection's numbers; a formula's trouble */
    click_cell(0, 0);
    gkf_key(WIN_KEY_DOWN, WIN_MOD_SHIFT);
    gkf_key(WIN_KEY_DOWN, WIN_MOD_SHIFT);
    CHECK(run() && status_is("Sum 43  Average 14.3333333333333  Count 3") && !strcmp(gk_text(A.name), "A1:A3"));
    click_cell(1, 0);
    CHECK(run() && !strcmp(gk_text(A.formula), "=A1*2") && !strcmp(gk_text(A.name), "A2"));
    click_cell(3, 0);
    gkf_type("=SUM(1\n");
    CHECK(run() && status_is("A4: a ( has no ) (at character 5)"));
    click_cell(3, 0);
    CHECK(run() && status_is("a ( has no ) (at character 5)"));
    grid_origin(&gx, &gy);
    CHECK(gkf_text(gx + 48 + (80 - 56) / 2, gy + 82, "#ERROR!", GW_CORAL));
    /* the formula bar: Ctrl+E, Enter; Esc; a double-click; leaving it */
    click_cell(1, 0);
    ctrl('E', 0);
    gkf_type("+1");
    CHECK(run() && gk_focused(A.ui) == A.formula && A.editing == E_FIELD);
    gkf_key('\r', 0);
    CHECK(run() && !strcmp(inp("A2"), "=A1*2+1") && num("A2") == 25 && gk_focused(A.ui) == A.grid && A.v.row == 2);
    click_cell(1, 0);
    ctrl('E', 0);
    gkf_type("9");
    ctrl('E', 0);                               /* Ctrl+E again keeps what is being typed */
    CHECK(run() && !strcmp(gk_text(A.formula), "=A1*2+19") && A.editing == E_FIELD);
    gkf_key(27, 0);
    CHECK(run() && !strcmp(inp("A2"), "=A1*2+1") && gk_focused(A.ui) == A.grid && !A.editing);
    gkf_advance(1000);
    cell_xy(0, 1, &x, &y);
    gkf_click(x, y);
    gkf_click(x, y);                            /* a double-click edits the cell in the bar */
    CHECK(run() && gk_focused(A.ui) == A.formula && A.editing == E_FIELD && A.edit_row == 0 && A.edit_col == 1);
    gkf_type("hello");
    click_cell(5, 5);                           /* a click on the grid puts it in */
    CHECK(run() && !strcmp(inp("B1"), "hello") && A.v.row == 5 && A.v.col == 5 && gk_focused(A.ui) == A.grid);
    /* the cell's name: a cell or a range to go to */
    ctrl('L', 0);
    gkf_type("c5\n");
    CHECK(run() && A.v.row == 4 && A.v.col == 2 && gk_focused(A.ui) == A.grid && !strcmp(gk_text(A.name), "C5"));
    ctrl('L', 0);
    gkf_type("B2:D3\n");
    CHECK(run() && A.v.row == 1 && A.v.col == 1 && A.v.erow == 2 && A.v.ecol == 3);
    ctrl('L', 0);
    gkf_type("nope\n");
    CHECK(run() && status_is("No such cell or range: nope") && gk_focused(A.ui) == A.name);
    gkf_key(27, 0);
    CHECK(run() && gk_focused(A.ui) == A.grid);
    /* Copy and Paste: the cells with their formulas moved; text from elsewhere as typed; Cut */
    click_cell(0, 0);
    gkf_key(WIN_KEY_DOWN, WIN_MOD_SHIFT);
    ctrl('C', 0);
    CHECK(run() && !strcmp(gkf_clip(), "12\n25") && status_is("Copied A1:A2"));
    click_cell(0, 2);
    ctrl('V', 0);
    CHECK(run() && num("C1") == 12 && !strcmp(inp("C2"), "=C1*2+1") && num("C2") == 25);
    CHECK(status_is("Pasted into C1:C2"));
    CHECK(A.v.row == 0 && A.v.erow == 1 && A.v.col == 2);
    gkf_set_paste("5\tsix\n=1+1\n");
    click_cell(0, 4);
    ctrl('V', 0);
    CHECK(run() && num("E1") == 5 && !strcmp(inp("F1"), "six") && num("E2") == 2);
    click_cell(0, 4);
    gkf_key(WIN_KEY_RIGHT, WIN_MOD_SHIFT);
    ctrl('X', 0);
    CHECK(run() && vtype("E1") == SH_EMPTY && vtype("F1") == SH_EMPTY && !strcmp(gkf_clip(), "5\tsix"));
    click_cell(6, 4);
    ctrl('V', 0);
    CHECK(run() && num("E7") == 5 && !strcmp(inp("F7"), "six"));
    /* Undo and Redo */
    CHECK(!(gkf_menu_entry(gkf_menu_find("Undo"))->flags & WIN_MENU_DISABLED));
    ctrl('Z', 0);
    CHECK(run() && vtype("E7") == SH_EMPTY && status_is("Undone: paste") && A.v.row == 6 && A.v.col == 4);
    ctrl('Z', 0);
    CHECK(run() && num("E1") == 5);
    ctrl('Y', 0);
    CHECK(run() && vtype("E1") == SH_EMPTY && status_is("Redone: clearing"));
    gkf_type("1");                              /* Undo while typing: the cell as it was */
    ctrl('Z', 0);
    CHECK(run() && !A.editing && vtype("E1") == SH_EMPTY && status_is("The cell is as it was"));
    /* the formats, in the Format menu, its dot following the active cell */
    click_cell(0, 0);
    CHECK(gkf_menu_choose(MENU_PERCENT));
    CHECK(run() && sh_format(A.s, 0, 0) == SH_PERCENT(0));
    CHECK(gkf_menu_entry(gkf_menu_find("Percent"))->flags & WIN_MENU_RADIO);
    grid_origin(&gx, &gy);
    CHECK(gkf_text(gx + 128 - 3 - 40, gy + 22, "1200%", GW_INK));
    CHECK(gkf_menu_choose(MENU_MORE));
    CHECK(run() && sh_format(A.s, 0, 0) == SH_PERCENT(1));
    gkf_key('%', WIN_MOD_CTRL | WIN_MOD_SHIFT); /* Ctrl+%: percent again, its decimals kept */
    CHECK(run() && sh_format(A.s, 0, 0) == SH_PERCENT(1));
    gkf_key('~', WIN_MOD_CTRL | WIN_MOD_SHIFT);
    CHECK(run() && sh_format(A.s, 0, 0) == SH_GENERAL);
    CHECK(gkf_menu_entry(gkf_menu_find("General"))->flags & WIN_MENU_RADIO);
    /* rows inserted: the references follow */
    click_cell(0, 0);
    gkf_key('+', WIN_MOD_CTRL | WIN_MOD_SHIFT);
    CHECK(run() && num("A2") == 12 && !strcmp(inp("A3"), "=A2*2+1") && vtype("A1") == SH_EMPTY);
    ctrl('Z', 0);
    CHECK(run() && num("A1") == 12 && !strcmp(inp("A2"), "=A1*2+1"));
    /* a column's width by its header's edge, and by the Column width box */
    grid_origin(&gx, &gy);
    gkf_advance(1000);
    gkf_drag(gx + 48 + 80 - 2, gy + 10, gx + 48 + 80 + 38, gy + 10);
    CHECK(run() && sh_width(A.s, 0) == 120);
    CHECK(gkf_menu_choose(MENU_WIDTH));
    CHECK(run() && box_is(BOX_WIDTH));
    gkf_type("20\n");
    CHECK(run() && !gk_box_open(A.ui) && sh_width(A.s, 0) == 166);
    CHECK(gkf_menu_choose(MENU_WIDTH));
    gkf_type("0\n");
    CHECK(run() && status_is("A width is 1 to 99 characters") && sh_width(A.s, 0) == 166);
    CHECK(gkf_menu_choose(MENU_WIDTH_DEFAULT));
    CHECK(run() && sh_width(A.s, 0) == SH_WIDTH_DEFAULT);
    /* the wheel scrolls; keys that move far take the bars along */
    grid_origin(&gx, &gy);
    gkf_wheel(gx + 200, gy + 100, 3);
    CHECK(run() && A.v.top == 3);
    gkf_wheel(gx + 200, gy + 100, -3);
    CHECK(run() && A.v.top == 0);
    ctrl('L', 0);
    gkf_type("Z900\n");
    CHECK(run() && A.v.row == 899 && A.v.col == 25 && A.v.top > 800 && A.v.left > 10);
    gk_area_view(A.grid, &gv);
    CHECK(gv.sy == A.v.top && gv.sx == A.v.left);
    gkf_key(WIN_KEY_HOME, WIN_MOD_CTRL);
    CHECK(run() && A.v.top == 0 && A.v.left == 0);
    /* a move presents only the cells it left and reached, and the headers */
    gkf_presented(&x, &y, &w, &h);
    gkf_key(WIN_KEY_RIGHT, 0);
    CHECK(run());
    n = gkf_presented(&x, &y, &w, &h);
    {
        int px, py, pw, ph, area = 0;
        for(i = 0; i < n; i++) {
            gkf_presented_rect(i, &px, &py, &pw, &ph);
            area += pw * ph;
        }
        CHECK(n >= 1 && area < APP_W * APP_H / 4);
    }
    /* Save: the desktop's chooser the first time, then the same file; a save that fails; CSV asks first */
    ctrl('S', 0);
    CHECK(run() && gkf_dialog() && gkf_dialog()->kind == WIN_DLG_SAVE);
    CHECK(gkf_dialog() && !strcmp(gkf_dialog()->name, "Untitled.sheet") && !strcmp(gkf_dialog()->types, "sheet csv"));
    gkf_dialog_answer("/stick/budget.sheet");
    CHECK(run() && starts(file_text("/stick/budget.sheet"), "SurfOS sheet 1\n") && status_is("Saved budget.sheet"));
    CHECK(!strcmp(gkf_title(), "budget.sheet - Spreadsheet") && !sh_modified(A.s));
    CHECK(strstr(file_text("/stick/budget.sheet"), "cell A2 general =A1*2+1\n") != NULL);
    click_cell(9, 0);
    gkf_type("10\n");
    CHECK(run() && sh_modified(A.s));
    ctrl('S', 0);
    CHECK(run() && !gkf_dialog() && !sh_modified(A.s));
    CHECK(strstr(file_text("/stick/budget.sheet"), "cell A10 general 10\n") != NULL);
    gkf_type("11\n");
    save_fails = 1;
    ctrl('S', 0);
    CHECK(run() && box_is(BOX_FAILED) && sh_modified(A.s));
    gkf_key('\r', 0);
    CHECK(run() && !gk_box_open(A.ui));
    ctrl('S', WIN_MOD_SHIFT);                   /* Save as a CSV: it asks first, as the formulas would be lost */
    CHECK(run() && gkf_dialog());
    gkf_dialog_answer("/stick/budget.csv");
    CHECK(run() && box_is(BOX_CSV));
    gkf_key('\r', 0);
    CHECK(run() && starts(file_text("/stick/budget.csv"), "12,hello,12,,\r\n25,x,25,,2\r\n7,,,,\r\n#ERROR!,,,,\r\n"));
    CHECK(status_is("Saved the values in budget.csv") && !strcmp(gkf_title(), "budget.csv - Spreadsheet"));
    /* Open: unsaved changes asked about first; a CSV read; files that are not there, or not sheets */
    gkf_type("x");
    gkf_key('\r', 0);
    put_file("/stick/data.csv", "a,1\r\nb,2\r\n", 10);
    ctrl('O', 0);
    CHECK(run() && box_is(BOX_SAVE));
    gkf_key(WIN_KEY_RIGHT, 0);                  /* Don't save */
    gkf_key('\r', 0);
    CHECK(run() && gkf_dialog() && gkf_dialog()->kind == WIN_DLG_OPEN && !strcmp(gkf_dialog()->types, "sheet csv"));
    gkf_dialog_answer("/stick/data.csv");
    CHECK(run() && !app_reading(&A) && !strcmp(inp("A1"), "a") && num("B2") == 2 && !sh_modified(A.s));
    CHECK(status_is("Opened data.csv: 4 cells (CSV: values only)") && !strcmp(gkf_title(), "data.csv - Spreadsheet"));
    ctrl('O', 0);
    CHECK(run() && gkf_dialog());
    gkf_dialog_answer("/stick/missing.sheet");
    CHECK(run() && box_is(BOX_FAILED));
    gkf_key('\r', 0);
    put_file("/stick/bad.sheet", "hello\n", 6);
    ctrl('O', 0);
    run();
    gkf_dialog_answer("/stick/bad.sheet");
    CHECK(run() && box_is(BOX_FAILED) && !strcmp(inp("A1"), "a"));
    gkf_key('\r', 0);
    CHECK(run());
    /* a file named on the command line, as Files starts it */
    fresh("/stick/budget.sheet");
    CHECK(num("A2") == 25 && !strcmp(inp("B1"), "hello") && !strcmp(gkf_title(), "budget.sheet - Spreadsheet"));
    /* New: the status bar speaks of the new sheet, not of what the old one said at the same cell */
    put_file("/stick/broken.sheet", "SurfOS sheet 1\ncell A1 general =1+\n", 35);
    fresh("/stick/broken.sheet");
    CHECK(status_is("Opened broken.sheet: 1 cells"));
    gkf_key(27, 0);
    CHECK(run() && status_is("the formula ends too soon (at character 4)"));
    ctrl('N', 0);
    CHECK(run() && status_is("A new sheet") && vtype("A1") == SH_EMPTY);
    gkf_key(27, 0);
    CHECK(run() && status_is(""));
        /* a big file is read a piece at a time, and Esc stops it */
    for(n = i = 0; n < (int)sizeof(big) - 32; i++)
        n += snprintf(big + n, sizeof(big) - (size_t)n, "%d,row %d\r\n", i, i);
    put_file("/stick/big.csv", big, n);
    fresh(NULL);
    reads = 0;
    gkf_type("kept\n");
    run();
    ctrl('O', 0);
    run();
    CHECK(box_is(BOX_SAVE));
    gkf_key(WIN_KEY_RIGHT, 0);
    gkf_key('\r', 0);
    run();
    gkf_dialog_answer("/stick/big.csv");
    {
        struct gk_event ev;
        while(gk_next(A.ui, &ev, 0) > 0) app_event(&A, &ev);
        CHECK(app_reading(&A));
        app_read(&A);                           /* one turn of the loop: four pieces */
        CHECK(app_reading(&A) && reads == APP_CHUNKS && status_starts("Reading big.csv:"));
        gkf_type("q");
        while(gk_next(A.ui, &ev, 0) > 0) app_event(&A, &ev);
        CHECK(status_is("A file is being read: Esc stops it") && !strcmp(inp("A1"), "kept"));
        gkf_key(27, 0);
        while(gk_next(A.ui, &ev, 0) > 0) app_event(&A, &ev);
        CHECK(!app_reading(&A) && status_is("Not opened") && !strcmp(inp("A1"), "kept"));
    }
    ctrl('O', 0);                               /* read whole: 40,000 rows */
    run();
    CHECK(box_is(BOX_SAVE));
    gkf_key(WIN_KEY_RIGHT, 0);
    gkf_key('\r', 0);
    run();
    gkf_dialog_answer("/stick/big.csv");
    CHECK(run() && !app_reading(&A) && !strcmp(inp("B1"), "row 0") && num("A2") == 1);
    /* the theme and a new size */
    gkf_set_theme(WIN_THEME_DUSK, NULL);
    CHECK(run());
    gkf_resize(500, 300);
    CHECK(run() && A.v.w < 500 && A.v.h < 300 && A.v.w > 300);
    /* the review (11/09/26) */
    /* cells the formula bar cannot edit (a line break, a tab): neither a double-click, Ctrl+E nor a click in the
       bar edits them, and nothing changes; an edit of a cell it can edit that changes nothing changes nothing */
    {
        static const char lines[] = "SurfOS sheet 1\ncell A1 general line1\\nline2\ncell A2 general a\\tb\n"
                                    "cell B1 general =1+2\n";
        put_file("/stick/lines.sheet", lines, (int)strlen(lines));
    }
    fresh("/stick/lines.sheet");
    gkf_advance(1000);
    cell_xy(0, 0, &x, &y);
    gkf_click(x, y);
    gkf_click(x, y);                            /* a double-click on A1 */
    CHECK(run() && !A.editing && gk_focused(A.ui) == A.grid && status_starts("A1 holds a line break"));
    click_cell(5, 5);
    CHECK(run() && !strcmp(inp("A1"), "line1\nline2") && !sh_modified(A.s) && !sh_undo_label(A.s));
    click_cell(1, 0);
    ctrl('E', 0);                               /* Ctrl+E on A2, then Enter */
    CHECK(run() && !A.editing && status_starts("A2 holds a line break or a tab"));
    gkf_key('\r', 0);
    CHECK(run() && !A.editing && !strcmp(inp("A2"), "a\tb") && !sh_modified(A.s));
    click_cell(0, 0);
    gk_bounds(A.formula, &x, &y, &w, &h);
    gkf_click(x + 40, y + h / 2);               /* a click in the bar, on A1 */
    CHECK(run() && gk_focused(A.ui) == A.grid && !A.editing && status_starts("A1 holds a line break"));
    gkf_advance(1000);
    cell_xy(0, 1, &x, &y);
    gkf_click(x, y);
    gkf_click(x, y);                            /* B1, which the bar can edit */
    CHECK(run() && A.editing == E_FIELD);
    click_cell(5, 5);
    CHECK(run() && !A.editing && !sh_modified(A.s) && !sh_undo_label(A.s) && num("B1") == 3);
    click_cell(0, 1);
    ctrl('E', 0);
    gkf_key('\r', 0);
    CHECK(run() && !A.editing && !sh_modified(A.s) && !sh_undo_label(A.s));
    /* a text of 8,192 bytes that keeps its ' (a CSV field beginning with =): saved as .sheet, opened again, and
       its edit in the bar that changes nothing */
    big[0] = '=';
    memset(big + 1, 'a', 8191);
    memcpy(big + 8192, "\r\n", 2);
    put_file("/stick/long.csv", big, 8194);
    fresh("/stick/long.csv");
    CHECK(vtype("A1") == SH_TEXT && strlen(inp("A1")) == 8193);
    ctrl('S', WIN_MOD_SHIFT);
    run();
    gkf_dialog_answer("/stick/long.sheet");
    CHECK(run() && status_is("Saved long.sheet"));
    fresh("/stick/long.sheet");
    CHECK(!gk_box_open(A.ui) && vtype("A1") == SH_TEXT && strlen(inp("A1")) == 8193);
    ctrl('E', 0);
    CHECK(run() && A.editing == E_FIELD && strlen(gk_text(A.formula)) == 8193);
    gkf_key('\r', 0);
    CHECK(run() && !A.editing && !sh_modified(A.s) && strlen(inp("A1")) == 8193);
    /* a file larger than a reader takes is not written */
    sh_file_max = 100;
    ctrl('S', WIN_MOD_SHIFT);
    run();
    gkf_dialog_answer("/stick/huge.sheet");
    CHECK(run() && box_is(BOX_FAILED) && !find("/stick/huge.sheet", 0));
    sh_file_max = SH_FILE_MAX;
    gkf_key('\r', 0);
    CHECK(run() && !gk_box_open(A.ui));
    /* CSV leaves out formats too: it asks first; after such a save, Close asks, its Save as offering a .sheet */
    fresh(NULL);
    gkf_type("50%\n1.5\n");
    run();
    ctrl('S', 0);
    run();
    gkf_dialog_answer("/stick/f.csv");
    CHECK(run() && box_is(BOX_CSV) && !find("/stick/f.csv", 0));
    gkf_key('\r', 0);                           /* Save */
    CHECK(run() && !strcmp(file_text("/stick/f.csv"), "0.5\r\n1.5\r\n") && !sh_modified(A.s) &&
          A.lossy == SH_LOSE_FORMATS);
    ctrl('W', 0);
    CHECK(run() && box_is(BOX_SAVE));           /* the formats are in this window alone */
    gkf_key('\r', 0);                           /* Save as */
    CHECK(run() && gkf_dialog() && !strcmp(gkf_dialog()->name, "f.sheet"));
    gkf_dialog_answer("/stick/f.sheet");
    CHECK(run() == 0 && find("/stick/f.sheet", 0));     /* saved whole, then closed */
    /* the values only, saved: Ctrl+W still asks, and Don't save closes */
    fresh(NULL);
    gkf_type("2\n=A1*3\n");
    run();
    ctrl('S', WIN_MOD_SHIFT);
    run();
    gkf_dialog_answer("/stick/v.csv");
    CHECK(run() && box_is(BOX_CSV));
    gkf_key('\r', 0);
    CHECK(run() && !strcmp(file_text("/stick/v.csv"), "2\r\n6\r\n") && A.lossy == SH_LOSE_FORMULAS);
    ctrl('W', 0);
    CHECK(run() && box_is(BOX_SAVE));
    gkf_key(WIN_KEY_RIGHT, 0);                  /* Don't save */
    gkf_key('\r', 0);
    CHECK(run() == 0);
    /* the close button over the CSV question: its own question takes its place and goes on */
    fresh(NULL);
    gkf_type("=1+1\n");
    run();
    ctrl('S', WIN_MOD_SHIFT);
    run();
    gkf_dialog_answer("/stick/w.csv");
    CHECK(run() && box_is(BOX_CSV));
    gkf_close();
    CHECK(run() && box_is(BOX_SAVE) && A.after == AFTER_CLOSE);
    gkf_key(WIN_KEY_RIGHT, 0);                  /* Don't save */
    gkf_key('\r', 0);
    CHECK(run() == 0 && !find("/stick/w.csv", 0));
    /* the menu's Paste is off while the bar edits (its field's Ctrl+V pastes at its cursor); pasted into what is
       typed, the clipboard's first line, a tab as a space */
    fresh(NULL);
    gkf_type("=A1+B1\n");
    run();
    click_cell(0, 0);
    ctrl('E', 0);
    CHECK(run() && A.editing == E_FIELD && (gkf_menu_entry(gkf_menu_find("Paste"))->flags & WIN_MENU_DISABLED));
    gkf_set_paste("C1*");
    gkf_menu_choose(MENU_PASTE);
    CHECK(run() && !strcmp(gk_text(A.formula), "=A1+B1"));
    gkf_key(27, 0);
    CHECK(run() && !(gkf_menu_entry(gkf_menu_find("Paste"))->flags & WIN_MENU_DISABLED));
    gkf_set_paste("a\tb\r\nnext");
    gkf_type("x");
    CHECK(gkf_menu_choose(MENU_PASTE) && run() && A.editing == E_TYPING && !strcmp(A.edit, "xa b"));
    gkf_key(27, 0);
    run();
    /* a Cut pasted moves the cell: its formula keeps its reference */
    gkf_type("4\n");
    click_cell(0, 2);
    gkf_type("=A1*2\n");
    run();
    click_cell(0, 2);
    ctrl('X', 0);
    run();
    gkf_set_paste(gkf_clip());                  /* the desktop's clipboard holds what the Cut put there */
    click_cell(4, 2);
    ctrl('V', 0);
    CHECK(run() && !strcmp(inp("C5"), "=A1*2") && num("C5") == 8 && vtype("C1") == SH_EMPTY);
    /* reading far into a file of 30 MB: its percentage worked out in 64 bits */
    fresh(NULL);
    ctrl('O', 0);
    run();
    gkf_dialog_answer("/stick/big.csv");
    {
        struct gk_event ev;
        while(gk_next(A.ui, &ev, 0) > 0) app_event(&A, &ev);
        CHECK(app_reading(&A));
        A.rd_done = 25000000;
        A.rd_size = 30000000;
        app_read(&A);
        CHECK(app_reading(&A) && status_is("Reading big.csv: 84%"));
        gkf_key(27, 0);
        while(gk_next(A.ui, &ev, 0) > 0) app_event(&A, &ev);
        CHECK(!app_reading(&A));
    }
    fresh(NULL);
    /* closing: asked first when anything is not saved; Cancel keeps it, Don't save ends it */
    gkf_close();
    CHECK(run() == 0);                          /* nothing unsaved: it closes */
    fresh(NULL);
    gkf_type("1\n");
    run();
    gkf_close();
    CHECK(run() && box_is(BOX_SAVE));
    gkf_close();                                /* pressed again: the question stays */
    CHECK(run() && box_is(BOX_SAVE) && A.after == AFTER_CLOSE);
    gkf_key(27, 0);
    CHECK(run() && !gk_box_open(A.ui) && status_is("Nothing changed"));
    ctrl('N', 0);                               /* a question in place of another goes on with the new one */
    CHECK(run() && box_is(BOX_SAVE) && A.after == AFTER_NEW);
    gkf_close();
    CHECK(run() && box_is(BOX_SAVE) && A.after == AFTER_CLOSE);
    gkf_key(27, 0);
    CHECK(run() && !gk_box_open(A.ui) && A.after == AFTER_NONE);
    ctrl('W', 0);
    CHECK(run() && box_is(BOX_SAVE));
    gkf_key(WIN_KEY_RIGHT, 0);
    gkf_key('\r', 0);
    CHECK(run() == 0);
    app_close(&A);
    CHECK(!gkf_open());
    for(i = 0; i < FILES; i++) free(files[i].data);
    gkf_reset(0, 0);
    printf("test_wsheet_win: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
