/*
SurfOS Desktop: Disks
--------------------
File: guidisks.c Date: 10/17/26
--------------------
The block devices as lsblk lists them (struct gui_sys's disks, shell/guisys_disk.c): each disk
with its partitions under it, their size, bus, type, model or label and status in lsblk's own
words, the columns as wide as what they hold (the model gives way first, as in lsblk). Below
the list, the selected device's details, and for a mounted volume where it is mounted and a
bar of used and free space. Mount and Eject, for USB volumes only, are struct gui_fs's: the
shell's own mount and eject, so the storage rules hold (the boot drive mounts read-only, a
busy stick is not ejected); what they say goes to the status bar, and Eject also waits for
Notepad's unsaved changes to a file there, as Files' does. The tick reads the devices again
about every two seconds while the window is open and in view, so a stick that is plugged in
appears; the selection follows a device's name. Track SYS of
docs/superpowers/plans/2026-10-17-gui-suite.md; the window manager reaches it through
gui_app_disks (struct gui_app, shell/guipriv.h).
*/
#include "gui_sys.h"
#include <string.h>
#include <stdio.h>

#define READ_MS 2000                    /* how often the tick reads the devices */
#define DETAIL_H 66                     /* the selected device's details, above the status bar */
enum { TOOL_MOUNT, TOOL_EJECT, TOOL_REFRESH, TOOL_FILES };

static const struct gui_tool tools[] = {
    { "Mount", TOOL_MOUNT }, { "Eject", TOOL_EJECT }, { "Refresh", TOOL_REFRESH }, { "Show in Files", TOOL_FILES },
};
#define TOOLS (int)(sizeof(tools) / sizeof(tools[0]))

static const char *const help[] = {
    "#Disks",
    "The block devices as lsblk lists them, read every 2 s:",
    "the disks, their partitions under them. The arrows,",
    "Page Up and Down, Home and End select one; its details",
    "and, mounted, a bar of used and free space are below.",
    "Enter (or a double-click) mounts a USB volume, or shows",
    "a mounted one in Files. M mounts, E ejects the stick",
    "(every volume of it; then pull it out), Ctrl+R reads",
    "the devices again, Ctrl+C copies the line. Mount and",
    "Eject are the shell's own: the boot drive mounts",
    "read-only, a busy stick is not ejected.",
};

struct disks_layout {
    int head_y, list_y, rows, bar_x, detail_y, status_y;
    int size_r, bus_x, type_x, label_x, label_cells, status_x, status_cells;   /* the columns (window-relative) */
};

static int min_width(void) {
    int w = tool_span(tools, TOOLS);
    return w > 360 ? w : 360;
}

static int longest(int now, const char *s, int more) {
    int n = (int)strlen(s) + more;
    return n > now ? n : now;
}

static void layout(const struct gui_state *g, struct disks_layout *l) {
    const struct gui_window *w = &g->win[GUI_DISKS];
    const struct gui_disks *k = &g->disks;
    int name = 4, size = 4, type = 4, label = 11, status = 6, i;
    l->head_y = TITLE_H + BAR_H + 3;
    l->list_y = l->head_y + 20;
    l->status_y = w->h - STATUS_H - 1;
    l->detail_y = l->status_y - DETAIL_H;
    l->rows = (l->detail_y - l->list_y) / ROW_H;
    if(l->rows < 1) l->rows = 1;
    l->bar_x = w->w - 2 - SCROLL_W;
    for(i = 0; i < k->count; i++) {                     /* as lsblk: each column as wide as what it holds */
        name = longest(name, k->disk[i].name, k->disk[i].part ? 2 : 0);   /* a partition: two cells in */
        size = longest(size, k->disk[i].size, 0);
        type = longest(type, k->disk[i].type, 0);
        label = longest(label, k->disk[i].label, 0);
        status = longest(status, k->disk[i].status, 0);
    }
    l->size_r = 28 + name * 8 + 8 + size * 8;           /* the icon at 8, the names at 28 */
    l->bus_x = l->size_r + 8;
    l->type_x = l->bus_x + 3 * 8 + 8;
    l->label_x = l->type_x + type * 8 + 8;
    l->status_x = l->label_x + label * 8 + 8;
    if(l->status_x + status * 8 > l->bar_x - 6) l->status_x = l->bar_x - 6 - status * 8;
    l->label_cells = (l->status_x - 8 - l->label_x) / 8;
    if(l->label_cells < 4) {                            /* the model gives way first */
        l->label_cells = 0;
        l->status_x = l->label_x;
    }
    l->status_cells = (l->bar_x - 6 - l->status_x) / 8;
}

