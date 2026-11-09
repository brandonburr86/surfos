/*
SurfOS window toolkit (gk, part of libgui in libsurf)
----------------------
File: gk.h      Date: 11/05/26
----------------------
Widgets in a program's window (the desktop applications, track kit:
docs/superpowers/plans/2026-11-05-desktop-apps.md), drawn as the desktop draws its own windows
(gui.h's drawing kit) and laid out in boxes, so that a window keeps its shape at any size.

A program opens a window with gk_open(), builds a tree of widgets under gk_root(ui), and then loops
on gk_next(), which paints what changed and presents only that (damage tracking), waits for the
window's events, its timers and the descriptors it watches, gives each event to the widget it
belongs to, and returns what the program must hear: a button clicked, a value changed, a key no
widget took, a modal box answered, a timer, the close button. A program changes widgets between
the calls (gk_set_text(), gk_set_value(), ...), and the next gk_next() shows the change.

    struct gk *ui = gk_open("Example", 400, 240, WIN_RESIZABLE);
    struct gk_widget *row = gk_hbox(gk_root(ui), 8), *name;
    struct gk_event ev;
    gk_label(row, "Name:");
    name = gk_field(row, ID_NAME, 64, 0);
    gk_button(row, "Greet", ID_GREET);
    while(gk_next(ui, &ev, -1) > 0) {
        if(ev.type == GK_EV_CLOSE) break;
        if(ev.type == GK_EV_CLICK && ev.id == ID_GREET) gk_message(ui, 0, "Hello", gk_text(name), "");
    }
    gk_close(ui);

Layout. gk_vbox() stacks its children top to bottom, gk_hbox() left to right, gap pixels apart,
within its padding (gk_pad()). Each child has a natural size (its own, or more by gk_size()); what
is left over goes to the children that grow (gk_grow(), weights) along the box, and across the box a
child that grows fills it, another keeps its natural size where gk_align() puts it (an hbox's
children are centred in its height, a vbox's at its left). gk_spacer() takes room, gk_separator()
draws a line, gk_pages() shows one child at a time. Hidden widgets (gk_show()) take no room.

Areas. gk_area() is a rectangle a program draws itself, inside the layout like any widget: its
draw function is called with the window's drawing (gui.h's gw_* calls) at the area's top left
(0, 0) and clipped to it, whenever any of it must be painted again (gk_redraw(), gk_damage()). It
gets the pointer (GK_EV_POINTER, its own coordinates; buttons held only after a press over it,
which keeps the pointer its own until every button is up), the wheel and, if it takes the focus,
the keys (GK_EV_KEY) and the focus (GK_EV_FOCUS). gk_area_extent() gives it scroll bars, which the
toolkit draws and moves (with the wheel too), in the program's own units (rows, columns, pixels).

Keys. The focus moves with Tab and Shift+Tab (WIN_KEY_BACKTAB) through the widgets that take it, in
the order of the tree, and a click gives a widget the focus; Ctrl+Tab (WIN_KEY_TAB with
WIN_MOD_CTRL, Ctrl+Shift+Tab back) moves it from any widget, one that takes Tab too, unless a menu
item has it for its shortcut. A key the focused widget does not use is a menu item's shortcut
(GK_EV_MENU), or comes back as GK_EV_KEY with w NULL, the window's. Keys and their modifiers are as
surfos/win.h says: Ctrl and a letter is its control byte with WIN_MOD_CTRL (Ctrl+S 19, Ctrl+I 9),
Tab, Enter, Esc and Backspace are 9, 13, 27 and 127 and with Ctrl WIN_KEY_TAB .. WIN_KEY_BACKSPACE;
a program tests the modifiers' bits (ev.mods & WIN_MOD_CTRL, & WIN_MOD_REPEAT for a key held down),
never mods whole. Fields and lists type and jump only for a key without Ctrl or Alt. A key held
down: movement keys repeat (the arrows, Home and End, Page Up and Down move a list, a slider, a
radio group, tabs, a field's or a text's cursor at each repeat), and activation keys act once a
press (Space and Enter on a button, a check box or a radio group; a box's keys; a menu item's
shortcut, but GK_ITEM_REPEAT's). All text is UTF-8, shown with the console's characters (code
page 437), 8 x 16 pixels each.

Testing: tests/host/gk_fake.h is a desktop for a program's window on the host (the Makefile's
GK_HOST_FLAGS and GK_HOST_OBJ), and user/wkit/ with tests/host/test_wkit.c is the pattern.

Memory: every widget is allocated; when an allocation fails the call returns NULL, a call given a
NULL parent does nothing and returns NULL, and gk_ok() says 0 from then on, so a program builds its
tree and asks once. gk_close() frees everything. Nothing here waits but gk_next(), and nothing
calls the program but an area's draw function, from gk_next() or gk_flush().
*/
#ifndef _GK_H
#define _GK_H

