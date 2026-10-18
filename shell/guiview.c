/*
SurfOS Desktop: Image Viewer
--------------------
File: guiview.c Date: 10/17/26
--------------------
BMP pictures (guibmp.c reads them), fitted to the window or zoomed. A picture is opened with the
chooser (the toolbar's Open, or O) or by another file (viewer_show, shell/gui_media.h: Files). It
is read a piece at a time in the tick (Esc stops it), so a big file never freezes the desktop: a
frame reads at most VIEW_TICK bytes, each read counting as VIEW_READ_MIN at least (a read is an
open and a walk of the file's clusters), and one read takes as many rows next to each other in the
file as a chunk holds, those a reduced picture skips too. The tick never opens a dialog: what goes
wrong there is said in the window and its status bar, as a dialog would replace whatever question
is in front. The picture is kept decoded in gui_state: at most GUI_VIEW_PIXELS pixels, a
bigger picture being reduced as it is read (nearest neighbour; the status bar says so). Drawing
scales the decoded pixels into the window, nearest neighbour again, clipped, without allocating:
each row is stepped through exactly, with a quotient and a remainder, and written straight into
the surface when it has the desktop's 32-bit pixels. The views: Fit, the default (a picture larger
than the window is shrunk to fit; a smaller one is enlarged by a whole number, as far as it fits,
so its pixels stay square), the actual size, and zoom steps from 1/64 to 16 times, about the middle
of the view. A zoomed picture moves with the mouse (a drag), the arrows, Page Up and Down, Home,
End and the wheel; a double-click switches between Fit and the actual size.
Track MEDIA of docs/superpowers/plans/2026-10-17-gui-suite.md; the window manager reaches it
through gui_app_viewer (struct gui_app, shell/guipriv.h). Its answer hook also answers Sketch's
dialogs (Save as BMP and Open BMP, guibmp.c).
*/
#include "gui_media.h"
#include <surfos/math64.h>
#include <string.h>
#include <stdio.h>

#define ONE 65536                       /* the actual size, as a 16.16 zoom */
#define ZOOM_MIN (ONE / 64)
#define ZOOM_MAX (ONE * 16)
#define VIEW_TICK (256 * 1024)          /* the most a frame reads of a picture (and one chunk past it) */
#define VIEW_READ_MIN (16 * 1024)       /* what a read counts for at least: 16 reads a frame at most */
#define PAN_STEP 32                     /* the pixels an arrow moves a zoomed picture */

/* the toolbar's buttons and the viewer's own dialogs (an Open chooser); Sketch's are gui_media.h's */
enum { VA_OPEN = 1, VA_FIT, VA_ACTUAL, VA_IN, VA_OUT };
static const struct gui_tool tools[] = {
    { "Open", VA_OPEN }, { "Fit", VA_FIT }, { "Actual size", VA_ACTUAL }, { "Zoom in", VA_IN }, { "Zoom out", VA_OUT },
};
#define TOOLS (int)(sizeof(tools) / sizeof(tools[0]))
static const u32 steps[] = { ONE / 64, ONE / 32, ONE / 16, ONE / 8, ONE / 4, ONE / 3, ONE / 2, ONE * 2 / 3, ONE,
                             ONE * 3 / 2, ONE * 2, ONE * 3, ONE * 4, ONE * 6, ONE * 8, ONE * 12, ONE * 16 };
#define STEPS (int)(sizeof(steps) / sizeof(steps[0]))

static const char *const help[] = {
    "#Image Viewer",
    "BMP pictures of 1, 4, 8, 24 or 32 bits, uncompressed.",
    "Open (or O) chooses one. F (or 0) fits it to the window",
    "(the default), 1 shows its actual size, + and - zoom in",
    "and out (1/64 to 16 times); a double-click switches",
    "between Fit and the actual size. Drag a zoomed picture,",
    "or move it with the arrows, Page Up and Down, Home, End",
    "and the wheel. Up to 16384 x 16384 pixels; a picture of",
    "more than 1,048,576 is reduced as it is read (the status",
    "bar says so). Esc stops a reading.",
    "Sketch: Ctrl+S, or File > Save as BMP\xF9, saves the",
    "drawing as a 24-bit BMP; File > Open BMP\xF9 reads one",
    "of up to 8 MB in, fitted, in Sketch's six colours.",
};