/**** the devices ****/

static const struct gui_disk *selected(const struct gui_disks *k) {
    return k->selected >= 0 && k->selected < k->count ? &k->disk[k->selected] : NULL;
}

static bool usb(const struct gui_disk *d) { return !strcmp(d->bus, "usb"); }

/* a volume: a partition, or a disk without partitions */
static bool volume(const struct gui_disks *k, int i) {
    int j;
    if(k->disk[i].part) return true;
    for(j = 0; j < k->count; j++) if(k->disk[j].part && !strcmp(k->disk[j].disk, k->disk[i].name)) return false;
    return true;
}

static bool mountable(const struct gui_disks *k, int i) {
    return volume(k, i) && usb(&k->disk[i]) && !k->disk[i].mounted && !k->disk[i].gone;
}

/* the row Mount mounts: the selected volume, or the first partition of the selected disk that is not mounted; -1 */
static int mount_row(const struct gui_state *g) {
    const struct gui_disks *k = &g->disks;
    int s = k->selected, i;
    if(!selected(k) || !g->fs || !g->fs->mount) return -1;
    if(volume(k, s)) return mountable(k, s) ? s : -1;
    for(i = 0; i < k->count; i++)
        if(k->disk[i].part && !strcmp(k->disk[i].disk, k->disk[s].name) && mountable(k, i)) return i;
    return -1;
}

/* the disk Eject ejects: the selected device's, when it is a USB disk with something of it mounted; NULL */
static const char *eject_disk(const struct gui_state *g) {
    const struct gui_disks *k = &g->disks;
    const struct gui_disk *d = selected(k);
    int i;
    if(!d || !g->fs || !g->fs->eject || !usb(d)) return NULL;
    for(i = 0; i < k->count; i++) if(!strcmp(k->disk[i].disk, d->disk) && k->disk[i].mounted) return d->disk;
    return NULL;
}

static bool can_show(const struct gui_state *g) {
    const struct gui_disk *d = selected(&g->disks);
    return d && g->fs && d->mounted && !d->gone && d->mount[0];
}

/* everything again, from struct gui_sys (from the handlers and the tick only); the selection by its name, or where
   it was when that device is gone */
static void read_disks(struct gui_state *g) {
    struct gui_disks *k = &g->disks;
    struct disks_layout l;
    int n, i;
    k->read_ms = g->now;
    k->read = true;
    g->dirty = true;
    n = g->sys && g->sys->disks ? g->sys->disks(g->sys->ctx, k->disk, GUI_DISK_MAX) : -ENOSYS;
    k->err = n < 0 ? n : 0;
    k->count = n < 0 ? 0 : n > GUI_DISK_MAX ? GUI_DISK_MAX : n;
    for(i = 0; i < k->count && strcmp(k->disk[i].name, k->name); i++) ;
    if(i < k->count) k->selected = i;
    else {
        if(k->name[0] && !k->err) snprintf(k->status, sizeof(k->status), "%s is not there any more", k->name);
        k->selected = k->count ? gui_clamp(k->selected, 0, k->count - 1) : -1;
    }
    strlcpy(k->name, k->selected >= 0 ? k->disk[k->selected].name : "", sizeof(k->name));
    layout(g, &l);
    k->top = gui_clamp(k->top, 0, k->count > l.rows ? k->count - l.rows : 0);
}

