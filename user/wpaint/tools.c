/*
SurfOS user program: wpaint
--------------------
File: tools.c   Date: 11/05/26
--------------------
What the tools do with the pointer and the keys, in canvas coordinates. The left button draws in
the first colour and the right one in the second; the eraser paints the second colour. The
pencil, the brush and the eraser draw while the button is held, a step of the history from press
to release. A line, a rectangle and an ellipse are shown while they are dragged (the view draws
p->shape over the canvas) and drawn when the button is let go. The fill fills at the click, the
colour picker takes the colour clicked into the first colour (or the second) and goes back to the
tool before it, and the text tool starts text where it is clicked.

The selection: a drag with the selection tool marks a rectangle of the canvas; a drag that starts
inside it lifts its pixels (they float, and where they were becomes the second colour) and moves
them, as the arrows do. What floats is put down when another tool is chosen, at a click outside
it, at Enter or Esc, before a save, or when anything else draws: one step of the history from the
lift to there. Undo before that puts it back. Copy and Cut keep its pixels on Paint's own
clipboard; Paste floats them at the top left of the view, until they are put down. The clipboard
and what floats share their pixels (struct pbuf), which never change once made, so a paste takes
no memory of its own.

Text is typed into p->text, shown by the view and drawn into the canvas when it is put down (a
click elsewhere, another tool, or anything that puts down what floats); Esc or Undo drops it.
Keys are bytes, the characters beyond ASCII as UTF-8, each shown as the console's character
(code page 437) or '?' when it has none.
*/
#include <stdio.h>
#include <string.h>
#include <surfos/cp437.h>
#include "paint.h"
#include "ppriv.h"

/* the palette: black to white and the desktop's paper; the desktop's navy, teal, sky, foam, coral and sand; and
   twelve hues chosen for it */
const unsigned paint_palette[PAINT_COLOURS] = {
    0x000000, 0x404040, 0x808080, 0xC0C0C0, 0xFFFFFF, 0xF4F1E6,
    0x102E3B, 0x197D83, 0x659ECB, 0xB8E9D9, 0xEE906D, 0xD0B056,
    0xC03030, 0xE07830, 0xF0D040, 0x40A040, 0x90D060, 0x3060C0,
    0x40B0D0, 0x8040B0, 0xE070A0, 0x806040, 0x503820, 0xF8C8A8,
};

const char *const paint_tool_names[PAINT_TOOLS] = { "Pencil", "Brush", "Eraser", "Line", "Rectangle", "Ellipse",
                                                    "Fill", "Text", "Picker", "Select" };

static int on_canvas(const struct paint *p, int x, int y) {
    return x >= 0 && y >= 0 && x < p->w && y < p->h;
}

/* a drag's point, kept within PAINT_FAR of the canvas: the shapes' arithmetic stays within its bounds */
static void far(const struct paint *p, int *x, int *y) {
    if(*x < -PAINT_FAR) *x = -PAINT_FAR;
    if(*y < -PAINT_FAR) *y = -PAINT_FAR;
    if(*x > p->w - 1 + PAINT_FAR) *x = p->w - 1 + PAINT_FAR;
    if(*y > p->h - 1 + PAINT_FAR) *y = p->h - 1 + PAINT_FAR;
}

/* the selection's rectangle and the outline around it, for the window to draw again */
static void mark_sel(struct paint *p) {
    if(p->sel.on) mark(p, p->sel.x - 1, p->sel.y - 1, p->sel.x + p->sel.w, p->sel.y + p->sel.h);
}

static void mark_text(struct paint *p) {
    int w, h, s = p->text_scale;
    if(!p->text.on) return;
    text_size(&p->text, s, &w, &h);
    mark(p, p->text.x - 1, p->text.y - 1, p->text.x + w + 8 * s, p->text.y + h);   /* the cursor after it too */
}

static void mark_shape(struct paint *p) {
    int x0, y0, x1, y1;
    if(!p->shape.on) return;
    shape_bounds(&p->shape, &x0, &y0, &x1, &y1);
    mark(p, x0, y0, x1, y1);
}

/**** what floats ****/