/**** where things are ****/

struct view_layout { int x, y, w, h, status_y; };   /* the picture's room, window-relative */

static void layout(const struct gui_state *g, struct view_layout *l) {
    const struct gui_window *w = &g->win[GUI_VIEWER];
    l->x = 1;
    l->y = TITLE_H + BAR_H + 3;
    l->status_y = w->h - STATUS_H - 1;
    l->w = w->w - 2 > 1 ? w->w - 2 : 1;
    l->h = l->status_y - l->y > 1 ? l->status_y - l->y : 1;
}

static int min_width(void) {
    return tool_span(tools, TOOLS);
}

/* Fit's zoom: shrunk to the room, or enlarged by the largest whole number that fits */
static u32 fit_zoom(const struct gui_viewer *v, const struct view_layout *l) {
    u32 k, zx, zy;
    if(!v->w || !v->h) return ONE;
    if((u32)l->w >= v->w && (u32)l->h >= v->h) {
        k = (u32)l->w / v->w < (u32)l->h / v->h ? (u32)l->w / v->w : (u32)l->h / v->h;
        return (k > 16 ? 16 : k) * ONE;
    }
    zx = ((u32)l->w << 16) / v->w;
    zy = ((u32)l->h << 16) / v->h;
    zx = zx < zy ? zx : zy;
    return zx < ZOOM_MIN ? ZOOM_MIN : zx;
}

static u32 zoom_of(const struct gui_viewer *v, const struct view_layout *l) {
    return v->fit ? fit_zoom(v, l) : v->zoom;
}

/* n pixels of the picture at zoom z: the screen's pixels (at least one) */
static int shown(u32 n, u32 z) {
    u64 s = (u64)n * z >> 16;           /* 16384 at 16 times: 262144 */
    return s ? (int)s : 1;
}

static int pan_max(int size, int room) {
    return size > room ? size - room : 0;
}

/* the picture on the screen: where its top left is (it may be off the room), and its size */
static void place(const struct gui_state *g, const struct view_layout *l, int *x, int *y, int *pw, int *ph) {
    const struct gui_viewer *v = &g->viewer;
    const struct gui_window *w = &g->win[GUI_VIEWER];
    u32 z = zoom_of(v, l);
    *pw = shown(v->w, z);
    *ph = shown(v->h, z);
    *x = w->x + l->x + (*pw < l->w ? (l->w - *pw) / 2 : -gui_clamp(v->pan_x, 0, pan_max(*pw, l->w)));
    *y = w->y + l->y + (*ph < l->h ? (l->h - *ph) / 2 : -gui_clamp(v->pan_y, 0, pan_max(*ph, l->h)));
}

void viewer_place(const struct gui_state *g, int *x, int *y, int *w, int *h) {
    struct view_layout l;
    *x = *y = *w = *h = 0;
    if(!g->viewer.ready) return;
    layout(g, &l);
    place(g, &l, x, y, w, h);
}

u32 viewer_zoom(const struct gui_state *g) {
    struct view_layout l;
    if(!g->viewer.ready) return 0;
    layout(g, &l);
    return zoom_of(&g->viewer, &l);
}

bool viewer_takes(const char *name) {
    u32 n = strlen(name);
    return n > 4 && name[n - 4] == '.' && (name[n - 3] | 0x20) == 'b' && (name[n - 2] | 0x20) == 'm' &&
           (name[n - 1] | 0x20) == 'p';
}

/**** moving and zooming ****/

static void clamp_pan(struct gui_state *g) {
    struct gui_viewer *v = &g->viewer;
    struct view_layout l;
    u32 z;
    layout(g, &l);
    z = zoom_of(v, &l);
    v->pan_x = gui_clamp(v->pan_x, 0, pan_max(shown(v->w, z), l.w));
    v->pan_y = gui_clamp(v->pan_y, 0, pan_max(shown(v->h, z), l.h));
    g->dirty = true;
}

