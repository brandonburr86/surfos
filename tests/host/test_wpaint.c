/* Host tests for Paint's model (user/wpaint/: canvas.c, history.c, draw.c, tools.c, view.c), built as it is with the
   host's C library, the console's real font (driver/font8x16.c) and code page 437 (driver/cp437.c). They check:
   the canvas's sizes and the memory bound; every tool's pixels (the pencil, the brush's discs at each size, the
   eraser's squares, lines, rectangles and ellipses outlined and filled, Shift's squares and 45-degree lines); the
   flood fill against a fill done here the slow way, also with its stack nearly gone so that it scans again; text in
   the console's font at a scale, UTF-8 typed, its limits; the colour picker; the selection: moving, Copy, Cut, Paste,
   Delete, the arrows, past the canvas's edges, Undo while it floats; undo and redo exactly, pixel for pixel, at the
   tiles' edges too, the step and byte bounds, a change too large to keep, the redo dropped by a new change and kept by
   an empty one, what was saved; a new size and its undo; the view: every zoom both ways, zooming about a point,
   scrolling, fitting, what the view shows and what it says changed; the colours as typed; the status line; and many
   random actions, each undone and then redone, with the memory counted checked after each. Run natively under
   AddressSanitizer and UndefinedBehaviorSanitizer on the Mac; built and run by `make unittest`. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "paint.h"
#include "ppriv.h"

static int checks, failed;
#define CHECK(x) do { checks++; if(!(x)) { failed++; printf("FAIL %s line %d: %s\n", __FILE__, __LINE__, #x); } } while(0)

#define BLACK 0x000000u
#define WHITE 0xFFFFFFu
#define RED   0xC03030u
#define BLUE  0x3060C0u

static struct paint P, *p = &P;

static unsigned px(int x, int y) {
    return p->px[(size_t)y * (size_t)p->w + x];
}

static void setpx(int x, int y, unsigned c) {       /* behind the history's back: only for a canvas's setting up */
    p->px[(size_t)y * (size_t)p->w + x] = c;
}

static long count(unsigned c) {
    long n = 0;
    size_t i;
    for(i = 0; i < (size_t)p->w * (size_t)p->h; i++) n += p->px[i] == c;
    return n;
}

static long count_in(unsigned c, int x0, int y0, int x1, int y1) {
    long n = 0;
    int x, y;
    for(y = y0; y <= y1; y++)
        for(x = x0; x <= x1; x++) n += px(x, y) == c;
    return n;
}

/* a copy of the canvas, and whether the canvas is it again */
struct snap { int w, h; unsigned *px; };

static struct snap snap(void) {
    struct snap s;
    s.w = p->w;
    s.h = p->h;
    s.px = (unsigned *)malloc(canvas_bytes(p->w, p->h));
    memcpy(s.px, p->px, canvas_bytes(p->w, p->h));
    return s;
}

static int same(struct snap s) {
    return s.w == p->w && s.h == p->h && !memcmp(s.px, p->px, canvas_bytes(s.w, s.h));
}

static void unsnap(struct snap *s) {
    free(s->px);
    s->px = NULL;
}

/* the memory the model says it holds is what it holds: the canvas and its rows, the steps, the shared pixels */
static size_t pbuf_bytes(const struct pbuf *b) {
    return b ? sizeof(*b) + canvas_bytes(b->w, b->h) : 0;
}

static int counted(void) {
    size_t steps = 0, n;
    int i;
    for(i = 0; i < p->nsteps; i++) steps += p->steps[i]->bytes;
    if(p->open && p->open != &p->nostep) steps += p->open->bytes;
    n = canvas_bytes(p->w, p->h) + (size_t)p->h * 2 * sizeof(int) + steps + pbuf_bytes(p->clip);
    if(p->sel.buf && p->sel.buf != p->clip) n += pbuf_bytes(p->sel.buf);
    return n == p->mem && steps == p->undo_bytes && p->mem <= p->mem_max;
}

static void fresh(int w, int h) {
    paint_free(p);
    CHECK(paint_init(p, w, h) == 0);
    paint_view_size(p, 400, 300);
}

static void tool(int t) { paint_set_tool(p, t); }

static void drag(int button, int x0, int y0, int x1, int y1) {
    paint_press(p, x0, y0, button, 0);
    paint_move(p, (x0 + x1) / 2, (y0 + y1) / 2, 0);
    paint_move(p, x1, y1, 0);
    paint_release(p, x1, y1, 0);
}

static void click(int x, int y) {
    paint_press(p, x, y, PAINT_LEFT, 0);
    paint_release(p, x, y, 0);
}

static void type(const char *s) {
    while(*s) paint_key(p, (unsigned char)*s++, 0);
}

static void rect(int tool_, int filled, int size, unsigned c, int x0, int y0, int x1, int y1) {
    paint_set_colour(p, 0, c);
    paint_set_filled(p, filled);
    paint_set_size(p, size);
    tool(tool_);
    drag(PAINT_LEFT, x0, y0, x1, y1);
}

/**** the canvas and its memory ****/

static void canvas(void) {
    fresh(0, 0);
    CHECK(p->w == PAINT_NEW_W && p->h == PAINT_NEW_H && count(WHITE) == (long)PAINT_NEW_W * PAINT_NEW_H);
    CHECK(!paint_modified(p) && !paint_can_undo(p) && !paint_can_redo(p) && counted());
    CHECK(p->tool == PAINT_BRUSH && p->fg == BLACK && p->bg == WHITE);
    CHECK(paint_new(p, 0, 10) < 0 && strstr(p->msg, "1 to 4096") && p->w == PAINT_NEW_W);
    CHECK(paint_new(p, 4097, 10) < 0 && paint_new(p, 10, -1) < 0 && p->w == PAINT_NEW_W && p->h == PAINT_NEW_H);
    CHECK(paint_new(p, 4096, 1) == 0 && p->w == 4096 && p->h == 1 && p->tiles_x == 64 && p->tiles_y == 1);
    CHECK(paint_new(p, 1, 4096) == 0 && p->w == 1 && p->tiles_y == 64 && counted());
    CHECK(paint_new(p, 130, 70) == 0 && p->tiles_x == 3 && p->tiles_y == 2 && count(WHITE) == 130 * 70);
    /* the bound: a canvas that would not fit is refused, and the old one stays */
    p->mem_max = p->mem + 1000;
    CHECK(paint_new(p, 200, 200) < 0 && strstr(p->msg, "Not enough memory") && p->w == 130 && counted());
    p->mem_max = PAINT_MEM_MAX;
    /* a picture taken as the canvas: its block counted from then on */
    {
        unsigned *pic = (unsigned *)malloc(canvas_bytes(30, 20));
        int i;
        for(i = 0; i < 600; i++) pic[i] = (unsigned)i;
        CHECK(paint_adopt(p, pic, 30, 20) == 0 && p->w == 30 && p->h == 20 && px(5, 1) == 35 && counted());
        CHECK(!paint_modified(p) && !paint_can_undo(p));
        pic = (unsigned *)malloc(canvas_bytes(5000, 1));
        CHECK(paint_adopt(p, pic, 5000, 1) < 0 && p->w == 30);   /* not taken: still the caller's */
        free(pic);
    }
    /* what the codecs read and write: 4096 a side, 16,000,000 pixels in all */
    CHECK(paint_new(p, 4096, 3907) < 0 && strstr(p->msg, "16,000,000") && p->w == 30);
    CHECK(paint_resize(p, 4001, 4000) < 0 && p->w == 30 && paint_new(p, 4096, 3906) == 0 && counted());
    /* the largest canvas, 64,000,000 bytes: made, cleared and undone within the bound; a new size of nearly as much
       beside it, and a selection of all of it lifted and copied, still fit */
    CHECK(paint_new(p, 4000, 4000) == 0 && p->mem >= 64000000u && counted());
    paint_set_colour(p, 1, RED);
    CHECK(paint_clear(p) && px(3999, 3999) == RED && paint_can_undo(p) && counted());
    CHECK(paint_undo(p) && px(0, 0) == WHITE && px(3999, 3999) == WHITE && counted());
    CHECK(paint_resize(p, 3999, 4000) == 0 && p->w == 3999 && counted() && paint_undo(p) && p->w == 4000);
    CHECK(paint_select_all(p) && paint_key(p, PAINT_KEY_RIGHT, 0) && p->sel.buf && paint_copy(p) && p->clip == p->sel.buf);
    paint_put_down(p);
    CHECK(px(0, 0) == RED && px(1, 0) == WHITE && counted() && p->mem <= PAINT_MEM_MAX);
    paint_set_colour(p, 1, WHITE);
    paint_free(p);
    CHECK(p->mem == 0);
}

/**** the tools ****/

