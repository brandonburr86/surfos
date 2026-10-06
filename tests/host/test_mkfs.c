/* Host-side tests for fs/fat_mkfs.c: FAT16 and FAT32 file systems written into image files
   through a struct bdev backed by a file. Every boot sector field, FSInfo, the backups, the
   first FAT entries, the root directory and the 1 MiB alignment are checked here; then the
   images go to dosfstools and mtools when they are installed (the Docker toolchain has
   them): fsck.fat -n -v must find nothing to fix, minfo and mdir must read them. The images
   (16 MB, 64 MB forced FAT32, 600 MB, 1 GB with a partition, 8 GB and 40 GB, the large ones
   sparse) live in a temporary directory ($TMPDIR or /tmp) that is removed at the end, unless
   SURFOS_KEEP_IMAGES is set. Built like the kernel objects and run by `make unittest`. */
#include <stdio.h>
#include <string.h>
#include <surfos/types.h>
#include <sys/bdev.h>
#include <fs/vfs.h>
#include <fs/fat.h>
#include <time.h>

/* the host's C library, declared by hand: the SurfOS headers stand in for its own */
void exit(int status) __attribute__((noreturn));
void *malloc(size_t n);
void free(void *p);
int system(const char *cmd);
char *mkdtemp(char *tmpl);
char *getenv(const char *name);
int open64(const char *path, int flags, ...);
int close(int fd);
int unlink(const char *path);
int rmdir(const char *path);
int ftruncate64(int fd, long long length);
int pread64(int fd, void *buf, size_t n, long long off);
int pwrite64(int fd, const void *buf, size_t n, long long off);
#define HOST_O_RDWR  02
#define HOST_O_CREAT 0100
#define HOST_O_TRUNC 01000

static int failures, checks;

static void check(int ok, const char *what) {
    checks++;
    if(!ok) { failures++; printf("FAIL: %s\n", what); }
}

/**** what fat_mkfs.c needs from the kernel ****/

static u32 writes, written, first_lba, last_lba, flushes, invalidated, outside;
static u32 allowed_end, tail_start;              /* writes must stay below allowed_end or in the last 33 sectors */
static int fail_at = -1;                         /* fail the write that touches this sector */

void *kalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }
void binvalidate(struct bdev *d) { invalidated++; }
void get_time(time_t *t) {
    memset(t, 0, sizeof(*t));
    t->year = 2026; t->month = 10; t->date = 5; t->hour = 12; t->minute = 34; t->second = 56;
}
struct bdev *bdev_disk(struct bdev *d) { while(d && d->parent) d = d->parent; return d; }
int bdev_may_format(struct bdev *d, char *why, size_t n) {
    if(d->protect) { snprintf(why, n, "%s", d->protect); return -1; }
    return 0;
}

static int disk_io(struct bdev *d, u32 lba, u32 count, void *buf, int write) {
    struct bdev *disk = d;
    u32 rel = lba;
    long long off;
    if(lba + count < lba || lba + count > d->nblocks) return BDEV_ERANGE;
    while(disk->parent) { lba += disk->start; disk = disk->parent; }
    off = (long long)lba * 512;
    if(!write) return pread64((int)(long)disk->priv, buf, count * 512, off) == (int)(count * 512) ? 0 : BDEV_EIO;
    if(fail_at >= 0 && (u32)fail_at >= rel && (u32)fail_at < rel + count) return BDEV_EIO;
    if(!writes) first_lba = rel;
    last_lba = rel;
    writes++;
    written += count;
    if(rel + count > allowed_end && rel < tail_start) outside++;
    return pwrite64((int)(long)disk->priv, buf, count * 512, off) == (int)(count * 512) ? 0 : BDEV_EIO;
}
int bdev_read(struct bdev *d, u32 lba, u32 count, void *buf) { return disk_io(d, lba, count, buf, 0); }
int bdev_write(struct bdev *d, u32 lba, u32 count, const void *buf) { return disk_io(d, lba, count, (void *)buf, 1); }
int bdev_flush(struct bdev *d) { flushes++; return 0; }

/**** images ****/

static char dir[64];
static char made[64][160];                       /* every file made, removed at the end */
static int nmade, have_tools;

struct image {
    char path[160];
    int fd;
    struct bdev disk, part;
};

static const char *track(const char *path) {
    if(nmade < 64) strlcpy(made[nmade++], path, sizeof(made[0]));
    return path;
}