static void pan(struct gui_state *g, int dx, int dy) {
    clamp_pan(g);                       /* from where it is shown: the window may have changed */
    g->viewer.pan_x += dx;
    g->viewer.pan_y += dy;
    clamp_pan(g);
}

/* the point of the picture in the middle of the room, as a fraction (16.16) of the picture shown */
static u32 middle(int pan, int size, int room) {
    if(size <= room) return ONE / 2;
    return (u32)div64_32((u64)(gui_clamp(pan, 0, size - room) + room / 2) << 16, (u32)size, NULL);
}

/* a new zoom (or Fit), with the picture's point in the middle of the room staying there */
static void zoom_to(struct gui_state *g, u32 z, bool fit) {
    struct gui_viewer *v = &g->viewer;
    struct view_layout l;
    u32 old, mx, my;
    layout(g, &l);
    old = zoom_of(v, &l);
    mx = middle(v->pan_x, shown(v->w, old), l.w);
    my = middle(v->pan_y, shown(v->h, old), l.h);
    v->fit = fit;
    if(!fit) v->zoom = z < ZOOM_MIN ? ZOOM_MIN : z > ZOOM_MAX ? ZOOM_MAX : z;
    z = zoom_of(v, &l);
    v->pan_x = (int)((u64)mx * (u32)shown(v->w, z) >> 16) - l.w / 2;
    v->pan_y = (int)((u64)my * (u32)shown(v->h, z) >> 16) - l.h / 2;
    clamp_pan(g);
}

/* the next step in or out from the zoom shown (Fit's may lie between two) */
static void zoom_step(struct gui_state *g, bool in) {
    struct view_layout l;
    u32 z;
    int i;
    layout(g, &l);
    z = zoom_of(&g->viewer, &l);
    if(in) {
        for(i = 0; i < STEPS && steps[i] <= z + (z >> 7); i++) ;
        if(i < STEPS) zoom_to(g, steps[i], false);
    } else {
        for(i = STEPS - 1; i >= 0 && steps[i] >= z - (z >> 7); i--) ;
        if(i >= 0) zoom_to(g, steps[i], false);
    }
}

/**** reading ****/

static void forget(struct gui_viewer *v) {
    v->ready = v->loading = v->dragging = false;
    v->path[0] = 0;
    v->w = v->h = v->dw = v->dh = 0;
}

/* the row of the file that the k-th row read is (they are read in the file's order) */
static u32 file_row(const struct gui_viewer *v, u32 k) {
    u32 dy = v->bmp.topdown ? k : v->dh - 1 - k;
    u32 sy = v->dh == v->h ? dy : (2 * dy + 1) * v->h / (2 * v->dh);    /* the middle of what it stands for */
    return v->bmp.topdown ? sy : v->h - 1 - sy;
}

static void decode_row(struct gui_viewer *v, const u8 *row, u32 dy) {
    u8 *out = v->pixels + dy * v->dw * 3;
    u32 dx, c;
    for(dx = 0; dx < v->dw; dx++) {
        c = bmp_pixel(&v->bmp, v->palette, row, v->dw == v->w ? dx : (2 * dx + 1) * v->w / (2 * v->dw));
        *out++ = (u8)(c >> 16);
        *out++ = (u8)(c >> 8);
        *out++ = (u8)c;
    }
}

/* the picture could not be read to its end: said in the window and its status bar, and in a dialog only when the
   user's own action asked for it (not from the tick) and no other dialog is in front, which it would replace (a
   question about unsaved changes, a chooser half filled in) */
static void read_failed(struct gui_state *g, int err, bool tick) {
    struct gui_viewer *v = &g->viewer;
    char name[GUI_NAME_MAX];
    strlcpy(name, gui_basename(v->path), sizeof(name));
    forget(v);
    snprintf(v->status, sizeof(v->status), "%s could not be read: %s", name,
             err == -EIO ? "it ended early, or the disk failed" : gui_error(g, err));
    if(!tick && !g->dialog.kind)
        dialog_message(g, "Cannot show the picture", name, err == -EIO ? "It could not be read to its end." : gui_error(g, err));
}