static void pencil_brush_eraser(void) {
    int size, r, l, rr, x, y, k, d, ok;
    fresh(200, 120);
    tool(PAINT_PENCIL);
    drag(PAINT_LEFT, 10, 10, 20, 10);
    CHECK(count(BLACK) == 11 && px(10, 10) == BLACK && px(20, 10) == BLACK && px(21, 10) == WHITE);
    CHECK(paint_modified(p) && paint_can_undo(p) && !strcmp(paint_undo_what(p), "Pencil"));
    drag(PAINT_RIGHT, 10, 10, 20, 10);                  /* the right button: the second colour */
    CHECK(count(BLACK) == 0);
    /* the brush's dabs: discs, the same as an ellipse in a square box */
    for(size = 0; size < PAINT_SIZES; size++) {
        fresh(200, 120);
        paint_set_size(p, size);
        tool(PAINT_BRUSH);
        click(100, 60);
        d = paint_sizes[size];
        ok = 1;
        for(r = 0; r < d; r++) {
            dab_row(d, 1, r, &l, &rr);
            y = 60 - (d - 1) / 2 + r;
            for(x = 100 - 30; x <= 100 + 30; x++)
                if((px(x, y) == BLACK) != (x >= 100 + l && x <= 100 + rr)) ok = 0;
        }
        CHECK(ok && count_in(BLACK, 0, 0, 199, 60 - (d - 1) / 2 - 1) == 0);
        CHECK(count(BLACK) == (d == 1 ? 1 : d == 2 ? 4 : d == 3 ? 9 : d == 5 ? 21 : count(BLACK)));
    }
    /* discs: top and bottom alike, left and right alike in their box, within it */
    for(ok = 1, d = 1; d <= PAINT_DAB_MAX; d++)
        for(r = 0; r < d; r++) {
            int l2, r2, up = (d - 1) / 2;
            dab_row(d, 1, r, &l, &rr);
            dab_row(d, 1, d - 1 - r, &l2, &r2);
            if(l != l2 || rr != r2 || l + up != d - 1 - up - rr || l < -up || rr > d - 1 - up || l > rr) ok = 0;
        }
    CHECK(ok);
    /* a stroke: every point of the line under the brush, no gaps, both ends */
    fresh(200, 120);
    paint_set_size(p, 3);                               /* 5 pixels */
    tool(PAINT_BRUSH);
    paint_press(p, 20, 20, PAINT_LEFT, 0);
    paint_move(p, 120, 70, 0);
    paint_move(p, 180, 20, 0);
    paint_release(p, 180, 20, 0);
    for(k = 0, ok = 1; k <= 100; k++) {
        if(px(20 + k, 20 + k / 2) != BLACK) ok = 0;
        if(k <= 60 && px(120 + k, 70 - k * 50 / 60) != BLACK) ok = 0;
    }
    CHECK(ok && px(20, 18) == BLACK && px(20, 17) == WHITE && px(180, 22) == BLACK && !strcmp(paint_undo_what(p), "Brush"));
    CHECK(paint_undo(p) && count(BLACK) == 0 && paint_redo(p) && count(BLACK) > 400);
    /* the eraser: squares of the second colour, with either button */
    fresh(200, 120);
    paint_set_colour(p, 1, RED);
    paint_set_size(p, 4);                               /* 8 pixels */
    tool(PAINT_ERASER);
    click(50, 50);
    CHECK(count(RED) == 64 && px(47, 47) == RED && px(54, 54) == RED && px(46, 50) == WHITE && px(55, 50) == WHITE);
    drag(PAINT_RIGHT, 100, 50, 100, 50);
    CHECK(count(RED) == 128);
    /* far outside: drawn where it reaches, nothing else, no fault */
    tool(PAINT_BRUSH);
    drag(PAINT_LEFT, -100000, -5, 100000, 5);
    CHECK(px(0, 0) == BLACK && px(199, 0) == BLACK && px(100, 5) == WHITE);
    drag(PAINT_LEFT, -50000, -50000, -40000, 50000);
    CHECK(counted());
}

static int connected8(unsigned c, int x0, int y0, int x1, int y1) {
    /* the pixels of colour c in the box are one 8-connected piece */
    int w = x1 - x0 + 1, h = y1 - y0 + 1, *queue, n = 0, head = 0, total = 0, x, y, dx, dy, k, found = 0;
    unsigned char *seen = (unsigned char *)calloc((size_t)w * h, 1);
    queue = (int *)malloc(sizeof(int) * (size_t)w * h);
    for(y = y0; y <= y1; y++)
        for(x = x0; x <= x1; x++)
            if(px(x, y) == c) {
                total++;
                if(!n) {
                    queue[n++] = (y - y0) * w + (x - x0);
                    seen[(y - y0) * w + (x - x0)] = 1;
                }
            }
    while(head < n) {
        k = queue[head++];
        found++;
        for(dy = -1; dy <= 1; dy++)
            for(dx = -1; dx <= 1; dx++) {
                x = k % w + dx;
                y = k / w + dy;
                if(x < 0 || y < 0 || x >= w || y >= h || seen[y * w + x] || px(x0 + x, y0 + y) != c) continue;
                seen[y * w + x] = 1;
                queue[n++] = y * w + x;
            }
    }
    free(seen);
    free(queue);
    return found == total;
}

static void shapes(void) {
    int w, h, t, ok, y, x, sym, size;
    fresh(200, 120);
    /* lines: both ends, and a preview that is the line itself */
    paint_set_size(p, 0);
    tool(PAINT_LINE);
    paint_press(p, 10, 10, PAINT_LEFT, 0);
    paint_move(p, 60, 35, 0);
    CHECK(count(BLACK) == 0 && p->shape.on && !paint_can_redo(p));
    {
        unsigned view[400 * 300];
        int n = 0;
        paint_render(p, view, 400, 0, 0, 400, 300);
        for(x = 0; x < 400 * 300; x++) n += view[x] == BLACK;
        paint_release(p, 60, 35, 0);
        CHECK(n == count(BLACK) && n == 51);            /* x-major: a pixel a column */
    }
    CHECK(px(10, 10) == BLACK && px(60, 35) == BLACK && !p->shape.on && !strcmp(paint_undo_what(p), "Line"));
    /* Shift: 45 degrees, or flat */
    fresh(200, 120);
    paint_set_size(p, 0);
    tool(PAINT_LINE);
    paint_press(p, 10, 10, PAINT_LEFT, 0);
    paint_move(p, 40, 37, PAINT_SHIFT);
    paint_release(p, 40, 37, PAINT_SHIFT);
    CHECK(px(40, 40) == BLACK && px(25, 25) == BLACK && count(BLACK) == 31);
    paint_press(p, 10, 100, PAINT_LEFT, 0);
    paint_release(p, 60, 106, PAINT_SHIFT);
    CHECK(px(60, 100) == BLACK && px(60, 106) == WHITE);
    /* rectangles: the outline's pixels, at widths 1 and 3, and filled */
    fresh(200, 120);
    rect(PAINT_RECT, 0, 0, BLACK, 10, 10, 29, 19);
    CHECK(count(BLACK) == 2 * 20 + 2 * 8 && px(10, 10) == BLACK && px(29, 19) == BLACK && px(11, 11) == WHITE);
    rect(PAINT_RECT, 0, 2, RED, 50, 10, 69, 19);
    CHECK(count(RED) == 60 + 60 + 4 * 6 && px(52, 12) == RED && px(53, 13) == WHITE);
    rect(PAINT_RECT, 1, 2, BLUE, 109, 29, 90, 20);      /* dragged backwards */
    CHECK(count(BLUE) == 200 && px(90, 20) == BLUE && px(109, 29) == BLUE);
    rect(PAINT_RECT, 0, 7, BLUE, 120, 40, 150, 60);     /* thicker than half of it: filled */
    CHECK(count_in(BLUE, 120, 40, 150, 60) == 31 * 21);
    paint_press(p, 160, 80, PAINT_LEFT, 0);             /* Shift: a square */
    paint_release(p, 170, 100, PAINT_SHIFT);
    CHECK(px(180, 100) == BLUE && px(160, 80) == BLUE && px(181, 100) == WHITE);
    /* ellipses: symmetric, inside their box, touching its four sides, outlines one 8-connected piece */
    ok = sym = 1;
    for(w = 1; w <= 40 && ok; w += 3)
        for(h = 1; h <= 40 && ok; h += 4)
            for(t = 0; t < 3; t++) {
                fresh(60, 60);
                rect(PAINT_ELLIPSE, t == 2, t, BLACK, 5, 5, 5 + w - 1, 5 + h - 1);
                if(count_in(BLACK, 5, 5, 5 + w - 1, 5 + h - 1) != count(BLACK)) ok = 0;
                for(y = 5; y < 5 + h; y++)
                    for(x = 5; x < 5 + w; x++)
                        if(px(x, y) != px(5 + w - 1 - (x - 5), y) || px(x, y) != px(x, 5 + h - 1 - (y - 5))) sym = 0;
                if(!count_in(BLACK, 5, 5, 5, 5 + h - 1) || !count_in(BLACK, 5 + w - 1, 5, 5 + w - 1, 5 + h - 1) ||
                   !count_in(BLACK, 5, 5, 5 + w - 1, 5) || !count_in(BLACK, 5, 5 + h - 1, 5 + w - 1, 5 + h - 1)) ok = 0;
                if(!connected8(BLACK, 5, 5, 5 + w - 1, 5 + h - 1)) ok = 0;
                if(!ok || !sym) printf("  ellipse %d x %d, %d: ok %d sym %d\n", w, h, t, ok, sym);
                if(t == 2 && w > 2 && h > 2 && px(5 + w / 2, 5 + h / 2) != BLACK) ok = 0;
                if(t == 0 && w > 4 && h > 4 && px(5 + w / 2, 5 + h / 2) != WHITE) ok = 0;
            }
    CHECK(ok && sym);
    /* a 4 x 4 circle is rounded; Shift makes a circle of a drag */
    fresh(60, 60);
    rect(PAINT_ELLIPSE, 1, 0, BLACK, 10, 10, 13, 13);
    CHECK(count(BLACK) == 12 && px(10, 10) == WHITE && px(11, 10) == BLACK && px(10, 11) == BLACK);
    paint_press(p, 20, 20, PAINT_LEFT, 0);
    paint_release(p, 40, 30, PAINT_SHIFT);
    CHECK(px(30, 40) == BLACK && px(30, 20) == BLACK && px(20, 30) == BLACK && px(40, 30) == BLACK);
    /* the largest outline, past every edge: clipped, no fault */
    fresh(100, 100);
    for(size = 0; size < PAINT_SIZES; size++) {
        rect(PAINT_ELLIPSE, 0, size, RED, -8000, -8000, 8000, 8000);
        rect(PAINT_LINE, 0, size, BLUE, -9000, 50, 12000, 50);
    }
    CHECK(px(50, 50) == BLUE && counted());
}

