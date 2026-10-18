/*
SurfOS Desktop Internals
--------------------
File: guipriv.h Date: 10/17/26
--------------------
What the desktop's files share (include/sys/gui.h is the model's outside): the
theme, the layout, the drawing kit (guidraw.c), the actions that menus, toolbars
and dialogs run (guicore.c), Notepad (guinote.c), Files (guifiles.c) and the
dialogs (guidialog.c). Drawing never changes the state and never calls the file
system; the handlers do both.
10/17/26 (the suite): struct gui_app, what the window manager asks of an application
in a file of its own (Terminal, System Monitor, Disks, Log, Clock, Image Viewer, Hex,
Character Map), and the table of them (guicore.c).
*/
#ifndef _SHELL_GUIPRIV_H
#define _SHELL_GUIPRIV_H
#include <sys/gui.h>

/* the errors the desktop tells apart: the VFS's numbers (fs/vfs.h), which the model does not include */
#ifndef ENOENT
#define ENOENT 2
#define EIO 5
#define ENOMEM 12
#define EACCES 13
#define EBUSY 16
#define EEXIST 17
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define EFBIG 27
#define ENOSPC 28
#define EROFS 30
#define ENAMETOOLONG 36
#define ENOSYS 38
#define ENOTEMPTY 39
#endif
#ifndef EPIPE                           /* 10/17/26 (the suite): the Terminal's session */
#define EAGAIN 11
#define EPIPE 32
#endif

/* the SurfOS palette */
#define NAVY 0x102E3B
#define INK 0x153D49
#define TEAL 0x197D83
#define FOAM 0xB8E9D9
#define PAPER 0xF4F1E6
#define MUTED 0x6C888C
#define CORAL 0xEE906D
#define SAND 0xD0B056
#define SKY 0x659ECB
#define SHELL 0xDEE3D8                  /* light buttons */
#define BAND 0xE7E2D2                   /* gutters, headers, status bars */
#define LINE 0xC9CFC4                   /* thin separators */
#define SHADOW 0x0B2530
#define PICK 0xBFE3DD                   /* selected text and rows */

/* layout, in logical pixels */
#define MENU_H 28                       /* the menu bar */
#define TASK_H 26                       /* the taskbar */
#define TITLE_H 28                      /* a window's title bar */
#define BAR_H 28                        /* a window's toolbar */
#define STATUS_H 22                     /* a window's status bar */
#define SCROLL_W 12
#define ROW_H 18                        /* a row of Files or a chooser */
#define TAB_W 8                         /* a tab moves to the next multiple of 8 columns, as in edit */
#define DOUBLE_MS 500                   /* two clicks this close together are a double-click */

/* where a drawing may go: the surface and a clipping rectangle (left, top, right, bottom) */
struct gui_draw { const struct fb_surface *s; int l, t, r, b; };
void gd_init(struct gui_draw *d, const struct fb_surface *s, int width, int height);
struct gui_draw gd_clip(const struct gui_draw *d, int x, int y, int w, int h);
void gd_rect(const struct gui_draw *d, int x, int y, int w, int h, u32 c);
void gd_frame(const struct gui_draw *d, int x, int y, int w, int h, u32 c);
void gd_glyph(const struct gui_draw *d, int x, int y, u8 ch, u32 c, int scale);
int gd_text(const struct gui_draw *d, int x, int y, const char *s, u32 c);     /* x after the text */
int gd_text_scaled(const struct gui_draw *d, int x, int y, const char *s, u32 c, int scale);
int gd_name(const struct gui_draw *d, int x, int y, const char *s, int cells, u32 c);   /* UTF-8, cut with … */
int gd_cells(const char *s);                                                    /* cells a UTF-8 name takes */
u32 gd_char_len(const char *s, u32 avail);            /* the bytes of the character at s (UTF-8 as the console has it) */
u8 gd_char(const char *s, u32 avail, u32 *len);       /* its glyph, and its bytes */
void gd_button(const struct gui_draw *d, int x, int y, int w, int h, const char *label, u32 bg, u32 fg);
void gd_wave(const struct gui_draw *d, int x, int y, int w, int h, u32 c);
void gd_scrollbar(const struct gui_draw *d, int x, int y, int h, int first, int shown, int total);
#define ICON_FOLDER 0
#define ICON_OPEN_FOLDER 1
#define ICON_FILE 2
#define ICON_TEXT 3
#define ICON_DRIVE 4
#define ICON_NOTEPAD 5
#define ICON_FILES 6
#define ICON_CALC 7
#define ICON_SKETCH 8
#define ICON_WAVE 9
#define ICON_HELP 10
#define ICON_GEAR 11
#define ICON_TERMINAL 12                /* 10/17/26: the suite's */
#define ICON_MONITOR 13
#define ICON_DISKS 14
#define ICON_LOG 15
#define ICON_CLOCK 16
#define ICON_VIEWER 17
#define ICON_HEX 18
#define ICON_CHARMAP 19
void gd_icon(const struct gui_draw *d, int x, int y, int icon, int scale);
void gd_icon_tint(const struct gui_draw *d, int x, int y, int icon, int scale, u32 tint);   /* one colour */
/* the scroll bar's thumb, for drawing and for the pointer: its top and height in a bar of h pixels */
void gd_thumb(int h, int first, int shown, int total, int *top, int *size);
int gd_scroll_to(int h, int y, int shown, int total);  /* the first row for the thumb's top at y */

