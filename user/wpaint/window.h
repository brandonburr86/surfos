/*
SurfOS user program: wpaint
--------------------
File: window.h  Date: 11/05/26
--------------------
Paint's window (window.c, panel.c), on the toolkit (gk.h): the tool palette at the left, the canvas
with its scroll bars, the colours under them and a status bar; menus in the desktop's bar; the
desktop's choosers for Open and Save as. Its logic is apart from its loop (main.c), so that
tests/host/test_wpaint_window.c drives it on the fake desktop (tests/host/gk_fake.c) as a person
would, the pattern of track kit's wkit.
*/
#ifndef _WPAINT_WINDOW_H
#define _WPAINT_WINDOW_H

#include <gk.h>
#include "paint.h"
#include "files.h"

#define PW_W         800                /* the window's content, as asked: the desktop keeps it within its room */
#define PW_H         440
#define PW_MIN_W     480                /* its least: the panels and a canvas still fit (a 640 x 400 desktop's room is
                                           638 x 317) */
#define PW_MIN_H     (PW_TOOLS_H + PW_COLOURS_H + GK_STATUS_H)
#define PW_STRIP     16                 /* the canvas is rendered this many rows at a time */
#define PW_PATH      WIN_PATH_MAX

/* the tool palette (panel.c): two columns of tool buttons, then the chosen tool's options */
#define PW_TOOLS_W   60
#define PW_TOOL_B    24                 /* a tool's button, square */
#define PW_TOOL_STEP 26
#define PW_TOOL_X0   5
#define PW_TOOL_Y0   4
#define PW_OPT_Y0    140                /* the options, under a line at 136 */
#define PW_OPT_W     24
#define PW_OPT_H     18
#define PW_OPT_STEP  20
#define PW_TOOLS_H   246                /* the palette's height: a rectangle's two rows of options and four of sizes */
/* the colours (panel.c): the first over the second, the palette, the custom colours, Edit..., the first's code */
#define PW_COLOURS_H 44
#define PW_SWATCH    16
#define PW_SWATCH_STEP 18
#define PW_PALETTE_X 48
#define PW_CUSTOM_X  (PW_PALETTE_X + 12 * PW_SWATCH_STEP + 8)
#define PW_EDIT_X    (PW_CUSTOM_X + 4 * PW_SWATCH_STEP + 8)
#define PW_EDIT_W    56
#define PW_CODE_X    (PW_EDIT_X + PW_EDIT_W + 10)

/* what the panels' places are (panel.c's hit tests) */
#define PW_HIT_NONE   0
#define PW_HIT_TOOL   1                 /* value: the tool */
#define PW_HIT_SIZE   2                 /* value: an index of paint_sizes[] */
#define PW_HIT_STYLE  3                 /* value: 0 outlined, 1 filled */
#define PW_HIT_SCALE  4                 /* value: the text's scale */
#define PW_HIT_COLOUR 5                 /* value: the colour */
#define PW_HIT_CUSTOM 6                 /* value: the custom colour's index */
#define PW_HIT_SWAP   7                 /* the first and the second colour */
#define PW_HIT_EDIT   8                 /* Edit... */

/* the ids of what the program hears from */
enum {
    ID_TOOLS = 1, ID_CANVAS, ID_COLOURS,
    BOX_UNSAVED = 100, BOX_NEW, BOX_RESIZE, BOX_COLOUR, BOX_MESSAGE, BOX_THEN_OPEN, BOX_KEYS, BOX_ABOUT,
    MENU_NEW = 200, MENU_OPEN, MENU_SAVE, MENU_SAVE_AS, MENU_CLOSE,
    MENU_UNDO, MENU_REDO, MENU_CUT, MENU_COPY, MENU_PASTE, MENU_DELETE, MENU_SELECT_ALL, MENU_DESELECT,
    MENU_RESIZE, MENU_CLEAR, MENU_ZOOM_IN, MENU_ZOOM_OUT, MENU_ACTUAL, MENU_FIT, MENU_ZOOMS,
    MENU_FILLED, MENU_SIZES, MENU_SCALES, MENU_EDIT_COLOUR, MENU_SWAP, MENU_DEFAULT_COLOURS, MENU_KEYS, MENU_ABOUT,
    MENU_ZOOM = 250,                    /* + a zoom level */
    MENU_TOOL = 270,                    /* + a tool */
    MENU_SIZE = 290,                    /* + an index of paint_sizes[] */
    MENU_SCALE = 300,                   /* + a text scale - 1 */
    CHOOSE_OPEN = 400, CHOOSE_SAVE,
};

/* what waits on the unsaved changes' question, and on a Save as it led to */
#define PW_THEN_NOTHING 0
#define PW_THEN_NEW     1
#define PW_THEN_OPEN    2
#define PW_THEN_CLOSE   3

/* what the panels last showed, to paint them again when it changes */
struct pw_shown { int tool, size, filled, scale; unsigned fg, bg, custom[PAINT_CUSTOM]; };

struct pwin {
    struct gk *ui;
    struct gk_widget *tools, *canvas, *colours, *status;
    struct paint p;
    char path[PW_PATH];                 /* the file the picture came from or was saved as; "" for a new one */
    char title[WIN_TITLE_MAX];
    int then;                           /* PW_THEN_*: what waits on the unsaved changes' question */
    int save_then;                      /* a Save as asked for by that question: then goes on once it is saved */
    int closing;                        /* the window is to close: pw_event() says 0 */
    int panning, pan_x, pan_y, pan_sx, pan_sy;  /* the middle button drags the view */
    int cw, ch;                         /* the zoomed canvas the scroll bars were last given */
    struct pw_shown shown;
    char why[PF_WHY], kept[PW_PATH];
    unsigned strip[PAINT_VIEW_MAX * PW_STRIP];
};

int pw_open(struct pwin *w, const char *file);          /* 0, or -1 with errno (ENODEV: no desktop here) */
int pw_event(struct pwin *w, const struct gk_event *ev);    /* 0 once the window is to close */
void pw_close(struct pwin *w);

/* panel.c */
int pw_tools_hit(const struct paint *p, int x, int y, int *value);
int pw_colours_hit(const struct paint *p, int x, int y, int *value);
void pw_draw_tools(struct gk_widget *area, struct gw *win, const struct gk_view *v, void *data);
void pw_draw_colours(struct gk_widget *area, struct gw *win, const struct gk_view *v, void *data);
const char *pw_icon(int tool);                          /* a tool's 16 x 16 icon, '#' its ink: rows of 16 */

#endif
