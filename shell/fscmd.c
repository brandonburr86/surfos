/*
SurfOS ring0 Shell - file system commands
(C)2004 Brandon Burr, written 10/5/26 (USB storage)

mkfs, mount, umount, eject, df and mv, and fsbench to time a volume; mount
and umount moved here from main.c. mkfs is the one destructive command: it
obeys bdev_may_format() (only USB drives and ramdisks, nothing mounted, never
the SurfOS boot drive), shows what is on the device, and goes ahead only when
the user types the device's name. A formatted partition gets the type that
matches (MBR 0c or 0e, GPT basic data) through the partition code's checked
rewrite; a formatted whole disk is read again, so its old partitions go.
eject unmounts everything on a disk and flushes it before it says the disk
may be pulled out. A protected device (the SurfOS boot drive) is only ever
mounted read-only.
*/

#include <blibc_common.h>
#include <surfos/types.h>
#include <surfos/task.h>
#include <surfos/ktimer.h>
#include <surfos/math64.h>
#include <sys/bdev.h>
#include <sys/part.h>
#include <fs/vfs.h>
#include <fs/bcache.h>
#include <fs/fat.h>
#include <mm/kalloc.h>

#include "shell.h"

/* a size for people, from a count of 512-byte blocks: 64 MB, 1.9 GB (powers of 1024) */
static void human(u64 blocks, char *out, size_t n) {
    u64 tenths, whole;
    u32 tenth;
    const char *unit;
    if(blocks >= 2097152) { tenths = (blocks * 10) >> 21; unit = "GB"; }
    else if(blocks >= 2048) { tenths = (blocks * 10) >> 11; unit = "MB"; }
    else { tenths = blocks * 5; unit = "KB"; }
    whole = div64_32(tenths, 10, &tenth);                    /* no libgcc: no u64 division in C */
    if(!tenth || whole >= 100) snprintf(out, n, "%lu %s", (u_long)whole, unit);
    else snprintf(out, n, "%lu.%lu %s", (u_long)whole, (u_long)tenth, unit);
}

/* the mount a device is on (or the mount at a path), or NULL */
static const struct mount *mount_of(const char *name) {
    const struct mount *m;
    for(m = vfs_mounts(); m; m = m->next) {
        if(!strcmp(m->path, name) || (m->sb->dev && !strcmp(m->sb->dev->name, name))) return m;
    }
    return NULL;
}

/* one line about a mounted volume: "rd1 on /scratch: fat16, label TESTVOL, 64 MB, 63.9 MB free, read-write" */
static void describe_mount(const char *path) {
    struct statfs st;
    char size[16], free[16];
    if(vfs_statfs(path, &st) != 0) return;
    human((u64)st.units * st.unit >> 9, size, sizeof(size));
    human((u64)st.free_units * st.unit >> 9, free, sizeof(free));
    printf("    %s on %s: %s%s%s, %s, %s free, %s\n", st.dev, st.path, st.fs, st.label[0] ? ", label " : "", st.label, size, free,
           (st.flags & MNT_RDONLY) ? "read-only" : "read-write");
}

/**** mkfs ****/

/* the partition type a new FAT volume on d should have, in *k; false when it has it already
   (or d is no partition) */
static bool type_to_set(struct bdev *d, int bits, struct part_kind *k) {
    if(!d->parent || part_parse_type(bits == 32 ? "fat32" : "fat16", k) != 0) return false;
    if(d->scheme == PART_SCHEME_GPT) return d->part_type != 0x07;          /* basic data, FAT's GPT type */
    if(d->scheme != PART_SCHEME_MBR) return false;
    if(bits == 32) return d->part_type != 0x0B && d->part_type != 0x0C;
    return d->part_type != 0x04 && d->part_type != 0x06 && d->part_type != 0x0E;
}

/* rewrite partition d's type through the partition code (checked, read back, the disk read
   again: d itself is replaced by a new device of the same name) */