/* the selection's pixels lifted off the canvas: they float, and where they were is the second colour */
static int lift(struct paint *p) {
    struct psel *s = &p->sel;
    struct pbuf *b;
    int y;
    selection_fit(p);
    if(!s->on || s->buf) return 0;
    b = pbuf_new(p, s->w, s->h);
    if(!b) {
        say(p, "Not enough memory to move the selection");
        return -1;
    }
    for(y = 0; y < s->h; y++)
        memcpy(b->px + (size_t)y * (size_t)s->w, p->px + (size_t)(s->y + y) * (size_t)p->w + s->x,
               (size_t)s->w * sizeof(unsigned));
    step_begin(p, "Move");
    for(y = 0; y < s->h; y++) fill_span(p, s->y + y, s->x, s->x + s->w - 1, p->bg);
    s->buf = b;
    s->lifted = 1;
    s->from_x = s->x;
    s->from_y = s->y;
    return 0;
}

/* what floats goes onto the canvas where it is, and its step ends; the selection stays, on the canvas */
static void put_down(struct paint *p) {
    struct psel *s = &p->sel;
    int y;
    if(!s->buf) return;
    if(!p->open) step_begin(p, "Paste");                /* a paste's step: nothing changed before this */
    for(y = 0; y < s->h; y++) copy_span(p, s->y + y, s->x, s->buf->px + (size_t)y * (size_t)s->w, s->w);
    step_end(p);
    pbuf_put(p, s->buf);
    s->buf = NULL;
    s->lifted = 0;
    mark_sel(p);
    selection_fit(p);
}

static void text_commit(struct paint *p) {
    struct ptext *t = &p->text;
    if(!t->on) return;
    mark_text(p);
    t->on = 0;
    if(!t->n) return;
    step_begin(p, "Text");
    text_draw(p, t, p->text_scale, p->fg);
    step_end(p);
}

static void text_drop(struct paint *p) {
    mark_text(p);
    p->text.on = 0;
    p->text.n = 0;
}

/* a drag under way ends first, as if its button were let go where it is */
void paint_put_down(struct paint *p) {
    if(p->drag) paint_release(p, p->bx, p->by, 0);
    text_commit(p);
    put_down(p);
}

int work_undo(struct paint *p) {
    struct psel *s = &p->sel;
    int y;
    if(p->text.on) {
        text_drop(p);
        return 1;
    }
    if(s->buf) {
        mark_sel(p);
        if(s->lifted && p->open && !p->open->lost) step_abort(p);  /* its tiles: the pixels back where they were */
        else if(s->lifted) {                            /* the step lost them (no memory): put them back by hand */
            for(y = 0; y < s->h; y++)
                copy_span(p, s->from_y + y, s->from_x, s->buf->px + (size_t)y * (size_t)s->w, s->w);
            step_end(p);
        } else if(p->open) step_abort(p);
        if(s->lifted) {                                 /* still selected, where it was */
            s->x = s->from_x;
            s->y = s->from_y;
        } else s->on = 0;
        pbuf_put(p, s->buf);
        s->buf = NULL;
        s->lifted = 0;
        mark_sel(p);
        return 1;
    }
    if(p->open) {
        step_abort(p);
        return 1;
    }
    return 0;
}

/**** the selection and the clipboard ****/

int paint_has_selection(const struct paint *p) {
    return p->sel.on;
}

int paint_can_paste(const struct paint *p) {
    return p->clip != NULL;
}

int paint_select_all(struct paint *p) {
    paint_put_down(p);
    mark_sel(p);
    p->sel.on = 1;
    p->sel.x = p->sel.y = 0;
    p->sel.w = p->w;
    p->sel.h = p->h;
    mark_sel(p);
    return 1;
}

/* the selection's pixels onto Paint's clipboard: 1 when they are there, 0 when there is not the memory (said why).
   What the clipboard held goes only once the copy is made, so a copy that fails leaves it as it was */
