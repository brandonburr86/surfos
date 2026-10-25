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
10/25/26 (programs' windows): guiclient.c's calls, gd_blit() and the programs' icon; the palette
and the drawing kit moved to include/sys/guidraw.h (libgui builds them too).
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

/* 10/25/26: the palette and the drawing kit (guidraw.c) are sys/guidraw.h's, which libsurf shares */
#include <sys/guidraw.h>

/* layout, in logical pixels */
#define MENU_H 28                       /* the menu bar */
#define TASK_H 26                       /* the taskbar */
#define TITLE_H 28                      /* a window's title bar */
#define BAR_H 28                        /* a window's toolbar */
#define STATUS_H 22                     /* a window's status bar */
#define ROW_H 18                        /* a row of Files or a chooser */
#define TAB_W 8                         /* a tab moves to the next multiple of 8 columns, as in edit */
#define DOUBLE_MS 500                   /* two clicks this close together are a double-click */

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
#define ACT_DLG_LEAVE (ACT_WINDOW + GUI_SLOTS)              /* leave the desktop although an application is busy */
#define ACT_APP_DIALOG (ACT_WINDOW + GUI_SLOTS + 1)         /* + 32 * (id - GUI_FIRST_APP) + the app's own number */
/* 10/25/26: "End the program?" for a program's window: + its slot (id - GUI_FIRST_CLIENT); beyond Sketch's two
   numbers after the suite's dialogs (gui_media.h) */
#define ACT_CLIENT_END (ACT_APP_DIALOG + 32 * (GUI_WINDOWS - GUI_FIRST_APP) + 8)

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
void gui_close_now(struct gui_state *g, int id);                      /* 10/25/26: shut, asking nothing */
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

/* guiclient.c (10/25/26): programs' windows, in the slots from GUI_FIRST_CLIENT (sys/gui_client.h) */
struct gui_client *client_of(struct gui_state *g, int id);           /* NULL: no program's window */
const struct gui_client *client_at(const struct gui_state *g, int id);
int client_count(const struct gui_state *g);
void client_tick(struct gui_state *g);                  /* windows come, are read again, go; RESIZE and FOCUS */
void client_finish(struct gui_state *g);                /* the desktop closes: every window is let go */
bool client_pointer(struct gui_state *g, unsigned old); /* true: the event was a program's (old: the buttons before) */
void client_wheel(struct gui_state *g, int id, int rows);
bool client_keys(const struct gui_state *g);            /* the focused window is a program's: the keys are its own */
bool client_waiting(const struct gui_state *g, int id); /* 10/25/26: opened without the focus, not given it yet */
void client_key(struct gui_state *g, int key);
void client_close(struct gui_state *g, int id);         /* the close button: CLOSE, then "End the program?" */
void client_answer(struct gui_state *g, int slot, int button, const char *target);
void client_render(const struct gui_state *g, const struct gui_draw *d, int id);   /* d: its content */
const char *client_title(const struct gui_state *g, int id);
bool client_resizable(const struct gui_state *g, int id);
int client_min_width(const struct gui_state *g, int id);
int client_min_height(const struct gui_state *g, int id);
void client_limit(const struct gui_state *g, int id, int *w, int *h);   /* a frame no larger than it may be */

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
void gui_raise(struct gui_state *g, int id);   /* 10/25/26: in front, the focus left where it is */
void field_set(struct gui_field *f, const char *text, bool all);
bool field_key(struct gui_field *f, int key, const char *clip, u32 clip_len);   /* true: the key was the field's */
void field_render(const struct gui_draw *d, const struct gui_field *f, int x, int y, int w, bool focused);
#endif
