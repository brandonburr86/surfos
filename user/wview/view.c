/*
SurfOS user program: wview
--------------------
File: view.c    Date: 11/05/26
--------------------
The Image Viewer's model (view.h): the views and their arithmetic as the desktop's Image Viewer
had them (shell/guiview.c before 11/05/26): Fit, the default (a picture larger than the room is
shrunk to fit; a smaller one is enlarged by a whole number, as far as it fits and 16 at most, so
that its pixels stay square), the actual size, and zoom steps from 1/64 to 16 times about the
middle of the room; a zoomed picture moves with a drag, the arrows, Page Up and Down, Home, End and
the wheel; a double-click switches between Fit and the actual size. New with the program: the
folder's next and previous picture (N or Space, P or Backspace, the toolbar's arrows), the file's
details over the room (I), Copy path (Ctrl+C, which the desktop's Edit > Copy sends), and the
picture painted by view_paint() with an average where it is shown smaller, where the kernel's took
the nearest pixel, and its transparency over squares of grey.
*/

#include <stdio.h>
#include <string.h>
#include "view.h"

static const unsigned steps[] = { VIEW_ONE / 64, VIEW_ONE / 32, VIEW_ONE / 16, VIEW_ONE / 8, VIEW_ONE / 4, VIEW_ONE / 3,
                                  VIEW_ONE / 2, VIEW_ONE * 2 / 3, VIEW_ONE, VIEW_ONE * 3 / 2, VIEW_ONE * 2, VIEW_ONE * 3,
                                  VIEW_ONE * 4, VIEW_ONE * 6, VIEW_ONE * 8, VIEW_ONE * 12, VIEW_ONE * 16 };
#define STEPS (int)(sizeof(steps) / sizeof(steps[0]))

/* gw_button() draws a label in the console's characters as they are (code page 437, as the desktop's own buttons are),
   not UTF-8 as gw_text(): the arrows are its 0x11 and 0x10 */
static const char *const labels[VT_COUNT] = { "Open", "\x11", "\x10", "Fit", "Actual", "Zoom in", "Zoom out", "Details",
                                              "Copy path" };
static const int cells[VT_COUNT] = { 4, 1, 1, 3, 6, 7, 8, 7, 9 };        /* their characters */

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/**** where things are ****/

void view_room(const struct view *v, struct view_room *r) {
    r->x = 0;
    r->y = VIEW_ROOM_Y;
    r->w = v->win_w > 1 ? v->win_w : 1;
    r->h = v->win_h - VIEW_STATUS_H - VIEW_ROOM_Y > 1 ? v->win_h - VIEW_STATUS_H - VIEW_ROOM_Y : 1;
}

/* Fit's zoom: shrunk to the room, or enlarged by the largest whole number that fits. A room is at most 4096 a side (a
   window's widest), so a side << 16 fits 32 bits: nothing here divides in 64, which would need libgcc */
static unsigned fit_zoom(const struct view *v, const struct view_room *r) {
    unsigned k, zx, zy;
    if(v->pw <= 0 || v->ph <= 0) return VIEW_ONE;
    if(r->w >= v->pw && r->h >= v->ph) {
        k = (unsigned)(r->w / v->pw < r->h / v->ph ? r->w / v->pw : r->h / v->ph);
        return (k > 16 ? 16 : k) * VIEW_ONE;
    }
    zx = ((unsigned)r->w << 16) / (unsigned)v->pw;
    zy = ((unsigned)r->h << 16) / (unsigned)v->ph;
    zx = zx < zy ? zx : zy;
    return zx < VIEW_ZOOM_MIN ? VIEW_ZOOM_MIN : zx;
}

static unsigned zoom_in_room(const struct view *v, const struct view_room *r) {
    return v->fit ? fit_zoom(v, r) : v->zoom;
}

unsigned view_zoom(const struct view *v) {
    struct view_room r;
    view_room(v, &r);
    return zoom_in_room(v, &r);
}

