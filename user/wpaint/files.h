/*
SurfOS user program: wpaint
--------------------
File: files.h   Date: 11/05/26
--------------------
Paint's pictures as files (files.c): BMP, PNG, GIF and JPEG opened as the canvas, BMP and PNG
saved, through libsurf's codecs (image.h). main.c asks the desktop's choosers for the names.
*/
#ifndef _WPAINT_FILES_H
#define _WPAINT_FILES_H

#include <stddef.h>
#include "paint.h"

#define PF_BMP      1                   /* what a name ending in .bmp is saved as: 24 bits */
#define PF_PNG      2                   /* .png: 8 bits a sample, RGB */
#define PF_FILE_MAX (72u << 20)         /* the largest file opened: 16,000,000 pixels are about 64 MB as BMP or PNG */
#define PF_WHY      160                 /* a message's room */
#define PF_KEPT     (-2)                /* pf_save(): the disk failed in the rename, which may have half happened */
#define PF_LOST_ALPHA 1                 /* pf_open(): the file has transparency, which Paint put on white */
#define PF_LOST_DEPTH 2                 /* and more than 8 bits a channel, which Paint keeps the high 8 of */

/* the kind a name saves as (its ending, in either case): PF_BMP, PF_PNG, or 0 for one Paint does not write */
int pf_kind(const char *path);

/* the name Save as offers for path: itself when Paint writes its kind, else with .png for its ending */
void pf_save_name(const char *path, char *out, size_t n);

/* the file's picture as the canvas, its transparency put on white: 0, or PF_LOST_* for what the file has that the
   canvas does not (saving over the file would lose it); -1 with why (a sentence) and the canvas as it was. The
   history starts again; the clipboard stays. For its moment it holds the file's bytes (PF_FILE_MAX at most) and
   the decoder's memory beside Paint's own; the file's go once the decoder is done with them */
int pf_open(struct paint *p, const char *path, char *why, size_t cap);

/* the canvas into path, as its name says, what floats and what is typed put down first: 0, and the canvas is saved
   (paint_saved()); -1 with why, and the file as it was; PF_KEPT with why when the disk failed in the rename, kept
   naming the new file when it is there (the file and it may share their space: the disk wants checking) */
int pf_save(struct paint *p, const char *path, char *why, size_t cap, char *kept, size_t kcap);

/* pixels as the codecs give them (0xTTRRGGBB, TT how transparent) as Paint's: each on white. 1 when one was not
   solid, else 0 */
int pf_flatten(unsigned *px, size_t n);

#endif
