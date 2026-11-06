/*
SurfOS user program: wpaint
--------------------
File: paint.h   Date: 11/05/26
--------------------
Paint's model: a canvas of 32-bit pixels, the tools that draw on it, the rectangular selection
with Paint's own clipboard, the history of changes, the canvas's size, the colours, and the view
of the canvas at a zoom with its scrolling, rendered into a plain buffer of pixels. Nothing here
makes a system call or draws in a window: main.c (the window, libgui) and files.c (the pictures,
libsurf's codecs) hand in what happens, in canvas coordinates, and tests/host/test_wpaint.c runs
the model on the host as it is.

A pixel is 0x00RRGGBB in 32 bits, as a window's pixels are. Paint has no transparency: a picture
opened with some is put on white (files.c).

A canvas is at most what libsurf's codecs read and write (image.h: 4096 pixels a side and
16,000,000 in all), so that every canvas Paint makes can be saved and opened again.

Memory is counted. Every large block (the canvas, the history's tiles, the selection's and the
clipboard's pixels, a flood fill's work) is taken through pm_alloc() against PAINT_MEM_MAX. The
largest canvas is 64,000,000 bytes by itself; Paint keeps at most twice that and 4 MB, the canvas
and in what is left the history, the change under way, the selection and the clipboard, so that
on the largest canvas a new size, a selection of all of it, or a picture opened in its place
still fit beside it. When a block would go past the bound, the oldest steps of the history go
first; if that is not enough, what asked is refused and the status line says why. The history itself keeps
at most PAINT_UNDO_MAX bytes and PAINT_UNDO_STEPS steps, the oldest dropped first; a single
change larger than that is made but cannot be undone, and the whole history is dropped with it
(as Notepad's history is by a paste too large to keep).

The history saves changed regions, not whole canvases: the canvas is cut into tiles of
PAINT_TILE x PAINT_TILE pixels, and the first time a change touches a tile its pixels are saved,
run-length coded when that is smaller (a blank tile is a few bytes). Every write to the canvas
goes through fill_span() or copy_span() (ppriv.h), which save the tiles first. Undo puts the saved
pixels back and keeps those it replaced, which Redo puts back in turn.
*/
#ifndef _WPAINT_PAINT_H
#define _WPAINT_PAINT_H

#include <stddef.h>
#include <image.h>

#define PAINT_SIDE_MAX   WIMG_SIDE_MAX  /* a canvas's side, at most (4096) */
#define PAINT_PIXELS_MAX WIMG_PIXELS_MAX    /* and its pixels (16,000,000: 64,000,000 bytes) */
#define PAINT_MEM_MAX    (2u * 4u * PAINT_PIXELS_MAX + (4u << 20))     /* all Paint keeps (above): 132,194,304 */
#define PAINT_UNDO_MAX   (32u << 20)    /* the history's bytes, the change under way included */
#define PAINT_UNDO_STEPS 100            /* the history's steps */
#define PAINT_TILE       64             /* the history saves tiles of 64 x 64 pixels (16 KB at most) */
#define PAINT_NEW_W      640            /* a new canvas */
#define PAINT_NEW_H      400
#define PAINT_PAPER      0xFFFFFFu      /* a new canvas's colour, and what a transparent pixel is put on */
#define PAINT_FILL_STACK 16384          /* a flood fill's pending runs (16 bytes each); past it, it scans again */
#define PAINT_FAR        (2 * PAINT_SIDE_MAX)   /* a drag's points are kept within this of the canvas */
#define PAINT_TEXT_MAX   255            /* characters of text being typed, its line ends included */
#define PAINT_TEXT_LINES 16
#define PAINT_TEXT_SCALE 8              /* text is the console's 8 x 16 characters, 1 to 8 times */
#define PAINT_SIZES      8              /* brush sizes (paint_sizes[]) */
#define PAINT_COLOURS    24             /* the palette (paint_palette[]) */
#define PAINT_CUSTOM     8              /* custom colours, the newest replacing the oldest */
#define PAINT_MSG        96
#define PAINT_WHAT       16