/* what menus, toolbars and dialogs do (guicore.c: gui_act) */
enum {
    ACT_NONE,
    ACT_ABOUT, ACT_SETTINGS, ACT_THEME, ACT_QUIT,
    ACT_NEW, ACT_OPEN, ACT_SAVE, ACT_SAVE_AS, ACT_REVEAL, ACT_CLOSE,
    ACT_UNDO, ACT_REDO, ACT_CUT, ACT_COPY, ACT_PASTE, ACT_DELETE, ACT_SELECT_ALL, ACT_FIND, ACT_FIND_NEXT, ACT_GOTO,
    ACT_NOTEPAD, ACT_FILES, ACT_CALC, ACT_SKETCH, ACT_CLEAR_SKETCH,
    ACT_NEXT_WINDOW, ACT_MINIMIZE, ACT_MAXIMIZE, ACT_HELP,
    ACT_FILES_OPEN, ACT_FILES_NEW_FILE, ACT_FILES_NEW_FOLDER, ACT_FILES_RENAME, ACT_FILES_DELETE, ACT_FILES_COPY,
    ACT_FILES_REFRESH, ACT_FILES_MOUNT, ACT_FILES_EJECT,
    /* a dialog's answer */
    ACT_DLG_NEW_FILE, ACT_DLG_NEW_FOLDER, ACT_DLG_RENAME, ACT_DLG_DELETE, ACT_DLG_GOTO, ACT_DLG_OPEN,
    ACT_DLG_SAVE_AS, ACT_DLG_REPLACE, ACT_DLG_GUARD, ACT_DLG_READONLY,
    ACT_WINDOW                          /* + a window's number: bring it to the front */
};
/* 10/17/26 (the suite): the answers to the suite's dialogs */
#define ACT_DLG_LEAVE (ACT_WINDOW + GUI_WINDOWS)            /* leave the desktop although an application is busy */
#define ACT_APP_DIALOG (ACT_WINDOW + GUI_WINDOWS + 1)       /* + 32 * (id - GUI_FIRST_APP) + the app's own number */

/* what waits for Notepad's changes to be saved or dropped (gui_state.pending) */
enum { PEND_NONE, PEND_NEW, PEND_OPEN, PEND_OPEN_PATH, PEND_QUIT, PEND_CLOSE };

/* a toolbar: buttons in a row below the title bar, each with its action */
struct gui_tool { const char *label; int act; };
int tool_hit(const struct gui_tool *t, int n, int x, int y);          /* window-relative; the button, or -1 */
int tool_span(const struct gui_tool *t, int n);                      /* the width a window needs for them all */
void tool_render(const struct gui_draw *d, int x, int y, const struct gui_tool *t, int n, const bool *on);

/* 10/17/26 (the suite): an application in a file of its own, as the window manager sees it. Every hook but render
   may be NULL. The window's content is below its title bar: x and y are relative to the window, as for Notepad. */
