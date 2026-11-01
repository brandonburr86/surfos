/*
SurfOS Undertow's window: painting a laid-out page
----------------------
File: paint.c  Date: 10/25/26
----------------------
paint.h's painting, pixel by pixel and clipped: rectangles row by row, glyphs bit by bit from the console's
font (font.h), at a whole scale.
*/

#include <stddef.h>
#include <string.h>
#include "undertow.h"
#include "doc.h"
#include "font.h"
#include "layout.h"
#include "paint.h"

#define READABLE 32                     /* how far apart in brightness text and what is behind it must be, of 255 */

void wp_theme_default(struct wp_theme *t) {
    t->bg = 0xfffdf5;                   /* the desktop's text background */
    t->fg = 0x153d49;                   /* INK */
    t->link = 0x197d83;                 /* TEAL */
    t->vlink = 0x6c4f8c;
    t->hover = 0x102e3b;                /* NAVY */
    t->frame = 0x6c888c;                /* MUTED */
    t->rule = 0x9aa8a6;
    t->rule_light = 0xffffff;
    t->mark = 0xfff2a8;
    t->field_bg = 0xffffff;
    t->field_line = 0x6c888c;
    t->field_fg = 0x153d49;
    t->placeholder = 0x6c888c;
    t->button_bg = 0xdee3d8;            /* SHELL */
    t->button_line = 0x6c888c;
    t->image_line = 0x9aa8a6;
    t->image_fg = 0x6c888c;
}

/**** drawing in the viewport ****/

void wp_fill(const struct wp_target *t, int x, int y, int w, int h, unsigned int color) {
    int x0 = x < t->x ? t->x : x, y0 = y < t->y ? t->y : y;
    int x1 = x + w > t->x + t->cw ? t->x + t->cw : x + w, y1 = y + h > t->y + t->ch ? t->y + t->ch : y + h;
    int i, j;
    if(x1 > t->w) x1 = t->w;
    if(y1 > t->h) y1 = t->h;
    if(x0 < 0) x0 = 0;
    if(y0 < 0) y0 = 0;
    for(j = y0; j < y1; j++) {
        unsigned int *row = t->pixels + (long)j * t->stride;
        for(i = x0; i < x1; i++) row[i] = color;
    }
}

void wp_frame(const struct wp_target *t, int x, int y, int w, int h, unsigned int color) {
    if(w <= 0 || h <= 0) return;
    wp_fill(t, x, y, w, 1, color);
    wp_fill(t, x, y + h - 1, w, 1, color);
    wp_fill(t, x, y, 1, h, color);
    wp_fill(t, x + w - 1, y, 1, h, color);
}

/* One cell of a size at (x, y), its lit pixels only: bold as a smear to the right, italic as a slant */
static void glyph(const struct wp_target *t, int x, int y, unsigned char g, int size, unsigned int style,
                  unsigned int color) {
    int row, col, inside, w = wf_cell_w(size), h = wf_cell_h(size), bold = style & WS_BOLD ? wf_bold(size) : 0;
    int reach = w + bold + (style & WS_ITALIC ? h / 5 : 0);
    int cx0 = t->x, cy0 = t->y, cx1 = t->x + t->cw, cy1 = t->y + t->ch;
    if(cx1 > t->w) cx1 = t->w;
    if(cy1 > t->h) cy1 = t->h;
    if(cx0 < 0) cx0 = 0;
    if(cy0 < 0) cy0 = 0;
    if(x >= cx1 || y >= cy1 || x + reach <= cx0 || y + h <= cy0) return;
    inside = x >= cx0 && y >= cy0 && x + reach <= cx1 && y + h <= cy1;
    for(row = 0; row < h; row++) {
        unsigned int bits = wf_row(size, g, row) << 16, *line;
        int shift = style & WS_ITALIC ? (h - 1 - row) / 5 : 0;     /* 3 pixels at the top, 0 at the foot, at 16 */
        if(!bits || (!inside && (y + row < cy0 || y + row >= cy1))) continue;
        if(bold) bits |= bits >> 1;
        if(bold > 1) bits |= bits >> 1;
        line = t->pixels + (long)(y + row) * t->stride;
        for(col = 0; col < w + bold; col++) {
            int px = x + col + shift;
            if((bits & (0x80000000u >> col)) && (inside || (px >= cx0 && px < cx1))) line[px] = color;
        }
    }
}