static void choose(struct gui_state *g, int row) {
    struct gui_disks *k = &g->disks;
    struct disks_layout l;
    if(row < 0 || row >= k->count) return;
    k->selected = row;
    strlcpy(k->name, k->disk[row].name, sizeof(k->name));
    k->status[0] = 0;
    layout(g, &l);
    k->top = sys_view(row, k->top, k->count, l.rows);
    g->dirty = true;
}

/**** Mount, Eject, Show in Files ****/

static void mount(struct gui_state *g) {
    struct gui_disks *k = &g->disks;
    const struct gui_disk *d = selected(k);
    char name[16], msg[GUI_STATUS];
    int v = mount_row(g), r;
    g->dirty = true;
    if(v < 0) {
        if(!d) return;
        if(!g->fs || !g->fs->mount) snprintf(k->status, sizeof(k->status), "Mount needs the file systems: none here");
        else if(!usb(d))
            snprintf(k->status, sizeof(k->status), "%s is not on a USB drive: Disks mounts USB volumes", d->name);
        else if(d->gone) snprintf(k->status, sizeof(k->status), "%s was unplugged", d->name);
        else if(d->mounted) snprintf(k->status, sizeof(k->status), "%s is mounted already, on %s", d->name, d->mount);
        else snprintf(k->status, sizeof(k->status), "Every partition of %s is mounted", d->name);
        return;
    }
    strlcpy(name, k->disk[v].name, sizeof(name));
    msg[0] = 0;
    r = g->fs->mount(g->fs->ctx, name, msg, sizeof(msg));
    if(!r) strlcpy(k->name, name, sizeof(k->name));    /* the volume that was mounted */
    read_disks(g);
    strlcpy(k->status, msg[0] ? msg : r ? "Not mounted" : "Mounted", sizeof(k->status));   /* what mount said, first */
    if(g->win[GUI_FILES].open) files_refresh(g);       /* Files shows it at once too */
}

static void eject(struct gui_state *g) {
    struct gui_disks *k = &g->disks;
    const struct gui_disk *d = selected(k);
    const char *disk = eject_disk(g);
    char name[16], msg[GUI_STATUS];
    u32 len;
    int i, r;
    g->dirty = true;
    if(!disk) {
        if(!d) return;
        if(!g->fs || !g->fs->eject) snprintf(k->status, sizeof(k->status), "Eject needs the file systems: none here");
        else if(!usb(d))
            snprintf(k->status, sizeof(k->status), "%s is not on a USB drive: Disks ejects USB sticks", d->name);
        else snprintf(k->status, sizeof(k->status), "Nothing of %s is mounted", d->disk);
        return;
    }
    strlcpy(name, disk, sizeof(name));
    for(i = 0; i < k->count && g->note.modified; i++) {   /* eject takes every volume of the disk along */
        len = strlen(k->disk[i].mount);
        if(!len || strcmp(k->disk[i].disk, name) || strncmp(g->note.path, k->disk[i].mount, len) || g->note.path[len] != '/')
            continue;
        snprintf(k->status, sizeof(k->status), "Not ejected: Notepad has unsaved changes to a file on %s", name);
        return;
    }
    msg[0] = 0;
    r = g->fs->eject(g->fs->ctx, name, msg, sizeof(msg));
    read_disks(g);                                      /* an unplugged stick is gone from the list now */
    strlcpy(k->status, msg[0] ? msg : r ? "Not ejected" : "Ejected", sizeof(k->status));   /* what eject said, first */
    if(g->win[GUI_FILES].open) files_refresh(g);
}

static void show_in_files(struct gui_state *g) {
    char path[GUI_PATH_MAX];
    if(!can_show(g)) return;
    strlcpy(path, selected(&g->disks)->mount, sizeof(path));
    gui_act(g, ACT_FILES);
    files_reveal(g, path);
}

/* Enter, or a double-click: mount a volume, or show a mounted one */
static void open_selected(struct gui_state *g) {
    if(mount_row(g) >= 0) mount(g);
    else if(can_show(g)) show_in_files(g);
    else mount(g);                                      /* which says why not */
}

static void refresh(struct gui_state *g) {
    g->disks.status[0] = 0;
    read_disks(g);
}