/* the fill the slow way: a queue of pixels, 4-connected */
static void fill_ref(unsigned *pix, int w, int h, int x, int y, unsigned c) {
    unsigned target = pix[y * w + x];
    int *queue, n = 0, head = 0, k, nx, ny, d;
    static const int dx[4] = { 1, -1, 0, 0 }, dy[4] = { 0, 0, 1, -1 };
    if(target == c) return;
    queue = (int *)malloc(sizeof(int) * (size_t)w * h);
    pix[y * w + x] = c;
    queue[n++] = y * w + x;
    while(head < n) {
        k = queue[head++];
        for(d = 0; d < 4; d++) {
            nx = k % w + dx[d];
            ny = k / w + dy[d];
            if(nx < 0 || ny < 0 || nx >= w || ny >= h || pix[ny * w + nx] != target) continue;
            pix[ny * w + nx] = c;
            queue[n++] = ny * w + nx;
        }
    }
    free(queue);
}

static unsigned seed = 12345;
static unsigned rnd(unsigned n) {
    seed = seed * 1103515245u + 12345u;
    return (seed >> 8) % n;
}

static void fill(void) {
    int round, stack, x, y, ok;
    struct snap s;
    fresh(100, 100);
    rect(PAINT_RECT, 0, 0, BLACK, 20, 20, 60, 60);
    paint_set_colour(p, 0, RED);
    tool(PAINT_FILL);
    click(40, 40);
    CHECK(count(RED) == 39 * 39 && px(20, 20) == BLACK && px(10, 10) == WHITE && !strcmp(paint_undo_what(p), "Fill"));
    s = snap();
    click(40, 40);                                      /* the same colour again: nothing, and no step */
    CHECK(same(s) && !strcmp(paint_undo_what(p), "Fill") && paint_undo(p) && count(RED) == 0);
    CHECK(paint_can_redo(p));
    click(-5, 40);                                      /* outside the canvas: nothing, and the redo stays */
    CHECK(paint_can_redo(p) && count(RED) == 0);
    unsnap(&s);
    /* a diagonal wall stops a fill (4-connected), and a region of the same colour apart is left as it is */
    fresh(50, 50);
    for(x = 0; x < 50; x++) setpx(x, x, BLACK);
    paint_set_colour(p, 0, BLUE);
    tool(PAINT_FILL);
    click(40, 5);
    CHECK(px(5, 40) == WHITE && px(40, 5) == BLUE && count(BLUE) == 49 * 50 / 2);
    paint_set_colour(p, 0, RED);
    click(0, 0);                                        /* one black pixel's run: the wall is one pixel at a time */
    CHECK(px(0, 0) == RED && px(1, 1) == BLACK);
    /* random walls, against the fill the slow way, with stacks from roomy down to a single run */
    for(round = 0; round < 60; round++) {
        int w = 20 + (int)rnd(60), h = 10 + (int)rnd(50);
        unsigned *ref;
        fresh(w, h);
        for(y = 0; y < h; y++)
            for(x = 0; x < w; x++)
                if(rnd(100) < (round % 3 == 0 ? 45u : round % 3 == 1 ? 30u : 55u)) setpx(x, y, BLACK);
        stack = round % 4 == 0 ? PAINT_FILL_STACK : round % 4 == 1 ? 1 : round % 4 == 2 ? 2 : 7;
        p->fill_stack = stack;
        x = (int)rnd((unsigned)w);
        y = (int)rnd((unsigned)h);
        ref = (unsigned *)malloc(canvas_bytes(w, h));
        memcpy(ref, p->px, canvas_bytes(w, h));
        fill_ref(ref, w, h, x, y, RED);
        paint_set_colour(p, 0, RED);
        tool(PAINT_FILL);
        click(x, y);
        ok = !memcmp(ref, p->px, canvas_bytes(w, h));
        CHECK(ok && counted());
        if(!ok) printf("  fill %d: %d x %d at %d, %d, stack %d\n", round, w, h, x, y, stack);
        CHECK(paint_undo(p) && paint_redo(p) && !memcmp(ref, p->px, canvas_bytes(w, h)));
        free(ref);
    }
    /* a comb: every column a run of its own, which a stack of one cannot hold */
    fresh(301, 101);
    for(x = 1; x < 300; x += 2)
        for(y = x % 4 == 1 ? 0 : 1; y < (x % 4 == 1 ? 100 : 101); y++) setpx(x, y, BLACK);
    {
        unsigned *ref = (unsigned *)malloc(canvas_bytes(301, 101));
        memcpy(ref, p->px, canvas_bytes(301, 101));
        fill_ref(ref, 301, 101, 0, 0, BLUE);
        p->fill_stack = 1;
        paint_set_colour(p, 0, BLUE);
        tool(PAINT_FILL);
        click(0, 0);
        CHECK(!memcmp(ref, p->px, canvas_bytes(301, 101)) && count(BLUE) == 301 * 101 - count(BLACK));
        free(ref);
    }
    /* no memory for the fill's work: refused, nothing changed */
    fresh(100, 100);
    p->fill_stack = PAINT_FILL_STACK;
    p->mem_max = p->mem + 1000;
    tool(PAINT_FILL);
    click(50, 50);
    CHECK(count(WHITE) == 10000 && strstr(p->msg, "Not enough memory") && counted());
    p->mem_max = PAINT_MEM_MAX;
    /* a large canvas, whole */
    fresh(2048, 2048);
    paint_set_colour(p, 0, RED);
    tool(PAINT_FILL);
    click(1000, 1000);
    CHECK(count(RED) == 2048L * 2048 && counted());
    CHECK(paint_undo(p) && count(WHITE) == 2048L * 2048);
}

static int glyph_at(int x0, int y0, unsigned char ch, int scale, unsigned c) {
    int x, y;
    for(y = 0; y < 16 * scale; y++)
        for(x = 0; x < 8 * scale; x++)
            if((px(x0 + x, y0 + y) == c) != !!(font8x16[ch][y / scale] & (0x80 >> (x / scale)))) return 0;
    return 1;
}

