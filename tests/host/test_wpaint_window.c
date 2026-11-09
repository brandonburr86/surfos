/* Host tests for Paint's window (user/wpaint/window.c and panel.c, 11/05/26, the desktop applications, track paint) on
   the fake desktop (tests/host/gk_fake.c), driven as a person drives it, the pattern of track kit's test_wkit: no
   desktop refused; the window's parts in place and the canvas shown on the backdrop; the brush drawing by a drag, a
   click presenting little more than its pixels; tools chosen by the palette, the keys and the Tools menu, the chosen
   one lit and checked; sizes, outlined or filled, the text's scale; a rectangle, a line, text typed and put down, the
   picker; the selection copied, pasted and moved; undo and redo by their shortcuts; zoom by the keys, the menu (its
   submenu's dot following), Ctrl and the wheel, about the pointer; scrolling by the wheel, the keys and the middle
   button; the colours by the palette (both buttons), the swap, a colour typed into Edit...; the status line and the
   title (* while changed); the menus greyed while they do nothing; Save as through the desktop's chooser (the name and
   kinds offered), Save, Open (a picture fitted to the view), a GIF saved as PNG, a file that is no picture refused
   with the canvas as it was, a desktop without choosers (the path typed); New, Open and the close button asking first
   about changes, with Save, Don't save and Cancel, File > Close too; a picture given at the start; a new size of the
   window; the desktop's theme. Built and run by `make unittest`. */
#define _FILE_OFFSET_BITS 64    /* readdir(): in the Docker image the 32-bit call fails at once (EOVERFLOW), as the */
                                /* folders' offsets there are 64-bit; the large-file one reads them. macOS ignores it */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <gk.h>
#include <image.h>
#include "gk_fake.h"
#include "window.h"
#define WPH_NAMES_ONLY
#include "wpaint_host.h"

int wph_at[WPH_CALLS], wph_errno[WPH_CALLS], wph_count[WPH_CALLS], wph_short;   /* files.c's calls: none fail here */

static int checks, failed;
#define CHECK(x) do { checks++; if(!(x)) { failed++; printf("FAIL %s line %d: %s\n", __FILE__, __LINE__, #x); } } while(0)

#define BLACK 0x000000u
#define WHITE 0xFFFFFFu

static struct pwin W, *w = &W;
static char dir[400];

static const char *at(const char *name) {
    static char path[4][600];
    static int k;
    k = (k + 1) % 4;
    snprintf(path[k], sizeof(path[k]), "%s/%s", dir, name);
    return path[k];
}

/* every event the window has, through the program; 0 once it asked to close */
static int run(void) {
    struct gk_event ev;
    while(gk_next(w->ui, &ev, 0) > 0) if(!pw_event(w, &ev)) return 0;
    return 1;
}

/* where a canvas pixel shows in the window */
static void spot(int cx, int cy, int *x, int *y) {
    int ax, ay, aw, ah, vx, vy;
    gk_bounds(w->canvas, &ax, &ay, &aw, &ah);
    paint_to_view(&w->p, cx, cy, &vx, &vy);
    *x = ax + vx;
    *y = ay + vy;
}

static unsigned shown(int cx, int cy) {
    int x, y;
    spot(cx, cy, &x, &y);
    return gkf_pixel(x, y);
}

static unsigned px(int cx, int cy) {
    return w->p.px[(size_t)cy * w->p.w + cx];
}

static void drag(int cx0, int cy0, int cx1, int cy1) {
    int x0, y0, x1, y1;
    spot(cx0, cy0, &x0, &y0);
    spot(cx1, cy1, &x1, &y1);
    gkf_advance(1000);                                  /* no double-click with what came before */
    gkf_drag(x0, y0, x1, y1);
    run();
}

static void click(int cx, int cy) {
    int x, y;
    spot(cx, cy, &x, &y);
    gkf_advance(1000);
    gkf_click(x, y);
    run();
}

static void click_in(struct gk_widget *area, int x, int y, unsigned button) {
    int ax, ay, aw, ah;
    gk_bounds(area, &ax, &ay, &aw, &ah);
    gkf_advance(1000);
    gkf_pointer(ax + x, ay + y, button);
    gkf_pointer(ax + x, ay + y, 0);
    run();
}

static void click_tool(int tool) {
    click_in(w->tools, PW_TOOL_X0 + tool % 2 * PW_TOOL_STEP + 12, PW_TOOL_Y0 + tool / 2 * PW_TOOL_STEP + 12,
             WIN_BUTTON_LEFT);
}

static void key(unsigned k, unsigned mods) {
    gkf_key(k, mods);
    run();
}

static int status_says(const char *s) {
    int x, y, sw, sh;
    gk_bounds(w->status, &x, &y, &sw, &sh);
    return gkf_text(x + 10, y + 3, s, GW_INK);
}

static int checked(const char *label) {
    int i = gkf_menu_find(label);
    return i >= 0 && (gkf_menu_entry(i)->flags & (WIN_MENU_CHECKED | WIN_MENU_RADIO)) != 0;
}

static int greyed(const char *label) {
    int i = gkf_menu_find(label);
    return i >= 0 && (gkf_menu_entry(i)->flags & WIN_MENU_DISABLED) != 0;
}

static long canvas_count(unsigned c) {
    long n = 0;
    int i;
    for(i = 0; i < w->p.w * w->p.h; i++) n += w->p.px[i] == c;
    return n;
}

/**** the window and drawing ****/