static void set_type(struct bdev *d, const struct part_kind *k) {
    struct bdev *disk = bdev_disk(d), *now;
    struct part_table *t = (struct part_table *)kalloc(sizeof(struct part_table));
    struct part_plan plan;
    struct part_io io;
    char name[BDEV_NAME_LEN], why[128];
    u_int index = d->part_index;

    strlcpy(name, d->name, sizeof(name));
    strlcpy(why, "out of memory", sizeof(why));
    plan.buf = (u8 *)kalloc(PART_PLAN_BUF);
    if(t && plan.buf) {
        part_io_init(&io, disk, true);
        strlcpy(why, "the partition table cannot be read", sizeof(why));
        if(part_read(&io, disk->nblocks, t) == 0 && part_plan_type(&io, t, index, k, &plan, why, sizeof(why)) == 0 &&
           part_commit(disk, t, &plan, why, sizeof(why)) == 0) {
            now = bdev_find(name);
            printf("    %s: partition type: %s\n", name, now && now->type_name ? now->type_name : "changed");
            kfree(t);
            kfree(plan.buf);
            return;
        }
    }
    printf("    %s: the partition type stays as it was: %s\n", name, why);
    kfree(t);
    kfree(plan.buf);
}

void cmd_mkfs(int argc, char **argv) {
    struct fat_layout l;
    struct part_kind kind;
    struct bdev *d, *p;
    char why[128], size[16], msg[256], line[SHELL_LINE_LEN + 1];
    const char *label, *t;
    int bits = 0, i = 1, parts = 0;
    bool retype;

    if(argc > 2 && !strcmp(argv[1], "-t")) {
        if(!strcmp(argv[2], "fat16")) bits = 16;
        else if(!strcmp(argv[2], "fat32")) bits = 32;
        else i = argc;                                       /* a usage error */
        i += 2;
    }
    if(i >= argc || argc > i + 2) { printf("    usage: mkfs [-t fat16|fat32] <device> [label]   (lsblk lists the devices)\n"); return; }
    d = bdev_find(argv[i]);
    label = argc > i + 1 ? argv[i + 1] : NULL;
    if(!d) { printf("    mkfs: no block device '%s' (lsblk lists them)\n", argv[i]); return; }
    if(bdev_may_format(d, why, sizeof(why)) != 0) { printf("    mkfs: %s: %s\n", d->name, why); return; }
    if(fat_mkfs_plan(d, label, bits, &l, msg, sizeof(msg)) != 0) { printf("    mkfs: %s: %s\n", d->name, msg); return; }

    human(d->nblocks, size, sizeof(size));
    printf("    %s: %s, %s (%lu blocks)%s\n", d->name, d->model[0] ? d->model : "no model name", size, (u_long)d->nblocks,
           d->parent ? ", a partition" : "");
    if(fat_describe(d, msg, sizeof(msg)) == 0) printf("    it holds a %s volume\n", msg);
    else if((t = vfs_probe(d)) != NULL) printf("    it holds a %s file system\n", t);
    else printf("    no file system found on it\n");
    for(p = bdev_first(); p; p = p->next) {                  /* a whole disk: its partition table goes too */
        if(p->parent != d) continue;
        printf("%s%s", parts++ ? " " : "    its partitions, which will be gone: ", p->name);
    }
    if(parts) printf("\n");
    printf("    new: FAT%i, %lu-byte clusters, %lu of them, label %s\n", l.bits, (u_long)l.spc * 512, (u_long)l.clusters,
           label && label[0] ? label : "NO NAME");
    retype = type_to_set(d, l.bits, &kind);
    if(retype) printf("    the partition's type (%s) becomes %s\n", d->type_name ? d->type_name : "unknown",
                      d->scheme == PART_SCHEME_GPT ? "basic data" : l.bits == 32 ? "0c FAT32 LBA" : "0e FAT16 LBA");
    printf("    Type %s to erase %s (%s) and create a FAT%i file system: ", d->name, d->name, size, l.bits);
    memset(line, 0, sizeof(line));
    gets(line);
    if(strcmp(line, d->name)) { printf("    Cancelled\n"); return; }
    if(fat_mkfs(d, label, bits, msg, sizeof(msg)) != 0) { printf("    mkfs: %s: %s\n", d->name, msg); return; }
    printf("    %s: %s\n", d->name, msg);
    if(retype) set_type(d, &kind);
    else if(!d->parent && bdev_rescan(d) >= 0 && parts) printf("    %s was read again: no partitions now\n", d->name);
}