static void text(void) {
    int i;
    fresh(300, 200);
    tool(PAINT_TEXT);
    paint_set_text_scale(p, 1);
    click(10, 10);
    CHECK(paint_typing(p) && !strcmp(paint_undo_what(p), "Text") && paint_modified(p));
    type("Hi");
    CHECK(count(BLACK) == 0);                           /* shown by the view until it is put down */
    paint_key(p, '\r', 0);
    type("\xC3\xA9");                                   /* é, U+00E9: code page 437's 0x82 */
    type("\xE2\x82\xAC");                               /* €, which the console has not: ? */
    type("\x80");                                       /* a byte that starts nothing: ? */
    paint_key(p, 3, 0);                                 /* Ctrl+C: not text */
    CHECK(p->text.n == 6 && p->text.cell[2] == PAINT_TEXT_NL && p->text.cell[3] == 0x82 && p->text.cell[4] == '?' &&
          p->text.cell[5] == '?');
    paint_key(p, 8, 0);                                 /* Backspace */
    CHECK(p->text.n == 5);
    paint_set_tool(p, PAINT_BRUSH);                     /* another tool: put down */
    CHECK(!paint_typing(p) && glyph_at(10, 10, 'H', 1, BLACK) && glyph_at(18, 10, 'i', 1, BLACK));
    CHECK(glyph_at(10, 26, 0x82, 1, BLACK) && glyph_at(18, 26, '?', 1, BLACK) && !strcmp(paint_undo_what(p), "Text"));
    CHECK(paint_undo(p) && count(BLACK) == 0 && paint_redo(p) && glyph_at(10, 10, 'H', 1, BLACK));
    /* at a scale, in the first colour; a click elsewhere puts it down and starts another */
    fresh(300, 200);
    tool(PAINT_TEXT);
    paint_set_text_scale(p, 3);
    paint_set_colour(p, 0, RED);
    click(100, 50);
    type("A");
    click(10, 150);
    CHECK(glyph_at(100, 50, 'A', 3, RED) && paint_typing(p) && p->text.x == 10 && p->text.n == 0);
    type("zz");
    paint_key(p, 27, 0);                                /* Esc: dropped */
    CHECK(!paint_typing(p) && count_in(RED, 0, 140, 299, 199) == 0);
    click(10, 150);
    type("q");
    CHECK(paint_undo(p) && !paint_typing(p) && count_in(RED, 0, 140, 299, 199) == 0);   /* Undo: dropped */
    /* limits */
    click(0, 0);
    for(i = 0; i < 300; i++) paint_key(p, 'x', 0);
    CHECK(p->text.n == PAINT_TEXT_MAX && strstr(p->msg, "full"));
    paint_key(p, 27, 0);
    click(0, 0);
    for(i = 0; i < 30; i++) paint_key(p, '\r', 0);
    CHECK(p->text.n == PAINT_TEXT_LINES - 1);
    paint_put_down(p);
    CHECK(!paint_typing(p) && counted());
    /* past the canvas's edges: what is on it is drawn */
    fresh(40, 40);
    tool(PAINT_TEXT);
    paint_set_text_scale(p, 8);
    click(-20, 0);
    type("MW");
    paint_put_down(p);
    CHECK(count(BLACK) > 0 && counted());
}

static void picker(void) {
    fresh(100, 100);
    setpx(5, 5, 0x123456);
    setpx(6, 5, 0x654321);
    tool(PAINT_FILL);
    tool(PAINT_PICKER);
    click(5, 5);
    CHECK(p->fg == 0x123456 && p->tool == PAINT_FILL && strstr(p->msg, "#123456"));
    tool(PAINT_PICKER);
    paint_press(p, 6, 5, PAINT_RIGHT, 0);
    paint_release(p, 6, 5, 0);
    CHECK(p->bg == 0x654321 && p->fg == 0x123456 && !paint_can_undo(p));
    tool(PAINT_PICKER);
    click(500, 5);                                      /* outside: nothing taken, the picker stays */
    CHECK(p->tool == PAINT_PICKER && p->fg == 0x123456);
}

/**** the selection ****/

static void selection(void) {
    struct snap s0, s1;
    unsigned view[400 * 300];
    int x, y, vx, vy;
    fresh(100, 100);
    rect(PAINT_RECT, 1, 0, BLACK, 10, 10, 19, 19);
    paint_saved(p);
    s0 = snap();
    tool(PAINT_SELECT);
    drag(PAINT_LEFT, 10, 10, 19, 19);
    CHECK(paint_has_selection(p) && p->sel.x == 10 && p->sel.w == 10 && p->sel.h == 10 && !p->sel.buf);
    /* moving it: lifted, the second colour behind */
    paint_press(p, 15, 15, PAINT_LEFT, 0);
    paint_move(p, 30, 15, 0);
    paint_move(p, 45, 15, 0);
    paint_release(p, 45, 15, 0);
    CHECK(p->sel.buf && p->sel.lifted && p->sel.x == 40 && count_in(BLACK, 10, 10, 19, 19) == 0);
    CHECK(count(BLACK) == 0 && paint_modified(p) && !strcmp(paint_undo_what(p), "Move") && !paint_can_redo(p));
    paint_render(p, view, 400, 0, 0, 400, 300);         /* the view shows it where it floats */
    paint_to_view(p, 45, 15, &vx, &vy);
    CHECK(view[vy * 400 + vx] == BLACK);
    paint_key(p, PAINT_KEY_RIGHT, 0);
    paint_key(p, PAINT_KEY_DOWN, PAINT_SHIFT);
    CHECK(p->sel.x == 41 && p->sel.y == 20);
    /* Undo while it floats: back where it was, still selected */
    CHECK(paint_undo(p) && same(s0) && !p->sel.buf && p->sel.on && p->sel.x == 10 && !paint_modified(p) && counted());
    /* moved and put down: one step */
    paint_press(p, 12, 12, PAINT_LEFT, 0);
    paint_release(p, 52, 62, 0);
    paint_key(p, '\r', 0);
    CHECK(!p->sel.buf && p->sel.on && count_in(BLACK, 50, 60, 59, 69) == 100 && count(BLACK) == 100);
    s1 = snap();
    CHECK(paint_undo(p) && same(s0) && paint_redo(p) && same(s1));
    unsnap(&s1);
    /* Copy, Paste at the view's top left, dragged, put down by a click outside (a drag inside the selection would
       move it: Esc first) */
    paint_cancel(p);
    CHECK(!p->sel.on && count(BLACK) == 100);
    tool(PAINT_SELECT);
    drag(PAINT_LEFT, 50, 60, 59, 69);
    CHECK(paint_copy(p) && p->clip && p->clip->w == 10 && strstr(p->msg, "Copied 10 x 10"));
    CHECK(paint_paste(p) && p->sel.buf == p->clip && p->clip->refs == 2 && p->sel.x == 0 && p->sel.y == 0);
    CHECK(count(BLACK) == 100 && paint_modified(p) && !strcmp(paint_undo_what(p), "Paste") && counted());
    paint_press(p, 5, 5, PAINT_LEFT, 0);
    paint_release(p, 85, 5, 0);
    click(30, 90);                                      /* outside: put down */
    CHECK(count_in(BLACK, 80, 0, 89, 9) == 100 && count(BLACK) == 200 && !p->sel.on && p->clip->refs == 1);
    CHECK(paint_undo(p) && count(BLACK) == 100 && paint_redo(p) && count(BLACK) == 200);
    /* a paste dropped by Undo; one past the edges, put down as far as it is on the canvas */
    paint_paste(p);
    CHECK(paint_undo(p) && !p->sel.on && count(BLACK) == 200 && p->clip->refs == 1);
    paint_paste(p);
    for(x = 0; x < 20; x++) paint_key(p, PAINT_KEY_LEFT, PAINT_SHIFT);
    CHECK(p->sel.x == -9);                              /* a pixel of it stays on the canvas */
    paint_cancel(p);                                    /* Esc: put down, nothing selected */
    CHECK(!p->sel.on && px(0, 0) == BLACK && count(BLACK) == 210 && counted());
    /* Cut and Delete leave the second colour; Delete of what floats; Select all */
    paint_set_colour(p, 1, RED);
    tool(PAINT_SELECT);
    drag(PAINT_LEFT, 80, 0, 89, 9);
    CHECK(paint_cut(p) && count_in(RED, 80, 0, 89, 9) == 100 && !p->sel.on && strstr(p->msg, "Cut 10 x 10"));
    drag(PAINT_LEFT, 50, 60, 59, 69);
    CHECK(paint_delete(p) && count_in(RED, 50, 60, 59, 69) == 100 && !strcmp(paint_undo_what(p), "Delete"));
    paint_paste(p);
    CHECK(paint_delete(p) && !p->sel.on && count_in(BLACK, 0, 0, 9, 9) == 10);  /* a paste deleted: nothing changed */
    drag(PAINT_LEFT, 0, 0, 0, 0);
    CHECK(!p->sel.on && paint_copy(p) && strstr(p->msg, "Select a part"));
    CHECK(paint_select_all(p) && p->sel.w == 100 && p->sel.h == 100);
    paint_key(p, PAINT_KEY_DOWN, 0);
    CHECK(p->sel.buf && count(RED) == 100 * 100 - count(BLACK) && count_in(RED, 0, 0, 99, 0) == 100);
    paint_key(p, PAINT_KEY_DELETE, 0);
    CHECK(count(RED) == 10000 && !p->sel.on && !strcmp(paint_undo_what(p), "Delete"));
    CHECK(paint_undo(p) && count(RED) == 200 && count(BLACK) == 10);
    /* a selection dragged out of the canvas is held to it; a click selects nothing */
    drag(PAINT_LEFT, 90, 90, 500, 500);
    CHECK(p->sel.on && p->sel.w == 10 && p->sel.h == 10);
    click(50, 50);
    CHECK(!p->sel.on);
    /* the outline in the view, and no paste without a copy */
    paint_free(p);
    paint_init(p, 100, 100);
    paint_view_size(p, 400, 300);
    CHECK(paint_paste(p) && strstr(p->msg, "Nothing to paste") && !p->sel.on);
    paint_select_all(p);
    paint_render(p, view, 400, 0, 0, 400, 300);
    paint_to_view(p, 0, 0, &vx, &vy);
    CHECK(view[(vy - 1) * 400 + vx + 10] == BLACK || view[(vy - 1) * 400 + vx + 10] == WHITE);
    CHECK(view[(vy - 1) * 400 + vx + 10] != p->backdrop && view[vy * 400 + vx] == WHITE);
    y = 0;
    for(x = vx - 1; x <= vx + 100; x++) y += view[(vy - 1) * 400 + x] == BLACK;
    CHECK(y > 30 && y < 72);                            /* dashes */
    unsnap(&s0);
    CHECK(counted());
}