/* n pixels of the picture at zoom z: the screen's pixels, at least one (4096 at 16 times: 65536) */
static int shown(int n, unsigned z) {
    unsigned long long s = (unsigned long long)(unsigned)n * z >> 16;
    return s ? (int)s : 1;
}

static int pan_max(int size, int room) {
    return size > room ? size - room : 0;
}

void view_place(const struct view *v, int *x, int *y, int *w, int *h) {
    struct view_room r;
    unsigned z;
    view_room(v, &r);
    z = zoom_in_room(v, &r);
    *w = shown(v->pw, z);
    *h = shown(v->ph, z);
    *x = r.x + (*w < r.w ? (r.w - *w) / 2 : -clamp(v->pan_x, 0, pan_max(*w, r.w)));
    *y = r.y + (*h < r.h ? (r.h - *h) / 2 : -clamp(v->pan_y, 0, pan_max(*h, r.h)));
}

void view_init(struct view *v, int win_w, int win_h) {
    memset(v, 0, sizeof(*v));
    v->win_w = win_w;
    v->win_h = win_h;
    v->fit = 1;
    v->zoom = VIEW_ONE;
}

/**** moving and zooming ****/

static void clamp_pan(struct view *v) {
    struct view_room r;
    unsigned z;
    view_room(v, &r);
    z = zoom_in_room(v, &r);
    v->pan_x = clamp(v->pan_x, 0, pan_max(shown(v->pw, z), r.w));
    v->pan_y = clamp(v->pan_y, 0, pan_max(shown(v->ph, z), r.h));
}

static void pan(struct view *v, int dx, int dy) {
    clamp_pan(v);                       /* from where it is shown: the window may have changed */
    v->pan_x += dx;
    v->pan_y += dy;
    clamp_pan(v);
}

void view_resize(struct view *v, int win_w, int win_h) {
    v->win_w = win_w;
    v->win_h = win_h;
    clamp_pan(v);
}

void view_picture(struct view *v, int w, int h) {
    v->pw = w > 0 && h > 0 ? w : 0;
    v->ph = w > 0 && h > 0 ? h : 0;
    v->fit = 1;
    v->zoom = VIEW_ONE;
    v->pan_x = v->pan_y = 0;
    v->dragging = 0;
    v->click_ms = 0;
}

/* the point of the picture in the middle of the room, as a fraction (16.16) of the picture shown: a / size in two
   steps of 32 bits, exactly (a is at most size, which is at most 65536, so the remainder << 16 fits) */
static unsigned middle(int p, int size, int room) {
    unsigned a, s = (unsigned)size;
    if(size <= room) return VIEW_ONE / 2;
    a = (unsigned)(clamp(p, 0, size - room) + room / 2);
    return (a / s << 16) + (a % s << 16) / s;
}

/* a new zoom (or Fit), with the picture's point in the middle of the room staying there */
static void zoom_to(struct view *v, unsigned z, int fit) {
    struct view_room r;
    unsigned old, mx, my;
    view_room(v, &r);
    old = zoom_in_room(v, &r);
    mx = middle(v->pan_x, shown(v->pw, old), r.w);
    my = middle(v->pan_y, shown(v->ph, old), r.h);
    v->fit = fit;
    if(!fit) v->zoom = z < VIEW_ZOOM_MIN ? VIEW_ZOOM_MIN : z > VIEW_ZOOM_MAX ? VIEW_ZOOM_MAX : z;
    z = zoom_in_room(v, &r);
    v->pan_x = (int)((unsigned long long)mx * (unsigned)shown(v->pw, z) >> 16) - r.w / 2;
    v->pan_y = (int)((unsigned long long)my * (unsigned)shown(v->ph, z) >> 16) - r.h / 2;
    clamp_pan(v);
}

