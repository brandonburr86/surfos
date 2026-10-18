/*
SurfOS board: SurfCPU on the Arty A7 -- an SD card as a block device, and its card detect
--------------------
File: sdblk.c   Date: 10/16/26 (SurfCPU, milestone 8)
--------------------
The block device over sdcard.c that the Arty (sd.c) and QEMU's sdsim (boards/qemu_virt/
sdsim.c) share: sd0, its partitions (sd0p1, ...) and a task that looks at card detect five
times a second, since surf_spi raises no interrupt for it (MEMORY-MAP.md).

Taken out: the same answer on two looks in a row (so a bouncing switch is not a removal),
and the disk and its partitions are unregistered (driver/bdev.c: every I/O on them fails,
the block cache drops their blocks, a mount on them stays until umount). The console says
so, and for each mount on the card whether it was read-write: what the cache held for it
and had not yet written is lost, and FAT32 keeps no journal, so the volume is left as the
last completed writes left it (surfcpu/docs/CARD.md, "Writing from SurfOS"). Before the
watch has seen it, the driver already refuses every transfer: card detect is the first
thing sdcard.c looks at.

Put in (back, or for the first time): initialized and registered as sd0 again, or as sd1
while a mount of the old sd0 is still there, its partitions read, and not mounted: mount
sd0p1. A transfer that began for the old disk never reaches the new card: a transfer is
for the disk the card has now, or it fails.

A card taken out and put back between two looks (in less than half a second) is not seen:
it lost its power, so it no longer answers in SPI mode, and every transfer fails until it
is taken out again for a moment. That is said in CARD.md too.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/task.h>
#include <surfos/sync.h>
#include <sys/bdev.h>
#include <fs/vfs.h>
#include <mm/kalloc.h>
#include "sdcard.h"
#include "sdproto.h"
#include "sdblk.h"
#include <blibc_common.h>

#define LOOK_MS 200                     /* card detect, five times a second */

/**** the block device ****/

static int bdev_result(int r) {
    if(r == SD_OK) return 0;
    if(r == SD_ERANGE) return BDEV_ERANGE;
    if(r == SD_ENOCARD) return BDEV_EGONE;
    return BDEV_EIO;
}

/* d is the disk of the card in the socket, or the transfer is refused: a card put back in the
   meantime is another disk */
static int sdblk_read(struct bdev *d, u32 lba, u32 count, void *buf) {
    struct sdblk *s = (struct sdblk *)d->priv;
    int r;
    mutex_lock(&s->lock);
    r = d == s->disk ? bdev_result(sd_read(&s->card, lba, count, (u8 *)buf)) : BDEV_EGONE;
    mutex_unlock(&s->lock);
    return r;
}

static int sdblk_write(struct bdev *d, u32 lba, u32 count, const void *buf) {
    struct sdblk *s = (struct sdblk *)d->priv;
    int r;
    mutex_lock(&s->lock);
    r = d == s->disk ? bdev_result(sd_write(&s->card, lba, count, (const u8 *)buf)) : BDEV_EGONE;
    mutex_unlock(&s->lock);
    return r;
}

/* every write already waited for the card's busy time and checked CMD13: SPI mode has no cache to flush */
static int sdblk_flush(struct bdev *d) {
    struct sdblk *s = (struct sdblk *)d->priv;
    return d == s->disk ? 0 : BDEV_EGONE;
}

static const struct bdev_ops sdblk_ops = { sdblk_read, sdblk_write, sdblk_flush };

void sdblk_size(u64 blocks, char *buf, size_t size) {
    u64 mb = blocks / 2048;
    if(mb >= 10240) snprintf(buf, size, "%lu GB", (u_long)(mb / 1024));
    else snprintf(buf, size, "%lu MB", (u_long)mb);
}

/* sd0, or the first sdN that is neither registered nor the disk of a mount that outlived its card */
static void pick_name(char *name, size_t size) {
    u_int n;
    for(n = 0; n < 10; n++) {
        snprintf(name, size, "sd%u", n);
        if(!bdev_find(name) && !vfs_disk_name_mounted(name)) return;
    }
}