/**** undo and redo ****/

/* noise over the canvas: as a step of the history (0), or behind its back (1) */
static void noise(int quietly) {
    int x, y;
    if(!quietly) step_begin(p, "Noise");
    for(y = 0; y < p->h; y++)
        for(x = 0; x < p->w; x++)
            if(quietly) setpx(x, y, rnd(0xFFFFFF));
            else fill_span(p, y, x, x, rnd(0xFFFFFF));
    if(!quietly) step_end(p);
}

static void history(void) {
    struct snap s[12];
    int i, ok;
    fresh(130, 70);                                     /* tiles at the edges: 2 pixels and 6 */
    tool(PAINT_BRUSH);
    paint_set_size(p, 4);
    for(i = 0; i < 12; i++) {
        s[i] = snap();
        paint_set_colour(p, 0, 0x010101u * (unsigned)(i * 20) + 5);
        drag(PAINT_LEFT, (i * 37) % 130, (i * 23) % 70, 129 - (i * 11) % 130, 69 - (i * 7) % 70);
        if(i % 3 == 0) {                                /* noise: tiles kept as they are */
            int x, y;
            step_begin(p, "Noise");
            for(y = 60; y < 70; y++)
                for(x = 120; x < 130; x++) fill_span(p, y, x, x, rnd(0xFFFFFF));
            step_end(p);
        }
    }
    for(ok = 1, i = 11; i >= 0; i--) {
        paint_undo(p);
        if(i % 3 == 0) paint_undo(p);
        if(!same(s[i])) ok = 0;
    }
    CHECK(ok && !paint_can_undo(p) && !paint_modified(p) && counted());
    CHECK(paint_undo(p) && strstr(p->msg, "Nothing to undo"));
    for(i = 0; i < 12; i++) paint_redo(p);
    CHECK(paint_modified(p) && paint_can_undo(p) && counted());
    for(i = 0; i < 12; i++) unsnap(&s[i]);
    /* what was saved: undoing back to it is not modified */
    fresh(100, 100);
    tool(PAINT_PENCIL);
    click(1, 1);
    paint_saved(p);
    CHECK(!paint_modified(p));
    click(2, 2);
    CHECK(paint_modified(p) && paint_undo(p) && !paint_modified(p) && paint_undo(p) && paint_modified(p));
    CHECK(paint_redo(p) && !paint_modified(p));
    /* a new change drops what could be redone; an empty one (a fill of the same colour) does not */
    click(3, 3);
    CHECK(!paint_can_redo(p) && paint_undo(p) && paint_can_redo(p));
    tool(PAINT_FILL);
    paint_set_colour(p, 0, WHITE);
    click(50, 50);
    CHECK(paint_can_redo(p) && !strcmp(paint_redo_what(p), "Pencil"));
    CHECK(paint_redo(p) && px(3, 3) == BLACK);
    /* at most PAINT_UNDO_STEPS steps: the oldest go */
    fresh(100, 100);
    tool(PAINT_PENCIL);
    for(i = 0; i < PAINT_UNDO_STEPS + 20; i++) click(i % 100, i / 100);
    CHECK(p->nsteps == PAINT_UNDO_STEPS && counted());
    for(i = 0; i < PAINT_UNDO_STEPS; i++) paint_undo(p);
    CHECK(!paint_can_undo(p) && count(BLACK) == 20 && px(19, 0) == BLACK && px(20, 0) == WHITE);
    /* the bytes: noise everywhere, so that each tile costs its 16 KB; then strokes of a tile each, the oldest going
       to keep the history within its bytes */
    fresh(640, 400);
    noise(1);
    noise(0);
    CHECK(paint_can_undo(p) && p->undo_bytes > 1000000);
    p->undo_max = 200000;
    paint_set_size(p, 2);
    tool(PAINT_BRUSH);
    for(i = 0; i < 40; i++)                             /* each in a tile of its own */
        drag(PAINT_LEFT, 32 + i % 9 * 64, 10 + i / 9 * 64, 32 + i % 9 * 64, 50 + i / 9 * 64);
    CHECK(p->undo_bytes <= 200000 && p->nsteps >= 5 && p->nsteps <= 13 && counted());
    for(i = 0; i < 39; i++) paint_undo(p);
    CHECK(!paint_can_undo(p) && px(32 + 39 % 9 * 64, 30 + 39 / 9 * 64) != BLACK && px(32, 30) == BLACK);
    CHECK(px(32 + (39 - p->nsteps) % 9 * 64, 30 + (39 - p->nsteps) / 9 * 64) == BLACK);
    /* a change too large for the history: made, the history gone with it, the status line says so */
    fresh(400, 400);
    p->undo_max = 64 * 1024;
    tool(PAINT_PENCIL);
    click(1, 1);
    CHECK(paint_can_undo(p));
    noise(1);
    noise(0);
    CHECK(!paint_can_undo(p) && strstr(p->msg, "cannot be undone") && paint_modified(p) && counted());
    /* no memory for a tile: the change is made, and cannot be undone */
    fresh(300, 300);
    noise(1);
    tool(PAINT_PENCIL);
    click(1, 1);
    p->mem_max = p->mem + sizeof(struct pstep) + 2000;
    tool(PAINT_BRUSH);
    paint_set_size(p, 7);
    paint_set_colour(p, 0, 0x777777);
    drag(PAINT_LEFT, 0, 0, 299, 299);
    CHECK(px(150, 150) == 0x777777 && !paint_can_undo(p) && strstr(p->msg, "Not enough memory") && counted());
    p->mem_max = PAINT_MEM_MAX;
}

static void resize(void) {
    struct snap a, b;
    fresh(100, 80);
    tool(PAINT_PENCIL);
    drag(PAINT_LEFT, 0, 0, 99, 79);
    a = snap();
    paint_set_colour(p, 1, RED);
    CHECK(paint_resize(p, 150, 60) == 0 && p->w == 150 && p->h == 60 && px(149, 59) == RED && px(0, 0) == BLACK);
    CHECK(px(99, 59) == WHITE && px(100, 0) == RED && !strcmp(paint_undo_what(p), "Size") && counted());
    b = snap();
    CHECK(paint_undo(p) && same(a) && p->tiles_x == 2 && counted());
    CHECK(paint_redo(p) && same(b) && counted());
    CHECK(paint_undo(p) && paint_undo(p) && p->w == 100 && count(BLACK) == 0 && !paint_modified(p));
    CHECK(paint_redo(p) && paint_redo(p) && same(b));
    CHECK(paint_resize(p, 150, 60) == 0 && same(b) && paint_resize(p, 0, 5) < 0 && paint_resize(p, 4097, 5) < 0);
    /* what floats is put down first; the selection held within the new size */
    tool(PAINT_SELECT);
    drag(PAINT_LEFT, 10, 10, 140, 50);
    paint_key(p, PAINT_KEY_RIGHT, 0);
    CHECK(paint_resize(p, 50, 30) == 0 && !p->sel.buf && p->sel.on && p->sel.w == 39 && p->sel.h == 20 && counted());
    /* no memory for the new size: refused, nothing changed */
    unsnap(&b);
    b = snap();
    p->mem_max = p->mem + 100;
    CHECK(paint_resize(p, 2000, 2000) < 0 && strstr(p->msg, "Not enough memory") && same(b) && counted());
    p->mem_max = PAINT_MEM_MAX;
    unsnap(&a);
    unsnap(&b);
    /* a new size too large for the history is made, and cannot be undone */
    fresh(500, 500);
    p->undo_max = 4096;
    {
        int y;
        unsigned row[500];
        step_begin(p, "Noise");
        for(y = 0; y < 500; y++) {
            int x;
            for(x = 0; x < 500; x++) row[x] = rnd(0xFFFFFF);
            copy_span(p, y, 0, row, 500);
        }
        step_end(p);
    }
    CHECK(paint_resize(p, 600, 600) == 0 && p->w == 600 && !paint_can_undo(p) && counted());
}