#include <gui.h>

struct gk;
struct gk_widget;

/* sizes, in logical pixels, as the desktop's windows have them */
#define GK_LINE_H    22                 /* a label, a field, a check box: one line */
#define GK_BUTTON_H  24
#define GK_ROW_H     18                 /* a list's row */
#define GK_TABS_H    30                 /* a tab strip: the desktop's toolbar and the line under it */
#define GK_STATUS_H  22
#define GK_SCROLL_W  12

/* the colours the widgets are drawn in: the desktop's theme (WIN_THEME, by its roles WIN_COLOR_*), the ocean's
   (gui.h's GW_*) from a desktop that does not say it; gk_next() follows a new theme (GK_EV_THEME) */
struct gk_theme {
    unsigned paper;                     /* the window's background (WIN_COLOR_WINDOW) */
    unsigned ink, muted;                /* text, and quieter text */
    unsigned teal, on_teal;             /* what is chosen or lit (the accent), and text on it */
    unsigned foam, coral;               /* light text on dark; what is wrong or late */
    unsigned shell, band, line;         /* buttons; bars and headers; thin lines */
    unsigned sheet, pick;               /* fields' and lists' insides; what is selected there */
    unsigned frame, shadow;             /* a box's frame; its shadow */
    unsigned desktop;                   /* the desktop behind the windows */
    unsigned grey;                      /* greyed text: what cannot be used now (WIN_COLOR_DISABLED) */
    int which;                          /* WIN_THEME_OCEAN or WIN_THEME_DUSK */
};

/* events (gk_next()) */
struct gk_event {
    int type;                           /* GK_EV_* */
    int id;                             /* the widget's id, or the timer's, the watch's, the box's */
    struct gk_widget *w;                /* the widget it is about; NULL: the window's own */
    int value;                          /* as each type says */
    int x, y;                           /* the pointer in the widget's coordinates; a new size; scroll positions */
    unsigned buttons, mods;             /* the pointer's buttons held (WIN_BUTTON_*); a key's modifiers (WIN_MOD_*) */
    int clicks;                         /* a press of the left button: 1, 2 for a double-click's second, 3 */
    const char *text;                   /* a box's answer, a chosen path, else "": valid until the next gk_next() */
};
#define GK_EV_CLICK     1               /* a button: clicked, or Space or Enter while it had the focus */
#define GK_EV_CHANGE    2               /* a person changed a value: value is the new one (a field's: its bytes) */
#define GK_EV_ACTIVATE  3               /* Enter in a field (value: its bytes); a list's row double-clicked or Enter */
#define GK_EV_KEY       4               /* value: the key (a byte, or WIN_KEY_*), mods; w the area, or NULL */
#define GK_EV_POINTER   5               /* over an area, or anywhere while a button pressed in it is held: value the
                                           buttons pressed now (0: a move or a release), clicks; outside its view
                                           once: the pointer left it */
#define GK_EV_WHEEL     6               /* value: rows, + down; to an area that does not scroll by it, or with Ctrl
                                           (mods), which is the program's (a zoom) */
#define GK_EV_FOCUS     7               /* value 1: the area has the keys now, 0: it has not */
#define GK_EV_SCROLL    8               /* an area's view moved: x, y its scroll positions */
#define GK_EV_TIMER     9               /* id: the timer's */
#define GK_EV_FD        10              /* id: the watch's; value: poll()'s revents */
#define GK_EV_ANSWER    11              /* a modal box: value 0 its first button (OK, Yes), 1 its second, -1 Cancel
                                           or Esc; text: what was typed in gk_ask()'s */