static int image(struct image *im, const char *name, u32 mb) {
    memset(im, 0, sizeof(*im));
    snprintf(im->path, sizeof(im->path), "%s/%s", dir, name);
    im->fd = open64(track(im->path), HOST_O_RDWR | HOST_O_CREAT | HOST_O_TRUNC, 0644);
    if(im->fd < 0 || ftruncate64(im->fd, (long long)mb << 20) != 0) { printf("cannot make %s\n", im->path); return -1; }
    strlcpy(im->disk.name, "usb9", sizeof(im->disk.name));
    im->disk.block_size = 512;
    im->disk.nblocks = (u32)(((unsigned long long)mb << 20) / 512);
    im->disk.bus = BDEV_BUS_USB;
    im->disk.priv = (void *)(long)im->fd;
    return 0;
}

/* partition 1 of the image's disk: an MBR entry and a struct bdev for it */
static struct bdev *partition(struct image *im, u32 start, u8 type) {
    u8 mbr[512];
    u32 count = im->disk.nblocks - start;
    memset(mbr, 0, sizeof(mbr));
    mbr[446 + 4] = type;
    mbr[446 + 8] = start & 0xFF; mbr[446 + 9] = (start >> 8) & 0xFF; mbr[446 + 10] = (start >> 16) & 0xFF; mbr[446 + 11] = start >> 24;
    mbr[446 + 12] = count & 0xFF; mbr[446 + 13] = (count >> 8) & 0xFF; mbr[446 + 14] = (count >> 16) & 0xFF; mbr[446 + 15] = count >> 24;
    mbr[510] = 0x55; mbr[511] = 0xAA;
    pwrite64(im->fd, mbr, 512, 0);
    im->part = im->disk;
    snprintf(im->part.name, sizeof(im->part.name), "usb9p1");
    im->part.parent = &im->disk;
    im->part.start = start;
    im->part.nblocks = count;
    im->part.part_index = 1;
    return &im->part;
}

static u16 le16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 le32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

static void sector(struct bdev *d, u32 lba, u8 *buf) {
    if(bdev_read(d, lba, 1, buf) != 0) { memset(buf, 0xEE, 512); printf("read of sector %u failed\n", (u_int)lba); }
}

/* run a shell command with its output in log; its exit status */
static int sh(const char *log, const char *cmd) {
    char full[700];
    int st;
    snprintf(full, sizeof(full), "%s > '%s' 2>&1", cmd, track(log));
    st = system(full);
    return st == -1 ? -1 : (st >> 8) & 0xFF;
}

static void show(const char *log) {
    char cmd[300];
    snprintf(cmd, sizeof(cmd), "sed 's/^/    /' '%s' | head -40", log);
    system(cmd);
}

/* the dosfstools and mtools verdicts on a volume: the image itself, or a sparse copy of
   its partition for fsck.fat, which takes no offset */
static void tools(const char *what, struct image *im, struct bdev *d, const char *label) {
    char cmd[600], log[200], target[200], msg[200];
    u32 start = d->parent ? d->start : 0;
    int st;
    if(!have_tools) return;
    strlcpy(target, im->path, sizeof(target));
    if(start) {
        snprintf(target, sizeof(target), "%s.part", im->path);
        snprintf(cmd, sizeof(cmd), "dd if='%s' of='%s' bs=512 skip=%u count=%u conv=sparse status=none", im->path, track(target),
                 (u_int)start, (u_int)d->nblocks);
        snprintf(log, sizeof(log), "%s.dd", im->path);
        sh(log, cmd);
    }
    snprintf(log, sizeof(log), "%s.fsck", im->path);
    snprintf(cmd, sizeof(cmd), "fsck.fat -n -v '%s'", target);
    st = sh(log, cmd);
    snprintf(msg, sizeof(msg), "%s: fsck.fat -n -v finds nothing to fix", what);
    check(st == 0, msg);
    if(st) show(log);
    snprintf(log, sizeof(log), "%s.minfo", im->path);
    snprintf(cmd, sizeof(cmd), "minfo -i '%s@@%llu' ::", im->path, (unsigned long long)start * 512);
    st = sh(log, cmd);
    snprintf(msg, sizeof(msg), "%s: minfo reads the boot sector", what);
    check(st == 0, msg);
    if(st) show(log);
    snprintf(log, sizeof(log), "%s.mdir", im->path);
    snprintf(cmd, sizeof(cmd), "mdir -i '%s@@%llu' ::", im->path, (unsigned long long)start * 512);
    st = sh(log, cmd);
    if(st == 0 && label) {
        snprintf(cmd, sizeof(cmd), "grep -q 'Volume in drive : is %s' '%s'", label, log);
        st = system(cmd);
    }
    snprintf(msg, sizeof(msg), "%s: mdir lists the root%s%s", what, label ? " and the label " : "", label ? label : "");
    check(st == 0, msg);
    if(st) show(log);
}

