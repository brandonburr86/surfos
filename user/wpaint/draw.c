/*
SurfOS user program: wpaint
--------------------
File: draw.c    Date: 11/05/26
--------------------
What the tools draw, as runs of pixels in rows, in whole numbers only.

A dab is the brush's print: a disc (the brush, the pencil at one pixel) or a square (the eraser),
size pixels across. A line, and a stroke between two points of a drag, is a dab at every point of
the line between them (Bresenham's line, from his 1965 paper); in each row the dabs make one run,
whose ends line_rows() gathers into p->rowl[] and p->rowr[], so a stroke is drawn a row at a time
and a line looks the same while it is dragged as when it is drawn.

A rectangle and an ellipse are rows too: up to two runs each (an outline's sides). An ellipse's
row is worked out from its equation at the pixels' centres, x^2/a^2 + y^2/b^2 <= 1 in doubled
whole numbers, with an integer square root (digit by digit), so it is symmetric and has no gaps.
Its outline is the ellipse less the one inside it, size pixels in.

The flood fill is a fill of runs: a run of the colour clicked is filled whole, and the runs
touching it in the rows above and below are queued, after the method in Paul Heckbert's "A Seed
Fill Algorithm" (Graphics Gems, 1990), with a stack instead of recursion. The stack is bounded
(p->fill_stack runs): when it is full a run is not queued and its row is marked, and once the
stack is empty those rows alone are scanned for runs of the colour beside a pixel this fill made,
which a bitmap marks, until no row is marked. A run left unfilled beside the fill can only be in
such a row (every other run beside one filled was queued and taken), so each scan reads only rows
where the fill was cut short. So any picture is filled whole, in bounded memory and without
recursion, and a picture made to fill the stack again and again costs a few rows each time, not
the canvas.

Text is the console's font (driver/font8x16.c), each pixel of a character scale x scale.
*/
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include "paint.h"
#include "ppriv.h"

const int paint_sizes[PAINT_SIZES] = { 1, 2, 3, 5, 8, 12, 16, 24 };