static int to_clipboard(struct paint *p) {
    struct psel *s = &p->sel;
    struct pbuf *b;
    int y;
    if(s->buf) {                                        /* what floats: the same pixels, shared */
        b = s->buf;
        b->refs++;
    } else {
        selection_fit(p);
        if(!s->on) {
            say(p, "Select a part of the picture first");
            return 0;
        }
        b = pbuf_new(p, s->w, s->h);
        if(!b) {
            say(p, "Not enough memory to copy %d x %d pixels: the clipboard keeps what it had", s->w, s->h);
            return 0;
        }
        for(y = 0; y < s->h; y++)
            memcpy(b->px + (size_t)y * (size_t)s->w, p->px + (size_t)(s->y + y) * (size_t)p->w + s->x,
                   (size_t)s->w * sizeof(unsigned));
    }
    pbuf_put(p, p->clip);
    p->clip = b;
    return 1;
}

int paint_copy(struct paint *p) {
    if(p->drag) paint_release(p, p->bx, p->by, 0);     /* the drag ends where it is: what it drew is copied with it */
    if(!p->sel.on) {
        say(p, "Select a part of the picture first");
        return 1;
    }
    if(to_clipboard(p)) say(p, "Copied %d x %d pixels", p->clip->w, p->clip->h);
    return 1;
}

/* the selection gone, in a step named what: what floats goes (a lift leaves the second colour behind, a paste
   nothing), a part of the canvas becomes the second colour */
static void remove_selection(struct paint *p, const char *what) {
    struct psel *s = &p->sel;
    int y;
    text_commit(p);
    mark_sel(p);
    if(s->buf) {
        pbuf_put(p, s->buf);
        s->buf = NULL;
        if(p->open) snprintf(p->open->what, sizeof(p->open->what), "%s", what);
        step_end(p);
    } else {
        step_begin(p, what);
        for(y = 0; y < s->h; y++) fill_span(p, s->y + y, s->x, s->x + s->w - 1, p->bg);
        step_end(p);
    }
    s->on = 0;
    s->lifted = 0;
}

int paint_delete(struct paint *p) {
    if(p->drag) paint_release(p, p->bx, p->by, 0);
    if(!p->sel.on) {
        say(p, "Select a part of the picture first");
        return 1;
    }
    remove_selection(p, "Delete");
    return 1;
}

/* Copy, then Delete, but only when the copy was made: else nothing goes, and the status line says why */
int paint_cut(struct paint *p) {
    int w, h;
    if(p->drag) paint_release(p, p->bx, p->by, 0);
    if(!p->sel.on) {
        say(p, "Select a part of the picture first");
        return 1;
    }
    if(!to_clipboard(p)) return 1;
    w = p->clip->w;
    h = p->clip->h;
    p->msg[0] = 0;
    remove_selection(p, "Cut");
    if(p->msg[0]) {                                     /* the step could not be kept: what lose() said stays */
        char was[PAINT_MSG];
        snprintf(was, sizeof(was), "%s", p->msg);
        say(p, "Cut %d x %d pixels. %s", w, h, was);
    } else say(p, "Cut %d x %d pixels", w, h);
    return 1;
}

int paint_paste(struct paint *p) {
    struct psel *s = &p->sel;
    int x, y;
    if(!p->clip) {
        say(p, "Nothing to paste: copy a part of a picture first");
        return 1;
    }
    paint_put_down(p);
    mark_sel(p);
    paint_to_canvas(p, 0, 0, &x, &y);                   /* at the top left of what the view shows */
    if(x < 0) x = 0;
    if(y < 0) y = 0;
    if(x > p->w - 1) x = p->w - 1;
    if(y > p->h - 1) y = p->h - 1;
    s->on = 1;
    s->x = x;
    s->y = y;
    s->w = p->clip->w;
    s->h = p->clip->h;
    s->buf = p->clip;
    s->buf->refs++;
    s->lifted = 0;
    p->tool = PAINT_SELECT;                             /* so that it can be dragged at once */
    mark_sel(p);
    say(p, "Pasted %d x %d pixels: drag them, then click outside to put them down", s->w, s->h);
    return 1;
}

/* the floating selection by dx, dy: at least a pixel of it stays on the canvas */
static void float_to(struct paint *p, int x, int y) {
    struct psel *s = &p->sel;
    if(x < 1 - s->w) x = 1 - s->w;
    if(y < 1 - s->h) y = 1 - s->h;
    if(x > p->w - 1) x = p->w - 1;
    if(y > p->h - 1) y = p->h - 1;
    mark_sel(p);
    s->x = x;
    s->y = y;
    mark_sel(p);
}

