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
*/
#ifndef _SURF_GUIKIT_H
#define _SURF_GUIKIT_H

unsigned guikit_size(void);
void guikit_setup(void *kit, unsigned *pixels, int w, int h);
void guikit_rect(void *kit, int x, int y, int w, int h, unsigned color);
void guikit_frame(void *kit, int x, int y, int w, int h, unsigned color);
void guikit_line(void *kit, int x0, int y0, int x1, int y1, unsigned color);
int guikit_text(void *kit, int x, int y, const char *utf8, unsigned color);    /* x after it */
int guikit_cells(const char *utf8);                                            /* characters it shows */
void guikit_blit(void *kit, int x, int y, const unsigned *pixels, int w, int h, int stride);
void guikit_button(void *kit, int x, int y, int w, int h, const char *label, unsigned bg, unsigned fg);

#endif
