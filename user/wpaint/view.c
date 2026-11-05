/*
SurfOS user program: wpaint
--------------------
File: view.c    Date: 11/05/26
--------------------
The view of the canvas: the area of the window it is shown in (vw x vh), a zoom from 1/8 to 32
times, and the scroll, in the zoomed canvas's pixels, held so that the view never shows past it.
A canvas smaller than the view, zoomed, is shown in its middle on the backdrop.

Zoomed in, a canvas pixel is num x num pixels of the view; zoomed out, a pixel of the view shows
the canvas pixel in the middle of the den x den it stands for. A click goes to that same pixel,
so what is clicked is what is shown. Zooming keeps the canvas's point under a point of the view
(the pointer, or the view's middle) where it is.

paint_render() fills a rectangle of the view: the canvas, and over it what floats, the shape
being dragged, the text being typed with its cursor, and the selection's outline in black and
white dashes, which show on any colour. paint_damage() says which rectangle of the view changed,
from what the model marked in canvas pixels, so that the window presents only that.
*/
#include <string.h>
#include "paint.h"
#include "ppriv.h"

static const struct { short num, den; } zooms[PAINT_ZOOMS] = {   /* fourteen: a submenu of the desktop's holds them */
    { 1, 8 }, { 1, 4 }, { 1, 3 }, { 1, 2 }, { 1, 1 }, { 2, 1 }, { 3, 1 }, { 4, 1 }, { 6, 1 }, { 8, 1 }, { 12, 1 },
    { 16, 1 }, { 24, 1 }, { 32, 1 }
};