/* the tools */
enum { PAINT_PENCIL, PAINT_BRUSH, PAINT_ERASER, PAINT_LINE, PAINT_RECT, PAINT_ELLIPSE, PAINT_FILL, PAINT_TEXT,
       PAINT_PICKER, PAINT_SELECT, PAINT_TOOLS };

/* the buttons and the modifier a press or a move carries */
#define PAINT_LEFT  1
#define PAINT_RIGHT 2
#define PAINT_SHIFT 1                   /* lines at 45 degrees, squares and circles; arrows ten pixels */

/* the keys that are no bytes (surfos/win.h's WIN_KEY_*, as clock.h has them) */
#define PAINT_KEY_UP     0x100
#define PAINT_KEY_DOWN   0x101
#define PAINT_KEY_LEFT   0x102
#define PAINT_KEY_RIGHT  0x103
#define PAINT_KEY_DELETE 0x108

/* the zoom levels: from 1/8 to 32 times (view.c) */
#define PAINT_ZOOMS      14
#define PAINT_ZOOM_1     4              /* 100% */
#define PAINT_VIEW_MAX   4096           /* the view's width, at most (a window's rows are at most WIN_STRIDE_MAX) */

/* pixels the clipboard and a floating selection share; never changed once made */
struct pbuf {
    int refs;
    int w, h;
    unsigned *px;                       /* w x h, rows of w */
};

/* a tile's pixels as they were: n words, the pixels themselves (raw) or (count, colour) pairs */
struct ptile {
    int t;                              /* its number: row * tiles across + column */
    int raw;
    unsigned n;
    unsigned *data;
};

/* a step of the history: the tiles a change touched, as they were (or, once undone, as they were after it) */
#define PSTEP_TILES  0
#define PSTEP_CANVAS 1                  /* a new size: every tile of the canvas of w x h */
struct pstep {
    char what[PAINT_WHAT];              /* "Brush", "Fill": the Edit menu's Undo names it */
    int kind;
    int w, h;                           /* the canvas the tiles belong to */
    int ntiles, cap;
    struct ptile *tiles;
    unsigned char *saved;               /* while it is open: which tiles it holds already, a bit each */
    size_t bytes;                       /* what it holds: its tiles, their table and the bits */
    int lost;                           /* while it is open: it could not keep what it changes */
    unsigned before, after;             /* the states it goes between (paint_modified()) */
};

/* a line, a rectangle or an ellipse being dragged: shown by the view, drawn when the button is let go */
struct pshape {
    int on;
    int tool;                           /* PAINT_LINE, PAINT_RECT, PAINT_ELLIPSE */
    int x0, y0, x1, y1;                 /* a line's ends; a rectangle's or an ellipse's corners, in order */
    int size;                           /* the line's width and the outline's, in pixels */
    int filled;
    unsigned colour;
    int top, bottom;                    /* the canvas rows it covers (a line's rows are in rowl[], rowr[]) */
};

/* the selection */
struct psel {
    int on;
    int x, y, w, h;                     /* its rectangle; while it floats it may reach past the canvas */
    struct pbuf *buf;                   /* its pixels while it floats, lifted from the canvas or pasted */
    int lifted;                         /* it floats because it was lifted (from_x, from_y), not pasted */
    int from_x, from_y;
    int grab_x, grab_y;                 /* a drag that moves it: the press, from its corner */
};

/* text being typed: the console's characters (code page 437), PAINT_TEXT_NL ending a line */
#define PAINT_TEXT_NL 0x100
struct ptext {
    int on;
    int x, y;                           /* its top left on the canvas */
    int n;
    unsigned short cell[PAINT_TEXT_MAX];
    unsigned char utf8[4];              /* a character's bytes still to come */
    int have, want;
};

