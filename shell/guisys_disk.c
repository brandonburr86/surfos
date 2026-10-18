/*
SurfOS Desktop: Disks' Side of struct gui_sys
--------------------
File: guisys_disk.c Date: 10/17/26
--------------------
The block devices for Disks (struct gui_disk), as lsblk lists them: each disk followed by
its partitions (bdev_walk()), in lsblk's own words (bdev_describe(): size, type, bus, model
or partition name, status); then the devices that were unplugged while mounted, which are
off lsblk's list but still mounted until Eject. A mounted one says where it is mounted
(vfs_mount_views()) and how full it is (vfs_statfs(), which the file system answers from
what it keeps: FAT counted its free clusters when it was mounted). Nothing here reads a
device or waits for one: an unmounted volume's own label is on the device, so Disks shows
lsblk's column for it. Mounting and ejecting are struct gui_fs's: the shell's own mount
and eject (shell/guifs.c). Track SYS of docs/superpowers/plans/2026-10-17-gui-suite.md.
*/
#include <stdio.h>
#include <string.h>
#include <surfos/types.h>
#include <sys/bdev.h>
#include <fs/vfs.h>
#include <mm/kalloc.h>
#include "guisys.h"

#define VIEWS 32                        /* the mounts looked at, as many as Files' volumes and the shell's mount */

struct rows {
    struct gui_disk *out;
    struct bdev *dev[GUI_DISK_MAX];     /* the device of each row */
    int n, max;
};

/* a row for d (no more than max), lsblk's words in it */
static struct gui_disk *row(struct rows *r, struct bdev *d) {
    struct gui_disk *o;
    struct bdev_info i;
    if(r->n >= r->max) return NULL;
    o = &r->out[r->n];
    memset(o, 0, sizeof(*o));
    bdev_describe(d, &i);
    strlcpy(o->name, d->name, sizeof(o->name));
    strlcpy(o->disk, bdev_disk(d)->name, sizeof(o->disk));
    strlcpy(o->bus, i.bus, sizeof(o->bus));
    strlcpy(o->type, i.type, sizeof(o->type));
    strlcpy(o->label, i.label, sizeof(o->label));
    strlcpy(o->status, i.status, sizeof(o->status));
    strlcpy(o->size, i.size, sizeof(o->size));
    o->size_kb = d->nblocks / 2;
    o->part = d->parent != NULL;
    o->removable = d->removable;
    o->readonly = d->readonly || bdev_disk(d)->readonly || bdev_mount_readonly(d);
    r->dev[r->n++] = d;
    return o;
}

static void listed(struct bdev *d, void *arg) {
    row((struct rows *)arg, d);
}

static bool in_rows(const struct rows *r, const struct bdev *d) {
    int k;
    for(k = 0; k < r->n; k++) if(r->dev[k] == d) return true;
    return false;
}

/* the unplugged disks that something is still mounted from: each disk, then those of its devices */
static void unplugged(struct rows *r, const struct vfs_mount_view *v, int nv) {
    struct gui_disk *o;
    struct bdev *disk;
    int j, k;
    for(j = 0; j < nv; j++) {
        if(!v[j].dev || !v[j].dev->gone || in_rows(r, v[j].dev)) continue;
        disk = bdev_disk(v[j].dev);
        if(!in_rows(r, disk) && (o = row(r, disk))) {
            strlcpy(o->status, disk == v[j].dev ? "unplugged: eject it" : "unplugged", sizeof(o->status));
            o->gone = true;
        }
        for(k = j; k < nv; k++) {                       /* its partitions that are mounted, together */
            if(!v[k].dev || bdev_disk(v[k].dev) != disk || in_rows(r, v[k].dev) || !(o = row(r, v[k].dev))) continue;
            strlcpy(o->status, "unplugged: eject it", sizeof(o->status));
            o->gone = true;
        }
    }
}

int guisys_disks(void *ctx, struct gui_disk *out, int max) {
    struct vfs_mount_view *v = (struct vfs_mount_view *)kalloc(VIEWS * sizeof(*v));
    struct rows r;
    struct statfs st;
    int nv, j, k;
    (void)ctx;
    if(!v) return -ENOMEM;
    nv = vfs_mount_views(v, VIEWS);
    if(nv > VIEWS) nv = VIEWS;
    r.out = out;
    r.n = 0;
    r.max = max < GUI_DISK_MAX ? max : GUI_DISK_MAX;
    bdev_walk(listed, &r);
    unplugged(&r, v, nv);
    for(k = 0; k < r.n; k++) {                          /* where each mounted one is, and how full */
        for(j = 0; j < nv && v[j].dev != r.dev[k]; j++) ;
        if(j == nv) continue;
        out[k].mounted = true;
        out[k].readonly = v[j].readonly;
        strlcpy(out[k].mount, v[j].path, sizeof(out[k].mount));
        strlcpy(out[k].fs, v[j].type, sizeof(out[k].fs));
        if(r.dev[k]->gone || vfs_statfs(v[j].path, &st) != 0 || !st.units) continue;
        out[k].used_kb = (u32)((u64)(st.units - st.free_units) * st.unit >> 10);
        out[k].free_kb = (u32)((u64)st.free_units * st.unit >> 10);
        strlcpy(out[k].fs, st.fs, sizeof(out[k].fs));
        strlcpy(out[k].volume, st.label, sizeof(out[k].volume));
    }
    kfree(v);
    return r.n;
}
