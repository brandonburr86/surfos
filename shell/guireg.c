/*
SurfOS Desktop: the Application Registry
--------------------
File: guireg.c Date: 11/05/26
--------------------
The programs of /initrd/apps/<name>.app (rootfs/apps/README is the format; docs/superpowers/plans/
2026-11-05-desktop-apps.md, track desk): read when the desktop starts, through struct gui_fs, into
the desktop's own state (struct gui_registry, sys/gui_reg.h), where Apps > Programs, the tiles,
Files' Open and Open with, and the icons of the windows the desktop started find them.

The kernel reads these, so the parser bounds everything before it keeps anything: a file of at
most GUI_REG_FILE bytes read whole, lines of at most GUI_REG_LINE, each value checked against what
its key may be, nothing allocated and nothing recursive. A file it cannot take is left out whole,
and why goes to the kernel log through struct gui_sys's note. An entry whose program is one of the
desktop's applications (struct gui_app's program: the Clock's wclock) shows only there.
11/05/26 (the review of the ABI): types and open-with are read by surfos/win.h's win_types(), as a
program's chooser's are (extensions, and whole names after '='); no line of the log carries a
value's bytes, and a file whose name it could not say is left out; one entry a program, the first.

A program started from Apps or its tile is remembered (struct gui_registry's pid), so that the
next choice brings its window back rather than starting another, as an application's launcher
does (guicore.c's app_program()); one started with a file is a new one each time, remembered only
for its window's icon.
*/
#include "guipriv.h"
#include <string.h>
#include <stdio.h>

#define APPS_DIR "/initrd/apps"
#define BIN_DIR "/initrd/bin"

/* the icon palette (rootfs/apps/README): '.' is nothing, the rest these */
static const char palette_keys[] = "KNTFPAMLCSYBRGWD";
static const u32 palette[] = { INK, NAVY, TEAL, FOAM, 0xFFFDF5, PAPER, MUTED, LINE, CORAL, SAND, 0xE8D08A, SKY,
                               0xC8473B, 0x4F9A58, 0xFFFFFF, 0x000000 };

static bool blank(char c) { return c == ' ' || c == '\t'; }
static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static bool alnum(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }

/* 11/05/26 (the review): a file name the log may say: letters, digits, '.', '-' and '_' */
static bool plain_name(const char *s) {
    for(; *s; s++) if(!alnum(*s) && *s != '.' && *s != '-' && *s != '_') return false;
    return true;
}

u32 gui_reg_color(char c) {
    const char *p = c ? strchr(palette_keys, c) : NULL;
    return p ? palette[p - palette_keys] : 0xFF000000;
}

/**** a .app file ****/

/* a value of types (types, open-with) into out, as a program's chooser takes them (surfos/win.h's win_types(), which
   the kernel checks those with): NULL, or why not */
static const char *type_list(const char *v, char out[][GUI_REG_TYPE], int *n) {
    u32 len = (u32)strlen(v), i;
    for(i = 0; i < len; i++) if(v[i] == '\t') return "types are separated by spaces";
    *n = win_types(v, len + 1, out);
    if(*n < 0) {
        *n = 0;
        return "not a list of types";
    }
    return *n ? NULL : "no type is given";
}

static const char *yes_no(const char *v, bool *out) {
    if(!strcmp(v, "yes")) *out = true;
    else if(!strcmp(v, "no")) *out = false;
    else return "yes or no";
    return NULL;
}

/* one "key = value" into r (*seen: the keys given so far, a bit each; *icons: icon lines so far): NULL, or why not.
   *name: the key's name as the desktop knows it, for the log (never the file's bytes: 11/05/26, the review) */