static int nudge(struct paint *p, int dx, int dy) {
    if(!p->sel.buf && lift(p) < 0) return 1;
    float_to(p, p->sel.x + dx, p->sel.y + dy);
    return 1;
}

/* the selection a drag from ax, ay to bx, by marks, both corners in it, on the canvas */
static void select_drag(struct paint *p) {
    struct psel *s = &p->sel;
    int x0 = p->ax < p->bx ? p->ax : p->bx, x1 = p->ax < p->bx ? p->bx : p->ax;
    int y0 = p->ay < p->by ? p->ay : p->by, y1 = p->ay < p->by ? p->by : p->ay;
    mark_sel(p);
    s->on = 1;
    s->x = x0;
    s->y = y0;
    s->w = x1 - x0 + 1;
    s->h = y1 - y0 + 1;
    selection_fit(p);
    mark_sel(p);
}

/**** text ****/

static int text_cells(const struct ptext *t, int nl) {
    int i, n = 0;
    for(i = 0; i < t->n; i++) n += t->cell[i] == PAINT_TEXT_NL;
    return nl ? n : t->n;
}

static int text_add(struct paint *p, unsigned short cell) {
    struct ptext *t = &p->text;
    if(t->n >= PAINT_TEXT_MAX) {
        say(p, "The text is full: %d characters at most", PAINT_TEXT_MAX);
        return 1;
    }
    if(cell == PAINT_TEXT_NL && text_cells(t, 1) + 1 >= PAINT_TEXT_LINES) {
        say(p, "The text is full: %d lines at most", PAINT_TEXT_LINES);
        return 1;
    }
    mark_text(p);
    t->cell[t->n++] = cell;
    mark_text(p);
    return 1;
}

/* a byte typed: ASCII as it is; UTF-8 gathered into its character, the console's (or '?') */
static int text_byte(struct paint *p, unsigned char b) {
    struct ptext *t = &p->text;
    unsigned cp;
    int k;
    if(b < 0x80) {
        t->have = 0;
        return text_add(p, b);
    }
    if(b >= 0xC0) {                                     /* a character's first byte: one unfinished before it is lost */
        if(b >= 0xF8) {
            t->have = 0;
            return text_add(p, '?');
        }
        t->want = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : 2;
        t->utf8[0] = b;
        t->have = 1;
        return 0;
    }
    if(!t->have) return text_add(p, '?');               /* a continuation without a start */
    t->utf8[t->have++] = b;
    if(t->have < t->want) return 0;
    t->have = 0;
    if(t->want == 2) cp = (t->utf8[0] & 0x1Fu) << 6 | (t->utf8[1] & 0x3Fu);
    else if(t->want == 3) cp = (t->utf8[0] & 0x0Fu) << 12 | (t->utf8[1] & 0x3Fu) << 6 | (t->utf8[2] & 0x3Fu);
    else cp = (t->utf8[0] & 0x07u) << 18 | (t->utf8[1] & 0x3Fu) << 12 | (t->utf8[2] & 0x3Fu) << 6 | (t->utf8[3] & 0x3Fu);
    k = cp < 0x80 ? -1 : cp437_from_unicode(cp);        /* a long form of ASCII is no character */
    return text_add(p, (unsigned short)(k > 0 ? k : '?'));
}

static int text_key(struct paint *p, int key) {
    struct ptext *t = &p->text;
    if(key == 27) {
        text_drop(p);
        return 1;
    }
    if(key == 8 || key == 127) {
        t->have = 0;
        if(t->n) {
            mark_text(p);
            t->n--;
        }
        return 1;
    }
    if(key == '\r' || key == '\n') return text_add(p, PAINT_TEXT_NL);
    if(key < 0x20 || key > 0xFF) return 0;              /* Ctrl+ letters and the keys that are no bytes: not text */
    return text_byte(p, (unsigned char)key);
}

int paint_typing(const struct paint *p) {
    return p->text.on;
}

/**** the tools ****/

static void stroke_to(struct paint *p, int x0, int y0, int x1, int y1) {
    int size = p->tool == PAINT_PENCIL ? 1 : paint_sizes[p->size];
    stroke(p, x0, y0, x1, y1, size, p->tool != PAINT_ERASER, p->tool == PAINT_ERASER ? p->bg : p->drag_colour);
}