static void window(void) {
    int x, y, ww, hh, n, i, area;
    gkf_reset(0, 0);
    gkf_refuse(ENODEV);
    CHECK(pw_open(w, NULL) < 0 && errno == ENODEV);
    CHECK(pw_open(w, NULL) == 0 && gkf_w() == PW_W && gkf_h() == PW_H && gkf_resizable());
    gkf_focus(1);
    CHECK(run() && !strcmp(gkf_title(), "Untitled - Paint"));
    /* the parts: the palette at the left, the canvas, the colours, the status line */
    gk_bounds(w->tools, &x, &y, &ww, &hh);
    CHECK(x == 0 && y == 0 && ww == PW_TOOLS_W && hh == PW_H - PW_COLOURS_H - GK_STATUS_H);
    gk_bounds(w->canvas, &x, &y, &ww, &hh);
    CHECK(x == PW_TOOLS_W && ww == PW_W - PW_TOOLS_W && hh == PW_H - PW_COLOURS_H - GK_STATUS_H);
    gk_bounds(w->colours, &x, &y, &ww, &hh);
    CHECK(y == PW_H - PW_COLOURS_H - GK_STATUS_H && hh == PW_COLOURS_H && ww == PW_W);
    /* a new canvas at 100%: white, on the backdrop, the brush lit in the palette */
    CHECK(w->p.zoom == PAINT_ZOOM_1 && shown(0, 0) == WHITE && shown(639, 100) == WHITE);
    spot(0, 0, &x, &y);
    CHECK(gkf_pixel(x - 1, y) == GW_NAVY && gkf_pixel(x - 20, y + 50) == GW_NAVY);
    CHECK(gkf_pixel(PW_TOOL_X0 + PW_TOOL_STEP + 2, PW_TOOL_Y0 + 2) == GW_TEAL);      /* the brush, lit */
    CHECK(gkf_pixel(PW_TOOL_X0 + 2, PW_TOOL_Y0 + 2) == GW_SHELL);                    /* the pencil, not */
    CHECK(status_says("Brush, 3 px: drag to paint"));
    CHECK(checked("Brush") && !checked("Pencil") && checked("100%") && checked("3 px") && !checked("Filled shapes"));
    CHECK(greyed("Undo") && greyed("Redo") && greyed("Copy") && greyed("Paste") && !greyed("Select all"));
    /* the brush: a drag draws; the title says it changed */
    drag(20, 20, 120, 20);
    CHECK(px(20, 20) == BLACK && px(120, 21) == BLACK && px(70, 19) == BLACK && px(70, 23) == WHITE);
    CHECK(shown(70, 20) == BLACK && !strcmp(gkf_title(), "*Untitled - Paint") && !greyed("Undo"));
    /* a click presents little more than its pixels */
    gkf_presented(&i, &i, &i, &i);
    click(300, 200);
    n = gkf_presented(&x, &y, &ww, &hh);
    for(area = 0, i = 0; i < n; i++) {
        int px0, py0, pw0, ph0;
        gkf_presented_rect(i, &px0, &py0, &pw0, &ph0);
        area += pw0 * ph0;
    }
    CHECK(px(300, 200) == BLACK && n >= 1 && area < PW_W * PW_H / 8);
    /* undo and redo by their shortcuts */
    key(26, WIN_MOD_CTRL);
    CHECK(px(300, 200) == WHITE && px(20, 20) == BLACK && !greyed("Redo"));
    key(25, WIN_MOD_CTRL);
    CHECK(px(300, 200) == BLACK);
    key(26, WIN_MOD_CTRL);
    key(26, WIN_MOD_CTRL);
    CHECK(canvas_count(BLACK) == 0 && !strcmp(gkf_title(), "Untitled - Paint") && greyed("Undo"));
}

static void tools(void) {
    int x, y, i, n;
    /* the palette: the pencil lit, the line and its sizes */
    click_tool(PAINT_PENCIL);
    CHECK(w->p.tool == PAINT_PENCIL && gkf_pixel(PW_TOOL_X0 + 2, PW_TOOL_Y0 + 2) == GW_TEAL && checked("Pencil"));
    CHECK(status_says("Pencil: drag to draw"));
    drag(10, 300, 60, 300);
    CHECK(px(10, 300) == BLACK && px(60, 300) == BLACK && px(35, 301) == WHITE);
    click_tool(PAINT_LINE);
    click_in(w->tools, PW_TOOL_X0 + PW_TOOL_STEP + 5, PW_OPT_Y0 + 3 * PW_OPT_STEP + 5, WIN_BUTTON_LEFT);  /* 24 px */
    CHECK(w->p.size == 7 && checked("24 px") && status_says("Line, 24 px"));
    key(']', 0);                                        /* no larger */
    key('[', 0);
    key('[', 0);
    CHECK(w->p.size == 5 && checked("12 px"));
    drag(100, 100, 200, 100);
    CHECK(px(150, 95) == BLACK && px(150, 106) == BLACK && px(150, 94) == WHITE && px(150, 107) == WHITE);  /* 12 rows */
    /* by the keys, and by the Tools menu */
    key('r', 0);
    CHECK(w->p.tool == PAINT_RECT && checked("Rectangle"));
    CHECK(gkf_menu_choose(MENU_FILLED) && run() && w->p.filled && checked("Filled shapes"));
    CHECK(status_says("Rectangle, filled"));
    drag(300, 300, 309, 309);
    for(n = 0, y = 300; y < 310; y++)
        for(x = 300; x < 310; x++) n += px(x, y) == BLACK;
    CHECK(n == 100);
    click_in(w->tools, PW_TOOL_X0 + 5, PW_OPT_Y0 + 5, WIN_BUTTON_LEFT);     /* outlined, in the palette */
    CHECK(!w->p.filled && !checked("Filled shapes"));
    CHECK(gkf_menu_choose(MENU_TOOL + PAINT_ELLIPSE) && run() && w->p.tool == PAINT_ELLIPSE);
    /* text: typed, put down by a click elsewhere, in the console's font */
    key('t', 0);
    click_in(w->tools, PW_TOOL_X0 + 5, PW_OPT_Y0 + 5, WIN_BUTTON_LEFT);     /* 1x */
    CHECK(w->p.tool == PAINT_TEXT && w->p.text_scale == 1 && checked("1 times"));
    click(400, 50);
    gkf_type("Pq");
    run();
    CHECK(paint_typing(&w->p) && w->p.text.n == 2 && status_says("Type; Enter"));
    gkf_type("\n2");
    run();
    CHECK(w->p.text.n == 4);
    for(n = 0, i = 0; i < 16 * 8; i++) n += shown(400 + i % 8, 50 + i / 8) == BLACK;   /* shown before it is drawn */
    CHECK(n > 10 && px(401, 52) == WHITE);
    click(500, 300);
    for(n = 0, i = 0; i < 16 * 8; i++) n += px(400 + i % 8, 50 + i / 8) == BLACK;
    CHECK(n > 10 && paint_typing(&w->p));
    key(27, 0);
    CHECK(!paint_typing(&w->p));
    /* the picker: the colour clicked, and the tool before it back */
    key('k', 0);
    w->p.px[10 * w->p.w + 10] = 0x2468AC;
    click(10, 10);
    CHECK(w->p.fg == 0x2468AC && w->p.tool == PAINT_TEXT && status_says("Took #2468AC"));
    paint_set_colour(&w->p, 0, BLACK);
    run();
}

