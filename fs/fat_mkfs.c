/*
SurfOS FAT Formatter
--------------------
File: fat_mkfs.c  Date: 10/5/26 (USB storage)
--------------------
mkfs: a new, empty FAT16 or FAT32 file system on a block device, laid out
the way Microsoft's FAT specification (fatgen103) and Windows do it, so that
macOS, Windows and Linux take it as one of their own. FAT16 below 512 MB,
FAT32 from 512 MB, or FAT32 on request down to its 65525-cluster minimum.
FAT32's data region starts on a 1 MiB boundary of the disk (flash erase
blocks); its backup boot sector and FSInfo are at sectors 6 and 7. Only the
reserved area, the two FATs and the root directory are written, in large
requests: the data area keeps whatever it held. The old boot sector goes
first and the new one last, so an interrupted mkfs leaves no half volume
that looks whole. The safety rule, bdev_may_format(), is checked here as
well as by the shell. Also compiled into tests/host/test_mkfs.
*/

#include <surfos/types.h>
#include <sys/bdev.h>
#include <fs/vfs.h>
#include <fs/bcache.h>
#include <fs/fat.h>
#include <mm/kalloc.h>
#include <blibc_common.h>

#define SECTOR 512
#define CHUNK 128                   /* sectors per write when zeroing: 64 KB */
#define ALIGN 2048                  /* FAT32's data region starts on a multiple of 1 MiB of the disk */
#define FAT16_ROOT_ENTRIES 512
#define MIN_FAT16 (4085 + 16)       /* cluster counts kept a little away from the type boundaries, */
#define MAX_FAT16 (65525 - 16)      /* where implementations that count differently would disagree */
#define MIN_FAT32 (65525 + 16)
#define MAX_FAT32 0x0FFFFFF5UL
#define FAT32_AUTO (512UL * 2048)   /* sectors: auto picks FAT32 from 512 MB */
#define FSINFO_LEAD   0x41615252
#define FSINFO_STRUCT 0x61417272
#define FSINFO_TRAIL  0xAA550000