static const char *setting(struct gui_reg *r, const char *key, const char *v, u32 *seen, int *icons, const char **name) {
    static const char *const keys[] = { "program", "title", "types", "open-with", "apps", "desktop", "icon" };
    const int nkeys = (int)(sizeof(keys) / sizeof(keys[0]));
    int k, i, n = (int)strlen(v);
    for(k = 0; k < nkeys && strcmp(key, keys[k]); k++) ;
    *name = k < nkeys ? keys[k] : "a key";
    if(k == nkeys) return "not one the desktop knows";
    if(k != 6 && (*seen & (1u << k))) return "given twice";
    *seen |= 1u << k;
    switch(k) {
    case 0:
        if(n < 1 || n >= GUI_REG_NAME || v[0] == '.') return "a program is 1 to 31 letters, digits, . - _";
        for(i = 0; i < n; i++)
            if(!alnum(v[i]) && v[i] != '.' && v[i] != '-' && v[i] != '_') return "a program is 1 to 31 letters, digits, . - _";
        strlcpy(r->program, v, sizeof(r->program));
        return NULL;
    case 1:
        if(n < 1 || n >= GUI_REG_NAME) return "a title is 1 to 31 bytes";
        for(i = 0; i < n; i++) if((u8)v[i] < 0x20 || v[i] == 0x7F) return "a title has no control characters";
        strlcpy(r->title, v, sizeof(r->title));
        return NULL;
    case 2: return type_list(v, r->types, &r->ntypes);
    case 3: return type_list(v, r->open_with, &r->nopen);
    case 4: return yes_no(v, &r->apps);
    case 5: return yes_no(v, &r->desktop);
    }
    if(*icons >= GUI_REG_ICON) return "more than 16 icon lines";
    if(n != GUI_REG_ICON) return "an icon line is 16 characters";
    for(i = 0; i < GUI_REG_ICON; i++) if(v[i] != '.' && gui_reg_color(v[i]) == 0xFF000000) return "a letter that is not in the palette";
    memcpy(r->icon[(*icons)++], v, GUI_REG_ICON);
    return NULL;
}

int gui_reg_parse(const char *text, u32 len, struct gui_reg *r, char *why, size_t n) {
    char line[GUI_REG_LINE + 1], *key, *value, *eq, *end;
    const char *w, *kname;
    u32 pos = 0, at, take, seen = 0, number = 0;
    int icons = 0;
    memset(r, 0, sizeof(*r));
    r->apps = true;
    r->app = -1;
    if(len > GUI_REG_FILE) {
        snprintf(why, n, "longer than %d bytes", GUI_REG_FILE);
        return -1;
    }
    while(pos < len) {
        number++;
        for(at = pos; at < len && text[at] != '\n'; at++) ;
        take = at - pos;
        if(take && text[at - 1] == '\r') take--;        /* CR LF */
        if(take > GUI_REG_LINE) {
            snprintf(why, n, "line %u: longer than %d bytes", number, GUI_REG_LINE);
            return -1;
        }
        if(memchr(text + pos, 0, take)) {
            snprintf(why, n, "line %u: a NUL byte", number);
            return -1;
        }
        memcpy(line, text + pos, take);
        line[take] = 0;
        pos = at + 1;
        for(key = line; blank(*key); key++) ;
        if(!*key || *key == '#') continue;
        eq = strchr(key, '=');
        if(eq) {
            for(end = eq; end > key && blank(end[-1]); end--) ;
            *end = 0;
        }
        if(!eq || !*key) {
            snprintf(why, n, "line %u: not key = value", number);
            return -1;
        }
        for(value = eq + 1; blank(*value); value++) ;
        for(end = value + strlen(value); end > value && blank(end[-1]); end--) ;
        *end = 0;
        if((w = setting(r, key, value, &seen, &icons, &kname))) {
            snprintf(why, n, "line %u: %s: %s", number, kname, w);
            return -1;
        }
    }
    if(!r->program[0]) { strlcpy(why, "no program", n); return -1; }
    if(!r->title[0]) { strlcpy(why, "no title", n); return -1; }
    if(icons && icons != GUI_REG_ICON) { snprintf(why, n, "%d icon lines, not 16", icons); return -1; }
    r->icon_set = icons == GUI_REG_ICON;
    return 0;
}

/**** the folder ****/

/* entry r's types name type as it is kept (lower case; '=' and a whole name), or (also) its open-with */
static bool type_named(const struct gui_reg *r, const char *type, bool also) {
    int i;
    for(i = 0; i < r->ntypes; i++) if(!strcmp(r->types[i], type)) return true;
    for(i = 0; also && i < r->nopen; i++) if(!strcmp(r->open_with[i], type)) return true;
    return false;
}


static void note(struct gui_state *g, const char *line) {
    if(g->sys && g->sys->note) g->sys->note(g->sys->ctx, line);
}

/* titles in order, any case alike, then by file */
static int title_cmp(const struct gui_reg *a, const struct gui_reg *b) {
    const char *x = a->title, *y = b->title;
    while(*x && lower(*x) == lower(*y)) x++, y++;
    if(lower(*x) != lower(*y)) return (u8)lower(*x) - (u8)lower(*y);
    return strcmp(a->file, b->file);
}