/**** mount, umount, eject ****/

void cmd_mount(int argc, char **argv) {
    const struct mount *m;
    char path[VFS_PATH_MAX];
    struct bdev *d;
    int flags = 0, i = 1, r;

    if(argc == 1) {
        for(m = vfs_mounts(); m; m = m->next)
            printf("    %-6s on %-12s type %s%s%s\n", m->sb->dev ? m->sb->dev->name : "none", m->path, m->sb->type->name,
                   m->sb->readonly ? " (ro)" : "", m->sb->dev && m->sb->dev->gone ? " (unplugged: umount it)" : "");
        return;
    }
    if(!strcmp(argv[1], "-r")) { flags = MNT_RDONLY; i++; }
    if(i >= argc || argc > i + 3) { printf("    usage: mount [-r] <device> [path [type]]   (path: /<device>)\n"); return; }
    d = bdev_find(argv[i]);
    if(!d) { printf("    mount: no block device '%s' (lsblk lists them)\n", argv[i]); return; }
    if(argc > i + 1) strlcpy(path, argv[i + 1], sizeof(path));
    else snprintf(path, sizeof(path), "/%s", d->name);
    if(vfs_bdev_busy(d)) { printf("    mount: %s is mounted already, or its disk or a partition of it is (mount lists them)\n", d->name); return; }
    if(d->protect || bdev_disk(d)->protect) {               /* the SurfOS boot drive is only ever read */
        printf("    %s: %s: mounted read-only\n", d->name, d->protect ? d->protect : bdev_disk(d)->protect);
        flags |= MNT_RDONLY;
    }
    r = vfs_mount_flags(d, path, argc > i + 2 ? argv[i + 2] : NULL, flags);
    if(r == -EINVAL && argc <= i + 2) printf("    mount: %s: no file system found (mkfs makes one)\n", d->name);
    else if(r == -EBUSY) printf("    mount: %s: something is mounted there already\n", path);
    else if(r) printf("    mount: %s: %s\n", d->name, strerror(r));
    else describe_mount(path);
}

/* unmount the mount at path, saying why not */
static int unmount(const char *cmd, const char *path) {
    char why[128];
    int r = vfs_umount(path);
    if(r == -EBUSY && vfs_busy(path, why, sizeof(why)) == -EBUSY) printf("    %s: %s is busy: %s\n", cmd, path, why);
    else if(r) printf("    %s: %s: %s\n", cmd, path, strerror(r));
    return r;
}

void cmd_umount(int argc, char **argv) {
    const struct mount *m;
    char path[VFS_PATH_MAX];
    if(argc != 2) { printf("    usage: umount <path|device>\n"); return; }
    m = argv[1][0] == '/' ? NULL : mount_of(argv[1]);        /* a device name stands for its mount */
    strlcpy(path, m ? m->path : argv[1], sizeof(path));
    unmount("umount", path);
}

/* eject <disk>: every mount of the disk and its partitions goes (all of them, or none when one
   is busy), the cache and the disk's own cache are written out, and its blocks forgotten */