static int hover(struct paint *p, int x, int y) {
    int on = on_canvas(p, x, y);
    if(on == p->at_on && (!on || (x == p->at_x && y == p->at_y))) return 0;
    p->at_on = on;
    p->at_x = x;
    p->at_y = y;
    return 1;
}

int paint_leave(struct paint *p) {
    if(!p->at_on) return 0;
    p->at_on = 0;
    return 1;
}

void paint_set_tool(struct paint *p, int tool) {
    if(tool < 0 || tool >= PAINT_TOOLS || tool == p->tool) return;
    if(p->drag) paint_release(p, p->bx, p->by, 0);
    text_commit(p);
    if(tool != PAINT_SELECT) put_down(p);
    if(tool == PAINT_PICKER) p->back_tool = p->tool;
    p->tool = tool;
    p->msg[0] = 0;
}

int paint_press(struct paint *p, int x, int y, int button, int mods) {
    struct psel *s = &p->sel;
    unsigned c;
    if(p->drag || (button != PAINT_LEFT && button != PAINT_RIGHT)) return 0;
    far(p, &x, &y);
    hover(p, x, y);
    c = button == PAINT_RIGHT ? p->bg : p->fg;
    p->msg[0] = 0;
    if(p->tool != PAINT_TEXT) text_commit(p);
    if(p->tool != PAINT_SELECT) put_down(p);
    p->drag = button;
    p->ax = p->bx = x;
    p->ay = p->by = y;
    p->drag_colour = c;
    switch(p->tool) {
    case PAINT_PENCIL:
    case PAINT_BRUSH:
    case PAINT_ERASER:
        step_begin(p, paint_tool_names[p->tool]);
        stroke_to(p, x, y, x, y);
        break;
    case PAINT_LINE:
    case PAINT_RECT:
    case PAINT_ELLIPSE:
        shape_set(p, p->tool, x, y, x, y, mods & PAINT_SHIFT);
        mark_shape(p);
        break;
    case PAINT_FILL:
        p->drag = 0;
        if(!on_canvas(p, x, y)) break;
        step_begin(p, "Fill");
        flood_fill(p, x, y, c);
        step_end(p);
        break;
    case PAINT_TEXT:
        p->drag = 0;
        text_commit(p);
        memset(&p->text, 0, sizeof(p->text));
        p->text.on = 1;
        p->text.x = x;
        p->text.y = y;
        mark_text(p);
        break;
    case PAINT_PICKER:
        p->drag = 0;
        if(!on_canvas(p, x, y)) break;
        c = p->px[(size_t)y * (size_t)p->w + x];
        paint_set_colour(p, button == PAINT_RIGHT, c);
        p->tool = p->back_tool;
        say(p, "Took #%06X as the %s colour", c, button == PAINT_RIGHT ? "second" : "first");
        break;
    case PAINT_SELECT:
        p->moving = s->on && x >= s->x && y >= s->y && x < s->x + s->w && y < s->y + s->h;
        if(p->moving) {                                 /* inside: it is lifted, and the drag moves it */
            if(!s->buf && lift(p) < 0) {
                p->drag = p->moving = 0;
                break;
            }
            s->grab_x = x - s->x;
            s->grab_y = y - s->y;
            break;
        }
        put_down(p);                                    /* outside: what floats goes down, and a new one starts */
        mark_sel(p);
        s->on = 0;
        break;
    }
    return 1;
}

int paint_move(struct paint *p, int x, int y, int mods) {
    int changed;
    far(p, &x, &y);
    changed = hover(p, x, y);
    if(!p->drag) return changed;
    if(x == p->bx && y == p->by && p->tool != PAINT_LINE && p->tool != PAINT_RECT && p->tool != PAINT_ELLIPSE)
        return changed;
    switch(p->tool) {
    case PAINT_PENCIL:
    case PAINT_BRUSH:
    case PAINT_ERASER:
        stroke_to(p, p->bx, p->by, x, y);
        break;
    case PAINT_LINE:
    case PAINT_RECT:
    case PAINT_ELLIPSE:                                 /* where it was and where it is now are drawn again */
        mark_shape(p);
        shape_set(p, p->tool, p->ax, p->ay, x, y, mods & PAINT_SHIFT);
        mark_shape(p);
        break;
    case PAINT_SELECT:
        p->bx = x;
        p->by = y;
        if(p->moving) float_to(p, x - p->sel.grab_x, y - p->sel.grab_y);
        else select_drag(p);
        break;
    }
    p->bx = x;
    p->by = y;
    return 1;
}

