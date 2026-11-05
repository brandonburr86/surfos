/*
SurfOS Desktop Dialogs
--------------------
File: guidialog.c Date: 10/17/26
--------------------
One dialog at a time, in front of everything, taking every key and click: a
message, a question with up to three buttons, a line to type, and the choosers
of Open and Save as (a folder's entries, .. to go up, a name to type). Its answer
goes to gui_answer() after the dialog has closed, so the answer may open another.
Also the one-line text box that dialogs and Notepad's find bar use.
10/25/26 (programs' windows, stage C6): the launcher's chooser, GUI_DLG_RUN: the programs of one
folder (it never leaves it) above the line to run, which a program picked in the list replaces;
its answer is that line as typed.
11/05/26 (the desktop applications, track desk): the choosers for a program (WIN_DIALOG): its own
title, the file types it is for (Open lists those files and the folders; a Save as name without one
of them gets the first), and Choose folder (GUI_DLG_FOLDER: the folders only; Enter or Choose takes
the folder selected, or the one shown while .. is; Right or a double-click goes into one). A
program's chooser, or the screenshot's, that another dialog replaces before it is answered is told
so (client_chooser_dropped()), so that nothing waits for it for ever.
*/
#include "guipriv.h"
#include <string.h>
#include <stdio.h>

/**** the text box ****/

static bool cont(u8 c) { return (c & 0xC0) == 0x80; }

static u32 field_prev(const struct gui_field *f, u32 at) {
    if(!at) return 0;
    for(at--; at > 0 && cont((u8)f->text[at]); at--) ;
    return at;
}

static u32 field_next(const struct gui_field *f, u32 at) {
    if(at >= f->len) return f->len;
    for(at++; at < f->len && cont((u8)f->text[at]); at++) ;
    return at;
}

void field_set(struct gui_field *f, const char *text, bool all) {
    strlcpy(f->text, text, sizeof(f->text));
    f->len = f->cursor = strlen(f->text);
    f->all = all && f->len;
}

static void field_insert(struct gui_field *f, const char *s, u32 n) {
    u32 i;
    if(n > sizeof(f->text) - 1 - f->len) n = sizeof(f->text) - 1 - f->len;
    memmove(f->text + f->cursor + n, f->text + f->cursor, f->len - f->cursor + 1);
    for(i = 0; i < n; i++) f->text[f->cursor + i] = (u8)s[i] < 32 ? ' ' : s[i];
    f->len += n;
    f->cursor += n;
}

bool field_key(struct gui_field *f, int key, const char *clip, u32 clip_len) {
    u32 k;
    char c;
    bool text = key == 22 || (key >= 32 && key < 256 && key != 127);
    if(key == 1) { f->all = f->len > 0; f->cursor = f->len; return true; }   /* Ctrl+A */
    if(f->all && (text || key == 8 || key == 127 || key == GUI_DELETE)) {
        f->text[0] = 0;                                 /* typing replaces what is selected */
        f->len = f->cursor = 0;
        f->all = false;
        if(!text) return true;
    }
    switch(key) {
    case GUI_LEFT: f->cursor = f->all ? 0 : field_prev(f, f->cursor); f->all = false; return true;
    case GUI_RIGHT: f->cursor = f->all ? f->len : field_next(f, f->cursor); f->all = false; return true;
    case GUI_HOME: f->cursor = 0; f->all = false; return true;
    case GUI_END: f->cursor = f->len; f->all = false; return true;
    case 8: case 127:
        k = field_prev(f, f->cursor);
        memmove(f->text + k, f->text + f->cursor, f->len - f->cursor + 1);
        f->len -= f->cursor - k;
        f->cursor = k;
        return true;
    case GUI_DELETE:
        k = field_next(f, f->cursor);
        memmove(f->text + f->cursor, f->text + k, f->len - k + 1);
        f->len -= k - f->cursor;
        return true;
    case 22:                                            /* Ctrl+V: the clipboard's first line */
        for(k = 0; k < clip_len && clip[k] != '\n' && clip[k] != '\r'; k++) ;
        field_insert(f, clip, k);
        return true;
    }
    if(!text) return false;
    c = (char)key;
    field_insert(f, &c, 1);
    return true;
}

