/*
SurfOS Desktop: Disks' Side of struct gui_sys
--------------------
File: guisys_disk.c Date: 10/17/26
--------------------
The block devices for Disks (struct gui_disk), as lsblk lists them: each disk followed by
its partitions (bdev_walk()), in lsblk's own words (bdev_describe(): size, type, bus, model
or partition name, status); then the devices that were unplugged while mounted, which are
off lsblk's list but still mounted until Eject. A mounted one says where it is mounted
(vfs_mount_views()) and how full it is (vfs_statfs()). Mounting and ejecting are struct
gui_fs's: the shell's own mount and eject (shell/guifs.c). Track SYS of
docs/superpowers/plans/2026-10-17-gui-suite.md.
10/18/26: what this file reads, and what it can wait for. It reads nothing from a device
itself: an unmounted volume's own label is on the device, so Disks shows lsblk's column for
it. But vfs_statfs() takes the file system's lock, so a reading can wait behind another
task's long write to that volume, and FAT counts a read-only volume's free space once (at
its mount, or at the first statfs). The device list is taken with interrupts off, so a stick
unplugged meanwhile (bdev_unregister()) cannot cut the walk short and drop the devices after
it; the rows that only Eject can end (unplugged while mounted) are kept when there are more
devices than rows; a statfs that fails is told (statfs_err).
*/
#include <stdio.h>
#include <string.h>
#include <surfos/types.h>
#include <surfos/irq.h>
#include <sys/bdev.h>
#include <fs/vfs.h>
#include <mm/kalloc.h>
#include "guisys.h"

#define VIEWS 32                        /* the mounts looked at, as many as Files' volumes and the shell's mount */
#define WALKED (GUI_DISK_MAX * 2)       /* lsblk's devices looked at: more than the rows hold are cut anyway */

/* a row to be: its device, and its own status ("unplugged") or NULL for lsblk's */
struct pick { struct bdev *dev; const char *status; };
struct walked { struct bdev *dev[WALKED]; int n; };

static void walk_one(struct bdev *d, void *arg) {
    struct walked *w = (struct walked *)arg;
    if(w->n < WALKED) w->dev[w->n++] = d;
}

static bool walked(const struct walked *w, const struct bdev *d) {
    int k;
    for(k = 0; k < w->n; k++) if(w->dev[k] == d) return true;
    return false;
}

static bool picked(const struct pick *p, int n, const struct bdev *d) {
    int k;
    for(k = 0; k < n; k++) if(p[k].dev == d) return true;
    return false;
}

/* the unplugged disks that something is still mounted from, each followed by those of its devices: off lsblk's
   list (unless it went between the walk and now: then it is there already) */
static int unplugged(struct pick *out, int max, const struct walked *w, const struct vfs_mount_view *v, int nv) {
    struct bdev *disk;
    int n = 0, j, k;
    for(j = 0; j < nv && n < max; j++) {
        if(!v[j].dev || !v[j].dev->gone || walked(w, v[j].dev) || picked(out, n, v[j].dev)) continue;
        disk = bdev_disk(v[j].dev);
        if(!walked(w, disk) && !picked(out, n, disk)) {
            out[n].dev = disk;
            out[n++].status = disk == v[j].dev ? "unplugged: eject it" : "unplugged";
        }
        for(k = j; k < nv && n < max; k++) {            /* its partitions that are mounted, together */
            if(!v[k].dev || bdev_disk(v[k].dev) != disk || walked(w, v[k].dev) || picked(out, n, v[k].dev)) continue;
            out[n].dev = v[k].dev;
            out[n++].status = "unplugged: eject it";
        }
    }
    return n;
}

/* row o for device d, in lsblk's words (status: its own, or NULL for lsblk's) */
static void row(struct gui_disk *o, struct bdev *d, const char *status) {
    struct bdev_info i;
    memset(o, 0, sizeof(*o));
    bdev_describe(d, &i);
    strlcpy(o->name, d->name, sizeof(o->name));
    strlcpy(o->disk, bdev_disk(d)->name, sizeof(o->disk));
    strlcpy(o->bus, i.bus, sizeof(o->bus));
    strlcpy(o->type, i.type, sizeof(o->type));
    strlcpy(o->label, i.label, sizeof(o->label));
    strlcpy(o->status, status ? status : i.status, sizeof(o->status));
    strlcpy(o->size, i.size, sizeof(o->size));
    o->size_kb = d->nblocks / 2;
    o->part = d->parent != NULL;
    o->removable = d->removable;
    o->readonly = d->readonly || bdev_disk(d)->readonly || bdev_mount_readonly(d);
    o->gone = status != NULL;
}

int guisys_disks(void *ctx, struct gui_disk *out, int max) {
    struct vfs_mount_view *v = (struct vfs_mount_view *)kalloc(VIEWS * sizeof(*v));
    struct walked w;
    struct pick gone[GUI_DISK_MAX];
    struct bdev *dev[GUI_DISK_MAX];
    struct statfs st;
    u_long flags;
    int nv, ngone, n = 0, i, j;
    (void)ctx;
    if(!v) return -ENOMEM;
    if(max > GUI_DISK_MAX) max = GUI_DISK_MAX;
    nv = vfs_mount_views(v, VIEWS);
    if(nv > VIEWS) nv = VIEWS;
    w.n = 0;
    flags = irq_save();                                 /* the list as it is now, whole */
    bdev_walk(walk_one, &w);
    irq_restore(flags);
    ngone = unplugged(gone, max, &w, v, nv);
    for(i = 0; i < w.n && n < max - ngone; i++) {       /* room is kept for what only Eject can end */
        row(&out[n], w.dev[i], NULL);
        dev[n++] = w.dev[i];
    }
    for(i = 0; i < ngone && n < max; i++) {
        row(&out[n], gone[i].dev, gone[i].status);
        dev[n++] = gone[i].dev;
    }
    for(i = 0; i < n; i++) {                            /* where each mounted one is, and how full */
        for(j = 0; j < nv && v[j].dev != dev[i]; j++) ;
        if(j == nv) continue;
        out[i].mounted = true;
        out[i].readonly = v[j].readonly;
        strlcpy(out[i].mount, v[j].path, sizeof(out[i].mount));
        strlcpy(out[i].fs, v[j].type, sizeof(out[i].fs));
        if(dev[i]->gone) continue;
        out[i].statfs_err = vfs_statfs(v[j].path, &st);
        if(out[i].statfs_err || !st.units) continue;
        out[i].used_kb = (u32)((u64)(st.units - st.free_units) * st.unit >> 10);
        out[i].free_kb = (u32)((u64)st.free_units * st.unit >> 10);
        strlcpy(out[i].fs, st.fs, sizeof(out[i].fs));
        strlcpy(out[i].volume, st.label, sizeof(out[i].volume));
    }
    kfree(v);
    return n;
}