/* the desktop's application whose program is program (its window number), -1 */
static int app_of(const char *program) {
    int id;
    for(id = GUI_FIRST_APP; id < GUI_WINDOWS; id++) {
        const struct gui_app *a = gui_app_of(id);
        if(a && a->program && !strcmp(gui_basename(a->program), program)) return id;
    }
    return -1;
}

void reg_load(struct gui_state *g) {
    struct gui_registry *reg = &g->reg;
    struct gui_entry *list = g->files.scratch, e;   /* Files' folder being read: nobody reads one meanwhile */
    char names[GUI_REG_MAX][GUI_REG_FILENAME], path[GUI_PATH_MAX], why[96], line[160];
    bool device, readonly;
    int n, i, j, k, t, count = 0, skipped = 0;
    u32 len;
    reg->loaded = true;
    reg->count = 0;
    if(!g->fs) return;
    n = g->fs->list(g->fs->ctx, APPS_DIR, list, GUI_LIST_MAX);
    if(n < 0) return;                                   /* no folder: no programs */
    for(i = 0; i < n; i++) {                            /* the .app files, by name in byte order, the first GUI_REG_MAX */
        size_t l = strlen(list[i].name);
        if(list[i].type != GUI_FILE || l < 5 || strcmp(list[i].name + l - 4, ".app")) continue;
        if(l >= GUI_REG_FILENAME || !plain_name(list[i].name)) {    /* 11/05/26 (the review): never its bytes in the log */
            if(!skipped++) note(g, "gui: apps/: a .app file whose name is not letters, digits, '.', '-' and '_' is left out");
            continue;
        }
        for(j = count; j > 0 && strcmp(names[j - 1], list[i].name) > 0; j--)
            if(j < GUI_REG_MAX) strlcpy(names[j], names[j - 1], GUI_REG_FILENAME);
        if(j < GUI_REG_MAX) strlcpy(names[j], list[i].name, GUI_REG_FILENAME);
        if(count < GUI_REG_MAX) count++;
    }
    for(i = 0; i < count; i++) {
        struct gui_reg *r = &reg->e[reg->count];
        if(!gui_join(path, sizeof(path), APPS_DIR, names[i])) continue;
        k = g->fs->read(g->fs->ctx, path, reg->text, GUI_REG_FILE, &len);
        if(k == -EFBIG) snprintf(why, sizeof(why), "longer than %d bytes", GUI_REG_FILE);
        else if(k < 0) snprintf(why, sizeof(why), "it cannot be read: %s", gui_error(g, k));
        else if(!gui_reg_parse(reg->text, len, r, why, sizeof(why))) {
            snprintf(path, sizeof(path), BIN_DIR "/%s", r->program);
            if(g->fs->stat(g->fs->ctx, path, &e, &device, &readonly) || e.type != GUI_FILE) {
                snprintf(why, sizeof(why), "program: %s is not there", path);
                k = -1;
            } else k = 0;
        } else k = -1;
        for(j = 0; !k && j < reg->count; j++)          /* 11/05/26 (the review): one entry a program */
            if(!strcmp(reg->e[j].program, r->program)) {
                snprintf(why, sizeof(why), "program: %s has an entry already, apps/%s", r->program, reg->e[j].file);
                k = -1;
            }
        if(k) {
            snprintf(line, sizeof(line), "gui: apps/%s: %s", names[i], why);
            note(g, line);
            continue;
        }
        strlcpy(r->file, names[i], sizeof(r->file));
        r->app = app_of(r->program);
        reg->count++;
    }
    for(i = 0; i < reg->count; i++) {                   /* a type named twice: the first file's */
        for(t = 0; t < reg->e[i].ntypes; t++)
            for(j = 0; j < i; j++)
                if(type_named(&reg->e[j], reg->e[i].types[t], false)) {
                    snprintf(line, sizeof(line), "gui: apps/%s: %s is apps/%s's, the first that names it", reg->e[i].file,
                             reg->e[i].types[t], reg->e[j].file);
                    note(g, line);
                    break;
                }
    }
    for(i = 0; i < reg->count; i++) {                   /* by title */
        for(j = i; j > 0 && title_cmp(&reg->e[reg->by[j - 1]], &reg->e[i]) > 0; j--) reg->by[j] = reg->by[j - 1];
        reg->by[j] = i;
    }
}

/**** what it is for ****/

/* entry r takes the file called name: its types (or also open-with) name its extension, or its whole name after '=',
   in any case */