#define GK_EV_CLOSE     12              /* the window's close button */
#define GK_EV_RESIZE    13              /* the window's new size, x by y, laid out already */
#define GK_EV_TAB_CLOSE 14              /* a closable tab's x: value its index */
#define GK_EV_MENU      15              /* a menu item chosen, in the desktop's bar or by its shortcut: id the item's,
                                           value its menu's number (a submenu's item: the menu it is in) */
#define GK_EV_CHOSEN    16              /* a chooser answered (gk_choose()): id the request's, value 1 with text the
                                           path, 0 cancelled */
#define GK_EV_THEME     17              /* the desktop's theme changed: value WIN_THEME_*, gk_theme() its colours (the
                                           window is painted again: an area's draw reads them) */
#define GK_EV_UNCOPIED  18              /* 11/09/26: the clipboard refused a field's or a text's copy (Ctrl+C) or cut
                                           (Ctrl+X, which then deletes nothing): w the widget, value the errno (EINVAL:
                                           more than WIN_CLIP_MAX bytes; EACCES: the window has not the focus) */

/* flags */
#define GK_PASSWORD     0x01            /* a field: dots for its characters, nothing copied, its text wiped once gone */
#define GK_FOCUSABLE    0x02            /* an area: it takes the focus and the keys */
#define GK_TAKES_TAB    0x04            /* an area: Tab and Shift+Tab are its keys too */
#define GK_OWN_WHEEL    0x08            /* an area: the wheel comes to it (GK_EV_WHEEL), never scrolls it */
#define GK_CLOSABLE     0x10            /* tabs: each has an x, which says GK_EV_TAB_CLOSE */

#define GK_INK          0xFF000000u     /* gk_label_color(): the theme's ink */

/* alignment across a box */
#define GK_START        0
#define GK_CENTER       1
#define GK_END          2
#define GK_FILL         3

/**** the window ****/

/* a window of w x h (its content) as the desktop allows, WIN_RESIZABLE or 0; NULL with errno set (ENODEV: this
   program's terminal is no desktop's). The limits: min_w..max_w, min_h..max_h (0: none) */
struct gk *gk_open(const char *title, int w, int h, unsigned flags);
struct gk *gk_open_limits(const char *title, int w, int h, int min_w, int min_h, int max_w, int max_h, unsigned flags);
void gk_close(struct gk *ui);
struct gw *gk_gw(struct gk *ui);                /* the window: its title, gw_alert(), drawing in an area */
struct gk_widget *gk_root(struct gk *ui);       /* the window's content: a vbox */
int gk_ok(const struct gk *ui);                 /* 0 once an allocation failed */
const struct gk_theme *gk_theme(const struct gk *ui);
/* paint what changed and present it, wait at most ms (< 0: as long as it takes) for what the program must hear:
   1 with it in *ev, 0 when nothing came in time, -1 when the window has gone (errno EPIPE) or the wait failed (errno;
   EINTR: a kill or Ctrl+C). With -1 it never says 0. With a time (0 too: a game's frame), the window's events are
   read before 0 is said, a burst the toolkit takes itself as well (11/05/26: one read of 16 before) */
int gk_next(struct gk *ui, struct gk_event *ev, int ms);
int gk_flush(struct gk *ui);                    /* paint and present what changed now: 0, or -1 (errno EPIPE) */

/**** timers and descriptors ****/

#define GK_TIMERS 16
#define GK_WATCHES 8
int gk_timer(struct gk *ui, int id, int ms, int repeat);   /* GK_EV_TIMER after ms (again every ms): 0, -1 full */
void gk_timer_stop(struct gk *ui, int id);
int gk_watch(struct gk *ui, int fd, short events, int id);   /* (11/09/26: a watch ends by itself once it was told
                                                               POLLHUP, POLLERR or POLLNVAL: read what is left then) */ /* GK_EV_FD while poll() finds fd ready: 0, -1 full */
void gk_unwatch(struct gk *ui, int fd);

/**** the clipboard ****/

/* len bytes of text for the desktop's clipboard, while the window has the focus: gw_clip()'s 0, or -1 with errno */
int gk_copy(struct gk *ui, const char *text, unsigned len);
/* the clipboard's text (WIN_PASTE), while the window has the focus: into buf, at most size - 1 bytes and a NUL, cut at
   a character's start (its bytes); or all of it, allocated with its NUL (free() it; *len its bytes). -1 or NULL with
   errno: EACCES without the focus, ENOSYS or EINVAL from a desktop that cannot be read */