/**** the Edit menu: Copy ****/

static void copy(struct gui_state *g) {
    struct gui_disks *k = &g->disks;
    const struct gui_disk *d = selected(k);
    char line[GUI_PATH_MAX + 160];
    u32 len;
    if(!d) return;
    snprintf(line, sizeof(line), "%s  %s  %s  %s%s%s%s%s%s%s", d->name, d->size, d->bus, d->type, d->label[0] ? "  " : "",
             d->label, d->status[0] ? "  " : "", d->status, d->mount[0] ? "  " : "", d->mount);
    len = strlen(line);
    memcpy(g->clip, line, len);
    g->clip_len = len;
    snprintf(k->status, sizeof(k->status), "Copied the line of %s", d->name);
    g->dirty = true;
}

static bool can_edit(const struct gui_state *g, int act) {
    return act == ACT_COPY && selected(&g->disks) != NULL;
}

static void edit(struct gui_state *g, int act) {
    if(act == ACT_COPY) copy(g);
}

/**** keys and the pointer ****/

static bool key(struct gui_state *g, int k) {
    struct gui_disks *d = &g->disks;
    struct disks_layout l;
    int row;
    layout(g, &l);
    row = sys_move(d->selected, d->count, l.rows, k);
    if(row != -2) {
        if(row >= 0) choose(g, row);
        return true;
    }
    switch(k) {
    case '\r': case '\n': open_selected(g); return true;
    case 'm': case 'M': mount(g); return true;
    case 'e': case 'E': eject(g); return true;
    case 18: refresh(g); return true;                   /* Ctrl+R, as in Files */
    }
    return false;
}

static void press(struct gui_state *g, int x, int y) {
    struct gui_disks *k = &g->disks;
    struct disks_layout l;
    int i, row, top, size;
    layout(g, &l);
    i = tool_hit(tools, TOOLS, x, y);
    if(i >= 0) {
        switch(tools[i].act) {
        case TOOL_MOUNT: if(mount_row(g) >= 0) mount(g); break;
        case TOOL_EJECT: if(eject_disk(g)) eject(g); break;
        case TOOL_REFRESH: refresh(g); break;
        case TOOL_FILES: show_in_files(g); break;
        }
        return;
    }
    if(y < l.list_y || y >= l.list_y + l.rows * ROW_H) return;
    if(x >= l.bar_x) {                                  /* the scroll bar: a page, or hold the thumb */
        gd_thumb(l.rows * ROW_H, k->top, l.rows, k->count, &top, &size);
        if(y - l.list_y < top) k->top -= l.rows;
        else if(y - l.list_y >= top + size) k->top += l.rows;
        else { k->thumb = true; k->scroll_at = y - l.list_y - top; }
        k->top = gui_clamp(k->top, 0, k->count > l.rows ? k->count - l.rows : 0);
        g->dirty = true;
        return;
    }
    row = k->top + (y - l.list_y) / ROW_H;
    if(row >= k->count) return;
    choose(g, row);
    if(gui_double(&k->click_ms, &k->click_row, g->now, row)) open_selected(g);
}

static void drag(struct gui_state *g, int x, int y) {
    struct gui_disks *k = &g->disks;
    struct disks_layout l;
    (void)x;
    if(!k->thumb) return;
    layout(g, &l);
    k->top = gd_scroll_to(l.rows * ROW_H, y - l.list_y - k->scroll_at, l.rows, k->count);
    g->dirty = true;
}

static void release(struct gui_state *g) {
    g->disks.thumb = false;
}

static void wheel(struct gui_state *g, int rows) {
    struct gui_disks *k = &g->disks;
    struct disks_layout l;
    layout(g, &l);
    k->top = gui_clamp(k->top + rows, 0, k->count > l.rows ? k->count - l.rows : 0);
    g->dirty = true;
}

/**** opening, and every frame ****/

static void opened(struct gui_state *g) {
    struct gui_disks *k = &g->disks;
    k->status[0] = 0;
    k->name[0] = 0;
    k->selected = 0;
    k->top = 0;
    k->thumb = false;
    k->click_row = -1;
    read_disks(g);
}

