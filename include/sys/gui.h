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
10/17/26 (the suite, docs/superpowers/plans/2026-10-17-gui-suite.md): Terminal,
System Monitor, Disks, Log, Clock, Image Viewer, Hex and Character Map, each in a
file of its own that the window manager reaches through a table (struct gui_app,
shell/guipriv.h). What they need of the kernel is struct gui_sys (sys/gui_sys.h),
which the model calls from its handlers and from gui_tick(), never from gui_render().
10/25/26 (programs' windows, docs/superpowers/plans/2026-10-25-app-windows.md): the windows of
ordinary programs, in GUI_CLIENTS slots after the fixed windows (sys/gui_client.h,
shell/guiclient.c), through struct gui_sys's win_* calls.
11/05/26 (the desktop applications, docs/superpowers/plans/2026-11-05-desktop-apps.md, track desk):
keys with their modifiers for programs' windows (GUI_KEY_SHIFT, GUI_KEY_ALT and GUI_KEY_CTRL on a key,
gui_state.mods for the pointer), F5 to F12; programs' menus in the bar, the clipboard they paste,
their choosers and the theme; the application registry (sys/gui_reg.h, shell/guireg.c); screenshots
(struct gui_shot: the shell captures, the model asks where and saves); the preferences on the stick.
*/
#ifndef _SYS_GUI_H
#define _SYS_GUI_H
#include <sys/framebuffer.h>
#include <sys/pointer.h>
#include <sys/guicalc.h>                /* 10/25/26: the Calculator's arithmetic, which libsurf shares */
#define GUI_CALC 0
#define GUI_SKETCH 1
#define GUI_ABOUT 2
#define GUI_SETTINGS 3
#define GUI_NOTEPAD 4
#define GUI_FILES 5
#define GUI_HELP 6
/* 10/17/26 (the suite): windows from GUI_FIRST_APP on are those of struct gui_app's table (guicore.c) */
#define GUI_TERMINAL 7
#define GUI_MONITOR 8
#define GUI_DISKS 9
#define GUI_LOG 10
#define GUI_CLOCK 11
#define GUI_VIEWER 12
#define GUI_HEX 13
#define GUI_CHARMAP 14
#define GUI_FIRST_APP GUI_TERMINAL
#define GUI_WINDOWS 15
/* 10/25/26 (programs' windows, docs/superpowers/plans/2026-10-25-app-windows.md): after the fixed windows come
   GUI_CLIENTS slots for programs' windows (shell/guiclient.c); win[] and order[] hold all GUI_SLOTS */
#define GUI_CLIENTS 8
#define GUI_FIRST_CLIENT GUI_WINDOWS
#define GUI_SLOTS (GUI_FIRST_CLIENT + GUI_CLIENTS)
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
#define GUI_BACKTAB 0x10a               /* 11/1/26: for a program's window only: the desktop's apps have no use for them, */
#define GUI_SHIFT_ENTER 0x10b           /* shell/gui.c's input_key() keeps them from those */
/* 11/05/26: F1 to F12 (surfos/win.h's WIN_KEY_F(n)): for a program's window only, as Shift+Tab (F1-F4 never come:
   they switch consoles); and the modifiers a key goes with to a program's window, above its code: the desktop's
   own windows get the code alone (gui_key()), as before.
   11/05/26 (the review): shell/gui.c gives gui_key() "full keys": a character as typed (with Shift, before Ctrl made a
   control byte of it: Ctrl+S is 's' with GUI_KEY_CTRL), Tab 9, Enter 13, Esc 27, Backspace 127 or a GUI_* key, with
   the modifiers held and GUI_KEY_REPEAT for the keyboard's repeat (gui_byte_key(), gui_sequence_key(): its terminal's
   bytes and sequences, tty.h's TTY_RAW_KEYS). A program's window gets them as surfos/win.h's "The keys" says
   (gui_program_key(): Ctrl+Tab .. Ctrl+Backspace as GUI_CTRL_TAB .. GUI_CTRL_BACKSPACE), the desktop's own windows
   as they did before (gui_own_key(): the control bytes; none for a key Ctrl made nothing of) */
#define GUI_F(n) (0x110 + (n))
#define GUI_CTRL_TAB 0x10c
#define GUI_CTRL_ENTER 0x10d
#define GUI_CTRL_ESC 0x10e
#define GUI_CTRL_BACKSPACE 0x10f
#define GUI_KEY_MASK 0xFFFF
#define GUI_KEY_SHIFT (WIN_MOD_SHIFT << 16)
#define GUI_KEY_ALT (WIN_MOD_ALT << 16)
#define GUI_KEY_CTRL (WIN_MOD_CTRL << 16)
#define GUI_KEY_REPEAT (WIN_MOD_REPEAT << 16)

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
    /* 10/17/26 (the suite): up to n bytes from offset off of a file (Hex, Image Viewer): 0 with *len (0 at its end)
       and *size its whole size; NULL in a struct gui_fs that cannot */
    int (*read_at)(void *ctx, const char *path, u32 off, char *buf, u32 n, u32 *len, u32 *size);
};

