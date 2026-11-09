/*
SurfOS Desktop Model and Drawing
--------------------
File: guicore.c Date: 10/10/26
--------------------
A small immediate-mode desktop, rendered to ordinary RAM by its owning task.
All coordinates here are logical pixels. The shell handles input and presentation.
10/17/26: a window manager (minimize, maximize, resize from the corner, a taskbar
with a clock), the menus as tables (items are disabled where they do not apply),
desktop tiles, Help, and the actions that menus, toolbars, keys and dialogs share
(gui_act, gui_answer). Notepad (guinote.c), Files (guifiles.c), the dialogs
(guidialog.c) and the drawing kit (guidraw.c) have files of their own.
10/17/26 (the suite): the windows from GUI_FIRST_APP on are applications in files of
their own (struct gui_app, guipriv.h), reached through the table below: their menu
items in Apps, their tiles, Help's lines, keys, the pointer, gui_tick() and
gui_finish(). The desktop's own windows keep their code as it was.
10/17/26 (the suite, track MEDIA): Sketch saves its canvas as a BMP and opens one
(Save as BMP, Ctrl+S, and Open BMP in its File menu and its window; shell/guibmp.c).
10/25/26 (programs' windows, docs/superpowers/plans/2026-10-25-app-windows.md): the window
manager's loops run over GUI_SLOTS, the fixed windows and then the programs' slots; the
suite's loops stop at GUI_FIRST_CLIENT, and a program's slot is opened only by its window.
Where the window manager asks an application something it asks a program's window too
(shell/guiclient.c): its title, icon, limits, keys, pointer and wheel, closing, drawing, the
Window menu, the tick and the end; leaving the desktop with programs' windows open asks first.
10/25/26 (stage C6): the Calculator's arithmetic is shell/guicalc.c's (the program wcalc shares it),
and Apps > Run program... (the programs of /initrd/bin in a list, and the line to run: guidialog.c's
GUI_DLG_RUN) starts one on the desktop's console (struct gui_sys's run).
10/26/26 (stage C8): a turn of the wheel that moves nothing else (the mice send it in a report of its
own) is only the wheel's: a program's window under the pointer gets WIN_EV_WHEEL and no pointer event
after it, so the wheel is its last event. A right-click opens a menu where it was (context_press()):
over a window, brought to the front, the menu of what it does there (Edit's for Notepad and the
suite's applications that edit, File's for Files, Window's on a title bar and for the rest); over the
desktop, Apps. It is the menu bar's own menu, chosen and run as from the bar (menu_hit(), menu_run(),
the keys); a program's content keeps every button for its program. It is for its window: it closes
when the focus leaves that window (a program's window arriving with the focus, the window gone), and
an item is never run on another. With a menu open, a right press in it does nothing and one
elsewhere only closes it, as a left click does. In Files a right-click selects the row first.
10/26/26 (stage C12): an application of the suite may be a program now (struct gui_app's program:
the Clock, user/wclock/). Its slot never opens: gui_open() runs the program through the launcher,
or brings its window to the front when the program it started last has one (app_program(); a
press before that window came, within GUI_START_MS, starts nothing: a double-click), and
guiclient.c puts that window where the slot's stands. Edit > Copy for a program's window is its
Ctrl+C.
11/05/26 (the desktop applications, docs/superpowers/plans/2026-11-05-desktop-apps.md, track desk).
The menu bar is a list (bar()): the desktop's six menus, or, while a program's window with menus of
its own has the focus, "≋ SurfOS", its menus in place of File and Edit (its Help's items first in
the desktop's Help), then Apps, Window and Help. Items open menus of their own in their parent's
place (Left goes back): Apps > Programs, the registry's programs (shell/guireg.c), Files' File >
Open with, every program that takes the selected file, and a program's submenus (MENU_CLIENT_SUB).
A menu taller than the room below it scrolls (the arrows, the wheel, strips of arrows at its ends),
as Apps does on a 640x400 screen once Programs is in it. The registry's
programs with desktop = yes have tiles after the suite's, and the windows of programs the desktop
started from the registry have their icons. SurfOS > Screenshot (and Print Screen, shell/gui.c):
the frame is captured, then the Save as chooser asks where, in the first writable volume mounted,
and the picture is saved through struct gui_fs's write, Notepad's care included. Pointer & desktop
keeps its theme and speed in the settings on the stick (struct gui_sys's pref_set), and the desktop
reads them each second (prefs()), so that a change made by Settings applies at once. Keys come as
full keys (sys/gui.h: gui_byte_key(), gui_sequence_key()); a program's window gets them with their
modifiers (guiclient.c), the desktop's own windows as before (gui_own_key()), the Terminal an Alt
key as Esc and the key.
*/
#include "guipriv.h"
#include "gui_media.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

const u32 sketch_paints[SKETCH_PAINTS] = { PAPER, INK, TEAL, CORAL, 0x659ECB, 0xD0B056 };   /* 10/17/26: guibmp.c's too */
static const char *const titles[] = { "Calculator", "Sketch", "About SurfOS", "Pointer & desktop", "Notepad", "Files", "Help" };
static const int app_icons[] = { ICON_CALC, ICON_SKETCH, ICON_WAVE, ICON_GEAR, ICON_NOTEPAD, ICON_FILES, ICON_HELP };
static const char keys[] = "789/456*123-C0=+";
static const char *const menu_names[] = { "\xF7 SurfOS", "File", "Edit", "Apps", "Window", "Help" };
#define MENUS 6
#define MENU_MAX 48                     /* 11/05/26: 32 before: Open with lists the registry's and three */
static const struct { const char *label; int app, icon; } tiles[] = {
    { "Notepad", GUI_NOTEPAD, ICON_NOTEPAD }, { "Files", GUI_FILES, ICON_FILES },
    { "Calculator", GUI_CALC, ICON_CALC }, { "Sketch", GUI_SKETCH, ICON_SKETCH },
};
#define TILES (int)(sizeof(tiles) / sizeof(tiles[0]))
/* 10/17/26 (the suite): the applications in files of their own, by window number from GUI_FIRST_APP */
static const struct gui_app *const suite[GUI_WINDOWS - GUI_FIRST_APP] = {
    &gui_app_terminal, &gui_app_monitor, &gui_app_disks, &gui_app_log, &gui_app_clock, &gui_app_viewer, &gui_app_hex,
    &gui_app_charmap,
};
#define SUITE (GUI_WINDOWS - GUI_FIRST_APP)
#define TILE_ROWS 4                     /* tiles a column holds: the first column is the desktop's own four */

const struct gui_app *gui_app_of(int id) { return id >= GUI_FIRST_APP && id < GUI_WINDOWS ? suite[id - GUI_FIRST_APP] : NULL; }

/* a window's title and icon, whoever it is */
static void title_of(const struct gui_state *g, int id, char *out, size_t n) {
    const struct gui_app *a = gui_app_of(id);
    if(id >= GUI_FIRST_CLIENT) strlcpy(out, client_title(g, id), n);      /* 10/25/26: a program's window */
    else if(id == GUI_NOTEPAD) note_title(g, out, n);
    else if(a && a->title_of) a->title_of(g, out, n);
    else strlcpy(out, a ? a->title : titles[id], n);
}

/* the icon in a window's title bar and its taskbar button; 10/26/26 (stage C12): an application's program's window
   has the application's icon */
int gui_window_icon(const struct gui_state *g, int id) {
    const struct gui_app *a = gui_app_of(id);
    int slot;
    if(id >= GUI_FIRST_CLIENT) {
        slot = client_at(g, id) ? gui_app_slot_of(g, client_at(g, id)->st.pid) : -1;
        return slot >= 0 ? gui_app_of(slot)->icon : ICON_PROGRAM;
    }
    return a ? a->icon : app_icons[id];
}

/* the suite's tile k on the desktop: columns after the desktop's own four */
static void tile_box(int k, int *x, int *y) {
    *x = 24 + (1 + k / TILE_ROWS) * 152;
    *y = 120 + (k % TILE_ROWS) * 52;
}
static const char *const help[] = {
    "#The desktop",
    "Ctrl+E Notepad      Ctrl+B Files      Ctrl+K Calculator",
    "Ctrl+D Sketch       Ctrl+P Pointer & desktop",
    "Ctrl+O opens a file in Notepad; Ctrl+W closes a window.",
    "Ctrl+G opens the menus: the arrows and Enter choose.",
    "Tab goes to the next window (in Notepad it is a tab).",
    "Esc closes a menu, a dialog or a window (not Notepad);",
    "with no window open at all it returns to the shell.",
    "Ctrl+Q returns to the shell, asking first about changes",
    "Notepad has not saved. F1-F4 switch consoles; the",
    "desktop waits on its own and comes back on it.",
    "",
    "#Windows",
    "Drag a title bar to move a window. A window with a",
    "dotted corner also fills the screen (double-click the",
    "title bar, or its middle button) and resizes from it.",
    "The taskbar shows each open window: click one to bring",
    "it up, or to tuck it away when it is in front.",
    "A right-click opens the menu for what is under the",
    "pointer there: Edit, File, Window, or Apps.",
    "",
    "#Notepad",
    "Ctrl+N new, Ctrl+O open, Ctrl+S save. Save as is in the",
    "File menu. Ctrl+Z undo, Ctrl+Y redo; Ctrl+X, Ctrl+C and",
    "Ctrl+V cut, copy and paste; Ctrl+A selects everything.",
    "Ctrl+F finds (Enter finds the next one), Ctrl+L goes to",
    "a line. Drag to select; double-click selects a word.",
    "Files of up to 256 KB. Their line ends stay as they are:",
    "LF, or CRLF when every line has one.",
    "",
    "#Files",
    "The folders from / down. Double-click, or Enter, opens a",
    "folder, or a file: a .bmp in the Image Viewer, text in",
    "Notepad, any other file in Hex. Right and Left open and",
    "close folders, Ctrl+R reads them again, Ctrl+C copies",
    "the path. The File menu has all of Files' commands. A",
    "USB stick that is not mounted is listed at the top with",
    "Mount; Eject it before you pull it out. Delete asks",
    "first, and deletes a file or an empty folder only.",
};
#define HELP_OWN (int)(sizeof(help) / sizeof(help[0]))

/* 10/17/26: Help's lines: the desktop's, then each application's of the suite, a blank line before each */
static int help_count(void) {
    int n = HELP_OWN, i;
    for(i = 0; i < SUITE; i++) if(suite[i]->help_lines) n += 1 + suite[i]->help_lines;
    return n;
}

static const char *help_line(int line) {
    int i;
    if(line < HELP_OWN) return help[line];
    line -= HELP_OWN;
    for(i = 0; i < SUITE; i++) {
        if(!suite[i]->help_lines) continue;
        if(line == 0) return "";
        if(line <= suite[i]->help_lines) return suite[i]->help[line - 1];
        line -= 1 + suite[i]->help_lines;
    }
    return "";
}
#define HELP_LINES help_count()

int gui_clamp(int n, int a, int b) { return n < a ? a : n > b ? b : n; }
bool gui_inside(int x, int y, int l, int t, int w, int h) { return x >= l && y >= t && x < l + w && y < t + h; }
static bool resizable(const struct gui_state *g, int id) {
    if(id >= GUI_FIRST_CLIENT) return client_resizable(g, id);                /* 10/25/26: as its program asked */
    return id == GUI_NOTEPAD || id == GUI_FILES || id == GUI_HELP || (gui_app_of(id) && gui_app_of(id)->resizable);
}
/* a resizable window is never narrower than its toolbar */
static int min_width(const struct gui_state *g, int id) {
    const struct gui_app *a = gui_app_of(id);
    if(id >= GUI_FIRST_CLIENT) return client_min_width(g, id);
    if(a) return a->min_width ? a->min_width() : 360;
    return id == GUI_NOTEPAD ? note_min_width() : id == GUI_FILES ? files_min_width() : 360;
}
/* 10/25/26: nor lower than 200, or than a program's window may be */
static int min_height(const struct gui_state *g, int id) {
    return id >= GUI_FIRST_CLIENT ? client_min_height(g, id) : 200;
}

/**** small things the desktop's files share ****/

/* false when the path is too long for out: what is there then must not be used */
bool gui_join(char *out, size_t n, const char *dir, const char *name) {
    char d[GUI_PATH_MAX];
    strlcpy(d, dir, sizeof(d));                         /* out may be dir */
    return snprintf(out, n, "%s%s%s", d, d[0] && d[strlen(d) - 1] == '/' ? "" : "/", name) < (int)n;
}

const char *gui_basename(const char *path) {
    const char *s = strrchr(path, '/');
    return s && s[1] ? s + 1 : path;
}