void field_render(const struct gui_draw *d, const struct gui_field *f, int x, int y, int w, bool focused) {
    struct gui_draw in;
    int cells = (w - 8) / 8, before = 0, col, px;
    u32 i, len, start = 0;
    gd_rect(d, x, y, w, 22, focused ? TEAL : LINE);
    gd_rect(d, x + 1, y + 1, w - 2, 20, 0xFFFDF5);
    in = gd_clip(d, x + 2, y + 1, w - 4, 20);
    for(i = 0; i < f->cursor; i += len) { gd_char(f->text + i, f->len - i, &len); before++; }
    while(before >= cells && start < f->len) {          /* the cursor stays in view */
        gd_char(f->text + start, f->len - start, &len);
        start += len;
        before--;
    }
    for(i = start, col = 0; i < f->len && col < cells; i += len, col++) {
        u8 glyph = gd_char(f->text + i, f->len - i, &len);
        px = x + 4 + col * 8;
        if(f->all) gd_rect(&in, px, y + 3, 8, 16, PICK);
        gd_glyph(&in, px, y + 3, glyph, INK, 1);
    }
    if(focused && !f->all) gd_rect(&in, x + 4 + before * 8 - 1, y + 3, 2, 16, INK);
}

/**** dialogs ****/

struct dlg_layout { int x, y, w, h, list_y, rows, field_y, buttons_y, up_x; };

static bool chooser(const struct gui_dialog *dl) {
    return dl->kind == GUI_DLG_OPEN || dl->kind == GUI_DLG_SAVE || dl->kind == GUI_DLG_RUN || dl->kind == GUI_DLG_FOLDER;
}

/* 11/05/26: s is t in any case (t in lower case, as surfos/win.h's win_types() gives it) */
static bool same_name(const char *s, const char *t) {
    for(; *s && *t; s++, t++) if((*s >= 'A' && *s <= 'Z' ? *s + 32 : *s) != *t) return false;
    return !*s && !*t;
}

/* 11/05/26: name is of one of types (a program's: "png bmp =Makefile", read by surfos/win.h's win_types(), which the
   kernel checked them with): its extension, or the whole name for an '=' type, in any case */
static bool type_listed(const char *types, const char *name) {
    char list[WIN_TYPES][WIN_TYPE_LEN + 2];
    const char *ext = strrchr(name, '.');
    int n = win_types(types, (unsigned)strlen(types) + 1, list), i;
    for(i = 0; i < n; i++)
        if(list[i][0] == '=' ? same_name(name, list[i] + 1) : ext && ext != name && same_name(ext + 1, list[i])) return true;
    return false;
}

/* the first extension of types into out, "" when it gives only whole names */
static void first_type(const char *types, char *out, size_t size) {
    char list[WIN_TYPES][WIN_TYPE_LEN + 2];
    int n = win_types(types, (unsigned)strlen(types) + 1, list), i;
    out[0] = 0;
    for(i = 0; i < n; i++) if(list[i][0] != '=') { strlcpy(out, list[i], size); return; }
}

/* 10/25/26: a chooser with a line to type under its list: Save's name, the launcher's command */
static bool named(const struct gui_dialog *dl) {
    return dl->kind == GUI_DLG_SAVE || dl->kind == GUI_DLG_RUN;
}

/* 11/05/26: the list of a program's chooser: Choose folder's folders only, a typed chooser's folders and its types'
   files (the rest are not counted as not shown either) */
static void filter(struct gui_dialog *dl) {
    int i, k = 0, gone = 0;
    if(dl->kind != GUI_DLG_FOLDER && !dl->types[0]) return;
    for(i = 0; i < dl->count; i++) {
        bool keep = dl->list[i].type == GUI_DIR || (dl->kind != GUI_DLG_FOLDER && type_listed(dl->types, dl->list[i].name));
        if(keep) {
            if(k != i) dl->list[k] = dl->list[i];
            k++;
        } else gone++;
    }
    dl->count = k;
    dl->total -= gone;
}

static int has_up(const struct gui_dialog *dl) {
    return dl->kind != GUI_DLG_RUN && strcmp(dl->dir, "/") != 0;      /* the launcher stays in its folder */
}