static void tick(struct gui_state *g) {
    struct gui_disks *k = &g->disks;
    if(g->win[GUI_DISKS].min) return;                   /* tucked away: read again once it is back */
    if(k->read && g->now - k->read_ms < READ_MS) return;
    read_disks(g);
}

/**** drawing ****/

/* "part mbr FAT32 LBA": the table ("mbr") and what the partition is ("FAT32 LBA"); a disk: its table, or "" */
static void type_words(const struct gui_disk *d, char *table, size_t tn, const char **what) {
    const char *s = d->type, *sp;
    table[0] = 0;
    *what = "";
    if(!strncmp(s, "part ", 5) || !strncmp(s, "disk ", 5)) {
        s += 5;
        sp = strchr(s, ' ');
        snprintf(table, tn, "%.*s", sp ? (int)(sp - s) : (int)strlen(s), s);
        *what = sp ? sp + 1 : "";
    }
}

static void table_name(const char *table, char *out, size_t n) {
    strlcpy(out, !strcmp(table, "gpt") ? "GPT" : !strcmp(table, "mbr") ? "MBR" : table, n);
}

/* the details' three lines; bar: the third is a bar of used space before its words */
static void details(const struct gui_state *g, const struct gui_disk *d, char lines[3][112], bool *bar) {
    const struct gui_disks *k = &g->disks;
    char table[16], tname[16], used[16], free[16], total[16];
    const char *what;
    int parts = 0, i;
    *bar = false;
    lines[0][0] = lines[1][0] = lines[2][0] = 0;
    type_words(d, table, sizeof(table), &what);
    table_name(table, tname, sizeof(tname));
    if(d->part) {
        snprintf(lines[0], sizeof(lines[0]), "%s: %s partition of %s, %s%s%s%s", d->name, tname, d->disk, d->size,
                 what[0] ? " (" : "", what, what[0] ? ")" : "");
        if(d->label[0]) snprintf(lines[1], sizeof(lines[1]), "Named %s", d->label);
    } else {
        for(i = 0; i < k->count; i++) if(k->disk[i].part && !strcmp(k->disk[i].disk, d->name)) parts++;
        snprintf(lines[0], sizeof(lines[0]), "%s: %s disk, %s%s%s", d->name, !strcmp(d->bus, "usb") ? "USB" :
                 !strcmp(d->bus, "ata") ? "internal (ATA)" : !strcmp(d->bus, "ram") ? "RAM" : "a", d->size,
                 d->removable ? ", removable" : "", d->label[0] ? ", " : "");
        strlcat(lines[0], d->label, sizeof(lines[0]));
        if(parts) snprintf(lines[1], sizeof(lines[1]), "%s, %d partition%s", tname, parts, parts == 1 ? "" : "s");
        else if(table[0]) snprintf(lines[1], sizeof(lines[1]), "%s, no partitions", tname);
        else strlcpy(lines[1], "No partition table", sizeof(lines[1]));
    }
    if(d->gone) {
        snprintf(lines[1], sizeof(lines[1]), "Unplugged%s%s", d->mounted ? " while mounted on " : "",
                 d->mounted ? d->mount : "");
        strlcpy(lines[2], d->mounted ? "Eject unmounts it; nothing more can be written to it" : "", sizeof(lines[2]));
        return;
    }
    if(d->mounted) {
        snprintf(lines[1], sizeof(lines[1]), "Mounted on %s: %s, %s%s%s", d->mount, d->fs[0] ? d->fs : "?",
                 d->readonly ? "read-only" : "read-write", d->volume[0] ? ", volume " : "", d->volume);
        if(d->used_kb || d->free_kb) {
            sys_kb(d->used_kb, used, sizeof(used));
            sys_kb(d->free_kb, free, sizeof(free));
            sys_kb(d->used_kb + d->free_kb, total, sizeof(total));
            snprintf(lines[2], sizeof(lines[2]), "%s used, %s free, of %s (%u%%)", used, free, total,
                     sys_part(d->used_kb, d->used_kb + d->free_kb, 100));
            *bar = true;
        } else snprintf(lines[2], sizeof(lines[2]), "%s keeps no count of used and free space", d->fs[0] ? d->fs : "It");
        return;
    }
    if(volume(k, (int)(d - k->disk)) && usb(d))
        snprintf(lines[2], sizeof(lines[2]), "Not mounted: Mount (or Enter) opens it%s at /%s",
                 d->readonly ? " read-only" : "", d->name);
    if(d->status[0]) {
        if(lines[2][0]) snprintf(lines[1] + strlen(lines[1]), sizeof(lines[1]) - strlen(lines[1]), "%s%s",
                                 lines[1][0] ? "; " : "", d->status);
        else strlcpy(lines[2], d->status, sizeof(lines[2]));
    }
}