static char fold(char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* path is name, or a folder name is in (FAT's names ignore case) */
static bool covers(const char *path, const char *name) {
    const char *p = path;
    while(*p && fold(*p) == fold(*name)) { p++; name++; }
    return !*p && (!*name || *name == '/' || (p > path && p[-1] == '/'));
}

/* path is one of the pairs remembered (1), a folder one is in (2), or neither (0) */
static int remembered(const struct gui_state *g, const char *path) {
    int i, r = 0;
    for(i = 0; i < GUI_UNSURE && i < g->nunsure; i++) {
        if(covers(path, g->unsure[i]) || covers(path, g->kept[i])) r = 2;
        if(covers(g->unsure[i], path) || covers(g->kept[i], path)) return 1;   /* both ways: the very name */
    }
    return r;
}

bool gui_unsure(const struct gui_state *g, const char *path) {
    return g->nunsure > GUI_UNSURE || remembered(g, path);     /* more than it keeps: anything */
}

bool gui_remembered(const struct gui_state *g, const char *path) {
    return remembered(g, path) == 1;
}

void gui_unsure_line(const struct gui_state *g, const char *path, char *line, size_t n) {
    char name[GUI_PATH_MAX];
    int r = remembered(g, path);
    if(!r) { strlcpy(line, "Saves may have failed half way: check the disk.", n); return; }
    gui_middle(gui_basename(path), name, sizeof(name), r == 1 ? 32 : 23);      /* 54 cells in all */
    snprintf(line, n, r == 1 ? "%s may share its data." : "A file in %s may share its data.", name);
}

void gui_parent(char *path) {
    char *s = strrchr(path, '/');
    if(!s) return;
    if(s == path) path[1] = 0;
    else *s = 0;
}

/* seconds since 1970 as a date (the days to a civil date: H. Hinnant's method, in 32 bits) */
void gui_date(u32 t, char *out, size_t n) {
    u32 z = t / 86400 + 719468, era = z / 146097, doe = z - era * 146097;
    u32 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    u32 mp = (5 * doy + 2) / 153, day = doy - (153 * mp + 2) / 5 + 1, month = mp < 10 ? mp + 3 : mp - 9;
    u32 year = yoe + era * 400 + (month <= 2), s = t % 86400;
    snprintf(out, n, "%04u-%02u-%02u %02u:%02u", year, month, day, s / 3600, s / 60 % 60);
}

void gui_bytes(u32 n, char *out, size_t size) {
    u32 tenths;
    if(n < 1024) { snprintf(out, size, "%u byte%s", n, n == 1 ? "" : "s"); return; }
    tenths = n < 1048576 ? n * 10 / 1024 : n / 1024 * 10 / 1024;
    if(tenths < 100) snprintf(out, size, "%u.%u %s", tenths / 10, tenths % 10, n < 1048576 ? "KB" : "MB");
    else snprintf(out, size, "%u %s", tenths / 10, n < 1048576 ? "KB" : "MB");
}

bool gui_double(u32 *last_ms, int *last_row, u32 now, int row) {
    if(row == *last_row && now - *last_ms <= DOUBLE_MS) { *last_row = -1; return true; }
    *last_ms = now;
    *last_row = row;
    return false;
}

const char *gui_error(const struct gui_state *g, int err) {
    return g->fs && g->fs->error ? g->fs->error(err) : "failed";
}

/* a toolbar: left to right below the title bar */
static int tool_width(const struct gui_tool *t) { return (int)strlen(t->label) * 8 + 14; }

int tool_span(const struct gui_tool *t, int n) {
    int i, at = 8;
    for(i = 0; i < n; i++) at += tool_width(&t[i]) + 4;
    return at + 4;
}

int tool_hit(const struct gui_tool *t, int n, int x, int y) {
    int i, at = 8;
    if(y < TITLE_H + 4 || y >= TITLE_H + 4 + 22) return -1;
    for(i = 0; i < n; at += tool_width(&t[i]) + 4, i++) if(x >= at && x < at + tool_width(&t[i])) return i;
    return -1;
}

void tool_render(const struct gui_draw *d, int x, int y, const struct gui_tool *t, int n, const bool *on) {
    int i, at = x + 8;
    for(i = 0; i < n; at += tool_width(&t[i]) + 4, i++)
        gd_button(d, at, y + TITLE_H + 4, tool_width(&t[i]), 22, t[i].label, on[i] ? SHELL : BAND, on[i] ? INK : MUTED);
}

/**** windows ****/

static void refocus(struct gui_state *g) {
    int i;
    g->focus = -1;
    for(i = GUI_SLOTS - 1; i >= 0; i--) {
        if(g->win[g->order[i]].open && !g->win[g->order[i]].min) { g->focus = g->order[i]; break; }
    }
    g->dirty = true;
}

/* 10/25/26 (the review): a window in front of the others, the focus left where it is (a program's new window) */
void gui_raise(struct gui_state *g, int id) {
    int i,j;
    for(i=0;i<GUI_SLOTS;i++) if(g->order[i]==id) {
        for(j=i;j<GUI_SLOTS-1;j++) g->order[j]=g->order[j+1];
        g->order[GUI_SLOTS-1]=id; break;
    }
    g->dirty=true;
}

static void app_program(struct gui_state *g, int app);

void gui_open(struct gui_state *g, int app) {
    bool was;
    if(app<0 || app>=GUI_SLOTS) return;
    if(gui_app_of(app) && gui_app_of(app)->program) { app_program(g, app); return; }  /* 10/26/26 (C12) */
    if(app>=GUI_FIRST_CLIENT && !g->win[app].open) return;     /* 10/25/26: a program's slot opens only with its window */
    was = g->win[app].open;
    gui_raise(g, app);
    g->win[app].open=true; g->win[app].min=false; g->focus=app; g->menu=0; g->dirty=true;
    if(!was && app == GUI_FILES) files_open(g);
    if(!was && gui_app_of(app) && gui_app_of(app)->opened) gui_app_of(app)->opened(g);
}

void gui_close_now(struct gui_state *g, int id) {
    const struct gui_app *a = gui_app_of(id);
    g->win[id].open = g->win[id].min = false;
    if(id == GUI_NOTEPAD) note_reset(&g->note);
    if(a && a->closed) a->closed(g);
    if(g->app_drag == id) g->app_drag = -1;
    refocus(g);
}

/* Notepad's document goes with its window, after the question about its changes; 10/17/26: an application that is
   busy (a Terminal's session) is asked about first */
void gui_close(struct gui_state *g, int id) {
    const struct gui_app *a;
    char why[80];
    if(id < 0 || id >= GUI_SLOTS || !g->win[id].open) return;
    if(id >= GUI_FIRST_CLIENT) { client_close(g, id); return; }          /* 10/25/26: CLOSE, then "End the program?" */
    if(id == GUI_NOTEPAD && !gui_guard(g, PEND_CLOSE, NULL)) return;
    a = gui_app_of(id);
    if(a && a->busy && a->busy(g, why, sizeof(why))) {
        dialog_confirm(g, gui_app_act(id, GUI_APP_CLOSE), "Close the window?", why, "It ends when the window closes.",
                       "Close", NULL, "Cancel");
        return;
    }
    gui_close_now(g, id);
}

static void minimize(struct gui_state *g, int id) {
    if(id < 0 || !g->win[id].open) return;
    if(g->app_drag == id) gui_release(g);       /* 10/18/26: a drag in it ends with it out of sight */
    g->win[id].min = true;
    refocus(g);
}

static void maximize(struct gui_state *g, int id) {
    struct gui_window *w;
    if(id < 0 || !resizable(g, id)) return;
    w = &g->win[id];                            /* 10/18/26: after the check (no window in front: id -1) */
    if(w->max) { w->max = false; w->x = w->nx; w->y = w->ny; w->w = w->nw; w->h = w->nh; }
    else { w->nx = w->x; w->ny = w->y; w->nw = w->w; w->nh = w->h; w->max = true; }
    gui_place(g, id);
    g->dirty = true;
}

/* 10/25/26: the highest a window's top may be: 6 pixels below the menu bar; for a program's window too tall for that
   (one as large as can be) less, so that it still ends above the taskbar (the desktop's own windows as before) */
static int top_limit(const struct gui_state *g, int id, int h) {
    if(id < GUI_FIRST_CLIENT) return MENU_H + 6;
    return MENU_H + 6 - gui_clamp(h - (g->height - MENU_H - TASK_H - 8), 0, 6);
}

void gui_place(struct gui_state *g, int id) {
    struct gui_window *w = &g->win[id];
    int room = g->height - MENU_H - TASK_H, top, bottom;
    if(w->max) {
        w->x = 0; w->y = MENU_H; w->w = g->width; w->h = room;
        if(id >= GUI_FIRST_CLIENT) client_limit(g, id, &w->w, &w->h);   /* 10/25/26: no larger than its program allows */
        return;
    }
    if(w->w > g->width) w->w = g->width;
    if(w->h > room) w->h = room;
    top = top_limit(g, id, w->h);
    bottom = g->height - TASK_H - 2 - w->h;             /* as a drag leaves it */
    w->x = gui_clamp(w->x, 0, g->width - w->w);
    w->y = gui_clamp(w->y, top, bottom > top ? bottom : top);
}

void gui_init(struct gui_state *g, int width, int height) {
    int i, room = height - MENU_H - TASK_H, w, h;
    memset(g,0,sizeof(*g)); g->width=width; g->height=height;
    g->focus=g->drag=g->resize=g->scroll=g->app_drag=-1; g->brush=1; g->speed=1;
    g->title_id=g->files.click_row=g->dialog.click_row=-1;
    g->clients.grab=g->clients.hover=-1;                /* 10/25/26 */
    g->theme_given=g->pref_theme=g->pref_speed=-1;      /* 11/05/26: told nothing, read nothing yet */
    g->x=width/2; g->y=height/2;
    for(i=0;i<GUI_SLOTS;i++) g->order[i]=i;
    g->win[GUI_CALC]=(struct gui_window){width-272,50,248,300};
    g->win[GUI_SKETCH]=(struct gui_window){(width-432)/2,64,432,294};
    g->win[GUI_ABOUT]=(struct gui_window){(width-432)/2,(height-234)/2,432,234};
    g->win[GUI_SETTINGS]=(struct gui_window){(width-400)/2,72,400,244};
    w = width - 48 < 640 ? width - 48 : 640; h = room - 20 < 420 ? room - 20 : 420;
    g->win[GUI_NOTEPAD]=(struct gui_window){width-w-24,MENU_H+10,w,h};
    w = width - 48 < 600 ? width - 48 : 600; h = room - 30 < 380 ? room - 30 : 380;
    g->win[GUI_FILES]=(struct gui_window){24,MENU_H+20,w,h};
    w = width - 48 < 540 ? width - 48 : 540; h = room - 20 < 380 ? room - 20 : 380;
    g->win[GUI_HELP]=(struct gui_window){(width-w)/2,MENU_H+10,w,h};
    for(i=0;i<SUITE;i++) {                              /* 10/17/26: the suite's, cascading from the top left */
        w = suite[i]->w < width - 48 ? suite[i]->w : width - 48;
        h = suite[i]->h < room - 20 ? suite[i]->h : room - 20;
        g->win[GUI_FIRST_APP+i]=(struct gui_window){48+i*24,MENU_H+12+i*14,w,h};
    }
    for(i=0;i<GUI_WINDOWS;i++) gui_place(g,i);
    g->term.handle = -1;
    note_reset(&g->note);
    gui_open(g,GUI_ABOUT);
}

/**** the menus ****/

/* 11/05/26: mark: a program's checked item ('v') or one of a group ('o'); sub: it opens a menu of its own */
struct menu_item { const char *label, *keys; int act; bool on, sub; char mark; };

/* 10/17/26: the focused application, when the Edit menu is its own */
static const struct gui_app *edit_app(const struct gui_state *g) {
    const struct gui_app *app = g->focus >= 0 && !g->win[g->focus].min ? gui_app_of(g->focus) : NULL;
    return app && app->can_edit && app->edit ? app : NULL;
}

/* 10/17/26: room for one more item in a menu of count items above the taskbar (the separators: counted as items) */
static bool item_room(const struct gui_state *g, int count) {
    return MENU_H + 4 + (count + 1) * 24 + 4 <= g->height - TASK_H && count < MENU_MAX;
}

/* 11/05/26 (track desk): the focused program's menus in the bar: the entry of its k-th title that is not its Help
   (merged into the desktop's Help instead, surfos/win.h's WIN_MENU_HELP), -1; its Help title's entry, -1 */
static int client_title_entry(const struct gui_state *g, int k, const struct win_menu_item **mp, int *count) {
    const struct win_menu_item *m = client_menu(g, count);
    int i;
    *mp = m;
    for(i = 0; m && i < *count; i++)
        if((m[i].flags & WIN_MENU_TITLE) && !(m[i].flags & WIN_MENU_HELP) && k-- == 0) return i;
    return -1;
}

static int client_help_entry(const struct gui_state *g, const struct win_menu_item **mp, int *count) {
    const struct win_menu_item *m = client_menu(g, count);
    int i;
    *mp = m;
    for(i = 0; m && i < *count; i++) if((m[i].flags & WIN_MENU_TITLE) && (m[i].flags & WIN_MENU_HELP)) return i;
    return -1;
}

/* the menu bar's menus, in order: the desktop's six, or, while a program's window with menus of its own in the bar
   has the focus, "≋ SurfOS", its menus (MENU_CLIENT + k) in place of File and Edit, then Apps, Window and Help. The
   menus an item opens (Apps > Programs, File > Open with, a program's submenu) stand in their parent's place */
#define BAR_MAX (3 + WIN_MENUS + 3)
static int bar(const struct gui_state *g, int *out) {
    const struct win_menu_item *m;
    int n = 0, k = 0, count;
    out[n++] = 1;
    while(k < WIN_MENUS && client_title_entry(g, k, &m, &count) >= 0) out[n++] = MENU_CLIENT + k++;
    if(!k) {
        out[n++] = 2;
        out[n++] = 3;
    }
    out[n++] = 4;
    out[n++] = 5;
    out[n++] = 6;
    return n;
}

static int bar_parent(const struct gui_state *g, int m) {
    return m == MENU_PROGRAMS ? 4 : m == MENU_OPEN_WITH ? 2 : m == MENU_CLIENT_SUB ? g->menu_sub_of : m;
}

/* menu m's name in the bar */
static const char *menu_name(const struct gui_state *g, int m) {
    const struct win_menu_item *c;
    int count, t;
    if(m >= MENU_CLIENT) {
        t = client_title_entry(g, m - MENU_CLIENT, &c, &count);
        return t >= 0 ? c[t].label : "";
    }
    return m >= 1 && m <= MENUS ? menu_names[m - 1] : "";
}

/* a program's menu that is greyed whole */
static bool menu_greyed(const struct gui_state *g, int m) {
    const struct win_menu_item *c;
    int count, t = m >= MENU_CLIENT ? client_title_entry(g, m - MENU_CLIENT, &c, &count) : -1;
    return t >= 0 && (c[t].flags & WIN_MENU_DISABLED);
}

static int title_w(const struct gui_state *g, int m) { return gd_cells(menu_name(g, m)) * 8 + 24; }

#define ITEM(l, k, a, e) do { if(count < MENU_MAX) { it[count].label = (l); it[count].keys = (k); it[count].act = (a); \
                              it[count].on = (e); it[count].sub = false; it[count].mark = 0; count++; } } while(0)
#define SEPARATOR() do { if(count < MENU_MAX) { it[count].label = NULL; it[count].keys = ""; it[count].act = ACT_NONE; \
                         it[count].on = false; it[count].sub = false; it[count].mark = 0; count++; } } while(0)

/* 11/05/26: the registry's programs that show in Apps > Programs (the desktop's own applications are in Apps) */
static int program_items(const struct gui_state *g, struct menu_item *it, int count) {
    int i, k;
    for(i = 0; i < g->reg.count; i++) {
        k = g->reg.by[i];
        if(g->reg.e[k].apps && g->reg.e[k].app < 0) ITEM(g->reg.e[k].title, "", ACT_REG_START + k, true);
    }
    return count;
}

/* 11/05/26: how many Apps > Programs lists */
static int programs_count(const struct gui_state *g) {
    int i, n = 0;
    for(i = 0; i < g->reg.count; i++) if(g->reg.e[i].apps && g->reg.e[i].app < 0) n++;
    return n;
}

/* 11/05/26: Files' selected file, its name; NULL when no file is selected */
static const char *files_file(const struct gui_state *g) {
    const struct gui_files *f = &g->files;
    return g->focus == GUI_FILES && f->selected > 0 && f->selected < f->count && f->node[f->selected].type == GUI_FILE ?
           f->node[f->selected].name : NULL;
}

/* 11/05/26: File > Open with: every program that takes the selected file (its types or open-with name its type),
   then the desktop's own: the Image Viewer for what it reads (but when a program listed is the one it starts),
   Notepad and Hex */
static int open_with_items(const struct gui_state *g, struct menu_item *it, int count) {
    const char *name = files_file(g);
    bool viewer = name && viewer_takes(name);
    int i, k, listed = 0;
    for(i = 0; name && i < g->reg.count; i++) {
        k = g->reg.by[i];
        if(!reg_takes(&g->reg.e[k], name, true)) continue;
        if(g->reg.e[k].app == GUI_VIEWER) viewer = false;
        ITEM(g->reg.e[k].title, reg_takes(&g->reg.e[k], name, false) ? "Enter" : "", ACT_REG_OPEN + k, true);
        listed++;
    }
    if(listed) SEPARATOR();
    if(viewer) ITEM(gui_app_of(GUI_VIEWER)->title, "", ACT_WITH_VIEWER, true);
    ITEM("Notepad", "", ACT_WITH_NOTEPAD, name != NULL);
    ITEM("Hex", "", ACT_WITH_HEX, name != NULL);
    return count;
}