static void layout(const struct gui_state *g, struct dlg_layout *l) {
    const struct gui_dialog *dl = &g->dialog;
    int room = g->height - MENU_H - TASK_H;
    l->w = chooser(dl) ? 520 : 470;
    if(l->w > g->width - 24) l->w = g->width - 24;
    l->h = chooser(dl) ? (room - 24 < 380 ? room - 24 : 380) : dl->kind == GUI_DLG_INPUT ? 160 : 146;
    l->x = (g->width - l->w) / 2;
    l->y = MENU_H + (room - l->h) / 2;
    l->buttons_y = l->h - 40;
    l->up_x = l->w - 16 - 56;
    l->list_y = 64;
    l->field_y = named(dl) ? l->h - 74 : dl->kind == GUI_DLG_INPUT ? 66 : 0;
    /* 10/25/26: a named chooser's list ends above its line's label (- 18) */
    l->rows = ((named(dl) ? l->field_y - 18 : l->buttons_y) - 8 - l->list_y) / ROW_H;
    if(l->rows < 1) l->rows = 1;
}

void dialog_box(const struct gui_state *g, int *x, int *y, int *w, int *h) {
    struct dlg_layout l;
    layout(g, &l);
    *x = l.x; *y = l.y; *w = l.w; *h = l.h;
}

/* where button i is (the buttons with a label, at the right of the box); false: there is none */
static bool button_place(const struct gui_dialog *dl, const struct dlg_layout *l, int i, int *bx, int *bw) {
    int k, x = l->x + l->w - 16, w;
    if(i < 0 || i > 2 || !dl->label[i]) return false;
    for(k = 2; k >= i; k--) {
        if(!dl->label[k]) continue;
        w = (int)strlen(dl->label[k]) * 8 + 32;
        if(w < 88) w = 88;
        x -= w;
        if(k == i) { *bx = x; *bw = w; return true; }
        x -= 8;
    }
    return false;
}

static void begin(struct gui_state *g, int kind, int act, const char *title) {
    struct gui_dialog *dl = &g->dialog;
    /* 10/18/26: a dialog replaced before it is answered (a message from an application's tick) while what Notepad's
       question held back waits takes that along: the question itself, or the chooser, the Replace or the read-only
       question its Save led to. A later save then does not do it. (An answer closes its dialog before what it
       leads to opens, so the question's own flow keeps it.) */
    if(dl->kind && g->pending != PEND_NONE) g->pending = PEND_NONE;
    if(dl->kind) client_chooser_dropped(g, dl->act, dl->target);   /* 11/05/26: a program's chooser, never answered */
    dl->kind = kind;
    dl->act = act;
    dl->choice = 0;
    dl->list_focus = false;
    dl->types[0] = 0;                                   /* 11/05/26: every file, unless a program's says */
    strlcpy(dl->title, title, sizeof(dl->title));
    dl->text[0][0] = dl->text[1][0] = 0;
    dl->label[0] = "OK";
    dl->label[1] = dl->label[2] = NULL;
    dl->target[0] = 0;
    field_set(&dl->field, "", false);
    g->menu = 0;
    g->drag = g->resize = g->scroll = -1;
    g->drawing = g->note.selecting = false;
    g->dirty = true;
}

void dialog_close(struct gui_state *g) {
    g->dialog.kind = 0;
    g->dirty = true;
}

/* the dialog closes, then its action gets the answer */
static void answer(struct gui_state *g, int button, const char *text) {
    struct gui_dialog *dl = &g->dialog;
    char said[GUI_PATH_MAX], about[GUI_PATH_MAX];
    int act = dl->act;
    strlcpy(said, text, sizeof(said));
    strlcpy(about, dl->target, sizeof(about));
    dialog_close(g);
    gui_answer(g, act, button, said, about);
}

void dialog_message(struct gui_state *g, const char *title, const char *line1, const char *line2) {
    begin(g, GUI_DLG_MESSAGE, ACT_NONE, title);
    strlcpy(g->dialog.text[0], line1, sizeof(g->dialog.text[0]));
    strlcpy(g->dialog.text[1], line2, sizeof(g->dialog.text[1]));
}