/**** the view ****/

static void view(void) {
    unsigned buf[400 * 300], *v = buf;
    int z, vx, vy, cx, cy, ok, num, den, x, y, n, dx, dy, dw, dh;
    fresh(640, 400);
    CHECK(p->vw == 400 && p->vh == 300 && p->zoom == PAINT_ZOOM_1 && paint_zoom_percent(p->zoom) == 100);
    /* every zoom: a view's pixel goes to the canvas's pixel it shows, and that pixel back to a view's pixel at or
       before it */
    for(ok = 1, z = 0; z < PAINT_ZOOMS; z++) {
        paint_zoom_to(p, z, 0, 0);
        paint_zoom_ratio(z, &num, &den);
        for(vx = -50; vx < 450; vx += 7) {
            paint_to_canvas(p, vx, vx / 2, &cx, &cy);
            paint_to_view(p, cx, cy, &x, &y);
            if(x > vx || (den == 1 && vx - x >= num) || (den > 1 && x != vx)) ok = 0;
        }
    }
    CHECK(ok);
    CHECK(paint_zoom_percent(0) == 12 && paint_zoom_percent(PAINT_ZOOMS - 1) == 3200);
    /* zooming keeps the point under the pointer */
    paint_zoom_to(p, PAINT_ZOOM_1, 0, 0);
    paint_scroll_to(p, 100, 50);
    paint_to_canvas(p, 0, 0, &cx, &cy);
    CHECK(cx == 100 && cy == 50 && p->sx == 100 && p->sy == 50);
    paint_to_canvas(p, 200, 150, &cx, &cy);
    CHECK(paint_zoom_by(p, 2, 200, 150) && paint_zoom_percent(p->zoom) == 300);
    paint_to_canvas(p, 200, 150, &x, &y);
    CHECK(x == cx && y == cy);
    CHECK(paint_zoom_by(p, -1, 200, 150) && paint_zoom_percent(p->zoom) == 200);
    paint_to_canvas(p, 200, 150, &x, &y);
    CHECK(x == cx && y == cy);
    CHECK(paint_zoom_by(p, -2, 200, 150) && paint_zoom_percent(p->zoom) == 50);
    /* smaller than the view: in its middle; scrolling held to the canvas */
    paint_origin(p, &x, &y);
    CHECK(x == (400 - 320) / 2 && y == (300 - 200) / 2 && p->sx == 0 && p->sy == 0);
    CHECK(!paint_scroll_by(p, 50, 50));
    paint_zoom_to(p, PAINT_ZOOM_1, 0, 0);
    CHECK(paint_scroll_to(p, 100000, -5) && p->sx == 640 - 400 && p->sy == 0);
    /* Fit: the largest that shows it all, at most 100% */
    CHECK(paint_zoom_fit(p) && paint_zoom_percent(p->zoom) == 50);
    paint_new(p, 100, 50);
    paint_zoom_to(p, PAINT_ZOOMS - 1, 0, 0);
    CHECK(paint_zoom_fit(p) && p->zoom == PAINT_ZOOM_1);
    paint_new(p, 4000, 4000);
    CHECK(paint_zoom_fit(p) && p->zoom == 0);           /* 1/8 is the least */
    /* what the view shows: the canvas at 100%, doubled at 200%, every other pixel at 50%; the backdrop around */
    fresh(64, 48);
    for(y = 0; y < 48; y++)
        for(x = 0; x < 64; x++) setpx(x, y, (unsigned)(x << 8 | y));
    paint_render(p, v, 400, 0, 0, 400, 300);
    paint_origin(p, &x, &y);
    CHECK(x == 168 && y == 126 && v[126 * 400 + 168] == 0 && v[(126 + 47) * 400 + 168 + 63] == (63u << 8 | 47));
    CHECK(v[0] == p->backdrop && v[125 * 400 + 200] == p->backdrop && v[126 * 400 + 167] == p->backdrop);
    paint_zoom_to(p, PAINT_ZOOM_1 + 1, 0, 0);
    paint_render(p, v, 400, 0, 0, 400, 300);
    paint_origin(p, &x, &y);
    for(ok = 1, n = 0; n < 128; n++)
        if(v[(y + 5) * 400 + x + n] != (unsigned)((n / 2) << 8 | 2)) ok = 0;
    CHECK(ok);
    paint_zoom_to(p, PAINT_ZOOM_1 - 1, 0, 0);
    paint_render(p, v, 400, 0, 0, 400, 300);
    paint_origin(p, &x, &y);
    CHECK(v[(y + 3) * 400 + x + 5] == (11u << 8 | 7));  /* the middle of each 2 x 2 */
    /* only a part: what is outside it is left alone */
    for(n = 0; n < 400 * 300; n++) v[n] = 0xABCDEF;
    paint_render(p, v + 20 * 400 + 10, 400, 10, 20, 30, 40);  /* at its rectangle's own top left */
    CHECK(v[20 * 400 + 10] == p->backdrop && v[19 * 400 + 10] == 0xABCDEF && v[20 * 400 + 40] == 0xABCDEF);
    CHECK(v[59 * 400 + 39] == p->backdrop && v[60 * 400 + 39] == 0xABCDEF);
    paint_render(p, v, 400, -100, -100, 50, 50);        /* clipped to the view: nothing */
    paint_render(p, v + 290 * 400 + 390, 400, 390, 290, 100, 100);
    CHECK(v[299 * 400 + 399] == p->backdrop);
    paint_zoom_to(p, PAINT_ZOOM_1, 0, 0);
    for(n = 0; n < 400 * 300; n++) v[n] = 0xABCDEF;
    paint_render(p, v + 116 * 400 + 158, 400, 158, 116, 20, 20);   /* across the canvas's corner: each pixel in place */
    CHECK(v[126 * 400 + 168] == 0 && v[125 * 400 + 167] == p->backdrop && v[135 * 400 + 177] == (9u << 8 | 9));
    CHECK(v[136 * 400 + 177] == 0xABCDEF && v[135 * 400 + 178] == 0xABCDEF);
    paint_render(p, v + 2 * 400 + 3, 400, -3, -2, 10, 10);   /* from (-3, -2): clipped, dst moving with it to (0, 0) */
    CHECK(v[4 * 400 + 6] == p->backdrop && v[11 * 400 + 12] == p->backdrop && v[2 * 400 + 3] == 0xABCDEF);
    CHECK(v[4 * 400 + 5] == 0xABCDEF && v[3 * 400 + 6] == 0xABCDEF && v[12 * 400 + 12] == 0xABCDEF);
    /* what changed: all of it after a zoom; a stroke's pixels after that; nothing once drawn */
    fresh(640, 400);
    CHECK(paint_damage(p, &dx, &dy, &dw, &dh) && dw == 400 && dh == 300);
    paint_drawn(p);
    CHECK(!paint_damage(p, &dx, &dy, &dw, &dh));
    paint_scroll_to(p, 100, 100);
    paint_drawn(p);
    tool(PAINT_PENCIL);
    click(150, 120);
    CHECK(paint_damage(p, &dx, &dy, &dw, &dh) && dx <= 50 && dx + dw > 50 && dy <= 20 && dy + dh > 20 && dw < 10);
    paint_drawn(p);
    paint_set_size(p, 0);
    tool(PAINT_LINE);
    paint_press(p, 110, 110, PAINT_LEFT, 0);
    paint_move(p, 140, 115, 0);
    paint_drawn(p);
    paint_move(p, 200, 130, 0);                         /* the line's old place and its new one */
    CHECK(paint_damage(p, &dx, &dy, &dw, &dh) && dx <= 10 && dx + dw > 100 && counted());
    paint_release(p, 200, 130, 0);
    paint_drawn(p);
    paint_zoom_by(p, 1, 0, 0);
    CHECK(paint_damage(p, &dx, &dy, &dw, &dh) && dw == 400);
    /* text being typed and its cursor show in the view */
    fresh(100, 100);
    tool(PAINT_TEXT);
    paint_set_text_scale(p, 1);
    click(10, 10);
    type("I");
    paint_render(p, v, 400, 0, 0, 400, 300);
    paint_to_view(p, 10, 10, &vx, &vy);
    for(n = 0, y = 0; y < 16; y++)
        for(x = 0; x < 8; x++) n += v[(vy + y) * 400 + vx + x] == BLACK;
    for(ok = 0, y = 0; y < 16; y++)
        for(x = 0; x < 8; x++) ok += !!(font8x16['I'][y] & (0x80 >> x));
    CHECK(n == ok && v[(vy + 2) * 400 + vx + 8] == (WHITE ^ 0xFFFFFF));
    paint_free(p);
    CHECK(p->mem == 0);
}