/* the next step in or out from the zoom shown (Fit's may lie between two) */
static void zoom_step(struct view *v, int in) {
    unsigned z = view_zoom(v);
    int i;
    if(in) {
        for(i = 0; i < STEPS && steps[i] <= z + (z >> 7); i++) ;
        if(i < STEPS) zoom_to(v, steps[i], 0);
    } else {
        for(i = STEPS - 1; i >= 0 && steps[i] >= z - (z >> 7); i--) ;
        if(i >= 0) zoom_to(v, steps[i], 0);
    }
}

/**** the toolbar ****/

void view_tool(int tool, struct view_tool *t) {
    int i, at = 7;                      /* the desktop's toolbar: 8 from the window's edge, its frame's pixel in that */
    for(i = 0; i < tool && i < VT_COUNT; i++) at += cells[i] * 8 + 14 + 4;
    t->x = at;
    t->y = 4;
    t->w = tool >= 0 && tool < VT_COUNT ? cells[tool] * 8 + 14 : 0;
    t->h = 22;
}

const char *view_tool_label(int tool) {
    return tool >= 0 && tool < VT_COUNT ? labels[tool] : "";
}

int view_tool_at(int x, int y) {
    struct view_tool t;
    int i;
    for(i = 0; i < VT_COUNT; i++) {
        view_tool(i, &t);
        if(x >= t.x && x < t.x + t.w && y >= t.y && y < t.y + t.h) return i;
    }
    return -1;
}

int view_tool_on(const struct view *v, int tool) {
    switch(tool) {
    case VT_OPEN: return 1;
    case VT_PREVIOUS: case VT_NEXT: return v->pictures > 1;
    case VT_DETAILS: case VT_COPY: return v->file.path[0] != 0;
    default: return v->pw > 0;
    }
}

int view_act(struct view *v, int tool) {
    if(!view_tool_on(v, tool)) return VIEW_NOTHING;
    switch(tool) {
    case VT_OPEN: return VIEW_OPEN;
    case VT_PREVIOUS: return VIEW_PREVIOUS;
    case VT_NEXT: return VIEW_NEXT;
    case VT_FIT: zoom_to(v, 0, 1); break;
    case VT_ACTUAL: zoom_to(v, VIEW_ONE, 0); break;
    case VT_IN: case VT_OUT: zoom_step(v, tool == VT_IN); break;
    case VT_DETAILS: v->details = !v->details; break;
    case VT_COPY: return VIEW_COPY;
    }
    return VIEW_DRAW;
}

/**** keys and the pointer ****/

int view_key(struct view *v, int key) {
    struct view_room r;
    view_room(v, &r);
    v->status[0] = 0;
    switch(key) {
    case 'o': case 'O': case 15: return view_act(v, VT_OPEN);       /* and Ctrl+O */
    case 'f': case 'F': case '0': return view_act(v, VT_FIT);
    case '1': return view_act(v, VT_ACTUAL);
    case '+': case '=': return view_act(v, VT_IN);
    case '-': case '_': return view_act(v, VT_OUT);
    case 'n': case 'N': case ' ': return view_act(v, VT_NEXT);
    case 'p': case 'P': case 8: case 127: return view_act(v, VT_PREVIOUS);
    case 'i': case 'I': return view_act(v, VT_DETAILS);
    case 3: return view_act(v, VT_COPY);                /* Ctrl+C, and the desktop's Edit > Copy */
    case 23: return VIEW_CLOSE;                         /* Ctrl+W */
    case 27:                                            /* Esc: the details first, then the window */
        if(v->details) {
            v->details = 0;
            return VIEW_DRAW;
        }
        return VIEW_CLOSE;
    }
    if(v->pw <= 0) return VIEW_NOTHING;
    switch(key) {
    case VIEW_KEY_LEFT: pan(v, -VIEW_PAN_STEP, 0); break;
    case VIEW_KEY_RIGHT: pan(v, VIEW_PAN_STEP, 0); break;
    case VIEW_KEY_UP: pan(v, 0, -VIEW_PAN_STEP); break;
    case VIEW_KEY_DOWN: pan(v, 0, VIEW_PAN_STEP); break;
    case VIEW_KEY_PGUP: pan(v, 0, -(r.h > 2 * VIEW_PAN_STEP ? r.h - VIEW_PAN_STEP : VIEW_PAN_STEP)); break;
    case VIEW_KEY_PGDN: pan(v, 0, r.h > 2 * VIEW_PAN_STEP ? r.h - VIEW_PAN_STEP : VIEW_PAN_STEP); break;
    case VIEW_KEY_HOME: v->pan_x = v->pan_y = 0; clamp_pan(v); break;
    case VIEW_KEY_END: v->pan_x = v->pan_y = 1 << 30; clamp_pan(v); break;
    default: return VIEW_NOTHING;
    }
    return VIEW_DRAW;
}