void dialog_confirm(struct gui_state *g, int act, const char *title, const char *line1, const char *line2,
                    const char *yes, const char *no, const char *cancel) {
    begin(g, GUI_DLG_CONFIRM, act, title);
    strlcpy(g->dialog.text[0], line1, sizeof(g->dialog.text[0]));
    strlcpy(g->dialog.text[1], line2, sizeof(g->dialog.text[1]));
    g->dialog.label[0] = yes;
    g->dialog.label[1] = no;
    g->dialog.label[2] = cancel;
}

void dialog_input(struct gui_state *g, int act, const char *title, const char *prompt, const char *text) {
    begin(g, GUI_DLG_INPUT, act, title);
    strlcpy(g->dialog.text[0], prompt, sizeof(g->dialog.text[0]));
    field_set(&g->dialog.field, text, true);
    g->dialog.label[2] = "Cancel";
}

/* a chooser's folder: read it, or stay where it was when it cannot be read (and at / as a last resort) */
static void navigate(struct gui_state *g, const char *to) {
    struct gui_dialog *dl = &g->dialog;
    char old[GUI_PATH_MAX], dir[GUI_PATH_MAX];
    int n = -ENOSYS, tries;
    strlcpy(old, dl->dir, sizeof(old));
    strlcpy(dir, to, sizeof(dir));
    dl->text[0][0] = 0;
    for(tries = 0; tries < 3; tries++) {
        strlcpy(dl->dir, dir, sizeof(dl->dir));
        n = g->fs ? gui_list(g->fs, dl->dir, dl->list, GUI_LIST_MAX, &dl->total) : -ENOSYS;
        if(n >= 0) break;
        if(!tries) snprintf(dl->text[0], sizeof(dl->text[0]), "%s: %s", gui_basename(dir), gui_error(g, n));
        if(dl->kind == GUI_DLG_RUN) break;              /* 10/25/26: the launcher's list is its folder's, or none */
        strlcpy(dir, tries == 0 && old[0] ? old : "/", sizeof(dir));
    }
    dl->count = n < 0 ? 0 : n;
    if(n < 0) dl->total = 0;
    filter(dl);                                         /* 11/05/26: a program's chooser's types, or folders */
    dl->selected = dl->top = 0;
    dl->click_row = -1;
    dl->list_focus = false;                             /* in a new folder, Enter is for the name again */
    g->dirty = true;
}

static void show_row(struct gui_state *g);

void dialog_chooser(struct gui_state *g, int kind, int act, const char *dir, const char *name) {
    struct gui_dialog *dl = &g->dialog;
    char start[GUI_PATH_MAX];
    int i;
    strlcpy(start, dir && dir[0] ? dir : "/", sizeof(start));        /* dir may be the last chooser's own */
    begin(g, kind, act, kind == GUI_DLG_OPEN ? "Open a file" : kind == GUI_DLG_RUN ? "Run a program" : "Save as");
    g->dialog.label[0] = kind == GUI_DLG_OPEN ? "Open" : kind == GUI_DLG_RUN ? "Run" : "Save";
    g->dialog.label[2] = "Cancel";
    g->dialog.dir[0] = 0;
    navigate(g, start);
    if(named(dl)) field_set(&dl->field, name, true);
    if(kind != GUI_DLG_RUN) return;
    for(i = 0; i < dl->count; i++)                      /* 10/25/26: the program offered, selected in view */
        if(dl->list[i].type != GUI_DIR && !strcmp(dl->list[i].name, name)) {
            dl->selected = i;
            show_row(g);
            break;
        }
}

static void show_row(struct gui_state *g) {
    struct gui_dialog *dl = &g->dialog;
    struct dlg_layout l;
    int rows = dl->count + has_up(dl);
    layout(g, &l);
    dl->selected = gui_clamp(dl->selected, 0, rows - 1);
    if(dl->selected < dl->top) dl->top = dl->selected;
    if(dl->selected >= dl->top + l.rows) dl->top = dl->selected - l.rows + 1;
    dl->top = gui_clamp(dl->top, 0, rows > l.rows ? rows - l.rows : 0);
    g->dirty = true;
}