struct gui_window { int x,y,w,h; bool open, max, min; int nx,ny,nw,nh; };   /* n*: the place before maximizing */
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
#define GUI_DLG_RUN 6                   /* 10/25/26: the launcher's: a folder's programs and the line to run */
#define GUI_DLG_FOLDER 7                /* 11/05/26: a program's Choose folder */
#define GUI_DIALOG_TITLE 160            /* 11/05/26: a title's bytes (a program's chooser's names its window too) */
#define GUI_TYPES_MAX 144               /* 11/05/26: a program's chooser's file types (surfos/win.h's WIN_TYPES_MAX) */
struct gui_dialog {
    int kind, act, choice;
    char types[GUI_TYPES_MAX];          /* 11/05/26: the file types a program's chooser is for ("png bmp"), "" all */
    bool list_focus;                    /* a chooser's Enter is for its list, not its name field */
    bool program;                       /* 11/05/26 (the review of stage 2): a program's chooser: no desktop's sign */
    char title[GUI_DIALOG_TITLE], text[2][80];
    const char *label[3];
    struct gui_field field;
    char dir[GUI_PATH_MAX], target[GUI_PATH_MAX];   /* a chooser's folder; what the action is about */
    int count, total, selected, top;
    u32 click_ms;
    int click_row;
    struct gui_entry list[GUI_LIST_MAX];
};

/* 10/17/26 (the suite): the applications' own state, each in its track's header */
#include <sys/gui_sys.h>
#include <sys/gui_term.h>
#include <sys/gui_sysapps.h>
#include <sys/gui_tools.h>
#include <sys/gui_media.h>
#include <sys/gui_client.h>             /* 10/25/26: programs' windows */
#include <sys/gui_reg.h>                /* 11/05/26: the application registry */

/* 11/05/26: a screenshot. The shell captures the frame once one is WANTED (the frame after the request: a menu its
   item closed is gone from it), as a 24-bit BMP of its own memory, and hands it over (TAKEN); the model asks where
   with the Save as chooser (ASKING) when no other dialog is up, saves it through struct gui_fs's write, and lets it
   go (DONE), which the shell frees */
#define GUI_SHOT_NONE 0
#define GUI_SHOT_WANTED 1
#define GUI_SHOT_TAKEN 2
#define GUI_SHOT_ASKING 3
#define GUI_SHOT_DONE 4
struct gui_shot { int state; const char *data; u32 len; char name[48]; };