/* Moving, zooming and stepping go on while their key is held; what opens, shows, copies or closes something acts once */
int view_repeats(int key) {
    switch(key) {
    case VIEW_KEY_LEFT: case VIEW_KEY_RIGHT: case VIEW_KEY_UP: case VIEW_KEY_DOWN: case VIEW_KEY_PGUP: case VIEW_KEY_PGDN:
    case '+': case '=': case '-': case '_': case 'n': case 'N': case ' ': case 'p': case 'P': case 8: case 127:
        return 1;
    }
    return 0;
}

int view_press(struct view *v, int x, int y) {
    struct view_room r;
    int t = view_tool_at(x, y);
    if(t >= 0) {
        v->status[0] = 0;
        return view_act(v, t);
    }
    view_room(v, &r);
    if(x < r.x || x >= r.x + r.w || y < r.y || y >= r.y + r.h) return VIEW_NOTHING;
    if(v->details) {                                    /* a press in the room puts the details away */
        v->details = 0;
        return VIEW_DRAW;
    }
    if(v->pw <= 0) return VIEW_NOTHING;
    /* a double-click: a second press in about the same place soon after */
    if(v->click_ms && v->now - v->click_ms <= VIEW_DOUBLE_MS && x - v->click_x >= -4 && x - v->click_x <= 4 &&
       y - v->click_y >= -4 && y - v->click_y <= 4) {
        v->click_ms = 0;
        zoom_to(v, VIEW_ONE, !v->fit);
        return VIEW_DRAW;
    }
    v->click_ms = v->now ? v->now : 1;
    v->click_x = x;
    v->click_y = y;
    clamp_pan(v);
    v->dragging = 1;
    v->press_x = x;
    v->press_y = y;
    v->press_pan_x = v->pan_x;
    v->press_pan_y = v->pan_y;
    return VIEW_NOTHING;
}

void view_drag(struct view *v, int x, int y) {
    if(!v->dragging || v->pw <= 0) return;
    v->pan_x = v->press_pan_x - (x - v->press_x);
    v->pan_y = v->press_pan_y - (y - v->press_y);
    clamp_pan(v);
}

void view_release(struct view *v) {
    v->dragging = 0;
}

/* the wheel: up and down, or sideways when the picture is as tall as the room */
void view_wheel(struct view *v, int rows) {
    struct view_room r;
    if(v->pw <= 0) return;
    view_room(v, &r);
    if(shown(v->ph, zoom_in_room(v, &r)) > r.h) pan(v, 0, rows * 16);
    else pan(v, rows * 16, 0);
}

/**** words ****/

const char *view_name(const char *path) {
    const char *s = strrchr(path, '/');
    return s && s[1] ? s + 1 : path;
}

void view_title(const struct view *v, char *out, size_t n) {
    if(v->file.path[0]) snprintf(out, n, "%s - Image Viewer", view_name(v->file.path));
    else snprintf(out, n, "Image Viewer");
}