/* 11/05/26: a program's entries from first on, as menu items: its items and separators, as it set them, to the next
   title (children: to the first entry that is no WIN_MENU_CHILD; else past the children of a SUBMENU item, which
   opens its submenu); greyed with the whole menu when its title (or the SUBMENU item, for children) is */
static int client_entries(const struct win_menu_item *m, int n, int first, bool children, bool whole,
                          struct menu_item *it, int count) {
    int i;
    for(i = first; i < n && !(m[i].flags & WIN_MENU_TITLE); i++) {
        const struct win_menu_item *e = &m[i];
        if(!(e->flags & WIN_MENU_CHILD) != !children) {
            if(children) break;
            continue;
        }
        if(e->flags & WIN_MENU_SEPARATOR) { SEPARATOR(); continue; }
        ITEM(e->label, e->keys, ACT_CLIENT_MENU + i, whole && !(e->flags & WIN_MENU_DISABLED));
        if(count && it[count - 1].act == ACT_CLIENT_MENU + i) {
            it[count - 1].mark = (e->flags & WIN_MENU_RADIO) ? 'o' : (e->flags & WIN_MENU_CHECKED) ? 'v' : 0;
            it[count - 1].sub = (e->flags & WIN_MENU_SUBMENU) != 0;
        }
    }
    return count;
}

/* the focused program's k-th menu in the bar */
static int client_items(const struct gui_state *g, int k, struct menu_item *it, int count) {
    const struct win_menu_item *m;
    int n, t = client_title_entry(g, k, &m, &n);
    return t < 0 ? count : client_entries(m, n, t + 1, false, !(m[t].flags & WIN_MENU_DISABLED), it, count);
}

/* the submenu open now (MENU_CLIENT_SUB): the children of the program's entry g->menu_sub, when that is still an
   item with one */
static int client_sub_items(const struct gui_state *g, struct menu_item *it, int count) {
    const struct win_menu_item *m = NULL;
    int n = 0, i, title = -1;
    if(g->menu_sub_of == 6) client_help_entry(g, &m, &n);
    else if(g->menu_sub_of >= MENU_CLIENT) client_title_entry(g, g->menu_sub_of - MENU_CLIENT, &m, &n);
    if(!m || g->menu_sub < 0 || g->menu_sub >= n || !(m[g->menu_sub].flags & WIN_MENU_SUBMENU)) return count;
    for(i = 0; i < g->menu_sub; i++) if(m[i].flags & WIN_MENU_TITLE) title = i;
    if(title < 0) return count;
    return client_entries(m, n, g->menu_sub + 1, true,
                          !(m[title].flags & WIN_MENU_DISABLED) && !(m[g->menu_sub].flags & WIN_MENU_DISABLED), it, count);
}

static int menu_items(const struct gui_state *g, int m, struct menu_item *it) {
    const struct gui_note *n = &g->note;
    bool note = g->focus == GUI_NOTEPAD, sel = note && note_has_selection(n), files = g->fs != NULL, any = false;
    int count = 0, i, id;
    for(i = 0; i < GUI_SLOTS; i++) if(g->win[i].open) any = true;
    if(m == MENU_PROGRAMS) return program_items(g, it, 0);         /* 11/05/26 */
    if(m == MENU_OPEN_WITH) return open_with_items(g, it, 0);
    if(m == MENU_CLIENT_SUB) return client_sub_items(g, it, 0);
    if(m >= MENU_CLIENT) return client_items(g, m - MENU_CLIENT, it, 0);
    switch(m) {
    case 1:
        ITEM("About SurfOS", "", ACT_ABOUT, true);
        ITEM("Pointer & desktop", "Ctrl+P", ACT_SETTINGS, true);
        ITEM("Ocean / dusk colors", "", ACT_THEME, true);
        ITEM("Screenshot", "PrtSc", ACT_SCREENSHOT, files);    /* 11/05/26: for keyboards without the key */
        SEPARATOR();
        ITEM("Return to shell", "Ctrl+Q", ACT_QUIT, true);
        break;
    case 2:
        if(g->focus == GUI_FILES) {                     /* Files in front: its commands, for the keyboard too */
            ITEM("Open selected", "Enter", ACT_FILES_OPEN, files_can(g, ACT_FILES_OPEN));
            ITEM("Open with", "", ACT_OPEN_WITH, files_file(g) != NULL);     /* 11/05/26: a menu of its own */
            if(count) it[count - 1].sub = true;
            ITEM("New file\xF9", "", ACT_FILES_NEW_FILE, files_can(g, ACT_FILES_NEW_FILE));
            ITEM("New folder\xF9", "", ACT_FILES_NEW_FOLDER, files_can(g, ACT_FILES_NEW_FOLDER));
            ITEM("Rename\xF9", "", ACT_FILES_RENAME, files_can(g, ACT_FILES_RENAME));
            ITEM("Delete\xF9", "Del", ACT_FILES_DELETE, files_can(g, ACT_FILES_DELETE));
            ITEM("Copy path", "Ctrl+C", ACT_FILES_COPY, files_can(g, ACT_FILES_COPY));
            SEPARATOR();
            ITEM("Mount", "", ACT_FILES_MOUNT, files_can(g, ACT_FILES_MOUNT));
            ITEM("Eject", "", ACT_FILES_EJECT, files_can(g, ACT_FILES_EJECT));
            ITEM("Refresh", "Ctrl+R", ACT_FILES_REFRESH, files_can(g, ACT_FILES_REFRESH));
            SEPARATOR();
        }
        if(g->focus == GUI_SKETCH) {                    /* 10/17/26: Sketch in front: its pictures (guibmp.c) */
            ITEM("Open BMP\xF9", "", ACT_SKETCH_OPEN, sketch_can_open(g));
            ITEM("Save as BMP\xF9", "Ctrl+S", ACT_SKETCH_SAVE, files);
            SEPARATOR();
        }
        ITEM("New note", "Ctrl+N", ACT_NEW, true);
        ITEM("Open\xF9", "Ctrl+O", ACT_OPEN, files);
        if(g->focus != GUI_FILES && g->focus != GUI_SKETCH) {
            ITEM("Save", "Ctrl+S", ACT_SAVE, note && files);
            ITEM("Save as\xF9", "", ACT_SAVE_AS, note && files);
            SEPARATOR();
            ITEM("Show in Files", "", ACT_REVEAL, note && files && n->path[0]);
        }
        SEPARATOR();
        ITEM("Close window", "Ctrl+W", ACT_CLOSE, g->focus >= 0);
        break;
    case 3:
        if(client_keys(g)) {                            /* 10/26/26 (C12): a program's window: its Ctrl+C */
            ITEM("Copy", "Ctrl+C", ACT_CLIENT_COPY, true);
            break;
        }
        if(edit_app(g)) {                               /* 10/17/26: the focused application's */
            const struct gui_app *app = edit_app(g);
            ITEM("Undo", "Ctrl+Z", ACT_UNDO, app->can_edit(g, ACT_UNDO));
            ITEM("Redo", "Ctrl+Y", ACT_REDO, app->can_edit(g, ACT_REDO));
            SEPARATOR();
            ITEM("Cut", "Ctrl+X", ACT_CUT, app->can_edit(g, ACT_CUT));
            ITEM("Copy", "Ctrl+C", ACT_COPY, app->can_edit(g, ACT_COPY));
            ITEM("Paste", "Ctrl+V", ACT_PASTE, app->can_edit(g, ACT_PASTE));
            ITEM("Delete", "Del", ACT_DELETE, app->can_edit(g, ACT_DELETE));
            SEPARATOR();
            ITEM("Select all", "Ctrl+A", ACT_SELECT_ALL, app->can_edit(g, ACT_SELECT_ALL));
            ITEM("Find\xF9", "Ctrl+F", ACT_FIND, app->can_edit(g, ACT_FIND));
            ITEM("Find next", "", ACT_FIND_NEXT, app->can_edit(g, ACT_FIND_NEXT));
            ITEM(app->goto_label ? app->goto_label : "Go to line\xF9", "Ctrl+L", ACT_GOTO, app->can_edit(g, ACT_GOTO));
            break;
        }
        ITEM("Undo", "Ctrl+Z", ACT_UNDO, note && n->undo_at > 0);
        ITEM("Redo", "Ctrl+Y", ACT_REDO, note && n->undo_at < n->nundo);
        SEPARATOR();
        ITEM("Cut", "Ctrl+X", ACT_CUT, sel);
        ITEM("Copy", "Ctrl+C", ACT_COPY, sel);
        ITEM("Paste", "Ctrl+V", ACT_PASTE, note && g->clip_len);
        ITEM("Delete", "Del", ACT_DELETE, sel);
        SEPARATOR();
        ITEM("Select all", "Ctrl+A", ACT_SELECT_ALL, note);
        ITEM("Find\xF9", "Ctrl+F", ACT_FIND, note);
        ITEM("Find next", "", ACT_FIND_NEXT, note && n->find.len);
        ITEM("Go to line\xF9", "Ctrl+L", ACT_GOTO, note);
        break;
    case 4:
        ITEM("Notepad", "Ctrl+E", ACT_NOTEPAD, true);
        ITEM("Files", "Ctrl+B", ACT_FILES, files);
        ITEM("Calculator", "Ctrl+K", ACT_CALC, true);
        ITEM("Sketch", "Ctrl+D", ACT_SKETCH, true);
        SEPARATOR();
        ITEM("Clear sketch", "", ACT_CLEAR_SKETCH, true);
        SEPARATOR();                                    /* 10/17/26: the suite */
        for(i = 0; i < SUITE; i++) ITEM(suite[i]->title, suite[i]->shortcut, ACT_WINDOW + GUI_FIRST_APP + i, true);
        SEPARATOR();                                    /* 10/25/26: the launcher (the last item a 640x400 screen has */
        ITEM("Run program\xF9", "", ACT_RUN, g->sys && g->sys->run);    /* room for in this menu) */
        if(programs_count(g) > 0) {                     /* 11/05/26: the registry's programs, a menu of their own */
            ITEM("Programs", "", ACT_PROGRAMS, true);   /* (after Run program..., which keeps its place: the menu */
            if(count) it[count - 1].sub = true;         /* scrolls on a 640x400 screen) */
        }
        break;
    case 5:
        ITEM("Next window", note ? "" : "Tab", ACT_NEXT_WINDOW, any);   /* in Notepad, Tab is a tab */
        ITEM("Minimize", "", ACT_MINIMIZE, g->focus >= 0);
        ITEM(g->focus >= 0 && g->win[g->focus].max ? "Restore size" : "Maximize", "", ACT_MAXIMIZE, g->focus >= 0 && resizable(g, g->focus));
        ITEM("Close", "Ctrl+W", ACT_CLOSE, g->focus >= 0);
        if(any) SEPARATOR();
        /* 10/17/26: the open windows, as many as fit above the taskbar (Next window reaches the rest) */
        for(id = 0; id < GUI_SLOTS && item_room(g, count); id++)
            if(g->win[id].open) ITEM(id >= GUI_FIRST_CLIENT ? client_title(g, id) : gui_app_of(id) ? gui_app_of(id)->title : titles[id],
                                     g->focus == id ? "\x10" : g->win[id].min ? "hidden" : "", ACT_WINDOW + id, true);
        break;
    case 6:
        {                                               /* 11/05/26: a program's Help first, then the desktop's */
            const struct win_menu_item *c;
            int cn, t = client_help_entry(g, &c, &cn);
            if(t >= 0) {
                count = client_entries(c, cn, t + 1, false, !(c[t].flags & WIN_MENU_DISABLED), it, count);
                SEPARATOR();
            }
        }
        ITEM("Keyboard and mouse", "", ACT_HELP, true);
        ITEM("About SurfOS", "", ACT_ABOUT, true);
        break;
    }
    return count;
}
#undef ITEM
#undef SEPARATOR

/* the x of menu m's title in the bar (a menu an item opens: its parent's) */
static int menu_left(const struct gui_state *g, int m) {
    int b[BAR_MAX], n = bar(g, b), x = 8, i;
    m = bar_parent(g, m);
    for(i = 0; i < n && b[i] != m; i++) x += title_w(g, b[i]);
    return x;
}

/* the bar's menu whose title is at x, 0 */
static int menu_at(const struct gui_state *g, int x) {
    int b[BAR_MAX], n = bar(g, b), left = 8, i;
    for(i = 0; i < n; i++) {
        if(x >= left - 4 && x < left + title_w(g, b[i]) - 4) return b[i];
        left += title_w(g, b[i]);
    }
    return 0;
}

static int menu_width(const struct menu_item *it, int n) {
    int w = 184, i, k;
    for(i = 0; i < n; i++) {
        if(!it[i].label) continue;
        k = (gd_cells(it[i].label) + gd_cells(it[i].keys)) * 8 + 64 + (it[i].sub ? 16 : 0);
        if(k > w) w = k;
    }
    return w;
}

static int entry_h(const struct menu_item *e) { return e->label ? 24 : 9; }

/* 11/05/26: where an open menu is and which of its entries show: its box x, y, w, h; entries first to end - 1 from
   rows down; a menu taller than the room below its top (the screen's bottom for the bar's, the taskbar for a
   right-click's) shows what fits, under a strip of up arrows while some are hidden above (up) and over one of down
   arrows while some are below (down) */
#define STRIP 14
struct menu_box { int x, y, w, h, first, end, rows; bool up, down; };

static void menu_box_of(const struct gui_state *g, int m, const struct menu_item *it, int n, struct menu_box *b) {
    int full = 8, i, room, limit, at;
    for(i = 0; i < n; i++) full += entry_h(&it[i]);
    b->w = menu_width(it, n);
    b->x = gui_clamp(menu_left(g, m) - 4, 0, g->width - b->w > 0 ? g->width - b->w : 0);
    b->y = MENU_H;
    room = g->height - MENU_H;
    if(m == g->menu && g->menu_y) {                     /* 10/26/26 (stage C8): where a right-click opened it */
        room = g->height - TASK_H - MENU_H;
        b->x = gui_clamp(g->menu_x, 0, g->width - b->w > 0 ? g->width - b->w : 0);
        b->y = gui_clamp(g->menu_y, MENU_H, g->height - TASK_H - full > MENU_H ? g->height - TASK_H - full : MENU_H);
    }
    b->first = 0;
    b->end = n;
    b->up = b->down = false;
    b->h = full;
    b->rows = b->y + 4;
    if(full <= room || n == 0) return;
    b->h = room;
    b->first = m == g->menu ? gui_clamp(g->menu_top, 0, n - 1) : 0;
    b->up = b->first > 0;
    b->rows = b->y + 4 + (b->up ? STRIP : 0);
    for(limit = b->y + b->h - 4, at = b->rows, i = b->first; i < n && at + entry_h(&it[i]) <= limit; i++)
        at += entry_h(&it[i]);
    if(i < n) {                                         /* some below: room for their strip too */
        for(limit -= STRIP, at = b->rows, i = b->first; i < n && at + entry_h(&it[i]) <= limit; i++)
            at += entry_h(&it[i]);
        b->down = true;
    }
    b->end = i > b->first ? i : b->first + 1;
}

/* entry i's top in box b */
static int entry_top(const struct menu_box *b, const struct menu_item *it, int i) {
    int y = b->rows, k;
    for(k = b->first; k < i; k++) y += entry_h(&it[k]);
    return y;
}