static void go_up(struct gui_state *g) {
    char path[GUI_PATH_MAX];
    strlcpy(path, g->dialog.dir, sizeof(path));
    gui_parent(path);
    navigate(g, path);
}

/* 11/05/26: a chooser for a program (guiclient.c, WIN_DIALOG) */
void dialog_chooser_for(struct gui_state *g, int kind, int act, const char *title, const char *dir, const char *name,
                        const char *types) {
    struct gui_dialog *dl = &g->dialog;
    char start[GUI_PATH_MAX];
    strlcpy(start, dir && dir[0] ? dir : g->dialog.dir[0] ? g->dialog.dir : "/", sizeof(start));
    begin(g, kind, act, title && title[0] ? title : kind == GUI_DLG_OPEN ? "Open a file" :
                        kind == GUI_DLG_FOLDER ? "Choose a folder" : "Save as");
    dl->label[0] = kind == GUI_DLG_OPEN ? "Open" : kind == GUI_DLG_FOLDER ? "Choose" : "Save";
    dl->label[2] = "Cancel";
    strlcpy(dl->types, kind == GUI_DLG_FOLDER ? "" : types, sizeof(dl->types));
    dl->dir[0] = 0;
    navigate(g, start);
    if(named(dl)) field_set(&dl->field, name, true);
}

/* the selected row was picked in a Save chooser (or the launcher's): a file's name goes in the box */
static void picked(struct gui_state *g) {
    struct gui_dialog *dl = &g->dialog;
    int i = dl->selected - has_up(dl);
    if(named(dl) && i >= 0 && i < dl->count && dl->list[i].type != GUI_DIR)
        field_set(&dl->field, dl->list[i].name, false);
}

static void too_long(struct gui_state *g) {
    strlcpy(g->dialog.text[0], "That path is too long for SurfOS.", sizeof(g->dialog.text[0]));
    g->dirty = true;
}

/* 11/05/26: Choose folder's Enter or Choose: the folder selected, or the one shown while .. is selected */
static void choose_folder(struct gui_state *g) {
    struct gui_dialog *dl = &g->dialog;
    char path[GUI_PATH_MAX];
    int i = dl->selected - has_up(dl);
    if(i >= 0 && i < dl->count && dl->list[i].type == GUI_DIR) {
        if(!gui_join(path, sizeof(path), dl->dir, dl->list[i].name)) { too_long(g); return; }
        answer(g, 0, path);
    } else answer(g, 0, dl->dir);
}

/* 11/05/26: into the folder selected (Choose folder's Right and double-click), or up for .. */
static void folder_into(struct gui_state *g) {
    struct gui_dialog *dl = &g->dialog;
    char path[GUI_PATH_MAX];
    int up = has_up(dl), i = dl->selected - up;
    if(up && dl->selected == 0) { go_up(g); return; }
    if(i < 0 || i >= dl->count || dl->list[i].type != GUI_DIR) return;
    if(!gui_join(path, sizeof(path), dl->dir, dl->list[i].name)) { too_long(g); return; }
    navigate(g, path);
}