static void list_render(const struct gui_state *g, const struct gui_draw *d, const struct disks_layout *l) {
    const struct gui_window *w = &g->win[GUI_DISKS];
    const struct gui_disks *k = &g->disks;
    struct gui_draw list;
    bool focused = g->focus == GUI_DISKS && !g->dialog.kind && !g->menu;
    int x0 = w->x, y0 = w->y, row, py, ix;
    gd_rect(d, x0 + 1, y0 + l->head_y, w->w - 2, 20, SHELL);
    gd_text(d, x0 + 28, y0 + l->head_y + 2, "Name", MUTED);
    gd_text(d, x0 + l->size_r - 32, y0 + l->head_y + 2, "Size", MUTED);
    gd_text(d, x0 + l->bus_x, y0 + l->head_y + 2, "Bus", MUTED);
    gd_text(d, x0 + l->type_x, y0 + l->head_y + 2, "Type", MUTED);
    if(l->label_cells) gd_name(d, x0 + l->label_x, y0 + l->head_y + 2, "Model/label", l->label_cells, MUTED);
    gd_name(d, x0 + l->status_x, y0 + l->head_y + 2, "Status", l->status_cells, MUTED);
    gd_rect(d, x0 + 1, y0 + l->list_y, l->bar_x - 1, l->detail_y - l->list_y, SYS_SHEET);
    list = gd_clip(d, x0 + 1, y0 + l->list_y, l->bar_x - 1, l->rows * ROW_H);
    if(!g->sys || k->err)
        gd_text(&list, x0 + 16, y0 + l->list_y + 8, !g->sys ? "The block devices cannot be read here." :
                "The block devices could not be read.", MUTED);
    else if(!k->count) gd_text(&list, x0 + 16, y0 + l->list_y + 8, "No block devices", MUTED);
    for(row = 0; row < l->rows && k->top + row < k->count; row++) {
        const struct gui_disk *e = &k->disk[k->top + row];
        py = y0 + l->list_y + row * ROW_H;
        ix = x0 + 8 + (e->part ? 16 : 0);
        if(k->top + row == k->selected) gd_rect(&list, x0 + 1, py, l->bar_x - 1, ROW_H, focused ? PICK : BAND);
        gd_icon(&list, ix, py + 1, ICON_DRIVE, 1);
        gd_name(&list, ix + 20, py + 1, e->name, 15, INK);
        gd_text(&list, x0 + l->size_r - (int)strlen(e->size) * 8, py + 1, e->size, MUTED);
        gd_text(&list, x0 + l->bus_x, py + 1, e->bus, MUTED);
        gd_name(&list, x0 + l->type_x, py + 1, e->type, (l->label_x - l->type_x) / 8, MUTED);
        if(l->label_cells) gd_name(&list, x0 + l->label_x, py + 1, e->label, l->label_cells, MUTED);
        gd_name(&list, x0 + l->status_x, py + 1, e->status, l->status_cells, e->gone ? CORAL : e->mounted ? TEAL : MUTED);
    }
    gd_scrollbar(d, x0 + l->bar_x, y0 + l->list_y, l->rows * ROW_H, k->top, l->rows, k->count);
}