struct paint {
    /* the canvas */
    int w, h;
    unsigned *px;
    int tiles_x, tiles_y;
    int *rowl, *rowr;                   /* a line's extent in each row (draw.c), as tall as the canvas */
    size_t mem;                         /* bytes held through pm_alloc() */
    size_t mem_max;                     /* PAINT_MEM_MAX (the tests make it small) */
    /* what changed since the window last drew it: canvas pixels (inclusive), or the whole view */
    int dirty, dx0, dy0, dx1, dy1;
    int view_dirty;
    /* the history: steps[0 .. cur) can be undone (the newest last), steps[cur .. nsteps) redone */
    struct pstep *steps[PAINT_UNDO_STEPS];
    int nsteps, cur;
    struct pstep *open;                 /* the change under way */
    struct pstep nostep;                /* the change under way when not even a step could be had: lost */
    size_t undo_bytes;                  /* every step's, the open one's too */
    size_t undo_max;                    /* PAINT_UNDO_MAX (the tests make it small) */
    unsigned state, clean, serial;      /* the canvas's state; the state last saved or opened */
    /* the tools */
    int tool, back_tool;                /* back_tool: where the colour picker goes back to */
    int size;                           /* paint_sizes[size]: the brush, the eraser, lines and outlines */
    int filled;                         /* rectangles and ellipses: outlined 0, filled 1 */
    int text_scale;
    unsigned fg, bg;                    /* the first colour (the left button) and the second (the right) */
    unsigned custom[PAINT_CUSTOM];
    int custom_next;
    /* a drag: the button held (0: none), where it started and where it is */
    int drag;
    int ax, ay, bx, by;
    unsigned drag_colour;
    int moving;                         /* the selection tool's drag moves the selection */
    struct pshape shape;
    struct psel sel;
    struct pbuf *clip;                  /* Paint's clipboard */
    struct ptext text;
    /* the view: the zoom (an index of view.c's levels), the area the canvas is shown in, its scroll in the
       zoomed canvas's pixels */
    int zoom, vw, vh, sx, sy;
    unsigned backdrop;                  /* the view around the canvas */
    /* the pointer over the canvas, for the status line (at_on 0: elsewhere) */
    int at_on, at_x, at_y;
    char msg[PAINT_MSG];                /* what the last action said, until the next one */
    int fill_stack;                     /* PAINT_FILL_STACK (the tests make it small) */
    long fill_rows;                     /* the rows the last fill read again after its stack was full (its work) */
};

extern const int paint_sizes[PAINT_SIZES];
extern const unsigned paint_palette[PAINT_COLOURS];
extern const char *const paint_tool_names[PAINT_TOOLS];

/* the console's 8 x 16 font (driver/font8x16.c, which libsurf builds): code page 437's characters, a byte a row,
   the leftmost pixel the highest bit */
extern const unsigned char font8x16[256][16];

/* the model: a white canvas of w x h (PAINT_NEW_W x PAINT_NEW_H for 0): 0, or -1 (no memory, or a size that is
   none: 1 to PAINT_SIDE_MAX each way, PAINT_PIXELS_MAX in all); paint_free() lets everything go */
int paint_init(struct paint *p, int w, int h);
void paint_free(struct paint *p);

/* a new white canvas, or a picture opened: the history starts again, the clipboard stays. 0, or -1 with
   p->msg saying why and the canvas as it was. paint_adopt() takes px (w x h, from malloc()) on success only */
int paint_new(struct paint *p, int w, int h);
int paint_adopt(struct paint *p, unsigned *px, int w, int h);
int paint_resize(struct paint *p, int w, int h);       /* the canvas's size, from its top left: the history keeps it */
int paint_clear(struct paint *p);                       /* all of it the second colour */

/* the tools, in canvas coordinates: a drag goes on past the canvas's edges. Each returns 1 when something
   changed (pixels, the view, the colours, the status line), 0 otherwise */