static void selection(void) {
    int i;
    key('s', 0);
    paint_new(&w->p, 640, 400);
    run();
    w->p.px[5 * 640 + 5] = BLACK;
    w->p.px[6 * 640 + 6] = BLACK;
    drag(5, 5, 6, 6);
    CHECK(paint_has_selection(&w->p) && w->p.sel.w == 2 && !greyed("Copy") && !greyed("Delete"));
    key(3, WIN_MOD_CTRL);                               /* Ctrl+C */
    CHECK(paint_can_paste(&w->p) && !greyed("Paste"));
    key(22, WIN_MOD_CTRL);                              /* Ctrl+V: floating at the view's top left */
    CHECK(w->p.sel.buf && w->p.sel.x == 0 && w->p.sel.y == 0);
    drag(0, 0, 100, 100);
    CHECK(w->p.sel.x == 100 && w->p.sel.y == 100 && shown(100, 100) == BLACK && px(100, 100) == WHITE);
    key(13, 0);                                         /* Enter: put down */
    CHECK(!w->p.sel.buf && px(100, 100) == BLACK && px(101, 101) == BLACK);
    key(WIN_KEY_RIGHT, 0);                              /* the arrows move the selection */
    CHECK(w->p.sel.buf && w->p.sel.x == 101);
    key(WIN_KEY_DELETE, 0);                             /* Del, the Edit menu's shortcut */
    CHECK(!paint_has_selection(&w->p) && px(100, 100) == WHITE && canvas_count(BLACK) == 2);
    key(1, WIN_MOD_CTRL);                               /* Ctrl+A */
    CHECK(paint_has_selection(&w->p) && w->p.sel.w == 640);
    key(27, 0);
    CHECK(!paint_has_selection(&w->p));
    for(i = 0; i < 3; i++) key(26, WIN_MOD_CTRL);
    run();
}

/**** the view ****/

static void view(void) {
    int x, y, cx, cy, ax, ay, aw, ah;
    paint_new(&w->p, 640, 400);
    run();
    gk_bounds(w->canvas, &ax, &ay, &aw, &ah);
    /* the zoom: + about the view's middle, Ctrl+0 back, the Zoom submenu's dot following */
    key('+', 0);
    CHECK(w->p.zoom == PAINT_ZOOM_1 + 1 && checked("200%") && !checked("100%"));
    key('0', WIN_MOD_CTRL);
    CHECK(w->p.zoom == PAINT_ZOOM_1 && checked("100%"));
    CHECK(gkf_menu_choose(MENU_ZOOM + PAINT_ZOOM_1 + 3) && run() && w->p.zoom == PAINT_ZOOM_1 + 3 && checked("400%"));
    /* Ctrl and the wheel, about the pointer: the canvas's point under it stays */
    paint_to_canvas(&w->p, 300, 200, &cx, &cy);
    gkf_mods(WIN_MOD_CTRL);
    gkf_wheel(ax + 300, ay + 200, 3);                   /* toward the user: out */
    gkf_mods(0);
    run();
    paint_to_canvas(&w->p, 300, 200, &x, &y);
    CHECK(w->p.zoom == PAINT_ZOOM_1 + 2 && abs(x - cx) <= 1 && abs(y - cy) <= 1);
    /* the wheel scrolls down, and across with Shift; the arrows and Home with nothing selected */
    x = w->p.sx;
    y = w->p.sy;
    gkf_wheel(ax + 300, ay + 200, 3);
    run();
    CHECK(w->p.sy == y + 48 && w->p.sx == x);
    gkf_mods(WIN_MOD_SHIFT);
    gkf_wheel(ax + 300, ay + 200, -3);
    gkf_mods(0);
    run();
    CHECK(w->p.sx == x - 48 || w->p.sx == 0);
    key(WIN_KEY_HOME, 0);
    CHECK(w->p.sx == 0 && w->p.sy == 0);
    key(WIN_KEY_DOWN, 0);
    CHECK(w->p.sy == 32);
    /* the middle button drags the view */
    gkf_pointer(ax + 300, ay + 200, WIN_BUTTON_MIDDLE);
    gkf_pointer(ax + 250, ay + 150, WIN_BUTTON_MIDDLE);
    gkf_pointer(ax + 250, ay + 150, 0);
    run();
    CHECK(w->p.sx == 50 && w->p.sy == 82 && canvas_count(BLACK) == 0);
    /* Fit and the status line's size and zoom */
    CHECK(gkf_menu_choose(MENU_FIT) && run() && w->p.zoom == PAINT_ZOOM_1 - 1);
    CHECK(gkf_find("640 x 400   50%", GW_MUTED, &x, &y));
    /* a new size of the window: the view follows */
    gkf_resize(600, 360);
    run();
    gk_bounds(w->canvas, &ax, &ay, &aw, &ah);
    CHECK(aw == 600 - PW_TOOLS_W && w->p.vw <= aw && w->p.vh <= 360 - PW_COLOURS_H - GK_STATUS_H);
    gkf_resize(PW_W, PW_H);
    run();
    key('0', WIN_MOD_CTRL);
}

