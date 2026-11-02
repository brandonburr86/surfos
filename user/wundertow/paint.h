/*
SurfOS Undertow's window: painting a laid-out page
----------------------
File: paint.h  Date: 10/25/26
----------------------
Any part of a layout (layout.h) into a 0x00RRGGBB image: the page's rectangle from (scroll_x, scroll_y) into a
viewport of the image, everything clipped to it. Text in the console's font (font.h) at its scale, bold as a
pixel's smear, italic as a slant, underlined and struck through by rows; links in the link colour and
underlined; fields and images' boxes as the desktop draws its own (GUI.md's palette). The page's own colours
(<body>, <font>, bgcolor) come first, the theme's where it gives none.
*/

#ifndef WUNDERTOW_PAINT_H
#define WUNDERTOW_PAINT_H

#include "doc.h"
#include "layout.h"

struct wp_theme {
    unsigned int bg;                    /* the page's background */
    unsigned int fg;                    /* its text */
    unsigned int link, vlink, hover;    /* links, visited links, the link under the pointer */
    unsigned int frame;                 /* tables' and cells' borders */
    unsigned int rule, rule_light;      /* <hr>: its dark and its light line */
    unsigned int mark;                  /* <mark>'s background */
    unsigned int field_bg, field_line, field_fg, placeholder;
    unsigned int button_bg, button_line;
    unsigned int image_line, image_fg;  /* an image's box until images are loaded */
};

/* The desktop's colours: INK on the text background FFFDF5, links in TEAL */
void wp_theme_default(struct wp_theme *t);

struct wp_target {
    unsigned int *pixels;               /* the image: rows of stride pixels, 0x00RRGGBB */
    int stride, w, h;
    int x, y, cw, ch;                   /* the viewport in it, which the page is painted into */
};

/* Paint l (made of d) from page point (scroll_x, scroll_y) into t's viewport; the link hover is drawn as the
   link under the pointer (-1: none) */
void wp_paint(const struct wl_layout *l, const struct wd_document *d, const struct wp_theme *th,
              const struct wp_target *t, int scroll_x, int scroll_y, int hover);

/* Drawing for the window's own chrome too: a filled rectangle, a frame, and text (UTF-8, as layout.c measures
   it) at scale, clipped to the target's viewport; text returns the x after it */
void wp_fill(const struct wp_target *t, int x, int y, int w, int h, unsigned int color);
void wp_frame(const struct wp_target *t, int x, int y, int w, int h, unsigned int color);
void wp_tint(const struct wp_target *t, int x, int y, int w, int h, unsigned int color);    /* what is there, through a colour */
int wp_text(const struct wp_target *t, int x, int y, const char *s, size_t n, int scale, unsigned int style,
            unsigned int color);

#endif