void paint_set_tool(struct paint *p, int tool);
int paint_press(struct paint *p, int x, int y, int button, int mods);
int paint_move(struct paint *p, int x, int y, int mods);   /* with a button held: the drag; without: hovering */
int paint_release(struct paint *p, int x, int y, int mods);
int paint_leave(struct paint *p);                       /* the pointer left the canvas */
int paint_key(struct paint *p, int key, int mods);      /* text typed, the selection's keys; 0: not Paint's */
int paint_typing(const struct paint *p);                /* text is being typed: the keys are the text's */
void paint_put_down(struct paint *p);                   /* what floats and what is typed goes onto the canvas */
int paint_cancel(struct paint *p);                      /* Esc: text dropped, the selection put down and gone */

/* the selection and the clipboard */
int paint_select_all(struct paint *p);
int paint_copy(struct paint *p);
int paint_cut(struct paint *p);
int paint_paste(struct paint *p);
int paint_delete(struct paint *p);
int paint_can_paste(const struct paint *p);
int paint_has_selection(const struct paint *p);

/* the history */
int paint_undo(struct paint *p);
int paint_redo(struct paint *p);
int paint_can_undo(const struct paint *p);
int paint_can_redo(const struct paint *p);
const char *paint_undo_what(const struct paint *p);     /* the step Undo would undo ("" for none) */
const char *paint_redo_what(const struct paint *p);
int paint_modified(const struct paint *p);              /* changed since it was opened, saved or made */
void paint_saved(struct paint *p);                      /* it was saved as it is */

/* the colours: parse "#RRGGBB", "RRGGBB", "#RGB" or "R, G, B" (0-255): 0, or -1 */
int paint_parse_colour(const char *s, unsigned *rgb);
void paint_colour_text(unsigned rgb, char *out, int n);    /* "#RRGGBB" */
void paint_add_custom(struct paint *p, unsigned rgb);   /* the newest custom colour, and the first colour */
void paint_swap_colours(struct paint *p);
void paint_set_colour(struct paint *p, int second, unsigned rgb);  /* the first colour (second 0) or the second */
void paint_set_size(struct paint *p, int size);         /* an index of paint_sizes[] */
void paint_set_filled(struct paint *p, int filled);
void paint_set_text_scale(struct paint *p, int scale);  /* 1 to PAINT_TEXT_SCALE */

/* the view (view.c): the area the canvas is shown in (vw x vh); the zoom, kept about the view's point ax, ay;
   scrolling, held within the zoomed canvas; the canvas's pixel under a view's point (outside it too) and the
   view's point where a canvas's pixel starts */
void paint_view_size(struct paint *p, int vw, int vh);
int paint_zoom_to(struct paint *p, int zoom, int ax, int ay);
int paint_zoom_by(struct paint *p, int steps, int ax, int ay);
int paint_zoom_fit(struct paint *p);                    /* the largest level that shows it whole, at most 100% */
int paint_scroll_to(struct paint *p, int sx, int sy);
int paint_scroll_by(struct paint *p, int dx, int dy);
void paint_zoomed(const struct paint *p, int *cw, int *ch);   /* the zoomed canvas's size */
void paint_origin(const struct paint *p, int *ox, int *oy);   /* where it starts in the view when it is smaller */
void paint_zoom_ratio(int zoom, int *num, int *den);
int paint_zoom_percent(int zoom);
void paint_to_canvas(const struct paint *p, int vx, int vy, int *cx, int *cy);
void paint_to_view(const struct paint *p, int cx, int cy, int *vx, int *vy);
/* the view's pixels in its rectangle x, y, w, h (clipped to the view), into dst, which is that rectangle's top left,
   in rows of stride: the canvas, what floats, the shape or the text under way, and the selection's outline */
void paint_render(const struct paint *p, unsigned *dst, int stride, int x, int y, int w, int h);
/* what changed, in the view's coordinates, since paint_drawn(): 0 for nothing */
int paint_damage(const struct paint *p, int *x, int *y, int *w, int *h);
void paint_drawn(struct paint *p);

/* the status line: the last message or the tool's hint; and where the pointer is, the selection, the size and
   the zoom */
void paint_hint(const struct paint *p, char *out, int n);
void paint_where(const struct paint *p, char *out, int n);

#endif
