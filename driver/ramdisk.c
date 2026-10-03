/*
SurfOS RAM Disk
--------------------
File: ramdisk.c Date: 10/3/26 (roadmap D2)
--------------------
Every Multiboot module (qemu -initrd, GRUB "module") becomes a writable block
device rd0, rd1, ... over the memory the boot loader put it in. The physical
memory manager keeps those frames out of the free pool (see mm/pmm.c).
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/multiboot.h>
#include <sys/bdev.h>
#include <mm/memory.h>
#include <mm/paging.h>
#include <mm/kalloc.h>
#include <blibc_common.h>

struct ramdisk {
    u8 *base;
    u_long size;
};

static int rd_read(struct bdev *d, u32 lba, u32 count, void *buf) {
    struct ramdisk *rd = (struct ramdisk *)d->priv;
    memcpy(buf, rd->base + (u_long)lba * BDEV_BLOCK_SIZE, (u_long)count * BDEV_BLOCK_SIZE);
    return 0;
}

static int rd_write(struct bdev *d, u32 lba, u32 count, const void *buf) {
    struct ramdisk *rd = (struct ramdisk *)d->priv;
    memcpy(rd->base + (u_long)lba * BDEV_BLOCK_SIZE, buf, (u_long)count * BDEV_BLOCK_SIZE);
    return 0;
}

static const struct bdev_ops rd_ops = { rd_read, rd_write };

/* "/boot/initrd.tar" -> "initrd.tar" */
static const char *basename_of(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

int init_ramdisk(void) {
    u_int i, n = 0;

    for(i = 0; i < bootinfo.nmods; i++) {
        u_long start = bootinfo.mods[i].start, end = bootinfo.mods[i].end;
        struct ramdisk *rd;
        struct bdev *d;

        if(end <= start || end - start < BDEV_BLOCK_SIZE) continue;
        rd = (struct ramdisk *)kcalloc(1, sizeof(struct ramdisk));
        d = (struct bdev *)kcalloc(1, sizeof(struct bdev));
        if(!rd || !d) { kfree(rd); kfree(d); break; }
        rd->size = end - start;
        if(end <= IDENTITY_END) rd->base = (u8 *)start;          /* already mapped 1:1 */
        else rd->base = (u8 *)ioremap(start, rd->size, true);    /* a module GRUB placed higher up */
        if(!rd->base) { kfree(rd); kfree(d); continue; }

        snprintf(d->name, sizeof(d->name), "rd%u", n);
        strlcpy(d->model, bootinfo.mods[i].name[0] ? basename_of(bootinfo.mods[i].name) : "module", sizeof(d->model));
        d->block_size = BDEV_BLOCK_SIZE;
        d->nblocks = rd->size / BDEV_BLOCK_SIZE;                  /* a partial last block is not addressable */
        d->ops = &rd_ops;
        d->priv = rd;
        if(bdev_register(d) != 0) { kfree(rd); kfree(d); continue; }
        kprintf("+RAMDISK: %s is module %u (%s) at 0x%08lx, %lu blocks\n", d->name, i, d->model, start, (u_long)d->nblocks);
        bdev_scan_partitions(d);
        n++;
    }
    return n ? 0 : 1;
}