int gk_paste(struct gk *ui, char *buf, unsigned size);
char *gk_paste_all(struct gk *ui, int *len);

/**** menus in the desktop's bar ****/

/* A window's menus show in the desktop's bar while it has the focus (WIN_MENU), and their shortcuts work in the
   window itself, so that a desktop that does not show them still has every command by its keys. Each item says
   GK_EV_MENU, by a click in the bar or by its shortcut: a key the focused widget did not take (an area takes a key
   that types a character, and its shortcuts come first for any other: Ctrl or Alt and a key, a function key, Esc).
   A shortcut is a key, with GK_CTRL, GK_SHIFT and GK_ALT: GK_CTRL | 'S', GK_CTRL | GK_SHIFT | 'Z', WIN_KEY_F5,
   WIN_KEY_DELETE, GK_CTRL | WIN_KEY_TAB (a letter in capitals; a character that Shift makes, '+' say, with or without
   GK_SHIFT: the same); its text in the bar is made of it ("Ctrl+Shift+Z"). A shortcut acts once a press: the
   keyboard's repeats of its key held down (WIN_MOD_REPEAT) do nothing, unless its item has GK_ITEM_REPEAT, for what
   holding the key naturally does again (Undo, Redo, Zoom in and out). The toolkit sends the menus whole again
   at the next flush when anything in them changed.
   gk_menu(): a menu at the end of the bar, its number; -1 when there is no room (WIN_MENUS menus, their titles in
   WIN_MENU_BAR cells) or the title is one of the desktop's own (SurfOS, Apps, Window, or ≋ in it). A menu titled
   Help is the program's part of the desktop's Help menu (WIN_MENU_HELP: WIN_MENU_HELP_ITEMS items, not in the bar;
   an item that reads as one of the desktop's own Help items, About SurfOS or Keyboard and mouse, is refused).
   gk_submenu(): an item at the end of a menu that opens a submenu, one level deep (WIN_MENU_SUBMENU), the submenu's
   number for gk_menu_item(); a submenu with no items is not shown, and gk_menu_enable() of the item's id greys it and
   all in it. gk_menu_item(): an item at the end of a menu or a submenu (WIN_MENU_ITEMS of them and lines), -1 when
   there is no room. A label is cut to WIN_MENU_LABEL bytes, and a byte in it that is no character the desktop draws,
   or a control, is shown as '?'. What has no room is left out, and the rest go on working. */
#define GK_SHIFT        (WIN_MOD_SHIFT << 16)
#define GK_ALT          (WIN_MOD_ALT << 16)
#define GK_CTRL         (WIN_MOD_CTRL << 16)
#define GK_ITEM_OFF     0x01            /* greyed: it cannot be chosen, and its shortcut does nothing */
#define GK_ITEM_CHECK   0x02            /* a check mark, put and taken away by gk_menu_check() */
#define GK_ITEM_RADIO   0x04            /* one of a group: a dot, put by gk_menu_check() (the program takes the others') */
#define GK_ITEM_ON      0x08            /* with CHECK or RADIO: marked from the start */
#define GK_ITEM_REPEAT  0x10            /* its shortcut held down acts again at each of the keyboard's repeats */
int gk_menu(struct gk *ui, const char *title);
int gk_submenu(struct gk *ui, int menu, int id, const char *label, unsigned flags);   /* flags: GK_ITEM_OFF */
int gk_menu_item(struct gk *ui, int menu, int id, const char *label, unsigned shortcut, unsigned flags);
int gk_menu_line(struct gk *ui, int menu);
void gk_menu_enable(struct gk *ui, int id, int on);      /* every item of that id */
void gk_menu_check(struct gk *ui, int id, int on);
int gk_menus_shown(const struct gk *ui);                 /* 1: the desktop's bar shows them; 0: their shortcuts only */

/**** the desktop's choosers ****/