static void colours(void) {
    int x, y, cw, ch;
    gk_bounds(w->colours, &x, &y, &cw, &ch);
    click_in(w->colours, PW_PALETTE_X + 12 * 0 + 2, 4 + PW_SWATCH_STEP + 2, WIN_BUTTON_LEFT);   /* the second row's first */
    CHECK(w->p.fg == paint_palette[12] && gkf_pixel(x + 10, y + 8) == paint_palette[12]);
    click_in(w->colours, PW_PALETTE_X + PW_SWATCH_STEP + 2, 6, WIN_BUTTON_RIGHT);
    CHECK(w->p.bg == paint_palette[1]);
    click_in(w->colours, 10, 10, WIN_BUTTON_LEFT);      /* the swatches: swapped */
    CHECK(w->p.fg == paint_palette[1] && w->p.bg == paint_palette[12]);
    key('x', 0);
    CHECK(w->p.fg == paint_palette[12]);
    /* Edit...: a colour typed, the newest custom colour and the first */
    click_in(w->colours, PW_EDIT_X + 10, 20, WIN_BUTTON_LEFT);
    CHECK(gk_box_open(w->ui) && gk_asking(w->ui) == BOX_COLOUR);
    key(1, WIN_MOD_CTRL);
    gkf_type("10, 20, 30\n");
    run();
    CHECK(w->p.fg == 0x0A141E && w->p.custom[0] == 0x0A141E && gkf_find("#0A141E", GW_MUTED, &x, &y));
    CHECK(gkf_menu_choose(MENU_EDIT_COLOUR) && run() && gk_box_open(w->ui));
    key(1, WIN_MOD_CTRL);
    gkf_type("purple\n");
    run();
    CHECK(gk_box_open(w->ui) && gk_asking(w->ui) == BOX_MESSAGE);
    key(13, 0);
    click_in(w->colours, PW_CUSTOM_X + 2, 6, WIN_BUTTON_RIGHT);
    CHECK(w->p.bg == 0x0A141E && !gk_box_open(w->ui));
    CHECK(gkf_menu_choose(MENU_DEFAULT_COLOURS) && run() && w->p.fg == BLACK && w->p.bg == WHITE);
}

/**** files ****/

static int exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static void files(void) {
    char path[600];
    int x, y;
    paint_new(&w->p, 200, 100);
    run();
    key('b', 0);
    drag(10, 10, 50, 10);
    /* Save of a picture with no name: the desktop's chooser, the name and the kinds offered */
    key(19, WIN_MOD_CTRL);
    CHECK(gkf_dialog() && gkf_dialog()->kind == WIN_DLG_SAVE && gkf_dialog()->id == CHOOSE_SAVE &&
          !strcmp(gkf_dialog()->name, "picture.png") && !strcmp(gkf_dialog()->types, "png bmp"));
    gkf_dialog_answer(at("one.png"));
    run();
    CHECK(exists(at("one.png")) && !strcmp(gkf_title(), "one.png - Paint") && !paint_modified(&w->p));
    CHECK(status_says("Saved one.png"));
    /* changed, then Save: the same file, no chooser */
    drag(10, 50, 50, 50);
    CHECK(!strcmp(gkf_title(), "*one.png - Paint"));
    key(19, WIN_MOD_CTRL);
    CHECK(!gkf_dialog() && !paint_modified(&w->p) && !strcmp(gkf_title(), "one.png - Paint"));
    /* Save as: BMP, in the folder of the file */
    key(19, WIN_MOD_CTRL | WIN_MOD_SHIFT);
    CHECK(gkf_dialog() && !strcmp(gkf_dialog()->dir, dir) && !strcmp(gkf_dialog()->name, "one.png"));
    gkf_dialog_answer(at("two.bmp"));
    run();
    CHECK(exists(at("two.bmp")) && !strcmp(gkf_title(), "two.bmp - Paint"));
    /* Open: the chooser with the kinds read; the picture as it was saved */
    paint_new(&w->p, 10, 10);
    w->path[0] = 0;
    run();
    key(15, WIN_MOD_CTRL);
    CHECK(gkf_dialog() && gkf_dialog()->kind == WIN_DLG_OPEN && !strcmp(gkf_dialog()->types, "png bmp gif jpg jpeg"));
    gkf_dialog_answer(at("one.png"));
    run();
    CHECK(w->p.w == 200 && px(30, 10) == BLACK && px(30, 50) == BLACK && !strcmp(gkf_title(), "one.png - Paint"));
    CHECK(status_says("Opened one.png, 200 x 100") && !paint_can_undo(&w->p));
    /* a GIF: opened, then Save offers it as a PNG */
    key(15, WIN_MOD_CTRL);
    gkf_dialog_answer("tests/fixtures/wundertow/images/local.gif");
    run();
    CHECK(!strcmp(gkf_title(), "local.gif - Paint"));
    key(19, WIN_MOD_CTRL);
    CHECK(gkf_dialog() && gkf_dialog()->kind == WIN_DLG_SAVE && !strcmp(gkf_dialog()->name, "local.png") &&
          gkf_dialog()->dir[0] == '/' && strstr(gkf_dialog()->dir, "/tests/fixtures/wundertow/images"));   /* absolute */
    gkf_dialog_answer(NULL);
    run();
    /* a large picture is fitted to the view */
    paint_free(&w->p);
    CHECK(paint_init(&w->p, 1500, 1000) == 0);
    {
        char why[PF_WHY], kept[600];
        CHECK(pf_save(&w->p, at("big.png"), why, sizeof(why), kept, sizeof(kept)) == 0);
    }
    run();
    key(15, WIN_MOD_CTRL);
    gkf_dialog_answer(at("big.png"));
    run();
    CHECK(w->p.w == 1500 && w->p.zoom < PAINT_ZOOM_1 && w->p.zoom >= 0);
    /* no picture: refused, the canvas as it was */
    {
        FILE *f = fopen(at("notes.png"), "w");
        fputs("not a picture", f);
        fclose(f);
    }
    key(15, WIN_MOD_CTRL);
    gkf_dialog_answer(at("notes.png"));
    run();
    CHECK(gk_box_open(w->ui) && gk_asking(w->ui) == BOX_MESSAGE && w->p.w == 1500 && !strcmp(gkf_title(), "big.png - Paint"));
    CHECK(gkf_find("Cannot open the picture", GW_PAPER, &x, &y) || gkf_find("Cannot open the picture", GW_INK, &x, &y));
    key(13, 0);
    CHECK(!gk_box_open(w->ui));
    /* a desktop without choosers: the path typed */
    gkf_lacks(1u << WIN_DIALOG, ENOSYS);
    key(15, WIN_MOD_CTRL);
    CHECK(gk_box_open(w->ui));
    key(1, WIN_MOD_CTRL);
    snprintf(path, sizeof(path), "%s\n", at("two.bmp"));
    gkf_type(path);
    run();
    CHECK(w->p.w == 200 && !strcmp(gkf_title(), "two.bmp - Paint"));
    gkf_lacks(0, 0);
    /* a kind Paint does not write, typed into Save as: refused, saying so */
    key(19, WIN_MOD_CTRL | WIN_MOD_SHIFT);
    gkf_dialog_answer(at("three.jpg"));
    run();
    CHECK(gk_box_open(w->ui) && !exists(at("three.jpg")));
    key(13, 0);
}

