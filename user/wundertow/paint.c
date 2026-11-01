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
#include "form.h"
#include "layout.h"
#include "paint.h"

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

/* 11/1/26: each pixel multiplied by color: a highlight over what is painted, text and all */
void wp_tint(const struct wp_target *t, int x, int y, int w, int h, unsigned int color) {
    int x0 = x < t->x ? t->x : x, y0 = y < t->y ? t->y : y;
    int x1 = x + w > t->x + t->cw ? t->x + t->cw : x + w, y1 = y + h > t->y + t->ch ? t->y + t->ch : y + h;
    unsigned int cr = color >> 16 & 255, cg = color >> 8 & 255, cb = color & 255;
    int i, j;
    if(x1 > t->w) x1 = t->w;
    if(y1 > t->h) y1 = t->h;
    if(x0 < 0) x0 = 0;
    if(y0 < 0) y0 = 0;
    for(j = y0; j < y1; j++) {
        unsigned int *row = t->pixels + (long)j * t->stride;
        for(i = x0; i < x1; i++) {
            unsigned int c = row[i];
            row[i] = ((c >> 16 & 255) * cr / 255) << 16 | ((c >> 8 & 255) * cg / 255) << 8 | (c & 255) * cb / 255;
        }
    }
}

/* One cell of the font at (x, y), its lit pixels only */
static void glyph(const struct wp_target *t, int x, int y, unsigned char g, int scale, unsigned int style,
                  unsigned int color) {
    int row, col, inside, s = scale < 1 ? 1 : scale;
    int cx0 = t->x, cy0 = t->y, cx1 = t->x + t->cw, cy1 = t->y + t->ch;
    if(cx1 > t->w) cx1 = t->w;
    if(cy1 > t->h) cy1 = t->h;
    if(cx0 < 0) cx0 = 0;
    if(cy0 < 0) cy0 = 0;
    if(x >= cx1 || y >= cy1 || x + (WF_W + 4) * s <= cx0 || y + WF_H * s <= cy0) return;
    inside = x >= cx0 && y >= cy0 && x + (WF_W + 4) * s <= cx1 && y + WF_H * s <= cy1;
    for(row = 0; row < WF_H; row++) {
        unsigned int bits = font8x16[g][row];
        int shift = style & WS_ITALIC ? (WF_H - 1 - row) / 5 * s : 0;     /* 3 pixels at the top, 0 at the foot */
        if(!bits) continue;
        if(style & WS_BOLD) bits |= bits >> 1;
        for(col = 0; col < WF_W; col++) {
            int px, py, a, b;
            if(!(bits & (0x80u >> col))) continue;
            px = x + col * s + shift;
            py = y + row * s;
            for(b = 0; b < s; b++) {
                unsigned int *line;
                if(!inside && (py + b < cy0 || py + b >= cy1)) continue;
                line = t->pixels + (long)(py + b) * t->stride;
                for(a = 0; a < s; a++) {
                    if(!inside && (px + a < cx0 || px + a >= cx1)) continue;
                    line[px + a] = color;
                }
            }
        }
    }
}

int wp_text(const struct wp_target *t, int x, int y, const char *s, size_t n, int scale, unsigned int style,
            unsigned int color) {
    unsigned char cells[WF_CELLS_MAX];
    unsigned int cp;
    size_t i = 0;
    int k, c, sc = scale < 1 ? 1 : scale;
    while(i < n) {
        if((unsigned char)s[i] < 0x80) cp = (unsigned char)s[i++];
        else i += ut_utf8_decode((const unsigned char *)s + i, n - i, &cp);
        k = wf_cells(cp, cells);
        for(c = 0; c < k; c++) {
            if(cells[c] != ' ' && cells[c] != 0xff) glyph(t, x, y, cells[c], sc, style, color);
            x += WF_W * sc;
        }
    }
    return x;
}

/**** the page ****/

