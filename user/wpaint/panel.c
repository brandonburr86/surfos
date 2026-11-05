/*
SurfOS user program: wpaint
--------------------
File: panel.c   Date: 11/05/26
--------------------
Paint's two panels, areas it draws itself (gk.h has no tool palette or colour swatch), in the
desktop's colours and the way its toolbars light the chosen button: the theme's accent behind the
tool, size or style chosen, light buttons for the others.

The tool palette, at the left: the ten tools in two columns, each by an icon of 16 x 16 made for
Paint (pw_icon(): the text tool's is the console's A), then the chosen tool's options under a
line: the eight brush sizes for the brush, the eraser, lines and outlines; outlined or filled for
rectangles and ellipses; the text's scale, 1 to 8 times, for text.

The colours, under the canvas: the first colour over the second (a click swaps them), the 24 of
the palette and the 8 custom colours (the left button makes one the first colour, the right one
the second), Edit... (a colour typed as #RRGGBB or red, green, blue), and the first colour's code.

pw_tools_hit() and pw_colours_hit() say what is at a point, so that the window's clicks and the
drawing agree on every place.
*/
#include <stdio.h>
#include <string.h>
#include "window.h"

static const char *const icons[PAINT_TOOLS][16] = {
    {   /* pencil: its point at the bottom left */
        "................", "............##..", "...........####.", "..........####..",
        ".........####...", "........####....", ".......####.....", "......####......",
        ".....####.......", "....####........", "...####.........", "...###..........",
        "..##............", "..#.............", "................", "................" },
    {   /* brush: a handle, and its tuft */
        "................", ".............##.", "............##..", "...........##...",
        "..........##....", ".........##.....", "........##......", ".......##.......",
        ".......#........", "...####.........", "..######........", "..######........",
        "..######........", "..######........", ".######.........", ".#.............." },
    {   /* eraser: a block, from above and in front */
        "................", "................", "................", "................",
        "...############.", "..#..........##.", ".#..........#.#.", "############..#.",
        "#####......#..#.", "#####......#..#.", "#####......#.#..", "#####......##...",
        "############....", "................", "................", "................" },
    {   /* line */
        "................", "..............#.", ".............#..", "............#...",
        "...........#....", "..........#.....", ".........#......", "........#.......",
        ".......#........", "......#.........", ".....#..........", "....#...........",
        "...#............", "..#.............", ".#..............", "................" },
    {   /* rectangle */
        "................", "................", "................", ".##############.",
        ".#............#.", ".#............#.", ".#............#.", ".#............#.",
        ".#............#.", ".#............#.", ".#............#.", ".#............#.",
        ".##############.", "................", "................", "................" },
    {   /* ellipse */
        "................", "................", "................", "................",
        "....########....", "...#........#...", "..#..........#..", "..#..........#..",
        "..#..........#..", "..#..........#..", "...#........#...", "....########....",
        "................", "................", "................", "................" },
    {   /* fill: a pail, and a drop */
        "................", ".....####.......", "....#....#......", "...#......#.....",
        "..############..", "..#..........#..", "..#..........#..", "...#........#..#",
        "...#........#.##", "...#........#.##", "....#......#..#.", "....########....",
        "................", "................", "................", "................" },
    { NULL },                           /* text: the console's A (gw_glyph()) */
    {   /* picker: a dropper, its bulb at the top right */
        "................", "...........##...", "..........####..", ".........######.",
        "..........####..", ".........#.##...", "........#.#.....", ".......#.#......",
        "......#.#.......", ".....#.#........", "....#.#.........", "...#.#..........",
        "..#.#...........", "..##............", ".#..............", "................" },
    {   /* select: a dashed rectangle */
        "................", "................", ".###..###..####.", ".#............#.",
        ".#............#.", "................", "................", ".#............#.",
        ".#............#.", ".#............#.", "................", "................",
        ".#............#.", ".####..###..###.", "................", "................" },
};

const char *pw_icon(int tool) {
    static char flat[16 * 16 + 1];
    int r;
    if(tool < 0 || tool >= PAINT_TOOLS || !icons[tool][0]) return NULL;
    for(r = 0; r < 16; r++) memcpy(flat + r * 16, icons[tool][r], 16);
    flat[16 * 16] = 0;
    return flat;
}

/**** where things are ****/

