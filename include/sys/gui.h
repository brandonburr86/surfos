/*
SurfOS Desktop
--------------------
File: gui.h Date: 10/10/26
--------------------
Pure desktop model and renderer; no hardware, allocation or blocking calls.
10/17/26: Notepad, Files, dialogs, menus as tables and a window manager with a
taskbar (docs/GUI.md). Files are reached only through struct gui_fs, which the
model calls from gui_key() and gui_pointer(), never from gui_render():
shell/guifs.c implements it on the VFS, the host test in memory.
*/
#ifndef _SYS_GUI_H
#define _SYS_GUI_H
#include <sys/framebuffer.h>
#include <sys/pointer.h>
#define GUI_CALC 0
#define GUI_SKETCH 1
#define GUI_ABOUT 2
#define GUI_SETTINGS 3
#define GUI_NOTEPAD 4
#define GUI_FILES 5
#define GUI_HELP 6
#define GUI_WINDOWS 7
#define GUI_CANVAS_W 400
#define GUI_CANVAS_H 180
/* keys that are not bytes: shell/gui.c turns the terminal's escape sequences into these */
#define GUI_UP 0x100
#define GUI_DOWN 0x101
#define GUI_LEFT 0x102
#define GUI_RIGHT 0x103
#define GUI_HOME 0x104
#define GUI_END 0x105
#define GUI_PGUP 0x106
#define GUI_PGDN 0x107
#define GUI_DELETE 0x108
#define GUI_INSERT 0x109

#define GUI_PATH_MAX 256
#define GUI_NAME_MAX 256
#define GUI_NOTE_MAX (256 * 1024)       /* Notepad's largest file */
#define GUI_CLIP_MAX (128 * 1024)
#define GUI_UNDO_TEXT (64 * 1024)       /* the text undo keeps, oldest dropped first */
#define GUI_UNDO_STEPS 512
#define GUI_NODES 400                   /* rows of the Files tree */
#define GUI_LIST_MAX 256                /* entries a chooser shows of one folder */
#define GUI_VOLUMES 16
#define GUI_UNSURE 4                    /* saves whose rename may have half happened, remembered */
#define GUI_STATUS 96

/* a folder entry, or what a file is (struct gui_fs) */
#define GUI_FILE 1
#define GUI_DIR 2
struct gui_entry { char name[GUI_NAME_MAX]; int type; u32 size, mtime; };
/* a USB volume with a file system: the device, its file system and size; path is where
   it is mounted, "" when it is not; readonly: it is mounted read-only, or would be */
struct gui_volume { char dev[16], disk[16], fs[8], size[16], path[GUI_PATH_MAX]; bool readonly, boot, gone; };   /* gone: unplugged, still mounted */
/* Every call returns 0 (list and volumes: how many there are) or a negative errno. */
struct gui_fs {
    void *ctx;
    int (*list)(void *ctx, const char *path, struct gui_entry *out, int max);   /* not . and .., in no order */
    int (*stat)(void *ctx, const char *path, struct gui_entry *out, bool *device, bool *readonly);
    int (*read)(void *ctx, const char *path, char *buf, u32 size, u32 *len);    /* -EFBIG: more than size bytes */
    /* crlf: each \n as \r\n. kept: when the save's rename may have half happened, its new file, where the
       text was left (gone, if the rename did finish), "" otherwise */
    int (*write)(void *ctx, const char *path, const char *data, u32 len, bool crlf, char *kept, size_t n);
    int (*mkdir)(void *ctx, const char *path);
    int (*remove)(void *ctx, const char *path, bool dir);                       /* a file, or an empty folder */
    int (*rename)(void *ctx, const char *from, const char *to);
    int (*volumes)(void *ctx, struct gui_volume *out, int max);
    int (*mount)(void *ctx, const char *dev, char *msg, size_t n);   /* the shell's mount and eject: 0 or -1, */
    int (*eject)(void *ctx, const char *dev, char *msg, size_t n);   /* and the last line they printed in msg */
    const char *(*error)(int err);
};

struct gui_window { int x,y,w,h; bool open, max, min; int nx,ny,nw,nh; };   /* n*: the place before maximizing */
struct gui_calc { i32 value, operand; char op; bool fresh, error, entered; };
/* a one-line text box; all: its whole text is selected, and typing replaces it */
struct gui_field { char text[GUI_PATH_MAX]; u32 len, cursor; bool all; };

/* Notepad: one document. The selection runs from anchor to cursor (byte offsets, always at the
   start of a character); top and left are the first line and column shown; goal is the column
   Up and Down keep (-1: the cursor's). An undo step is len bytes at pos that were inserted or
   deleted, kept at text in undo_text; steps of one group are undone together. */
