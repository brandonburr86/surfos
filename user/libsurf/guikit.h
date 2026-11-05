/*
SurfOS user library: libgui's side of the drawing kit
--------------------
File: guikit.h  Date: 10/25/26
--------------------
The drawing kit (sys/guidraw.h) over a window's image, in plain types: guikit.c sees the
kernel's headers, gui.c the library's, and this is all they share (milestone M12, stage C5 of
docs/superpowers/plans/2026-10-25-app-windows.md). A kit is guikit_size() bytes that gui.c
allocates; guikit_setup() aims it at w x h pixels (0x00RRGGBB, rows of w), and every drawing is
clipped to them.
11/05/26 (the desktop applications, track kit): a view, an origin and a clip, which every drawing
goes through: guikit_setup() makes it the whole image with its origin at 0, 0, and the toolkit
(gk.h) narrows it to a widget, or to an area a program draws itself. One of the console's
characters by its UTF-8 bytes, and text cut to a number of cells.
*/
#ifndef _SURF_GUIKIT_H
#define _SURF_GUIKIT_H

unsigned guikit_size(void);
void guikit_setup(void *kit, unsigned *pixels, int w, int h);
void guikit_rect(void *kit, int x, int y, int w, int h, unsigned color);
void guikit_frame(void *kit, int x, int y, int w, int h, unsigned color);
void guikit_line(void *kit, int x0, int y0, int x1, int y1, unsigned color);
int guikit_text(void *kit, int x, int y, const char *utf8, unsigned color);    /* x after it */
int guikit_text_scaled(void *kit, int x, int y, const char *utf8, unsigned color, int scale);   /* 10/26/26 */
int guikit_cells(const char *utf8);                                            /* characters it shows */
void guikit_glyph(void *kit, int x, int y, unsigned code, unsigned color, int scale);  /* 10/26/26: code page 437 */
void guikit_blit(void *kit, int x, int y, const unsigned *pixels, int w, int h, int stride);
void guikit_button(void *kit, int x, int y, int w, int h, const char *label, unsigned bg, unsigned fg);
/* 11/05/26: the view (origin ox, oy in the image; clip l, t, r, b in the image, r and b past its last pixel), the
   character at s (its code, and its bytes in *len, of avail), and at most cells characters, a longer text's last
   cell a dot */
void guikit_get_view(const void *kit, int *ox, int *oy, int *l, int *t, int *r, int *b);
void guikit_set_view(void *kit, int ox, int oy, int l, int t, int r, int b);
int guikit_char(const char *s, int avail, int *len);
int guikit_text_cut(void *kit, int x, int y, const char *utf8, int cells, unsigned color);

#endif