/* the card that is in: initialized and registered. 0, 1 when there is no card, < 0 when it failed */
static int attach(struct sdblk *s) {
    struct bdev *d;
    char name[96];
    int r;
    mutex_lock(&s->lock);
    r = sd_init(&s->card, s->host);
    mutex_unlock(&s->lock);
    s->status = r;
    if(r) return r == SD_ENOCARD ? 1 : -1;
    d = (struct bdev *)kcalloc(1, sizeof(struct bdev));
    if(!d) return -1;
    pick_name(d->name, sizeof(d->name));
    if(s->model) strlcpy(d->model, s->model, sizeof(d->model));
    else {
        sd_cid_describe(s->card.cid, name, sizeof(name));
        strlcpy(d->model, name, sizeof(d->model));
    }
    d->block_size = BDEV_BLOCK_SIZE;
    d->nblocks = s->card.blocks > 0xFFFFFFFFull ? 0xFFFFFFFFu : (u32)s->card.blocks;
    d->bus = BDEV_BUS_OTHER;            /* never partitioned or formatted from SurfOS */
    d->removable = true;
    d->readonly = s->readonly;
    d->ops = &sdblk_ops;
    d->priv = s;
    mutex_lock(&s->lock);
    s->disk = d;                        /* before the partitions are read: they read through it */
    mutex_unlock(&s->lock);
    if(bdev_register(d) != 0) {
        mutex_lock(&s->lock);
        s->disk = NULL;
        mutex_unlock(&s->lock);
        kfree(d);
        return -1;
    }
    if(s->announce) s->announce(s);
    bdev_scan_partitions(d);
    return 0;
}

int sdblk_attach(struct sdblk *s) {
    int r;
    mutex_init(&s->lock, "sd");
    r = attach(s);
    s->failed = r < 0;
    if(r) kprintf("+%s: %s (%s)\n", s->tag, sd_strerror(s->status), s->card.why ? s->card.why : "?");
    return r;
}

void sdblk_set_readonly(struct sdblk *s, bool ro) {
    struct bdev *p;
    s->readonly = ro;
    if(!s->disk) return;
    s->disk->readonly = ro;
    for(p = bdev_first(); p; p = p->next) if(p->parent == s->disk) p->readonly = ro;
}

/**** card detect ****/

/* out of the socket: its disk goes, and the console says what was mounted on it */
static void taken_out(struct sdblk *s) {
    struct vfs_mount_view *v;
    struct bdev *d;
    int n, i;
    mutex_lock(&s->lock);
    d = s->disk;
    s->disk = NULL;
    s->card.ready = false;              /* a card put back is initialized again first */
    mutex_unlock(&s->lock);
    s->taken_out++;
    bdev_unregister(d);
    kprintf("+%s: %s: the card was taken out\n", s->tag, d->name);
    v = (struct vfs_mount_view *)kalloc(8 * sizeof(struct vfs_mount_view));
    if(!v) return;
    n = vfs_mount_views(v, 8);
    for(i = 0; i < n && i < 8; i++) {
        if(!v[i].dev || bdev_disk(v[i].dev) != d) continue;
        if(v[i].readonly) kprintf("+%s: %s was mounted read-only: umount %s\n", s->tag, v[i].path, v[i].path);
        else kprintf("+%s: %s was mounted read-write: what was not yet written to the card is lost; umount %s\n",
                     s->tag, v[i].path, v[i].path);
    }
    kfree(v);
}

static void put_in(struct sdblk *s) {
    int r = attach(s);
    s->failed = r != 0;
    if(r) {
        kprintf("+%s: a card was put in, but %s (%s)\n", s->tag, sd_strerror(s->status), s->card.why ? s->card.why : "?");
        return;
    }
    s->put_in++;
    kprintf("+%s: %s: a card was put in: mount %sp1 to use it\n", s->tag, s->disk->name, s->disk->name);
}

static void watch(void *arg) {
    struct sdblk *s = (struct sdblk *)arg;
    int last = -1;
    for(;;) {
        int in;
        sleep_ms(LOOK_MS);
        in = s->host->present(s->host->ctx) ? 1 : 0;
        if(in != last) {                /* changed since the last look: wait for the next one */
            last = in;
            continue;
        }
        if(!in) {
            if(s->disk) taken_out(s);
            s->failed = false;          /* the next card is tried */
        } else if(!s->disk && !s->failed) {
            put_in(s);
        }
    }
}

void sdblk_watch(struct sdblk *s) {
    if(s->watching) return;
    s->watching = kthread_create("sdwatch", watch, s, PL_LOW, TF_DETACHED | TF_SYSTEM) != NULL;   /* (B59) */
}