bool reg_takes(const struct gui_reg *r, const char *name, bool also) {
    char low[GUI_REG_TYPE];
    const char *dot = strrchr(name, '.');
    size_t i, n = strlen(name);
    if(n && n < GUI_REG_TYPE - 1) {                     /* the whole name */
        low[0] = '=';
        for(i = 0; i < n; i++) low[i + 1] = lower(name[i]);
        low[n + 1] = 0;
        if(type_named(r, low, also)) return true;
    }
    if(!dot || dot == name || !dot[1] || strlen(dot + 1) >= GUI_REG_TYPE) return false;
    for(i = 0; dot[1 + i]; i++) low[i] = lower(dot[1 + i]);
    low[i] = 0;
    return type_named(r, low, also);
}

/* the entry a double-click opens the file name in: the first whose types name it; -1 */
int reg_for(const struct gui_state *g, const char *name) {
    int k;
    for(k = 0; k < g->reg.count; k++) if(reg_takes(&g->reg.e[k], name, false)) return k;
    return -1;
}

/* the entry whose program the desktop started as pid (from Apps, a tile, or Files), -1 */
int reg_of_pid(const struct gui_state *g, int pid) {
    int k;
    for(k = 0; pid > 0 && k < g->reg.count; k++) if(g->reg.pid[k] == pid) return k;
    for(k = 0; pid > 0 && k < GUI_REG_OPENED; k++) if(g->reg.opened_pid[k] == pid) return g->reg.opened_entry[k];
    return -1;
}

/* entry k from Apps or its tile: the window of the program it started last to the front, or the program started
   (its first window takes the focus: client_launched()); nothing while the one just started has no window yet */
void reg_start(struct gui_state *g, int k) {
    struct gui_registry *reg = &g->reg;
    char msg[80];
    int id, pid;
    if(k < 0 || k >= reg->count) return;
    if(reg->e[k].app >= 0) { gui_open(g, reg->e[k].app); return; }   /* the desktop's application's own */
    g->menu = 0;
    g->dirty = true;
    if((id = client_of_pid(g, reg->pid[k])) >= 0) { gui_open(g, id); return; }
    if(reg->starting[k] && g->now - reg->at[k] < GUI_START_MS) return;
    if(!g->sys || !g->sys->run) return;
    msg[0] = 0;
    pid = g->sys->run(g->sys->ctx, reg->e[k].program, msg, sizeof(msg));
    if(pid < 0) {
        dialog_message(g, "Not started", msg[0] ? msg : "The program could not be started.", "");
        return;
    }
    client_launched(g, pid);
    reg->pid[k] = pid;
    reg->at[k] = g->now;
    reg->starting[k] = true;
}

/* the file at path opened in entry k's program, started with the path as its argument: 0, or -1 when it could not
   be started (said in a message) */
int reg_open(struct gui_state *g, int k, const char *path) {
    struct gui_registry *reg = &g->reg;
    char msg[80];
    int pid, i;
    if(k < 0 || k >= reg->count || !g->sys || !g->sys->run_file) return -1;
    msg[0] = 0;
    pid = g->sys->run_file(g->sys->ctx, reg->e[k].program, path, msg, sizeof(msg));
    if(pid < 0) {
        dialog_message(g, "Not opened", msg[0] ? msg : "The program could not be started.", gui_basename(path));
        return -1;
    }
    client_launched(g, pid);
    for(i = GUI_REG_OPENED - 1; i > 0; i--) {           /* the oldest makes room */
        reg->opened_pid[i] = reg->opened_pid[i - 1];
        reg->opened_entry[i] = reg->opened_entry[i - 1];
    }
    reg->opened_pid[0] = pid;
    reg->opened_entry[0] = k;
    g->dirty = true;
    return 0;
}

/* its icon, 16 x 16 at scale: its own, or the programs' (ICON_PROGRAM) */
void reg_icon(const struct gui_draw *d, int x, int y, const struct gui_reg *r, int scale) {
    int row, col, start;
    if(!r || !r->icon_set) { gd_icon(d, x, y, ICON_PROGRAM, scale); return; }
    for(row = 0; row < GUI_REG_ICON; row++)
        for(col = 0; col < GUI_REG_ICON;) {
            char c = r->icon[row][col];
            if(c == '.') { col++; continue; }
            start = col;
            while(col < GUI_REG_ICON && r->icon[row][col] == c) col++;
            gd_rect(d, x + start * scale, y + row * scale, (col - start) * scale, scale, gui_reg_color(c));
        }
}