/* the button that started the drag was let go */
int paint_release(struct paint *p, int x, int y, int mods) {
    if(!p->drag) return 0;
    paint_move(p, x, y, mods);
    switch(p->tool) {
    case PAINT_PENCIL:
    case PAINT_BRUSH:
    case PAINT_ERASER:
        step_end(p);
        break;
    case PAINT_LINE:
    case PAINT_RECT:
    case PAINT_ELLIPSE:
        mark_shape(p);
        step_begin(p, paint_tool_names[p->tool]);
        shape_draw(p, &p->shape);
        step_end(p);
        p->shape.on = 0;
        break;
    case PAINT_SELECT:
        if(!p->moving && p->ax == p->bx && p->ay == p->by) {    /* a click: nothing selected */
            mark_sel(p);
            p->sel.on = 0;
        }
        p->moving = 0;
        break;
    }
    p->drag = 0;
    return 1;
}

int paint_cancel(struct paint *p) {
    if(p->drag && p->shape.on) {                        /* the shape being dragged is not drawn */
        mark_shape(p);
        p->shape.on = 0;
        p->drag = 0;
        return 1;
    }
    if(p->drag) paint_release(p, p->bx, p->by, 0);
    if(p->text.on) {
        text_drop(p);
        return 1;
    }
    if(p->sel.on) {
        put_down(p);
        mark_sel(p);
        p->sel.on = 0;
        return 1;
    }
    return 0;
}

int paint_key(struct paint *p, int key, int mods) {
    int d = mods & PAINT_SHIFT ? 10 : 1;
    if(p->text.on) return text_key(p, key);
    if(key == 27) return paint_cancel(p);
    if(!p->sel.on || p->drag) return 0;
    switch(key) {
    case PAINT_KEY_LEFT: return nudge(p, -d, 0);
    case PAINT_KEY_RIGHT: return nudge(p, d, 0);
    case PAINT_KEY_UP: return nudge(p, 0, -d);
    case PAINT_KEY_DOWN: return nudge(p, 0, d);
    case PAINT_KEY_DELETE: return paint_delete(p);
    case '\r':
    case '\n':
        if(!p->sel.buf) return 0;
        put_down(p);
        return 1;
    }
    return 0;
}

/**** the colours and the settings ****/

void paint_set_colour(struct paint *p, int second, unsigned rgb) {
    rgb &= 0xFFFFFF;
    if(second) p->bg = rgb;
    else {
        p->fg = rgb;
        mark_text(p);                                   /* text being typed is in the first colour */
    }
}

void paint_swap_colours(struct paint *p) {
    unsigned t = p->fg;
    p->fg = p->bg;
    p->bg = t;
    mark_text(p);
}

void paint_add_custom(struct paint *p, unsigned rgb) {
    int i;
    rgb &= 0xFFFFFF;
    for(i = 0; i < PAINT_CUSTOM; i++)
        if(p->custom[i] == rgb) break;
    if(i == PAINT_CUSTOM) {                             /* new: in the oldest's place */
        p->custom[p->custom_next] = rgb;
        p->custom_next = (p->custom_next + 1) % PAINT_CUSTOM;
    }
    paint_set_colour(p, 0, rgb);
}

void paint_set_size(struct paint *p, int size) {
    if(size >= 0 && size < PAINT_SIZES) p->size = size;
}

void paint_set_filled(struct paint *p, int filled) {
    p->filled = filled != 0;
}

void paint_set_text_scale(struct paint *p, int scale) {
    if(scale < 1 || scale > PAINT_TEXT_SCALE) return;
    mark_text(p);
    p->text_scale = scale;
    mark_text(p);
}

