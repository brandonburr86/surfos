/*
SurfOS user program: wview
--------------------
File: view.h    Date: 11/05/26
--------------------
The Image Viewer's model (view.c, folder.c): where the picture stands in the window and at what
zoom, the toolbar, what a key, a press, a drag or the wheel does, the window's title, the status bar
and the file's details, the folder's pictures in their order, and the picture painted into the room
at its zoom. main.c reads the files, decodes them (libsurf's image.h) and draws; everything here is
plain arithmetic on what it hands in, so that tests/host/test_wview.c runs it on the host as it is.

The desktop's Image Viewer (shell/guiview.c until 11/05/26) moved out of the kernel, as the Clock
and the Character Map did: its views, keys, toolbar and status bar, at the same places in a window
of its own, and what the kernel's could not do: PNG, GIF and JPEG besides BMP, the folder's next and
previous picture, the file's details, Copy path.

Coordinates are the window's content's, in logical pixels: the toolbar at the top, the room the
picture is shown in below it, the status bar at the bottom. A zoom is 16.16: VIEW_ONE is the actual
size.
*/
#ifndef _WVIEW_VIEW_H
#define _WVIEW_VIEW_H

#include <stddef.h>

/* the window: the desktop's Image Viewer's 560 x 400 with its frame, this its content; and its least */
#define VIEW_W 558
#define VIEW_H 371
#define VIEW_MIN_W 540                  /* the toolbar's span, as the desktop's viewer had it */
#define VIEW_MIN_H 160

#define VIEW_BAR_H 28                   /* the toolbar, then a line */
#define VIEW_STATUS_H 22                /* the status bar */
#define VIEW_ROOM_Y (VIEW_BAR_H + 3)    /* the room's top */

#define VIEW_ONE 65536                  /* the actual size, as a 16.16 zoom */
#define VIEW_ZOOM_MIN (VIEW_ONE / 64)
#define VIEW_ZOOM_MAX (VIEW_ONE * 16)
#define VIEW_PAN_STEP 32                /* the pixels an arrow moves a zoomed picture */
#define VIEW_DOUBLE_MS 500              /* a second press within this, about the same place: a double-click */

#define VIEW_STATUS 160
#define VIEW_PATH 512                   /* a path, with its NUL: longer ones are refused */
#define VIEW_NAME 256                   /* a name, with its NUL */

/* the keys that are no bytes (surfos/win.h's WIN_KEY_*) */
#define VIEW_KEY_UP 0x100
#define VIEW_KEY_DOWN 0x101
#define VIEW_KEY_LEFT 0x102
#define VIEW_KEY_RIGHT 0x103
#define VIEW_KEY_HOME 0x104
#define VIEW_KEY_END 0x105
#define VIEW_KEY_PGUP 0x106
#define VIEW_KEY_PGDN 0x107

/* what a key, a press or the toolbar asks of main.c besides drawing again */
enum {
    VIEW_NOTHING,
    VIEW_DRAW,                          /* draw again */
    VIEW_OPEN,                          /* the Open chooser */
    VIEW_NEXT,                          /* the folder's next picture */
    VIEW_PREVIOUS,                      /* its previous */
    VIEW_COPY,                          /* the path to the clipboard */
    VIEW_CLOSE                          /* the window closes */
};

/* the toolbar's buttons, left to right */
enum { VT_OPEN, VT_PREVIOUS, VT_NEXT, VT_FIT, VT_ACTUAL, VT_IN, VT_OUT, VT_DETAILS, VT_COPY, VT_COUNT };

/* the file shown, as main.c found it */
struct view_file {
    char path[VIEW_PATH];
    char kind[24];                      /* "PNG", "24-bit BMP", ...: by its first bytes */
    unsigned size;                      /* bytes */
    unsigned mtime;                     /* seconds since 1970, the PC's clock (local time); 0: not known */
    int w, h;                           /* the picture's size */
    int index, count;                   /* its place among the folder's pictures, from 1 (count 0: not known) */
};