struct gui_app {
    const char *title;
    int icon;
    const char *shortcut;               /* the Apps menu's column ("Ctrl+T"), "" for none */
    int w, h;                           /* its size when it first opens (made to fit the screen) */
    bool resizable;                     /* maximize, and the corner grip */
    bool arrows;                        /* the arrow keys and Home, End, Page Up and Down are its own */
    bool keys;                          /* every key but Ctrl+G is its own: none of the desktop's shortcuts */
    int (*min_width)(void);             /* a resizable window's narrowest; NULL: 360 */
    void (*opened)(struct gui_state *g);    /* the window opens, from closed */
    void (*closed)(struct gui_state *g);    /* it closes, or the desktop does (gui_finish) */
    void (*render)(const struct gui_state *g, const struct gui_draw *d);   /* d: clipped to the window, below its title */
    bool (*key)(struct gui_state *g, int key);              /* true: the key was its own (else Esc closes, Tab moves on) */
    void (*press)(struct gui_state *g, int x, int y);       /* the left button, in the content */
    void (*drag)(struct gui_state *g, int x, int y);        /* the button held since that press (x, y may be outside) */
    void (*release)(struct gui_state *g);
    void (*wheel)(struct gui_state *g, int rows);
    void (*tick)(struct gui_state *g);                      /* every frame while it is open (minimized too) */
    void (*title_of)(const struct gui_state *g, char *out, size_t n);  /* NULL: title */
    bool (*busy)(const struct gui_state *g, char *why, size_t n);     /* leaving the desktop would end something */
    void (*answer)(struct gui_state *g, int act, int button, const char *text, const char *target);   /* its dialogs */
    const char *const *help;            /* its lines in Help ("#" first: a heading), help_lines of them */
    int help_lines;
    /* the Edit menu while it has the focus (ACT_UNDO .. ACT_GOTO, and their keys Ctrl+Z, Y, X, C, V, A, F, L when its
       key hook leaves them): whether an item is on, and doing it. NULL: the Edit menu is Notepad's */
    bool (*can_edit)(const struct gui_state *g, int act);
    void (*edit)(struct gui_state *g, int act);
    const char *goto_label;             /* the Edit menu's last item, "Go to offset\xF9"; NULL: "Go to line\xF9" */
    /* 10/18/26: whether the arrow keys are its own just now (with nothing to move they move the pointer, as in
       Calculator); NULL: arrows says */
    bool (*wants_arrows)(const struct gui_state *g);
};
extern const struct gui_app gui_app_terminal, gui_app_monitor, gui_app_disks, gui_app_log, gui_app_clock,
                            gui_app_viewer, gui_app_hex, gui_app_charmap;
const struct gui_app *gui_app_of(int id);       /* the table's, NULL for the desktop's own windows */
/* a dialog of application id: its answer comes to its answer hook with act (0-30) */
static inline int gui_app_act(int id, int act) { return ACT_APP_DIALOG + 32 * (id - GUI_FIRST_APP) + act; }
#define GUI_APP_CLOSE 31                /* the window manager's own: closing the window although it is busy */

/* guicore.c */
void gui_act(struct gui_state *g, int act);
/* a dialog's answer: button 0 (OK, Yes, Save), 1 (No, Don't save) or -1 (Cancel, Esc); text is
   what was typed or chosen, target what the dialog was about */
void gui_answer(struct gui_state *g, int act, int button, const char *text, const char *target);
void gui_close(struct gui_state *g, int id);
bool gui_guard(struct gui_state *g, int pending, const char *path);   /* true: go ahead now */
void gui_proceed(struct gui_state *g);                                /* run what was pending */
void gui_place(struct gui_state *g, int id);                          /* keep a window on the screen */
const char *gui_error(const struct gui_state *g, int err);
bool gui_join(char *out, size_t n, const char *dir, const char *name);   /* false: too long, not usable */
const char *gui_basename(const char *path);
/* not to be saved over, deleted or renamed: one of g->unsure and g->kept, a folder one is in, or anything
   once more saves failed half way than the desktop keeps; and the dialog line that says why */
bool gui_unsure(const struct gui_state *g, const char *path);
bool gui_remembered(const struct gui_state *g, const char *path);  /* one of g->unsure and g->kept itself */
void gui_unsure_line(const struct gui_state *g, const char *path, char *line, size_t n);
void gui_parent(char *path);
void gui_date(u32 t, char *out, size_t n);                           /* "2026-10-17 14:03" */
void gui_bytes(u32 n, char *out, size_t size);                       /* "1,234 B", "12.5 KB" */
bool gui_double(u32 *last_ms, int *last_row, u32 now, int row);      /* a second click on the same row in time */
bool gui_inside(int x, int y, int l, int t, int w, int h);
int gui_clamp(int n, int a, int b);
bool gui_menu_on(const struct gui_state *g, int menu, int act);      /* for the host test */
void gui_menu_box(const struct gui_state *g, int menu, int *x, int *y, int *w, int *h);