static void asking(void) {
    /* New with changes: Cancel keeps them; Don't save goes on to the size */
    drag(10, 90, 50, 90);
    CHECK(paint_modified(&w->p));
    key(14, WIN_MOD_CTRL);
    CHECK(gk_box_open(w->ui) && gk_asking(w->ui) == BOX_UNSAVED);
    key(27, 0);
    CHECK(!gk_box_open(w->ui) && paint_modified(&w->p) && w->p.w == 200);
    key(14, WIN_MOD_CTRL);
    key(WIN_KEY_RIGHT, 0);
    key(13, 0);                                         /* Don't save */
    CHECK(gk_box_open(w->ui) && gk_asking(w->ui) == BOX_NEW);
    key(1, WIN_MOD_CTRL);
    gkf_type("300 x 150\n");
    run();
    CHECK(w->p.w == 300 && w->p.h == 150 && !strcmp(gkf_title(), "Untitled - Paint") && w->p.zoom == PAINT_ZOOM_1);
    /* a size that is none */
    key(14, WIN_MOD_CTRL);
    key(1, WIN_MOD_CTRL);
    gkf_type("5000 x 10\n");
    run();
    CHECK(gk_box_open(w->ui) && gk_asking(w->ui) == BOX_MESSAGE && w->p.w == 300);
    key(13, 0);
    /* Canvas size, through Image */
    CHECK(gkf_menu_choose(MENU_RESIZE) && run() && gk_asking(w->ui) == BOX_RESIZE);
    key(1, WIN_MOD_CTRL);
    gkf_type("320x160\n");
    run();
    CHECK(w->p.w == 320 && w->p.h == 160 && !strcmp(paint_undo_what(&w->p), "Size"));
    /* Open with changes, Save first: an untitled picture goes through Save as, then the chooser to open */
    key(15, WIN_MOD_CTRL);
    CHECK(gk_asking(w->ui) == BOX_UNSAVED);
    key(13, 0);                                         /* Save */
    CHECK(gkf_dialog() && gkf_dialog()->kind == WIN_DLG_SAVE);
    gkf_dialog_answer(at("four.png"));
    run();
    CHECK(exists(at("four.png")) && gk_asking(w->ui) == BOX_THEN_OPEN);
    key(13, 0);
    CHECK(gkf_dialog() && gkf_dialog()->kind == WIN_DLG_OPEN);
    gkf_dialog_answer(at("one.png"));
    run();
    CHECK(w->p.w == 200 && !strcmp(gkf_title(), "one.png - Paint"));
    /* the close button with changes: Cancel stays; Save saves and closes */
    drag(100, 10, 150, 10);
    gkf_close();
    CHECK(run() && gk_asking(w->ui) == BOX_UNSAVED);
    key(27, 0);
    CHECK(gkf_open() && !gk_box_open(w->ui));
    gkf_key(23, WIN_MOD_CTRL);                          /* Ctrl+W, File > Close: asked again */
    CHECK(run() && gk_asking(w->ui) == BOX_UNSAVED);
    gkf_key(13, 0);                                     /* Save: saved, and closed */
    CHECK(!run());
    pw_close(w);
    CHECK(!gkf_open());
    {
        struct paint q;
        char why[PF_WHY];
        paint_init(&q, 5, 5);
        CHECK(pf_open(&q, at("one.png"), why, sizeof(why)) == 0 && q.px[10 * 200 + 120] == BLACK);
        paint_free(&q);
    }
}