/* the entry under x, y of the open menu: its number; -1 for none, -2 and -3 for the up and the down strips */
static int menu_hit(const struct gui_state *g, int x, int y) {
    struct menu_item it[MENU_MAX];
    struct menu_box b;
    int n = menu_items(g, g->menu, it), i;
    menu_box_of(g, g->menu, it, n, &b);
    if(x < b.x || x >= b.x + b.w || y < b.y || y >= b.y + b.h) return -1;
    if(b.up && y < b.rows) return -2;
    if(b.down && y >= b.y + b.h - 4 - STRIP) return -3;
    for(i = b.first; i < b.end; i++)
        if(it[i].label && y >= entry_top(&b, it, i) && y < entry_top(&b, it, i) + 24) return i;
    return -1;
}

/* for the host test: whether menu m has act and it is enabled now; where menu m opens */
bool gui_menu_on(const struct gui_state *g, int m, int act) {
    struct menu_item it[MENU_MAX];
    int n = menu_items(g, m, it), i;
    for(i = 0; i < n; i++) if(it[i].label && it[i].act == act) return it[i].on;
    return false;
}

void gui_menu_box(const struct gui_state *g, int m, int *x, int *y, int *w, int *h) {
    struct menu_item it[MENU_MAX];
    struct menu_box b;
    int n = menu_items(g, m, it);
    menu_box_of(g, m, it, n, &b);                       /* 10/26/26: where a right-click opened it, when it did */
    *x = b.x;
    *y = b.y;
    *w = b.w;
    *h = b.h;
}

static int menu_step(const struct menu_item *it, int n, int from, int step) {
    int k, i = from;
    for(k = 0; k < n; k++) {
        i = (i + step + n) % n;
        if(it[i].label && it[i].on) return i;
    }
    return from;
}

/* 11/05/26: the open menu scrolled so that its selection shows */
static void menu_reveal(struct gui_state *g) {
    struct menu_item it[MENU_MAX];
    struct menu_box b;
    int n = menu_items(g, g->menu, it), k;
    for(k = 0; k < n; k++) {
        menu_box_of(g, g->menu, it, n, &b);
        if(g->selection < b.first) g->menu_top = g->selection;
        else if(g->selection >= b.end && g->menu_top < n - 1) g->menu_top++;
        else break;
    }
}

static void menu_open(struct gui_state *g, int m) {
    struct menu_item it[MENU_MAX];
    int n;
    g->menu = m;
    g->menu_y = 0;                                      /* 10/26/26: under its title (context_press() moves it) */
    g->menu_top = 0;                                    /* 11/05/26 */
    g->selection = 0;
    if(m) {
        n = menu_items(g, m, it);
        g->selection = n && it[0].label && it[0].on ? 0 : menu_step(it, n, 0, 1);
    }
    g->dirty = true;
}

/* 11/05/26: the menu an item opens (Programs, Open with), where its parent menu was (under the bar's title, or where a
   right-click opened it) */
static void menu_child(struct gui_state *g, int child) {
    int x = g->menu_x, y = g->menu_y, win = g->menu_win;
    menu_open(g, child);
    if(y) {
        g->menu_x = x;
        g->menu_y = y;
        g->menu_win = win;
    }
}

/* 10/26/26: a right-click menu whose window no longer has the focus (it acts on the focus); 11/05/26: and a menu the
   bar no longer has (the focus left the program whose menus it showed, or came to one that has its own), or a
   program's that is gone */
static bool menu_stale(const struct gui_state *g) {
    const struct win_menu_item *c;
    struct menu_item it[MENU_MAX];
    int b[BAR_MAX], n, i, count;
    if(g->menu && g->menu_y && g->menu_win >= 0 &&
       (g->focus != g->menu_win || !g->win[g->menu_win].open || g->win[g->menu_win].min)) return true;
    if(g->menu >= MENU_CLIENT && client_title_entry(g, g->menu - MENU_CLIENT, &c, &count) < 0) return true;
    if(g->menu == MENU_CLIENT_SUB && !client_sub_items(g, it, 0)) return true;     /* its item is gone */
    if(g->menu == MENU_OPEN_WITH && g->focus != GUI_FILES) return true;
    if(!g->menu || g->menu_y) return false;
    n = bar(g, b);
    for(i = 0; i < n && b[i] != bar_parent(g, g->menu); i++) ;
    return i == n;
}

static void menu_run(struct gui_state *g, int i) {
    struct menu_item it[MENU_MAX];
    int n = menu_items(g, g->menu, it);
    bool stale = menu_stale(g);                         /* 10/26/26: never on another window */
    if(!stale && i >= 0 && i < n && it[i].label && it[i].on && it[i].sub) {    /* 11/05/26: its menu opens */
        if(it[i].act >= ACT_CLIENT_MENU && it[i].act < ACT_CLIENT_MENU + WIN_MENU_ENTRIES) {
            g->menu_sub = it[i].act - ACT_CLIENT_MENU;  /* a program's submenu, one level deep */
            g->menu_sub_of = g->menu;
            menu_child(g, MENU_CLIENT_SUB);
        } else menu_child(g, it[i].act == ACT_PROGRAMS ? MENU_PROGRAMS : MENU_OPEN_WITH);
        return;
    }
    g->menu = 0;
    g->dirty = true;
    if(!stale && i >= 0 && i < n && it[i].label && it[i].on) gui_act(g, it[i].act);
}

/* 11/05/26: Left and Right: the bar's menu before or after this one's; Left in a menu an item opened goes back to it,
   its item selected */
static void menu_side(struct gui_state *g, int step) {
    struct menu_item it[MENU_MAX];
    int b[BAR_MAX], n = bar(g, b), i, child = g->menu, parent = bar_parent(g, g->menu), k;
    if(child != parent && step < 0) {
        int act = child == MENU_PROGRAMS ? ACT_PROGRAMS : child == MENU_OPEN_WITH ? ACT_OPEN_WITH : ACT_CLIENT_MENU + g->menu_sub;
        menu_child(g, parent);
        k = menu_items(g, parent, it);
        for(i = 0; i < k; i++) if(it[i].sub && it[i].act == act) g->selection = i;
        menu_reveal(g);
        return;
    }
    for(i = 0; i < n && b[i] != parent; i++) ;
    if(i == n) i = 0;
    menu_open(g, b[(i + step + n) % n]);
}

static void menu_key(struct gui_state *g, int key) {
    struct menu_item it[MENU_MAX];
    int n = menu_items(g, g->menu, it);
    if(key == 27) g->menu = 0;
    else if(key == GUI_UP || key == GUI_DOWN) {
        g->selection = menu_step(it, n, g->selection, key == GUI_UP ? -1 : 1);
        menu_reveal(g);
    } else if(key == GUI_RIGHT && g->selection >= 0 && g->selection < n && it[g->selection].sub && it[g->selection].on)
        menu_run(g, g->selection);                      /* 11/05/26: into the menu it opens */
    else if(key == GUI_LEFT || key == GUI_RIGHT) menu_side(g, key == GUI_RIGHT ? 1 : -1);
    else if(key == '\r' || key == '\n' || key == ' ') { menu_run(g, g->selection); return; }
    g->dirty = true;
}

/* 11/05/26: the wheel over an open menu that scrolls; and a press on its strips */
static void menu_scroll(struct gui_state *g, int step) {
    struct menu_item it[MENU_MAX];
    struct menu_box b;
    int n = menu_items(g, g->menu, it);
    menu_box_of(g, g->menu, it, n, &b);
    if(!b.up && !b.down) return;
    if((step < 0 && b.up) || (step > 0 && b.down)) g->menu_top = gui_clamp(b.first + step, 0, n - 1);
    g->dirty = true;
}

/**** what menus, toolbars, keys and dialogs do ****/

static void chooser_dir(const struct gui_state *g, char *out, size_t n) {
    if(g->note.path[0]) { strlcpy(out, g->note.path, n); gui_parent(out); }
    else strlcpy(out, g->dialog.dir[0] ? g->dialog.dir : "/", n);
}

static void save_as(struct gui_state *g) {
    char dir[GUI_PATH_MAX];
    if(!g->fs) return;
    chooser_dir(g, dir, sizeof(dir));
    gui_open(g, GUI_NOTEPAD);
    dialog_chooser(g, GUI_DLG_SAVE, ACT_DLG_SAVE_AS, dir, g->note.path[0] ? gui_basename(g->note.path) : "untitled.txt");
}

static void save_now(struct gui_state *g, const char *path) {
    if(note_save(g, path) != 0) { g->pending = PEND_NONE; return; }
    if(g->win[GUI_FILES].open) files_refresh(g);
    if(g->pending) gui_proceed(g);
}

static void save(struct gui_state *g) {
    char line[80];
    if(!g->fs) { g->pending = PEND_NONE; return; }
    if(!g->note.path[0]) save_as(g);
    else if(g->note.readonly) {
        snprintf(line, sizeof(line), "%s is on a volume that cannot be written.", gui_basename(g->note.path));
        dialog_confirm(g, ACT_DLG_READONLY, "Read-only volume", line, "Save a copy somewhere else?", "Save as\xF9", NULL, "Cancel");
    } else if(note_refused(g, g->note.path)) {
        gui_unsure_line(g, g->note.path, line, sizeof(line));
        dialog_confirm(g, ACT_DLG_READONLY, "Check the disk", line, "Save a copy under another name?", "Save as\xF9", NULL, "Cancel");
    } else save_now(g, g->note.path);
}

/* true: nothing is unsaved, go ahead; otherwise ask, and do pending once the answer allows */
bool gui_guard(struct gui_state *g, int pending, const char *path) {
    char line[80];
    if(!g->note.modified) return true;
    g->pending = pending;
    strlcpy(g->pending_path, path ? path : "", sizeof(g->pending_path));
    snprintf(line, sizeof(line), "Save the changes to %s?", g->note.path[0] ? gui_basename(g->note.path) : "Untitled");
    gui_open(g, GUI_NOTEPAD);
    dialog_confirm(g, ACT_DLG_GUARD, "Unsaved changes", line,
                   pending == PEND_QUIT ? "If you don't, they are lost when the desktop closes." : "If you don't, they are lost.",
                   "Save", "Don't save", "Cancel");
    return false;
}

void gui_proceed(struct gui_state *g) {
    char path[GUI_PATH_MAX];
    int p = g->pending;
    strlcpy(path, g->pending_path, sizeof(path));
    g->pending = PEND_NONE;
    switch(p) {
    case PEND_NEW: note_reset(&g->note); gui_open(g, GUI_NOTEPAD); break;
    case PEND_OPEN_PATH: if(note_load(g, path) == 0) gui_open(g, GUI_NOTEPAD); break;
    case PEND_QUIT: g->quit = true; break;
    case PEND_CLOSE: gui_close_now(g, GUI_NOTEPAD); break;
    }
    g->dirty = true;
}

/* 10/17/26 (the suite): an application that is busy (a terminal session) asks first; then Notepad's guard */
void gui_request_quit(struct gui_state *g) {
    char why[80];
    int id;
    for(id = GUI_FIRST_APP; id < GUI_FIRST_CLIENT; id++) {
        const struct gui_app *a = gui_app_of(id);
        if(!g->win[id].open || !a->busy || !a->busy(g, why, sizeof(why))) continue;
        dialog_confirm(g, ACT_DLG_LEAVE, "Leave the desktop?", why, "It ends when the desktop closes.", "Leave", NULL,
                       "Cancel");
        return;
    }
    if((id = client_count(g)) > 0) {                    /* 10/25/26: programs' windows */
        if(id == 1) strlcpy(why, "A program has a window open.", sizeof(why));
        else snprintf(why, sizeof(why), "%d programs have windows open.", id);
        dialog_confirm(g, ACT_DLG_LEAVE, "Leave the desktop?", why, "Their windows close when the desktop does.",
                       "Leave", NULL, "Cancel");
        return;
    }
    if(gui_guard(g, PEND_QUIT, NULL)) g->quit = true;
}

/**** 11/05/26 (track desk): the clipboard's changes, the theme, the preferences, screenshots ****/

void gui_clipped(struct gui_state *g, u32 len) {
    g->clip_len = len;
    g->clip_serial++;
}

/* the colours by role (surfos/win.h's WIN_COLOR_*) the desktop draws its own windows in; the theme changes the desktop
   behind them */
void gui_theme_colors(const struct gui_state *g, u32 *c) {
    c[WIN_COLOR_DESKTOP] = g->night ? 0x142638 : 0x245A67;
    c[WIN_COLOR_WINDOW] = PAPER;
    c[WIN_COLOR_TEXT] = INK;
    c[WIN_COLOR_MUTED] = MUTED;
    c[WIN_COLOR_ACCENT] = TEAL;
    c[WIN_COLOR_ON_ACCENT] = PAPER;
    c[WIN_COLOR_BAND] = BAND;
    c[WIN_COLOR_BUTTON] = SHELL;
    c[WIN_COLOR_LINE] = LINE;
    c[WIN_COLOR_SELECTION] = PICK;
    c[WIN_COLOR_FIELD] = 0xFFFDF5;
    c[WIN_COLOR_FRAME] = INK;
    c[WIN_COLOR_DARK] = NAVY;
    c[WIN_COLOR_LIGHT] = FOAM;
    c[WIN_COLOR_WARN] = CORAL;
    c[WIN_COLOR_DISABLED] = 0xA3B4B3;
}

/* the preferences in the settings, read each second: a change (Settings', or the stick's at the start) applies here
   at once; one the desktop made itself reads back the same */
static void prefs_tick(struct gui_state *g) {
    int theme, speed;
    if(!g->sys || !g->sys->prefs || (g->pref_theme >= 0 && g->now - g->pref_at < 1000)) return;
    g->pref_at = g->now;
    if(g->sys->prefs(g->sys->ctx, &theme, &speed) < 0) return;
    if(theme != g->pref_theme && g->night != (theme == 1)) {   /* drawn again only for what changes */
        g->night = theme == 1;
        g->dirty = true;
    }
    if(speed != g->pref_speed && speed >= 1 && speed <= 3 && g->speed != speed) {
        g->speed = speed;
        g->dirty = true;
    }
    g->pref_theme = theme;
    g->pref_speed = speed;
}

/* Pointer & desktop changed one (which 0: the theme, 1: the speed): in force here at once (the caller set it); 11/05/26
   (the review of stage 2) written into the settings once it has stood GUI_PREF_MS, or at the desktop's end, so that a
   run of clicks writes the stick once, and the desktop's pass never waits on the stick for each */
static void pref_save(struct gui_state *g, int which) {
    g->pref_pending |= 1u << which;
    g->pref_changed = g->now;
    strlcpy(g->pref_status, "Kept in the settings in a moment.", sizeof(g->pref_status));
    g->dirty = true;
}

/* what waits written into the settings (the values in force now), and the status says so */
static void pref_flush(struct gui_state *g) {
    char msg[GUI_STATUS];
    int which, r;
    for(which = 0; which < 2; which++) {
        if(!(g->pref_pending & (1u << which))) continue;
        g->pref_pending &= ~(1u << which);
        if(!g->sys || !g->sys->pref_set) continue;
        msg[0] = 0;
        r = g->sys->pref_set(g->sys->ctx, which, which ? g->speed : g->night, msg, sizeof(msg));
        if(r == 0) strlcpy(g->pref_status, "Saved in the settings on the stick.", sizeof(g->pref_status));
        else snprintf(g->pref_status, sizeof(g->pref_status), "%s%s", r > 0 ? "For now, not saved: " : "Not changed: ",
                      msg[0] ? msg : "no settings stick");
    }
    g->dirty = true;
}

