/*
SurfOS Desktop Files
--------------------
File: guifiles.c Date: 10/17/26
--------------------
The file system as a tree from /: an open folder's rows follow it, folders first,
then files, by name. A USB volume that is not mounted is a row at the top with
Mount; a mounted one can be ejected. Mount and Eject are the shell's own commands
(shell/guifs.c), so the rules for the boot drive and for busy volumes are theirs,
and nothing is mounted unless the user asks. A refresh reads every open folder
again and keeps the selection. Drawing uses only what was read before.
10/17/26 (the suite): Open takes a picture to the Image Viewer and a file that
Notepad would refuse to Hex (open_file).
*/
#include "guipriv.h"
#include "gui_media.h"
#include <string.h>
#include <stdio.h>

static const struct gui_tool tools[] = {
    { "Open", ACT_FILES_OPEN }, { "New file", ACT_FILES_NEW_FILE }, { "New folder", ACT_FILES_NEW_FOLDER },
    { "Rename", ACT_FILES_RENAME }, { "Delete", ACT_FILES_DELETE }, { "Refresh", ACT_FILES_REFRESH },
    { "Mount", ACT_FILES_MOUNT }, { "Eject", ACT_FILES_EJECT },
};
#define TOOLS (int)(sizeof(tools) / sizeof(tools[0]))

struct files_layout { int head_y, list_y, rows, bar_x, status_y, size_x, date_x; bool dates; };

static void layout(const struct gui_state *g, struct files_layout *l) {
    const struct gui_window *w = &g->win[GUI_FILES];
    l->head_y = TITLE_H + BAR_H + 3;
    l->list_y = l->head_y + 20;
    l->status_y = w->h - STATUS_H - 1;
    l->rows = (l->status_y - l->list_y) / ROW_H;
    if(l->rows < 1) l->rows = 1;
    l->bar_x = w->w - 2 - SCROLL_W;
    l->dates = w->w >= 480;
    l->date_x = l->bar_x - 8 - 16 * 8;
    l->size_x = (l->dates ? l->date_x : l->bar_x) - 16;
}

int files_min_width(void) {
    return tool_span(tools, TOOLS);
}

int files_rows(const struct gui_state *g) {
    struct files_layout l;
    layout(g, &l);
    return l.rows;
}