static void zoom_text(const struct view *v, char *out, size_t n) {
    unsigned z = view_zoom(v);
    snprintf(out, n, "%s%u%%", v->fit ? "Fit " : "", (unsigned)(((unsigned long long)z * 100 + VIEW_ONE / 2) >> 16));
}

void view_status(const struct view *v, char *left, size_t ln, char *right, size_t rn) {
    right[0] = 0;
    if(v->status[0]) snprintf(left, ln, "%s", v->status);
    else if(v->pw > 0 && v->file.path[0]) {
        if(v->file.count > 1) snprintf(left, ln, "%s  %d x %d, %s  (%d of %d)", view_name(v->file.path), v->file.w,
                                       v->file.h, v->file.kind, v->file.index, v->file.count);
        else snprintf(left, ln, "%s  %d x %d, %s", view_name(v->file.path), v->file.w, v->file.h, v->file.kind);
    } else snprintf(left, ln, "No picture");
    if(v->pw > 0) zoom_text(v, right, rn);
}

/* n with a comma between each three digits */
static void grouped(unsigned n, char *out, size_t size) {
    char d[16];
    int len = snprintf(d, sizeof(d), "%u", n), i, o = 0;
    for(i = 0; i < len && o + 2 < (int)size; i++) {
        if(i && (len - i) % 3 == 0) out[o++] = ',';
        out[o++] = d[i];
    }
    out[o] = 0;
}

/* seconds since 1970 as the date and time they are (the PC's clock keeps local time), by the Gregorian calendar */
static void when(unsigned t, char *out, size_t size) {
    static const unsigned char month_days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    unsigned days = t / 86400, s = t % 86400, year = 1970, month = 0, n;
    for(;;) {
        n = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0) ? 366 : 365;
        if(days < n) break;
        days -= n;
        year++;
    }
    for(; month < 11; month++) {
        n = month_days[month] + (month == 1 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
        if(days < n) break;
        days -= n;
    }
    snprintf(out, size, "%04u-%02u-%02u %02u:%02u", year, month + 1, days + 1, s / 3600, s / 60 % 60);
}

int view_details(const struct view *v, char lines[VIEW_DETAILS][VIEW_DETAIL]) {
    const struct view_file *f = &v->file;
    char dir[VIEW_PATH], bytes[24], zoom[24], at[24];
    const char *name;
    int n = 0;
    size_t k;
    if(!f->path[0]) return 0;
    name = view_name(f->path);
    k = (size_t)(name - f->path);
    snprintf(dir, sizeof(dir), "%.*s", (int)(k > 1 ? k - 1 : k), f->path);
    grouped(f->size, bytes, sizeof(bytes));
    zoom_text(v, zoom, sizeof(zoom));
    snprintf(lines[n++], VIEW_DETAIL, "%s", name);
    snprintf(lines[n++], VIEW_DETAIL, "Folder: %.*s", VIEW_DETAIL - 9, k ? dir : ".");    /* cut as drawn, a dot after */
    snprintf(lines[n++], VIEW_DETAIL, "Kind: %s picture", f->kind[0] ? f->kind : "a");
    snprintf(lines[n++], VIEW_DETAIL, "File: %s bytes", bytes);
    snprintf(lines[n++], VIEW_DETAIL, "Picture: %d x %d pixels", f->w, f->h);
    if(f->mtime) {
        when(f->mtime, at, sizeof(at));
        snprintf(lines[n++], VIEW_DETAIL, "Changed: %s", at);
    }
    if(v->pw > 0) snprintf(lines[n++], VIEW_DETAIL, "Shown: %s", zoom);
    if(f->count > 0) snprintf(lines[n++], VIEW_DETAIL, "In its folder: %d of %d", f->index, f->count);
    return n;
}

/**** the picture ****/

