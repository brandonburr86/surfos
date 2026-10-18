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
/* what waits for Notepad's changes to be saved or dropped (gui_state.pending) */
enum { PEND_NONE, PEND_NEW, PEND_OPEN, PEND_OPEN_PATH, PEND_QUIT, PEND_CLOSE };

/* a toolbar: buttons in a row below the title bar, each with its action */
struct gui_tool { const char *label; int act; };
int tool_hit(const struct gui_tool *t, int n, int x, int y);          /* window-relative; the button, or -1 */
int tool_span(const struct gui_tool *t, int n);                      /* the width a window needs for them all */
void tool_render(const struct gui_draw *d, int x, int y, const struct gui_tool *t, int n, const bool *on);

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
bool gui_unsure(const struct gui_state *g, const char *path);       /* g->unsure or g->kept: not to be changed */
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