void cmd_eject(int argc, char **argv) {
    const struct mount *m;
    struct bdev *d, *disk = NULL, *p;
    char paths[8][VFS_PATH_MAX], why[128];
    u_int n = 0, i;
    int r = 0;

    if(argc != 2) { printf("    usage: eject <disk>   (usb0, or one of its partitions)\n"); return; }
    d = bdev_find(argv[1]);
    if(d) disk = bdev_disk(d);
    for(m = vfs_mounts(); m; m = m->next) {                  /* an unplugged disk is off the list: its mounts know it */
        struct bdev *x = m->sb->dev;
        if(!x) continue;
        if(!disk && (!strcmp(x->name, argv[1]) || !strcmp(bdev_disk(x)->name, argv[1]))) disk = bdev_disk(x);
        if(disk && bdev_disk(x) == disk && n < 8) strlcpy(paths[n++], m->path, sizeof(paths[0]));
    }
    if(!disk) { printf("    eject: no disk '%s' (lsblk lists them)\n", argv[1]); return; }
    for(i = 0; i < n; i++) {
        if(vfs_busy(paths[i], why, sizeof(why)) == -EBUSY) {
            printf("    eject: %s: %s is busy: %s\n", disk->name, paths[i], why);
            return;
        }
    }
    for(i = 0; i < n && !r; i++) {
        r = unmount("eject", paths[i]);
        if(!r) printf("    %s unmounted\n", paths[i]);
    }
    if(r) return;
    for(p = bdev_first(); p; p = p->next) if(p->parent == disk) { bsync(p); binvalidate(p); }
    bsync(disk);
    binvalidate(disk);
    if(disk->gone) { printf("    %s: unmounted (it was unplugged already)\n", disk->name); return; }
    if(bdev_flush(disk) != 0) printf("    eject: %s: the disk did not confirm that its cache was written (check it elsewhere)\n", disk->name);
    else printf("    %s: safe to remove\n", disk->name);
}

/**** df, mv ****/

void cmd_df(int argc, char **argv) {
    const struct mount *m;
    printf("    Device  Type       Size     Used    Avail  Use%%  Mounted on    Label\n");
    for(m = vfs_mounts(); m; m = m->next) {
        struct statfs st;
        char size[16], used[16], avail[16];
        u32 inuse;
        int r;
        if(!m->sb->dev) continue;                            /* the root and /dev hold no data */
        r = vfs_statfs(m->path, &st);
        if(r) { printf("    %-7s %-7s %s (%s)\n", m->sb->dev->name, m->sb->type->name, m->path, strerror(r)); continue; }
        if(!st.units) { printf("    %-7s %-7s %8s %8s %8s %5s  %-13s %s\n", st.dev, st.fs, "-", "-", "-", "-", st.path, st.label); continue; }
        inuse = st.units - st.free_units;
        human((u64)st.units * st.unit >> 9, size, sizeof(size));
        human((u64)inuse * st.unit >> 9, used, sizeof(used));
        human((u64)st.free_units * st.unit >> 9, avail, sizeof(avail));
        printf("    %-7s %-7s %8s %8s %8s %4lu%%  %-13s %s\n", st.dev, st.fs, size, used, avail,
               (u_long)div64_32((u64)inuse * 100 + st.units - 1, st.units, NULL), st.path, st.label);
    }
}

void cmd_mv(int argc, char **argv) {
    char to[VFS_PATH_MAX];
    struct stat st;
    int r;
    if(argc != 3) { printf("    usage: mv <from> <to>   (into <to> when it is a directory)\n"); return; }
    strlcpy(to, argv[2], sizeof(to));
    if(vfs_stat(to, &st) == 0 && st.type == VN_DIR) {        /* into the directory, under the same name */
        const char *base = strrchr(argv[1], '/');
        base = base ? base + 1 : argv[1];
        if(*base && strlen(to) + 1 + strlen(base) < sizeof(to)) { strlcat(to, "/", sizeof(to)); strlcat(to, base, sizeof(to)); }
    }
    r = vfs_rename(argv[1], to);
    if(r) printf("    mv: %s -> %s: %s\n", argv[1], to, strerror(r));
}