/* some more rows. A read takes the file's rows from the next one needed as far as a chunk holds them (the rows
   between that a reduced picture skips are read too, and passed over), and counts for its bytes, VIEW_READ_MIN at
   least, against the frame's VIEW_TICK: so a frame makes 16 reads at most, and reads 256 KB and a chunk at most */
static void read_some(struct gui_state *g, bool tick) {
    struct gui_viewer *v = &g->viewer;
    u32 budget = VIEW_TICK, first, rows, span, len, size, i, k, bytes;
    int r;
    while(v->loading && budget) {
        first = file_row(v, v->next);
        for(rows = 1; v->next + rows < v->dh && (file_row(v, v->next + rows) - first + 1) * v->bmp.stride <= GUI_VIEW_CHUNK;
            rows++) ;
        span = file_row(v, v->next + rows - 1) - first + 1;   /* the file's rows read, from first */
        bytes = span * v->bmp.stride;
        len = size = 0;
        r = g->fs && g->fs->read_at ? g->fs->read_at(g->fs->ctx, v->path, v->bmp.offset + first * v->bmp.stride,
                                                      (char *)v->chunk, bytes, &len, &size) : -ENOSYS;
        if(!r && len < bytes) r = -EIO;                 /* the file is shorter now than its headers said */
        if(r) { read_failed(g, r, tick); return; }
        for(i = 0; i < rows; i++) {
            k = v->next + i;
            decode_row(v, v->chunk + (file_row(v, k) - first) * v->bmp.stride, v->bmp.topdown ? k : v->dh - 1 - k);
        }
        v->next += rows;
        bytes = bytes > VIEW_READ_MIN ? bytes : VIEW_READ_MIN;
        budget = bytes < budget ? budget - bytes : 0;
        if(v->next >= v->dh) {
            v->loading = false;
            v->ready = true;
            v->status[0] = 0;
        }
    }
    if(v->loading)
        snprintf(v->status, sizeof(v->status), "Reading %s\xF9 %u%%", gui_basename(v->path), v->next * 100 / v->dh);
    g->dirty = true;
}

void viewer_show(struct gui_state *g, const char *path) {
    struct gui_viewer *v = &g->viewer;
    struct gui_bmp b;
    char why[80], name[GUI_NAME_MAX];
    int r;
    gui_open(g, GUI_VIEWER);
    strlcpy(name, gui_basename(path), sizeof(name));
    /* checked with a palette of its own: a picture being read or shown keeps its own until this one is good */
    r = bmp_open(g, path, &b, v->scratch, v->chunk, why, sizeof(why));
    if(r) {
        if(!v->ready && !v->loading) snprintf(v->status, sizeof(v->status), "%s: %s", name, why);
        if(!g->dialog.kind) dialog_message(g, "Cannot show the picture", name, why);
        return;
    }
    forget(v);
    strlcpy(v->path, path, sizeof(v->path));
    memcpy(v->palette, v->scratch, sizeof(v->palette));
    v->bmp = b;
    v->w = b.width;
    v->h = b.height;
    bmp_reduce(v->w, v->h, GUI_VIEW_PIXELS, &v->dw, &v->dh);
    v->loading = true;
    v->next = 0;
    v->fit = true;
    v->zoom = ONE;
    v->pan_x = v->pan_y = 0;
    v->click_ms = 0;
    read_some(g, false);                                /* the first piece now: a small picture is there at once */
}

/**** the window ****/

/* where the choosers open: the picture's folder, else the last chooser's */
static void chooser_dir(const struct gui_state *g, char *out, size_t n) {
    if(g->viewer.path[0]) { strlcpy(out, g->viewer.path, n); gui_parent(out); }
    else strlcpy(out, g->dialog.dir[0] ? g->dialog.dir : "/", n);
}