static void put16(u8 *p, u32 v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static void put32(u8 *p, u32 v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = v >> 24; }

/* the real-mode code in the boot sector: print the message that follows it, wait for a key,
   and ask the BIOS for the next boot device (int 0x19). The message's address is patched in. */
static const u8 boot_stub[] = {
    0x31, 0xC0,                     /* xor ax, ax */
    0x8E, 0xD8,                     /* mov ds, ax */
    0xBE, 0x00, 0x00,               /* mov si, message */
    0xFC,                           /* cld */
    0xAC,                           /* next: lodsb */
    0x84, 0xC0,                     /* test al, al */
    0x74, 0x09,                     /* jz key */
    0xB4, 0x0E,                     /* mov ah, 0x0e */
    0xBB, 0x07, 0x00,               /* mov bx, 7 */
    0xCD, 0x10,                     /* int 0x10: the character in al */
    0xEB, 0xF2,                     /* jmp next */
    0x32, 0xE4,                     /* key: xor ah, ah */
    0xCD, 0x16,                     /* int 0x16: wait for a key */
    0xCD, 0x19,                     /* int 0x19: the next boot device */
    0xEB, 0xFE                      /* jmp $ */
};
static const char boot_message[] = "\r\nThis is a SurfOS FAT data volume, not a bootable disk.\r\n"
                                   "Press a key to try the next boot device.\r\n";

/* fatgen103's FAT size: never too small, at most a little too large */
static u32 fat_size(u32 total, u32 rsv, u32 root_secs, u32 spc, int bits) {
    u32 t1 = total - (rsv + root_secs), t2 = 256 * spc + 2;
    if(bits == 32) t2 /= 2;
    return t1 / t2 + (t1 % t2 != 0);
}

/* FAT32 with clusters of spc sectors or, for a small volume, the largest smaller size that
   still gives FAT32 its minimum cluster count */
static int plan32(u32 total, u32 hidden, u32 spc, struct fat_layout *l) {
    for(; spc >= 1; spc /= 2) {
        u32 rsv = 32, spf, data, a;
        if(total < rsv + 2 + spc) continue;
        spf = fat_size(total, rsv, 0, spc, 32);
        data = rsv + 2 * spf;
        a = (hidden + data) % ALIGN;
        if(a) data += ALIGN - a;                             /* the reserved area takes up the slack */
        if(data >= total) continue;
        l->clusters = (total - data) / spc;
        if(l->clusters < MIN_FAT32 || l->clusters > MAX_FAT32 || spf * 128 < l->clusters + 2) continue;
        l->bits = 32;
        l->spc = spc;
        l->rsv = data - 2 * spf;
        l->spf = spf;
        l->root_entries = 0;
        l->data = data;
        return 0;
    }
    return -1;
}

/* FAT16 with one reserved sector and a 512-entry root; the cluster size moves from spc
   until the count lies between FAT16's limits */
static int plan16(u32 total, u32 spc, struct fat_layout *l) {
    u32 root_secs = FAT16_ROOT_ENTRIES * 32 / SECTOR, tries;
    for(tries = 0; tries < 8 && spc >= 1 && spc <= 64; tries++) {
        u32 spf = fat_size(total, 1, root_secs, spc, 16), data = 1 + 2 * spf + root_secs;
        u32 clusters = data < total ? (total - data) / spc : 0;
        if(clusters > MAX_FAT16) { spc *= 2; continue; }
        if(clusters < MIN_FAT16) { spc /= 2; continue; }
        if(spf * 256 < clusters + 2) return -1;
        l->bits = 16;
        l->spc = spc;
        l->rsv = 1;
        l->spf = spf;
        l->root_entries = FAT16_ROOT_ENTRIES;
        l->data = data;
        l->clusters = clusters;
        return 0;
    }
    return -1;
}

/* the 11-byte label field: upper case, space padded; "" is NO NAME. -1 for what DOS forbids. */
static int make_label(const char *label, u8 *out) {
    size_t i, len = label ? strlen(label) : 0;
    memset(out, ' ', 11);
    if(!len) { memcpy(out, "NO NAME", 7); return 0; }
    if(len > 11 || label[0] == ' ') return -1;
    for(i = 0; i < len; i++) {
        int c = (u8)label[i];
        if(c >= 'a' && c <= 'z') c -= 32;
        if(c < 0x20 || c > 0x7E || strchr("\"*+,./:;<=>?[\\]|", c)) return -1;
        out[i] = (u8)c;
    }
    return 0;
}

int fat_mkfs_plan(struct bdev *d, const char *label, int fat_bits, struct fat_layout *l, char *msg, size_t n) {
    u32 total = d->nblocks, spc;
    u8 lab[11];
    memset(l, 0, sizeof(*l));
    if(make_label(label, lab) != 0) {
        snprintf(msg, n, "a label has up to 11 characters: letters, digits, spaces and !#$%%&'()-@^_`{}~");
        return -EINVAL;
    }
    l->total = total;
    l->hidden = d->start;
    if(d->block_size != SECTOR) { snprintf(msg, n, "only 512-byte sectors are supported"); return -EINVAL; }
    if(fat_bits != 0 && fat_bits != 16 && fat_bits != 32) { snprintf(msg, n, "the type is fat16 or fat32"); return -EINVAL; }
    if(!fat_bits) fat_bits = total < FAT32_AUTO ? 16 : 32;
    if(fat_bits == 32) {
        /* Windows' cluster sizes: 4 KB up to 8 GB, 8 KB up to 16 GB, 16 KB up to 32 GB, then 32 KB */
        spc = total <= 16777216UL ? 8 : total <= 33554432UL ? 16 : total <= 67108864UL ? 32 : 64;
        if(plan32(total, d->start, spc, l) == 0) return 0;
        snprintf(msg, n, "too small for FAT32 (it needs %u clusters: 33 MB at least)", (u_int)MIN_FAT32);
        return -EINVAL;
    }
    /* fatgen103's table: 1 KB clusters up to 16 MB, 2 KB to 128 MB, 4 KB to 256 MB, 8 KB to
       512 MB, 16 KB to 1 GB, 32 KB to 2 GB */
    spc = total <= 32680 ? 2 : total <= 262144 ? 4 : total <= 524288 ? 8 : total <= 1048576 ? 16 : total <= 2097152 ? 32 : 64;
    if(total <= 4194304UL && plan16(total, spc, l) == 0) return 0;
    snprintf(msg, n, total > 4194304UL ? "too large for FAT16 (2 GB at most): use FAT32" : "too small for FAT16 (about 2 MB at least)");
    return -EINVAL;
}

static void fat_now(u16 *date, u16 *time) {
    time_t t;
    get_time(&t);
    if(t.year < 1980 || t.year > 2107) { *date = (1 << 5) | 1; *time = 0; return; }
    *date = (u16)(((t.year - 1980) << 9) | (t.month << 5) | t.date);
    *time = (u16)((t.hour << 11) | (t.minute << 5) | (t.second / 2));
}

/* DOS's volume serial number from the clock, plus a count so two formats in one second differ */
static u32 volume_serial(void) {
    static u32 made;
    time_t t;
    get_time(&t);
    made++;
    return ((((t.month << 8) | t.date) + (t.second << 8) + made) << 16) | ((((t.hour << 8) | t.minute) + t.year) & 0xFFFF);
}

static void boot_sector(u8 *bs, const struct fat_layout *l, const u8 *label, u32 serial) {
    u32 code = l->bits == 32 ? 90 : 62, msg = code + sizeof(boot_stub);
    u8 *ext = bs + (l->bits == 32 ? 64 : 36);
    memset(bs, 0, SECTOR);
    bs[0] = 0xEB; bs[1] = (u8)(code - 2); bs[2] = 0x90;     /* jmp short to the code, nop */
    memcpy(bs + 3, "MSWIN4.1", 8);                          /* the OEM name the specification recommends */
    put16(bs + 11, SECTOR);
    bs[13] = (u8)l->spc;
    put16(bs + 14, l->rsv);
    bs[16] = 2;                                             /* FATs */
    put16(bs + 17, l->root_entries);
    if(l->bits == 16 && l->total < 65536) put16(bs + 19, l->total);
    else put32(bs + 32, l->total);
    bs[21] = 0xF8;                                          /* media: a fixed disk */
    if(l->bits == 16) put16(bs + 22, l->spf);
    put16(bs + 24, 63);                                     /* sectors per track and heads: only for CHS */
    put16(bs + 26, 255);
    put32(bs + 28, l->hidden);
    if(l->bits == 32) {
        put32(bs + 36, l->spf);
        put16(bs + 40, 0);                                  /* the FATs are mirrored */
        put16(bs + 42, 0);                                  /* version 0.0 */
        put32(bs + 44, 2);                                  /* the root directory's cluster */
        put16(bs + 48, 1);                                  /* FSInfo */
        put16(bs + 50, 6);                                  /* the backup boot sector */
    }
    ext[0] = 0x80;                                          /* drive number */
    ext[2] = 0x29;                                          /* extended boot signature: serial, label, type follow */
    put32(ext + 3, serial);
    memcpy(ext + 7, label, 11);
    memcpy(ext + 18, l->bits == 32 ? "FAT32   " : "FAT16   ", 8);
    memcpy(bs + code, boot_stub, sizeof(boot_stub));
    put16(bs + code + 5, 0x7C00 + msg);
    memcpy(bs + msg, boot_message, sizeof(boot_message));
    bs[510] = 0x55; bs[511] = 0xAA;
}

/* the writes of one mkfs: the first failure is remembered and stops the rest */
struct job {
    struct bdev *d;
    const u8 *zeros;                /* CHUNK sectors of zeros */
    bool failed;
    u32 at;                         /* the sector that could not be written */
};

/* count sectors at lba: buf's (one sector), or zeros when buf is NULL */
static void put(struct job *j, u32 lba, u32 count, const u8 *buf) {
    while(count && !j->failed) {
        u32 k = buf ? 1 : (count < CHUNK ? count : CHUNK);
        if(bdev_write(j->d, lba, k, buf ? buf : j->zeros) != 0) { j->failed = true; j->at = lba; }
        lba += k;
        count -= k;
    }
}

int fat_mkfs(struct bdev *d, const char *label, int fat_bits, char *msg, size_t n) {
    struct fat_layout l;
    struct job j;
    u8 lab[11], *zeros, *sec;
    u32 serial, i, root, root_secs;
    u16 date, time;
    char why[96], name[12];
    int r;

    if(bdev_may_format(d, why, sizeof(why)) != 0) { snprintf(msg, n, "%s", why); return -EACCES; }
    r = fat_mkfs_plan(d, label, fat_bits, &l, msg, n);
    if(r) return r;
    make_label(label, lab);
    zeros = (u8 *)kalloc(CHUNK * SECTOR);
    sec = (u8 *)kalloc(SECTOR);
    if(!zeros || !sec) { kfree(zeros); kfree(sec); snprintf(msg, n, "out of memory"); return -ENOMEM; }
    memset(zeros, 0, CHUNK * SECTOR);
    j.d = d;
    j.zeros = zeros;
    j.failed = false;
    j.at = 0;
    serial = volume_serial();
    fat_now(&date, &time);
    root = l.rsv + 2 * l.spf;                               /* FAT16's root directory, or FAT32's cluster 2 */
    root_secs = l.bits == 32 ? l.spc : FAT16_ROOT_ENTRIES * 32 / SECTOR;

    /* the old boot sector goes first: from here on nothing takes the device for the old volume */
    put(&j, 0, l.rsv, NULL);
    /* both FATs: all free but for the media entry, FAT[1] (clean) and the root's chain */
    for(i = 0; i < 2; i++) {
        put(&j, l.rsv + i * l.spf, l.spf, NULL);
        memset(sec, 0, SECTOR);
        if(l.bits == 32) {
            put32(sec, 0x0FFFFFF8);
            put32(sec + 4, 0x0FFFFFFF);                     /* bit 27 set: cleanly unmounted */
            put32(sec + 8, 0x0FFFFFFF);                     /* cluster 2, the root directory, ends its chain */
        } else {
            put16(sec, 0xFFF8);
            put16(sec + 2, 0xFFFF);                         /* bit 15 set: cleanly unmounted */
        }
        put(&j, l.rsv + i * l.spf, 1, sec);
    }
    /* the root directory, the volume label its first entry */
    put(&j, root, root_secs, NULL);
    if(label && label[0]) {
        memset(sec, 0, SECTOR);
        memcpy(sec, lab, 11);
        sec[11] = 0x08;                                     /* the volume label attribute */
        put16(sec + 22, time);
        put16(sec + 24, date);
        put(&j, root, 1, sec);
    }
    /* a whole disk that held a GPT keeps its backup in the last 33 sectors: that goes too */
    if(!d->parent && l.total > l.data + 33) put(&j, l.total - 33, 33, NULL);
    if(l.bits == 32) {                                      /* FSInfo, then the backups at 6 and 7 */
        memset(sec, 0, SECTOR);
        put32(sec, FSINFO_LEAD);
        put32(sec + 484, FSINFO_STRUCT);
        put32(sec + 488, l.clusters - 1);                   /* every cluster but the root's is free */
        put32(sec + 492, 3);                                /* where to look for a free one */
        put32(sec + 508, FSINFO_TRAIL);
        put(&j, 1, 1, sec);
        put(&j, 7, 1, sec);
        boot_sector(sec, &l, lab, serial);
        put(&j, 6, 1, sec);
    }
    boot_sector(sec, &l, lab, serial);                      /* and the boot sector last */
    put(&j, 0, 1, sec);
    r = j.failed ? -EIO : bdev_flush(d) != 0 ? -EIO : 0;
    kfree(zeros);
    kfree(sec);
    binvalidate(d);                                         /* nothing cached from before may be served */
    if(bdev_disk(d) != d) binvalidate(bdev_disk(d));
    if(j.failed) { snprintf(msg, n, "write failed at sector %lu: the device holds no usable file system now", (u_long)j.at); return r; }
    if(r) { snprintf(msg, n, "the device did not confirm that its write cache reached the medium"); return r; }
    memcpy(name, lab, 11);
    for(i = 11; i && name[i - 1] == ' '; i--);
    name[i] = 0;
    snprintf(msg, n, "FAT%i, %lu-byte clusters: %lu clusters, 2 FATs of %lu sectors, %lu reserved sectors, data at sector %lu%s, "
             "serial %04lX-%04lX, label %s", l.bits, (u_long)l.spc * SECTOR, (u_long)l.clusters, (u_long)l.spf, (u_long)l.rsv,
             (u_long)l.data, l.bits == 32 ? " (1 MiB aligned)" : "", (u_long)(serial >> 16), (u_long)(serial & 0xFFFF), name);
    return 0;
}