/* the first volume mounted that may be written (a stick's), else / */
void gui_volume_dir(struct gui_state *g, char *out, size_t n) {
    int k, count = g->fs && g->fs->volumes ? g->fs->volumes(g->fs->ctx, g->vols, GUI_VOLUMES) : 0;
    strlcpy(out, "/", n);
    for(k = 0; k < count && k < GUI_VOLUMES; k++)
        if(g->vols[k].path[0] && !g->vols[k].readonly && !g->vols[k].gone) {
            strlcpy(out, g->vols[k].path, n);
            return;
        }
}

/* a screenshot asked for (SurfOS > Screenshot, Print Screen): its name now, the frame captured by the shell next */
void gui_screenshot(struct gui_state *g) {
    const struct gui_time *t = &g->time;
    if(!g->fs || g->shot.state != GUI_SHOT_NONE) return;
    if(t->year) snprintf(g->shot.name, sizeof(g->shot.name), "Screenshot %04u-%02u-%02u %02u%02u%02u.bmp", t->year,
                         t->month, t->date, t->hour, t->minute, t->second);
    else strlcpy(g->shot.name, "Screenshot.bmp", sizeof(g->shot.name));
    g->shot.state = GUI_SHOT_WANTED;
    g->dirty = true;
}

/* a screenshot captured: the Save as chooser, once no other dialog is up */
static void shot_ask(struct gui_state *g) {
    char dir[GUI_PATH_MAX];
    if(g->shot.state != GUI_SHOT_TAKEN || g->dialog.kind) return;
    if(!g->fs || !g->shot.data) {
        g->shot.state = GUI_SHOT_DONE;
        dialog_message(g, "No screenshot", "There was no memory for the picture.", "");
        return;
    }
    gui_volume_dir(g, dir, sizeof(dir));
    dialog_chooser_for(g, GUI_DLG_SAVE, ACT_DLG_SHOT, "Save the screenshot", dir, g->shot.name, "bmp");
    g->shot.state = GUI_SHOT_ASKING;
}

static void shot_save(struct gui_state *g, const char *path) {
    char kept[GUI_PATH_MAX], line[80];
    int r = g->fs->write(g->fs->ctx, path, g->shot.data, g->shot.len, false, kept, sizeof(kept));
    g->shot.state = GUI_SHOT_DONE;
    if(r == 0) {
        if(g->win[GUI_FILES].open) files_refresh(g);
        return;
    }
    if(kept[0]) note_remember(g, path, kept);           /* a rename that may have half happened: both taken care of */
    snprintf(line, sizeof(line), "%s: %s", gui_basename(path), gui_error(g, r));
    dialog_message(g, "Screenshot not saved", line, kept[0] ? "Its new file is kept beside it." : "");
}

/* the chooser's answer: a file that is not there is saved; one that is asks first, unless the desktop takes care of
   it (Notepad's Save as's rules) */
static void shot_answer(struct gui_state *g, int act, int button, const char *text, const char *target) {
    struct gui_entry e;
    bool device, readonly;
    char line[80];
    if(g->shot.state != GUI_SHOT_ASKING) return;
    if(button != 0 || !g->fs) { g->shot.state = GUI_SHOT_DONE; return; }
    if(act == ACT_DLG_SHOT_REPLACE) { shot_save(g, target); return; }
    if(g->fs->stat(g->fs->ctx, text, &e, &device, &readonly) == 0) {
        snprintf(line, sizeof(line), "%s is there already.", gui_basename(text));
        if(e.type == GUI_DIR || device) dialog_message(g, "Screenshot not saved", line, "It is not a file that can be replaced.");
        else if(note_guarded(g, text)) {
            gui_unsure_line(g, text, line, sizeof(line));
            dialog_message(g, "Screenshot not saved", line, "Have the disk checked first (fsck).");
        } else {
            dialog_confirm(g, ACT_DLG_SHOT_REPLACE, "Replace the file?", line, "Saving replaces what it holds.",
                           "Replace", NULL, "Cancel");
            strlcpy(g->dialog.target, text, sizeof(g->dialog.target));
            return;
        }
        g->shot.state = GUI_SHOT_DONE;
        return;
    }
    shot_save(g, text);
}

void gui_tick(struct gui_state *g) {
    int id;
    if(!g->reg.loaded) reg_load(g);                     /* 11/05/26: the registry, when the desktop starts */
    if(g->pref_pending && g->now - g->pref_changed >= GUI_PREF_MS) pref_flush(g);  /* a change that stood */
    prefs_tick(g);                                      /* 11/05/26: the preferences, each second */
    for(id = GUI_FIRST_APP; id < GUI_FIRST_CLIENT; id++)
        if(g->win[id].open && gui_app_of(id)->tick) gui_app_of(id)->tick(g);
    client_tick(g);                                     /* 10/25/26: programs' windows */
    if(menu_stale(g)) { g->menu = 0; g->dirty = true; } /* 10/26/26: its window lost the focus */
}

/* 10/26/26 (stage C12): what waits for a pass's input to be in; 11/05/26: and a screenshot's chooser */
void gui_input_done(struct gui_state *g) {
    client_alerts(g);
    shot_ask(g);
}

void gui_finish(struct gui_state *g) {
    int id;
    if(g->pref_pending) pref_flush(g);                  /* 11/05/26: a change not written yet */
    for(id = GUI_FIRST_APP; id < GUI_FIRST_CLIENT; id++)
        if(g->win[id].open && gui_app_of(id)->closed) gui_app_of(id)->closed(g);
    client_finish(g);                                   /* 10/25/26: programs' windows let go */
}

static void next_window(struct gui_state *g) {
    int i, id;
    for(i = GUI_SLOTS - 1; i >= 0; i--)                 /* 10/25/26: a program's window waiting for the focus first */
        if(client_waiting(g, g->order[i])) { gui_open(g, g->order[i]); return; }
    for(i = 0; i < GUI_SLOTS; i++) {                    /* the window at the back comes to the front */
        id = g->order[i];
        if(g->win[id].open && id != g->focus) { gui_open(g, id); return; }
    }
}

void gui_act(struct gui_state *g, int act) {
    struct gui_note *n = &g->note;
    char line[80], dir[GUI_PATH_MAX];
    bool note = g->focus == GUI_NOTEPAD && g->win[GUI_NOTEPAD].open;
    g->dirty = true;
    if(act >= ACT_WINDOW && act < ACT_WINDOW + GUI_SLOTS) { gui_open(g, act - ACT_WINDOW); return; }
    if(act >= ACT_CLIENT_MENU && act < ACT_CLIENT_MENU + WIN_MENU_ENTRIES) {     /* 11/05/26: a program's item */
        client_menu_chosen(g, act - ACT_CLIENT_MENU);
        return;
    }
    if(act >= ACT_REG_START && act < ACT_REG_START + GUI_REG_MAX) { reg_start(g, act - ACT_REG_START); return; }
    if((act >= ACT_REG_OPEN && act < ACT_REG_OPEN + GUI_REG_MAX) || (act >= ACT_WITH_NOTEPAD && act <= ACT_WITH_VIEWER)) {
        if(!files_file(g) || !files_path(g, g->files.selected, dir, sizeof(dir))) return;   /* Open with's */
        if(act == ACT_WITH_NOTEPAD) { if(gui_guard(g, PEND_OPEN_PATH, dir) && note_load(g, dir) == 0) gui_open(g, GUI_NOTEPAD); }
        else if(act == ACT_WITH_HEX) hex_show(g, dir);
        else if(act == ACT_WITH_VIEWER) viewer_show(g, dir);
        else reg_open(g, act - ACT_REG_OPEN, dir);
        return;
    }
    if(act >= ACT_FILES_OPEN && act <= ACT_FILES_EJECT) { files_act(g, act); return; }
    if(act >= ACT_UNDO && act <= ACT_GOTO && edit_app(g)) {    /* 10/17/26: the focused application's Edit menu */
        if(edit_app(g)->can_edit(g, act)) edit_app(g)->edit(g, act);
        return;
    }
    switch(act) {
    case ACT_ABOUT: gui_open(g, GUI_ABOUT); break;
    case ACT_SETTINGS: gui_open(g, GUI_SETTINGS); break;
    case ACT_THEME: g->night = !g->night; pref_save(g, 0); break;   /* 11/05/26: and kept */
    case ACT_SCREENSHOT: gui_screenshot(g); break;     /* 11/05/26 */
    case ACT_PROGRAMS: menu_child(g, MENU_PROGRAMS); break;
    case ACT_OPEN_WITH: if(files_file(g)) menu_child(g, MENU_OPEN_WITH); break;
    case ACT_QUIT: gui_request_quit(g); break;
    case ACT_NEW: if(gui_guard(g, PEND_NEW, NULL)) { note_reset(n); gui_open(g, GUI_NOTEPAD); } break;
    case ACT_OPEN:
        if(g->fs) {
            chooser_dir(g, dir, sizeof(dir));
            dialog_chooser(g, GUI_DLG_OPEN, ACT_DLG_OPEN, dir, "");
        }
        break;
    case ACT_SAVE: if(note) save(g); break;
    case ACT_SAVE_AS: if(note) save_as(g); break;
    case ACT_REVEAL: if(n->path[0] && g->fs) { gui_open(g, GUI_FILES); files_reveal(g, n->path); } break;
    case ACT_CLOSE: gui_close(g, g->focus); break;
    case ACT_UNDO: case ACT_REDO: if(note) note_undo(g, act == ACT_REDO); break;
    case ACT_CUT: case ACT_COPY: if(note) note_copy(g, act == ACT_CUT); break;
    case ACT_PASTE: if(note) note_paste(g); break;
    case ACT_DELETE: if(note && note_has_selection(n)) note_insert(g, "", 0); break;
    case ACT_SELECT_ALL: if(note) note_select_all(g); break;
    case ACT_FIND:
        if(!note) break;
        if(note_has_selection(n)) {                     /* a short selection is what to look for */
            u32 a = n->anchor < n->cursor ? n->anchor : n->cursor, b = n->anchor < n->cursor ? n->cursor : n->anchor;
            if(b - a < sizeof(n->find.text) && !memchr(n->text + a, '\n', b - a)) {
                memcpy(n->find.text, n->text + a, b - a);
                n->find.text[b - a] = 0;
            }
        }
        field_set(&n->find, n->find.text, true);
        n->finding = true;
        break;
    case ACT_FIND_NEXT: if(note && n->find.len) note_find(g, n->find.text); break;
    case ACT_GOTO:
        if(!note) break;
        snprintf(line, sizeof(line), "%d", note_line_of(n, n->cursor) + 1);
        dialog_input(g, ACT_DLG_GOTO, "Go to line", "", line);
        snprintf(g->dialog.text[0], sizeof(g->dialog.text[0]), "Line number, 1 to %d:", note_lines(n));
        break;
    case ACT_NOTEPAD: gui_open(g, GUI_NOTEPAD); break;
    case ACT_FILES: if(g->fs) gui_open(g, GUI_FILES); break;
    case ACT_CALC: gui_open(g, GUI_CALC); break;
    case ACT_SKETCH: gui_open(g, GUI_SKETCH); break;
    case ACT_CLEAR_SKETCH:
        memset(g->canvas, 0, sizeof(g->canvas));
        g->viewer.sketch.status[0] = 0;                 /* 10/17/26: what Save or Open did is no longer the drawing */
        gui_open(g, GUI_SKETCH);
        break;
    case ACT_SKETCH_SAVE: sketch_save_as(g); break;     /* 10/17/26: Sketch's pictures (guibmp.c) */
    case ACT_SKETCH_OPEN: sketch_open(g); break;
    case ACT_NEXT_WINDOW: next_window(g); break;
    case ACT_MINIMIZE: minimize(g, g->focus); break;
    case ACT_MAXIMIZE: maximize(g, g->focus); break;
    case ACT_HELP: gui_open(g, GUI_HELP); break;
    case ACT_CLIENT_COPY: client_key(g, 3); break;      /* 10/26/26 (C12): a program's window copies itself */
    case ACT_RUN:                                       /* 10/25/26: the launcher: /initrd/bin's programs, a line to run */
        if(g->sys && g->sys->run) dialog_chooser(g, GUI_DLG_RUN, ACT_DLG_RUN, "/initrd/bin", "wcalc");
        break;
    }
}

/* 10/25/26: the launcher: the line started on the desktop's console (struct gui_sys's run), its first window to
   take the focus as it appears; or why not */
static int run_program(struct gui_state *g, const char *line) {
    char msg[80];
    int pid;
    msg[0] = 0;
    if(!g->sys || !g->sys->run) return -1;
    pid = g->sys->run(g->sys->ctx, line, msg, sizeof(msg));
    if(pid > 0) client_launched(g, pid);
    if(pid >= 0) return pid;
    dialog_message(g, "Not started", msg[0] ? msg : "The program could not be started.", "");
    return pid;                                         /* 10/26/26: its pid, for an application's launcher */
}

/* 10/26/26 (stage C12): an application that is a program now: the window of the program its launcher started last
   to the front, or the program started, its window to take the focus as it appears (the launcher's rule) and to
   stand at the application's place (guiclient.c's accept()); but nothing while the one just started has no window
   yet (GUI_START_MS: the second press of a double-click) */
static void app_program(struct gui_state *g, int app) {
    int k = app - GUI_FIRST_APP, id = client_of_pid(g, g->app_pid[k]), pid;
    g->menu = 0;
    g->dirty = true;
    if(id >= 0) { gui_open(g, id); return; }
    if(g->app_starting[k] && g->now - g->app_at[k] < GUI_START_MS) return;
    pid = run_program(g, gui_app_of(app)->program);
    g->app_pid[k] = pid > 0 ? pid : 0;
    g->app_at[k] = g->now;
    g->app_starting[k] = pid > 0;
}

/* 10/26/26: the application whose launcher started pid, its slot; -1 */
int gui_app_slot_of(const struct gui_state *g, int pid) {
    int k;
    for(k = 0; pid > 0 && k < SUITE; k++) if(g->app_pid[k] == pid) return GUI_FIRST_APP + k;
    return -1;
}

