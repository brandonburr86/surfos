/*
SurfOS Desktop: Image Viewer, Hex and Sketch's Pictures, for the Other Files
--------------------
File: gui_media.h Date: 10/17/26
--------------------
What the desktop's other files call of track MEDIA (docs/superpowers/plans/2026-10-17-gui-suite.md):
Files opens a picture in the Image Viewer and any other file in Hex; Sketch's File menu, keys and
buttons (its code in shell/guicore.c) save the canvas as a BMP and open one; and the BMP reader
and writer they share (shell/guibmp.c). Everything here is called from the model's handlers,
never while drawing.
11/05/26 (track img): the Image Viewer is the program wview (user/wview/); guiview.c keeps its entry.
*/
#ifndef _SHELL_GUI_MEDIA_H
#define _SHELL_GUI_MEDIA_H
#include "guipriv.h"

/**** the Image Viewer (guiview.c) ****/
void viewer_show(struct gui_state *g, const char *path);   /* wview started on the file, at the viewer's place */
                                                           /* (what is no file is said, and starts nothing) */
bool viewer_takes(const char *name);                       /* a name it opens: *.bmp .png .gif .jpg .jpeg, any case */

/**** Hex (guihex.c) ****/
void hex_show(struct gui_state *g, const char *path);      /* its window opens on the file's first bytes */

/**** Sketch's pictures (its code in guicore.c, its files in guibmp.c) ****/
#define SKETCH_PAINTS 6
extern const u32 sketch_paints[SKETCH_PAINTS];             /* the canvas holds indexes into these */
/* its File menu's items and its buttons: numbers of gui_act's beyond the suite's dialogs */
#define ACT_SKETCH_SAVE (ACT_APP_DIALOG + 32 * (GUI_WINDOWS - GUI_FIRST_APP))
#define ACT_SKETCH_OPEN (ACT_SKETCH_SAVE + 1)
bool sketch_can_open(const struct gui_state *g);
void sketch_save_as(struct gui_state *g);                  /* the Save as chooser, for a 24-bit BMP */
void sketch_open(struct gui_state *g);                     /* the Open chooser */
/* the answers to Sketch's dialogs, which come through the Image Viewer's answer hook (gui_app_act(GUI_VIEWER, n)) */
#define VIEW_SKETCH_SAVE 20
#define VIEW_SKETCH_REPLACE 21
#define VIEW_SKETCH_OPEN 22
void sketch_answer(struct gui_state *g, int act, int button, const char *text, const char *target);
int sketch_save(struct gui_state *g, const char *path);    /* 0, or -errno after saying why */
int sketch_load(struct gui_state *g, const char *path);

/**** BMP files (guibmp.c) ****/
#define BMP_HEAD 2048                   /* what is read of a file for its headers and palette */
/* the headers in head (the file's first n bytes) checked against size, the file's length: 0, with what they
   say in b and the palette (256 entries, 0x00RRGGBB, black beyond the file's) filled; or -EINVAL with why */
int bmp_parse(const u8 *head, u32 n, u32 size, struct gui_bmp *b, u32 *palette, char *why, size_t len);
/* the same for the file at path, read with gui_fs's read_at into buf (BMP_HEAD bytes); or -errno, with why */
int bmp_open(struct gui_state *g, const char *path, struct gui_bmp *b, u32 *palette, u8 *buf, char *why, size_t len);
/* the colour (0x00RRGGBB) of pixel x of one of the file's rows */
u32 bmp_pixel(const struct gui_bmp *b, const u32 *palette, const u8 *row, u32 x);
/* the size a picture of w x h is kept at in at most max pixels: itself, or smaller with its proportions */
void bmp_reduce(u32 w, u32 h, u32 max, u32 *dw, u32 *dh);
/* the 54 bytes of headers of a 24-bit bottom-up BMP of w x h pixels into out; its rows' length, padded (the
   rows follow the headers, the bottom one first, each pixel B, G, R) */
u32 bmp_header24(u8 *out, u32 w, u32 h);
#endif