static void details_render(const struct gui_state *g, const struct gui_draw *d, const struct disks_layout *l) {
    const struct gui_window *w = &g->win[GUI_DISKS];
    const struct gui_disk *e = selected(&g->disks);
    char lines[3][112];
    bool bar;
    int x0 = w->x + 10, y = w->y + l->detail_y, cells = (w->w - 20) / 8, bw = (w->w - 20) / 3, x;
    gd_rect(d, w->x + 1, y, w->w - 2, 1, LINE);
    if(!e) return;
    details(g, e, lines, &bar);
    x = gd_name(d, x0, y + 8, e->name, cells, TEAL);
    gd_name(d, x, y + 8, lines[0] + strlen(e->name), cells - (x - x0) / 8, INK);   /* the name in colour, then the rest */
    gd_name(d, x0, y + 26, lines[1], cells, INK);
    if(bar) {
        sys_bar(d, x0, y + 46, bw, 14, e->used_kb, e->used_kb + e->free_kb);
        gd_name(d, x0 + bw + 10, y + 45, lines[2], cells - (bw + 10) / 8, MUTED);
    } else gd_name(d, x0, y + 45, lines[2], cells, MUTED);
}

static void render(const struct gui_state *g, const struct gui_draw *d) {
    const struct gui_window *w = &g->win[GUI_DISKS];
    const struct gui_disks *k = &g->disks;
    const struct gui_disk *e = selected(k);
    struct disks_layout l;
    bool on[TOOLS];
    char b[GUI_STATUS + 64], right[24];
    int x0 = w->x, y0 = w->y, x;
    layout(g, &l);
    on[TOOL_MOUNT] = mount_row(g) >= 0;
    on[TOOL_EJECT] = eject_disk(g) != NULL;
    on[TOOL_REFRESH] = g->sys != NULL;
    on[TOOL_FILES] = can_show(g);
    gd_rect(d, x0 + 1, y0 + TITLE_H, w->w - 2, BAR_H + 2, BAND);
    tool_render(d, x0, y0, tools, TOOLS, on);
    gd_rect(d, x0 + 1, y0 + TITLE_H + BAR_H + 2, w->w - 2, 1, LINE);
    list_render(g, d, &l);
    details_render(g, d, &l);
    gd_rect(d, x0 + 1, y0 + l.status_y, w->w - 2, STATUS_H, BAND);
    gd_rect(d, x0 + 1, y0 + l.status_y, w->w - 2, 1, LINE);
    if(k->status[0]) strlcpy(b, k->status, sizeof(b));
    else if(!g->sys) strlcpy(b, "No system here: no devices to show", sizeof(b));
    else if(!g->fs && e) strlcpy(b, "No file systems here: Mount and Eject are off", sizeof(b));
    else if(e && e->gone) snprintf(b, sizeof(b), "%s was unplugged: Eject (E) unmounts it", e->name);
    else if(e && e->mounted && usb(e)) snprintf(b, sizeof(b), "Eject (E) %s before you pull it out", e->disk);
    else if(e && mount_row(g) >= 0) snprintf(b, sizeof(b), "Mount (M) or Enter mounts %s", k->disk[mount_row(g)].name);
    else b[0] = 0;
    if(g->sys) snprintf(right, sizeof(right), "%d device%s", k->count, k->count == 1 ? "" : "s");
    else right[0] = 0;
    x = w->w - 24 - (int)strlen(right) * 8;             /* clear of the resize grip */
    gd_name(d, x0 + 10, y0 + l.status_y + 3, b, (x - 18) / 8, INK);
    gd_text(d, x0 + x, y0 + l.status_y + 3, right, MUTED);
}

const struct gui_app gui_app_disks = {
    .title = "Disks", .icon = ICON_DISKS, .shortcut = "", .w = 580, .h = 320,
    .resizable = true, .arrows = true, .keys = false,
    .min_width = min_width,
    .opened = opened, .render = render, .key = key, .press = press, .drag = drag, .release = release, .wheel = wheel,
    .tick = tick,
    .help = help, .help_lines = (int)(sizeof(help) / sizeof(help[0])),
    .can_edit = can_edit, .edit = edit,
};