void gui_answer(struct gui_state *g, int act, int button, const char *text, const char *target) {
    struct gui_entry e;
    bool device, readonly;
    char line[80];
    g->dirty = true;
    if(act >= ACT_APP_DIALOG && act < ACT_APP_DIALOG + 32 * SUITE) {   /* 10/17/26: an application's own dialog */
        const struct gui_app *a = suite[(act - ACT_APP_DIALOG) / 32];
        int id = GUI_FIRST_APP + (act - ACT_APP_DIALOG) / 32;
        if((act - ACT_APP_DIALOG) % 32 == GUI_APP_CLOSE) { if(button == 0 && g->win[id].open) gui_close_now(g, id); }
        else if(a->answer) a->answer(g, (act - ACT_APP_DIALOG) % 32, button, text, target);
        return;
    }
    if(act >= ACT_CLIENT_END && act < ACT_CLIENT_END + GUI_CLIENTS) {   /* 10/25/26: "End the program?" */
        client_answer(g, act - ACT_CLIENT_END, button, target);
        return;
    }
    if(act >= ACT_CLIENT_DIALOG && act < ACT_CLIENT_REPLACE + GUI_CLIENTS) {     /* 11/05/26: a program's chooser */
        client_chooser_answer(g, (act - ACT_CLIENT_DIALOG) % GUI_CLIENTS, act, button, text, target);
        return;
    }
    if(act == ACT_DLG_SHOT || act == ACT_DLG_SHOT_REPLACE) { shot_answer(g, act, button, text, target); return; }
    if(act == ACT_DLG_LEAVE) {
        if(button == 0 && gui_guard(g, PEND_QUIT, NULL)) g->quit = true;
        return;
    }
    switch(act) {
    case ACT_DLG_GUARD:
        if(button == 0) save(g);
        else if(button == 1) gui_proceed(g);
        else g->pending = PEND_NONE;
        break;
    case ACT_DLG_OPEN:
        if(button == 0 && gui_guard(g, PEND_OPEN_PATH, text) && note_load(g, text) == 0) gui_open(g, GUI_NOTEPAD);
        break;
    case ACT_DLG_SAVE_AS:
        if(button != 0 || !g->fs) { g->pending = PEND_NONE; break; }
        if(strcmp(text, g->note.path) && g->fs->stat(g->fs->ctx, text, &e, &device, &readonly) == 0) {
            snprintf(line, sizeof(line), "%s is there already.", gui_basename(text));
            if(e.type == GUI_DIR || device) {
                dialog_message(g, "Not saved", line, "It is not a file that can be replaced.");
                g->pending = PEND_NONE;
                break;
            }
            if(note_guarded(g, text)) {                 /* replacing it could free what another holds */
                gui_unsure_line(g, text, line, sizeof(line));
                dialog_message(g, "Not saved", line, "Have the disk checked first (fsck).");
                g->pending = PEND_NONE;
                break;
            }
            dialog_confirm(g, ACT_DLG_REPLACE, "Replace the file?", line, "Saving replaces what it holds.", "Replace", NULL, "Cancel");
            strlcpy(g->dialog.target, text, sizeof(g->dialog.target));
            break;
        }
        save_now(g, text);
        break;
    case ACT_DLG_REPLACE:
        if(button == 0) save_now(g, target);
        else g->pending = PEND_NONE;
        break;
    case ACT_DLG_READONLY:
        if(button == 0) save_as(g);
        else g->pending = PEND_NONE;
        break;
    case ACT_DLG_GOTO:
        if(button == 0) note_goto(g, atoi(text));
        break;
    case ACT_DLG_RUN:                                   /* 10/25/26 */
        if(button == 0) run_program(g, text);
        break;
    case ACT_DLG_NEW_FILE: case ACT_DLG_NEW_FOLDER: case ACT_DLG_RENAME: case ACT_DLG_DELETE:
        files_answer(g, act, button, text, target);
        break;
    }
}

/**** the pointer ****/

static void stroke(struct gui_state *g, int x, int y) {
    int dx=x-g->last_x, dy=y-g->last_y, n, i;
    int adx=dx<0?-dx:dx, ady=dy<0?-dy:dy;
    n=adx>ady?adx:ady;
    for(i=0;i<=n;i++) {
        int px=g->last_x+(n?dx*i/n:0), py=g->last_y+(n?dy*i/n:0);
        if(gui_inside(px,py,0,0,GUI_CANVAS_W,GUI_CANVAS_H)) g->canvas[py*GUI_CANVAS_W+px]=g->brush;
    }
    g->viewer.sketch.status[0]=0;                       /* 10/17/26: the line under the canvas is the hint again */
    g->last_x=x; g->last_y=y; g->dirty=true;
}

/* the taskbar's buttons: the open windows, by number */
static int task_buttons(const struct gui_state *g, int *ids, int *width) {
    int n = 0, i;
    for(i = 0; i < GUI_SLOTS; i++) if(g->win[i].open) ids[n++] = i;
    *width = n ? (g->width - 96) / n - 4 : 0;
    if(*width > 156) *width = 156;
    return n;
}

static void taskbar_press(struct gui_state *g) {
    int ids[GUI_SLOTS], n, w, i;
    n = task_buttons(g, ids, &w);
    for(i = 0; i < n; i++) {
        if(!gui_inside(g->x, g->y, 6 + i * (w + 4), g->height - TASK_H + 3, w, TASK_H - 6)) continue;
        if(g->focus == ids[i]) minimize(g, ids[i]);
        else gui_open(g, ids[i]);
        return;
    }
}

/* 10/26/26 (stage C8): a right-click opens, where it was, the menu bar's menu for what is under the pointer: over a
   window (brought to the front, so the menu's items act on it) Edit's when it edits (Notepad, the suite's that
   do), File's for Files, Window's on its title bar and for the rest; over the desktop, Apps. Nothing on the menu
   bar and the taskbar */
static void context_press(struct gui_state *g) {
    int i, id, m = 4, target = -1;
    struct gui_window *w;
    if(g->y < MENU_H || g->y >= g->height - TASK_H) return;
    for(i = GUI_SLOTS - 1; i >= 0; i--) {
        id = g->order[i];
        w = &g->win[id];
        if(!w->open || w->min || !gui_inside(g->x, g->y, w->x, w->y, w->w, w->h)) continue;
        gui_open(g, id);
        target = id;
        m = g->y - w->y < TITLE_H ? 5 : id == GUI_NOTEPAD || edit_app(g) ? 3 : id == GUI_FILES ? 2 : 5;
        if(m == 2) files_select_at(g, g->x - w->x, g->y - w->y);   /* the row under it, for File's items */
        break;
    }
    menu_open(g, m);
    g->menu_x = g->x;
    g->menu_y = g->y;
    g->menu_win = target;
}

/* 10/26/26: x, y in the open menu's box */
static bool in_menu(const struct gui_state *g, int x, int y) {
    struct menu_item it[MENU_MAX];
    struct menu_box b;
    int n = menu_items(g, g->menu, it);
    menu_box_of(g, g->menu, it, n, &b);
    return gui_inside(x, y, b.x, b.y, b.w, b.h);
}

/* Help's text rows, its last useful first row, and its scroll bar (above the resize grip) */
static int help_rows(const struct gui_window *w) { return (w->h - TITLE_H - 12) / 18; }
static int help_max(const struct gui_window *w) { return HELP_LINES > help_rows(w) ? HELP_LINES - help_rows(w) : 0; }
static int help_bar(const struct gui_window *w) { return w->h - TITLE_H - 16; }

static void title_press(struct gui_state *g, int id, int x, int y) {
    struct gui_window *w = &g->win[id];
    int dx = x - g->title_x, dy = y - g->title_y;
    if(x >= w->w - 28) { gui_close(g, id); return; }
    if(resizable(g, id) && x >= w->w - 52) { maximize(g, id); return; }
    if(x >= w->w - (resizable(g, id) ? 76 : 52)) { minimize(g, id); return; }
    /* a double-click: a second press in the same place soon after, with no drag between them */
    if(resizable(g, id) && id == g->title_id && g->now - g->title_ms <= DOUBLE_MS && dx >= -4 && dx <= 4 && dy >= -4 && dy <= 4) {
        g->title_id = -1;
        maximize(g, id);
        return;
    }
    g->title_id = id;
    g->title_ms = g->now;
    g->title_x = x;
    g->title_y = y;
    g->press_x = g->x;
    g->press_y = g->y;
    if(!w->max) { g->drag = id; g->drag_x = x; g->drag_y = y; }
}

static void content_press(struct gui_state *g, int id, int x, int y) {
    struct gui_window *w = &g->win[id];
    int rows;
    if(id==GUI_CALC && gui_inside(x,y,16,104,216,160)) {
        int col=(x-16)/54,row=(y-104)/40;
        if((x-16)%54<48 && (y-104)%40<34) gui_calc_key(&g->calc,keys[row*4+col]);
    }
    if(id==GUI_SKETCH) {
        if(gui_inside(x,y,16,38,6*32,24)) g->brush=(x-16)/32;
        else if(gui_inside(x,y,212,38,56,24)) gui_act(g,ACT_SKETCH_OPEN);   /* 10/17/26: BMP files (guibmp.c) */
        else if(gui_inside(x,y,272,38,56,24)) gui_act(g,ACT_SKETCH_SAVE);
        else if(gui_inside(x,y,336,38,80,24)) { memset(g->canvas,0,sizeof(g->canvas)); g->viewer.sketch.status[0]=0; }
        else if(gui_inside(x,y,16,70,GUI_CANVAS_W,GUI_CANVAS_H)) {
            g->drawing=true; g->last_x=x-16; g->last_y=y-70; stroke(g,x-16,y-70);
        }
    }
    if(id==GUI_ABOUT && gui_inside(x,y,16,166,4*102-8,34) && (x-16)%102<94)
        gui_act(g,tiles[(x-16)/102].app==GUI_FILES?ACT_FILES:ACT_WINDOW+tiles[(x-16)/102].app);
    if(id==GUI_SETTINGS) {                              /* 11/05/26: each change kept in the settings */
        if(gui_inside(x,y,16,100,3*112,30)) { g->speed=(x-16)/112+1; pref_save(g,1); }
        if(gui_inside(x,y,16,172,176,32)) { g->night=!g->night; pref_save(g,0); }
    }
    if(id == GUI_NOTEPAD) note_press(g, x, y);
    if(id == GUI_FILES) files_press(g, x, y);
    if(gui_app_of(id)) {                                /* 10/17/26: the suite's, and a drag after it */
        const struct gui_app *a = gui_app_of(id);
        if(a->press) a->press(g, x, y);
        if(a->drag || a->release) g->app_drag = id;
    }
    if(id == GUI_HELP && x >= w->w - 2 - SCROLL_W && y - TITLE_H < help_bar(w)) {   /* page, or hold the thumb */
        int top, size;
        rows = help_rows(w);
        gd_thumb(help_bar(w), g->help_top, rows, HELP_LINES, &top, &size);
        if(y - TITLE_H < top) g->help_top = gui_clamp(g->help_top - rows, 0, help_max(w));
        else if(y - TITLE_H >= top + size) g->help_top = gui_clamp(g->help_top + rows, 0, help_max(w));
        else { g->scroll = GUI_HELP; g->scroll_at = y - TITLE_H - top; }
    }
    g->dirty = true;
}

static void wheel(struct gui_state *g, int dz) {
    int i, id, lines = -dz * 3;                         /* the wheel turned away from the user: up */
    struct gui_window *w;
    if(g->dialog.kind) { dialog_wheel(g, lines); return; }
    if(g->menu) { menu_scroll(g, lines > 0 ? 1 : -1); return; }   /* 10/26/26 (C8): a menu open drops it (11/05/26:
                                                                     but one that scrolls) */
    if(g->clients.grab >= 0) { client_wheel(g, g->clients.grab, lines); return; }   /* a program's grab: its own */
    for(i = GUI_SLOTS - 1; i >= 0; i--) {
        id = g->order[i];
        w = &g->win[id];
        if(!w->open || w->min || !gui_inside(g->x, g->y, w->x, w->y, w->w, w->h)) continue;
        if(id == GUI_NOTEPAD) note_wheel(g, lines);
        if(id == GUI_FILES) files_wheel(g, lines);
        if(gui_app_of(id) && gui_app_of(id)->wheel) gui_app_of(id)->wheel(g, lines);
        if(id >= GUI_FIRST_CLIENT) client_wheel(g, id, lines);      /* 10/25/26: over its content */
        if(id == GUI_HELP) g->help_top = gui_clamp(g->help_top + lines, 0, help_max(w));
        g->dirty = true;
        return;
    }
}

/* the left button is let go of: every drag ends, the suite's through its release hook. 10/18/26: also when no
   pointer event says so (shell/gui.c, when the desktop gets the screen back; a window minimized mid-drag), so that
   no application goes on with a drag nobody holds (the Log scrolled by itself) */
void gui_release(struct gui_state *g) {
    int id;
    g->drag=g->resize=g->scroll=-1; g->drawing=false; g->note.selecting=false;
    if(g->app_drag>=0) {                                /* 10/17/26: the suite's drag ends */
        id=g->app_drag; g->app_drag=-1;
        if(gui_app_of(id)->release) gui_app_of(id)->release(g);
        g->dirty=true;
    }
}

