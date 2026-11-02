/*
SurfOS ring0 Shell - fsck
(C)2004 Brandon Burr, written 11/01/26 (the command-line suite, track fsck)

fsck, the checker of fs/fat_check.c on a device. It reads, and never writes. -r is refused:
repairs are not available in this version (11/02/26: two reviews found that they could still
lose data), and a stick is repaired on another computer. Nothing here reaches the repair code
of fs/fat_check.c: the checker is given no write function and never asked to repair, and that
code stays in the tree, tested on the host, for a later round.

The volume is read whole and nothing else changes it meanwhile: an unmounted device is taken
with vfs_bdev_change_begin() from before the first read to after the last. A volume mounted
read-only cannot be taken that way and need not be: its mount is held (vfs_hold_mount()), so
that it can neither go nor be swapped under the run. A volume mounted read-write is refused,
with the reason, and so is a mount of its disk or a partition of it, which nothing here could
hold.

In a file of its own so that tests/host/test_fsckcmd.c runs the real command over a device
layer it scripts. The x86 kernel takes every C file of shell/ by itself; SurfCPU's list
(arch/riscv32/kernel.mk) names its files, and has neither this one nor fs/fat_check.c.
*/

#include <blibc_common.h>
#include <surfos/types.h>
#include <surfos/task.h>
#include <sys/bdev.h>
#include <sys/part.h>
#include <fs/vfs.h>
#include <fs/fat_check.h>

#include "shell.h"

static int fsck_read(void *d, u32 lba, u32 count, void *buf) { return bdev_read((struct bdev *)d, lba, count, buf); }
static bool fsck_stop(void *d) { (void)d; return fs_cut_short(); }
static void fsck_say(void *d, const char *line) { (void)d; printf("    %s\n", line); }

int cmd_fsck(int argc, char **argv) {
    struct fat_check_io io;
    struct fat_check_result res;
    struct bdev *d;
    struct vnode *root = NULL;
    const char *name = NULL, *busy;
    char size[8], path[VFS_PATH_MAX];
    bool readonly = false, repair = false;
    int flags = 0, i, status;

    for(i = 1; i < argc; i++) {
        const char *a = argv[i];
        if(a[0] != '-') {
            if(name) { name = NULL; break; }
            name = a;
            continue;
        }
        while(*++a) {
            if(*a == 'v') flags |= FAT_CHECK_VERBOSE;
            else if(*a == 'r') repair = true;
            else { name = NULL; i = argc; break; }
        }
    }
    if(repair) {
        printf("    fsck: repairs are not available in this version, and the stick can be repaired on another computer\n");
        return CMD_USAGE;
    }
    if(!name) { printf("    usage: fsck [-v] <device>   (it only reads; lsblk lists the devices)\n"); return CMD_USAGE; }
    d = bdev_find(name);
    if(!d) { printf("    fsck: no block device '%s' (lsblk lists them)\n", name); return CMD_USAGE; }
    memset(&io, 0, sizeof(io));
    io.ctx = d;
    io.blocks = d->nblocks;
    io.read = fsck_read;
    io.stop = fsck_stop;
    io.say = fsck_say;
    fs_enter();                                              /* a kill waits until the device and the mount are let go of */
    if(vfs_hold_mount(d, path, sizeof(path), &root, &readonly) != 0) root = NULL;   /* not mounted */
    if(root && !readonly) {
        vfs_release_mount(root);
        fs_leave();
        printf("    fsck: %s is mounted read-write on %s: a volume in use changes under a check (umount it, or mount it with -r)\n", d->name, path);
        return CMD_USAGE;
    }
    if(!root && vfs_bdev_change_begin(d, &busy) != 0) {
        fs_leave();
        printf("    fsck: %s: %s\n", d->name, bdev_busy_words(busy));
        return CMD_USAGE;
    }
    part_fmt_size(d->nblocks, size, sizeof(size));
    printf("    %s: %s, %s\n", d->name, d->model[0] ? d->model : "no model name", size);
    status = fat_check(&io, flags, &res);
    if(root) vfs_release_mount(root);
    else vfs_bdev_change_end(d);
    fs_leave();
    if(status == FAT_CHECK_PROBLEMS) printf("    repairs are not available in this version: the stick can be repaired on another computer\n");
    return status;
}
