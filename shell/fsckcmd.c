/*
SurfOS ring0 Shell - fsck
(C)2004 Brandon Burr, written 11/01/26 (the command-line suite, track fsck)

fsck, the checker of fs/fat_check.c on a device. It is read whole and nothing else changes
it meanwhile: a run takes its device with vfs_bdev_change_begin() from before its first
read to after its last write, the question included, so what it shows is what it repairs.
A volume mounted read-only cannot be taken that way and need not be: its mount is held
(vfs_hold_mount()), so that it can neither go nor be swapped under the run. A volume mounted
read-write is refused, with the reason, and so is a mount of its disk or a partition of it,
which nothing here could hold.

-r obeys bdev_may_format(), before the question and again after it: the answer may have
been a long time coming. It is given a write function: a run without -r has none. The plan
is part of the question, because shell_ask() shows that on the console also when the output
goes to a file or q was answered at --More--; the keys typed before it shows are dropped,
so the answer is what was typed after reading it.

In a file of its own so that tests/host/test_fsckcmd.c runs the real command over a device
layer it scripts. The x86 kernel takes every C file of shell/ by itself; SurfCPU's list
(arch/riscv32/kernel.mk) names its files, and has neither this one nor fs/fat_check.c.
*/

#include <blibc_common.h>
#include <surfos/types.h>
#include <surfos/task.h>
#include <surfos/tty.h>
#include <sys/bdev.h>
#include <sys/part.h>
#include <fs/vfs.h>
#include <fs/bcache.h>
#include <fs/fat_check.h>

#include "shell.h"

static int fsck_read(void *d, u32 lba, u32 count, void *buf) { return bdev_read((struct bdev *)d, lba, count, buf); }
static int fsck_write(void *d, u32 lba, u32 count, const void *buf) { return bdev_write((struct bdev *)d, lba, count, buf); }
static int fsck_flush(void *d) { return bdev_flush((struct bdev *)d); }
static bool fsck_stop(void *d) { (void)d; return fs_cut_short(); }
static void fsck_say(void *d, const char *line) { (void)d; printf("    %s\n", line); }

/* the plan, the device's name typed, and the rule once more */
static bool fsck_ask(void *v, const char *plan) {
    struct bdev *d = (struct bdev *)v;
    char line[SHELL_LINE_LEN + 1], question[360], why[128];
    snprintf(question, sizeof(question), "    It will: %s\n    Type %s to repair %s: ", plan, d->name, d->name);
    tty_flush(curTask->tty ? curTask->tty : tty_active);
    shell_ask(question, line, sizeof(line));
    if(strcmp(line, d->name)) { printf("    Cancelled\n"); return false; }
    if(bdev_may_format(d, why, sizeof(why)) != 0) { printf("    fsck: %s: %s\n", d->name, why); return false; }
    return true;
}

int cmd_fsck(int argc, char **argv) {
    struct fat_check_io io;
    struct fat_check_result res;
    struct bdev *d;
    struct vnode *root = NULL;
    const char *name = NULL, *busy;
    char why[128], size[8], path[VFS_PATH_MAX];
    bool readonly = false;
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
            else if(*a == 'r') flags |= FAT_CHECK_REPAIR;
            else { name = NULL; i = argc; break; }
        }
    }
    if(!name) { printf("    usage: fsck [-v] [-r] <device>   (-r repairs, and asks first; lsblk lists the devices)\n"); return CMD_USAGE; }
    d = bdev_find(name);
    if(!d) { printf("    fsck: no block device '%s' (lsblk lists them)\n", name); return CMD_USAGE; }
    memset(&io, 0, sizeof(io));
    io.ctx = d;
    io.blocks = d->nblocks;
    io.read = fsck_read;
    io.flush = fsck_flush;
    io.stop = fsck_stop;
    io.say = fsck_say;
    if(flags & FAT_CHECK_REPAIR) {
        if(bdev_may_format(d, why, sizeof(why)) != 0) { printf("    fsck: %s: %s\n", d->name, why); return CMD_USAGE; }
        io.write = fsck_write;
        io.ask = fsck_ask;
    }
    fs_enter();                                              /* a kill waits until the buffers and the device are let go of */
    if(!(flags & FAT_CHECK_REPAIR) && vfs_hold_mount(d, path, sizeof(path), &root, &readonly) == 0 && !readonly) {
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
    if(res.wrote) {                                          /* nothing cached from before may be served */
        binvalidate(d);
        if(bdev_disk(d) != d) binvalidate(bdev_disk(d));
    }
    if(root) vfs_release_mount(root);
    else vfs_bdev_change_end(d);
    fs_leave();
    return status;
}