/* the colour shown for a pixel of transparency t (0 solid, 255 not there) over the squares, at x, y of the window */
static unsigned over(unsigned c, unsigned t, int x, int y) {
    unsigned k = ((x >> 3) ^ (y >> 3)) & 1 ? 0xC9CFC4 : 0xF4F1E6, a = 255 - t, out = 0;
    int shift;
    if(!t) return c & 0xffffff;
    for(shift = 0; shift <= 16; shift += 8) {
        unsigned p = c >> shift & 255, q = k >> shift & 255;
        out |= (p * a + q * t + 127) / 255 << shift;
    }
    return out;
}

/* the pixels x0 .. x1, y0 .. y1 of the picture, averaged: transparency counts in full, colour only where there is
   some (image_scale.c's rule) */
static unsigned block(const unsigned *px, int pw, int x0, int x1, int y0, int y1) {
    unsigned a = 0, r = 0, g = 0, b = 0, n = 0, seen = 0;
    int x, y;
    for(y = y0; y < y1; y++) {
        const unsigned *row = px + (size_t)y * (size_t)pw;
        for(x = x0; x < x1; x++) {
            unsigned c = row[x];
            a += c >> 24;
            n++;
            if((c >> 24) == 255) continue;
            r += c >> 16 & 255;
            g += c >> 8 & 255;
            b += c & 255;
            seen++;
        }
    }
    if(!seen) return 0xff000000u;
    return (a + n / 2) / n << 24 | (r + seen / 2) / seen << 16 | (g + seen / 2) / seen << 8 | (b + seen / 2) / seen;
}

/* The source columns of each shown column: span[i] .. span[i + 1] for column i of those painted. A window is at most
   WIN_STRIDE_MAX (4096) wide, and so is the room */
#define SPAN_MAX 4096
static unsigned short span_x[SPAN_MAX + 1];

void view_paint(const struct view *v, const unsigned *px, unsigned *dst, int stride) {
    struct view_room r;
    int ox, oy, sw, sh, x0, x1, y0, y1, x, y, i;
    if(v->pw <= 0 || v->ph <= 0 || !px) return;
    view_room(v, &r);
    view_place(v, &ox, &oy, &sw, &sh);
    x0 = ox > r.x ? ox : r.x;
    x1 = ox + sw < r.x + r.w ? ox + sw : r.x + r.w;
    y0 = oy > r.y ? oy : r.y;
    y1 = oy + sh < r.y + r.h ? oy + sh : r.y + r.h;
    if(x1 > stride) x1 = stride;
    if(x0 < 0) x0 = 0;
    if(y0 < 0) y0 = 0;
    if(x0 >= x1 || y0 >= y1 || x1 - x0 > SPAN_MAX) return;
    /* the column of the picture each shown column starts at: a place in the shown picture (at most 65536) times the
       picture's side (at most 4096) fits 32 bits */
    for(i = 0; i <= x1 - x0; i++) span_x[i] = (unsigned short)((unsigned)(x0 - ox + i) * (unsigned)v->pw / (unsigned)sw);
    for(y = y0; y < y1; y++) {
        int r0 = (int)((unsigned)(y - oy) * (unsigned)v->ph / (unsigned)sh);
        int r1 = (int)((unsigned)(y - oy + 1) * (unsigned)v->ph / (unsigned)sh);
        unsigned *out = dst + (size_t)y * (size_t)stride;
        if(r1 <= r0) r1 = r0 + 1;
        if(r1 > v->ph) r1 = v->ph;
        for(x = x0, i = 0; x < x1; x++, i++) {
            int c0 = span_x[i], c1 = span_x[i + 1];
            unsigned c;
            if(c1 <= c0) c1 = c0 + 1;
            if(c1 > v->pw) c1 = v->pw;
            c = c1 - c0 == 1 && r1 - r0 == 1 ? px[(size_t)r0 * (size_t)v->pw + (size_t)c0] : block(px, v->pw, c0, c1, r0, r1);
            out[x] = over(c, c >> 24, x, y);
        }
    }
}