struct view {
    int win_w, win_h;                   /* the window's content */
    int pw, ph;                         /* the picture shown, as decoded; 0: none */
    int fit;                            /* fitted to the room (the default) */
    unsigned zoom;                      /* when not fitted */
    int pan_x, pan_y;                   /* the shown picture's pixels left of and above the room, when it is larger */
    int dragging, press_x, press_y, press_pan_x, press_pan_y;
    unsigned click_ms;                  /* the last press in the room, for a double-click; 0: none */
    int click_x, click_y;
    int details;                        /* the file's details shown over the room */
    int pictures;                       /* the pictures of the folder N and P step through (0: none known) */
    unsigned now;                       /* milliseconds since boot, from main.c before each call */
    struct view_file file;              /* file.path[0] 0: none shown */
    char status[VIEW_STATUS];           /* a message in the status bar, until the next thing done ("" for none) */
};

/* the room the picture is shown in */
struct view_room { int x, y, w, h; };

void view_init(struct view *v, int win_w, int win_h);
void view_room(const struct view *v, struct view_room *r);
void view_resize(struct view *v, int win_w, int win_h);
/* a picture of w x h is shown now (pw 0: none): fitted, from its top left */
void view_picture(struct view *v, int w, int h);

/* the zoom shown (Fit's, or the one chosen), and where the picture stands: its top left (it may be off the room) and
   its size on the screen */
unsigned view_zoom(const struct view *v);
void view_place(const struct view *v, int *x, int *y, int *w, int *h);

int view_key(struct view *v, int key);          /* a key as WIN_EV_KEY gives it: VIEW_* */
int view_repeats(int key);                      /* does the key act again as the keyboard repeats it (held down) */
int view_press(struct view *v, int x, int y);   /* the left button pressed */
void view_drag(struct view *v, int x, int y);   /* the pointer with it held */
void view_release(struct view *v);
void view_wheel(struct view *v, int rows);      /* the wheel: rows, positive down */
int view_act(struct view *v, int tool);         /* a toolbar button: VIEW_* */
int view_tool_on(const struct view *v, int tool);   /* may it be used now */

/* the toolbar's buttons: where each is, and its label */
struct view_tool { int x, y, w, h; };
void view_tool(int tool, struct view_tool *t);
const char *view_tool_label(int tool);
int view_tool_at(int x, int y);                 /* the button at x, y, or -1 */

void view_title(const struct view *v, char *out, size_t n);
void view_status(const struct view *v, char *left, size_t ln, char *right, size_t rn);
/* the details panel's lines, VIEW_DETAILS of them at most, each up to VIEW_DETAIL bytes: how many */
#define VIEW_DETAILS 8
#define VIEW_DETAIL 96
int view_details(const struct view *v, char lines[VIEW_DETAILS][VIEW_DETAIL]);
const char *view_name(const char *path);        /* the path's last part */

/* The picture px (pw x ph, 0xTTRRGGBB as image.h has them) painted into dst (rows of stride pixels, 0x00RRGGBB) where
   it stands in the room, room pixels it does not cover left alone. Shown larger than it is, each pixel is the nearest;
   smaller, the average of those it stands for. A pixel partly there is blended over two greys in squares of 8, which
   show where a picture is transparent. Nothing is allocated. */
void view_paint(const struct view *v, const unsigned *px, unsigned *dst, int stride);

/* The folder's pictures (folder.c): names by their ending (.bmp .png .gif .jpg .jpeg, any case), in order by name
   with case not counted, at most FOLDER_MAX of them and FOLDER_BYTES of names; the rest are not counted. */
#define FOLDER_MAX 4096
#define FOLDER_BYTES (256 * 1024)
struct folder {
    char *names;                        /* FOLDER_BYTES, the caller's: each name with its NUL */
    unsigned at[FOLDER_MAX];            /* where each name starts */
    int count;
    unsigned used;                      /* bytes of names */
    int dropped;                        /* names past the limits */
};
int folder_is_picture(const char *name);
void folder_start(struct folder *f, char *names);
int folder_add(struct folder *f, const char *name);     /* 0, or -1 when it is not counted */
void folder_sort(struct folder *f);
const char *folder_name(const struct folder *f, int i);
int folder_find(const struct folder *f, const char *name);     /* its index, or -1 */
int folder_step(const struct folder *f, int i, int step);      /* the index step away, around the ends; -1 if none */

#endif