static bool tool_on(const struct gui_state *g, int a) {
    return a == VA_OPEN ? g->fs && g->fs->read_at : g->viewer.ready;
}

static void act(struct gui_state *g, int a) {
    char dir[GUI_PATH_MAX];
    if(!tool_on(g, a)) return;
    switch(a) {
    case VA_OPEN:
        chooser_dir(g, dir, sizeof(dir));
        dialog_chooser(g, GUI_DLG_OPEN, gui_app_act(GUI_VIEWER, VA_OPEN), dir, "");
        break;
    case VA_FIT: zoom_to(g, 0, true); break;
    case VA_ACTUAL: zoom_to(g, ONE, false); break;
    case VA_IN: case VA_OUT: zoom_step(g, a == VA_IN); break;
    }
    g->dirty = true;
}

static void opened(struct gui_state *g) {
    forget(&g->viewer);
    g->viewer.status[0] = 0;
}

static void closed(struct gui_state *g) {
    forget(&g->viewer);                                 /* the picture goes with the window, as Notepad's text does */
    g->viewer.status[0] = 0;
}

static void title_of(const struct gui_state *g, char *out, size_t n) {
    if(g->viewer.path[0]) snprintf(out, n, "%s - Image Viewer", gui_basename(g->viewer.path));
    else strlcpy(out, "Image Viewer", n);
}

static bool key(struct gui_state *g, int k) {
    struct gui_viewer *v = &g->viewer;
    struct view_layout l;
    layout(g, &l);
    if(k == 27 && v->loading) {                         /* Esc stops a reading */
        snprintf(v->status, sizeof(v->status), "Stopped reading %s", gui_basename(v->path));
        forget(v);
        g->dirty = true;
        return true;
    }
    switch(k) {
    case 'o': case 'O': act(g, VA_OPEN); return true;
    case 'f': case 'F': case '0': act(g, VA_FIT); return true;
    case '1': act(g, VA_ACTUAL); return true;
    case '+': case '=': act(g, VA_IN); return true;
    case '-': case '_': act(g, VA_OUT); return true;
    }
    if(!v->ready) return false;
    switch(k) {
    case GUI_LEFT: pan(g, -PAN_STEP, 0); return true;
    case GUI_RIGHT: pan(g, PAN_STEP, 0); return true;
    case GUI_UP: pan(g, 0, -PAN_STEP); return true;
    case GUI_DOWN: pan(g, 0, PAN_STEP); return true;
    case GUI_PGUP: pan(g, 0, -(l.h > 2 * PAN_STEP ? l.h - PAN_STEP : PAN_STEP)); return true;
    case GUI_PGDN: pan(g, 0, l.h > 2 * PAN_STEP ? l.h - PAN_STEP : PAN_STEP); return true;
    case GUI_HOME: v->pan_x = v->pan_y = 0; clamp_pan(g); return true;
    case GUI_END: v->pan_x = v->pan_y = 1 << 30; clamp_pan(g); return true;
    }
    return false;
}

static void press(struct gui_state *g, int x, int y) {
    struct gui_viewer *v = &g->viewer;
    struct view_layout l;
    int i = tool_hit(tools, TOOLS, x, y);
    if(i >= 0) { act(g, tools[i].act); return; }
    layout(g, &l);
    if(!v->ready || !gui_inside(x, y, l.x, l.y, l.w, l.h)) return;
    /* a double-click: a second press in about the same place soon after */
    if(v->click_ms && g->now - v->click_ms <= DOUBLE_MS && x - v->click_x >= -4 && x - v->click_x <= 4 &&
       y - v->click_y >= -4 && y - v->click_y <= 4) {
        v->click_ms = 0;
        zoom_to(g, ONE, !v->fit);
        return;
    }
    v->click_ms = g->now ? g->now : 1;
    v->click_x = x;
    v->click_y = y;
    clamp_pan(g);
    v->dragging = true;
    v->press_x = x;
    v->press_y = y;
    v->press_pan_x = v->pan_x;
    v->press_pan_y = v->pan_y;
}