/**** the checks ****/

/* everything mkfs promises about a fresh volume on d */
static void verify(const char *what, struct bdev *d, int bits, u32 spc, const char *label) {
    u8 bs[512], s[512], f[512];
    u32 rsv, spf, data, clusters, root, i, nroot;
    const u8 *ext;
    char msg[200], lab[12];
    int ok;

    sector(d, 0, bs);
    rsv = le16(bs + 14);
    spf = le16(bs + 22) ? le16(bs + 22) : le32(bs + 36);
    data = rsv + 2 * spf + (le16(bs + 17) * 32 + 511) / 512;
    clusters = (d->nblocks - data) / (bs[13] ? bs[13] : 1);
    ext = bs + (bits == 32 ? 64 : 36);
    memset(lab, ' ', 11);
    lab[11] = 0;
    memcpy(lab, label ? label : "NO NAME", strlen(label ? label : "NO NAME"));

    ok = bs[0] == 0xEB && bs[1] == (bits == 32 ? 0x58 : 0x3C) && bs[2] == 0x90 && !memcmp(bs + 3, "MSWIN4.1", 8) &&
         le16(bs + 11) == 512 && bs[13] == spc && bs[16] == 2 && bs[21] == 0xF8 && le32(bs + 28) == (d->parent ? d->start : 0) &&
         bs[510] == 0x55 && bs[511] == 0xAA;
    snprintf(msg, sizeof(msg), "%s: jump, OEM name, 512-byte sectors, %u-sector clusters, 2 FATs, media F8, hidden sectors", what, (u_int)spc);
    check(ok, msg);
    if(bits == 32)
        ok = le16(bs + 17) == 0 && le16(bs + 19) == 0 && le16(bs + 22) == 0 && le32(bs + 32) == d->nblocks && le16(bs + 40) == 0 &&
             le16(bs + 42) == 0 && le32(bs + 44) == 2 && le16(bs + 48) == 1 && le16(bs + 50) == 6 && rsv >= 32 &&
             (data + (d->parent ? d->start : 0)) % 2048 == 0;
    else
        ok = rsv == 1 && le16(bs + 17) == 512 && (d->nblocks < 65536 ? le16(bs + 19) == d->nblocks && le32(bs + 32) == 0
                                                                    : le16(bs + 19) == 0 && le32(bs + 32) == d->nblocks);
    snprintf(msg, sizeof(msg), bits == 32 ? "%s: FAT32 fields, root at cluster 2, FSInfo 1, backup 6, data on a 1 MiB boundary"
                                          : "%s: FAT16 fields, 1 reserved sector, 512 root entries, total sectors", what);
    check(ok, msg);
    ok = ext[0] == 0x80 && ext[2] == 0x29 && le32(ext + 3) != 0 && !memcmp(ext + 7, lab, 11) &&
         !memcmp(ext + 18, bits == 32 ? "FAT32   " : "FAT16   ", 8);
    snprintf(msg, sizeof(msg), "%s: drive 80, extended signature 29, serial, label '%s', type string", what, lab);
    check(ok, msg);
    ok = (bits == 32 ? clusters >= 65525 + 16 && clusters <= 0x0FFFFFF5 && spf * 128 >= clusters + 2
                     : clusters >= 4085 + 16 && clusters <= 65525 - 16 && spf * 256 >= clusters + 2);
    snprintf(msg, sizeof(msg), "%s: %u clusters are FAT%i by Microsoft's count, and the FAT holds them", what, (u_int)clusters, bits);
    check(ok, msg);
    for(i = 446, ok = 1; i < 510; i++) if(bs[i]) ok = 0;
    snprintf(msg, sizeof(msg), "%s: the boot code leaves the partition table area empty", what);
    check(ok && bs[bits == 32 ? 90 : 62] == 0x31, msg);

    for(i = 0, ok = 1; i < 2; i++) {
        sector(d, rsv + i * spf, f);
        if(bits == 32) ok &= le32(f) == 0x0FFFFFF8 && le32(f + 4) == 0x0FFFFFFF && le32(f + 8) == 0x0FFFFFFF && le32(f + 12) == 0;
        else ok &= le16(f) == 0xFFF8 && le16(f + 2) == 0xFFFF && le16(f + 4) == 0;
    }
    snprintf(msg, sizeof(msg), "%s: both FATs start with the media entry, FAT[1] clean%s", what, bits == 32 ? ", the root's end of chain" : "");
    check(ok, msg);
    if(bits == 32) {
        sector(d, 1, s);
        ok = le32(s) == 0x41615252 && le32(s + 484) == 0x61417272 && le32(s + 508) == 0xAA550000 && le32(s + 488) == clusters - 1 &&
             le32(s + 492) == 3;
        sector(d, 7, f);
        ok = ok && !memcmp(s, f, 512);
        sector(d, 6, f);
        ok = ok && !memcmp(bs, f, 512);
        snprintf(msg, sizeof(msg), "%s: FSInfo (signatures, %u free, next 3) and the backups at 6 and 7", what, (u_int)(clusters - 1));
        check(ok, msg);
    }
    root = rsv + 2 * spf;
    nroot = bits == 32 ? spc : 32;
    for(i = 0, ok = 1; i < nroot; i++) {
        u32 k;
        sector(d, root + i, s);
        for(k = (i == 0 && label) ? 32 : 0; k < 512; k++) if(s[k]) ok = 0;
        if(i == 0 && label) ok &= !memcmp(s, lab, 11) && s[11] == 0x08 && le16(s + 24) == ((46 << 9) | (10 << 5) | 5);
    }
    snprintf(msg, sizeof(msg), "%s: the root directory is empty%s", what, label ? " but for the label entry" : "");
    check(ok, msg);
}