/* guinote.c */
struct note_layout { int tx, ty, cols, rows, gutter, bar_x, find_y, status_y; };
void note_layout(const struct gui_state *g, struct note_layout *l);
void note_reset(struct gui_note *n);
int note_load(struct gui_state *g, const char *path);                /* 0, or -errno after saying so */
int note_save(struct gui_state *g, const char *path);
/* path is not to be saved over, deleted or renamed: gui_unsure(), or a twin beside it found now (and then
   remembered); and whether a save to path is refused (past GUI_UNSURE pairs only a new name may be saved) */
bool note_guarded(struct gui_state *g, const char *path);
bool note_refused(struct gui_state *g, const char *path);
void note_remember(struct gui_state *g, const char *file, const char *kept);   /* 10/18/26: a pair a write kept */
void note_key(struct gui_state *g, int key);
void note_press(struct gui_state *g, int x, int y);                 /* window-relative */
void note_drag(struct gui_state *g, int x, int y);
void note_wheel(struct gui_state *g, int lines);
void note_scroll(struct gui_state *g, int y);                        /* the held thumb, window-relative */
void note_render(const struct gui_state *g, const struct gui_draw *d);
void note_title(const struct gui_state *g, char *out, size_t n);
int note_min_width(void);                                            /* its toolbar's width */
void note_insert(struct gui_state *g, const char *s, u32 len);       /* over the selection */
void note_copy(struct gui_state *g, bool cut);
void note_paste(struct gui_state *g);
void note_undo(struct gui_state *g, bool redo);
void note_select_all(struct gui_state *g);
bool note_find(struct gui_state *g, const char *what);              /* the next match, selected */
void note_goto(struct gui_state *g, int line);
bool note_has_selection(const struct gui_note *n);
void note_status(struct gui_state *g, const char *msg);
void gui_middle(const char *name, char *out, size_t n, int cells);   /* at most cells cells: its middle goes */
int note_lines(const struct gui_note *n);
int note_line_of(const struct gui_note *n, u32 off);
u32 note_line_start(const struct gui_note *n, int line);
int note_column(const struct gui_note *n, u32 off);                  /* cells from the line's start */

/* guifiles.c */
void files_open(struct gui_state *g);                                /* the window opens: read / again */
void files_refresh(struct gui_state *g);                             /* read again, keeping what is open */
void files_reveal(struct gui_state *g, const char *path);            /* expand down to path and select it */
void files_key(struct gui_state *g, int key);
void files_press(struct gui_state *g, int x, int y);
void files_wheel(struct gui_state *g, int rows);
void files_scroll(struct gui_state *g, int y);
void files_render(const struct gui_state *g, const struct gui_draw *d);
void files_act(struct gui_state *g, int act);
void files_answer(struct gui_state *g, int act, int button, const char *text, const char *target);
bool files_can(const struct gui_state *g, int act);
bool files_path(const struct gui_state *g, int row, char *out, size_t n);   /* false: too long, not usable */
int files_min_width(void);
int files_rows(const struct gui_state *g);                           /* rows the window shows */
int gui_list(const struct gui_fs *fs, const char *path, struct gui_entry *out, int max, int *total);  /* sorted */

/* guidialog.c */
void dialog_message(struct gui_state *g, const char *title, const char *line1, const char *line2);
void dialog_confirm(struct gui_state *g, int act, const char *title, const char *line1, const char *line2,
                    const char *yes, const char *no, const char *cancel);
void dialog_input(struct gui_state *g, int act, const char *title, const char *prompt, const char *text);
void dialog_chooser(struct gui_state *g, int kind, int act, const char *dir, const char *name);
void dialog_key(struct gui_state *g, int key);
void dialog_pointer(struct gui_state *g, int x, int y, bool press);
void dialog_wheel(struct gui_state *g, int rows);
void dialog_render(const struct gui_state *g, const struct gui_draw *d);
void dialog_close(struct gui_state *g);
void dialog_box(const struct gui_state *g, int *x, int *y, int *w, int *h);
void field_set(struct gui_field *f, const char *text, bool all);
bool field_key(struct gui_field *f, int key, const char *clip, u32 clip_len);   /* true: the key was the field's */
void field_render(const struct gui_draw *d, const struct gui_field *f, int x, int y, int w, bool focused);
#endif