int wp_text(const struct wp_target *t, int x, int y, const char *s, size_t n, int scale, unsigned int style,
            unsigned int color) {
    unsigned char cells[WF_CELLS_MAX];
    unsigned int cp;
    size_t i = 0;
    int k, c, w = wf_cell_w(scale);
    while(i < n) {
        if((unsigned char)s[i] < 0x80) cp = (unsigned char)s[i++];
        else i += ut_utf8_decode((const unsigned char *)s + i, n - i, &cp);
        k = wf_cells(cp, cells);
        for(c = 0; c < k; c++) {
            if(cells[c] != ' ' && cells[c] != 0xff) glyph(t, x, y, cells[c], scale, style, color);
            x += w;
        }
    }
    return x;
}

/* A run's text: proportional, by its cells' widths and bearings; WS_MONO text in its cells */
static void run_text(const struct wp_target *t, int x, int y, const char *s, size_t n, int size, unsigned int style,
                     unsigned int color) {
    unsigned char cells[WF_CELLS_MAX];
    unsigned int cp;
    size_t i = 0;
    int k, c;
    while(i < n) {
        if((unsigned char)s[i] < 0x80) cp = (unsigned char)s[i++];
        else i += ut_utf8_decode((const unsigned char *)s + i, n - i, &cp);
        k = wf_cells(cp, cells);
        for(c = 0; c < k; c++) {
            if(cells[c] != ' ' && cells[c] != 0xff) glyph(t, x - wf_bearing(size, cells[c], style), y, cells[c], size, style, color);
            x += wf_advance(size, cells[c], style);
        }
    }
}

/**** the page ****/

static unsigned int text_color(const struct wd_document *d, const struct wp_theme *th, const struct wl_item *it, int hover) {
    if(it->color != WD_NONE && it->color != WD_TEXT) return it->color;
    if(it->link >= 0 && it->color == WD_NONE) {
        if(it->link == hover) return th->hover;
        return d->link_color != WD_NONE ? d->link_color : th->link;
    }
    return d->fg != WD_NONE ? d->fg : th->fg;
}

static int luma(unsigned int c) { return (int)((c >> 16 & 255) * 299 + (c >> 8 & 255) * 587 + (c & 255) * 114) / 1000; }

/* 11/01/26: text meant for a background that is not here (an image, a gradient, a colour by var()) may be the colour of
   what is behind it now: white on the page's cream. Text no one can read helps nobody, so it takes the theme's colour,
   or white on what is dark. What is behind it is what is painted there. */
static unsigned int readable(const struct wp_target *t, const struct wp_theme *th, int x, int y, unsigned int c) {
    int px = x < t->x ? t->x : x, py = y < t->y ? t->y : y, back;
    if(px < 0 || py < 0 || px >= t->w || py >= t->h) return c;
    back = luma(t->pixels[(long)py * t->stride + px]);
    if(luma(c) - back < READABLE && back - luma(c) < READABLE) return back < 128 ? 0xffffff : th->fg;
    return c;
}

/* A field's or an image's label inside its box, clipped to it */
static void label_in(const struct wp_target *t, int x, int y, int w, int h, const char *s, size_t n, unsigned int color,
                     int center) {
    struct wp_target in = *t;
    int x0 = x > t->x ? x : t->x, y0 = y > t->y ? y : t->y;
    int x1 = x + w < t->x + t->cw ? x + w : t->x + t->cw, y1 = y + h < t->y + t->ch ? y + h : t->y + t->ch;
    int tw = (int)wf_count(s, n) * WF_W;
    if(x1 <= x0 || y1 <= y0) return;
    in.x = x0;
    in.y = y0;
    in.cw = x1 - x0;
    in.ch = y1 - y0;
    wp_text(&in, center && tw < w ? x + (w - tw) / 2 : x + 4, y + (h - WF_H) / 2, s, n, 1, 0, color);
}

