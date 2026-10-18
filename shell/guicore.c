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
#define MENU_MAX 32
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
    if(id == GUI_NOTEPAD) note_title(g, out, n);
    else if(a && a->title_of) a->title_of(g, out, n);
    else strlcpy(out, a ? a->title : titles[id], n);
}

static int icon_of(int id) {
    const struct gui_app *a = gui_app_of(id);
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
    "Drag a title bar to move a window. Notepad, Files and",
    "Help also fill the screen (double-click the title bar,",
    "or its middle button) and resize from their corner.",
    "The taskbar shows each open window: click one to bring",
    "it up, or to tuck it away when it is in front.",
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
    "folder, or a file in Notepad. Right and Left open and",
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
static bool resizable(int id) {
    return id == GUI_NOTEPAD || id == GUI_FILES || id == GUI_HELP || (gui_app_of(id) && gui_app_of(id)->resizable);
}
/* a resizable window is never narrower than its toolbar */
static int min_width(int id) {
    const struct gui_app *a = gui_app_of(id);
    if(a) return a->min_width ? a->min_width() : 360;
    return id == GUI_NOTEPAD ? note_min_width() : id == GUI_FILES ? files_min_width() : 360;
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

/**** the calculator ****/

void gui_calc_key(struct gui_calc *c, int key) {
    i64 v;
    if(key=='c' || key=='C') { memset(c,0,sizeof(*c)); return; }
    if(c->error) return;
    if(key>='0' && key<='9') {
        v = (c->fresh ? 0 : (i64)c->value) * 10 + key-'0';
        if(v > 999999999 || v < -999999999) { c->error=true; return; }
        c->value=v; c->fresh=false; c->entered=true; return;
    }
    if(key==8 || key==127) { if(!c->fresh) c->value/=10; return; }
    if(key!='+' && key!='-' && key!='*' && key!='/' && key!='=' && key!='\r' && key!='\n') return;
    if(c->op && c->entered) {
        v=c->operand;
        switch(c->op) {
        case '+': v+=c->value; break;
        case '-': v-=c->value; break;
        case '*': v*=c->value; break;
        case '/': if(!c->value) { c->error=true; return; } v=c->operand/c->value; break;
        }
        if(v > 999999999 || v < -999999999) { c->error=true; return; }
        c->value=v;
    }
    c->operand=c->value; c->fresh=true; c->entered=false;
    c->op=(key=='=' || key=='\r' || key=='\n') ? 0 : key;
}

/**** windows ****/

static void refocus(struct gui_state *g) {
    int i;
    g->focus = -1;
    for(i = GUI_WINDOWS - 1; i >= 0; i--) {
        if(g->win[g->order[i]].open && !g->win[g->order[i]].min) { g->focus = g->order[i]; break; }
    }
    g->dirty = true;
}

void gui_open(struct gui_state *g, int app) {
    int i,j;
    bool was;
    if(app<0 || app>=GUI_WINDOWS) return;
    was = g->win[app].open;
    for(i=0;i<GUI_WINDOWS;i++) if(g->order[i]==app) {
        for(j=i;j<GUI_WINDOWS-1;j++) g->order[j]=g->order[j+1];
        g->order[GUI_WINDOWS-1]=app; break;
    }
    g->win[app].open=true; g->win[app].min=false; g->focus=app; g->menu=0; g->dirty=true;
    if(!was && app == GUI_FILES) files_open(g);
    if(!was && gui_app_of(app) && gui_app_of(app)->opened) gui_app_of(app)->opened(g);
}

static void close_now(struct gui_state *g, int id) {
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
    if(id < 0 || id >= GUI_WINDOWS || !g->win[id].open) return;
    if(id == GUI_NOTEPAD && !gui_guard(g, PEND_CLOSE, NULL)) return;
    a = gui_app_of(id);
    if(a && a->busy && a->busy(g, why, sizeof(why))) {
        dialog_confirm(g, gui_app_act(id, GUI_APP_CLOSE), "Close the window?", why, "It ends when the window closes.",
                       "Close", NULL, "Cancel");
        return;
    }
    close_now(g, id);
}

static void minimize(struct gui_state *g, int id) {
    if(id < 0 || !g->win[id].open) return;
    g->win[id].min = true;
    refocus(g);
}

static void maximize(struct gui_state *g, int id) {
    struct gui_window *w = &g->win[id];
    if(id < 0 || !resizable(id)) return;
    if(w->max) { w->max = false; w->x = w->nx; w->y = w->ny; w->w = w->nw; w->h = w->nh; }
    else { w->nx = w->x; w->ny = w->y; w->nw = w->w; w->nh = w->h; w->max = true; }
    gui_place(g, id);
    g->dirty = true;
}

void gui_place(struct gui_state *g, int id) {
    struct gui_window *w = &g->win[id];
    int room = g->height - MENU_H - TASK_H, top = MENU_H + 6, bottom;
    if(w->max) { w->x = 0; w->y = MENU_H; w->w = g->width; w->h = room; return; }
    if(w->w > g->width) w->w = g->width;
    if(w->h > room) w->h = room;
    bottom = g->height - TASK_H - 2 - w->h;             /* as a drag leaves it */
    w->x = gui_clamp(w->x, 0, g->width - w->w);
    w->y = gui_clamp(w->y, top, bottom > top ? bottom : top);
}

void gui_init(struct gui_state *g, int width, int height) {
    int i, room = height - MENU_H - TASK_H, w, h;
    memset(g,0,sizeof(*g)); g->width=width; g->height=height;
    g->focus=g->drag=g->resize=g->scroll=g->app_drag=-1; g->brush=1; g->speed=1;
    g->title_id=g->files.click_row=g->dialog.click_row=-1;
    g->x=width/2; g->y=height/2;
    for(i=0;i<GUI_WINDOWS;i++) g->order[i]=i;
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

struct menu_item { const char *label, *keys; int act; bool on; };

/* 10/17/26: the focused application, when the Edit menu is its own */
static const struct gui_app *edit_app(const struct gui_state *g) {
    const struct gui_app *app = g->focus >= 0 && !g->win[g->focus].min ? gui_app_of(g->focus) : NULL;
    return app && app->can_edit && app->edit ? app : NULL;
}

/* 10/17/26: room for one more item in a menu of count items above the taskbar (the separators: counted as items) */
static bool item_room(const struct gui_state *g, int count) {
    return MENU_H + 4 + (count + 1) * 24 + 4 <= g->height - TASK_H && count < MENU_MAX;
}

static int menu_items(const struct gui_state *g, int m, struct menu_item *it) {
    const struct gui_note *n = &g->note;
    bool note = g->focus == GUI_NOTEPAD, sel = note && note_has_selection(n), files = g->fs != NULL, any = false;
    int count = 0, i, id;
#define ITEM(l, k, a, e) do { it[count].label = (l); it[count].keys = (k); it[count].act = (a); it[count].on = (e); count++; } while(0)
#define SEPARATOR() do { it[count].label = NULL; it[count].keys = ""; it[count].act = ACT_NONE; it[count].on = false; count++; } while(0)
    for(i = 0; i < GUI_WINDOWS; i++) if(g->win[i].open) any = true;
    switch(m) {
    case 1:
        ITEM("About SurfOS", "", ACT_ABOUT, true);
        ITEM("Pointer & desktop", "Ctrl+P", ACT_SETTINGS, true);
        ITEM("Ocean / dusk colors", "", ACT_THEME, true);
        SEPARATOR();
        ITEM("Return to shell", "Ctrl+Q", ACT_QUIT, true);
        break;
    case 2:
        if(g->focus == GUI_FILES) {                     /* Files in front: its commands, for the keyboard too */
            ITEM("Open selected", "Enter", ACT_FILES_OPEN, files_can(g, ACT_FILES_OPEN));
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
        break;
    case 5:
        ITEM("Next window", note ? "" : "Tab", ACT_NEXT_WINDOW, any);   /* in Notepad, Tab is a tab */
        ITEM("Minimize", "", ACT_MINIMIZE, g->focus >= 0);
        ITEM(g->focus >= 0 && g->win[g->focus].max ? "Restore size" : "Maximize", "", ACT_MAXIMIZE, g->focus >= 0 && resizable(g->focus));
        ITEM("Close", "Ctrl+W", ACT_CLOSE, g->focus >= 0);
        if(any) SEPARATOR();
        /* 10/17/26: the open windows, as many as fit above the taskbar (Next window reaches the rest) */
        for(id = 0; id < GUI_WINDOWS && item_room(g, count); id++)
            if(g->win[id].open) ITEM(gui_app_of(id) ? gui_app_of(id)->title : titles[id],
                                     g->focus == id ? "\x10" : g->win[id].min ? "hidden" : "", ACT_WINDOW + id, true);
        break;
    case 6:
        ITEM("Keyboard and mouse", "", ACT_HELP, true);
        ITEM("About SurfOS", "", ACT_ABOUT, true);
        break;
    }
#undef ITEM
#undef SEPARATOR
    return count;
}

static int menu_left(int m) {
    int x = 8, i;
    for(i = 1; i < m; i++) x += (int)strlen(menu_names[i - 1]) * 8 + 24;
    return x;
}

static int menu_at(int x) {
    int m;
    for(m = 1; m <= MENUS; m++) if(x >= menu_left(m) - 4 && x < menu_left(m + 1) - 4) return m;
    return 0;
}

static int menu_width(const struct menu_item *it, int n) {
    int w = 184, i, k;
    for(i = 0; i < n; i++) {
        if(!it[i].label) continue;
        k = (int)(strlen(it[i].label) + strlen(it[i].keys)) * 8 + 64;
        if(k > w) w = k;
    }
    return w;
}

static int item_top(const struct menu_item *it, int i) {
    int y = MENU_H + 4, k;
    for(k = 0; k < i; k++) y += it[k].label ? 24 : 9;
    return y;
}

/* the item under x, y of the open menu: its number, or -1 */
static int menu_hit(const struct gui_state *g, int x, int y) {
    struct menu_item it[MENU_MAX];
    int n = menu_items(g, g->menu, it), left = menu_left(g->menu) - 4, i;
    if(x < left || x >= left + menu_width(it, n)) return -1;
    for(i = 0; i < n; i++) if(it[i].label && y >= item_top(it, i) && y < item_top(it, i) + 24) return i;
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
    int n = menu_items(g, m, it);
    *x = menu_left(m) - 4;
    *y = MENU_H;
    *w = menu_width(it, n);
    *h = item_top(it, n) - MENU_H + 4;
}

static int menu_step(const struct menu_item *it, int n, int from, int step) {
    int k, i = from;
    for(k = 0; k < n; k++) {
        i = (i + step + n) % n;
        if(it[i].label && it[i].on) return i;
    }
    return from;
}

static void menu_open(struct gui_state *g, int m) {
    struct menu_item it[MENU_MAX];
    int n;
    g->menu = m;
    g->selection = 0;
    if(m) {
        n = menu_items(g, m, it);
        g->selection = it[0].label && it[0].on ? 0 : menu_step(it, n, 0, 1);
    }
    g->dirty = true;
}

static void menu_run(struct gui_state *g, int i) {
    struct menu_item it[MENU_MAX];
    int n = menu_items(g, g->menu, it);
    g->menu = 0;
    g->dirty = true;
    if(i >= 0 && i < n && it[i].label && it[i].on) gui_act(g, it[i].act);
}

static void menu_key(struct gui_state *g, int key) {
    struct menu_item it[MENU_MAX];
    int n = menu_items(g, g->menu, it);
    if(key == 27) g->menu = 0;
    else if(key == GUI_UP || key == GUI_DOWN) g->selection = menu_step(it, n, g->selection, key == GUI_UP ? -1 : 1);
    else if(key == GUI_LEFT || key == GUI_RIGHT) menu_open(g, (g->menu + (key == GUI_RIGHT ? 0 : MENUS - 2)) % MENUS + 1);
    else if(key == '\r' || key == '\n' || key == ' ') { menu_run(g, g->selection); return; }
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
    case PEND_CLOSE: close_now(g, GUI_NOTEPAD); break;
    }
    g->dirty = true;
}

/* 10/17/26 (the suite): an application that is busy (a terminal session) asks first; then Notepad's guard */
void gui_request_quit(struct gui_state *g) {
    char why[80];
    int id;
    for(id = GUI_FIRST_APP; id < GUI_WINDOWS; id++) {
        const struct gui_app *a = gui_app_of(id);
        if(!g->win[id].open || !a->busy || !a->busy(g, why, sizeof(why))) continue;
        dialog_confirm(g, ACT_DLG_LEAVE, "Leave the desktop?", why, "It ends when the desktop closes.", "Leave", NULL,
                       "Cancel");
        return;
    }
    if(gui_guard(g, PEND_QUIT, NULL)) g->quit = true;
}

void gui_tick(struct gui_state *g) {
    int id;
    for(id = GUI_FIRST_APP; id < GUI_WINDOWS; id++)
        if(g->win[id].open && gui_app_of(id)->tick) gui_app_of(id)->tick(g);
}

void gui_finish(struct gui_state *g) {
    int id;
    for(id = GUI_FIRST_APP; id < GUI_WINDOWS; id++)
        if(g->win[id].open && gui_app_of(id)->closed) gui_app_of(id)->closed(g);
}

static void next_window(struct gui_state *g) {
    int i, id;
    for(i = 0; i < GUI_WINDOWS; i++) {                  /* the window at the back comes to the front */
        id = g->order[i];
        if(g->win[id].open && id != g->focus) { gui_open(g, id); return; }
    }
}

void gui_act(struct gui_state *g, int act) {
    struct gui_note *n = &g->note;
    char line[80], dir[GUI_PATH_MAX];
    bool note = g->focus == GUI_NOTEPAD && g->win[GUI_NOTEPAD].open;
    g->dirty = true;
    if(act >= ACT_WINDOW && act < ACT_WINDOW + GUI_WINDOWS) { gui_open(g, act - ACT_WINDOW); return; }
    if(act >= ACT_FILES_OPEN && act <= ACT_FILES_EJECT) { files_act(g, act); return; }
    if(act >= ACT_UNDO && act <= ACT_GOTO && edit_app(g)) {    /* 10/17/26: the focused application's Edit menu */
        if(edit_app(g)->can_edit(g, act)) edit_app(g)->edit(g, act);
        return;
    }
    switch(act) {
    case ACT_ABOUT: gui_open(g, GUI_ABOUT); break;
    case ACT_SETTINGS: gui_open(g, GUI_SETTINGS); break;
    case ACT_THEME: g->night = !g->night; break;
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
    case ACT_CLEAR_SKETCH: memset(g->canvas, 0, sizeof(g->canvas)); gui_open(g, GUI_SKETCH); break;
    case ACT_SKETCH_SAVE: sketch_save_as(g); break;     /* 10/17/26: Sketch's pictures (guibmp.c) */
    case ACT_SKETCH_OPEN: sketch_open(g); break;
    case ACT_NEXT_WINDOW: next_window(g); break;
    case ACT_MINIMIZE: minimize(g, g->focus); break;
    case ACT_MAXIMIZE: maximize(g, g->focus); break;
    case ACT_HELP: gui_open(g, GUI_HELP); break;
    }
}

void gui_answer(struct gui_state *g, int act, int button, const char *text, const char *target) {
    struct gui_entry e;
    bool device, readonly;
    char line[80];
    g->dirty = true;
    if(act >= ACT_APP_DIALOG && act < ACT_APP_DIALOG + 32 * SUITE) {   /* 10/17/26: an application's own dialog */
        const struct gui_app *a = suite[(act - ACT_APP_DIALOG) / 32];
        int id = GUI_FIRST_APP + (act - ACT_APP_DIALOG) / 32;
        if((act - ACT_APP_DIALOG) % 32 == GUI_APP_CLOSE) { if(button == 0 && g->win[id].open) close_now(g, id); }
        else if(a->answer) a->answer(g, (act - ACT_APP_DIALOG) % 32, button, text, target);
        return;
    }
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
    g->last_x=x; g->last_y=y; g->dirty=true;
}

/* the taskbar's buttons: the open windows, by number */
static int task_buttons(const struct gui_state *g, int *ids, int *width) {
    int n = 0, i;
    for(i = 0; i < GUI_WINDOWS; i++) if(g->win[i].open) ids[n++] = i;
    *width = n ? (g->width - 96) / n - 4 : 0;
    if(*width > 156) *width = 156;
    return n;
}

static void taskbar_press(struct gui_state *g) {
    int ids[GUI_WINDOWS], n, w, i;
    n = task_buttons(g, ids, &w);
    for(i = 0; i < n; i++) {
        if(!gui_inside(g->x, g->y, 6 + i * (w + 4), g->height - TASK_H + 3, w, TASK_H - 6)) continue;
        if(g->focus == ids[i]) minimize(g, ids[i]);
        else gui_open(g, ids[i]);
        return;
    }
}

/* Help's text rows, its last useful first row, and its scroll bar (above the resize grip) */
static int help_rows(const struct gui_window *w) { return (w->h - TITLE_H - 12) / 18; }
static int help_max(const struct gui_window *w) { return HELP_LINES > help_rows(w) ? HELP_LINES - help_rows(w) : 0; }
static int help_bar(const struct gui_window *w) { return w->h - TITLE_H - 16; }

static void title_press(struct gui_state *g, int id, int x, int y) {
    struct gui_window *w = &g->win[id];
    int dx = x - g->title_x, dy = y - g->title_y;
    if(x >= w->w - 28) { gui_close(g, id); return; }
    if(resizable(id) && x >= w->w - 52) { maximize(g, id); return; }
    if(x >= w->w - (resizable(id) ? 76 : 52)) { minimize(g, id); return; }
    /* a double-click: a second press in the same place soon after, with no drag between them */
    if(resizable(id) && id == g->title_id && g->now - g->title_ms <= DOUBLE_MS && dx >= -4 && dx <= 4 && dy >= -4 && dy <= 4) {
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
        else if(gui_inside(x,y,336,38,80,24)) memset(g->canvas,0,sizeof(g->canvas));
        else if(gui_inside(x,y,16,70,GUI_CANVAS_W,GUI_CANVAS_H)) {
            g->drawing=true; g->last_x=x-16; g->last_y=y-70; stroke(g,x-16,y-70);
        }
    }
    if(id==GUI_ABOUT && gui_inside(x,y,16,166,4*102-8,34) && (x-16)%102<94)
        gui_act(g,tiles[(x-16)/102].app==GUI_FILES?ACT_FILES:ACT_WINDOW+tiles[(x-16)/102].app);
    if(id==GUI_SETTINGS) {
        if(gui_inside(x,y,16,100,3*112,30)) g->speed=(x-16)/112+1;
        if(gui_inside(x,y,16,172,176,32)) g->night=!g->night;
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
    for(i = GUI_WINDOWS - 1; i >= 0; i--) {
        id = g->order[i];
        w = &g->win[id];
        if(!w->open || w->min || !gui_inside(g->x, g->y, w->x, w->y, w->w, w->h)) continue;
        if(id == GUI_NOTEPAD) note_wheel(g, lines);
        if(id == GUI_FILES) files_wheel(g, lines);
        if(gui_app_of(id) && gui_app_of(id)->wheel) gui_app_of(id)->wheel(g, lines);
        if(id == GUI_HELP) g->help_top = gui_clamp(g->help_top + lines, 0, help_max(w));
        g->dirty = true;
        return;
    }
}

void gui_pointer(struct gui_state *g, const struct pointer_event *e) {
    bool press=(e->buttons&POINTER_LEFT) && !(g->buttons&POINTER_LEFT);
    int i,id,x,y;
    struct gui_window *w;
    g->x=gui_clamp(e->x,0,g->width-1); g->y=gui_clamp(e->y,0,g->height-1); g->buttons=e->buttons;
    if(e->wheel) wheel(g,e->wheel);
    if(!(e->buttons&POINTER_LEFT)) {
        g->drag=g->resize=g->scroll=-1; g->drawing=false; g->note.selecting=false;
        if(g->app_drag>=0) {                            /* 10/17/26: the suite's drag ends */
            id=g->app_drag; g->app_drag=-1;
            if(gui_app_of(id)->release) gui_app_of(id)->release(g);
            g->dirty=true;
        }
    }
    if(g->dialog.kind) { dialog_pointer(g,g->x,g->y,press); return; }
    if(g->drag>=0) {                                    /* below the menu bar, above the taskbar (as gui_place) */
        int top=MENU_H+6, bottom, nx, ny;
        w=&g->win[g->drag];
        bottom=g->height-TASK_H-2-w->h;
        nx=gui_clamp(g->x-g->drag_x,0,g->width-w->w);
        ny=gui_clamp(g->y-g->drag_y,top,bottom>top?bottom:top);
        if(nx!=w->x || ny!=w->y) { w->x=nx; w->y=ny; g->dirty=true; }
        if(g->x-g->press_x>4 || g->press_x-g->x>4 || g->y-g->press_y>4 || g->press_y-g->y>4) g->title_id=-1;   /* a drag is no double-click */
        return;
    }
    if(g->resize>=0) {                                  /* never narrower than its toolbar */
        w=&g->win[g->resize];
        w->w=gui_clamp(g->x-w->x+g->drag_x,min_width(g->resize),g->width-w->x);
        w->h=gui_clamp(g->y-w->y+g->drag_y,200,g->height-TASK_H-2-w->y);   /* the bottom a drag keeps */
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
    if(g->menu && !press) {                             /* the item under the pointer is the one chosen */
        id=g->y<MENU_H?menu_at(g->x):0;
        if(id && id!=g->menu) { menu_open(g,id); return; }
        i=menu_hit(g,g->x,g->y);
        if(i>=0 && i!=g->selection) { g->selection=i; g->dirty=true; }
        return;
    }
    if(!press) return;
    if(g->y<MENU_H) { id=menu_at(g->x); menu_open(g,g->menu==id?0:id); return; }
    if(g->menu) {
        i=menu_hit(g,g->x,g->y);
        if(i>=0) menu_run(g,i);
        else { g->menu=0; g->dirty=true; }
        return;
    }
    if(g->y>=g->height-TASK_H) { taskbar_press(g); return; }
    for(i=GUI_WINDOWS-1;i>=0;i--) {
        id=g->order[i]; w=&g->win[id];
        if(!w->open || w->min || !gui_inside(g->x,g->y,w->x,w->y,w->w,w->h)) continue;
        gui_open(g,id); x=g->x-w->x; y=g->y-w->y;
        if(y<TITLE_H) { title_press(g,id,x,y); return; }
        if(resizable(id) && !w->max && x>=w->w-14 && y>=w->h-14) {
            g->resize=id; g->drag_x=w->w-x; g->drag_y=w->h-y; return;
        }
        content_press(g,id,x,y);
        return;
    }
    for(i=0;i<TILES;i++) if(gui_inside(g->x,g->y,24,120+i*52,144,44)) { gui_act(g,tiles[i].app==GUI_FILES?ACT_FILES:ACT_WINDOW+tiles[i].app); return; }
    for(i=0;i<SUITE;i++) {                              /* 10/17/26: the suite's tiles */
        tile_box(i,&x,&y);
        if(gui_inside(g->x,g->y,x,y,144,44)) { gui_act(g,ACT_WINDOW+GUI_FIRST_APP+i); return; }
    }
}

bool gui_wants_arrows(const struct gui_state *g) {
    const struct gui_app *a = gui_app_of(g->focus);
    return g->dialog.kind || g->menu || (g->focus >= 0 && !g->win[g->focus].min &&
           (g->focus == GUI_NOTEPAD || g->focus == GUI_FILES || g->focus == GUI_HELP || (a && (a->arrows || a->keys))));
}

/**** keys ****/

void gui_key(struct gui_state *g, int key) {
    const struct gui_app *app = g->focus >= 0 && !g->win[g->focus].min ? gui_app_of(g->focus) : NULL;
    int rows, i;
    if(g->dialog.kind) { dialog_key(g,key); g->dirty=true; return; }
    if(app && app->keys && !g->menu && key!=0 && key!=7) {             /* 10/17/26: every key but Ctrl+G is its own */
        if(app->key) app->key(g,key);
        g->dirty=true;
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
            for(rows=0;rows<GUI_WINDOWS && !g->win[rows].open;rows++) ;
            if(rows==GUI_WINDOWS) gui_request_quit(g);
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
    if(g->focus==GUI_SETTINGS) {
        if(key>='1' && key<='3') g->speed=key-'0';
        if(key=='t' || key=='T') g->night=!g->night;
        g->dirty=true;
    }
}

/**** drawing ****/

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
    int x=w->x,y=w->y,i,j,buttons=resizable(id)?3:2;
    bool focus=g->focus==id;
    u32 bar=focus?TEAL:MUTED, knob=focus?INK:0x557378;
    char b[96];
    gd_rect(d,x+5,y+6,w->w,w->h,SHADOW);
    gd_rect(d,x,y,w->w,w->h,INK); gd_rect(d,x+1,y+1,w->w-2,w->h-2,PAPER);
    gd_rect(d,x+1,y+1,w->w-2,27,bar);
    if(icon_of(id)==ICON_WAVE) gd_icon_tint(d,x+8,y+6,ICON_WAVE,1,FOAM);   /* teal on teal otherwise */
    else gd_icon(d,x+8,y+6,icon_of(id),1);
    title_of(g,id,b,sizeof(b));
    gd_name(d,x+30,y+6,b,(w->w-38-buttons*24)/8,PAPER);
    title_button(d,x+w->w-26,y+4,0,knob);
    if(resizable(id)) title_button(d,x+w->w-50,y+4,1,knob);
    title_button(d,x+w->w-(resizable(id)?74:50),y+4,2,knob);
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
        gd_text(&in,x+16,y+220,"Changes last for this desktop session.",MUTED);
    } else if(id==GUI_NOTEPAD) note_render(g,&in);
    else if(id==GUI_FILES) files_render(g,&in);
    else if(id==GUI_HELP) help_render(g,&in);
    else if(gui_app_of(id)) gui_app_of(id)->render(g,&in);   /* 10/17/26: the suite's */
    if(resizable(id) && !w->max)                        /* the grip: a triangle of dots in the corner */
        for(i=0;i<3;i++) for(j=0;j<=i;j++) gd_rect(d,x+w->w-6-j*4,y+w->h-14+i*4,2,2,MUTED);
}

static void menu_render(const struct gui_state *g, const struct gui_draw *d) {
    struct menu_item it[MENU_MAX];
    int n = menu_items(g, g->menu, it), x = menu_left(g->menu) - 4, w = menu_width(it, n), h = item_top(it, n) - MENU_H + 4, i, y;
    gd_rect(d, x + 4, MENU_H + 4, w, h, NAVY);
    gd_rect(d, x, MENU_H, w, h, INK);
    gd_rect(d, x + 1, MENU_H, w - 2, h - 1, PAPER);
    for(i = 0; i < n; i++) {
        y = item_top(it, i);
        if(!it[i].label) { gd_rect(d, x + 8, y + 4, w - 16, 1, LINE); continue; }
        if(i == g->selection && it[i].on) gd_rect(d, x + 3, y, w - 6, 24, TEAL);
        gd_text(d, x + 14, y + 4, it[i].label, !it[i].on ? 0xA3B4B3 : i == g->selection ? PAPER : INK);
        gd_text(d, x + w - 12 - (int)strlen(it[i].keys) * 8, y + 4, it[i].keys, i == g->selection && it[i].on ? FOAM : MUTED);
    }
}

void gui_render(const struct gui_state *g,const struct fb_surface *s) {
    struct gui_draw d,desk;
    int i,x,n,w,ids[GUI_WINDOWS];
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
    gd_rect(&d,0,0,g->width,MENU_H,PAPER);
    gd_rect(&d,0,MENU_H-1,g->width,1,LINE);
    for(i=1;i<=MENUS;i++) {
        x=menu_left(i);
        if(g->menu==i) gd_rect(&d,x-4,2,(int)strlen(menu_names[i-1])*8+16,MENU_H-4,TEAL);
        gd_text(&d,x+4,6,menu_names[i-1],g->menu==i?PAPER:INK);
    }
    gd_text(&d,g->width-128,6,"Ride the wave.",TEAL);
    gd_rect(&d,0,g->height-TASK_H,g->width,TASK_H,NAVY);
    n=task_buttons(g,ids,&w);
    for(i=0;i<n;i++) {
        const struct gui_window *win=&g->win[ids[i]];
        x=6+i*(w+4);
        gd_rect(&d,x,g->height-TASK_H+3,w,TASK_H-6,g->focus==ids[i]?TEAL:win->min?0x1A3B48:INK);
        if(icon_of(ids[i])==ICON_WAVE) gd_icon_tint(&d,x+4,g->height-TASK_H+4,ICON_WAVE,1,FOAM);
        else gd_icon(&d,x+4,g->height-TASK_H+4,icon_of(ids[i]),1);
        title_of(g,ids[i],b,sizeof(b));
        gd_name(&d,x+24,g->height-TASK_H+5,b,(w-28)/8,win->min?MUTED:FOAM);
    }
    if(!n) gd_text(&d,12,g->height-20,"Ctrl+E Notepad  Ctrl+B Files  Ctrl+K Calc  Ctrl+D Sketch  Ctrl+Q Shell",FOAM);
    if(g->clock[0]) gd_text(&d,g->width-10-(int)strlen(g->clock)*8,g->height-20,g->clock,FOAM);
    desk=gd_clip(&d,0,MENU_H,g->width,g->height-MENU_H-TASK_H);   /* windows and their shadows stay off the bars */
    for(i=0;i<GUI_WINDOWS;i++) {
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