struct gui_state {
    int width, height, menu, selection, focus, drag, drag_x, drag_y;
    int x, y, last_x, last_y, brush, speed;
    unsigned buttons, devices;
    bool quit, dirty, night, drawing;
    int order[GUI_SLOTS];
    struct gui_window win[GUI_SLOTS];
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
    /* 10/17/26 (the suite) */
    const struct gui_sys *sys;          /* NULL: no system (the applications that need it say so) */
    struct gui_time time;               /* the CMOS clock, from the shell (year 0: not known) */
    int app_drag;                       /* a suite window whose content has the held button (its drag handler), -1 */
    struct gui_term term;
    struct gui_monitor monitor;
    struct gui_disks disks;
    struct gui_logview logview;
    struct gui_viewer viewer;
    struct gui_hex hex;
    /* saves whose rename may have half happened, and pairs found that look so (note_guarded()): such a file
       and the new file kept beside it may share their clusters until the volume is checked, so the desktop
       saves over, deletes and renames neither (gui_unsure()). The first GUI_UNSURE are kept; nunsure counts
       them all, and past GUI_UNSURE the desktop saves over, deletes and renames no file (new names still) */
    char unsure[GUI_UNSURE][GUI_PATH_MAX], kept[GUI_UNSURE][GUI_PATH_MAX];
    int nunsure;
    struct gui_clients clients;         /* 10/25/26: programs' windows (shell/guiclient.c) */
    int menu_x, menu_y;                 /* 10/26/26 (stage C8): where a right-click opened the menu; menu_y 0: the bar's */
    int menu_win;                       /* and the window it is for, whose focus it lasts while; -1: the desktop's */
    int app_pid[GUI_WINDOWS - GUI_FIRST_APP];  /* 10/26/26 (stage C12): the program an application's launcher started
                                                 last, its pid (0: none): its window is the application's; */
    u32 app_at[GUI_WINDOWS - GUI_FIRST_APP];   /* and when (now); */
    bool app_starting[GUI_WINDOWS - GUI_FIRST_APP];    /* its window has not come yet: the tile pressed again within
                                                         GUI_START_MS starts no second one (a double-click) */
    /* 11/05/26 (the desktop applications, track desk) */
    unsigned mods;                      /* the modifiers held as the shell took the pointer's events (WIN_MOD_*) */
    u32 clip_serial, clip_given;        /* the clipboard's changes (gui_clipped()), and the one given for WIN_PASTE */
    bool clip_ever;                     /* it was given once */
    int menu_top;                       /* the first entry shown of a menu taller than the screen */
    int menu_sub, menu_sub_of;          /* the program's item whose submenu is open (MENU_CLIENT_SUB), and its menu */
    struct gui_registry reg;            /* /initrd/apps */
    int theme_given;                    /* the theme the programs' windows were told last (-1: none yet) */
    int pref_theme, pref_speed;         /* the preferences as the settings said them last (-1: not read yet), */
    u32 pref_at;                        /* and when (now) */
    u32 pref_pending, pref_changed;     /* (the review of stage 2) changed here, not written yet (a bit each: the theme,
                                           the speed), and when the last change was: written once they stand */
    struct gui_task chooser_tasks[GUI_TASK_MAX];   /* (the review of stage 2) the tasks, read for a chooser's name */
    char pref_status[GUI_STATUS];       /* what Pointer & desktop's last change did: saved, or why not */
    struct gui_shot shot;
    struct gui_volume vols[GUI_VOLUMES];    /* the volumes, read for the screenshot's folder */
};
#define GUI_START_MS 2000
void gui_init(struct gui_state *g, int width, int height);
void gui_open(struct gui_state *g, int app);
void gui_pointer(struct gui_state *g, const struct pointer_event *e);
void gui_release(struct gui_state *g);      /* 10/18/26: the button let go of with no event (the screen came back) */
void gui_key(struct gui_state *g, int key);
void gui_render(const struct gui_state *g, const struct fb_surface *s);
void gui_cursor(const struct fb_surface *s, int x, int y, int scale);
/* 10/17/26: the arrow keys belong to the model (Notepad, Files, a menu or a dialog has them),
   not to the pointer */
bool gui_wants_arrows(const struct gui_state *g);
bool gui_wants_shifted(const struct gui_state *g);
/* 11/05/26: full keys (above): a byte the terminal sent alone, the end of an escape sequence (its number, final byte
   and xterm's m; 0 for none), and a full key as the desktop's own windows take it (-1: none) or a program's window
   (its value, and its surfos/win.h WIN_MOD_* bits into *mods) */
int gui_byte_key(int byte);
int gui_sequence_key(int number, int final, int m);
int gui_own_key(int key);
int gui_program_key(int key, int *mods);
/* 10/25/26 (the review): an application works without drawing every pass (a picture being read, a Hex search, a
   Terminal's output read but not drawn, keys its session has not taken): the desktop's next pass comes a frame
   later, not after its idle nap */
bool gui_working(const struct gui_state *g);
/* leave the desktop: at once, or after asking about Notepad's unsaved changes */
void gui_request_quit(struct gui_state *g);
/* 10/17/26 (the suite): the shell calls gui_tick() every frame (the applications read what is new: a terminal's output,
   the task list), and gui_finish() before it frees the state (a terminal session hangs up) */
void gui_tick(struct gui_state *g);
void gui_finish(struct gui_state *g);
/* 10/26/26 (stage C12): and gui_input_done() once a pass's pointer events and keys are in: a program's window that
   asked to be seen comes up then, never under a press of that pass */
void gui_input_done(struct gui_state *g);
/* 11/05/26: a screenshot: the shell's Print Screen asks for one (the frame then captured and handed over in
   g->shot); the theme's colours by role (surfos/win.h's WIN_COLOR_*), for the programs' windows */
void gui_screenshot(struct gui_state *g);
void gui_theme_colors(const struct gui_state *g, u32 *colors);
/* 10/25/26: the largest content a program's window can have here, a maximized one's (shell/gui.c registers it) */
void gui_client_room(const struct gui_state *g, int *w, int *h);
const struct gui_fs *gui_kernel_fs(void);       /* shell/guifs.c: the VFS (the kernel only) */
/* 10/18/26: a desktop's own struct gui_fs, whose read_at keeps the file it read last open until
   gui_kernel_fs_idle() (each frame, before drawing or sleeping); gui_kernel_fs_end() closes and frees it */
struct gui_fs *gui_kernel_fs_begin(void);
void gui_kernel_fs_idle(const struct gui_fs *fs);
void gui_kernel_fs_end(struct gui_fs *fs);
#endif