/* the integer square root of v, rounded down, a bit pair at a time */
static unsigned long long isqrt64(unsigned long long v) {
    unsigned long long r = 0, bit = 1ULL << 62;
    while(bit > v) bit >>= 2;
    while(bit) {
        if(v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else r >>= 1;
        bit >>= 2;
    }
    return r;
}

/* Row r of the ellipse that fills a box of w x h: its first and last pixel from the box's left, a and b, and 1;
   0 when the row has none. A pixel is in when its centre is: with X = 2x + 1 - w and Y = 2r + 1 - h (twice the
   distances from the box's middle), X^2 h^2 + Y^2 w^2 <= w^2 h^2. Sides of up to 3 x PAINT_FAR keep that within
   64 bits, and the root within 32, so no 64-bit division is needed (programs do not link libgcc). */
static int ellipse_centres(int w, int h, int r, int *a, int *b) {
    long long Y, W = w, H = h;
    unsigned long long v;
    unsigned s;
    int m;
    if(w < 1 || h < 1 || r < 0 || r >= h) return 0;
    Y = 2LL * r + 1 - H;
    v = (unsigned long long)(W * W) * (unsigned long long)(H * H - Y * Y);
    s = (unsigned)isqrt64(v);                           /* at most w * h */
    m = (int)(s / (unsigned)h);                         /* the largest |X| */
    if((m & 1) != ((w - 1) & 1)) m--;                   /* X has the parity of w - 1 */
    if(m < 0) return 0;
    *a = (w - 1 - m) / 2;
    *b = (w - 1 + m) / 2;
    return 1;
}

/* the same, but a row of the box no pixel centre of which is in (the ends of a thin ellipse) has its middle pixel,
   or two: an ellipse drawn touches its box on every side */
int ellipse_span(int w, int h, int r, int *a, int *b) {
    if(w < 1 || h < 1 || r < 0 || r >= h) return 0;
    if(ellipse_centres(w, h, r, a, b)) return 1;
    *a = (w - 1) / 2;
    *b = w / 2;
    return 1;
}

void dab_row(int size, int round, int r, int *l, int *rr) {
    int up = (size - 1) / 2, a = 0, b = size - 1;
    if(round) ellipse_span(size, size, r, &a, &b);
    *l = a - up;
    *rr = b - up;
}

/* the rows of a line of dabs from x0, y0 to x1, y1 into rowl[] and rowr[] (rows without any: rowl > rowr), within
   the canvas's rows; top and bottom the rows looked at. 1 when it reaches a row of the canvas */
int line_rows(struct paint *p, int x0, int y0, int x1, int y1, int size, int round, int *top, int *bottom) {
    int dl[PAINT_DAB_MAX], dr[PAINT_DAB_MAX], dx, dy, sx, sy, err, e2, up, lo, hi, r, y;
    if(size < 1) size = 1;
    if(size > PAINT_DAB_MAX) size = PAINT_DAB_MAX;
    up = (size - 1) / 2;
    for(r = 0; r < size; r++) dab_row(size, round, r, &dl[r], &dr[r]);
    lo = (y0 < y1 ? y0 : y1) - up;
    hi = (y0 > y1 ? y0 : y1) + size - 1 - up;
    if(lo < 0) lo = 0;
    if(hi > p->h - 1) hi = p->h - 1;
    *top = lo;
    *bottom = hi;
    if(lo > hi) return 0;
    for(y = lo; y <= hi; y++) {
        p->rowl[y] = INT_MAX;
        p->rowr[y] = INT_MIN;
    }
    dx = x1 > x0 ? x1 - x0 : x0 - x1;
    dy = y1 > y0 ? y0 - y1 : y1 - y0;                   /* minus the height */
    sx = x0 < x1 ? 1 : -1;
    sy = y0 < y1 ? 1 : -1;
    err = dx + dy;
    for(;;) {
        for(r = 0; r < size; r++) {
            y = y0 - up + r;
            if(y < lo || y > hi) continue;
            if(x0 + dl[r] < p->rowl[y]) p->rowl[y] = x0 + dl[r];
            if(x0 + dr[r] > p->rowr[y]) p->rowr[y] = x0 + dr[r];
        }
        if(x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if(e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if(e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
    return 1;
}

void stroke(struct paint *p, int x0, int y0, int x1, int y1, int size, int round, unsigned c) {
    int top, bottom, y;
    if(!line_rows(p, x0, y0, x1, y1, size, round, &top, &bottom)) return;
    for(y = top; y <= bottom; y++)
        if(p->rowl[y] <= p->rowr[y]) fill_span(p, y, p->rowl[y], p->rowr[y], c);
}

/**** lines, rectangles and ellipses ****/

/* the shape a drag from x0, y0 to x1, y1 makes with the tool; constrain: a line at a multiple of 45 degrees, a
   square, a circle */
void shape_set(struct paint *p, int tool, int x0, int y0, int x1, int y1, int constrain) {
    struct pshape *s = &p->shape;
    int dx = x1 - x0, dy = y1 - y0, ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy, m, t;
    if(constrain && tool == PAINT_LINE) {
        if(ay * 2 < ax) dy = 0;                         /* nearer the horizontal */
        else if(ax * 2 < ay) dx = 0;
        else {
            m = ax > ay ? ax : ay;
            dx = dx < 0 ? -m : m;
            dy = dy < 0 ? -m : m;
        }
    } else if(constrain) {
        m = ax > ay ? ax : ay;
        dx = dx < 0 ? -m : m;
        dy = dy < 0 ? -m : m;
    }
    x1 = x0 + dx;
    y1 = y0 + dy;
    s->on = 1;
    s->tool = tool;
    s->size = paint_sizes[p->size];
    s->filled = p->filled;
    s->colour = p->drag_colour;
    if(tool == PAINT_LINE) {
        s->x0 = x0;
        s->y0 = y0;
        s->x1 = x1;
        s->y1 = y1;
        if(!line_rows(p, x0, y0, x1, y1, s->size, 1, &s->top, &s->bottom)) {
            s->top = 1;                                 /* no row of the canvas */
            s->bottom = 0;
        }
        return;
    }
    if(x1 < x0) { t = x0; x0 = x1; x1 = t; }
    if(y1 < y0) { t = y0; y0 = y1; y1 = t; }
    s->x0 = x0;
    s->y0 = y0;
    s->x1 = x1;
    s->y1 = y1;
    s->top = y0;
    s->bottom = y1;
}

void shape_bounds(const struct pshape *s, int *x0, int *y0, int *x1, int *y1) {
    int d = s->tool == PAINT_LINE ? s->size : 0;
    *x0 = (s->x0 < s->x1 ? s->x0 : s->x1) - d;
    *x1 = (s->x0 > s->x1 ? s->x0 : s->x1) + d;
    *y0 = (s->y0 < s->y1 ? s->y0 : s->y1) - d;
    *y1 = (s->y0 > s->y1 ? s->y0 : s->y1) + d;
}

/* row y's runs of the shape: x[0]..x[1] and x[2]..x[3]; how many */
int shape_row(const struct paint *p, const struct pshape *s, int y, int x[4]) {
    int t = s->size, w = s->x1 - s->x0 + 1, h = s->y1 - s->y0 + 1, a, b, c, d, left, right, k;
    if(!s->on || y < s->top || y > s->bottom) return 0;
    if(s->tool == PAINT_LINE) {
        if(y < 0 || y >= p->h || p->rowl[y] > p->rowr[y]) return 0;
        x[0] = p->rowl[y];
        x[1] = p->rowr[y];
        return 1;
    }
    if(s->tool == PAINT_RECT) {
        x[0] = s->x0;
        x[1] = s->x1;
        if(s->filled || w <= 2 * t || h <= 2 * t || y < s->y0 + t || y > s->y1 - t) return 1;
        x[1] = s->x0 + t - 1;
        x[2] = s->x1 - t + 1;
        x[3] = s->x1;
        return 2;
    }
    if(!ellipse_span(w, h, y - s->y0, &a, &b)) return 0;
    x[0] = s->x0 + a;
    x[3] = s->x0 + b;
    x[1] = x[3];
    if(s->filled || w <= 2 * t || h <= 2 * t || !ellipse_centres(w - 2 * t, h - 2 * t, y - s->y0 - t, &c, &d)) return 1;
    left = s->x0 + t + c - 1;                           /* the outline leaves out the ellipse t inside it */
    right = s->x0 + t + d + 1;
    for(k = -1; k <= 1; k += 2)                         /* each side reaches the next rows' ends: no gaps where it is flat */
        if(ellipse_span(w, h, y - s->y0 + k, &c, &d)) {
            if(s->x0 + c - 1 > left) left = s->x0 + c - 1;
            if(s->x0 + d + 1 < right) right = s->x0 + d + 1;
        }
    if(left < x[0]) left = x[0];
    if(right > x[3]) right = x[3];
    if(left + 1 >= right) return 1;                     /* the sides meet: one run */
    x[1] = left;
    x[2] = right;
    return 2;
}

void shape_draw(struct paint *p, const struct pshape *s) {
    int x[4], y, n, top = s->top < 0 ? 0 : s->top, bottom = s->bottom > p->h - 1 ? p->h - 1 : s->bottom;
    for(y = top; y <= bottom; y++) {
        n = shape_row(p, s, y, x);
        if(n > 0) fill_span(p, y, x[0], x[1], s->colour);
        if(n > 1) fill_span(p, y, x[2], x[3], s->colour);
    }
}

/**** the flood fill ****/

struct run { int y, x0, x1, dy; };

struct filling {
    struct paint *p;
    unsigned target, colour;
    struct run *stack;
    int n, cap, over;
    unsigned char *done;                                /* a bit for each pixel this fill made */
    unsigned char *cut;                                 /* a byte a row: a run of it was not queued (the stack full) */
    long filled;
};

static int is_target(const struct filling *f, int x, int y) {
    return f->p->px[(size_t)y * (size_t)f->p->w + x] == f->target;
}

static int is_done(const struct filling *f, int x, int y) {
    size_t i = (size_t)y * (size_t)f->p->w + x;
    return f->done[i >> 3] >> (i & 7) & 1;
}

static void push(struct filling *f, int y, int x0, int x1, int dy) {
    if(y + dy < 0 || y + dy >= f->p->h) return;
    if(f->n == f->cap) {                                /* full: the scan after it finds what this leaves */
        f->over = 1;
        f->cut[y + dy] = 1;
        return;
    }
    f->stack[f->n].y = y;
    f->stack[f->n].x0 = x0;
    f->stack[f->n].x1 = x1;
    f->stack[f->n].dy = dy;
    f->n++;
}

/* the run of the target colour through x, y, filled; its ends in a, b */
static void fill_run(struct filling *f, int x, int y, int *a, int *b) {
    size_t i;
    int l = x, r = x, k;
    while(l > 0 && is_target(f, l - 1, y)) l--;
    while(r < f->p->w - 1 && is_target(f, r + 1, y)) r++;
    fill_span(f->p, y, l, r, f->colour);
    for(k = l; k <= r; k++) {
        i = (size_t)y * (size_t)f->p->w + k;
        f->done[i >> 3] |= (unsigned char)(1u << (i & 7));
    }
    f->filled += r - l + 1;
    *a = l;
    *b = r;
}

static void drain(struct filling *f) {
    struct run s;
    int x, a, b, ny;
    while(f->n) {
        s = f->stack[--f->n];
        ny = s.y + s.dy;
        for(x = s.x0; x <= s.x1; x++) {
            if(!is_target(f, x, ny)) continue;
            fill_run(f, x, ny, &a, &b);
            push(f, ny, a, b, s.dy);
            if(a < s.x0) push(f, ny, a, s.x0 - 1, -s.dy);  /* past the run it came from: back the other way too */
            if(b > s.x1) push(f, ny, s.x1 + 1, b, -s.dy);
            x = b;
        }
    }
}

/* a pass over the rows marked as cut short, for runs of the target colour beside a pixel made by this fill: 1 when it
   found one. A row marked while the pass goes on is read by this pass if it is still ahead, else by the next */
static int rescan(struct filling *f) {
    int x, y, a, b, k, found = 0, w = f->p->w, h = f->p->h, near;
    for(y = 0; y < h; y++) {
        if(!f->cut[y]) continue;
        f->cut[y] = 0;
        f->p->fill_rows++;
        for(x = 0; x < w; x++) {
            if(!is_target(f, x, y)) continue;
            for(b = x; b < w - 1 && is_target(f, b + 1, y); b++)
                ;
            near = (x > 0 && is_done(f, x - 1, y)) || (b < w - 1 && is_done(f, b + 1, y));
            for(k = x; !near && k <= b; k++)
                near = (y > 0 && is_done(f, k, y - 1)) || (y < h - 1 && is_done(f, k, y + 1));
            if(near) {
                fill_run(f, x, y, &a, &b);
                push(f, y, a, b, 1);
                push(f, y, a, b, -1);
                found = 1;
            }
            x = b;
        }
    }
    return found;
}

int flood_fill(struct paint *p, int x, int y, unsigned c) {
    struct filling f;
    size_t bits, stack;
    int a, b;
    p->fill_rows = 0;
    if(x < 0 || y < 0 || x >= p->w || y >= p->h) return 0;
    memset(&f, 0, sizeof(f));
    f.p = p;
    f.colour = c;
    f.target = p->px[(size_t)y * (size_t)p->w + x];
    if(f.target == c) return 0;
    f.cap = p->fill_stack > 0 ? p->fill_stack : 1;
    bits = ((size_t)p->w * (size_t)p->h + 7) / 8;
    stack = (size_t)f.cap * sizeof(struct run);
    if(pm_room(p, stack + bits + (size_t)p->h)) {       /* all three, or none: the history gives way only for them */
        f.stack = (struct run *)pm_alloc(p, stack);
        if(f.stack) f.done = (unsigned char *)pm_alloc(p, bits);
        if(f.done) f.cut = (unsigned char *)pm_alloc(p, (size_t)p->h);
    }
    if(!f.cut) {
        pm_free(p, f.done, bits);
        pm_free(p, f.stack, stack);
        say(p, "Not enough memory to fill");
        return -1;
    }
    memset(f.done, 0, bits);
    memset(f.cut, 0, (size_t)p->h);
    fill_run(&f, x, y, &a, &b);
    push(&f, y, a, b, 1);
    push(&f, y, a, b, -1);
    for(;;) {
        drain(&f);
        if(!f.over) break;
        f.over = 0;
        if(!rescan(&f)) break;
    }
    pm_free(p, f.cut, (size_t)p->h);
    pm_free(p, f.done, bits);
    pm_free(p, f.stack, stack);
    return f.filled > INT_MAX ? INT_MAX : (int)f.filled;
}

/**** text ****/

int text_lines(const struct ptext *t, int *start, int *len, int max) {
    int i, n = 0;
    start[0] = 0;
    len[0] = 0;
    for(i = 0; i < t->n; i++) {
        if(t->cell[i] != PAINT_TEXT_NL) {
            len[n]++;
            continue;
        }
        if(n + 1 >= max) break;
        n++;
        start[n] = i + 1;
        len[n] = 0;
    }
    return n + 1;
}

void text_size(const struct ptext *t, int scale, int *w, int *h) {
    int start[PAINT_TEXT_LINES], len[PAINT_TEXT_LINES], i, n = text_lines(t, start, len, PAINT_TEXT_LINES), cols = 0;
    for(i = 0; i < n; i++) if(len[i] > cols) cols = len[i];
    *w = cols * 8 * scale;
    *h = n * 16 * scale;
}

/* each character's pixels as runs in its rows, scale x scale each */
void text_draw(struct paint *p, const struct ptext *t, int scale, unsigned c) {
    int start[PAINT_TEXT_LINES], len[PAINT_TEXT_LINES], n = text_lines(t, start, len, PAINT_TEXT_LINES);
    int line, i, g, bit, end, k, x, y;
    unsigned char bits;
    for(line = 0; line < n; line++)
        for(i = 0; i < len[line]; i++) {
            x = t->x + i * 8 * scale;
            for(g = 0; g < 16; g++) {
                bits = font8x16[t->cell[start[line] + i] & 0xFF][g];
                y = t->y + (line * 16 + g) * scale;
                for(bit = 0; bit < 8; bit = end) {
                    if(!(bits & (0x80 >> bit))) {
                        end = bit + 1;
                        continue;
                    }
                    for(end = bit; end < 8 && bits & (0x80 >> end); end++)
                        ;
                    for(k = 0; k < scale; k++)
                        fill_span(p, y + k, x + bit * scale, x + end * scale - 1, c);
                }
            }
        }
}