/* a / b rounded down, b > 0 */
static int fdiv(int a, int b) {
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

void paint_zoom_ratio(int zoom, int *num, int *den) {
    if(zoom < 0) zoom = 0;
    if(zoom >= PAINT_ZOOMS) zoom = PAINT_ZOOMS - 1;
    *num = zooms[zoom].num;
    *den = zooms[zoom].den;
}

int paint_zoom_percent(int zoom) {
    int num, den;
    paint_zoom_ratio(zoom, &num, &den);
    return num * 100 / den;
}

static void zoomed_at(const struct paint *p, int zoom, int *cw, int *ch) {
    int num, den;
    paint_zoom_ratio(zoom, &num, &den);
    *cw = den > 1 ? (p->w + den - 1) / den : p->w * num;
    *ch = den > 1 ? (p->h + den - 1) / den : p->h * num;
}

void paint_zoomed(const struct paint *p, int *cw, int *ch) {
    zoomed_at(p, p->zoom, cw, ch);
}

static void origin_at(const struct paint *p, int zoom, int *ox, int *oy) {
    int cw, ch;
    zoomed_at(p, zoom, &cw, &ch);
    *ox = cw < p->vw ? (p->vw - cw) / 2 : 0;
    *oy = ch < p->vh ? (p->vh - ch) / 2 : 0;
}

void paint_origin(const struct paint *p, int *ox, int *oy) {
    origin_at(p, p->zoom, ox, oy);
}

/* a zoomed canvas's column (or row) to the canvas's, and back to where it starts */
static int to_canvas(int z, int num, int den) {
    return den > 1 ? z * den + den / 2 : fdiv(z, num);
}

static int to_zoomed(int c, int num, int den) {
    return den > 1 ? fdiv(c, den) : c * num;
}

void paint_to_canvas(const struct paint *p, int vx, int vy, int *cx, int *cy) {
    int num, den, ox, oy;
    paint_zoom_ratio(p->zoom, &num, &den);
    paint_origin(p, &ox, &oy);
    *cx = to_canvas(vx - ox + p->sx, num, den);
    *cy = to_canvas(vy - oy + p->sy, num, den);
}

void paint_to_view(const struct paint *p, int cx, int cy, int *vx, int *vy) {
    int num, den, ox, oy;
    paint_zoom_ratio(p->zoom, &num, &den);
    paint_origin(p, &ox, &oy);
    *vx = to_zoomed(cx, num, den) - p->sx + ox;
    *vy = to_zoomed(cy, num, den) - p->sy + oy;
}

/**** scrolling and zooming ****/

int paint_scroll_to(struct paint *p, int sx, int sy) {
    int cw, ch;
    paint_zoomed(p, &cw, &ch);
    if(sx > cw - p->vw) sx = cw - p->vw;
    if(sy > ch - p->vh) sy = ch - p->vh;
    if(sx < 0) sx = 0;
    if(sy < 0) sy = 0;
    if(sx == p->sx && sy == p->sy) return 0;
    p->sx = sx;
    p->sy = sy;
    mark_view(p);
    return 1;
}

int paint_scroll_by(struct paint *p, int dx, int dy) {
    return paint_scroll_to(p, p->sx + dx, p->sy + dy);
}

void paint_view_size(struct paint *p, int vw, int vh) {
    if(vw < 0) vw = 0;
    if(vh < 0) vh = 0;
    if(vw > PAINT_VIEW_MAX) vw = PAINT_VIEW_MAX;
    p->vw = vw;
    p->vh = vh;
    paint_scroll_by(p, 0, 0);
    mark_view(p);
}

/* the canvas's point under the view's ax, ay stays there */
int paint_zoom_to(struct paint *p, int zoom, int ax, int ay) {
    int num, den, n2, d2, ox, oy, ox2, oy2, zx, zy;
    if(zoom < 0) zoom = 0;
    if(zoom >= PAINT_ZOOMS) zoom = PAINT_ZOOMS - 1;
    if(zoom == p->zoom) return 0;
    paint_zoom_ratio(p->zoom, &num, &den);
    paint_zoom_ratio(zoom, &n2, &d2);
    paint_origin(p, &ox, &oy);
    origin_at(p, zoom, &ox2, &oy2);
    zx = fdiv((ax - ox + p->sx) * den * n2, num * d2);  /* at most 131072 x 8 x 32: within an int */
    zy = fdiv((ay - oy + p->sy) * den * n2, num * d2);
    p->zoom = zoom;
    p->sx = p->sy = 0;
    paint_scroll_to(p, zx - (ax - ox2), zy - (ay - oy2));
    mark_view(p);
    return 1;
}

int paint_zoom_by(struct paint *p, int steps, int ax, int ay) {
    return paint_zoom_to(p, p->zoom + steps, ax, ay);
}

int paint_zoom_fit(struct paint *p) {
    int zoom, cw, ch;
    for(zoom = PAINT_ZOOM_1; zoom > 0; zoom--) {
        zoomed_at(p, zoom, &cw, &ch);
        if(cw <= p->vw && ch <= p->vh) break;
    }
    if(zoom == p->zoom) return 0;
    p->zoom = zoom;
    zoomed_at(p, zoom, &cw, &ch);
    p->sx = p->sy = 0;
    paint_scroll_to(p, (cw - p->vw) / 2, (ch - p->vh) / 2);
    mark_view(p);
    return 1;
}

/**** what changed ****/

int paint_damage(const struct paint *p, int *x, int *y, int *w, int *h) {
    int x0, y0, x1, y1;
    if(p->vw < 1 || p->vh < 1) return 0;
    if(p->view_dirty) {
        *x = *y = 0;
        *w = p->vw;
        *h = p->vh;
        return 1;
    }
    if(!p->dirty) return 0;
    paint_to_view(p, p->dx0, p->dy0, &x0, &y0);
    paint_to_view(p, p->dx1 + 1, p->dy1 + 1, &x1, &y1);
    x0 -= 2;                                            /* the outline and the cursor reach past their pixels */
    y0 -= 2;
    x1 += 2;
    y1 += 2;
    if(x0 < 0) x0 = 0;
    if(y0 < 0) y0 = 0;
    if(x1 > p->vw) x1 = p->vw;
    if(y1 > p->vh) y1 = p->vh;
    if(x0 >= x1 || y0 >= y1) return 0;
    *x = x0;
    *y = y0;
    *w = x1 - x0;
    *h = y1 - y0;
    return 1;
}

void paint_drawn(struct paint *p) {
    p->dirty = 0;
    p->view_dirty = 0;
}

/**** the view's pixels ****/

static void dash(unsigned *out, int rx, int rw, int x, int vy) {
    if(x >= rx && x < rx + rw) out[x - rx] = ((x + vy) >> 2) & 1 ? 0xFFFFFF : 0x000000;
}

/* the selection's outline: a pixel of the view outside its edges, in dashes of four */
static void outline(const struct paint *p, unsigned *out, int rx, int rw, int vy) {
    const struct psel *s = &p->sel;
    int x0, y0, x1, y1, x, last;
    paint_to_view(p, s->x, s->y, &x0, &y0);
    paint_to_view(p, s->x + s->w, s->y + s->h, &x1, &y1);
    x0--;
    y0--;
    if(vy < y0 || vy > y1) return;
    if(vy == y0 || vy == y1) {                          /* the top and the bottom edges: the row across */
        last = x1 < rx + rw - 1 ? x1 : rx + rw - 1;
        for(x = x0 > rx ? x0 : rx; x <= last; x++) dash(out, rx, rw, x, vy);
        return;
    }
    dash(out, rx, rw, x0, vy);
    dash(out, rx, rw, x1, vy);
}

void paint_render(const struct paint *p, unsigned *dst, int stride, int rx, int ry, int rw, int rh) {
    int cols[PAINT_VIEW_MAX], start[PAINT_TEXT_LINES], len[PAINT_TEXT_LINES], xs[4];
    const struct psel *s = &p->sel;
    const struct ptext *t = &p->text;
    const unsigned *src, *frow;
    unsigned *out, pix, bits;
    int num, den, ox, oy, cw, ch, i, vy, zy, cy, c, n, lines = 0, ts = p->text_scale, tline, tg, tx1, cur_x = 0, cur_y0 = 0,
        cur_y1 = -1, k;
    if(rx < 0) {                                        /* dst is the rectangle's top left: it moves with the clipping */
        rw += rx;
        dst -= rx;
        rx = 0;
    }
    if(ry < 0) {
        rh += ry;
        dst -= (long)ry * stride;
        ry = 0;
    }
    if(rw > p->vw - rx) rw = p->vw - rx;
    if(rh > p->vh - ry) rh = p->vh - ry;
    if(rw <= 0 || rh <= 0) return;
    paint_zoom_ratio(p->zoom, &num, &den);
    paint_origin(p, &ox, &oy);
    paint_zoomed(p, &cw, &ch);
    for(i = 0; i < rw; i++) {
        c = rx + i - ox + p->sx;
        cols[i] = c < 0 || c >= cw ? -1 : to_canvas(c, num, den);
        if(cols[i] > p->w - 1) cols[i] = p->w - 1;      /* a zoomed-out view's last, partial column */
    }
    if(t->on) {                                         /* the text's lines; its cursor after the last one */
        lines = text_lines(t, start, len, PAINT_TEXT_LINES);
        paint_to_view(p, t->x + len[lines - 1] * 8 * ts, t->y + (lines - 1) * 16 * ts, &cur_x, &cur_y0);
        paint_to_view(p, t->x, t->y + lines * 16 * ts, &k, &cur_y1);
        if(cur_y1 <= cur_y0) cur_y1 = cur_y0 + 1;
    }
    for(vy = ry; vy < ry + rh; vy++) {
        out = dst + (size_t)(vy - ry) * (size_t)stride;
        zy = vy - oy + p->sy;
        if(zy < 0 || zy >= ch) {
            for(i = 0; i < rw; i++) out[i] = p->backdrop;
        } else {
            cy = to_canvas(zy, num, den);
            if(cy > p->h - 1) cy = p->h - 1;
            src = p->px + (size_t)cy * (size_t)p->w;
            frow = s->buf && cy >= s->y && cy < s->y + s->h ? s->buf->px + (size_t)(cy - s->y) * (size_t)s->w : NULL;
            n = shape_row(p, &p->shape, cy, xs);
            tline = -1;
            tg = 0;
            tx1 = 0;
            if(t->on && cy >= t->y && cy < t->y + lines * 16 * ts) {
                tline = (cy - t->y) / (16 * ts);
                tg = (cy - t->y) / ts % 16;
                tx1 = t->x + len[tline] * 8 * ts;
            }
            for(i = 0; i < rw; i++) {
                c = cols[i];
                if(c < 0) {
                    out[i] = p->backdrop;
                    continue;
                }
                pix = src[c];
                if(frow && c >= s->x && c < s->x + s->w) pix = frow[c - s->x];
                if(n > 0 && ((c >= xs[0] && c <= xs[1]) || (n > 1 && c >= xs[2] && c <= xs[3]))) pix = p->shape.colour;
                if(tline >= 0 && c >= t->x && c < tx1) {
                    k = (c - t->x) / ts;
                    bits = font8x16[t->cell[start[tline] + k / 8] & 0xFF][tg];
                    if(bits & (0x80u >> (k % 8))) pix = p->fg;
                }
                out[i] = pix;
            }
        }
        if(s->on) outline(p, out, rx, rw, vy);
        if(t->on && vy >= cur_y0 && vy < cur_y1)         /* the cursor: two pixels of the view, the colours turned over */
            for(k = cur_x; k < cur_x + 2; k++)
                if(k >= rx && k < rx + rw) out[k - rx] ^= 0xFFFFFF;
    }
}