static void tool_box(int tool, int *x, int *y) {
    *x = PW_TOOL_X0 + tool % 2 * PW_TOOL_STEP;
    *y = PW_TOOL_Y0 + tool / 2 * PW_TOOL_STEP;
}

static void option_box(int i, int y0, int *x, int *y) {
    *x = PW_TOOL_X0 + i % 2 * PW_TOOL_STEP;
    *y = y0 + i / 2 * PW_OPT_STEP;
}

static int inside(int x, int y, int bx, int by, int bw, int bh) {
    return x >= bx && y >= by && x < bx + bw && y < by + bh;
}

/* the options the tool has, and where its sizes start */
static int has_sizes(int tool) {
    return tool == PAINT_BRUSH || tool == PAINT_ERASER || tool == PAINT_LINE || tool == PAINT_RECT ||
           tool == PAINT_ELLIPSE;
}

static int has_style(int tool) {
    return tool == PAINT_RECT || tool == PAINT_ELLIPSE;
}

static int sizes_y0(int tool) {
    return has_style(tool) ? PW_OPT_Y0 + PW_OPT_STEP + 4 : PW_OPT_Y0;
}

int pw_tools_hit(const struct paint *p, int x, int y, int *value) {
    int i, bx, by;
    for(i = 0; i < PAINT_TOOLS; i++) {
        tool_box(i, &bx, &by);
        if(inside(x, y, bx, by, PW_TOOL_B, PW_TOOL_B)) {
            *value = i;
            return PW_HIT_TOOL;
        }
    }
    if(has_style(p->tool))
        for(i = 0; i < 2; i++) {
            option_box(i, PW_OPT_Y0, &bx, &by);
            if(inside(x, y, bx, by, PW_OPT_W, PW_OPT_H)) {
                *value = i;
                return PW_HIT_STYLE;
            }
        }
    if(has_sizes(p->tool))
        for(i = 0; i < PAINT_SIZES; i++) {
            option_box(i, sizes_y0(p->tool), &bx, &by);
            if(inside(x, y, bx, by, PW_OPT_W, PW_OPT_H)) {
                *value = i;
                return PW_HIT_SIZE;
            }
        }
    if(p->tool == PAINT_TEXT)
        for(i = 0; i < PAINT_TEXT_SCALE; i++) {
            option_box(i, PW_OPT_Y0, &bx, &by);
            if(inside(x, y, bx, by, PW_OPT_W, PW_OPT_H)) {
                *value = i + 1;
                return PW_HIT_SCALE;
            }
        }
    return PW_HIT_NONE;
}

int pw_colours_hit(const struct paint *p, int x, int y, int *value) {
    int i, bx, by;
    if(inside(x, y, 6, 4, 34, 34)) return PW_HIT_SWAP;  /* the first colour over the second */
    for(i = 0; i < PAINT_COLOURS; i++) {
        bx = PW_PALETTE_X + i % 12 * PW_SWATCH_STEP;
        by = 4 + i / 12 * PW_SWATCH_STEP;
        if(inside(x, y, bx, by, PW_SWATCH, PW_SWATCH)) {
            *value = (int)paint_palette[i];
            return PW_HIT_COLOUR;
        }
    }
    for(i = 0; i < PAINT_CUSTOM; i++) {
        bx = PW_CUSTOM_X + i % 4 * PW_SWATCH_STEP;
        by = 4 + i / 4 * PW_SWATCH_STEP;
        if(inside(x, y, bx, by, PW_SWATCH, PW_SWATCH)) {
            *value = i;
            return PW_HIT_CUSTOM;
        }
    }
    if(inside(x, y, PW_EDIT_X, 10, PW_EDIT_W, 24)) return PW_HIT_EDIT;
    (void)p;
    return PW_HIT_NONE;
}

/**** drawing ****/

static void icon(struct gw *win, int x, int y, int tool, unsigned ink) {
    int r, c;
    if(tool == PAINT_TEXT) {
        gw_glyph(win, x + 4, y, 'A', ink, 1);
        return;
    }
    for(r = 0; r < 16; r++)
        for(c = 0; c < 16; c++)
            if(icons[tool][r][c] == '#') gw_rect(win, x + c, y + r, 1, 1, ink);
}

/* a button of the palette: the accent when it is the one chosen */
static void key_cap(struct gw *win, const struct gk_theme *t, int x, int y, int w, int h, int lit) {
    gw_rect(win, x, y, w, h, lit ? t->teal : t->shell);
    gw_frame(win, x, y, w, h, lit ? t->teal : t->line);
}