static char lower(char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int compare_names(const char *a, const char *b) {
    while(*a && lower(*a) == lower(*b)) { a++; b++; }
    return (u8)lower(*a) - (u8)lower(*b);
}

static int compare(const struct gui_entry *a, const struct gui_entry *b) {
    int r;
    if(a->type != b->type) return a->type == GUI_DIR ? -1 : 1;
    r = compare_names(a->name, b->name);
    return r ? r : strcmp(a->name, b->name);
}

int gui_list(const struct gui_fs *fs, const char *path, struct gui_entry *out, int max, int *total) {
    struct gui_entry t;
    int n = fs->list(fs->ctx, path, out, max), i, j, m;
    if(n < 0) return n;
    if(total) *total = n;
    if(n > max) n = max;
    for(i = 0; i < n; i++) {                            /* few entries, big ones: few moves */
        for(m = i, j = i + 1; j < n; j++) if(compare(&out[j], &out[m]) < 0) m = j;
        if(m != i) { t = out[i]; out[i] = out[m]; out[m] = t; }
    }
    return n;
}

/**** rows ****/

/* the path of a row; false when it has none or it does not fit (then nothing may be done with it:
   a cut path names something else) */
bool files_path(const struct gui_state *g, int row, char *out, size_t n) {
    const struct gui_files *f = &g->files;
    int chain[128], depth = 0, i;
    out[0] = 0;
    if(row < 0 || row >= f->count || f->node[row].type == GUI_NODE_DRIVE) return false;
    if(f->node[row].type == GUI_NODE_MORE) row = f->node[row].parent;
    for(i = row; i > 0; i = f->node[i].parent) {
        if(depth == 128) return false;                  /* deeper than any path that fits */
        chain[depth++] = i;
    }
    strlcpy(out, "/", n);
    while(depth--) {
        if((strcmp(out, "/") && strlcat(out, "/", n) >= n) || strlcat(out, f->node[chain[depth]].name, n) >= n) {
            out[0] = 0;
            return false;
        }
    }
    return true;
}

static void too_long(struct gui_state *g) {
    strlcpy(g->files.status, "That path is too long for SurfOS (255 bytes at most)", sizeof(g->files.status));
    g->dirty = true;
}

/* the USB volume a row is on, or -1 */
static int volume_of(const struct gui_state *g, int row) {
    const struct gui_files *f = &g->files;
    char path[GUI_PATH_MAX];
    int k;
    if(row <= 0 || row >= f->count) return -1;
    if(f->node[row].type == GUI_NODE_DRIVE) return f->node[row].vol;
    while(row > 0 && f->node[row].depth > 1) row = f->node[row].parent;
    if(row <= 0) return -1;
    snprintf(path, sizeof(path), "/%s", f->node[row].name);
    for(k = 0; k < f->volumes; k++) if(!strcmp(f->vol[k].path, path)) return k;
    return -1;
}

/* nothing can be written there: the root, the initrd, /dev, a read-only mount */
static bool row_readonly(const struct gui_state *g, int row) {
    const struct gui_files *f = &g->files;
    int v = volume_of(g, row), top = row;
    if(row <= 0 || row >= f->count) return true;
    if(v >= 0) return f->vol[v].readonly;
    while(top > 0 && f->node[top].depth > 1) top = f->node[top].parent;
    return !strcmp(f->node[top].name, "initrd") || !strcmp(f->node[top].name, "dev");
}

/* the folder a new file or folder would go in: the selected folder, or the selected file's */
static int folder_row(const struct gui_state *g) {
    const struct gui_files *f = &g->files;
    int row = f->selected;
    if(row < 0 || row >= f->count) return -1;
    if(f->node[row].type != GUI_DIR) row = f->node[row].parent;
    return row;
}

static void show_selected(struct gui_state *g) {
    struct gui_files *f = &g->files;
    int rows = files_rows(g);
    f->selected = gui_clamp(f->selected, 0, f->count - 1);
    if(f->selected < f->top) f->top = f->selected;
    if(f->selected >= f->top + rows) f->top = f->selected - rows + 1;
    f->top = gui_clamp(f->top, 0, f->count > rows ? f->count - rows : 0);
    g->dirty = true;
}

static void set_node(struct gui_node *n, const char *name, int parent, int depth, int type) {
    memset(n, 0, sizeof(*n));
    strlcpy(n->name, name, sizeof(n->name));
    n->parent = parent;
    n->depth = depth;
    n->type = type;
    n->vol = -1;
}

/* read folder row i and put its rows after it (the unmounted volumes first, at /) */
static void expand(struct gui_state *g, int i) {
    struct gui_files *f = &g->files;
    char path[GUI_PATH_MAX];
    int n, total, room, shown, extra, drives = 0, insert, j, k, v;
    if(i < 0 || i >= f->count || f->node[i].type != GUI_DIR || f->node[i].expanded || !g->fs) return;
    if(!files_path(g, i, path, sizeof(path))) { too_long(g); return; }
    n = gui_list(g->fs, path, f->scratch, GUI_LIST_MAX, &total);
    if(n < 0) {
        snprintf(f->status, sizeof(f->status), "%s: %s", path, gui_error(g, n));
        g->dirty = true;
        return;
    }
    if(i == 0) for(v = 0; v < f->volumes; v++) if(!f->vol[v].path[0]) drives++;
    room = GUI_NODES - f->count;
    if(drives + total > 0 && room <= 0) {               /* nothing more fits: the folder stays closed */
        strlcpy(f->status, "The list is full: close some folders first", sizeof(f->status));
        g->dirty = true;
        return;
    }
    if(drives > room) drives = room;
    room -= drives;
    shown = n;
    extra = total > n;
    if(shown + extra > room) { shown = room > 0 ? room - 1 : 0; extra = room > 0; }
    insert = drives + shown + extra;
    f->node[i].expanded = true;
    if(!insert) return;
    memmove(&f->node[i + 1 + insert], &f->node[i + 1], (f->count - i - 1) * sizeof(f->node[0]));
    f->count += insert;
    for(j = i + 1 + insert; j < f->count; j++) if(f->node[j].parent > i) f->node[j].parent += insert;
    if(f->selected > i) f->selected += insert;
    k = i + 1;
    for(v = 0; v < f->volumes && drives; v++) {
        if(f->vol[v].path[0]) continue;
        set_node(&f->node[k], f->vol[v].dev, i, f->node[i].depth + 1, GUI_NODE_DRIVE);
        f->node[k++].vol = v;
        drives--;
    }
    for(j = 0; j < shown; j++, k++) {
        set_node(&f->node[k], f->scratch[j].name, i, f->node[i].depth + 1, f->scratch[j].type == GUI_DIR ? GUI_DIR : GUI_FILE);
        f->node[k].size = f->scratch[j].size;
        f->node[k].mtime = f->scratch[j].mtime;
    }
    if(extra) {
        set_node(&f->node[k], "", i, f->node[i].depth + 1, GUI_NODE_MORE);
        f->node[k].size = total - shown;
    }
}

static void collapse(struct gui_state *g, int i) {
    struct gui_files *f = &g->files;
    int j = i + 1, gone, k;
    while(j < f->count && f->node[j].depth > f->node[i].depth) j++;
    gone = j - i - 1;
    memmove(&f->node[i + 1], &f->node[j], (f->count - j) * sizeof(f->node[0]));
    f->count -= gone;
    for(k = i + 1; k < f->count; k++) if(f->node[k].parent >= j) f->node[k].parent -= gone;
    if(f->selected > i && f->selected < j) f->selected = i;
    else if(f->selected >= j) f->selected -= gone;
    f->node[i].expanded = false;
}

static bool kept(const struct gui_files *f, u32 used, const char *path) {
    u32 at = strlen(f->keep) + 1;                       /* after the selected row's */
    while(at < used) {
        if(!strcmp(f->keep + at, path)) return true;
        at += strlen(f->keep + at) + 1;
    }
    return false;
}

static void keep(struct gui_files *f, u32 *used, const char *s) {
    u32 len = strlen(s) + 1;
    if(*used + len <= sizeof(f->keep)) { memcpy(f->keep + *used, s, len); *used += len; }
}

void files_refresh(struct gui_state *g) {
    struct gui_files *f = &g->files;
    char path[GUI_PATH_MAX], want[GUI_PATH_MAX];
    u32 used = 0;
    int i, top = f->top;
    if(!g->fs) return;
    if(f->count && f->selected >= 0 && f->selected < f->count && f->node[f->selected].type == GUI_NODE_DRIVE)
        snprintf(path, sizeof(path), "#%s", f->node[f->selected].name);   /* a volume: by its device */
    else files_path(g, f->selected, path, sizeof(path));
    keep(f, &used, path);
    for(i = 1; i < f->count; i++) {
        if(f->node[i].type != GUI_DIR || !f->node[i].expanded) continue;
        if(files_path(g, i, path, sizeof(path))) keep(f, &used, path);
    }
    f->volumes = g->fs->volumes(g->fs->ctx, f->vol, GUI_VOLUMES);
    if(f->volumes < 0) f->volumes = 0;
    if(f->volumes > GUI_VOLUMES) f->volumes = GUI_VOLUMES;
    f->count = 1;
    set_node(&f->node[0], "/", -1, 0, GUI_DIR);
    f->selected = 0;
    expand(g, 0);
    for(i = 1; i < f->count; i++) {
        if(f->node[i].type != GUI_DIR) continue;
        if(files_path(g, i, path, sizeof(path)) && kept(f, used, path)) expand(g, i);
    }
    /* the selection again: the same row, or what is left of its path (a deleted file: its folder) */
    if(f->keep[0] == '#') {
        for(i = 1; i < f->count; i++) if(f->node[i].type == GUI_NODE_DRIVE && !strcmp(f->node[i].name, f->keep + 1)) f->selected = i;
    } else if(f->keep[0] == '/') {
        strlcpy(want, f->keep, sizeof(want));
        for(;;) {
            for(i = 0; i < f->count; i++)
                if(f->node[i].type != GUI_NODE_DRIVE && f->node[i].type != GUI_NODE_MORE && files_path(g, i, path, sizeof(path)) && !strcmp(path, want)) break;
            if(i < f->count) { f->selected = i; break; }
            if(!strcmp(want, "/")) break;
            gui_parent(want);
        }
    }
    f->top = top;
    show_selected(g);
}

void files_open(struct gui_state *g) {
    g->files.status[0] = 0;
    files_refresh(g);
}

/* open the folders down to path and select it */
void files_reveal(struct gui_state *g, const char *path) {
    struct gui_files *f = &g->files;
    char part[GUI_NAME_MAX];
    int row = 0, k, len;
    if(!f->count) files_refresh(g);
    while(*path) {
        while(*path == '/') path++;
        for(len = 0; path[len] && path[len] != '/'; len++) ;
        if(!len) break;
        if(len >= (int)sizeof(part)) break;
        memcpy(part, path, len);
        part[len] = 0;
        path += len;
        expand(g, row);
        for(k = row + 1; k < f->count && f->node[k].depth > f->node[row].depth; k++)
            if(f->node[k].parent == row && f->node[k].type != GUI_NODE_DRIVE && !strcmp(f->node[k].name, part)) break;
        if(k >= f->count || f->node[k].parent != row) break;
        row = k;
    }
    f->selected = row;
    show_selected(g);
}

/**** actions ****/

bool files_can(const struct gui_state *g, int act) {
    const struct gui_files *f = &g->files;
    const struct gui_node *s;
    int v, folder;
    if(!g->fs) return false;
    if(act == ACT_FILES_REFRESH) return true;
    if(f->selected < 0 || f->selected >= f->count) return false;
    s = &f->node[f->selected];
    switch(act) {
    case ACT_FILES_OPEN: return f->selected > 0 && s->type != GUI_NODE_MORE;
    case ACT_FILES_COPY: return s->type != GUI_NODE_DRIVE && s->type != GUI_NODE_MORE;
    case ACT_FILES_NEW_FILE: case ACT_FILES_NEW_FOLDER:
        folder = folder_row(g);
        return s->type != GUI_NODE_DRIVE && folder > 0 && !row_readonly(g, folder);
    case ACT_FILES_RENAME: case ACT_FILES_DELETE:
        return (s->type == GUI_FILE || s->type == GUI_DIR) && s->depth >= 2 && !row_readonly(g, f->selected);
    case ACT_FILES_MOUNT: return s->type == GUI_NODE_DRIVE;
    case ACT_FILES_EJECT: v = volume_of(g, f->selected); return v >= 0 && f->vol[v].path[0];
    }
    return false;
}

/* the row of a top-level name (a mount point or an unmounted volume), or -1 */
static int top_row(const struct gui_state *g, const char *name) {
    int k;
    for(k = 1; k < g->files.count; k++) if(g->files.node[k].depth == 1 && !strcmp(g->files.node[k].name, name)) return k;
    return -1;
}

/* 10/17/26 (the suite): Open. A picture (*.bmp) goes to the Image Viewer, and a file that Notepad would refuse to
   Hex: one of more than Notepad's 256 KB, or with a NUL byte in its first 512 (one further on is Notepad's to find).
   Any other file goes to Notepad, as does what cannot be looked at here (a device, a file that is not there), so
   that Notepad says why. 11/05/26 (the desktop applications, track desk): first, a file whose type a program of the
   registry names (rootfs/apps/README) opens in that program, started with the file's path as its argument; these
   rules are for what none names. */
static void open_file(struct gui_state *g, const char *path) {
    struct gui_entry e;
    bool device = false, readonly = false;
    char head[512];
    u32 len = 0, size = 0;
    int k = reg_for(g, gui_basename(path));
    if(k >= 0) { reg_open(g, k, path); return; }
    if(viewer_takes(gui_basename(path))) { viewer_show(g, path); return; }
    if(g->fs && g->fs->read_at && !g->fs->stat(g->fs->ctx, path, &e, &device, &readonly) && e.type != GUI_DIR &&
       !device && (e.size > GUI_NOTE_MAX ||
                   (!g->fs->read_at(g->fs->ctx, path, 0, head, sizeof(head), &len, &size) && memchr(head, 0, len)))) {
        hex_show(g, path);
        return;
    }
    if(gui_guard(g, PEND_OPEN_PATH, path) && note_load(g, path) == 0) gui_open(g, GUI_NOTEPAD);
}

/* a file or folder the desktop leaves alone (note_guarded()): said */
static void refuse(struct gui_state *g, const char *title, const char *path) {
    char line[80];
    gui_unsure_line(g, path, line, sizeof(line));
    dialog_message(g, title, line, "Have the disk checked first (fsck).");
}

void files_act(struct gui_state *g, int act) {
    struct gui_files *f = &g->files;
    struct gui_node *s;
    char path[GUI_PATH_MAX], name[GUI_NAME_MAX], msg[GUI_STATUS], line[80];
    int v, r, row, k;
    u32 len;
    if(!files_can(g, act)) return;
    s = &f->node[f->selected];
    f->status[0] = 0;
    switch(act) {
    case ACT_FILES_OPEN:
        if(s->type == GUI_NODE_DRIVE) { files_act(g, ACT_FILES_MOUNT); return; }
        if(s->type == GUI_DIR) {
            if(s->expanded) collapse(g, f->selected);
            else expand(g, f->selected);
            show_selected(g);
            break;
        }
        if(!files_path(g, f->selected, path, sizeof(path))) { too_long(g); break; }
        open_file(g, path);
        return;
    case ACT_FILES_COPY:                                /* the path, for Notepad or a dialog's box */
        if(!files_path(g, f->selected, path, sizeof(path))) { too_long(g); break; }
        len = strlen(path);
        memcpy(g->clip, path, len);
        gui_clipped(g, len);
        snprintf(f->status, sizeof(f->status), "Copied %s", path);
        break;
    case ACT_FILES_NEW_FILE: case ACT_FILES_NEW_FOLDER:
        if(!files_path(g, folder_row(g), path, sizeof(path))) { too_long(g); break; }
        snprintf(line, sizeof(line), "Name of the new %s in %s:", act == ACT_FILES_NEW_FILE ? "file" : "folder", gui_basename(path));
        dialog_input(g, act == ACT_FILES_NEW_FILE ? ACT_DLG_NEW_FILE : ACT_DLG_NEW_FOLDER,
                     act == ACT_FILES_NEW_FILE ? "New file" : "New folder", line,
                     act == ACT_FILES_NEW_FILE ? "untitled.txt" : "New folder");
        strlcpy(g->dialog.target, path, sizeof(g->dialog.target));
        return;
    case ACT_FILES_RENAME:
        if(!files_path(g, f->selected, path, sizeof(path))) { too_long(g); break; }
        if(note_guarded(g, path)) { refuse(g, "Not renamed", path); return; }   /* its new name would be unguarded */
        snprintf(line, sizeof(line), "New name for %s:", s->name);
        dialog_input(g, ACT_DLG_RENAME, "Rename", line, s->name);
        strlcpy(g->dialog.target, path, sizeof(g->dialog.target));
        return;
    case ACT_FILES_DELETE:
        if(!files_path(g, f->selected, path, sizeof(path))) { too_long(g); break; }
        if(note_guarded(g, path)) { refuse(g, "Not deleted", path); return; }
        snprintf(line, sizeof(line), "Delete %s?", s->name);
        dialog_confirm(g, ACT_DLG_DELETE, s->type == GUI_DIR ? "Delete folder" : "Delete file", line,
                       s->type == GUI_DIR ? "Only an empty folder can be deleted." : "It cannot be brought back.",
                       "Delete", NULL, "Cancel");
        g->dialog.choice = 2;                           /* Enter alone does not delete */
        strlcpy(g->dialog.target, path, sizeof(g->dialog.target));
        return;
    case ACT_FILES_REFRESH:
        files_refresh(g);
        break;
    case ACT_FILES_MOUNT:
        strlcpy(name, f->vol[s->vol].dev, sizeof(name));
        r = g->fs->mount(g->fs->ctx, name, msg, sizeof(msg));
        files_refresh(g);
        if(r) { dialog_message(g, "Not mounted", msg, ""); break; }
        strlcpy(f->status, msg, sizeof(f->status));
        row = top_row(g, name);
        if(row > 0) { f->selected = row; expand(g, row); }
        show_selected(g);
        break;
    case ACT_FILES_EJECT:
        v = volume_of(g, f->selected);
        for(k = 0; k < f->volumes; k++) {               /* eject takes every volume of the disk along */
            len = strlen(f->vol[k].path);
            if(!len || strcmp(f->vol[k].disk, f->vol[v].disk) || !g->note.modified) continue;
            if(!strncmp(g->note.path, f->vol[k].path, len) && g->note.path[len] == '/') break;
        }
        if(k < f->volumes) {
            dialog_message(g, "Not ejected", "Notepad has unsaved changes to a file there.", "Save or close it first.");
            break;
        }
        strlcpy(name, f->vol[v].dev, sizeof(name));
        r = g->fs->eject(g->fs->ctx, name, msg, sizeof(msg));
        files_refresh(g);
        if(r) { dialog_message(g, "Not ejected", msg, ""); break; }
        strlcpy(f->status, msg, sizeof(f->status));
        break;
    }
    g->dirty = true;
}

static bool usable(const char *name) {
    return name[0] && strcmp(name, ".") && strcmp(name, "..") && !strchr(name, '/');
}

void files_answer(struct gui_state *g, int act, int button, const char *text, const char *target) {
    struct gui_entry e;
    char dest[GUI_PATH_MAX], line[80];
    const char *what;
    bool device, readonly;
    u32 len;
    int r;
    if(button != 0 || !g->fs) return;
    if(act == ACT_DLG_DELETE) {
        if(note_guarded(g, target)) { refuse(g, "Not deleted", target); return; }   /* it would free what another holds */
        r = g->fs->stat(g->fs->ctx, target, &e, &device, &readonly);
        if(!r) r = g->fs->remove(g->fs->ctx, target, e.type == GUI_DIR);
        if(r) {
            snprintf(line, sizeof(line), "%s: %s", gui_basename(target), gui_error(g, r));
            dialog_message(g, "Not deleted", line, r == -ENOTEMPTY ? "Only an empty folder can be deleted." : "");
            return;
        }
        snprintf(line, sizeof(line), "Deleted %s", gui_basename(target));
        files_refresh(g);
        strlcpy(g->files.status, line, sizeof(g->files.status));
        return;
    }
    what = act == ACT_DLG_NEW_FILE ? "Not created" : act == ACT_DLG_NEW_FOLDER ? "Not created" : "Not renamed";
    if(!usable(text)) { dialog_message(g, what, "A name cannot be empty, . or .., or have a /.", ""); return; }
    if(act == ACT_DLG_RENAME) {
        if(note_guarded(g, target)) { refuse(g, what, target); return; }   /* its new name would be unguarded */
        strlcpy(dest, target, sizeof(dest));
        gui_parent(dest);
        r = gui_join(dest, sizeof(dest), dest, text);
        if(r && !strcmp(dest, target)) return;
    } else r = gui_join(dest, sizeof(dest), target, text);
    if(!r) { dialog_message(g, what, "That name makes the path too long for SurfOS.", ""); return; }
    /* FAT's names ignore case: a.txt may become A.txt */
    if(g->fs->stat(g->fs->ctx, dest, &e, &device, &readonly) == 0 && !(act == ACT_DLG_RENAME && !compare_names(dest, target))) {
        snprintf(line, sizeof(line), "%s is there already.", text);
        dialog_message(g, what, line, "");
        return;
    }
    r = act == ACT_DLG_NEW_FILE ? g->fs->write(g->fs->ctx, dest, "", 0, false, NULL, 0) :
        act == ACT_DLG_NEW_FOLDER ? g->fs->mkdir(g->fs->ctx, dest) : g->fs->rename(g->fs->ctx, target, dest);
    if(r) {
        snprintf(line, sizeof(line), "%s: %s", text, gui_error(g, r));
        dialog_message(g, what, line, "");
        return;
    }
    len = strlen(target);
    if(act == ACT_DLG_RENAME && !strncmp(g->note.path, target, len) && (!g->note.path[len] || g->note.path[len] == '/')) {
        char moved[GUI_PATH_MAX];                       /* Notepad's file, or a folder it is in */
        if(snprintf(moved, sizeof(moved), "%s%s", dest, g->note.path + len) < (int)sizeof(moved))
            strlcpy(g->note.path, moved, sizeof(g->note.path));
        else g->note.path[0] = 0;                       /* too long now: Save asks for a name */
    }
    files_refresh(g);
    files_reveal(g, dest);
    snprintf(line, sizeof(line), "%s %s", act == ACT_DLG_RENAME ? "Renamed to" : "Created", text);
    strlcpy(g->files.status, line, sizeof(g->files.status));
}

/**** keys and the pointer ****/

void files_key(struct gui_state *g, int key) {
    struct gui_files *f = &g->files;
    int rows = files_rows(g), s = f->selected;
    if(!f->count) return;
    switch(key) {                                       /* moving: the status bar shows the selection again */
    case GUI_UP: case GUI_DOWN: case GUI_PGUP: case GUI_PGDN: case GUI_HOME: case GUI_END: case GUI_RIGHT: case GUI_LEFT: case 8: case 127:
        f->status[0] = 0;                               /* (what Right or Left does next may say something) */
    }
    switch(key) {
    case GUI_UP: f->selected--; break;
    case GUI_DOWN: f->selected++; break;
    case GUI_PGUP: f->selected -= rows - 1; break;
    case GUI_PGDN: f->selected += rows - 1; break;
    case GUI_HOME: f->selected = 0; break;
    case GUI_END: f->selected = f->count - 1; break;
    case GUI_RIGHT:
        if(f->node[s].type != GUI_DIR) break;
        if(!f->node[s].expanded) expand(g, s);
        else if(s + 1 < f->count && f->node[s + 1].parent == s) f->selected = s + 1;
        break;
    case GUI_LEFT: case 8: case 127:
        if(f->node[s].type == GUI_DIR && f->node[s].expanded && s > 0 && key == GUI_LEFT) collapse(g, s);
        else if(f->node[s].parent >= 0) f->selected = f->node[s].parent;
        break;
    case '\r': case '\n': files_act(g, ACT_FILES_OPEN); return;
    case GUI_DELETE: files_act(g, ACT_FILES_DELETE); return;
    case 18: files_act(g, ACT_FILES_REFRESH); return;              /* Ctrl+R */
    default: return;
    }
    show_selected(g);
}

void files_wheel(struct gui_state *g, int rows) {
    struct gui_files *f = &g->files;
    int shown = files_rows(g);
    f->top = gui_clamp(f->top + rows, 0, f->count > shown ? f->count - shown : 0);
    g->dirty = true;
}

void files_scroll(struct gui_state *g, int y) {
    struct files_layout l;
    layout(g, &l);
    g->files.top = gd_scroll_to(l.rows * ROW_H, y - l.list_y - g->scroll_at, l.rows, g->files.count);
    g->dirty = true;
}

/* 10/26/26 (stage C8): a right-click's row (x, y in the window): selected, nothing opened or expanded */
void files_select_at(struct gui_state *g, int x, int y) {
    struct gui_files *f = &g->files;
    struct files_layout l;
    int row;
    layout(g, &l);
    if(y < l.list_y || y >= l.list_y + l.rows * ROW_H || x >= l.bar_x) return;
    row = f->top + (y - l.list_y) / ROW_H;
    if(row >= f->count) return;
    f->selected = row;
    f->status[0] = 0;
    g->dirty = true;
}

void files_press(struct gui_state *g, int x, int y) {
    struct gui_files *f = &g->files;
    struct files_layout l;
    int i, row, ix, top, size;
    layout(g, &l);
    i = tool_hit(tools, TOOLS, x, y);
    if(i >= 0) { if(files_can(g, tools[i].act)) gui_act(g, tools[i].act); return; }
    if(y < l.list_y || y >= l.list_y + l.rows * ROW_H) return;
    if(x >= l.bar_x) {
        gd_thumb(l.rows * ROW_H, f->top, l.rows, f->count, &top, &size);
        if(y - l.list_y < top) files_wheel(g, -l.rows);
        else if(y - l.list_y >= top + size) files_wheel(g, l.rows);
        else { g->scroll = GUI_FILES; g->scroll_at = y - l.list_y - top; }
        return;
    }
    row = f->top + (y - l.list_y) / ROW_H;
    if(row >= f->count) return;
    ix = 8 + f->node[row].depth * 16;
    if(f->node[row].type == GUI_DIR && row > 0 && x >= ix - 2 && x < ix + 10) {   /* the triangle */
        if(f->node[row].expanded) collapse(g, row);
        else expand(g, row);
        f->top = gui_clamp(f->top, 0, f->count > l.rows ? f->count - l.rows : 0);   /* the view stays where it is */
        g->dirty = true;
        return;
    }
    f->selected = row;
    f->status[0] = 0;
    if(gui_double(&f->click_ms, &f->click_row, g->now, row)) files_act(g, ACT_FILES_OPEN);
    g->dirty = true;
}

/**** drawing ****/

static bool texty(const char *name) {
    static const char *const ends[] = { ".txt", ".md", ".c", ".h", ".s", ".S", ".asm", ".cfg", ".conf", ".ini", ".log",
                                        ".sh", ".py", ".json", ".html", ".htm", ".css", ".csv", ".xml", ".mk", NULL };
    u32 n = strlen(name), k, i;
    for(i = 0; ends[i]; i++) {
        k = strlen(ends[i]);
        if(n > k && !compare_names(name + n - k, ends[i])) return true;
    }
    return !compare_names(name, "Makefile") || !compare_names(name, "README");
}

void files_render(const struct gui_state *g, const struct gui_draw *d) {
    const struct gui_window *w = &g->win[GUI_FILES];
    const struct gui_files *f = &g->files;
    struct files_layout l;
    struct gui_draw list;
    bool on[TOOLS], focused = g->focus == GUI_FILES && !g->dialog.kind && !g->menu;
    char b[GUI_PATH_MAX + 16], size[24];
    int x0 = w->x, y0 = w->y, row, i, ix, py, icon, v, x, cells;
    layout(g, &l);
    for(i = 0; i < TOOLS; i++) on[i] = files_can(g, tools[i].act);
    gd_rect(d, x0 + 1, y0 + TITLE_H, w->w - 2, BAR_H + 2, BAND);
    tool_render(d, x0, y0, tools, TOOLS, on);
    gd_rect(d, x0 + 1, y0 + l.head_y, w->w - 2, 20, SHELL);
    gd_text(d, x0 + 10, y0 + l.head_y + 2, "Name", MUTED);
    gd_text(d, x0 + l.size_x - 32, y0 + l.head_y + 2, "Size", MUTED);
    if(l.dates) gd_text(d, x0 + l.date_x, y0 + l.head_y + 2, "Modified", MUTED);
    gd_rect(d, x0 + 1, y0 + l.list_y, l.bar_x - 1, l.status_y - l.list_y, 0xFFFDF5);
    list = gd_clip(d, x0 + 1, y0 + l.list_y, l.bar_x - 1, l.rows * ROW_H);
    for(row = 0; row < l.rows && f->top + row < f->count; row++) {
        const struct gui_node *n = &f->node[f->top + row];
        i = f->top + row;
        py = y0 + l.list_y + row * ROW_H;
        if(i == f->selected) gd_rect(&list, x0 + 1, py, l.bar_x - 1, ROW_H, focused ? PICK : BAND);
        ix = x0 + 8 + n->depth * 16;
        if(n->type == GUI_DIR && i > 0) gd_glyph(&list, ix, py + 1, n->expanded ? 0x1F : 0x10, MUTED, 1);
        v = n->type == GUI_DIR && n->depth == 1 ? volume_of(g, i) : -1;
        icon = i == 0 ? ICON_WAVE : n->type == GUI_NODE_DRIVE || v >= 0 ? ICON_DRIVE :
               n->type == GUI_DIR ? (n->expanded ? ICON_OPEN_FOLDER : ICON_FOLDER) :
               n->type == GUI_NODE_MORE ? -1 : texty(n->name) ? ICON_TEXT : ICON_FILE;
        if(icon >= 0) gd_icon(&list, ix + 10, py + 1, icon, 1);
        cells = (x0 + l.size_x - 80 - (ix + 30)) / 8;
        if(n->type == GUI_NODE_MORE) {
            snprintf(b, sizeof(b), "\xF9 %u more not shown", n->size);
            gd_text(&list, ix + 30, py + 1, b, MUTED);
            continue;
        }
        x = gd_name(&list, ix + 30, py + 1, i == 0 ? "SurfOS" : n->name, cells, INK);
        if(n->type == GUI_NODE_DRIVE) {
            snprintf(b, sizeof(b), f->vol[n->vol].boot ? " %s, boot drive, not mounted" : " %s, not mounted", f->vol[n->vol].fs);
            gd_text(&list, x, py + 1, b, MUTED);
            gd_text(&list, x0 + l.size_x - (int)strlen(f->vol[n->vol].size) * 8, py + 1, f->vol[n->vol].size, MUTED);
            continue;
        }
        if(v >= 0) {
            gd_text(&list, x, py + 1, f->vol[v].gone ? " USB, unplugged: eject it" :
                                      f->vol[v].boot ? (f->vol[v].readonly ? " USB boot drive, read-only" : " USB boot drive") :
                                      f->vol[v].readonly ? " USB, read-only" : " USB", MUTED);
            gd_text(&list, x0 + l.size_x - (int)strlen(f->vol[v].size) * 8, py + 1, f->vol[v].size, MUTED);
        }
        if(n->type == GUI_FILE) {
            gui_bytes(n->size, size, sizeof(size));
            gd_text(&list, x0 + l.size_x - (int)strlen(size) * 8, py + 1, size, MUTED);
        }
        if(l.dates && n->mtime) {
            gui_date(n->mtime, size, sizeof(size));
            gd_text(&list, x0 + l.date_x, py + 1, size, MUTED);
        }
    }
    gd_scrollbar(d, x0 + l.bar_x, y0 + l.list_y, l.rows * ROW_H, f->top, l.rows, f->count);
    gd_rect(d, x0 + 1, y0 + l.status_y, w->w - 2, STATUS_H, BAND);
    gd_rect(d, x0 + 1, y0 + l.status_y, w->w - 2, 1, LINE);
    if(f->status[0]) strlcpy(b, f->status, sizeof(b));
    else if(f->selected >= 0 && f->selected < f->count && f->node[f->selected].type == GUI_NODE_DRIVE)
        snprintf(b, sizeof(b), "%s: double-click or Mount to open it", f->node[f->selected].name);
    else {
        if(!files_path(g, f->selected, b, GUI_PATH_MAX)) strlcpy(b, f->selected > 0 ? "(a path too long for SurfOS)" : "", sizeof(b));
        if(f->selected > 0 && f->node[f->selected].type == GUI_FILE) {
            snprintf(size, sizeof(size), "  %u bytes", f->node[f->selected].size);
            strlcat(b, size, sizeof(b));
        }
    }
    gd_name(d, x0 + 10, y0 + l.status_y + 3, b, (w->w - 36) / 8, INK);   /* clear of the resize grip */
}
