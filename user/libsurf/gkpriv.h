/*
SurfOS window toolkit: what its files share
--------------------
File: gkpriv.h  Date: 11/05/26
--------------------
The toolkit's insides (gk.h is its outside): a widget and its class, the window's state, and the
helpers the widgets' files share with gkcore.c, which keeps the tree, lays it out, tracks what must
be painted again, moves the focus and turns the window's events into the program's (the desktop
applications, track kit: docs/superpowers/plans/2026-11-05-desktop-apps.md).

Coordinates are the window's: a widget's x, y, w, h are where it is in the window after the last
layout, and its class paints there, the drawing's view (gui.h) clipped to it and to what is being
painted. Painting never changes a widget; the event handlers change them and say what must be
painted again (gk_dirty(), gk_relayout()).
*/
#ifndef _SURF_GKPRIV_H
#define _SURF_GKPRIV_H

#include <gk.h>

#define GK_QUEUE 64                     /* events for the program, at most (one window event makes a few) */
#define GK_DAMAGE 16                    /* rectangles to paint again before they are merged into one */
#define GK_DOUBLE_MS 500                /* two clicks this close together are a double-click, as on the desktop */
#define GK_BATCHES 8                    /* gk_next() reads on while the window has events, at most these batches more
                                           (gui.h's GW_QUEUE each) once its time is up: the kernel's queue drained */

/* a widget's own flags (gk.h's GK_* are the program's, kept in opts) */
#define GKW_HIDDEN   0x01               /* gk_show(w, 0): no room, never painted or reached */
#define GKW_DISABLED 0x02               /* drawn muted, takes no focus and no input */
#define GKW_AWAY     0x04               /* a page not shown (pages, tabs): as hidden, but it keeps its room */
#define GKW_PRESSED  0x08               /* a button held down with the pointer over it */

struct gk_rect { int x, y, w, h; };

/* what a kind of widget does; any hook may be NULL */
struct gk_class {
    int takes_focus;                    /* by default: a stop in the Tab order */
    void (*measure)(struct gk_widget *w);                   /* its own natural size: base_w, base_h */
    void (*arrange)(struct gk_widget *w);                   /* a container: its children's places, from its own */
    void (*paint)(struct gk_widget *w, struct gw *win);     /* at w->x, w->y; the view's clip is set */
    int (*key)(struct gk_widget *w, unsigned key, unsigned mods);   /* 1: the key was its own */
    /* the pointer, in the window's coordinates: pressed and released are the buttons that went down and up now */
    void (*pointer)(struct gk_widget *w, int x, int y, unsigned buttons, unsigned pressed, unsigned released);
    int (*wheel)(struct gk_widget *w, int x, int y, int rows);  /* 1: it took the wheel */
    void (*focus)(struct gk_widget *w, int on);              /* it got or lost the keys (the window's focus too) */
    void (*destroy)(struct gk_widget *w);                   /* free what k holds */
    const char *(*get_text)(const struct gk_widget *w);     /* NULL: w->text */
    void (*set_text)(struct gk_widget *w, const char *text);    /* NULL: w->text, and its size measured again */
    void (*set_value)(struct gk_widget *w, int value);      /* NULL: w->value, painted again */
    void (*lit_part)(const struct gk_widget *w, struct gk_rect *r);  /* what the focus changes: NULL, all of it */
};

struct gk_widget {
    const struct gk_class *cls;
    struct gk *ui;
    struct gk_widget *parent, *child, *last, *next, *prev;
    int id;
    int x, y, w, h;                     /* in the window, from the last layout (hidden: 0) */
    int ox, oy, ow, oh;                 /* the same before it: what a layout moved is painted again */
    int min_w, min_h, grow_x, grow_y, align;
    int base_w, base_h;                 /* its own natural size (measure) */
    int nat_w, nat_h;                   /* and at least gk_size()'s */
    unsigned flags;                     /* GKW_* */
    unsigned opts;                      /* gk.h's GK_* given at its making */
    int value;                          /* a check box's, radio group's, list's, tabs', pages', scroll bar's */
    char *text;                         /* a label's, button's, check box's, status bar's */
    void *data;                         /* the program's */
    void *k;                            /* its kind's own state */
    int gap, pad, across;               /* a box's */
};

struct gk_timer { int id, ms, repeat, on; unsigned due; };
struct gk_watch { int fd, id; short events; };

/* the window's menus (gkdesk.c): the entries in the order the program made them, each in its menu by number (gk_menu()'s
   and gk_submenu()'s, from 0: a title's id is its menu's number, a SUBMENU item's sub the number of its submenu), with
   its shortcut and whether it is a radio item; which menu is the Help, and each number's menu above it (-1: in the
   bar); whether the desktop has them as they are now, and whether it shows them */
#define GK_MENU_NUMBERS 48              /* menus and submenus of a window, at most */
enum { GK_MENUS_UNSENT, GK_MENUS_SHOWN, GK_MENUS_KEYS };
struct gk_menu_entry {
    int menu, sub;
    unsigned id, flags, shortcut;
    unsigned char radio;
    char label[WIN_MENU_LABEL];
    char keys[WIN_MENU_KEYS];
};
struct gk_menus {
    struct gk_menu_entry e[WIN_MENU_ENTRIES];
    int parent[GK_MENU_NUMBERS];
    int n, numbers, help, sent, state;
};

struct gk {
    struct gw *win;
    struct gk_theme theme;
    struct gk_widget *root;             /* the window's content */
    struct gk_widget *box;              /* a modal box open: drawn over the content, it takes every key and click */
    struct gk_widget *focus;            /* the keys' */
    struct gk_widget *capture;          /* the pointer's, from a press until every button is up */
    struct gk_widget *under;            /* the area the pointer is over, told when it leaves */
    struct gk_widget *saved;            /* the focus before the box */
    struct gk_widget *last_area_focus;  /* the area told it has the keys (GK_EV_FOCUS) */
    unsigned buttons;                   /* the pointer's, as last seen */
    unsigned pmods;                     /* and the modifiers its last event had */
    int px, py;                         /* where the pointer was last */
    int layout;                         /* lay out again before painting */
    int failed;                         /* an allocation failed */
    int resizable;
    int had_focus;                      /* the window's focus, as the widgets were last told */
    int ndamage;
    struct gk_rect damage[GK_DAMAGE];
    struct gk_event queue[GK_QUEUE];
    char *qtext[GK_QUEUE];
    int qhead, qcount;
    char *given;                        /* the text of the event given last, freed (and wiped) at the next call */
    int given_len;
    unsigned click_ms;                  /* the last press of the left button, for a double-click */
    struct gk_widget *click_w;
    int click_x, click_y, clicks;
    struct gk_timer timers[GK_TIMERS];
    struct gk_watch watches[GK_WATCHES];
    int painting;                       /* inside gk_flush(): an area's draw must not change the tree */
    struct gk_menus *menus;             /* NULL until the first gk_menu() */
    int choosing;                       /* a chooser's request waits for its answer */
    int user_act;                       /* the event given last is a person's doing: a key, a press, a click, a menu
                                           item, a box's answer (until the next gk_next()): a chooser may be asked */
    int desk_calls;                     /* the kernel has the calls of 11/05/26 (WIN_THEME answered at the opening) */
    struct win_dialog *later;           /* a chooser's request the desktop had no room for (EAGAIN: the window's events
                                           unread), asked again once they are read (gk_choose_again()) */
};

/* gkcore.c */
struct gk_widget *gk_make(struct gk_widget *parent, const struct gk_class *cls, size_t extra);  /* k: extra bytes */
struct gk_widget *gk_make_in(struct gk *ui, const struct gk_class *cls, size_t extra);   /* not in the tree */
void gk_free_tree(struct gk_widget *w);
void gk_dirty(struct gk_widget *w);                         /* painted again */
void gk_dirty_lit(struct gk_widget *w);                     /* what its focus changes, painted again */
void gk_dirty_rect(struct gk *ui, int x, int y, int w, int h);
void gk_relayout(struct gk_widget *w);                      /* its natural size may have changed */
int gk_lit(const struct gk_widget *w);                      /* it has the keys, and the window has the focus */
int gk_reached(const struct gk_widget *w);                  /* shown, on a page shown, enabled */
int gk_shown_now(const struct gk_widget *w);                /* shown, on a page shown: painted */
void gk_post(struct gk *ui, int type, struct gk_widget *w, int value);
struct gk_event *gk_post_ev(struct gk *ui, int type, struct gk_widget *w, int value);  /* its other fields to fill */
void gk_post_text(struct gk *ui, int type, int id, int value, const char *text, int len);
char *gk_strdup(struct gk *ui, const char *s);              /* NULL (and gk_ok() 0) without memory */
int gk_set_str(struct gk_widget *w, char **slot, const char *text);   /* 1 when it changed */
unsigned gk_now(void);
long long gk_div64(long long n, long long d);               /* n / d, n >= 0, d > 0: no libgcc */
void gk_wipe(void *p, size_t n);                            /* n bytes zeroed, kept by the compiler (volatile) */
int gk_ctrl(unsigned key, unsigned mods);                   /* 'a'..'z' for Ctrl and a letter, else 0 */
int gk_shift(unsigned mods);
void gk_move_focus(struct gk *ui, int back);                /* Tab, Shift+Tab */
void gk_set_focus(struct gk *ui, struct gk_widget *w);      /* NULL: none has it */
void gk_paint_tree(struct gk *ui, struct gk_widget *w, const struct gk_rect *r);
struct gk_widget *gk_hit(struct gk_widget *w, int x, int y);
void gk_lay(struct gk_widget *w, int x, int y, int width, int height);   /* place one widget and what is in it */

/* gkwidget.c: drawing that several widgets share */
void gk_frame_in(struct gw *win, const struct gk_theme *t, int x, int y, int w, int h, int lit);  /* a sheet's border */
void gk_focus_ring(struct gw *win, const struct gk_theme *t, int x, int y, int w, int h);
int gk_text_x(const char *s, int cells);                    /* cells of s, at most */
void gk_draw_lines(struct gw *win, int x, int y, int w, const char *text, unsigned color);   /* \n between lines */
int gk_lines(const char *text, int *widest);                /* lines in text, and the widest in cells */

/* a scroll bar's arithmetic and drawing, the desktop's (shell/guidraw.c's gd_thumb() and gd_scrollbar()), in 64 bits
   for totals in the millions; across: a bar along the bottom */
struct gk_bar { int total, shown, first; };
void gk_bar_thumb(int len, const struct gk_bar *b, int *top, int *size);
int gk_bar_first(int len, int pos, const struct gk_bar *b);   /* the first unit for the thumb's top at pos */
int gk_bar_max(const struct gk_bar *b);                       /* the last first that shows the end */
void gk_bar_paint(struct gw *win, const struct gk_theme *t, int x, int y, int len, int across, const struct gk_bar *b);
/* a press at pos along a bar of len: on the thumb, its grab (pos - top, kept in *grab), or a page before or after
   it; 1 when first changed */
int gk_bar_press(int len, int pos, struct gk_bar *b, int *grab);
int gk_bar_drag(int len, int pos, struct gk_bar *b, int grab);

/* gkfield.c: a line of text's state (here for test_gk, which looks that a wiped password leaves no byte), and its text
   for gkbox.c, wiped when it goes */
struct gk_line {
    char *buf;                          /* the text and its NUL: max + 1 bytes */
    int len, max;
    int cursor, anchor;                 /* the selection is between them (equal: none) */
    int scroll;                         /* the first character shown */
    int dragging;
    char *hint;
    char **hist;                        /* remembered lines, oldest first */
    int nhist, maxhist, hpos;           /* hpos: the line brought back, -1 while none is */
    char *draft;                        /* what was being typed before Up */
    char part[4];                       /* a character typed byte by byte, until it is whole */
    int nparts, need;
};
void gk_field_take(struct gk_widget *field, char **text, int *len);

/* gkbox.c */
int gk_box_key(struct gk *ui, unsigned key, unsigned mods);
void gk_box_layout(struct gk *ui);
void gk_box_free(struct gk *ui);
/* a line to type a path in, for a desktop without choosers: its answer is GK_EV_CHOSEN with id */
void gk_box_chooser(struct gk *ui, int id, const char *title, const char *dir, const char *path);

/* gkdesk.c: the desktop's menus, choosers, clipboard and theme */
void gk_menus_send(struct gk *ui);                          /* at a flush: the menus again if they changed */
int gk_menu_key(struct gk *ui, unsigned key, unsigned mods);   /* 1: an item's shortcut, GK_EV_MENU posted */
void gk_dialog_answer(struct gk *ui, const struct win_event *e);   /* WIN_EV_DIALOG */
void gk_choose_again(struct gk *ui);                        /* ui->later asked again, with the window's events read */
int gk_theme_read(struct gk *ui);                           /* WIN_THEME: the colours by role; 1 when it answered */

#endif