/* Enter, a double-click or the Open/Save button */
static void choose(struct gui_state *g) {
    struct gui_dialog *dl = &g->dialog;
    char path[GUI_PATH_MAX];
    int up = has_up(dl), i;
    if(dl->kind == GUI_DLG_FOLDER) { choose_folder(g); return; }
    if(dl->kind == GUI_DLG_OPEN || dl->list_focus) {
        i = dl->selected - up;
        if(up && dl->selected == 0) { go_up(g); return; }
        if(i >= 0 && i < dl->count) {
            if(!gui_join(path, sizeof(path), dl->dir, dl->list[i].name)) { too_long(g); return; }
            if(dl->list[i].type == GUI_DIR) { if(dl->kind != GUI_DLG_RUN) navigate(g, path); return; }
            if(dl->kind == GUI_DLG_OPEN) { answer(g, 0, path); return; }
            field_set(&dl->field, dl->list[i].name, false);
        } else if(dl->kind == GUI_DLG_OPEN) return;
    }
    if(!dl->field.len) return;
    if(dl->kind == GUI_DLG_RUN) { answer(g, 0, dl->field.text); return; }   /* 10/25/26: the line, as it is */
    if(strchr(dl->field.text, '/') || !strcmp(dl->field.text, ".") || !strcmp(dl->field.text, "..")) {
        strlcpy(dl->text[0], "That is not a name for a file.", sizeof(dl->text[0]));
        g->dirty = true;
        return;
    }
    for(i = 0; i < dl->count; i++) {
        if(dl->list[i].type != GUI_DIR || strcmp(dl->list[i].name, dl->field.text)) continue;
        if(!gui_join(path, sizeof(path), dl->dir, dl->field.text)) { too_long(g); return; }   /* a folder's name: into it */
        field_set(&dl->field, "", false);
        navigate(g, path);
        return;
    }
    if(dl->types[0] && !type_listed(dl->types, dl->field.text)) {    /* 11/05/26: a name of one of its types */
        char ext[WIN_TYPE_LEN + 2], name[GUI_PATH_MAX];
        first_type(dl->types, ext, sizeof(ext));
        if(ext[0]) {                                    /* (only whole names: the name as it is) */
            if(snprintf(name, sizeof(name), "%s.%s", dl->field.text, ext) >= (int)sizeof(dl->field.text)) { too_long(g); return; }
            field_set(&dl->field, name, false);
        }
    }
    if(!gui_join(path, sizeof(path), dl->dir, dl->field.text)) { too_long(g); return; }
    answer(g, 0, path);
}

static void chooser_key(struct gui_state *g, int key) {
    struct gui_dialog *dl = &g->dialog;
    struct dlg_layout l;
    bool list = dl->kind == GUI_DLG_OPEN || dl->kind == GUI_DLG_FOLDER || dl->list_focus;
    layout(g, &l);
    if(dl->kind == GUI_DLG_FOLDER) {                    /* 11/05/26: Right goes in, Left and Backspace up */
        if(key == GUI_RIGHT) { folder_into(g); return; }
        if((key == GUI_LEFT || key == 8 || key == 127) && has_up(dl)) { go_up(g); return; }
    }
    switch(key) {
    case GUI_UP: dl->selected--; dl->list_focus = true; break;
    case GUI_DOWN: dl->selected++; dl->list_focus = true; break;
    case GUI_PGUP: dl->selected -= l.rows - 1; dl->list_focus = true; break;
    case GUI_PGDN: dl->selected += l.rows - 1; dl->list_focus = true; break;
    case '\r': case '\n': choose(g); return;
    case 9: if(named(dl)) dl->list_focus = !dl->list_focus; g->dirty = true; return;
    default:
        if(list && (key == GUI_HOME || key == GUI_END)) { dl->selected = key == GUI_HOME ? 0 : dl->count + has_up(dl) - 1; break; }
        if(dl->kind == GUI_DLG_OPEN || dl->kind == GUI_DLG_FOLDER) {
            if((key == 8 || key == 127) && has_up(dl)) go_up(g);
            return;
        }
        if(field_key(&dl->field, key, g->clip, g->clip_len)) dl->list_focus = false;
        g->dirty = true;
        return;
    }
    show_row(g);
    picked(g);
}

void dialog_key(struct gui_state *g, int key) {
    struct gui_dialog *dl = &g->dialog;
    int k;
    if(key == 27) { answer(g, -1, ""); return; }
    switch(dl->kind) {
    case GUI_DLG_MESSAGE:
        if(key == '\r' || key == '\n' || key == ' ') answer(g, 0, "");
        return;
    case GUI_DLG_CONFIRM:
        if(key == GUI_LEFT || key == GUI_RIGHT || key == 9) {
            for(k = 0; k < 3; k++) {                    /* the next button that has a label */
                dl->choice = (dl->choice + (key == GUI_LEFT ? 2 : 1)) % 3;
                if(dl->label[dl->choice]) break;
            }
            g->dirty = true;
        } else if(key == '\r' || key == '\n' || key == ' ') answer(g, dl->choice == 2 ? -1 : dl->choice, "");
        return;
    case GUI_DLG_INPUT:
        if(key == '\r' || key == '\n') answer(g, 0, dl->field.text);
        else if(field_key(&dl->field, key, g->clip, g->clip_len)) g->dirty = true;
        return;
    default:
        chooser_key(g, key);
    }
}

