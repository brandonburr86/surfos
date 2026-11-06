/*
SurfOS user program: wpaint
--------------------
File: window.c  Date: 11/05/26
--------------------
Paint's window (window.h): the tool palette at the left, the canvas in an area of the toolkit's
with its scroll bars (by pixels of the zoomed canvas), the colours under them, and a status bar
with the last message or the tool's hint at the left and where the pointer is, the selection, the
canvas's size and the zoom at the right.

The canvas: the model (paint.h) renders the part the toolkit asks to paint, a strip at a time into
a buffer that is blitted (gw_blit()), and says after each event what changed, which the toolkit
alone presents (gk_damage()). The left button draws in the first colour and the right in the
second; the middle one drags the view; the wheel scrolls (across with Shift) and zooms with Ctrl,
about the pointer. Keys while the canvas has them: the tools' letters (P B E L R O F T K S), [ and ]
the size, + and - the zoom, X the colours swapped, the arrows the selection (or the view), Esc,
and whatever text is typed with the text tool.

Menus in the desktop's bar (and their shortcuts in the window in any case): File, Edit, Image (the
canvas's size, Clear, the zoom), Tools (the tools, filled shapes, the brush's and the text's sizes)
and Colours, and Paint's lines in the desktop's Help. Open and Save as are the desktop's choosers
(gk_choose(): only in answer to the person's own key, click or menu choice), which ask before a
file is replaced; a desktop without them gives a line to type the path in. New, Open and Close ask
first about changes not saved (Save, Don't save, Cancel), and again, just before the canvas goes,
when it changed after that answer; Save of a picture with no name, or one whose kind Paint does
not write (GIF, JPEG), is Save as. A picture opened with transparency or more than 8 bits a
channel says so, and a save that would replace its file without them asks first (Save as..., the
first button and Enter's, Save, or Cancel).

While one of Paint's boxes is open, the menus in the desktop's bar do nothing (the box takes the
keys and clicks already), and a drag on the canvas ends where it was when a box opens or the
window loses the focus: the button let go meanwhile is never seen.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "window.h"

static const char *const tool_keys = "PBELROFTKS";      /* paint.h's tools, in order */