static void starting(void) {
    unsigned colors[WIN_COLORS];
    int i;
    /* a picture given at the start: opened at 100% when it fits, fitted to the view's real size when it does not; one
       that cannot be: said, over a new canvas */
    gkf_reset(0, 0);
    CHECK(pw_open(w, at("two.bmp")) == 0 && w->p.w == 200);
    gkf_focus(1);
    CHECK(run() && !strcmp(gkf_title(), "two.bmp - Paint") && w->p.zoom == PAINT_ZOOM_1 && shown(100, 50) != GW_NAVY);
    pw_close(w);
    gkf_reset(0, 0);
    CHECK(pw_open(w, at("big.png")) == 0 && w->p.w == 1500);
    gkf_focus(1);
    {
        int cw, ch;
        CHECK(run() && w->p.zoom == PAINT_ZOOM_1 - 2 && w->p.vw > 600 && w->p.vh > 300);   /* 1/3: 500 x 334 fits */
        paint_zoomed(&w->p, &cw, &ch);
        CHECK(cw <= w->p.vw && ch <= w->p.vh && shown(750, 500) == WHITE);
    }
    pw_close(w);
    gkf_reset(0, 0);
    CHECK(pw_open(w, at("two.bmp")) == 0);
    gkf_focus(1);
    run();
    /* the desktop's theme: the backdrop follows */
    for(i = 0; i < WIN_COLORS; i++) colors[i] = 0x101010u * (unsigned)(i % 15) + 0x0A0A0A;
    colors[WIN_COLOR_DARK] = 0x332211;
    gkf_set_theme(WIN_THEME_DUSK, colors);
    run();
    CHECK(w->p.backdrop == 0x332211);
    pw_close(w);
    gkf_reset(0, 0);
    CHECK(pw_open(w, at("notes.png")) == 0);
    gkf_focus(1);
    CHECK(run() && gk_box_open(w->ui) && w->p.w == PAINT_NEW_W && !strcmp(gkf_title(), "Untitled - Paint"));
    pw_close(w);
    gkf_reset(0, 0);
}

/**** what the review of 11/05 found ****/

static int put_file(void *ctx, const void *d, size_t n) {
    return fwrite(d, 1, n, (FILE *)ctx) == n ? 0 : -1;
}

/* a picture of 40 x 30, blue, as a PNG of 8 bits a sample: with alpha, its top left corner is not there */
static void png_with(const char *path, int alpha) {
    static unsigned pix[40 * 30];
    struct wimg m;
    FILE *f = fopen(path, "wb");
    int i;
    for(i = 0; i < 40 * 30; i++) pix[i] = 0x3060C0;
    if(alpha) pix[0] = WIMG_GONE;
    m.w = 40;
    m.h = 30;
    m.px = pix;
    wimg_save_png(&m, alpha, put_file, f, NULL, 0);
    fclose(f);
}

/* a file's bytes (malloc'ed), *n of them */
static unsigned char *bytes_of(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    unsigned char *b;
    long len;
    if(!f) return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (unsigned char *)malloc((size_t)len + 1);
    *n = fread(b, 1, (size_t)len, f);
    fclose(f);
    return b;
}

static int same_bytes(const char *path, const unsigned char *was, size_t n) {
    size_t m;
    unsigned char *b = bytes_of(path, &m);
    int same = b && m == n && !memcmp(b, was, n);
    free(b);
    return same;
}

/* every pixel of the picture in the file is solid (none see-through) */
static int solid_file(const char *path) {
    size_t n;
    unsigned char *b = bytes_of(path, &n);
    struct wimg m;
    char err[WIMG_ERR_MAX];
    int i, solid;
    if(!b || wimg_decode(b, n, &m, err, sizeof(err)) < 0) {
        free(b);
        return 0;
    }
    for(solid = 1, i = 0; i < m.w * m.h; i++) solid = solid && WIMG_SOLID(m.px[i]);
    wimg_free(&m);
    free(b);
    return solid;
}

/* a menu item's event, as the toolkit would hand it on: what the desktop's bar sends while a box is open the toolkit
   now greys (kit's stage 3), so this is how Paint's own guard is reached */
static int menu_event(int id) {
    struct gk_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = GK_EV_MENU;
    ev.id = id;
    return pw_event(w, &ev);
}

/* File > Open of path, saying Don't save when asked about changes */
static void open_through(const char *path) {
    key(15, WIN_MOD_CTRL);
    if(gk_asking(w->ui) == BOX_UNSAVED) {
        key(WIN_KEY_RIGHT, 0);
        key(13, 0);
    }
    gkf_dialog_answer(path);
    run();
}