/**** the colours and the status line ****/

static void colours(void) {
    unsigned c = 0;
    char t[16];
    int i;
    CHECK(!paint_parse_colour("#1A2B3C", &c) && c == 0x1A2B3C);
    CHECK(!paint_parse_colour("1a2b3c", &c) && c == 0x1A2B3C);
    CHECK(!paint_parse_colour(" #abc ", &c) && c == 0xAABBCC);
    CHECK(!paint_parse_colour("255, 0, 128", &c) && c == 0xFF0080);
    CHECK(!paint_parse_colour("12 34 56", &c) && c == 0x0C2238);
    CHECK(!paint_parse_colour("0,0,0", &c) && c == 0);
    CHECK(!paint_parse_colour("123", &c) && c == 0x112233);
    c = 7;
    CHECK(paint_parse_colour("#12345", &c) && paint_parse_colour("#1234567", &c) && paint_parse_colour("", &c));
    CHECK(paint_parse_colour("256,0,0", &c) && paint_parse_colour("1,2", &c) && paint_parse_colour("xyz", &c));
    CHECK(paint_parse_colour("1,2,3,4", &c) && paint_parse_colour("0001,2,3", &c) && paint_parse_colour("#12 34", &c));
    CHECK(paint_parse_colour("-1,2,3", &c) && paint_parse_colour("#ggg", &c) && c == 7);
    paint_colour_text(0x0A0B0C, t, sizeof(t));
    CHECK(!strcmp(t, "#0A0B0C"));
    fresh(10, 10);
    for(i = 0; i < PAINT_CUSTOM + 2; i++) paint_add_custom(p, 0x100000u * (unsigned)(i + 1));
    CHECK(p->custom[0] == 0x900000 && p->custom[1] == 0xA00000 && p->custom[2] == 0x300000 && p->fg == 0xA00000);
    paint_add_custom(p, 0x300000);                      /* there already: not again */
    CHECK(p->custom[2] == 0x300000 && p->custom[3] == 0x400000 && p->fg == 0x300000);
    paint_swap_colours(p);
    CHECK(p->fg == WHITE && p->bg == 0x300000);
    for(i = 0; i < PAINT_COLOURS; i++) CHECK(paint_palette[i] <= 0xFFFFFF);
}

static void status(void) {
    char s[160];
    fresh(640, 400);
    paint_hint(p, s, sizeof(s));
    CHECK(strstr(s, "Brush, 3 px"));
    tool(PAINT_RECT);
    paint_set_filled(p, 1);
    paint_hint(p, s, sizeof(s));
    CHECK(strstr(s, "Rectangle, filled"));
    paint_move(p, 12, 34, 0);
    paint_where(p, s, sizeof(s));
    CHECK(!strcmp(s, "12, 34   640 x 400   100%"));
    paint_leave(p);
    tool(PAINT_SELECT);
    drag(PAINT_LEFT, 0, 0, 9, 19);
    paint_leave(p);
    paint_where(p, s, sizeof(s));
    CHECK(!strcmp(s, "10 x 20 selected   640 x 400   100%"));
    paint_undo(p);
    paint_hint(p, s, sizeof(s));
    CHECK(!strcmp(s, "Nothing to undo"));
    paint_hint(p, s, 8);
    CHECK(strlen(s) == 7);
}

/**** what is under way when something else comes ****/

static void interrupted(void) {
    struct snap a;
    unsigned view[400 * 300];
    char hint[160];
    int t, vx, vy, x, n;
    /* a stroke under way when the canvas changes size: it ends first, a step of its own */
    fresh(100, 100);
    tool(PAINT_PENCIL);
    paint_press(p, 10, 10, PAINT_LEFT, 0);
    paint_move(p, 20, 10, 0);
    CHECK(paint_resize(p, 120, 90) == 0 && !p->drag && !p->open && !strcmp(paint_undo_what(p), "Size"));
    CHECK(paint_undo(p) && px(15, 10) == BLACK && !strcmp(paint_undo_what(p), "Pencil") && paint_undo(p));
    CHECK(count(BLACK) == 0 && counted());
    /* a selection being dragged when Delete, Clear, Select all or Paste come */
    paint_set_colour(p, 1, RED);
    tool(PAINT_SELECT);
    paint_press(p, 10, 10, PAINT_LEFT, 0);
    paint_move(p, 30, 30, 0);
    CHECK(paint_delete(p) && !p->drag && !p->sel.on && count(RED) == 21 * 21);
    paint_press(p, 50, 50, PAINT_LEFT, 0);
    paint_move(p, 60, 60, 0);
    CHECK(paint_select_all(p) && !p->drag && p->sel.w == 100 && paint_copy(p));
    paint_press(p, 5, 5, PAINT_LEFT, 0);                /* inside: lifted and moving */
    paint_move(p, 6, 6, 0);
    CHECK(p->drag && p->sel.buf && paint_clear(p) && !p->drag && !p->sel.buf && count(RED) == 10000 && counted());
    paint_press(p, 50, 50, PAINT_LEFT, 0);
    CHECK(paint_paste(p) && !p->drag && p->sel.buf == p->clip && counted());
    paint_put_down(p);
    /* Esc while a shape is dragged: not drawn; another tool while a stroke is under way: it ends */
    fresh(100, 100);
    tool(PAINT_RECT);
    paint_press(p, 10, 10, PAINT_LEFT, 0);
    paint_move(p, 50, 50, 0);
    CHECK(paint_key(p, 27, 0) && !p->drag && !p->shape.on && count(BLACK) == 0 && !paint_can_undo(p));
    paint_release(p, 50, 50, 0);                        /* the button let go after Esc: nothing */
    CHECK(count(BLACK) == 0 && !paint_can_undo(p));
    tool(PAINT_BRUSH);
    paint_press(p, 10, 10, PAINT_LEFT, 0);
    paint_move(p, 40, 10, 0);
    tool(PAINT_LINE);
    CHECK(!p->drag && !p->open && !strcmp(paint_undo_what(p), "Brush") && px(25, 10) == BLACK);
    CHECK(paint_undo(p) && count(BLACK) == 0);
    /* Undo and Redo do nothing while a button is held */
    paint_press(p, 10, 10, PAINT_LEFT, 0);
    CHECK(!paint_undo(p) && !paint_redo(p) && p->drag);
    paint_release(p, 30, 30, 0);
    /* a lift whose tiles cannot be kept, undone while it floats: the pixels go back by hand */
    fresh(200, 200);
    noise(1);
    a = snap();
    tool(PAINT_SELECT);
    drag(PAINT_LEFT, 0, 0, 199, 149);
    p->mem_max = p->mem + pbuf_bytes(&(struct pbuf){ 1, 200, 150, NULL }) + 3000;
    paint_key(p, PAINT_KEY_RIGHT, 0);
    CHECK(p->sel.buf && p->sel.lifted && strstr(p->msg, "Not enough memory") && counted());
    CHECK(paint_undo(p) && same(a) && !p->sel.buf && p->sel.on && p->sel.x == 0 && counted());
    p->mem_max = PAINT_MEM_MAX;
    unsnap(&a);
    /* no memory to lift a selection: refused, nothing changed */
    a = snap();
    p->mem_max = p->mem + 100;
    CHECK(paint_key(p, PAINT_KEY_RIGHT, 0) && !p->sel.buf && strstr(p->msg, "move the selection") && same(a));
    paint_press(p, 50, 50, PAINT_LEFT, 0);
    CHECK(!p->drag && !p->sel.buf && same(a));
    p->mem_max = PAINT_MEM_MAX;
    unsnap(&a);
    /* every tool's hint */
    fresh(100, 100);
    for(t = 0; t < PAINT_TOOLS; t++) {
        tool(t);
        paint_hint(p, hint, sizeof(hint));
        CHECK(strlen(hint) > 10 && (t == PAINT_SELECT || strstr(hint, t == PAINT_PICKER ? "Picker" : paint_tool_names[t])));
    }
    tool(PAINT_TEXT);
    click(5, 5);
    paint_hint(p, hint, sizeof(hint));
    CHECK(strstr(hint, "Enter: a new line"));
    /* zoomed out, what floats past the edge shows on the canvas only, the outline around all of it */
    fresh(300, 200);
    tool(PAINT_RECT);
    paint_set_filled(p, 1);
    drag(PAINT_LEFT, 0, 0, 99, 99);
    tool(PAINT_SELECT);
    drag(PAINT_LEFT, 0, 0, 99, 99);
    for(x = 0; x < 30; x++) paint_key(p, PAINT_KEY_LEFT, PAINT_SHIFT);
    paint_zoom_to(p, PAINT_ZOOM_1 - 1, 0, 0);
    paint_render(p, view, 400, 0, 0, 400, 300);
    paint_to_view(p, 0, 0, &vx, &vy);
    CHECK(view[(vy + 10) * 400 + vx] == BLACK && view[(vy + 10) * 400 + vx - 2] == p->backdrop);
    for(n = 0, x = 0; x < 400; x++) n += view[(vy - 1) * 400 + x] == BLACK || view[(vy - 1) * 400 + x] == WHITE;
    CHECK(n > 0 && p->sel.x == 1 - 100);
    paint_cancel(p);
    CHECK(px(0, 0) == BLACK && px(1, 0) == WHITE && counted());
}