static int hex(int c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* hexadecimal ("#RRGGBB", "#RGB", or the same without #) when it is one word; else three numbers of 0 to 255,
   apart by commas or spaces */
int paint_parse_colour(const char *s, unsigned *rgb) {
    const char *e;
    unsigned v[3];
    int n, i, k, d, apart = 0;
    while(*s == ' ') s++;
    for(e = s; *e && *e != ' ' && *e != ','; e++)
        ;
    if(*e == ',') apart = 1;
    while(*e == ' ') e++;
    if(*e) apart = 1;
    if(!apart) {
        if(*s == '#') s++;
        for(n = 0; hex(s[n]) >= 0; n++)
            ;
        for(i = n; s[i] == ' '; i++)
            ;
        if((n != 6 && n != 3) || s[i]) return -1;
        *rgb = 0;
        for(i = 0; i < n; i++) *rgb = *rgb << (n == 3 ? 8 : 4) | (unsigned)hex(s[i]) * (n == 3 ? 17u : 1u);
        return 0;
    }
    for(k = 0; k < 3; k++) {
        if(k) {
            while(*s == ' ') s++;
            if(*s == ',') s++;
            while(*s == ' ') s++;
        }
        if(*s < '0' || *s > '9') return -1;
        for(v[k] = 0, d = 0; *s >= '0' && *s <= '9'; s++, d++) {
            v[k] = v[k] * 10 + (unsigned)(*s - '0');
            if(d >= 3 || v[k] > 255) return -1;
        }
    }
    while(*s == ' ') s++;
    if(*s) return -1;
    *rgb = v[0] << 16 | v[1] << 8 | v[2];
    return 0;
}

void paint_colour_text(unsigned rgb, char *out, int n) {
    snprintf(out, (size_t)n, "#%06X", rgb & 0xFFFFFF);
}

/**** the status line ****/

void paint_hint(const struct paint *p, char *out, int n) {
    int size = paint_sizes[p->size];
    if(p->msg[0]) {
        snprintf(out, (size_t)n, "%s", p->msg);
        return;
    }
    switch(p->tool) {
    case PAINT_PENCIL: snprintf(out, (size_t)n, "Pencil: drag to draw (the right button: the second colour)"); break;
    case PAINT_BRUSH: snprintf(out, (size_t)n, "Brush, %d px: drag to paint (the right button: the second colour)", size); break;
    case PAINT_ERASER: snprintf(out, (size_t)n, "Eraser, %d px: drag to paint the second colour", size); break;
    case PAINT_LINE: snprintf(out, (size_t)n, "Line, %d px: drag from one end to the other", size); break;
    case PAINT_RECT:
    case PAINT_ELLIPSE:
        if(p->filled) snprintf(out, (size_t)n, "%s, filled: drag from corner to corner", paint_tool_names[p->tool]);
        else snprintf(out, (size_t)n, "%s, %d px: drag from corner to corner", paint_tool_names[p->tool], size);
        break;
    case PAINT_FILL: snprintf(out, (size_t)n, "Fill: click an area to fill it (the right button: the second colour)"); break;
    case PAINT_TEXT:
        if(p->text.on) snprintf(out, (size_t)n, "Type; Enter: a new line; a click elsewhere puts it down; Esc drops it");
        else snprintf(out, (size_t)n, "Text, %d times: click where it starts", p->text_scale);
        break;
    case PAINT_PICKER: snprintf(out, (size_t)n, "Picker: click a colour to take it (the right button: as the second)"); break;
    default:
        if(p->sel.buf) snprintf(out, (size_t)n, "Drag it or use the arrows; Enter puts it down, Del deletes it");
        else if(p->sel.on) snprintf(out, (size_t)n, "Drag inside it to move it; Ctrl+C copies, Del deletes");
        else snprintf(out, (size_t)n, "Select: drag a rectangle");
        break;
    }
}

void paint_where(const struct paint *p, char *out, int n) {
    char at[24] = "", sel[32] = "";
    if(p->at_on) snprintf(at, sizeof(at), "%d, %d   ", p->at_x, p->at_y);
    if(p->sel.on) snprintf(sel, sizeof(sel), "%d x %d selected   ", p->sel.w, p->sel.h);
    snprintf(out, (size_t)n, "%s%s%d x %d   %d%%", at, sel, p->w, p->h, paint_zoom_percent(p->zoom));
}