/* The desktop's Open, Save as or Choose folder chooser for the window (WIN_DIALOG), while it has the focus: kind
   WIN_DLG_OPEN, WIN_DLG_SAVE or WIN_DLG_FOLDER, starting in dir (absolute; "" or NULL: the desktop's choice), with name
   offered (Save as), for types ("png bmp": extensions without their dots; "" or NULL: every file). Its answer is
   GK_EV_CHOSEN with id. A desktop without choosers gets a line to type the path in instead, over the window (a modal
   box), answered the same way. Only in answer to a person's own action: while the program handles a key, a press, a
   click, a menu item, a change or a box's answer that gk_next() gave it (a chooser never comes up by itself, from a
   timer say; the desktop wants a key, a press or a menu choice since the window's last request). 0, or -1 with
   errno: EINVAL for a request that is none (the desktop's rules: types of letters and digits, "=Makefile" for a whole
   name), EBUSY while one waits for its answer, EACCES when it is not answering a person or the window has not the
   focus. (When the window's unread events leave the desktop no room for the answer, EAGAIN, the toolkit keeps the
   request and asks again once it has read them: 0 then, and the answer comes, cancelled if the desktop refuses it
   by then.) */
int gk_choose(struct gk *ui, int id, int kind, const char *dir, const char *name, const char *types);

/**** any widget ****/

int gk_id(const struct gk_widget *w);
void gk_set_data(struct gk_widget *w, void *data);
void *gk_data(const struct gk_widget *w);
void gk_bounds(const struct gk_widget *w, int *x, int *y, int *width, int *height);   /* in the window, as laid out */
void gk_size(struct gk_widget *w, int min_w, int min_h);   /* its natural size at least this (0: its own) */
void gk_grow(struct gk_widget *w, int gx, int gy);         /* weights of what is left over; across a box: fill */
void gk_align(struct gk_widget *w, int how);               /* GK_START, GK_CENTER, GK_END, GK_FILL across its box */
void gk_show(struct gk_widget *w, int on);
void gk_enable(struct gk_widget *w, int on);
int gk_shown(const struct gk_widget *w);
int gk_enabled(const struct gk_widget *w);
void gk_focus(struct gk_widget *w);                        /* the keys go to it (one that takes them) */
struct gk_widget *gk_focused(const struct gk *ui);
void gk_remove(struct gk_widget *w);                       /* it and what is in it go */
void gk_redraw(struct gk_widget *w);                       /* painted again at the next gk_next() */
/* the text of a label, button, check box, field, status bar (its left part), tab strip (the page shown's label);
   "" for none. gk_set_text() for a field puts the cursor at its end, and says whether the widget holds the text as
   given (11/09/26): 0, or 1 when it holds it changed to fit (a field's or a text's max reached, the text cut at a
   character's start; a control character as a space, a CR LF as one line end: compare gk_text()), or -1 when it
   could not take it (no memory: gk_ok() says 0) */
const char *gk_text(const struct gk_widget *w);
int gk_set_text(struct gk_widget *w, const char *text);
/* the value of a check box (0, 1), radio group, list (the row selected, -1), tabs, pages (the one shown), scroll
   bar (its first unit shown); setting it is no person's change: no GK_EV_CHANGE */
int gk_value(const struct gk_widget *w);
void gk_set_value(struct gk_widget *w, int value);

/**** boxes ****/

struct gk_widget *gk_vbox(struct gk_widget *parent, int gap);
struct gk_widget *gk_hbox(struct gk_widget *parent, int gap);
void gk_pad(struct gk_widget *box, int pad);               /* room inside its edges */
void gk_gap(struct gk_widget *box, int gap);               /* room between its children */
struct gk_widget *gk_pages(struct gk_widget *parent);      /* its children one at a time: gk_set_value() */
struct gk_widget *gk_spacer(struct gk_widget *parent);     /* empty, and it grows */
struct gk_widget *gk_separator(struct gk_widget *parent);  /* a thin line across its box */

/**** widgets ****/

struct gk_widget *gk_label(struct gk_widget *parent, const char *text);     /* lines apart at \n */
void gk_label_color(struct gk_widget *label, unsigned color);              /* 0x00RRGGBB; GK_INK again: its own */
struct gk_widget *gk_button(struct gk_widget *parent, const char *label, int id);
struct gk_widget *gk_check(struct gk_widget *parent, const char *label, int id, int on);
struct gk_widget *gk_switch(struct gk_widget *parent, const char *label, int id, int on);   /* a check box drawn as a
                                                                                               switch */