/**** fsbench ****/

static void pattern(u8 *buf, u32 off, u32 len) {
    u32 i;
    for(i = 0; i < len; i += 4) {
        u32 v = (off + i) ^ 0x5A17C0DE;
        buf[i] = (u8)v; buf[i + 1] = (u8)(v >> 8); buf[i + 2] = (u8)(v >> 16); buf[i + 3] = (u8)(v >> 24);
    }
}

/* read the file back in pieces of `piece` bytes and compare; the time in ms, or -1 */
static long bench_read(const char *path, u8 *buf, u8 *want, u32 size, u32 piece) {
    struct file *f;
    u_long t0 = uptime_ms();
    u32 off;
    if(vfs_open(path, O_RDONLY, &f) != 0) return -1;
    for(off = 0; off < size; off += piece) {
        if(vfs_read(f, buf, piece) != (int)piece) { vfs_close(f); return -1; }
        pattern(want, off, piece);
        if(memcmp(buf, want, piece)) { printf("    fsbench: wrong data at offset %lu\n", (u_long)off); vfs_close(f); return -1; }
    }
    vfs_close(f);
    return (long)(uptime_ms() - t0);
}

static void rate(const char *what, u32 kb, long ms) {
    printf("    %-24s %6lu KB in %5ld ms  %6lu KB/s\n", what, (u_long)kb, ms, (u_long)(ms > 0 ? kb * 1000UL / (u_long)ms : 0));
}

/* fsbench <dir> [MB]: time writing a file in 4 KB pieces, reading it in 4 KB and in 64 KB pieces */
void cmd_fsbench(int argc, char **argv) {
    struct bcache_io a, b;
    struct file *f;
    char path[VFS_PATH_MAX];
    u32 mb = argc > 2 ? strtoul(argv[2], NULL, 10) : 4, size, off;
    u8 *buf, *want;
    long tw, tr4, tr64;
    int r = 0;

    if(argc < 2 || argc > 3 || mb < 1 || mb > 64) { printf("    usage: fsbench <directory> [MB]   (1 to 64, default 4)\n"); return; }
    snprintf(path, sizeof(path), "%s/fsbench.tmp", argv[1]);
    size = mb << 20;
    buf = (u8 *)kalloc(65536);
    want = (u8 *)kalloc(65536);
    if(!buf || !want) { printf("    fsbench: out of memory\n"); kfree(buf); kfree(want); return; }
    bcache_io_stats(&a);
    tw = (long)uptime_ms();
    r = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC, &f);
    for(off = 0; r == 0 && off < size; off += 4096) {
        pattern(buf, off, 4096);
        if(vfs_write(f, buf, 4096) != 4096) r = -EIO;
    }
    if(r == 0) r = vfs_close(f);                             /* close writes it out */
    tw = (long)uptime_ms() - tw;
    if(r) { printf("    fsbench: %s: %s\n", path, strerror(r)); kfree(buf); kfree(want); return; }
    tr4 = bench_read(path, buf, want, size, 4096);
    tr64 = bench_read(path, buf, want, size, 65536);
    bcache_io_stats(&b);
    vfs_unlink(path);
    rate("write, 4 KB pieces:", size >> 10, tw);
    if(tr4 >= 0) rate("read, 4 KB pieces:", size >> 10, tr4);
    if(tr64 >= 0) rate("read, 64 KB pieces:", size >> 10, tr64);
    printf("    device requests: %lu reads (%lu blocks), %lu writes (%lu blocks)\n", b.reads - a.reads, b.read_blocks - a.read_blocks,
           b.writes - a.writes, b.written_blocks - a.written_blocks);
    printf("    fsbench: %s\n", tr4 >= 0 && tr64 >= 0 ? "ok" : "FAILED");
    kfree(buf);
    kfree(want);
}