static void review(void) {
    unsigned char *logo;
    size_t n;
    int x0, y0, x1, y1, x2, y2;
    long before;
    /* Critical's window: none (the model's test has Cut without the memory). Medium: the bar's menus while a box of
       Paint's is open do nothing; Esc on "Now choose the picture to open" opens no chooser */
    png_with(at("other.png"), 0);
    gkf_reset(0, 0);
    CHECK(pw_open(w, NULL) == 0);
    gkf_focus(1);
    run();
    key('b', 0);
    drag(10, 10, 60, 10);
    key(15, WIN_MOD_CTRL);
    key(13, 0);                                         /* Save: Save as, as it has no name */
    gkf_dialog_answer(at("mine.png"));
    run();
    CHECK(gk_asking(w->ui) == BOX_THEN_OPEN && !paint_modified(&w->p));
    CHECK(!gkf_menu_choose(MENU_CLEAR));                /* the toolkit greys the menus behind a box: refused */
    CHECK(menu_event(MENU_SELECT_ALL) && menu_event(MENU_CLEAR) && run());  /* and Paint ignores what came anyway */
    CHECK(gk_asking(w->ui) == BOX_THEN_OPEN && !paint_modified(&w->p) && canvas_count(BLACK) > 0 && !w->p.sel.on);
    key(27, 0);
    CHECK(!gk_box_open(w->ui) && !gkf_dialog());
    /* the canvas changed while the Open chooser was up (the bar works then): the question again before the picture
       chosen replaces it; Don't save then opens it */
    drag(10, 40, 60, 40);
    key(15, WIN_MOD_CTRL);
    key(WIN_KEY_RIGHT, 0);
    key(13, 0);                                         /* Don't save: the chooser */
    CHECK(gkf_dialog() && gkf_dialog()->kind == WIN_DLG_OPEN);
    CHECK(gkf_menu_choose(MENU_CLEAR) && run() && canvas_count(BLACK) == 0);
    gkf_dialog_answer(at("other.png"));
    run();
    CHECK(gk_asking(w->ui) == BOX_UNSAVED && w->p.w != 40);
    key(WIN_KEY_RIGHT, 0);
    key(13, 0);
    CHECK(!gk_box_open(w->ui) && w->p.w == 40 && !strcmp(gkf_title(), "other.png - Paint"));
    /* and Cancel then keeps the canvas, the picture unopened */
    drag(5, 5, 30, 5);
    key(15, WIN_MOD_CTRL);
    key(WIN_KEY_RIGHT, 0);
    key(13, 0);
    gkf_menu_choose(MENU_CLEAR);
    run();
    gkf_dialog_answer(at("mine.png"));
    run();
    CHECK(gk_asking(w->ui) == BOX_UNSAVED);
    key(27, 0);
    CHECK(!gk_box_open(w->ui) && w->p.w == 40 && paint_modified(&w->p) && !strcmp(gkf_title(), "*other.png - Paint"));
    /* New: the bar's Clear behind the size's box does nothing, and the new canvas comes without a question */
    key(14, WIN_MOD_CTRL);
    key(WIN_KEY_RIGHT, 0);
    key(13, 0);                                         /* Don't save */
    CHECK(gk_asking(w->ui) == BOX_NEW);
    CHECK(!gkf_menu_choose(MENU_CLEAR) && menu_event(MENU_CLEAR) && run() && gk_asking(w->ui) == BOX_NEW);
    key(1, WIN_MOD_CTRL);
    gkf_type("300 x 200\n");
    run();
    CHECK(!gk_box_open(w->ui) && w->p.w == 300 && w->p.h == 200 && !paint_modified(&w->p));
    /* Low: a drag cut short. The window loses the focus with the button held: the stroke ends there, and a move with
       no button once the focus is back draws nothing */
    key('b', 0);
    spot(20, 20, &x0, &y0);
    spot(60, 20, &x1, &y1);
    spot(250, 150, &x2, &y2);
    gkf_advance(1000);
    gkf_pointer(x0, y0, WIN_BUTTON_LEFT);
    gkf_pointer(x1, y1, WIN_BUTTON_LEFT);
    run();
    CHECK(w->p.drag && px(40, 20) == BLACK);
    gkf_focus(0);
    run();
    CHECK(!w->p.drag && !strcmp(paint_undo_what(&w->p), "Brush"));
    before = canvas_count(BLACK);
    gkf_focus(1);
    run();
    gkf_pointer(x2, y2, 0);
    run();
    CHECK(!w->p.drag && canvas_count(BLACK) == before && px(155, 85) == WHITE);     /* on the way to 250, 150 */
    /* a box opening over a drag (Ctrl+W, with changes): the drag ends as the box opens */
    gkf_advance(1000);
    gkf_pointer(x0, y0 + 40, WIN_BUTTON_LEFT);
    gkf_pointer(x1, y1 + 40, WIN_BUTTON_LEFT);
    run();
    CHECK(w->p.drag);
    key(23, WIN_MOD_CTRL);
    CHECK(gk_asking(w->ui) == BOX_UNSAVED && !w->p.drag);
    key(27, 0);
    before = canvas_count(BLACK);
    gkf_pointer(x2, y2, 0);
    run();
    CHECK(!w->p.drag && canvas_count(BLACK) == before && px(155, 105) == WHITE);
    /* Low: the desktop's clipboard pasted into text: control characters left out (C0 but the line ends, DEL, C1) */
    key('t', 0);
    click(20, 120);
    gkf_set_paste("a\x01" "b\x7f" "c\xc2\x85" "d\r\ne\tf\x1b\xc3\xa9");
    key(22, WIN_MOD_CTRL);
    CHECK(w->p.text.on && w->p.text.n == 8 && w->p.text.cell[0] == 'a' && w->p.text.cell[1] == 'b' &&
          w->p.text.cell[2] == 'c' && w->p.text.cell[3] == 'd' && w->p.text.cell[4] == PAINT_TEXT_NL &&
          w->p.text.cell[5] == 'e' && w->p.text.cell[6] == 'f' && w->p.text.cell[7] == 0x82);   /* the e acute kept */
    key(27, 0);
    /* High: a picture with transparency, said when it opens; Save over its file asks first, Save as... the first
       button (Enter's) and nothing replaced until Save */
    png_with(at("logo.png"), 1);
    logo = bytes_of(at("logo.png"), &n);
    open_through(at("logo.png"));
    CHECK(w->p.w == 40 && w->lost == PF_LOST_ALPHA && status_says("Opened logo.png, 40 x 30, its transparency on white"));
    key(19, WIN_MOD_CTRL);                              /* Ctrl+S, nothing changed */
    CHECK(gk_asking(w->ui) == BOX_LOSSY && same_bytes(at("logo.png"), logo, n));
    key(27, 0);
    CHECK(!gk_box_open(w->ui) && !gkf_dialog() && same_bytes(at("logo.png"), logo, n));
    key(19, WIN_MOD_CTRL);
    key(13, 0);                                         /* Enter: Save as... */
    CHECK(gkf_dialog() && gkf_dialog()->kind == WIN_DLG_SAVE && !strcmp(gkf_dialog()->name, "logo.png"));
    gkf_dialog_answer(at("logo2.png"));
    run();
    CHECK(exists(at("logo2.png")) && same_bytes(at("logo.png"), logo, n) && !strcmp(gkf_title(), "logo2.png - Paint"));
    key(19, WIN_MOD_CTRL);                              /* Paint's own file now: saved at once */
    CHECK(!gk_box_open(w->ui) && w->lost == 0 && status_says("Saved logo2.png"));
    open_through(at("logo.png"));
    key(19, WIN_MOD_CTRL);
    key(WIN_KEY_RIGHT, 0);
    key(13, 0);                                         /* Save: replaced, on white */
    CHECK(!gk_box_open(w->ui) && !same_bytes(at("logo.png"), logo, n) && solid_file(at("logo.png")) && w->lost == 0);
    free(logo);
    /* a PNG of 16 bits a sample: said, and asked about; Close with changes, Save: that question too, then closed */
    {
        size_t m;
        unsigned char *deep = bytes_of("tests/fixtures/wundertow/images/rgb16.png", &m);
        FILE *f = fopen(at("deep.png"), "wb");
        CHECK(deep && f && fwrite(deep, 1, m, f) == m);
        if(f) fclose(f);
        open_through(at("deep.png"));
        CHECK(w->lost == PF_LOST_DEPTH && status_says("Opened deep.png, 23 x 17, kept at 8 bits a channel"));
        key(19, WIN_MOD_CTRL);
        CHECK(gk_asking(w->ui) == BOX_LOSSY && same_bytes(at("deep.png"), deep, m));
        key(27, 0);
        drag(2, 2, 10, 2);
        gkf_close();
        CHECK(run() && gk_asking(w->ui) == BOX_UNSAVED);
        gkf_key(13, 0);                                 /* Save */
        CHECK(run() && gk_asking(w->ui) == BOX_LOSSY && same_bytes(at("deep.png"), deep, m));
        gkf_key(WIN_KEY_RIGHT, 0);
        gkf_key(13, 0);                                 /* Save: replaced, and closed */
        CHECK(!run() && !same_bytes(at("deep.png"), deep, m));
        free(deep);
    }
    pw_close(w);
    /* the second review's lows. The question before a save replaces the picture's file knows the file reached another
       way: through "..", or in another case (FAT's names ignore case); another file is saved at once */
    gkf_reset(0, 0);
    CHECK(pw_open(w, NULL) == 0);
    gkf_focus(1);
    run();
    png_with(at("logo4.png"), 1);
    logo = bytes_of(at("logo4.png"), &n);
    mkdir(at("sub"), 0755);
    open_through(at("logo4.png"));
    CHECK(w->lost == PF_LOST_ALPHA);
    key(19, WIN_MOD_CTRL | WIN_MOD_SHIFT);
    gkf_dialog_answer(at("sub/../logo4.png"));
    run();
    CHECK(gk_asking(w->ui) == BOX_LOSSY);
    key(27, 0);
    key(19, WIN_MOD_CTRL | WIN_MOD_SHIFT);
    gkf_dialog_answer(at("LOGO4.PNG"));
    run();
    CHECK(gk_asking(w->ui) == BOX_LOSSY);
    key(27, 0);
    CHECK(same_bytes(at("logo4.png"), logo, n));
    key(19, WIN_MOD_CTRL | WIN_MOD_SHIFT);
    gkf_dialog_answer(at("logo5.png"));
    run();
    CHECK(!gk_box_open(w->ui) && exists(at("logo5.png")) && same_bytes(at("logo4.png"), logo, n));
    free(logo);
    rmdir(at("sub"));
    /* the close button pressed again while its question is up: the question asked again, and its answer works (the
       first one's -1, for being replaced, is no one's Cancel) */
    drag(2, 2, 20, 2);
    gkf_close();
    CHECK(run() && gk_asking(w->ui) == BOX_UNSAVED);
    gkf_close();
    CHECK(run() && gk_asking(w->ui) == BOX_UNSAVED && w->then == PW_THEN_CLOSE);
    gkf_key(WIN_KEY_RIGHT, 0);
    gkf_key(13, 0);                                     /* Don't save: closed */
    CHECK(!run());
    pw_close(w);
    gkf_reset(0, 0);
}

static void tidy(void) {
    struct dirent *e;
    DIR *d = opendir(dir);
    while(d && (e = readdir(d))) {
        if(e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2]))) continue;
        unlink(at(e->d_name));
    }
    if(d) closedir(d);
    rmdir(dir);
}

int main(void) {
    const char *tmp = getenv("TMPDIR");
    setvbuf(stdout, NULL, _IOLBF, 0);
    snprintf(dir, sizeof(dir), "%s/wpaintw.XXXXXX", tmp && tmp[0] ? tmp : "/tmp");
    if(!mkdtemp(dir)) {
        printf("test_wpaint_window: no folder of its own: %s\n", strerror(errno));
        return 1;
    }
    window();
    tools();
    selection();
    view();
    colours();
    files();
    asking();
    starting();
    review();
    tidy();
    printf("test_wpaint_window: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