static void drag(struct gui_state *g, int x, int y) {
    struct gui_viewer *v = &g->viewer;
    if(!v->dragging || !v->ready) return;
    v->pan_x = v->press_pan_x - (x - v->press_x);
    v->pan_y = v->press_pan_y - (y - v->press_y);
    clamp_pan(g);
}

static void release(struct gui_state *g) {
    g->viewer.dragging = false;
}

/* the wheel: up and down, or sideways when the picture is as tall as the room */
static void wheel(struct gui_state *g, int rows) {
    struct gui_viewer *v = &g->viewer;
    struct view_layout l;
    if(!v->ready) return;
    layout(g, &l);
    if(shown(v->h, zoom_of(v, &l)) > l.h) pan(g, 0, rows * 16);
    else pan(g, rows * 16, 0);
}

static void tick(struct gui_state *g) {
    if(g->viewer.loading) read_some(g, true);
}

static void answer(struct gui_state *g, int a, int button, const char *text, const char *target) {
    if(a == VIEW_SKETCH_SAVE || a == VIEW_SKETCH_REPLACE || a == VIEW_SKETCH_OPEN) {   /* Sketch's (guibmp.c) */
        sketch_answer(g, a, button, text, target);
        return;
    }
    if(a == VA_OPEN && button == 0) viewer_show(g, text);
}

/**** drawing ****/

/* the surface's pixels are the desktop's 0x00RRGGBB in 32 bits (shell/gui.c's frame, the host test's): written
   straight into it; otherwise a pixel at a time through the drawing kit */
static bool plain(const struct fb_surface *s) {
    return s->bytespp == 4 && s->mode.red_pos == 16 && s->mode.green_pos == 8 && s->mode.blue_pos == 0 &&
           s->mode.red_size == 8 && s->mode.green_size == 8 && s->mode.blue_size == 8;
}

/* the decoded picture scaled into d's rectangle; a shown pixel c is decoded pixel c * dw / pw, which each row
   steps through as a quotient and a remainder, so that nothing is divided in the loop and nothing drifts */
static void picture(const struct gui_state *g, const struct gui_draw *d, const struct view_layout *l) {
    const struct gui_viewer *v = &g->viewer;
    const struct fb_surface *s = d->s;
    bool direct = plain(s);
    int ox, oy, pw, ph, x0, x1, y0, y1, x, y;
    u32 qx0, rx0, qy, ry, q, r, sq, sr, tq, tr;
    place(g, l, &ox, &oy, &pw, &ph);
    x0 = ox > d->l ? ox : d->l;
    x1 = ox + pw < d->r ? ox + pw : d->r;
    y0 = oy > d->t ? oy : d->t;
    y1 = oy + ph < d->b ? oy + ph : d->b;
    if(x1 > (int)s->mode.width) x1 = s->mode.width;
    if(y1 > (int)s->mode.height) y1 = s->mode.height;
    if(x0 < 0) x0 = 0;
    if(y0 < 0) y0 = 0;
    if(x0 >= x1 || y0 >= y1) return;
    qx0 = (u32)div64_32((u64)(u32)(x0 - ox) * v->dw, (u32)pw, &rx0);
    qy = (u32)div64_32((u64)(u32)(y0 - oy) * v->dh, (u32)ph, &ry);
    sq = v->dw / (u32)pw;
    sr = v->dw % (u32)pw;
    tq = v->dh / (u32)ph;
    tr = v->dh % (u32)ph;
    for(y = y0; y < y1; y++) {
        const u8 *src = v->pixels + qy * v->dw * 3;
        u32 *out = direct ? (u32 *)(s->base + (u32)y * s->mode.pitch) : NULL;
        q = qx0;
        r = rx0;
        for(x = x0; x < x1; x++) {
            const u8 *p = src + q * 3;
            u32 c = (u32)p[0] << 16 | (u32)p[1] << 8 | p[2];
            if(out) out[x] = c;
            else gd_rect(d, x, y, 1, 1, c);
            q += sq;
            r += sr;
            if(r >= (u32)pw) { r -= pw; q++; }
        }
        qy += tq;
        ry += tr;
        if(ry >= (u32)ph) { ry -= ph; qy++; }
    }
}