void gui_pointer(struct gui_state *g, const struct pointer_event *e) {
    bool press=(e->buttons&POINTER_LEFT) && !(g->buttons&POINTER_LEFT);
    bool right=(e->buttons&POINTER_RIGHT) && !(g->buttons&POINTER_RIGHT);  /* 10/26/26 (C8): a right-click */
    unsigned old=g->buttons;
    int i,id,x,y,oldx=g->x,oldy=g->y;
    struct gui_window *w;
    g->x=gui_clamp(e->x,0,g->width-1); g->y=gui_clamp(e->y,0,g->height-1); g->buttons=e->buttons;
    if(e->wheel) {                                      /* 10/26/26 (C8): a turn alone is the wheel's, but a drag */
        wheel(g,e->wheel);                              /* under way goes on after the scroll (a selection's end) */
        if(g->x==oldx && g->y==oldy && g->buttons==old && g->drag<0 && g->resize<0 && g->scroll<0 &&
           g->app_drag<0 && !g->note.selecting && !g->drawing) return;
    }
    if(!(e->buttons&POINTER_LEFT)) gui_release(g);
    if(g->dialog.kind) {                                /* 11/05/26 (the review of stage 2): the dialog's, and no */
        client_pointer_lost(g);                         /* program's window under it hears a button held */
        dialog_pointer(g,g->x,g->y,press);
        return;
    }
    if(!g->menu && client_grace(g,old)) return;         /* 10/26/26: a press on a window just come up by itself */
    if(client_pointer(g,old)) return;                   /* 10/25/26: a program's: over its content, or held for it */
    if(g->drag>=0) {                                    /* below the menu bar, above the taskbar (as gui_place) */
        int top, bottom, nx, ny;
        w=&g->win[g->drag];
        top=top_limit(g,g->drag,w->h);
        bottom=g->height-TASK_H-2-w->h;
        nx=gui_clamp(g->x-g->drag_x,0,g->width-w->w);
        ny=gui_clamp(g->y-g->drag_y,top,bottom>top?bottom:top);
        if(nx!=w->x || ny!=w->y) { w->x=nx; w->y=ny; g->dirty=true; }
        if(g->x-g->press_x>4 || g->press_x-g->x>4 || g->y-g->press_y>4 || g->press_y-g->y>4) g->title_id=-1;   /* a drag is no double-click */
        return;
    }
    if(g->resize>=0) {                                  /* never narrower than its toolbar */
        int room_w,room_h;
        w=&g->win[g->resize];
        room_w=g->width-w->x; room_h=g->height-TASK_H-2-w->y;              /* the bottom a drag keeps */
        if(g->resize>=GUI_FIRST_CLIENT) client_limit(g,g->resize,&room_w,&room_h);   /* 10/25/26: its program's limits */
        w->w=gui_clamp(g->x-w->x+g->drag_x,min_width(g,g->resize),room_w);
        w->h=gui_clamp(g->y-w->y+g->drag_y,min_height(g,g->resize),room_h);
        if(w->x+w->w>g->width) w->x=g->width-w->w>0?g->width-w->w:0;
        g->dirty=true; return;
    }
    if(g->scroll>=0) {
        w=&g->win[g->scroll];
        if(g->scroll==GUI_NOTEPAD) note_scroll(g,g->y-w->y);
        if(g->scroll==GUI_FILES) files_scroll(g,g->y-w->y);
        if(g->scroll==GUI_HELP) { g->help_top=gd_scroll_to(help_bar(w),g->y-w->y-TITLE_H-g->scroll_at,help_rows(w),HELP_LINES); g->dirty=true; }
        return;
    }
    if(g->note.selecting) { w=&g->win[GUI_NOTEPAD]; note_drag(g,g->x-w->x,g->y-w->y); return; }
    if(g->app_drag>=0 && (e->buttons&POINTER_LEFT)) {   /* 10/17/26: the suite's */
        w=&g->win[g->app_drag];
        if(gui_app_of(g->app_drag)->drag) gui_app_of(g->app_drag)->drag(g,g->x-w->x,g->y-w->y);
        return;
    }
    if(g->drawing && (e->buttons&POINTER_LEFT)) {
        w=&g->win[GUI_SKETCH]; x=g->x-w->x-16; y=g->y-w->y-70;
        if(gui_inside(x,y,0,0,GUI_CANVAS_W,GUI_CANVAS_H)) {
            if(g->last_x<0) { g->last_x=x; g->last_y=y; }
            stroke(g,x,y);
        } else g->last_x=-1;
        return;
    }
    if(right && !press) {                               /* 10/26/26 (C8): with a menu open, in it nothing, */
        if(!g->menu) context_press(g);                  /* elsewhere it closes as a left click does */
        else if(!in_menu(g, g->x, g->y)) { g->menu = 0; g->dirty = true; }
        return;
    }
    if(g->menu && !press) {                             /* the item under the pointer is the one chosen */
        id=g->y<MENU_H && !g->menu_y?menu_at(g,g->x):0;     /* 10/26/26: the bar's titles, for the bar's menus */
        if(id && id!=bar_parent(g, g->menu)) { menu_open(g,id); return; }   /* 11/05/26: a menu an item opened stays */
        i=menu_hit(g,g->x,g->y);
        if(i>=0 && i!=g->selection) { g->selection=i; g->dirty=true; }
        return;
    }
    if(!press) return;
    if(g->y<MENU_H) { id=menu_at(g,g->x); menu_open(g,bar_parent(g, g->menu)==id && !g->menu_y?0:id); return; }
    if(g->menu) {
        i=menu_hit(g,g->x,g->y);
        if(i==-2 || i==-3) menu_scroll(g,i==-2?-1:1);  /* 11/05/26: a strip of a menu that scrolls */
        else if(i>=0) menu_run(g,i);
        else { g->menu=0; g->dirty=true; }
        return;
    }
    if(g->y>=g->height-TASK_H) { taskbar_press(g); return; }
    for(i=GUI_SLOTS-1;i>=0;i--) {
        id=g->order[i]; w=&g->win[id];
        if(!w->open || w->min || !gui_inside(g->x,g->y,w->x,w->y,w->w,w->h)) continue;
        gui_open(g,id); x=g->x-w->x; y=g->y-w->y;
        if(y<TITLE_H) { title_press(g,id,x,y); return; }
        if(resizable(g,id) && !w->max && x>=w->w-14 && y>=w->h-14) {
            g->resize=id; g->drag_x=w->w-x; g->drag_y=w->h-y; return;
        }
        content_press(g,id,x,y);
        return;
    }
    for(i=0;i<TILES;i++) if(gui_inside(g->x,g->y,24,120+i*52,144,44)) { gui_act(g,tiles[i].app==GUI_FILES?ACT_FILES:ACT_WINDOW+tiles[i].app); return; }
    for(i=0;i<SUITE;i++) {                              /* 10/17/26: the suite's tiles */
        tile_box(i,&x,&y);                              /* 11/05/26 (the review of stage 2): those drawn alone */
        if(x+144<=g->width && gui_inside(g->x,g->y,x,y,144,44)) { gui_act(g,ACT_WINDOW+GUI_FIRST_APP+i); return; }
    }
    for(i=0,id=0;i<g->reg.count;i++) {                  /* 11/05/26: the registry's, after them */
        int k=g->reg.by[i];
        if(!g->reg.e[k].desktop || g->reg.e[k].app>=0) continue;
        tile_box(SUITE+id++,&x,&y);
        if(x+144<=g->width && gui_inside(g->x,g->y,x,y,144,44)) { reg_start(g,k); return; }
    }
}

/* 11/1/26: do keys go to a program's window now? */
bool gui_wants_shifted(const struct gui_state *g) {
    return !g->dialog.kind && !g->menu && client_keys(g);
}

bool gui_wants_arrows(const struct gui_state *g) {
    const struct gui_app *a = gui_app_of(g->focus);
    if(!g->dialog.kind && !g->menu && client_keys(g)) return true;    /* 10/25/26: a program's window's */
    return g->dialog.kind || g->menu || (g->focus >= 0 && !g->win[g->focus].min &&
           (g->focus == GUI_NOTEPAD || g->focus == GUI_FILES || g->focus == GUI_HELP ||
            (a && (a->keys || (a->wants_arrows ? a->wants_arrows(g) : a->arrows)))));
}

/* 10/25/26 (the review): sys/gui.h */
bool gui_working(const struct gui_state *g) {
    return g->viewer.loading || g->hex.searching || g->term.carry_at < g->term.carry_len ||
           g->term.pend_at < g->term.pend_len;
}

/**** keys ****/

/* 11/05/26 (the review): full keys (sys/gui.h). A byte the terminal sent alone: a control byte is Ctrl and its letter
   (or Ctrl+Space, Ctrl+\ ] ^ _), Tab, Enter, Esc and Backspace themselves; LF, Ctrl+J */
int gui_byte_key(int b) {
    if(b == 9 || b == 13 || b == 27 || b == 127) return b;
    if(b == 0) return ' ' | GUI_KEY_CTRL;
    if(b >= 1 && b <= 26) return ('a' + b - 1) | GUI_KEY_CTRL;
    if(b >= 28 && b <= 31) return "\\]^_"[b - 28] | GUI_KEY_CTRL;
    return b;
}

/* the end of an escape sequence (ESC [ or ESC O, a number, the final byte; m: xterm's 1 + Shift + 2 Alt + 4 Ctrl,
   and 16 for a key the keyboard repeats, tty.h): its key, 0 for none. TTY_RAW_KEYS's ESC[c;mu is the character c */
int gui_sequence_key(int number, int final, int m) {
    int key = 0, mods = m > 1 && m <= 32 ? m - 1 : 0;
    switch(final) {
    case 'A': key = GUI_UP; break;
    case 'B': key = GUI_DOWN; break;
    case 'C': key = GUI_RIGHT; break;
    case 'D': key = GUI_LEFT; break;
    case 'H': key = GUI_HOME; break;
    case 'F': key = GUI_END; break;
    case 'Z': return GUI_BACKTAB | GUI_KEY_SHIFT;       /* Shift+Tab, from a terminal at the serial line's end */
    case 'u': if(number == 9 || number == 13 || number == 27 || (number >= 32 && number <= 127)) key = number; break;
    case '~':
        switch(number) {
        case 1: case 7: key = GUI_HOME; break;
        case 2: key = GUI_INSERT; break;
        case 3: key = GUI_DELETE; break;
        case 4: case 8: key = GUI_END; break;
        case 5: key = GUI_PGUP; break;
        case 6: key = GUI_PGDN; break;
        case 15: key = GUI_F(5); break;
        case 17: case 18: case 19: case 20: case 21: key = GUI_F(number - 11); break;
        case 23: case 24: key = GUI_F(number - 12); break;
        }
    }
    if(!key) return 0;
    if(mods & 1) key |= GUI_KEY_SHIFT;
    if(mods & 2) key |= GUI_KEY_ALT;
    if(mods & 4) key |= GUI_KEY_CTRL;
    if(mods & 16) key |= GUI_KEY_REPEAT;
    return key;
}

/* a full key as the desktop's own windows take it, as before the keys had their modifiers: Ctrl and a letter or one of
   @ [ \ ] ^ _ its control byte, Ctrl+Space 0, Ctrl+/ 31, Tab, Enter and Backspace themselves with Ctrl; -1 for any
   other key with Ctrl, Shift+Tab and the function keys; the rest without their modifiers (gui_key() sends an Alt
   key's Esc first, to the Terminal) */
int gui_own_key(int key) {
    int code = key & GUI_KEY_MASK;
    if((code == 9 && (key & GUI_KEY_SHIFT)) || code == GUI_BACKTAB || (code >= GUI_F(1) && code <= GUI_F(12))) return -1;
    if(code >= 0x100 || !(key & GUI_KEY_CTRL)) return code;
    if(code >= 'a' && code <= 'z') return code - 'a' + 1;
    if(code >= '@' && code <= '_') return code & 0x1F;
    if(code == ' ') return 0;
    if(code == '/') return 31;
    if(code == 9 || code == 13 || code == 127) return code;
    return -1;
}

void gui_key(struct gui_state *g, int key) {
    const struct gui_app *app = g->focus >= 0 && !g->win[g->focus].min ? gui_app_of(g->focus) : NULL;
    int rows, i, full = key, code = key & GUI_KEY_MASK;
    bool ctrl = (key & GUI_KEY_CTRL) != 0, alt = (key & GUI_KEY_ALT) != 0;
    bool bar = ctrl ? code == ' ' || code == 'g' || code == 'G' : code == 0 || code == 7;   /* Ctrl+G, Ctrl+Space */
    if(!g->dialog.kind && client_keys(g) && !g->menu && !bar) {     /* 10/25/26: a program's window's, as the Terminal's */
        client_key(g,full);                             /* (11/05/26: every key with its modifiers) */
        return;
    }
    key = gui_own_key(full);                            /* 11/05/26: the desktop's own windows: as before */
    if(key < 0) return;
    if(g->dialog.kind) { dialog_key(g,key); g->dirty=true; return; }
    if(app && app->keys && !g->menu && key!=0 && key!=7) {             /* 10/17/26: every key but Ctrl+G is its own */
        if(app->key) app->key(g,key | (alt && g->focus==GUI_TERMINAL ? GUI_KEY_ALT : 0));    /* 11/05/26: Alt: */
        g->dirty=true;                                  /* the Terminal sends an Esc first, as a terminal does */
        return;
    }
    if(key==17) { gui_request_quit(g); return; }                        /* Ctrl+Q */
    if(key==0 || key==7) { menu_open(g,g->menu?0:1); return; }         /* Ctrl+G (also terminal Ctrl+Space) */
    if(g->menu) { menu_key(g,key); return; }
    switch(key) {
    case 11: gui_act(g,ACT_CALC); return;                               /* Ctrl+K */
    case 4: gui_act(g,ACT_SKETCH); return;                              /* Ctrl+D */
    case 16: gui_act(g,ACT_SETTINGS); return;                           /* Ctrl+P */
    case 5: gui_act(g,ACT_NOTEPAD); return;                             /* Ctrl+E */
    case 2: gui_act(g,ACT_FILES); return;                               /* Ctrl+B */
    case 15: gui_act(g,ACT_OPEN); return;                               /* Ctrl+O */
    case 23: gui_act(g,ACT_CLOSE); return;                              /* Ctrl+W */
    case 14: gui_act(g,g->focus==GUI_SKETCH?ACT_CLEAR_SKETCH:ACT_NEW); return;   /* Ctrl+N */
    case 20: gui_act(g,ACT_WINDOW+GUI_TERMINAL); return;                /* Ctrl+T: 10/17/26, the Terminal */
    }
    if(app && app->key && app->key(g,key)) { g->dirty=true; return; }  /* 10/17/26: the suite's, after the shortcuts */
    if(edit_app(g)) {                                   /* 10/17/26: and its Edit menu's keys (Ctrl+C copies, never leaves) */
        static const int edit_keys[][2] = { {26,ACT_UNDO}, {25,ACT_REDO}, {24,ACT_CUT}, {3,ACT_COPY}, {22,ACT_PASTE},
                                            {1,ACT_SELECT_ALL}, {6,ACT_FIND}, {12,ACT_GOTO} };
        for(i=0;i<(int)(sizeof(edit_keys)/sizeof(edit_keys[0]));i++)
            if(key==edit_keys[i][0]) { gui_act(g,edit_keys[i][1]); return; }
    }
    if(g->focus==GUI_NOTEPAD) {
        switch(key) {
        case 19: gui_act(g,ACT_SAVE); return;                           /* Ctrl+S */
        case 3: gui_act(g,ACT_COPY); return;                            /* Ctrl+C copies here */
        case 22: if(g->note.finding) break; gui_act(g,ACT_PASTE); return;   /* the find bar's own */
        case 1: if(g->note.finding) break; gui_act(g,ACT_SELECT_ALL); return;
        case 24: if(!g->note.finding) gui_act(g,ACT_CUT); return;     /* not under the find bar's keys */
        case 26: if(!g->note.finding) gui_act(g,ACT_UNDO); return;
        case 25: if(!g->note.finding) gui_act(g,ACT_REDO); return;
        case 6: gui_act(g,ACT_FIND); return;
        case 12: gui_act(g,ACT_GOTO); return;
        }
        note_key(g,key);
        return;
    }
    if(key==19 && g->focus==GUI_SKETCH) { gui_act(g,ACT_SKETCH_SAVE); return; } /* 10/17/26: Ctrl+S, Sketch's Save as BMP */
    if(key==3 && g->focus==GUI_FILES) { gui_act(g,ACT_FILES_COPY); return; }   /* Ctrl+C: the path */
    if(key==3) { gui_request_quit(g); return; }                         /* Ctrl+C elsewhere: back to the shell */
    if(key==9) { next_window(g); return; }
    if(key==27) {                                       /* with no window open at all, back to the shell */
        if(g->focus>=0) gui_close(g,g->focus);
        else {
            for(rows=0;rows<GUI_SLOTS && !g->win[rows].open;rows++) ;
            if(rows==GUI_SLOTS) gui_request_quit(g);
        }
        return;
    }
    if(g->focus==GUI_FILES) { files_key(g,key); return; }
    if(g->focus==GUI_HELP) {
        rows=help_rows(&g->win[GUI_HELP]);
        if(key==GUI_UP || key==GUI_DOWN) g->help_top+=key==GUI_UP?-1:1;
        if(key==GUI_PGUP || key==GUI_PGDN) g->help_top+=key==GUI_PGUP?-rows:rows;
        if(key==GUI_HOME) g->help_top=0;
        if(key==GUI_END) g->help_top=HELP_LINES;
        g->help_top=gui_clamp(g->help_top,0,help_max(&g->win[GUI_HELP]));
        g->dirty=true;
        return;
    }
    if(g->focus==GUI_CALC) { gui_calc_key(&g->calc,key); g->dirty=true; }
    if(g->focus==GUI_SETTINGS && !(full&GUI_KEY_REPEAT)) {     /* 11/05/26: each change kept in the settings; */
        if(key>='1' && key<='3') { g->speed=key-'0'; pref_save(g,1); }  /* a key held toggles once (the review */
        if(key=='t' || key=='T') { g->night=!g->night; pref_save(g,0); }    /* of stage 2) */
        g->dirty=true;
    }
}