/**** at random ****/

static void random_actions(void) {
    struct snap start, end;
    int round, i, a, x, y, ok = 1, steps;
    for(round = 0; round < 12; round++) {
        fresh(90 + (int)rnd(90), 60 + (int)rnd(90));
        p->fill_stack = 1 + (int)rnd(20);
        start = snap();
        steps = 0;
        for(i = 0; i < 70; i++) {
            a = (int)rnd(16);
            x = (int)rnd((unsigned)p->w + 40) - 20;
            y = (int)rnd((unsigned)p->h + 40) - 20;
            paint_set_colour(p, 0, rnd(0xFFFFFF));
            paint_set_colour(p, 1, rnd(4) ? WHITE : rnd(0xFFFFFF));
            paint_set_size(p, (int)rnd(PAINT_SIZES));
            paint_set_filled(p, (int)rnd(2));
            if(a < 10) {
                tool(a);
                paint_press(p, x, y, rnd(3) ? PAINT_LEFT : PAINT_RIGHT, 0);
                paint_move(p, x + (int)rnd(60) - 30, y + (int)rnd(60) - 30, (int)rnd(2));
                if(a == PAINT_TEXT) type("Ab\r\xC3\xA9");
                paint_release(p, x + (int)rnd(60) - 30, y + (int)rnd(60) - 30, (int)rnd(2));
            } else if(a == 10) paint_key(p, PAINT_KEY_LEFT + (int)rnd(4) - 2, (int)rnd(2));
            else if(a == 11) paint_copy(p);
            else if(a == 12) paint_paste(p);
            else if(a == 13) paint_cut(p);
            else if(a == 14) paint_put_down(p);
            else paint_resize(p, p->w + (int)rnd(21) - 10, p->h + (int)rnd(21) - 10);
            if(!counted()) ok = 0;
        }
        paint_put_down(p);
        end = snap();
        while(paint_can_undo(p)) {
            paint_undo(p);
            steps++;
            if(!counted()) ok = 0;
        }
        if(!same(start) || paint_modified(p)) ok = 0;
        while(paint_can_redo(p)) paint_redo(p);
        if(!same(end)) ok = 0;
        if(!ok) {
            printf("  random round %d differs after %d steps\n", round, steps);
            break;
        }
        unsnap(&start);
        unsnap(&end);
    }
    CHECK(ok);
    paint_free(p);
    CHECK(p->mem == 0);
}

/**** what the review of 11/05 found ****/

/* a canvas of noise set behind the history's back: every tile is kept raw */
static void noise_canvas(int w, int h) {
    unsigned s = 7;
    int x, y;
    fresh(w, h);
    for(y = 0; y < h; y++)
        for(x = 0; x < w; x++) {
            s = s * 1103515245u + 12345u;
            setpx(x, y, (s >> 8) & 0xFFFFFF);
        }
}

/* Critical: Cut when the copy cannot be had deletes nothing and keeps the clipboard (it once deleted the selection
   and read through a clipboard of NULL); a stroke under way ends first */
static void cut_without_memory(void) {
    struct snap s;
    fresh(1, 1);
    noise_canvas(200, 200);
    tool(PAINT_SELECT);
    drag(PAINT_LEFT, 0, 0, 9, 9);
    CHECK(paint_copy(p) && p->clip && p->clip->w == 10);
    paint_select_all(p);
    tool(PAINT_BRUSH);
    paint_press(p, 5, 5, PAINT_LEFT, 0);
    paint_move(p, 20, 5, 0);                            /* a stroke under way, in one tile */
    s = snap();
    p->mem_max = p->mem + 1000;                         /* no room for 200 x 200 more, even were the history gone */
    CHECK(paint_cut(p) && !p->drag && p->clip && p->clip->w == 10 && strstr(p->msg, "keeps what it had"));
    CHECK(same(s) && p->sel.on && !strcmp(paint_undo_what(p), "Brush") && counted());
    CHECK(paint_copy(p) && p->clip->w == 10 && strstr(p->msg, "Not enough memory to copy 200 x 200") && same(s));
    p->mem_max = PAINT_MEM_MAX;
    CHECK(paint_cut(p) && p->clip->w == 200 && strstr(p->msg, "Cut 200 x 200") && count(WHITE) == 200 * 200);
    CHECK(!memcmp(p->clip->px, s.px, canvas_bytes(200, 200)) && paint_undo(p) && same(s) && counted());
    unsnap(&s);
}

/* Low: Cut of what floats cuts it: a paste goes and the canvas is as it was; a lifted part leaves the second colour,
   one step, which Undo takes back */
static void cut_floating(void) {
    struct snap s;
    fresh(100, 80);
    rect(PAINT_RECT, 1, 0, RED, 10, 10, 29, 29);
    tool(PAINT_SELECT);
    drag(PAINT_LEFT, 10, 10, 29, 29);
    paint_copy(p);
    paint_paste(p);
    s = snap();
    CHECK(p->sel.buf == p->clip);
    CHECK(paint_cut(p) && !p->sel.on && !p->sel.buf && p->clip && p->clip->w == 20 && same(s) &&
          strstr(p->msg, "Cut 20 x 20") && counted());
    unsnap(&s);
    paint_set_colour(p, 1, BLUE);
    drag(PAINT_LEFT, 10, 10, 29, 29);
    paint_key(p, PAINT_KEY_RIGHT, 0);                   /* lifted, a pixel to the right */
    CHECK(p->sel.buf && p->sel.lifted);
    CHECK(paint_cut(p) && !p->sel.on && count_in(BLUE, 10, 10, 29, 29) == 400 && count(RED) == 0 &&
          !strcmp(paint_undo_what(p), "Cut") && p->clip->w == 20);
    CHECK(paint_undo(p) && count_in(RED, 10, 10, 29, 29) == 400 && count(BLUE) == 0 && counted());
}

/* Low: what is refused for want of memory costs no step of the history (and a failed copy no clipboard) */
static void refused_keeps_history(void) {
    int steps;
    fresh(100, 100);
    tool(PAINT_PENCIL);
    click(1, 1);
    click(2, 2);
    click(3, 3);
    tool(PAINT_SELECT);
    drag(PAINT_LEFT, 0, 0, 9, 9);
    paint_copy(p);
    steps = p->nsteps;
    paint_select_all(p);
    p->mem_max = p->mem + 1000;
    CHECK(paint_copy(p) && p->nsteps == steps && p->clip->w == 10 && strstr(p->msg, "Not enough memory to copy"));
    tool(PAINT_FILL);
    click(50, 50);
    CHECK(p->nsteps == steps && strstr(p->msg, "Not enough memory to fill") && count(WHITE) == 100 * 100 - 3);
    CHECK(paint_new(p, 1000, 1000) < 0 && p->nsteps == steps && p->w == 100);
    CHECK(paint_resize(p, 1000, 1000) < 0 && p->nsteps == steps && p->w == 100);
    p->mem_max = PAINT_MEM_MAX;
    CHECK(counted() && paint_undo(p) && px(3, 3) == WHITE);
}

static void review(void) {
    cut_without_memory();
    cut_floating();
    refused_keeps_history();
}

int main(void) {
    canvas();
    pencil_brush_eraser();
    shapes();
    fill();
    text();
    picker();
    selection();
    history();
    resize();
    view();
    colours();
    status();
    interrupted();
    random_actions();
    review();
    paint_free(p);
    CHECK(p->mem == 0);
    printf("test_wpaint: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