static unsigned int text_color(const struct wd_document *d, const struct wp_theme *th, const struct wl_item *it, int hover) {
    if(it->color != WD_NONE) return it->color;
    if(it->link >= 0) {
        if(it->link == hover) return th->hover;
        if(d->visited && d->visited[it->link]) return d->vlink_color != WD_NONE ? d->vlink_color : th->vlink;
        return d->link_color != WD_NONE ? d->link_color : th->link;
    }
    return d->fg != WD_NONE ? d->fg : th->fg;
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

/* A text area's lines, from its first shown, in its box */
static void area_text(const struct wp_target *t, const struct wd_control *c, const struct wp_theme *th, int x, int y, int w, int h) {
    int lines = (h - 2 * 4) / WF_H, cols = (w - 2 * 4) / WF_W, l;
    if(cols < 1) cols = 1;
    for(l = 0; l < lines; l++) {
        size_t a = fm_at(c, c->top + l, 0), e = fm_at(c, c->top + l, 1 << 30);
        int line, col;
        if(a >= c->len && (c->top + l) > 0) {       /* past the last line: fm_at() says the end */
            fm_where(c, c->len, &line, &col);
            if(c->top + l > line) break;
        }
        if(e > a) label_in(t, x, y + l * WF_H, w, WF_H + 8, c->value + a, e - a, th->field_fg, 0);
    }
}

static void field(const struct wp_target *t, const struct wd_document *d, const struct wp_theme *th,
                  const struct wl_item *it, int x, int y) {
    const struct wd_control *c = it->control >= 0 ? &d->controls[it->control] : NULL;
    const char *s = d->text + it->off;
    size_t n = it->len;
    char stars[64];
    int checked = c ? c->checked : it->checked;
    if(c && (it->sub == WF_SELECT || it->sub == WF_PASSWORD || ((it->sub == WF_TEXT) && c->len))) s = fm_shown(d, c, stars, &n);
    else if(c && it->sub == WF_TEXT && !it->checked) n = 0;     /* emptied by a person: not the page's value again */
    switch(it->sub) {
    case WF_CHECKBOX: case WF_RADIO:
        wp_fill(t, x, y, it->w, it->h, th->field_bg);
        wp_frame(t, x, y, it->w, it->h, th->field_line);
        if(checked) wp_fill(t, x + 4, y + 4, it->w - 8, it->h - 8, th->field_fg);
        return;
    case WF_BUTTON: case WF_FILE:
        wp_fill(t, x, y, it->w, it->h, th->button_bg);
        wp_frame(t, x, y, it->w, it->h, th->button_line);
        label_in(t, x, y, it->w, it->h, s, n, th->field_fg, 1);
        return;
    case WF_SELECT:
        wp_fill(t, x, y, it->w, it->h, th->field_bg);
        wp_frame(t, x, y, it->w, it->h, th->field_line);
        label_in(t, x, y, it->w - 16, it->h, s, n, th->field_fg, 0);
        wp_text(t, x + it->w - 13, y + (it->h - WF_H) / 2, "\xe2\x96\xbc", 3, 1, 0, th->field_fg);  /* U+25BC */
        return;
    }
    wp_fill(t, x, y, it->w, it->h, th->field_bg);
    wp_frame(t, x, y, it->w, it->h, th->field_line);
    if(c && it->sub == WF_AREA) area_text(t, c, th, x, y, it->w, it->h);
    else label_in(t, x, y, it->w, it->sub == WF_AREA ? WF_H + 8 : it->h, s, n,
                  it->checked && !(c && c->len) ? th->placeholder : th->field_fg, 0);
}

/* over what is there: a pixel half there is mixed with it */
static unsigned int mix(unsigned int under, unsigned int c) {
    unsigned int t = c >> 24, s = 255 - t;
    unsigned int r = ((c >> 16 & 255) * s + (under >> 16 & 255) * t + 127) / 255;
    unsigned int g = ((c >> 8 & 255) * s + (under >> 8 & 255) * t + 127) / 255;
    unsigned int b = ((c & 255) * s + (under & 255) * t + 127) / 255;
    return r << 16 | g << 8 | b;
}

/* An image's picture at its box (x, y) on the target: pixels made to its size when there are any, as they are when
   they are its size, else the nearest (a picture shown larger than it is kept). 0 when there are no pixels. */
static int picture(const struct wp_target *t, const struct wd_document *d, const struct wl_item *it, int x, int y) {
    const struct wd_picture *m = it->image >= 0 ? &d->pics[it->image] : NULL;
    const unsigned int *src;
    int sw, sh, cx0, cy0, cx1, cy1, i, j;
    if(!m || !m->px || it->w <= 0 || it->h <= 0) return 0;
    if(m->scaled && m->sw == it->w && m->sh == it->h) {
        src = m->scaled;
        sw = m->sw;
        sh = m->sh;
    } else {
        src = m->px;
        sw = m->pw;
        sh = m->ph;
    }
    cx0 = x > t->x ? x : t->x;
    cy0 = y > t->y ? y : t->y;
    cx1 = x + it->w < t->x + t->cw ? x + it->w : t->x + t->cw;
    cy1 = y + it->h < t->y + t->ch ? y + it->h : t->y + t->ch;
    if(cx0 < 0) cx0 = 0;
    if(cy0 < 0) cy0 = 0;
    if(cx1 > t->w) cx1 = t->w;
    if(cy1 > t->h) cy1 = t->h;
    for(j = cy0; j < cy1; j++) {
        unsigned int *row = t->pixels + (long)j * t->stride;
        const unsigned int *s = src + (size_t)(sh == it->h ? j - y : (int)((long long)(j - y) * sh / it->h)) * sw;
        for(i = cx0; i < cx1; i++) {
            unsigned int c = s[sw == it->w ? i - x : (int)((long long)(i - x) * sw / it->w)];
            if(!(c >> 24)) row[i] = c;
            else if((c >> 24) != 255) row[i] = mix(row[i], c);
        }
    }
    return 1;
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
            wp_text(t, x, y, l->marks + it->off, it->len, 1, 0, d->fg != WD_NONE ? d->fg : th->fg);
            break;
        case WI_FIELD:
            field(t, d, th, it, x, y);
            break;
        case WI_IMAGE:
            if(picture(t, d, it, x, y)) break;
            c = it->link >= 0 ? text_color(d, th, it, hover) : th->image_line;
            wp_frame(t, x, y, it->w, it->h, c);
            if(it->len) label_in(t, x, y, it->w, it->h, d->text + it->off, it->len, it->link >= 0 ? c : th->image_fg, 0);
            break;
        case WI_TEXT: {
            int s = it->scale;
            c = text_color(d, th, it, hover);
            if(it->style & WS_MARK) wp_fill(t, x, y, it->w, it->h, th->mark);
            wp_text(t, x, y, d->text + it->off, it->len, s, it->style, c);
            if((it->style & WS_UNDER) || it->link >= 0) wp_fill(t, x, y + WF_UNDER * s, it->w, s, c);
            if(it->style & WS_STRIKE) wp_fill(t, x, y + 8 * s, it->w, s, c);
            break;
        }
        }
    }
}