static const char *base(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* what the last action said, in the status line until the next */
static void note(struct pwin *w, const char *fmt, const char *arg) {
    snprintf(w->p.msg, sizeof(w->p.msg), fmt, arg);
}

/**** the view and the canvas's area ****/

/* the model's view as the toolkit has the area: its size without the bars, and its scroll */
static void sync_view(struct pwin *w) {
    struct gk_view v;
    gk_area_view(w->canvas, &v);
    if(v.w != w->p.vw || v.h != w->p.vh) paint_view_size(&w->p, v.w, v.h);
    if(v.sx != w->p.sx || v.sy != w->p.sy) paint_scroll_to(&w->p, v.sx, v.sy);
}

/* the toolkit's area as the model has it: the zoomed canvas's size for the bars, then the model's scroll */
static void push_view(struct pwin *w) {
    struct gk_view v;
    int cw, ch;
    paint_zoomed(&w->p, &cw, &ch);
    gk_area_extent(w->canvas, cw, 1, ch, 1);
    gk_area_view(w->canvas, &v);
    if(v.w != w->p.vw || v.h != w->p.vh) paint_view_size(&w->p, v.w, v.h);    /* a bar came or went */
    gk_scroll_to(w->canvas, w->p.sx, w->p.sy);
    sync_view(w);
    w->cw = cw;
    w->ch = ch;
    gk_redraw(w->canvas);
}

static void draw_canvas(struct gk_widget *area, struct gw *win, const struct gk_view *v, void *data) {
    struct pwin *w = data;
    int y, n;
    (void)area;
    if(v->w != w->p.vw || v->h != w->p.vh) paint_view_size(&w->p, v->w, v->h);
    if(v->sx != w->p.sx || v->sy != w->p.sy) paint_scroll_to(&w->p, v->sx, v->sy);
    if(v->cw <= 0 || v->cw > PAINT_VIEW_MAX) return;
    for(y = v->cy; y < v->cy + v->ch; y += n) {
        n = v->cy + v->ch - y < PW_STRIP ? v->cy + v->ch - y : PW_STRIP;
        paint_render(&w->p, w->strip, v->cw, v->cx, y, v->cw, n);
        gw_blit(win, v->cx, y, w->strip, v->cw, n, v->cw);
    }
}

static void zoom_to(struct pwin *w, int zoom, int ax, int ay) {
    sync_view(w);
    if(paint_zoom_to(&w->p, zoom, ax, ay)) push_view(w);
}

static void zoom_by(struct pwin *w, int steps, int ax, int ay) {
    zoom_to(w, w->p.zoom + steps, ax, ay);
}

/* the view's middle, the point a zoom from the menu or the keys keeps */
static void middle(const struct pwin *w, int *x, int *y) {
    *x = w->p.vw / 2;
    *y = w->p.vh / 2;
}

/* a picture opened: shown whole if it is larger than the view, at 100% if not */
static void fit(struct pwin *w) {
    sync_view(w);
    w->p.zoom = PAINT_ZOOM_1;
    paint_zoom_fit(&w->p);
    push_view(w);
}

static void scroll_by(struct pwin *w, int dx, int dy) {
    sync_view(w);
    gk_scroll_to(w->canvas, w->p.sx + dx, w->p.sy + dy);
    sync_view(w);
}

/**** files ****/

/* the folder a chooser starts in: the picture's, which is absolute; "" (the desktop's choice) for none */
static void dir_of(const char *path, char *out, size_t n) {
    const char *s = strrchr(path, '/');
    size_t len = s ? (size_t)(s - path) : 0;
    if(s && !len) len = 1;                              /* a file in / */
    if(len >= n || path[0] != '/') len = 0;
    memcpy(out, path, len);
    out[len] = 0;
}

/* a path given at the start, or typed, made absolute from the working folder (a chooser's folder must be): 0, or -1
   when it does not fit (it is kept as it was) */
static int absolute(const char *path, char *out, size_t n) {
    char cwd[PW_PATH];
    if(path[0] == '/' || !getcwd(cwd, sizeof(cwd))) return snprintf(out, n, "%s", path) < (int)n ? 0 : -1;
    while(path[0] == '.' && path[1] == '/') path += 2;
    return snprintf(out, n, "%s%s%s", cwd, cwd[strlen(cwd) - 1] == '/' ? "" : "/", path) < (int)n ? 0 : -1;
}

static void choose_open(struct pwin *w) {
    char dir[PW_PATH];
    dir_of(w->path, dir, sizeof(dir));
    if(gk_choose(w->ui, CHOOSE_OPEN, WIN_DLG_OPEN, dir, "", "png bmp gif jpg jpeg") < 0)
        note(w, "No chooser: %s", errno == EACCES ? "the window must have the focus" : strerror(errno));
}

/* the desktop's Save as chooser: 0 asked, -1 not (the status line says why) */
static int save_as(struct pwin *w) {
    char dir[PW_PATH], name[PW_PATH];
    dir_of(w->path, dir, sizeof(dir));
    if(w->path[0]) pf_save_name(base(w->path), name, sizeof(name));
    else snprintf(name, sizeof(name), "picture.png");
    if(gk_choose(w->ui, CHOOSE_SAVE, WIN_DLG_SAVE, dir, name, "png bmp") == 0) return 0;
    note(w, "No chooser: %s", errno == EACCES ? "the window must have the focus" : strerror(errno));
    return -1;
}

/* the canvas into path: 0, or -1 with a message box saying why. The file is Paint's from then on: nothing of it lost */
static int save_to(struct pwin *w, const char *path) {
    int r = pf_save(&w->p, path, w->why, sizeof(w->why), w->kept, sizeof(w->kept));
    if(r == 0) {
        if(path != w->path) snprintf(w->path, sizeof(w->path), "%s", path);
        w->lost = 0;
        note(w, "Saved %s", base(path));
        return 0;
    }
    gk_message(w->ui, BOX_MESSAGE, "Not saved", base(path), w->why);
    return -1;
}

/* what saving over the picture's file does to what the canvas has not of it (PF_LOST_*), for a sentence */
static const char *lost_how(int lost) {
    if(lost == PF_LOST_ALPHA) return "without its transparency";
    return lost == PF_LOST_DEPTH ? "at 8 bits a channel" : "on white, at 8 bits a channel";
}

/* the canvas into path; but first a question when that replaces the picture's own file without what the canvas has
   not of it: 0 saved, 2 asked (BOX_LOSSY's answer goes on), -1 not saved (a box says why) */
static int save_over(struct pwin *w, const char *path) {
    char line[PF_WHY];
    if(!w->lost || strcmp(path, w->path)) return save_to(w, path);
    snprintf(w->saving, sizeof(w->saving), "%s", path);
    snprintf(line, sizeof(line), "Saving replaces %s %s.", base(path), lost_how(w->lost));
    gk_confirm(w->ui, BOX_LOSSY, "Replace the picture's file?", line, "Save as\xe2\x80\xa6 keeps the file as it is.",
               "Save as\xe2\x80\xa6", "Save", "Cancel");
    return 2;
}

/* Save: Save as when the picture has no name, or one of a kind Paint does not write. 0 saved, 1 a chooser asked,
   2 a question asked first, -1 none of them */
static int save(struct pwin *w) {
    int r;
    if(!w->path[0] || !pf_kind(w->path)) return save_as(w) == 0 ? 1 : -1;
    r = save_over(w, w->path);
    return r == 2 ? 2 : r == 0 ? 0 : -1;
}

static void open_file(struct pwin *w, const char *given) {
    char line[PF_WHY], path[PW_PATH];
    int lost;
    if(absolute(given, path, sizeof(path)) < 0) {
        snprintf(line, sizeof(line), "%s", base(given));
        gk_message(w->ui, BOX_MESSAGE, "Cannot open the picture", line, "Its path is too long.");
        return;
    }
    if((lost = pf_open(&w->p, path, w->why, sizeof(w->why))) < 0) {
        snprintf(line, sizeof(line), "%s", base(path));
        gk_message(w->ui, BOX_MESSAGE, "Cannot open the picture", line, w->why);
        return;
    }
    snprintf(w->path, sizeof(w->path), "%s", path);
    w->lost = lost;
    fit(w);
    if(!lost) snprintf(w->p.msg, sizeof(w->p.msg), "Opened %s, %d x %d", base(path), w->p.w, w->p.h);
    else snprintf(w->p.msg, sizeof(w->p.msg), "Opened %s, %d x %d, %s", base(path), w->p.w, w->p.h,
                  lost == PF_LOST_DEPTH ? "kept at 8 bits a channel" : lost == PF_LOST_ALPHA ?
                  "its transparency on white" : "on white, at 8 bits a channel");
}

/* "640 x 400", "640x400", "640, 400": 0 and the size, or -1 */
static int parse_size(const char *s, int *pw, int *ph) {
    long v[2];
    int k;
    for(k = 0; k < 2; k++) {
        while(*s == ' ') s++;
        if(k) {
            if(*s == 'x' || *s == 'X' || *s == ',' || *s == '*') s++;
            while(*s == ' ') s++;
        }
        if(*s < '0' || *s > '9') return -1;
        for(v[k] = 0; *s >= '0' && *s <= '9'; s++)
            if((v[k] = v[k] * 10 + (*s - '0')) > 100000) return -1;
    }
    while(*s == ' ') s++;
    if(*s) return -1;
    *pw = (int)v[0];
    *ph = (int)v[1];
    return 0;
}

/**** what waits on the unsaved changes ****/

/* the canvas changed since the person let New or Open replace it (it is asked again then) */
static int changed_since(const struct pwin *w) {
    return paint_modified(&w->p) && w->p.state != w->agreed;
}

static void go_on(struct pwin *w, int what) {
    char size[32];
    w->then = PW_THEN_NOTHING;
    w->agreed = w->p.state;                             /* saved, not changed, or let go: as it is now */
    switch(what) {
    case PW_THEN_NEW:
        snprintf(size, sizeof(size), "%d x %d", PAINT_NEW_W, PAINT_NEW_H);
        gk_ask(w->ui, BOX_NEW, "New picture", "Its size, width x height:", size, 0);
        break;
    case PW_THEN_OPEN: choose_open(w); break;
    case PW_THEN_OPEN_PATH: open_file(w, w->pending); break;
    case PW_THEN_CLOSE: w->closing = 1; break;
    }
}

/* New, Open or Close: first the question about changes not saved, if there are any */
static void then(struct pwin *w, int what) {
    char line[PF_WHY];
    if(!paint_modified(&w->p)) {
        go_on(w, what);
        return;
    }
    w->then = what;
    snprintf(line, sizeof(line), "%s has changes that are not saved.", w->path[0] ? base(w->path) : "The picture");
    gk_confirm(w->ui, BOX_UNSAVED, "Save the changes?", line, "", "Save", "Don't save", "Cancel");
}

static void saved_then(struct pwin *w) {
    int what = w->then;
    w->save_then = 0;
    if(what == PW_THEN_OPEN) {                          /* a chooser answers a person's action: a box first */
        w->then = PW_THEN_NOTHING;
        w->agreed = w->p.state;
        gk_message(w->ui, BOX_THEN_OPEN, "Saved", "The picture is saved.", "Now choose the picture to open.");
        return;
    }
    go_on(w, what);
}

/* nothing waits any more */
static void give_up(struct pwin *w) {
    w->then = PW_THEN_NOTHING;
    w->save_then = 0;
}

static void answer(struct pwin *w, const struct gk_event *ev) {
    unsigned rgb;
    int nw, nh, r;
    switch(ev->id) {
    case BOX_UNSAVED:
        if(ev->value == 0) {                            /* Save, then go on */
            r = save(w);
            if(r == 1) w->save_then = 1;                /* when the chooser's picture is saved */
            else if(r == 0) go_on(w, w->then);
            else if(r != 2) give_up(w);                 /* 2: BOX_LOSSY's answer goes on */
        } else if(ev->value == 1) go_on(w, w->then);    /* Don't save */
        else give_up(w);
        break;
    case BOX_LOSSY:
        if(ev->value == 1) {                            /* Save: the file replaced, without what Paint has not of it */
            if(save_to(w, w->saving) < 0) give_up(w);
            else if(w->save_then) saved_then(w);
            else go_on(w, w->then);
        } else if(ev->value == 0) {                     /* Save as...: another file, the picture's own as it is */
            if(save_as(w) < 0) give_up(w);
            else if(w->then) w->save_then = 1;
        } else give_up(w);
        break;
    case BOX_THEN_OPEN:
        if(ev->value == 0) choose_open(w);              /* OK; not Esc, nor another box put in its place */
        break;
    case BOX_NEW:
    case BOX_RESIZE:
        if(ev->value != 0) break;
        if(parse_size(ev->text, &nw, &nh) < 0) {
            gk_message(w->ui, BOX_MESSAGE, ev->id == BOX_NEW ? "No new picture" : "Not resized", ev->text,
                       "Type its width and height, 640 x 400 say.");
            break;
        }
        if(ev->id == BOX_NEW && changed_since(w)) {     /* changed after the question: it is asked again */
            then(w, PW_THEN_NEW);
            break;
        }
        r = ev->id == BOX_NEW ? paint_new(&w->p, nw, nh) : paint_resize(&w->p, nw, nh);
        if(r < 0) {
            gk_message(w->ui, BOX_MESSAGE, ev->id == BOX_NEW ? "No new picture" : "Not resized", w->p.msg, "");
            w->p.msg[0] = 0;
            break;
        }
        if(ev->id == BOX_NEW) {                         /* a new canvas at 100%, from its top left */
            w->path[0] = 0;
            w->lost = 0;
            zoom_to(w, PAINT_ZOOM_1, 0, 0);
        }
        push_view(w);
        break;
    case BOX_COLOUR:
        if(ev->value != 0) break;
        if(paint_parse_colour(ev->text, &rgb) < 0) {
            gk_message(w->ui, BOX_MESSAGE, "Not a colour", ev->text, "Type #RRGGBB, or red, green, blue from 0 to 255.");
            break;
        }
        paint_add_custom(&w->p, rgb);
        break;
    }
}

static void chosen(struct pwin *w, const struct gk_event *ev) {
    int r;
    if(!ev->value) {                                    /* cancelled: nothing waits on it any more */
        give_up(w);
        return;
    }
    if(ev->id == CHOOSE_OPEN) {
        if(changed_since(w)) {                          /* changed while the chooser was up: asked again first */
            snprintf(w->pending, sizeof(w->pending), "%s", ev->text);
            then(w, PW_THEN_OPEN_PATH);
        } else open_file(w, ev->text);
    } else if(ev->id == CHOOSE_SAVE) {
        r = save_over(w, ev->text);
        if(r == 0 && w->save_then) saved_then(w);
        else if(r < 0 && w->save_then) give_up(w);
    }
}

/**** the menus ****/

static void edit_colour(struct pwin *w) {
    char code[16];
    paint_colour_text(w->p.fg, code, sizeof(code));
    gk_ask(w->ui, BOX_COLOUR, "Colour", "#RRGGBB, or red, green, blue (0 to 255):", code, 0);
}

static void resize_canvas(struct pwin *w) {
    char size[32];
    snprintf(size, sizeof(size), "%d x %d", w->p.w, w->p.h);
    gk_ask(w->ui, BOX_RESIZE, "Canvas size", "Width x height, at most 4096 each way:", size, 0);
}

/* the desktop's clipboard into the text being typed: its lines as lines, as much as the text takes. Control
   characters are left out (C0 but the line ends, DEL, and C1, U+0080 to U+009F, which UTF-8 writes C2 80 to C2 9F) */
static void paste_text(struct pwin *w) {
    int len, i;
    unsigned char c;
    char *s = gk_paste_all(w->ui, &len);
    if(!s) {
        note(w, "Nothing pasted: %s", errno == EACCES ? "the window must have the focus" : "no text to paste");
        return;
    }
    for(i = 0; i < len; i++) {
        c = (unsigned char)s[i];
        if(c == '\r' && i + 1 < len && s[i + 1] == '\n') continue;
        if(c == 0xC2 && i + 1 < len && (unsigned char)s[i + 1] >= 0x80 && (unsigned char)s[i + 1] <= 0x9F) {
            i++;
            continue;
        }
        if((c < 0x20 && c != '\r' && c != '\n') || c == 0x7F) continue;
        paint_key(&w->p, c == '\n' ? '\r' : c, 0);
    }
    free(s);
}

static void menu(struct pwin *w, int id) {
    int x, y;
    middle(w, &x, &y);
    switch(id) {
    case MENU_NEW: then(w, PW_THEN_NEW); break;
    case MENU_OPEN: then(w, PW_THEN_OPEN); break;
    case MENU_SAVE: save(w); break;
    case MENU_SAVE_AS: save_as(w); break;
    case MENU_CLOSE: then(w, PW_THEN_CLOSE); break;
    case MENU_UNDO: paint_undo(&w->p); break;
    case MENU_REDO: paint_redo(&w->p); break;
    case MENU_CUT: paint_cut(&w->p); break;
    case MENU_COPY: paint_copy(&w->p); break;
    case MENU_PASTE:
        if(paint_typing(&w->p)) paste_text(w);
        else paint_paste(&w->p);
        break;
    case MENU_DELETE: paint_delete(&w->p); break;
    case MENU_SELECT_ALL: paint_select_all(&w->p); break;
    case MENU_DESELECT: paint_cancel(&w->p); break;
    case MENU_RESIZE: resize_canvas(w); break;
    case MENU_CLEAR: paint_clear(&w->p); break;
    case MENU_ZOOM_IN: zoom_by(w, 1, x, y); break;
    case MENU_ZOOM_OUT: zoom_by(w, -1, x, y); break;
    case MENU_ACTUAL: zoom_to(w, PAINT_ZOOM_1, x, y); break;
    case MENU_FIT:
        sync_view(w);
        if(paint_zoom_fit(&w->p)) push_view(w);
        break;
    case MENU_FILLED: paint_set_filled(&w->p, !w->p.filled); break;
    case MENU_EDIT_COLOUR: edit_colour(w); break;
    case MENU_SWAP: paint_swap_colours(&w->p); break;
    case MENU_DEFAULT_COLOURS:
        paint_set_colour(&w->p, 0, 0x000000);
        paint_set_colour(&w->p, 1, PAINT_PAPER);
        break;
    case MENU_KEYS:
        gk_message(w->ui, BOX_KEYS, "Paint's keys", "P B E L R O F T K S: the tools; [ ]: the size",
                   "+ -: zoom; X: swap the colours; Esc: put down");
        break;
    case MENU_ABOUT:
        gk_message(w->ui, BOX_ABOUT, "About Paint", "Paint draws pictures, and opens and saves them",
                   "as PNG and BMP files (and opens GIF and JPEG).");
        break;
    default:
        if(id >= MENU_ZOOM && id < MENU_ZOOM + PAINT_ZOOMS) zoom_to(w, id - MENU_ZOOM, x, y);
        else if(id >= MENU_TOOL && id < MENU_TOOL + PAINT_TOOLS) paint_set_tool(&w->p, id - MENU_TOOL);
        else if(id >= MENU_SIZE && id < MENU_SIZE + PAINT_SIZES) paint_set_size(&w->p, id - MENU_SIZE);
        else if(id >= MENU_SCALE && id < MENU_SCALE + PAINT_TEXT_SCALE) paint_set_text_scale(&w->p, id - MENU_SCALE + 1);
        break;
    }
}

static void build_menus(struct pwin *w) {
    static const char *const tools[PAINT_TOOLS] = { "Pencil", "Brush", "Eraser", "Line", "Rectangle", "Ellipse",
                                                    "Fill", "Text", "Colour picker", "Select" };
    char label[16];
    int m, s, i;
    m = gk_menu(w->ui, "File");
    gk_menu_item(w->ui, m, MENU_NEW, "New\xe2\x80\xa6", GK_CTRL | 'N', 0);
    gk_menu_item(w->ui, m, MENU_OPEN, "Open\xe2\x80\xa6", GK_CTRL | 'O', 0);
    gk_menu_item(w->ui, m, MENU_SAVE, "Save", GK_CTRL | 'S', 0);
    gk_menu_item(w->ui, m, MENU_SAVE_AS, "Save as\xe2\x80\xa6", GK_CTRL | GK_SHIFT | 'S', 0);
    gk_menu_line(w->ui, m);
    gk_menu_item(w->ui, m, MENU_CLOSE, "Close", GK_CTRL | 'W', 0);
    m = gk_menu(w->ui, "Edit");
    gk_menu_item(w->ui, m, MENU_UNDO, "Undo", GK_CTRL | 'Z', 0);
    gk_menu_item(w->ui, m, MENU_REDO, "Redo", GK_CTRL | 'Y', 0);
    gk_menu_line(w->ui, m);
    gk_menu_item(w->ui, m, MENU_CUT, "Cut", GK_CTRL | 'X', 0);
    gk_menu_item(w->ui, m, MENU_COPY, "Copy", GK_CTRL | 'C', 0);
    gk_menu_item(w->ui, m, MENU_PASTE, "Paste", GK_CTRL | 'V', 0);
    gk_menu_item(w->ui, m, MENU_DELETE, "Delete", WIN_KEY_DELETE, 0);
    gk_menu_line(w->ui, m);
    gk_menu_item(w->ui, m, MENU_SELECT_ALL, "Select all", GK_CTRL | 'A', 0);
    gk_menu_item(w->ui, m, MENU_DESELECT, "Select none", 0, 0);
    m = gk_menu(w->ui, "Image");
    gk_menu_item(w->ui, m, MENU_RESIZE, "Canvas size\xe2\x80\xa6", GK_CTRL | 'E', 0);
    gk_menu_item(w->ui, m, MENU_CLEAR, "Clear", 0, 0);
    gk_menu_line(w->ui, m);
    gk_menu_item(w->ui, m, MENU_ZOOM_IN, "Zoom in", GK_CTRL | '+', 0);
    gk_menu_item(w->ui, m, MENU_ZOOM_OUT, "Zoom out", GK_CTRL | '-', 0);
    gk_menu_item(w->ui, m, MENU_ACTUAL, "Actual size", GK_CTRL | '0', 0);
    gk_menu_item(w->ui, m, MENU_FIT, "Fit", GK_CTRL | 'F', 0);
    s = gk_submenu(w->ui, m, MENU_ZOOMS, "Zoom", 0);
    for(i = 0; i < PAINT_ZOOMS; i++) {
        snprintf(label, sizeof(label), "%d%%", paint_zoom_percent(i));
        gk_menu_item(w->ui, s, MENU_ZOOM + i, label, 0, GK_ITEM_RADIO | (i == PAINT_ZOOM_1 ? GK_ITEM_ON : 0));
    }
    m = gk_menu(w->ui, "Tools");
    for(i = 0; i < PAINT_TOOLS; i++)
        gk_menu_item(w->ui, m, MENU_TOOL + i, tools[i], (unsigned)tool_keys[i],
                     GK_ITEM_RADIO | (i == w->p.tool ? GK_ITEM_ON : 0));
    gk_menu_line(w->ui, m);
    gk_menu_item(w->ui, m, MENU_FILLED, "Filled shapes", 0, GK_ITEM_CHECK);
    s = gk_submenu(w->ui, m, MENU_SIZES, "Brush size", 0);
    for(i = 0; i < PAINT_SIZES; i++) {
        snprintf(label, sizeof(label), "%d px", paint_sizes[i]);
        gk_menu_item(w->ui, s, MENU_SIZE + i, label, 0, GK_ITEM_RADIO | (i == w->p.size ? GK_ITEM_ON : 0));
    }
    s = gk_submenu(w->ui, m, MENU_SCALES, "Text size", 0);
    for(i = 0; i < PAINT_TEXT_SCALE; i++) {
        snprintf(label, sizeof(label), "%d times", i + 1);
        gk_menu_item(w->ui, s, MENU_SCALE + i, label, 0, GK_ITEM_RADIO | (i + 1 == w->p.text_scale ? GK_ITEM_ON : 0));
    }
    m = gk_menu(w->ui, "Colours");
    gk_menu_item(w->ui, m, MENU_EDIT_COLOUR, "Edit colour\xe2\x80\xa6", 0, 0);
    gk_menu_item(w->ui, m, MENU_SWAP, "Swap the colours", 'X', 0);
    gk_menu_item(w->ui, m, MENU_DEFAULT_COLOURS, "Black and white", 0, 0);
    m = gk_menu(w->ui, "Help");
    gk_menu_item(w->ui, m, MENU_KEYS, "Paint's keys", 0, 0);
    gk_menu_item(w->ui, m, MENU_ABOUT, "About Paint", 0, 0);
}

/**** the pointer, the wheel and the keys ****/

static int mods_of(unsigned mods) {
    return mods & WIN_MOD_SHIFT ? PAINT_SHIFT : 0;
}

/* past the view's edge while a button is held: the view follows, a little at a time */
static void follow(struct pwin *w, int x, int y) {
    int dx = x < 0 ? x : x >= w->p.vw ? x - w->p.vw + 1 : 0, dy = y < 0 ? y : y >= w->p.vh ? y - w->p.vh + 1 : 0;
    if(dx < -32) dx = -32;
    if(dx > 32) dx = 32;
    if(dy < -32) dy = -32;
    if(dy > 32) dy = 32;
    if(dx || dy) scroll_by(w, dx, dy);
}

static void canvas_pointer(struct pwin *w, const struct gk_event *ev) {
    unsigned now = ev->buttons, pressed = (unsigned)ev->value, mine;
    int cx, cy, mods = mods_of(ev->mods);
    sync_view(w);
    if(w->panning) {                                    /* the middle button drags the view */
        if(now & WIN_BUTTON_MIDDLE) {
            gk_scroll_to(w->canvas, w->pan_sx - (ev->x - w->pan_x), w->pan_sy - (ev->y - w->pan_y));
            sync_view(w);
        } else w->panning = 0;
        return;
    }
    if((pressed & WIN_BUTTON_MIDDLE) && !w->p.drag) {
        w->panning = 1;
        w->pan_x = ev->x;
        w->pan_y = ev->y;
        w->pan_sx = w->p.sx;
        w->pan_sy = w->p.sy;
        return;
    }
    if(w->p.drag) follow(w, ev->x, ev->y);
    paint_to_canvas(&w->p, ev->x, ev->y, &cx, &cy);
    if(!w->p.drag) {
        if(pressed & WIN_BUTTON_LEFT) paint_press(&w->p, cx, cy, PAINT_LEFT, mods);
        else if(pressed & WIN_BUTTON_RIGHT) paint_press(&w->p, cx, cy, PAINT_RIGHT, mods);
        else if(ev->x < 0 || ev->y < 0 || ev->x >= w->p.vw || ev->y >= w->p.vh) paint_leave(&w->p);
        else paint_move(&w->p, cx, cy, mods);
        return;
    }
    mine = w->p.drag == PAINT_LEFT ? WIN_BUTTON_LEFT : WIN_BUTTON_RIGHT;
    if(now & mine) paint_move(&w->p, cx, cy, mods);
    else paint_release(&w->p, cx, cy, mods);             /* the button that started the drag was let go */
}

static void tools_pointer(struct pwin *w, const struct gk_event *ev) {
    int value, hit;
    if(!(ev->value & WIN_BUTTON_LEFT)) return;
    hit = pw_tools_hit(&w->p, ev->x, ev->y, &value);
    if(hit == PW_HIT_TOOL) paint_set_tool(&w->p, value);
    else if(hit == PW_HIT_SIZE) paint_set_size(&w->p, value);
    else if(hit == PW_HIT_STYLE) paint_set_filled(&w->p, value);
    else if(hit == PW_HIT_SCALE) paint_set_text_scale(&w->p, value);
}

static void colours_pointer(struct pwin *w, const struct gk_event *ev) {
    int value, hit, second = (ev->value & WIN_BUTTON_RIGHT) != 0;
    if(!(ev->value & (WIN_BUTTON_LEFT | WIN_BUTTON_RIGHT))) return;
    hit = pw_colours_hit(&w->p, ev->x, ev->y, &value);
    if(hit == PW_HIT_COLOUR) paint_set_colour(&w->p, second, (unsigned)value);
    else if(hit == PW_HIT_CUSTOM) paint_set_colour(&w->p, second, w->p.custom[value]);
    else if(hit == PW_HIT_SWAP) paint_swap_colours(&w->p);
    else if(hit == PW_HIT_EDIT && !second) edit_colour(w);
}

static void canvas_wheel(struct pwin *w, const struct gk_event *ev) {
    sync_view(w);
    if(ev->mods & WIN_MOD_CTRL) zoom_by(w, ev->value < 0 ? 1 : -1, ev->x, ev->y);     /* away from the user: in */
    else if(ev->mods & WIN_MOD_SHIFT) scroll_by(w, ev->value * 16, 0);
    else scroll_by(w, 0, ev->value * 16);
}

static void key(struct pwin *w, const struct gk_event *ev) {
    unsigned k = (unsigned)ev->value;
    const char *t;
    int x, y, step = 32;
    if(paint_typing(&w->p) && paint_key(&w->p, (int)k, mods_of(ev->mods))) return;
    if(!(ev->mods & (WIN_MOD_CTRL | WIN_MOD_ALT)) && !paint_typing(&w->p) && k < 0x80) {
        middle(w, &x, &y);
        if(k >= 'a' && k <= 'z' && (t = strchr(tool_keys, (int)(k - 'a' + 'A')))) {
            paint_set_tool(&w->p, (int)(t - tool_keys));
            return;
        }
        switch(k) {
        case '[': paint_set_size(&w->p, w->p.size - 1); return;
        case ']': paint_set_size(&w->p, w->p.size + 1); return;
        case '+': case '=': zoom_by(w, 1, x, y); return;
        case '-': zoom_by(w, -1, x, y); return;
        case 'X': case 'x': paint_swap_colours(&w->p); return;
        }
    }
    if(paint_key(&w->p, (int)k, mods_of(ev->mods))) return;   /* the selection's keys, Esc */
    sync_view(w);
    switch(k) {                                         /* with nothing to move, the keys move the view */
    case WIN_KEY_LEFT: scroll_by(w, -step, 0); break;
    case WIN_KEY_RIGHT: scroll_by(w, step, 0); break;
    case WIN_KEY_UP: scroll_by(w, 0, -step); break;
    case WIN_KEY_DOWN: scroll_by(w, 0, step); break;
    case WIN_KEY_PGUP: scroll_by(w, 0, -(w->p.vh > 2 * step ? w->p.vh - step : step)); break;
    case WIN_KEY_PGDN: scroll_by(w, 0, w->p.vh > 2 * step ? w->p.vh - step : step); break;
    case WIN_KEY_HOME: scroll_by(w, -w->p.sx, -w->p.sy); break;
    case WIN_KEY_END: scroll_by(w, w->cw, w->ch); break;
    }
}

/**** after each event ****/

static void title(struct pwin *w) {
    char t[WIN_TITLE_MAX];
    snprintf(t, sizeof(t), "%s%s - Paint", paint_modified(&w->p) ? "*" : "", w->path[0] ? base(w->path) : "Untitled");
    if(strcmp(t, w->title)) {
        snprintf(w->title, sizeof(w->title), "%s", t);
        gw_title(gk_gw(w->ui), t);
    }
}

static void menus_state(struct pwin *w) {
    const struct paint *p = &w->p;
    int i, sel = paint_has_selection(p);
    gk_menu_enable(w->ui, MENU_UNDO, paint_can_undo(p));
    gk_menu_enable(w->ui, MENU_REDO, paint_can_redo(p));
    gk_menu_enable(w->ui, MENU_CUT, sel);
    gk_menu_enable(w->ui, MENU_COPY, sel);
    gk_menu_enable(w->ui, MENU_DELETE, sel);
    gk_menu_enable(w->ui, MENU_PASTE, paint_can_paste(p) || paint_typing(p));
    gk_menu_enable(w->ui, MENU_DESELECT, sel || paint_typing(p));
    gk_menu_enable(w->ui, MENU_ZOOM_IN, p->zoom < PAINT_ZOOMS - 1);
    gk_menu_enable(w->ui, MENU_ZOOM_OUT, p->zoom > 0);
    for(i = 0; i < PAINT_ZOOMS; i++) gk_menu_check(w->ui, MENU_ZOOM + i, i == p->zoom);
    for(i = 0; i < PAINT_TOOLS; i++) gk_menu_check(w->ui, MENU_TOOL + i, i == p->tool);
    gk_menu_check(w->ui, MENU_FILLED, p->filled);
    for(i = 0; i < PAINT_SIZES; i++) gk_menu_check(w->ui, MENU_SIZE + i, i == p->size);
    for(i = 0; i < PAINT_TEXT_SCALE; i++) gk_menu_check(w->ui, MENU_SCALE + i, i + 1 == p->text_scale);
}

/* the panels painted again when what they show changed */
static void panels(struct pwin *w) {
    struct pw_shown s;
    memset(&s, 0, sizeof(s));
    s.tool = w->p.tool;
    s.size = w->p.size;
    s.filled = w->p.filled;
    s.scale = w->p.text_scale;
    s.fg = w->p.fg;
    s.bg = w->p.bg;
    memcpy(s.custom, w->p.custom, sizeof(s.custom));
    if(s.tool != w->shown.tool || s.size != w->shown.size || s.filled != w->shown.filled || s.scale != w->shown.scale)
        gk_redraw(w->tools);
    if(s.fg != w->shown.fg || s.bg != w->shown.bg || memcmp(s.custom, w->shown.custom, sizeof(s.custom)))
        gk_redraw(w->colours);
    w->shown = s;
}

static void update(struct pwin *w) {
    char text[PAINT_MSG + 32];
    int x, y, cw, ch, vw, vh;
    paint_zoomed(&w->p, &cw, &ch);
    if(cw != w->cw || ch != w->ch) push_view(w);       /* a new size, an undone one, a picture opened */
    if(paint_damage(&w->p, &x, &y, &vw, &vh)) gk_damage(w->canvas, x, y, vw, vh);
    paint_drawn(&w->p);
    panels(w);
    paint_hint(&w->p, text, sizeof(text));
    gk_set_text(w->status, text);
    paint_where(&w->p, text, sizeof(text));
    gk_status_right(w->status, text);
    title(w);
    menus_state(w);
}

/* the canvas lost the keys (a box opened over it, or the window lost the focus): a drag ends where it was, since the
   button let go meanwhile will not be heard of */
static void let_go(struct pwin *w) {
    w->panning = 0;
    if(w->p.drag) paint_release(&w->p, w->p.bx, w->p.by, 0);
}

int pw_event(struct pwin *w, const struct gk_event *ev) {
    switch(ev->type) {
    case GK_EV_CLOSE: then(w, PW_THEN_CLOSE); break;
    case GK_EV_MENU:
        if(!gk_box_open(w->ui)) menu(w, ev->id);       /* the bar's, behind a box: nothing (its shortcuts are the box's) */
        break;
    case GK_EV_FOCUS:
        if(ev->w == w->canvas && !ev->value) let_go(w);
        break;
    case GK_EV_ANSWER: answer(w, ev); break;
    case GK_EV_CHOSEN: chosen(w, ev); break;
    case GK_EV_POINTER:
        if(ev->w == w->canvas) canvas_pointer(w, ev);
        else if(ev->w == w->tools) tools_pointer(w, ev);
        else if(ev->w == w->colours) colours_pointer(w, ev);
        break;
    case GK_EV_WHEEL:
        if(ev->w == w->canvas) canvas_wheel(w, ev);
        break;
    case GK_EV_KEY: key(w, ev); break;
    case GK_EV_SCROLL:
    case GK_EV_RESIZE: sync_view(w); break;
    case GK_EV_THEME:
        w->p.backdrop = gk_theme(w->ui)->shadow;
        gk_redraw(w->canvas);
        break;
    }
    if(w->closing) return 0;
    update(w);
    return 1;
}

/**** the window ****/

int pw_open(struct pwin *w, const char *file) {
    struct gk_widget *root, *body;
    int e;
    memset(w, 0, sizeof(*w));
    if(paint_init(&w->p, 0, 0) < 0) {
        errno = ENOMEM;
        return -1;
    }
    w->ui = gk_open_limits("Untitled - Paint", PW_W, PW_H, PW_MIN_W, PW_MIN_H, 0, 0, WIN_RESIZABLE);
    if(!w->ui) {
        e = errno;
        paint_free(&w->p);
        errno = e;
        return -1;
    }
    snprintf(w->title, sizeof(w->title), "Untitled - Paint");
    root = gk_root(w->ui);
    body = gk_hbox(root, 0);
    gk_grow(body, 1, 1);
    w->tools = gk_area(body, ID_TOOLS, 0, pw_draw_tools, w);
    gk_size(w->tools, PW_TOOLS_W, PW_TOOLS_H);
    gk_grow(w->tools, 0, 1);
    w->canvas = gk_area(body, ID_CANVAS, GK_FOCUSABLE | GK_OWN_WHEEL, draw_canvas, w);
    w->colours = gk_area(root, ID_COLOURS, 0, pw_draw_colours, w);
    gk_size(w->colours, 0, PW_COLOURS_H);
    gk_grow(w->colours, 1, 0);
    w->status = gk_status(root);
    build_menus(w);
    gk_focus(w->canvas);
    if(!gk_ok(w->ui)) {
        pw_close(w);
        errno = ENOMEM;
        return -1;
    }
    w->p.backdrop = gk_theme(w->ui)->shadow;
    w->shown.tool = -1;                                 /* the panels are painted as they first show anyway */
    push_view(w);
    if(file) {                                          /* laid out first: a picture larger than the view is fitted */
        gk_flush(w->ui);                                /* to the view's real size, not to nothing */
        open_file(w, file);
    }
    update(w);
    return 0;
}

void pw_close(struct pwin *w) {
    gk_close(w->ui);
    w->ui = NULL;
    paint_free(&w->p);
}