/* format d, check the writes and the volume, then let the tools look at it */
static void format(const char *what, struct image *im, struct bdev *d, int bits_wanted, int bits, u32 spc, const char *label) {
    struct fat_layout l;
    char msg[300], text[400];
    int r;
    writes = written = flushes = invalidated = outside = 0;
    if(fat_mkfs_plan(d, bits_wanted, &l, msg, sizeof(msg)) == 0) {
        allowed_end = l.data + (l.bits == 32 ? l.spc : 0);
        tail_start = d->parent ? d->nblocks : d->nblocks - 33;
    }
    r = fat_mkfs(d, label, bits_wanted, msg, sizeof(msg));
    snprintf(text, sizeof(text), "%s: mkfs succeeds (%s)", what, msg);
    check(r == 0, text);
    if(r) return;
    printf("    %s: %s; %u writes, %u sectors\n", what, msg, (u_int)writes, (u_int)written);
    snprintf(text, sizeof(text), "%s: FAT%i as planned", what, bits);
    check(l.bits == bits && l.spc == spc, text);
    snprintf(text, sizeof(text), "%s: the old boot sector is zeroed first, the new one written last, then a flush", what);
    check(first_lba == 0 && last_lba == 0 && flushes == 1 && invalidated >= 1, text);
    snprintf(text, sizeof(text), "%s: only the reserved area, the FATs and the root are written, in large requests", what);
    check(outside == 0 && writes < 40 + written / 64, text);
    verify(what, d, bits, spc, label);
    tools(what, im, d, label);
}