struct gk_widget *gk_radio(struct gk_widget *parent, int id, int across);   /* options down (across 0) or across */
int gk_radio_add(struct gk_widget *radio, const char *label);               /* its index, -1 */

/* a line of text of at most max bytes (UTF-8): the cursor and a selection by the keys (Shift with the arrows, Home and
   End; Ctrl with the arrows a word, with Backspace and Delete a word deleted) and the mouse (a drag, Shift and a
   click, a double-click takes a word, a third click all), Ctrl+A, Ctrl+C, Ctrl+X, Ctrl+V; Enter says GK_EV_ACTIVATE,
   every change GK_EV_CHANGE. flags: GK_PASSWORD */
struct gk_widget *gk_field(struct gk_widget *parent, int id, int max, unsigned flags);
void gk_field_hint(struct gk_widget *field, const char *hint);    /* shown, muted, while it is empty */
void gk_field_select(struct gk_widget *field, int from, int to);  /* bytes; to -1: the end */
/* history (the input line of a chat): Up and Down bring back the last n lines remembered */
int gk_field_history(struct gk_widget *field, int n);
void gk_field_remember(struct gk_widget *field);    /* its text, as the newest line (never a password field's) */
void gk_field_wipe(struct gk_widget *field);        /* its text gone, the bytes it was in zeroed */

/* a list of lines with a scroll bar: one selected (gk_value()), by the keys, the mouse and the wheel; Enter or a
   double-click says GK_EV_ACTIVATE. A line's tabs start its columns: gk_list_columns() gives the n columns after the
   first their x from where the first starts, each cut where the next begins, the last taking the rest of the line */
struct gk_widget *gk_list(struct gk_widget *parent, int id);
int gk_list_add(struct gk_widget *list, const char *text);         /* its index, -1 */
int gk_list_insert(struct gk_widget *list, int at, const char *text);
int gk_list_set(struct gk_widget *list, int i, const char *text);  /* 0, -1 */
void gk_list_remove(struct gk_widget *list, int i);
void gk_list_clear(struct gk_widget *list);
int gk_list_count(const struct gk_widget *list);
const char *gk_list_text(const struct gk_widget *list, int i);     /* NULL past the end */
void gk_list_columns(struct gk_widget *list, int n, const int *x);
void gk_list_reveal(struct gk_widget *list, int i);               /* scrolled so that row i shows */

/* a scroll bar on its own: total units, shown of them at a time, the first shown (gk_value()) */
struct gk_widget *gk_scrollbar(struct gk_widget *parent, int id, int across);
void gk_scroll_set(struct gk_widget *bar, int total, int shown, int first);

/* tabs: a strip of labels over pages, one shown (gk_value()); gk_tab() adds one and gives its page, a vbox to fill.
   Left and Right change the page while the strip has the focus. flags: GK_CLOSABLE */
struct gk_widget *gk_tabs(struct gk_widget *parent, int id, unsigned flags);
struct gk_widget *gk_tab(struct gk_widget *tabs, const char *label);
void gk_tab_label(struct gk_widget *tabs, int i, const char *label);
void gk_tab_remove(struct gk_widget *tabs, int i);
int gk_tab_count(const struct gk_widget *tabs);

/* a status bar: its text at the left (gk_set_text()), and a muted one at the right */
struct gk_widget *gk_status(struct gk_widget *parent);
void gk_status_right(struct gk_widget *status, const char *text);

/**** areas a program draws ****/

/* the view an area is drawn in: w x h pixels (without its scroll bars), its scroll positions in its units, and the
   rectangle to be painted now (cx, cy, cw, ch, in the view; drawing outside it is clipped away) */
struct gk_view { int w, h, sx, sy, cx, cy, cw, ch; };
typedef void gk_draw_fn(struct gk_widget *area, struct gw *win, const struct gk_view *v, void *data);
/* flags: GK_FOCUSABLE, GK_TAKES_TAB, GK_OWN_WHEEL */
struct gk_widget *gk_area(struct gk_widget *parent, int id, unsigned flags, gk_draw_fn *draw, void *data);
/* scroll bars: total_x units of unit_x pixels across, total_y of unit_y down (a total 0: none that way); the toolkit
   shows a bar while the units do not fit, keeps the positions within them and moves them by the bar and the wheel */
