/*
SurfOS Undertow's window: the browser
----------------------
File: browser.h  Date: 10/25/26
----------------------
What the window shows and what its keys and pointer do, with no window and no network: the program
(main.c) gives it events and paints what it draws, carries out the loads it asks for and gives it the pages
(or why they failed). So all of it runs in the host tests.

    +----------------------------------------------------------------------+
    | [<- Back] [Forward ->] [Reload] [ https://example.com/       ] [Go] |   toolbar, 30 + 1
    |----------------------------------------------------------------------|
    | the page, laid out at the window's width less the scroll bar     |##|
    |                                                                  |##|
    |----------------------------------------------------------------------|
    | the link under the pointer, Loading..., Done      HTTPS verified TLS |   status bar, 22
    +----------------------------------------------------------------------+

Keys (the desktop's: a byte as typed, or WB_KEY_* for those that are none, as surfos/win.h numbers them):
in the page, the arrows (32 pixels), Page Up and Page Down, Space, Home and End scroll, Backspace goes
back, Ctrl+L goes to the address field (all of it chosen) and Ctrl+R loads the page again; in the address
field, typing edits, Enter loads, Escape gives the page's address back, Tab returns to the page. The
wheel scrolls 16 pixels a row. A link is followed when the button is let go over the link it was pressed
on; its address shows in the status bar while the pointer is over it.

Pages come from http, https and file. A file page (file:///path, typed, or a link on a file page) is read
by the program; its links resolve against the host WB_FILE_HOST, which only a file page's links lead back
to files: an http(s) page never opens one. History: WB_HISTORY pages and WB_HISTORY_BYTES at most, each
with the place it was read at; back and forward load nothing.
*/

#ifndef WUNDERTOW_BROWSER_H
#define WUNDERTOW_BROWSER_H

#include <stddef.h>
#include "url.h"
#include "http.h"
#include "doc.h"
#include "layout.h"
#include "paint.h"

#define WB_HISTORY       16
#define WB_HISTORY_BYTES (8u << 20)
#define WB_TOOLBAR       31             /* the toolbar's 30 rows and the line under it */
#define WB_STATUS        22
#define WB_SCROLLBAR     12
#define WB_MIN_W         320
#define WB_MIN_H         160
#define WB_FILE_HOST     "local.file.invalid"   /* a file page's base: https://local.file.invalid/path */
#define WB_ADDR_MAX      (UT_URL_MAX + 16)

/* the keys that are no bytes: surfos/win.h's WIN_KEY_* */
#define WB_KEY_UP        0x100
#define WB_KEY_DOWN      0x101
#define WB_KEY_LEFT      0x102
#define WB_KEY_RIGHT     0x103
#define WB_KEY_HOME      0x104
#define WB_KEY_END       0x105
#define WB_KEY_PGUP      0x106
#define WB_KEY_PGDN      0x107
#define WB_KEY_DELETE    0x108

struct wb_page {
    struct wd_document doc;
    struct wl_layout lay;
    int lay_w;                          /* the width it is laid out at (0: not yet) */
    char url[WB_ADDR_MAX];              /* as the address field shows it: https://..., file:///... */
    int file;                           /* a file page (or the start page): its file links lead to files */
    int scroll;                         /* the place it is read at */
    char fragment[UT_URL_MAX + 1];      /* a fragment target to go to once it is laid out */
    int status, verified;
    char tls[16], reason[64];
    size_t bytes;
};

struct wb_request {                     /* a load the program is to carry out */
    char url[WB_ADDR_MAX];              /* https://..., http://..., file:///path */
    int reload;                         /* it takes the place of the page shown, at its place */
};

enum { WB_FOCUS_PAGE, WB_FOCUS_ADDR };

struct wb {
    struct wb_page *page[WB_HISTORY];
    struct wb_page *start;              /* the start page, shown with no page loaded */
    int npages, cur;
    int w, h;                           /* the window's content */
    char addr[WB_ADDR_MAX];             /* the address field */
    size_t addr_len, caret;
    int focus, chosen;                  /* chosen: all of the address is chosen (typing replaces it) */
    int hover, pressed;                 /* the link under the pointer, the link pressed (-1: none) */
    int button;                         /* the toolbar button pressed (0: none) */
    int drag, drag_y, drag_scroll;      /* the scroll bar's thumb is dragged: from where */
    unsigned int buttons;               /* the pointer's buttons, as last seen */
    char status[256];                   /* the status bar's text, safe */
    int loading;
    int want;                           /* a request waits for the program */
    struct wb_request req;
    struct wp_theme theme;
};

/* A browser in a window of w x h, with its start page; 0, or -1 without memory */
int wb_init(struct wb *b, int w, int h);
void wb_free(struct wb *b);

/* Events: each returns 1 when the window must be painted again */
int wb_key(struct wb *b, unsigned int key);
int wb_pointer(struct wb *b, int x, int y, unsigned int buttons);   /* buttons: 1 left, 2 right, 4 middle */
int wb_wheel(struct wb *b, int rows);                               /* rows down (negative: up) */
int wb_resize(struct wb *b, int w, int h);

/* An address as if typed and Enter pressed: a request, or the status says why not */
void wb_go(struct wb *b, const char *typed);

/* The program's side: the request that waits (1, and it no longer waits), then its page or why it failed */
int wb_take(struct wb *b, struct wb_request *out);
void wb_loaded(struct wb *b, struct wb_page *p, int reload);        /* the browser owns p now */
void wb_failed(struct wb *b, const char *url, const char *why);

/* A page from a response: file 1 for a file page (its response's url is https://WB_FILE_HOST/path); NULL with
   why (out of memory) */
struct wb_page *wb_page_new(const struct ut_response *r, int file, char *err, size_t cap);
void wb_page_free(struct wb_page *p);

/* An address the program loads: is it a file (1, with its path, %XX decoded, in path), or http(s) (0)? */
int wb_file_path(const char *url, char *path, size_t cap);

/* Everything the window shows, into a w x h image (rows of stride pixels) */
void wb_paint(struct wb *b, unsigned int *pixels, int stride);

/* The page shown (the start page when none is loaded) */
struct wb_page *wb_current(struct wb *b);

#endif