void dialog_pointer(struct gui_state *g, int x, int y, bool press) {
    struct gui_dialog *dl = &g->dialog;
    struct dlg_layout l;
    int i, bx, bw, row, rows, top, size;
    if(!press) return;
    layout(g, &l);
    if(!gui_inside(x, y, l.x, l.y, l.w, l.h)) return;    /* the rest of the screen waits */
    for(i = 0; i < 3; i++) {
        if(!button_place(dl, &l, i, &bx, &bw) || !gui_inside(x, y, bx, l.y + l.buttons_y, bw, 28)) continue;
        if(chooser(dl) && i == 0) { if(named(dl)) dl->list_focus = false; choose(g); }   /* Save, Run: the line */
        else answer(g, i == 2 ? -1 : i, dl->kind == GUI_DLG_INPUT ? dl->field.text : "");
        return;
    }
    if(!chooser(dl)) return;
    x -= l.x;
    y -= l.y;
    if(gui_inside(x, y, l.up_x, 34, 56, 24)) { if(has_up(dl)) go_up(g); return; }
    if(named(dl) && gui_inside(x, y, 16, l.field_y, l.w - 32, 24)) { dl->list_focus = false; g->dirty = true; return; }
    rows = dl->count + has_up(dl);
    if(y < l.list_y || y >= l.list_y + l.rows * ROW_H || x < 16 || x >= l.w - 16) return;
    if(x >= l.w - 16 - SCROLL_W) {
        gd_thumb(l.rows * ROW_H, dl->top, l.rows, rows, &top, &size);
        dl->top += y - l.list_y < top ? -l.rows : y - l.list_y >= top + size ? l.rows : 0;
        dl->top = gui_clamp(dl->top, 0, rows > l.rows ? rows - l.rows : 0);
        g->dirty = true;
        return;
    }
    row = dl->top + (y - l.list_y) / ROW_H;
    if(row >= rows) return;
    dl->selected = row;
    dl->list_focus = true;
    picked(g);
    g->dirty = true;
    if(gui_double(&dl->click_ms, &dl->click_row, g->now, row)) {
        if(dl->kind == GUI_DLG_FOLDER) folder_into(g);  /* 11/05/26: a folder opens; Choose takes it */
        else choose(g);
    }
}

void dialog_wheel(struct gui_state *g, int rows) {
    struct gui_dialog *dl = &g->dialog;
    struct dlg_layout l;
    int total = dl->count + has_up(dl);
    if(!chooser(dl)) return;
    layout(g, &l);
    dl->top = gui_clamp(dl->top + rows, 0, total > l.rows ? total - l.rows : 0);
    g->dirty = true;
}

/* a path that ends where it must: the start goes, behind a … */
static void tail(const struct gui_draw *d, int x, int y, const char *path, int cells, u32 c) {
    u32 len;
    if(gd_cells(path) > cells) {
        while(*path && gd_cells(path) > cells - 1) { gd_char(path, strlen(path), &len); path += len; }
        gd_glyph(d, x, y, 0xF9, c, 1);
        x += 8;
        cells--;
    }
    gd_name(d, x, y, path, cells, c);
}

