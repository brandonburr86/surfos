/*
SurfOS Desktop: Image Viewer and Hex
--------------------
File: gui_media.h Date: 10/17/26
--------------------
Their state in gui_state (shell/guiview.c, shell/guihex.c), and Sketch's BMP files
(shell/guibmp.c). Included by sys/gui.h; owned by track MEDIA of
docs/superpowers/plans/2026-10-17-gui-suite.md. gui_state is kcalloc'd once, so every buffer
here has a fixed size: the Image Viewer keeps the picture it shows decoded, at most
GUI_VIEW_PIXELS pixels of 3 bytes (3 MB); a bigger picture is reduced while it is read (nearest
neighbour), so that any BMP of up to GUI_VIEW_SIDE x GUI_VIEW_SIDE pixels can be shown. Hex keeps
only the page it shows and the piece of a search, so a file of any size (offsets are u32: up to
4 GB) is read with gui_fs's read_at.
*/
#ifndef _SYS_GUI_MEDIA_H
#define _SYS_GUI_MEDIA_H

#define GUI_VIEW_SIDE 16384             /* the widest and the tallest BMP that is read */
#define GUI_VIEW_PIXELS (1024 * 1024)   /* the pixels kept of a picture (R, G, B: 3 MB) */
#define GUI_VIEW_CHUNK (64 * 1024)      /* one read of rows: a row of GUI_VIEW_SIDE 32-bit pixels fits */
/* Sketch's canvas as a 24-bit BMP: the headers, then its rows, each padded to 4 bytes */
#define GUI_SKETCH_FILE (54 + (GUI_CANVAS_W * 3 + 3) / 4 * 4 * GUI_CANVAS_H)
#define GUI_HEX_PAGE 4096               /* what Hex reads of the view: more rows than any window shows */
#define GUI_HEX_PIECE (64 * 1024)       /* one read of a search or a copy */
#define GUI_HEX_PATTERN 128             /* the longest search, in bytes */

/* what a BMP file's headers say, checked against the file's length (guibmp.c: bmp_parse) */
struct gui_bmp {
    u32 width, height, bpp;             /* pixels; bits per pixel: 1, 4, 8, 24 or 32 */
    bool topdown;                       /* the file's first row is the picture's top one (a negative height) */
    u32 offset, stride, colors;         /* where the pixels start; a row's bytes, padded; the palette's entries */
    u32 size;                           /* the file's length */
};

/* Sketch's Save as BMP and Open BMP (guibmp.c; Sketch's own code is in guicore.c) */
struct gui_sketchbmp {
    char path[GUI_PATH_MAX];            /* the file Sketch last saved to or opened, "" */
    bool saved;                         /* path is one Sketch wrote: saving there again does not ask */
    char status[GUI_STATUS];            /* what Sketch's window says under the canvas, "" for its hint */
    u32 palette[256];
    u8 file[GUI_SKETCH_FILE];           /* the file being written; or, opening, a row read and the new canvas */
};

struct gui_viewer {
    char path[GUI_PATH_MAX];            /* the picture shown, or being read, "" for none */
    char status[GUI_STATUS];            /* the status bar's message, "" for the picture's size and zoom */
    struct gui_bmp bmp;                 /* the file's layout */
    u32 w, h;                           /* the picture's own size */
    u32 dw, dh;                         /* the size it is kept at: smaller when it has more than GUI_VIEW_PIXELS */
    bool ready, loading, fit, dragging; /* fit: Fit (else zoom is the view) */
    u32 zoom;                           /* 16.16 fixed point: 65536 is the actual size */
    int pan_x, pan_y;                   /* the shown picture's pixels scrolled off to the left and the top */
    int press_x, press_y, press_pan_x, press_pan_y;   /* where a drag began */
    u32 click_ms;                       /* the last press in the picture, for a double-click: when and where */
    int click_x, click_y;
    u32 next;                           /* reading: the rows read so far, in the file's order */
    u32 palette[256];
    u8 chunk[GUI_VIEW_CHUNK];
    u8 pixels[GUI_VIEW_PIXELS * 3];     /* dw x dh, the top row first, each pixel R, G, B */
    struct gui_sketchbmp sketch;
};

struct gui_hex {
    char path[GUI_PATH_MAX];            /* the file shown, "" for none */
    u32 size, top;                      /* its size (as the last read found it); the first byte shown (a row's) */
    char status[GUI_STATUS];            /* the status bar's message */
    u32 cursor, anchor;                 /* the selection: anchor to cursor, both included */
    bool selecting, thumb;              /* the mouse is selecting; it holds the scroll bar's thumb */
    int thumb_at;                       /* where in the thumb it holds it */
    bool page_ok, page_bad;             /* page holds what it says; reading at bad_top failed (not again until the view moves) */
    u32 page_off, page_len, bad_top;
    u8 page[GUI_HEX_PAGE];
    /* Find: from the cursor on, a piece at a time in the tick */
    char find[GUI_PATH_MAX];            /* what was typed */
    u8 pattern[GUI_HEX_PATTERN];
    u8 fail[GUI_HEX_PATTERN];           /* Knuth-Morris-Pratt: the longest proper prefix that is also a suffix */
    u32 plen;
    bool searching, found;
    u32 from, scan, matched, found_at;  /* where it began; the next byte to read; the bytes matched so far; the last match */
    u8 piece[GUI_HEX_PIECE];
};
#endif