/**** drawing ****/

/* a window's icon in its title bar and taskbar button: the wave in foam (teal on teal otherwise); 11/05/26: a program
   the desktop started from the registry, its entry's */
static void window_icon(const struct gui_state *g, const struct gui_draw *d, int x, int y, int id) {
    const struct gui_client *c = id >= GUI_FIRST_CLIENT ? client_at(g, id) : NULL;
    int k = c ? reg_of_pid(g, c->st.pid) : -1;
    if(k >= 0) reg_icon(d, x, y, &g->reg.e[k], 1);
    else if(gui_window_icon(g, id) == ICON_WAVE) gd_icon_tint(d, x, y, ICON_WAVE, 1, FOAM);
    else gd_icon(d, x, y, gui_window_icon(g, id), 1);
}

static void title_button(const struct gui_draw *d, int x, int y, int kind, u32 bg) {
    gd_rect(d, x, y, 20, 20, bg);
    if(kind == 0) gd_glyph(d, x + 6, y + 2, 'x', PAPER, 1);
    else if(kind == 1) gd_frame(d, x + 5, y + 5, 10, 10, PAPER);
    else gd_rect(d, x + 5, y + 13, 10, 2, PAPER);
}

static void help_render(const struct gui_state *g, const struct gui_draw *d) {
    const struct gui_window *w = &g->win[GUI_HELP];
    int rows = help_rows(w), i, line;
    for(i = 0; i < rows && g->help_top + i < HELP_LINES; i++) {
        line = g->help_top + i;
        if(help_line(line)[0] == '#') {
            gd_glyph(d, w->x + 14, w->y + TITLE_H + 8 + i * 18, 0xF7, TEAL, 1);
            gd_text(d, w->x + 30, w->y + TITLE_H + 8 + i * 18, help_line(line) + 1, TEAL);
        } else gd_name(d, w->x + 14, w->y + TITLE_H + 8 + i * 18, help_line(line), (w->w - 30 - SCROLL_W) / 8, INK);
    }
    gd_scrollbar(d, w->x + w->w - 2 - SCROLL_W, w->y + TITLE_H, help_bar(w), g->help_top, rows, HELP_LINES);
}

static void window(const struct gui_state *g, const struct gui_draw *d, int id) {
    const struct gui_window *w=&g->win[id];
    struct gui_draw in;
    int x=w->x,y=w->y,i,j,buttons=resizable(g,id)?3:2;
    bool focus=g->focus==id;
    u32 bar=focus?TEAL:MUTED, knob=focus?INK:0x557378;
    char b[96];
    gd_rect(d,x+5,y+6,w->w,w->h,SHADOW);
    gd_rect(d,x,y,w->w,w->h,INK); gd_rect(d,x+1,y+1,w->w-2,w->h-2,PAPER);
    gd_rect(d,x+1,y+1,w->w-2,27,bar);
    window_icon(g,d,x+8,y+6,id);
    title_of(g,id,b,sizeof(b));
    gd_name(d,x+30,y+6,b,(w->w-38-buttons*24)/8,PAPER);
    title_button(d,x+w->w-26,y+4,0,knob);
    if(resizable(g,id)) title_button(d,x+w->w-50,y+4,1,knob);
    title_button(d,x+w->w-(resizable(g,id)?74:50),y+4,2,knob);
    in=gd_clip(d,x+1,y+TITLE_H,w->w-2,w->h-TITLE_H-1);
    if(id==GUI_CALC) {
        gd_rect(&in,x+16,y+44,216,44,INK);
        snprintf(b,sizeof(b),"%ld",(long)g->calc.value);
        gd_text(&in,x+28,y+58,g->calc.error?"Error - press C":b,FOAM);
        if(g->calc.op && !g->calc.error) { b[0]=g->calc.op; b[1]=0; gd_text(&in,x+212,y+58,b,CORAL); }
        for(i=0;i<16;i++) {
            b[0]=keys[i]; b[1]=0;
            gd_button(&in,x+16+(i%4)*54,y+104+(i/4)*40,48,34,b,i%4==3?TEAL:i==14?CORAL:SHELL,INK);
        }
        gd_text(&in,x+16,y+278,"Integers | C clears",MUTED);
    } else if(id==GUI_SKETCH) {
        bool open=sketch_can_open(g), save=g->fs!=NULL;  /* 10/17/26: Open and Save, as BMP files */
        for(i=0;i<6;i++) {
            gd_rect(&in,x+16+i*32,y+38,26,24,i==g->brush?INK:MUTED);
            gd_rect(&in,x+19+i*32,y+41,20,18,sketch_paints[i]);
        }
        gd_button(&in,x+212,y+38,56,24,"Open",open?SHELL:BAND,open?INK:MUTED);
        gd_button(&in,x+272,y+38,56,24,"Save",save?SHELL:BAND,save?INK:MUTED);
        gd_button(&in,x+336,y+38,80,24,"Clear",SHELL,INK);
        gd_rect(&in,x+15,y+69,402,182,MUTED);
        for(j=0;j<GUI_CANVAS_H;j++) {
            for(i=0;i<GUI_CANVAS_W;) {
                int start=i, c=g->canvas[j*GUI_CANVAS_W+i];
                while(i<GUI_CANVAS_W && g->canvas[j*GUI_CANVAS_W+i]==c) i++;
                gd_rect(&in,x+16+start,y+70+j,i-start,1,sketch_paints[c%SKETCH_PAINTS]);
            }
        }
        gd_name(&in,x+16,y+266,g->viewer.sketch.status[0]?g->viewer.sketch.status:   /* 10/17/26: what Save and Open did */
                "Drag/Space draws. Ctrl+N clears, Ctrl+S saves.",GUI_CANVAS_W/8,INK);
    } else if(id==GUI_ABOUT) {
        gd_text_scaled(&in,x+16,y+44,"SurfOS",INK,3);
        gd_wave(&in,x+176,y+52,232,14,FOAM);
        gd_text(&in,x+16,y+104,"A small desktop for catching ideas.",INK);
        gd_text(&in,x+16,y+128,"Write notes, browse files, calculate, sketch.",MUTED);
        for(i=0;i<TILES;i++) gd_button(&in,x+16+i*102,y+166,94,34,tiles[i].label,TEAL,PAPER);
        gd_text(&in,x+16,y+212,"Brandon Burr / 2004 - 2026",MUTED);
    } else if(id==GUI_SETTINGS) {
        snprintf(b,sizeof(b),"%u pointing device%s connected",g->devices,g->devices==1?"":"s");
        gd_text(&in,x+16,y+44,b,INK);
        gd_text(&in,x+16,y+76,"Pointer speed (keys 1, 2, 3)",INK);
        for(i=0;i<3;i++) gd_button(&in,x+16+i*112,y+100,104,30,i==0?"1 / Slow":i==1?"2 / Medium":"3 / Fast",g->speed==i+1?TEAL:SHELL,g->speed==i+1?PAPER:INK);
        gd_text(&in,x+16,y+148,"Desktop colors (T)",INK);
        gd_button(&in,x+16,y+172,176,32,g->night?"Dusk / change":"Ocean / change",TEAL,PAPER);
        gd_name(&in,x+16,y+220,g->pref_status[0]?g->pref_status:"Kept in the settings on the stick, if any.",   /* 11/05/26 */
                (w->w-32)/8,MUTED);
    } else if(id==GUI_NOTEPAD) note_render(g,&in);
    else if(id==GUI_FILES) files_render(g,&in);
    else if(id==GUI_HELP) help_render(g,&in);
    else if(gui_app_of(id)) gui_app_of(id)->render(g,&in);   /* 10/17/26: the suite's */
    else if(id>=GUI_FIRST_CLIENT) client_render(g,&in,id);   /* 10/25/26: a program's pixels */
    if(resizable(g,id) && !w->max)                      /* the grip: a triangle of dots in the corner */
        for(i=0;i<3;i++) for(j=0;j<=i;j++) gd_rect(d,x+w->w-6-j*4,y+w->h-14+i*4,2,2,MUTED);
}

static void menu_render(const struct gui_state *g, const struct gui_draw *d) {
    struct menu_item it[MENU_MAX];
    struct menu_box b;
    int n = menu_items(g, g->menu, it), x, w, i, y, keys;
    bool chosen;
    u32 fg;
    menu_box_of(g, g->menu, it, n, &b);                 /* 10/26/26: where a right-click opened it, when it did */
    x = b.x;
    w = b.w;
    gd_rect(d, x + 4, b.y + 4, w, b.h, NAVY);
    gd_rect(d, x, b.y, w, b.h, INK);
    gd_rect(d, x + 1, b.y + (g->menu_y ? 1 : 0), w - 2, b.h - (g->menu_y ? 2 : 1), PAPER);
    if(b.up) gd_glyph(d, x + w / 2 - 4, b.y + 1, 0x1E, MUTED, 1);   /* 11/05/26: more above, more below */
    if(b.down) gd_glyph(d, x + w / 2 - 4, b.y + b.h - 4 - STRIP + 1, 0x1F, MUTED, 1);
    for(i = b.first; i < b.end; i++) {
        y = entry_top(&b, it, i);
        if(!it[i].label) { gd_rect(d, x + 8, y + 4, w - 16, 1, LINE); continue; }
        chosen = i == g->selection && it[i].on;
        if(chosen) gd_rect(d, x + 3, y, w - 6, 24, TEAL);
        fg = !it[i].on ? 0xA3B4B3 : chosen ? PAPER : INK;
        if(it[i].mark) gd_glyph(d, x + 4, y + 4, it[i].mark == 'v' ? 0xFB : 0x07, fg, 1);   /* 11/05/26: a check, a dot */
        gd_name(d, x + 14, y + 4, it[i].label, (w - 28) / 8 - gd_cells(it[i].keys) - (it[i].sub ? 2 : 0), fg);
        keys = gd_cells(it[i].keys) * 8;
        gd_text(d, x + w - 12 - keys - (it[i].sub ? 16 : 0), y + 4, it[i].keys, chosen ? FOAM : MUTED);
        if(it[i].sub) gd_glyph(d, x + w - 18, y + 4, 0x10, fg, 1);  /* 11/05/26: it opens a menu */
    }
}

void gui_render(const struct gui_state *g,const struct fb_surface *s) {
    struct gui_draw d,desk;
    int i,x,n,w,ids[GUI_SLOTS];
    u32 bg=g->night?0x142638:0x245A67;
    char b[96];
    gd_init(&d,s,g->width,g->height);
    gd_rect(&d,0,0,g->width,g->height,bg);
    /* An oversized horizon and the recurring SurfOS wave mark. */
    for(i=0;i<5;i++) gd_wave(&d,0,g->height-70+i*8,g->width,14,g->night?0x244454:0x3B7980);
    gd_text_scaled(&d,24,54,"SurfOS",FOAM,3);
    for(i=0;i<TILES;i++) {
        gd_rect(&d,24,120+i*52,144,44,INK);
        gd_icon(&d,34,134+i*52,tiles[i].icon,1);
        gd_text(&d,60,134+i*52,tiles[i].label,FOAM);
    }
    for(i=0;i<SUITE;i++) {                              /* 10/17/26: the suite's, in the columns after them */
        int tx, ty;
        tile_box(i,&tx,&ty);
        if(tx+144>g->width) continue;
        gd_rect(&d,tx,ty,144,44,INK);
        gd_icon(&d,tx+10,ty+14,suite[i]->icon,1);
        gd_name(&d,tx+36,ty+14,suite[i]->title,13,FOAM);
    }
    for(i=0,x=0;i<g->reg.count;i++) {                   /* 11/05/26: the registry's, after them, where there is room */
        const struct gui_reg *r=&g->reg.e[g->reg.by[i]];
        int tx, ty;
        if(!r->desktop || r->app>=0) continue;
        tile_box(SUITE+x++,&tx,&ty);
        if(tx+144>g->width) continue;
        gd_rect(&d,tx,ty,144,44,INK);
        reg_icon(&d,tx+10,ty+14,r,1);
        gd_name(&d,tx+36,ty+14,r->title,13,FOAM);
    }
    gd_rect(&d,0,0,g->width,MENU_H,PAPER);
    gd_rect(&d,0,MENU_H-1,g->width,1,LINE);
    n=bar(g,ids);                                       /* 11/05/26: a program's menus in place of File and Edit */
    for(i=0,x=8;i<n;x+=title_w(g,ids[i]),i++) {
        bool open=bar_parent(g, g->menu)==ids[i] && g->menu && !g->menu_y;
        if(open) gd_rect(&d,x-4,2,gd_cells(menu_name(g,ids[i]))*8+16,MENU_H-4,TEAL);
        gd_text(&d,x+4,6,menu_name(g,ids[i]),open?PAPER:menu_greyed(g,ids[i])?MUTED:INK);
    }
    if(x<=g->width-136) gd_text(&d,g->width-128,6,"Ride the wave.",TEAL);
    gd_rect(&d,0,g->height-TASK_H,g->width,TASK_H,NAVY);
    n=task_buttons(g,ids,&w);
    for(i=0;i<n;i++) {
        const struct gui_window *win=&g->win[ids[i]];
        x=6+i*(w+4);
        gd_rect(&d,x,g->height-TASK_H+3,w,TASK_H-6,g->focus==ids[i]?TEAL:win->min?0x1A3B48:INK);
        if(client_waiting(g,ids[i])) gd_rect(&d,x,g->height-6,w,3,CORAL);   /* 10/25/26: opened without the focus */
        window_icon(g,&d,x+4,g->height-TASK_H+4,ids[i]);
        title_of(g,ids[i],b,sizeof(b));
        gd_name(&d,x+24,g->height-TASK_H+5,b,(w-28)/8,win->min?MUTED:FOAM);
    }
    if(!n) gd_text(&d,12,g->height-20,"Ctrl+E Notepad  Ctrl+B Files  Ctrl+K Calc  Ctrl+D Sketch  Ctrl+Q Shell",FOAM);
    if(g->clock[0]) gd_text(&d,g->width-10-(int)strlen(g->clock)*8,g->height-20,g->clock,FOAM);
    desk=gd_clip(&d,0,MENU_H,g->width,g->height-MENU_H-TASK_H);   /* windows and their shadows stay off the bars */
    for(i=0;i<GUI_SLOTS;i++) {
        const struct gui_window *win=&g->win[g->order[i]];
        if(win->open && !win->min) window(g,&desk,g->order[i]);
    }
    if(g->menu) menu_render(g,&d);
    dialog_render(g,&d);
}

void gui_cursor(const struct fb_surface *s,int x,int y,int scale) {
    int row,col;
    static const char *shape[]={"X           ","XX          ","X.X         ","X..X        ","X...X       ","X....X      ","X.....X     ","X......X    ","X.......X   ","X........X  ","X.....XXXXX ","X..X..X     ","X.X X..X    ","XX  X..X    ","X    XX     ",NULL};
    for(row=0;shape[row];row++) for(col=0;shape[row][col];col++)
        if(shape[row][col]!=' ') fb_rect_rgb(s,x+col*scale,y+row*scale,scale,scale,shape[row][col]=='X'?NAVY:PAPER);
}