static void field(const struct wp_target *t, const struct wd_document *d, const struct wp_theme *th,
                  const struct wl_item *it, int x, int y) {
    const char *s = d->text + it->off;
    switch(it->sub) {
    case WF_CHECKBOX: case WF_RADIO:
        wp_fill(t, x, y, it->w, it->h, th->field_bg);
        wp_frame(t, x, y, it->w, it->h, th->field_line);
        if(it->checked) wp_fill(t, x + 4, y + 4, it->w - 8, it->h - 8, th->field_fg);
        return;
    case WF_BUTTON: case WF_FILE:
        wp_fill(t, x, y, it->w, it->h, th->button_bg);
        wp_frame(t, x, y, it->w, it->h, th->button_line);
        label_in(t, x, y, it->w, it->h, s, it->len, th->field_fg, 1);
        return;
    case WF_SELECT:
        wp_fill(t, x, y, it->w, it->h, th->field_bg);
        wp_frame(t, x, y, it->w, it->h, th->field_line);
        label_in(t, x, y, it->w - 16, it->h, s, it->len, th->field_fg, 0);
        wp_text(t, x + it->w - 13, y + (it->h - WF_H) / 2, "\xe2\x96\xbc", 3, 1, 0, th->field_fg);  /* U+25BC */
        return;
    }
    wp_fill(t, x, y, it->w, it->h, th->field_bg);
    wp_frame(t, x, y, it->w, it->h, th->field_line);
    label_in(t, x, y, it->w, it->sub == WF_AREA ? WF_H + 8 : it->h, s, it->len, it->checked ? th->placeholder : th->field_fg, 0);
}

void wp_paint(const struct wl_layout *l, const struct wd_document *d, const struct wp_theme *th,
              const struct wp_target *t, int scroll_x, int scroll_y, int hover) {
    size_t i;
    int dx = t->x - scroll_x, dy = t->y - scroll_y;
    wp_fill(t, t->x, t->y, t->cw, t->ch, d->bg != WD_NONE ? d->bg : th->bg);
    for(i = 0; i < l->count; i++) {
        const struct wl_item *it = &l->items[i];
        int x = it->x + dx, y = it->y + dy;
        unsigned int c;
        if(y >= t->y + t->ch || y + it->h <= t->y || x >= t->x + t->cw || x + it->w <= t->x) continue;
        switch(it->kind) {
        case WI_FILL:
            wp_fill(t, x, y, it->w, it->h, it->color);
            break;
        case WI_FRAME:
            wp_frame(t, x, y, it->w, it->h, it->color != WD_NONE ? it->color : th->frame);
            break;
        case WI_RULE:
            wp_fill(t, x, y, it->w, 1, th->rule);
            wp_fill(t, x, y + 1, it->w, 1, th->rule_light);
            break;
        case WI_MARK:
            wp_text(t, x, y, l->marks + it->off, it->len, it->scale, 0, d->fg != WD_NONE ? d->fg : th->fg);
            break;
        case WI_FIELD:
            field(t, d, th, it, x, y);
            break;
        case WI_IMAGE:
            c = it->link >= 0 ? text_color(d, th, it, hover) : th->image_line;
            wp_frame(t, x, y, it->w, it->h, c);
            if(it->len) label_in(t, x, y, it->w, it->h, d->text + it->off, it->len, it->link >= 0 ? c : th->image_fg, 0);
            break;
        case WI_TEXT: {
            int s = it->scale;
            c = text_color(d, th, it, hover);
            if(it->style & WS_MARK) wp_fill(t, x, y, it->w, it->h, th->mark);
            c = readable(t, th, x, y, c);
            run_text(t, x, y, d->text + it->off, it->len, s, it->style, c);
            if((it->style & WS_UNDER) || (it->link >= 0 && !(it->style & WS_NOLINE)))
                wp_fill(t, x, y + wf_under(s), it->w, wf_stroke(s), c);
            if(it->style & WS_STRIKE) wp_fill(t, x, y + wf_strike(s), it->w, wf_stroke(s), c);
            break;
        }
        }
    }
}