void pw_draw_tools(struct gk_widget *area, struct gw *win, const struct gk_view *v, void *data) {
    const struct pwin *pw = data;
    const struct paint *p = &pw->p;
    const struct gk_theme *t = gk_theme(pw->ui);
    char label[8];
    int i, x, y, lit;
    (void)area;
    gw_rect(win, v->cx, v->cy, v->cw, v->ch, t->band);
    gw_rect(win, v->w - 1, 0, 1, v->h, t->line);        /* the edge toward the canvas */
    for(i = 0; i < PAINT_TOOLS; i++) {
        tool_box(i, &x, &y);
        lit = i == p->tool;
        key_cap(win, t, x, y, PW_TOOL_B, PW_TOOL_B, lit);
        icon(win, x + (PW_TOOL_B - 16) / 2, y + (PW_TOOL_B - 16) / 2, i, lit ? t->on_teal : t->ink);
    }
    gw_rect(win, PW_TOOL_X0, PW_OPT_Y0 - 4, 2 * PW_TOOL_STEP - 2, 1, t->line);
    if(has_style(p->tool))
        for(i = 0; i < 2; i++) {
            option_box(i, PW_OPT_Y0, &x, &y);
            lit = i == p->filled;
            key_cap(win, t, x, y, PW_OPT_W, PW_OPT_H, lit);
            if(i) gw_rect(win, x + 6, y + 5, 12, 8, lit ? t->on_teal : t->ink);
            else gw_frame(win, x + 6, y + 5, 12, 8, lit ? t->on_teal : t->ink);
        }
    if(has_sizes(p->tool))
        for(i = 0; i < PAINT_SIZES; i++) {
            option_box(i, sizes_y0(p->tool), &x, &y);
            lit = i == p->size;
            key_cap(win, t, x, y, PW_OPT_W, PW_OPT_H, lit);
            snprintf(label, sizeof(label), "%d", paint_sizes[i]);
            gw_text(win, x + (PW_OPT_W - gw_text_width(label)) / 2, y + 1, label, lit ? t->on_teal : t->ink);
        }
    if(p->tool == PAINT_TEXT)
        for(i = 0; i < PAINT_TEXT_SCALE; i++) {
            option_box(i, PW_OPT_Y0, &x, &y);
            lit = i + 1 == p->text_scale;
            key_cap(win, t, x, y, PW_OPT_W, PW_OPT_H, lit);
            snprintf(label, sizeof(label), "%dx", i + 1);
            gw_text(win, x + (PW_OPT_W - gw_text_width(label)) / 2, y + 1, label, lit ? t->on_teal : t->ink);
        }
}

static void swatch(struct gw *win, const struct gk_theme *t, int x, int y, int side, unsigned rgb) {
    gw_rect(win, x, y, side, side, rgb);
    gw_frame(win, x, y, side, side, t->frame);
}

void pw_draw_colours(struct gk_widget *area, struct gw *win, const struct gk_view *v, void *data) {
    const struct pwin *pw = data;
    const struct paint *p = &pw->p;
    const struct gk_theme *t = gk_theme(pw->ui);
    char code[16];
    int i;
    (void)area;
    gw_rect(win, v->cx, v->cy, v->cw, v->ch, t->band);
    gw_rect(win, 0, 0, v->w, 1, t->line);               /* the edge toward the canvas */
    swatch(win, t, 16, 14, 24, p->bg);                  /* the second colour, under the first */
    swatch(win, t, 6, 4, 24, p->fg);
    for(i = 0; i < PAINT_COLOURS; i++)
        swatch(win, t, PW_PALETTE_X + i % 12 * PW_SWATCH_STEP, 4 + i / 12 * PW_SWATCH_STEP, PW_SWATCH, paint_palette[i]);
    for(i = 0; i < PAINT_CUSTOM; i++)
        swatch(win, t, PW_CUSTOM_X + i % 4 * PW_SWATCH_STEP, 4 + i / 4 * PW_SWATCH_STEP, PW_SWATCH, p->custom[i]);
    gw_button(win, PW_EDIT_X, 10, PW_EDIT_W, 24, "Edit\xe2\x80\xa6", t->shell, t->ink);
    paint_colour_text(p->fg, code, sizeof(code));
    gw_text(win, PW_CODE_X, 14, code, t->muted);
}