static void refusals(void) {
    struct image im;
    struct fat_layout l;
    char msg[200];
    if(image(&im, "small.img", 16) != 0) return;
    writes = 0;
    im.disk.protect = "the SurfOS boot drive";
    check(fat_mkfs(&im.disk, "X", 0, msg, sizeof(msg)) == -EACCES && !strcmp(msg, "the SurfOS boot drive") && writes == 0,
          "mkfs asks bdev_may_format itself and writes nothing when refused");
    im.disk.protect = NULL;
    check(fat_mkfs(&im.disk, "a/b", 0, msg, sizeof(msg)) == -EINVAL && fat_mkfs(&im.disk, "TWELVE CHARS", 0, msg, sizeof(msg)) == -EINVAL &&
          fat_mkfs(&im.disk, " LEADING", 0, msg, sizeof(msg)) == -EINVAL && writes == 0, "labels with forbidden characters, too long, a leading space");
    check(fat_mkfs(&im.disk, NULL, 32, msg, sizeof(msg)) == -EINVAL && strstr(msg, "too small for FAT32") && writes == 0,
          "FAT32 on 16 MB is refused (too few clusters)");
    check(fat_mkfs(&im.disk, NULL, 12, msg, sizeof(msg)) == -EINVAL, "FAT12 is not offered");
    im.disk.nblocks = 2048;                                     /* 1 MB */
    check(fat_mkfs_plan(&im.disk, 0, &l, msg, sizeof(msg)) == -EINVAL && strstr(msg, "too small"), "1 MB is too small for anything");
    im.disk.nblocks = 8U * 2097152;                             /* 8 GB, never written */
    check(fat_mkfs_plan(&im.disk, 16, &l, msg, sizeof(msg)) == -EINVAL && strstr(msg, "too large for FAT16"), "FAT16 on 8 GB is refused");
    im.disk.nblocks = 511U * 2048;
    check(fat_mkfs_plan(&im.disk, 0, &l, msg, sizeof(msg)) == 0 && l.bits == 16 && l.spc == 16, "511 MB: FAT16 with 8 KB clusters");
    im.disk.nblocks = 512U * 2048;
    check(fat_mkfs_plan(&im.disk, 0, &l, msg, sizeof(msg)) == 0 && l.bits == 32 && l.spc == 8, "512 MB: FAT32 with 4 KB clusters");
    im.disk.nblocks = 16U * 2097152 + 8;
    check(fat_mkfs_plan(&im.disk, 0, &l, msg, sizeof(msg)) == 0 && l.spc == 32, "just over 16 GB: 16 KB clusters");
    im.disk.nblocks = 32768;
    fail_at = 5;
    check(fat_mkfs(&im.disk, NULL, 0, msg, sizeof(msg)) == -EIO && strstr(msg, "write failed at sector") != NULL, "a write error is reported");
    fail_at = -1;
    close(im.fd);
}

int main(void) {
    struct image im;
    struct bdev *p;
    u8 a[512], b[512];
    char msg[200];
    const char *keep = getenv("SURFOS_KEEP_IMAGES");
    int i;

    snprintf(dir, sizeof(dir), "%s/surfos-mkfs-XXXXXX", getenv("TMPDIR") && strlen(getenv("TMPDIR")) < 40 ? getenv("TMPDIR") : "/tmp");
    if(!mkdtemp(dir)) { printf("MKFS UNIT TESTS FAILED: no temporary directory\n"); exit(1); }
    have_tools = system("command -v fsck.fat > /dev/null 2>&1 && command -v minfo > /dev/null 2>&1 && command -v mdir > /dev/null 2>&1") == 0;
    if(!have_tools) printf("    (fsck.fat or mtools missing: the images are checked here only)\n");

    refusals();
    if(image(&im, "fat16-16m.img", 16) == 0) {
        format("16 MB", &im, &im.disk, 0, 16, 4, "TESTVOL");
        sector(&im.disk, 0, a);
        fat_mkfs(&im.disk, "TESTVOL", 0, msg, sizeof(msg));
        sector(&im.disk, 0, b);
        check(memcmp(a + 39, b + 39, 4) != 0, "two formats get different serial numbers");
        close(im.fd);
    }
    if(image(&im, "fat32-64m.img", 64) == 0) { format("64 MB forced FAT32", &im, &im.disk, 32, 32, 1, "SMALL32"); close(im.fd); }
    if(image(&im, "fat32-600m.img", 600) == 0) { format("600 MB", &im, &im.disk, 0, 32, 8, NULL); close(im.fd); }
    if(image(&im, "part-1g.img", 1024) == 0) {
        p = partition(&im, 2048, 0x0C);
        format("1 GB partition at 2048", &im, p, 0, 32, 8, "PART1");
        close(im.fd);
    }
    if(image(&im, "part-63.img", 300) == 0) {
        p = partition(&im, 63, 0x0C);
        format("300 MB partition at 63, forced FAT32", &im, p, 32, 32, 8, "ODD");
        close(im.fd);
    }
    if(image(&im, "fat32-8g.img", 8192) == 0) { format("8 GB", &im, &im.disk, 0, 32, 8, "EIGHT"); close(im.fd); }
    if(image(&im, "fat32-40g.img", 40960) == 0) { format("40 GB", &im, &im.disk, 0, 32, 64, "FORTY"); close(im.fd); }

    if(keep) printf("    images kept in %s\n", dir);
    else {
        for(i = nmade - 1; i >= 0; i--) unlink(made[i]);
        rmdir(dir);
    }
    printf("MKFS UNIT TESTS %s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    exit(failures ? 1 : 0);
}