struct gui_undo { u32 pos, len, text, group; bool insert; };
struct gui_note {
    char path[GUI_PATH_MAX];            /* "" until it is saved */
    u32 len, lf, cursor, anchor;        /* lf: the line ends in the text */
    int top, left, goal;
    bool modified, crlf, readonly, selecting, finding, typing;
    u32 click_ms, click_at;             /* the last click, for a double-click */
    struct gui_field find;
    char status[GUI_STATUS];
    int nundo, undo_at, saved_at;       /* steps kept; steps in force (redo is above); in force when saved (-1: gone) */
    u32 undo_used, group;
    struct gui_undo undo[GUI_UNDO_STEPS];
    char undo_text[GUI_UNDO_TEXT];
    char text[GUI_NOTE_MAX];
};

/* Files: the tree as rows. A row's parent is an earlier row (-1 for /); a folder's children
   follow it when it is expanded. GUI_NODE_DRIVE is a USB volume that is not mounted;
   GUI_NODE_MORE says how many entries did not fit. */
#define GUI_NODE_DRIVE 3
#define GUI_NODE_MORE 4
struct gui_node { char name[GUI_NAME_MAX]; int parent, depth, type, vol; u32 size, mtime; bool expanded; };
struct gui_files {
    struct gui_node node[GUI_NODES];
    int count, selected, top;
    u32 click_ms;
    int click_row;
    char status[GUI_STATUS];
    int volumes;
    struct gui_volume vol[GUI_VOLUMES];
    char keep[4096];                    /* a refresh: the selected row's path, then the open folders' */
    struct gui_entry scratch[GUI_LIST_MAX];   /* a folder being read */
};

/* a dialog in front of everything; its OK runs act (shell/guidialog.c) */
#define GUI_DLG_MESSAGE 1
#define GUI_DLG_CONFIRM 2
#define GUI_DLG_INPUT 3
#define GUI_DLG_OPEN 4
#define GUI_DLG_SAVE 5
struct gui_dialog {
    int kind, act, choice;
    bool list_focus;                    /* a chooser's Enter is for its list, not its name field */
    char title[48], text[2][80];
    const char *label[3];
    struct gui_field field;
    char dir[GUI_PATH_MAX], target[GUI_PATH_MAX];   /* a chooser's folder; what the action is about */
    int count, total, selected, top;
    u32 click_ms;
    int click_row;
    struct gui_entry list[GUI_LIST_MAX];
};

struct gui_state {
    int width, height, menu, selection, focus, drag, drag_x, drag_y;
    int x, y, last_x, last_y, brush, speed;
    unsigned buttons, devices;
    bool quit, dirty, night, drawing;
    int order[GUI_WINDOWS];
    struct gui_window win[GUI_WINDOWS];
    struct gui_calc calc;
    u8 canvas[GUI_CANVAS_W * GUI_CANVAS_H];
    /* 10/17/26 */
    const struct gui_fs *fs;            /* NULL: no files (Notepad still edits) */
    u32 now;                            /* milliseconds, from the shell (double-clicks) */
    char clock[8];                      /* "14:05" from the shell, "" for none */
    int resize, scroll, scroll_at;      /* a window being resized; one whose scroll bar is held, and where */
    u32 title_ms;                       /* the last press on a title bar, for a double-click: when, which, where */
    int title_id, title_x, title_y, press_x, press_y, help_top;   /* press_*: where the pointer was then */
    int pending;                        /* what to do once Notepad's changes are saved or dropped */
    char pending_path[GUI_PATH_MAX];
    struct gui_dialog dialog;
    struct gui_files files;
    u32 clip_len;
    char clip[GUI_CLIP_MAX];
    struct gui_note note;
    /* saves whose rename may have half happened, and pairs found that look so (note_guarded()): such a file
       and the new file kept beside it may share their clusters until the volume is checked, so the desktop
       saves over, deletes and renames neither (gui_unsure()). The first GUI_UNSURE are kept; nunsure counts
       them all, and past GUI_UNSURE the desktop saves over, deletes and renames no file (new names still) */
    char unsure[GUI_UNSURE][GUI_PATH_MAX], kept[GUI_UNSURE][GUI_PATH_MAX];
    int nunsure;
};
void gui_init(struct gui_state *g, int width, int height);
void gui_open(struct gui_state *g, int app);
void gui_pointer(struct gui_state *g, const struct pointer_event *e);
void gui_key(struct gui_state *g, int key);
void gui_render(const struct gui_state *g, const struct fb_surface *s);
void gui_cursor(const struct fb_surface *s, int x, int y, int scale);
void gui_calc_key(struct gui_calc *c, int key);
/* 10/17/26: the arrow keys belong to the model (Notepad, Files, a menu or a dialog has them),
   not to the pointer */
bool gui_wants_arrows(const struct gui_state *g);
/* leave the desktop: at once, or after asking about Notepad's unsaved changes */
void gui_request_quit(struct gui_state *g);
const struct gui_fs *gui_kernel_fs(void);       /* shell/guifs.c: the VFS (the kernel only) */
#endif