void gk_area_extent(struct gk_widget *area, int total_x, int unit_x, int total_y, int unit_y);
void gk_area_view(const struct gk_widget *area, struct gk_view *v);   /* as draw would get it now (cx.. the view) */
void gk_scroll_to(struct gk_widget *area, int sx, int sy);           /* kept within the extent; painted again */
/* units ux, uy (and uw x uh of them from there) in view: the least scrolling that shows them (a cell, a cursor) */
void gk_area_reveal(struct gk_widget *area, int ux, int uy, int uw, int uh);
void gk_damage(struct gk_widget *w, int x, int y, int width, int height);   /* a part of it, in its own coordinates */

/**** modal boxes in the window ****/

/* one at a time over the window's content, taking every key and click: a message (OK), a question with up to three
   buttons (yes, no, cancel from left to right: NULL for none), a line to type (OK and Cancel; GK_PASSWORD). Each is
   answered once, GK_EV_ANSWER with its id: a box opened over another answers the other -1 first. A question's
   Enter answers its first button, or with gk_confirm_default() the one def says (0 yes, 1 no, 2 cancel: a
   destructive question's safe answer); Left and Right move between them and stop at the ends. A key held down
   (WIN_MOD_REPEAT) answers no box: Enter held on a button does not answer the question it opens */
void gk_message(struct gk *ui, int id, const char *title, const char *line1, const char *line2);
void gk_confirm(struct gk *ui, int id, const char *title, const char *line1, const char *line2,
                const char *yes, const char *no, const char *cancel);
void gk_confirm_default(struct gk *ui, int id, const char *title, const char *line1, const char *line2,
                        const char *yes, const char *no, const char *cancel, int def);
void gk_ask(struct gk *ui, int id, const char *title, const char *prompt, const char *text, unsigned flags);
int gk_asking(const struct gk *ui);             /* the id of the box open, 0 for none (or a box of id 0) */
int gk_box_open(const struct gk *ui);           /* 1 while one is */
void gk_dismiss(struct gk *ui);                 /* the box open answers -1 and goes */

/**** stage 2 (11/05/26): a text of several lines, progress bars, sliders, palettes ****/

#define GK_READONLY     0x20            /* a text of several lines: shown, selected and copied, never changed */

/* a small text of several lines (UTF-8, lines at \n), edited as Notepad edits: no wrapping, the view following the
   cursor, a tab to the next multiple of 8 columns, the keys and the mouse of a field and Up, Down, Page Up, Page Down,
   Ctrl+Home and Ctrl+End; Enter starts a line. At most max bytes. flags: GK_READONLY, GK_TAKES_TAB (a tab typed) */
struct gk_widget *gk_text_area(struct gk_widget *parent, int id, int max, unsigned flags);
void gk_text_select(struct gk_widget *text, int from, int to);    /* bytes; to -1: the end */
void gk_text_cursor(const struct gk_widget *text, int *from, int *to);   /* the selection, from <= to */

/* a progress bar: gk_set_value() from 0 to max, its text in the middle (gk_set_text()) */
struct gk_widget *gk_progress(struct gk_widget *parent, int max);

/* a slider: a whole number from min to max by step (gk_value()), by the arrows (a step), Page Up and Down (ten), Home,
   End, a click or a drag on its track, the wheel */
struct gk_widget *gk_slider(struct gk_widget *parent, int id, int min, int max, int step);

/* a palette: cells of cell_w x cell_h, cols to a row, each a colour or a picture (w x h pixels of 0x00RRGGBB, those
   with the top byte set see-through), one chosen (gk_value(): -1 for none) by a click or the arrows */
struct gk_widget *gk_palette(struct gk_widget *parent, int id, int cols, int cell_w, int cell_h);
int gk_palette_color(struct gk_widget *palette, unsigned color);                     /* its index, -1 */
int gk_palette_icon(struct gk_widget *palette, const unsigned *pixels, int w, int h);  /* its index, -1 */
int gk_palette_set_color(struct gk_widget *palette, int i, unsigned color);          /* 0, -1 (a picture's) */
unsigned gk_palette_get_color(const struct gk_widget *palette, int i);

#endif