/* the status bar's words for the picture: its name, its size, and what it was read at */
static void describe(const struct gui_viewer *v, char *out, size_t n) {
    if(v->dw != v->w || v->dh != v->h)
        snprintf(out, n, "%s  %u x %u, %u-bit, kept at %u x %u", gui_basename(v->path), v->w, v->h, v->bmp.bpp, v->dw, v->dh);
    else snprintf(out, n, "%s  %u x %u, %u-bit", gui_basename(v->path), v->w, v->h, v->bmp.bpp);
}

static void render(const struct gui_state *g, const struct gui_draw *d) {
    const struct gui_window *w = &g->win[GUI_VIEWER];
    const struct gui_viewer *v = &g->viewer;
    struct view_layout l;
    struct gui_draw room;
    bool on[TOOLS];
    char left[GUI_STATUS + GUI_NAME_MAX], right[32];
    int i, x, ty;
    u32 z;
    layout(g, &l);
    for(i = 0; i < TOOLS; i++) on[i] = tool_on(g, tools[i].act);
    gd_rect(d, w->x + 1, w->y + TITLE_H, w->w - 2, BAR_H + 2, BAND);
    tool_render(d, w->x, w->y, tools, TOOLS, on);
    gd_rect(d, w->x + 1, w->y + TITLE_H + BAR_H + 2, w->w - 2, 1, LINE);
    room = gd_clip(d, w->x + l.x, w->y + l.y, l.w, l.h);
    gd_rect(&room, w->x + l.x, w->y + l.y, l.w, l.h, NAVY);
    if(v->ready) picture(g, &room, &l);
    else {                                              /* the reading's progress, or what became of the last picture */
        ty = w->y + l.y + 16;                           /* (the tick says it only here and in the status bar) */
        if(v->status[0]) {
            gd_name(&room, w->x + l.x + 16, ty, v->status, (l.w - 32) / 8, v->loading ? FOAM : CORAL);
            ty += 22;
        }
        if(!v->loading) gd_name(&room, w->x + l.x + 16, ty, g->fs && g->fs->read_at ? "Open a BMP picture: the toolbar's "
                                "Open, or O." : "There are no files here to open.", (l.w - 32) / 8, FOAM);
    }
    gd_rect(d, w->x + 1, w->y + l.status_y, w->w - 2, STATUS_H, BAND);
    gd_rect(d, w->x + 1, w->y + l.status_y, w->w - 2, 1, LINE);
    right[0] = 0;
    if(v->status[0]) strlcpy(left, v->status, sizeof(left));
    else if(v->ready) {
        describe(v, left, sizeof(left));
        z = zoom_of(v, &l);
        snprintf(right, sizeof(right), "%s%u%%", v->fit ? "Fit " : "", (z * 100 + ONE / 2) >> 16);
    } else strlcpy(left, "No picture", sizeof(left));
    x = w->w - 24 - (int)strlen(right) * 8;             /* clear of the resize grip */
    gd_name(d, w->x + 10, w->y + l.status_y + 3, left, (x - 18) / 8, INK);
    gd_text(d, w->x + x, w->y + l.status_y + 3, right, MUTED);
}

/* 10/18/26: the arrows move a picture; with none they move the pointer */
static bool wants_arrows(const struct gui_state *g) { return g->viewer.ready; }

const struct gui_app gui_app_viewer = {
    .title = "Image Viewer", .icon = ICON_VIEWER, .shortcut = "", .w = 560, .h = 400,
    .resizable = true, .arrows = true, .keys = false,
    .min_width = min_width, .opened = opened, .closed = closed,
    .render = render, .key = key, .press = press, .drag = drag, .release = release, .wheel = wheel, .tick = tick,
    .title_of = title_of, .answer = answer,
    .help = help, .help_lines = (int)(sizeof(help) / sizeof(help[0])),
    .wants_arrows = wants_arrows,
};