void dialog_render(const struct gui_state *g, const struct gui_draw *d) {
    const struct gui_dialog *dl = &g->dialog;
    struct dlg_layout l;
    struct gui_draw list;
    char size[24];
    int i, bx, bw, row, rows, py, x, cells, up;
    if(!dl->kind) return;
    layout(g, &l);
    gd_rect(d, l.x + 6, l.y + 7, l.w, l.h, SHADOW);
    gd_rect(d, l.x, l.y, l.w, l.h, INK);
    gd_rect(d, l.x + 1, l.y + 1, l.w - 2, l.h - 2, PAPER);
    gd_rect(d, l.x + 1, l.y + 1, l.w - 2, TITLE_H - 1, TEAL);
    gd_glyph(d, l.x + 12, l.y + 6, 0xF7, FOAM, 1);
    gd_name(d, l.x + 28, l.y + 6, dl->title, (l.w - 40) / 8, PAPER);
    cells = (l.w - 32) / 8;
    if(dl->kind == GUI_DLG_MESSAGE || dl->kind == GUI_DLG_CONFIRM) {
        gd_name(d, l.x + 16, l.y + 44, dl->text[0], cells, INK);
        gd_name(d, l.x + 16, l.y + 66, dl->text[1], cells, MUTED);
    } else if(dl->kind == GUI_DLG_INPUT) {
        gd_name(d, l.x + 16, l.y + 42, dl->text[0], cells, INK);
        field_render(d, &dl->field, l.x + 16, l.y + l.field_y, l.w - 32, true);
    } else {
        up = has_up(dl);
        rows = dl->count + up;
        gd_text(d, l.x + 16, l.y + 38, "Folder:", MUTED);
        tail(d, l.x + 80, l.y + 38, dl->dir, (l.up_x - 80 - 8) / 8, INK);
        if(dl->kind != GUI_DLG_RUN)                     /* 10/25/26: the launcher has no Up */
            gd_button(d, l.x + l.up_x, l.y + 34, 56, 24, "Up", has_up(dl) ? SHELL : BAND, has_up(dl) ? INK : MUTED);
        gd_rect(d, l.x + 15, l.y + l.list_y - 1, l.w - 30, l.rows * ROW_H + 2, LINE);
        gd_rect(d, l.x + 16, l.y + l.list_y, l.w - 32, l.rows * ROW_H, 0xFFFDF5);
        list = gd_clip(d, l.x + 16, l.y + l.list_y, l.w - 32 - SCROLL_W, l.rows * ROW_H);
        for(row = 0; row < l.rows && dl->top + row < rows; row++) {
            int k = dl->top + row - up;
            py = l.y + l.list_y + row * ROW_H;
            if(dl->top + row == dl->selected)
                gd_rect(&list, l.x + 16, py, l.w - 32, ROW_H, dl->kind == GUI_DLG_OPEN || dl->kind == GUI_DLG_FOLDER ||
                        dl->list_focus ? PICK : BAND);
            if(k < 0) {
                gd_icon(&list, l.x + 22, py + 1, ICON_FOLDER, 1);
                gd_text(&list, l.x + 44, py + 1, ".. (up one folder)", MUTED);
                continue;
            }
            gd_icon(&list, l.x + 22, py + 1, dl->list[k].type == GUI_DIR ? ICON_FOLDER : dl->kind == GUI_DLG_RUN ? ICON_PROGRAM : ICON_TEXT, 1);
            x = l.x + l.w - 32 - SCROLL_W;
            if(dl->list[k].type != GUI_DIR) {
                gui_bytes(dl->list[k].size, size, sizeof(size));
                gd_text(&list, x - (int)strlen(size) * 8, py + 1, size, MUTED);
            }
            gd_name(&list, l.x + 44, py + 1, dl->list[k].name, (x - 96 - l.x - 44) / 8, INK);
        }

        gd_scrollbar(d, l.x + l.w - 16 - SCROLL_W, l.y + l.list_y, l.rows * ROW_H, dl->top, l.rows, rows);
        if(named(dl)) {
            gd_text(d, l.x + 16, l.y + l.field_y - 18, dl->kind == GUI_DLG_RUN ? "The program, and its arguments:" : "Name:", MUTED);
            field_render(d, &dl->field, l.x + 16, l.y + l.field_y, l.w - 32, !dl->list_focus);
        }
        if(button_place(dl, &l, 0, &bx, &bw)) cells = (bx - l.x - 24) / 8;   /* left of the buttons */
        if(dl->text[0][0]) gd_name(d, l.x + 16, l.y + l.buttons_y + 6, dl->text[0], cells, CORAL);
        else if(dl->total > dl->count) {
            snprintf(size, sizeof(size), "(%d not shown)", dl->total - dl->count);
            gd_name(d, l.x + 16, l.y + l.buttons_y + 6, size, cells, MUTED);
        }
    }
    for(i = 0; i < 3; i++) {
        if(!button_place(dl, &l, i, &bx, &bw)) continue;
        gd_button(d, bx, l.y + l.buttons_y, bw, 28, dl->label[i], i == dl->choice ? TEAL : SHELL, i == dl->choice ? PAPER : INK);
    }
}
