/*
SurfOS FAT File System
--------------------
File: fat.c     Date: 10/3/26 (roadmap F1)
--------------------
FAT12, FAT16 and FAT32 with long file names, read and write: lookup, readdir,
read, write (extending the cluster chain), truncate, create (files and
directories, with VFAT long-name entries when the name is not a plain 8.3
name), unlink and rmdir. Every access goes through the block cache; the
superblock mutex serializes operations on one volume. No FSInfo updates,
no extended partition walking.

10/5/26 (USB storage): rename, and one vnode per object in use (lookups of an
open file share its vnode, so its size stays right and it follows its entry
when renamed; the VFS can tell that it is open). Names are UTF-8: long names
are UTF-16 on the disk, lookups fold ASCII letters only, an 8.3 alias puts '_'
for what it cannot hold, and short names without a long name are read as code
page 437. Once the device is unplugged every operation fails with EIO and the
volume can still be unmounted. statfs with the volume label;
a free-cluster count, from FAT32's FSInfo sector when that can be trusted, else
counted, and written back to FSInfo at sync and unmount; the clean-shutdown bit
in FAT[1]: cleared while mounted read-write, set again at unmount, so macOS and
Windows check a volume that was unplugged while mounted. A read-only mount
writes nothing.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/irq.h>
#include <sys/bdev.h>
#include <fs/vfs.h>
#include <fs/bcache.h>
#include <fs/fat.h>
#include <mm/kalloc.h>
#include <blibc_common.h>

#define ATTR_RO      0x01
#define ATTR_HIDDEN  0x02
#define ATTR_SYSTEM  0x04
#define ATTR_LABEL   0x08
#define ATTR_DIR     0x10
#define ATTR_ARCHIVE 0x20
#define ATTR_LFN     0x0F
#define NTRES_LOWER_BASE 0x08
#define NTRES_LOWER_EXT  0x10
#define LFN_CHARS 13
#define LFN_MAX_ENTRIES 20          /* 255 characters */
#define NO_IDX 0xFFFFFFFFUL         /* no directory entry */
#define DIR_MAX_ENTRIES 65536       /* the most a FAT directory may hold: walks stop there */
#define FAT_DIRECT 16               /* whole-block runs this long or longer bypass the cache (8 KB) */
#define FAT_READAHEAD BCACHE_RUN    /* blocks a cache miss reads ahead along a file (64 KB) */
#define META_AHEAD 8                /* and in a FAT or a directory (4 KB) */
#define RW_READ  0
#define RW_WRITE 1
#define RW_ZERO  2                  /* write zeros */
#define FREE_UNKNOWN 0xFFFFFFFFUL   /* also what FSInfo says when it does not know */
#define FSINFO_LEAD   0x41615252
#define FSINFO_STRUCT 0x61417272
#define FSINFO_TRAIL  0xAA550000
#define FAT16_CLEAN 0x8000          /* FAT[1]: set = cleanly unmounted */
#define FAT32_CLEAN 0x08000000

struct fat_node;
struct fat_fs;

/* position inside a directory */
struct diter {
    struct fat_fs *fs;
    u32 first;                      /* 0: the fixed FAT12/16 root */
    u32 idx, clus, lba, off;
    bool at_end;
};

struct fat_fs {
    struct superblock *sb;
    struct bdev *dev;
    int type;                       /* 12, 16 or 32 */
    u32 spc, rsv, nfats, root_entries, spf, total;
    u32 fat_lba, root_lba, root_sectors, data_lba, nclusters, root_cluster;
    u32 cluster_bytes, eoc, next_free;
    struct fat_node *live;          /* the objects that have a vnode now (not the root) */
    u32 free;                       /* free clusters; FREE_UNKNOWN until counted */
    u32 fsinfo;                     /* FAT32: the FSInfo sector, 0 when there is no valid one */
    bool fsinfo_dirty;              /* free or next_free changed since FSInfo was written */
    bool marked;                    /* this read-write mount cleared the clean bit */
    char label[12];                 /* "" for none */
};

struct fat_node {
    struct vnode *v;                /* the one vnode of this object while it is in use */
    struct fat_node *next;          /* fs->live */
    u32 first;                      /* first cluster; 0 = empty file, or the FAT12/16 root */
    u32 dir_lba, dir_off;           /* where the short entry lives (not for the root) */
    bool is_root;
    u32 pos_ci, pos_c;              /* the last cluster used: its index in the chain, its number (0: none) */
    bool rd_valid;                  /* readdir resumes at rd_it for entry rd_index */
    u_int rd_index;
    struct diter rd_it;
};

/* one logical directory entry as the iterator returns it */
struct fat_dirent {
    char name[VFS_NAME_MAX + 1];
    u8 raw[32];                     /* the short entry */
    u32 first, size;
    u32 lba, off;                   /* short entry location */
    u32 idx, lfn_idx;               /* entry indexes: the short entry and the first LFN entry */
};


static const struct vnode_ops fat_vops;

/**** little helpers ****/

static u16 le16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 le32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static void put16(u8 *p, u16 v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void put32(u8 *p, u32 v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = v >> 24; }
static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int upper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

/* names compare with ASCII letters folded; any other byte must match exactly */
static bool name_eq(const char *a, const char *b) {
    while(*a && *b) { if(lower(*a) != lower(*b)) return false; a++; b++; }
    return *a == *b;
}

/**** UTF-8, UTF-16 and code page 437 ****/

/* the upper half of code page 437, what short names without a long name are read as */
static const u16 cp437[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0
};

/* code point cp as UTF-8 at out; the number of bytes (1 to 4) */
static u32 utf8_put(char *out, u32 cp) {
    if(cp < 0x80) { out[0] = (char)cp; return 1; }
    if(cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if(cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* the code point at *s, which moves past it; -1 for malformed UTF-8 (a stray continuation
   byte, a short sequence, an overlong form, a surrogate, beyond U+10FFFF) */
static i32 utf8_next(const char **s) {
    const u8 *p = (const u8 *)*s;
    u32 c = p[0], cp, need, i, min;
    if(c < 0x80) { *s += 1; return (i32)c; }
    if(c >= 0xC2 && c <= 0xDF) { need = 1; cp = c & 0x1F; min = 0x80; }
    else if(c >= 0xE0 && c <= 0xEF) { need = 2; cp = c & 0x0F; min = 0x800; }
    else if(c >= 0xF0 && c <= 0xF4) { need = 3; cp = c & 0x07; min = 0x10000; }
    else return -1;
    for(i = 1; i <= need; i++) {
        if((p[i] & 0xC0) != 0x80) return -1;
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    if(cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return -1;
    *s += need + 1;
    return (i32)cp;
}

/* a UTF-8 name as the UTF-16 units of a long name: -EINVAL for malformed UTF-8,
   -ENAMETOOLONG beyond 255 units */
static int name_to_utf16(const char *name, u16 *out, u32 *count) {
    u32 n = 0;
    while(*name) {
        i32 cp = utf8_next(&name);
        if(cp < 0) return -EINVAL;
        if(n + (cp >= 0x10000 ? 2 : 1) > 255) return -ENAMETOOLONG;
        if(cp >= 0x10000) {                                  /* a surrogate pair */
            out[n++] = (u16)(0xD800 + ((cp - 0x10000) >> 10));
            out[n++] = (u16)(0xDC00 + ((cp - 0x10000) & 0x3FF));
        } else out[n++] = (u16)cp;
    }
    *count = n;
    return 0;
}

/* n UTF-16 units as UTF-8 (a lone surrogate becomes U+FFFD); false when it does not fit size */
static bool utf16_to_utf8(const u16 *in, u32 n, char *out, size_t size) {
    size_t o = 0;
    u32 i;
    char tmp[4];
    for(i = 0; i < n; i++) {
        u32 cp = in[i], len;
        if(cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n && in[i + 1] >= 0xDC00 && in[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (in[i + 1] - 0xDC00);
            i++;
        } else if(cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
        len = utf8_put(tmp, cp);
        if(o + len >= size) return false;
        memcpy(out + o, tmp, len);
        o += len;
    }
    out[o] = 0;
    return true;
}

static u32 clus_lba(struct fat_fs *fs, u32 c) {
    return fs->data_lba + (c - 2) * fs->spc;
}

static bool is_eoc(struct fat_fs *fs, u32 v) {
    if(v < 2) return true;                                   /* free or reserved: a broken chain ends here */
    return fs->type == 12 ? v >= 0xFF7 : fs->type == 16 ? v >= 0xFFF7 : v >= 0x0FFFFFF7;
}

/**** time ****/

static i32 days_from_civil(i32 y, u32 m, u32 d) {
    i32 era, yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (i32)(m + (m > 2 ? -3 : 9)) + 2) / 5 + (i32)d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static u32 fat_to_unix(u16 date, u16 time) {
    u32 y = 1980 + (date >> 9), m = (date >> 5) & 15, d = date & 31;
    if(!m || m > 12 || !d) return 0;
    return (u32)days_from_civil((i32)y, m, d) * 86400 + (time >> 11) * 3600 + ((time >> 5) & 63) * 60 + (time & 31) * 2;
}

static void fat_now(u16 *date, u16 *time) {
    time_t t;
    get_time(&t);
    if(t.year < 1980 || t.year > 2107) { *date = (1 << 5) | 1; *time = 0; return; }
    *date = (u16)(((t.year - 1980) << 9) | (t.month << 5) | t.date);
    *time = (u16)((t.hour << 11) | (t.minute << 5) | (t.second / 2));
}

/**** the file allocation table ****/

static int fat_get(struct fat_fs *fs, u32 c, u32 *val) {
    u32 off, lba, o;
    struct buf *b;
    if(c < 2 || c >= fs->nclusters + 2) return -EIO;
    off = fs->type == 12 ? c + c / 2 : fs->type == 16 ? c * 2 : c * 4;
    lba = fs->fat_lba + off / 512;
    o = off % 512;
    b = breada(fs->dev, lba, fs->fat_lba + fs->spf - lba < META_AHEAD ? fs->fat_lba + fs->spf - lba : META_AHEAD);
    if(!b) return -EIO;
    if(fs->type == 12) {
        u32 v = b->data[o];
        if(o == 511) {
            struct buf *b2 = bread(fs->dev, lba + 1);
            if(!b2) { brelse(b); return -EIO; }
            v |= (u32)b2->data[0] << 8;
            brelse(b2);
        } else v |= (u32)b->data[o + 1] << 8;
        *val = (c & 1) ? v >> 4 : v & 0xFFF;
    } else if(fs->type == 16) *val = le16(b->data + o);
    else *val = le32(b->data + o) & 0x0FFFFFFF;
    brelse(b);
    return 0;
}

static int fat_set(struct fat_fs *fs, u32 c, u32 val) {
    u32 off, o, i;
    if(c < 2 || c >= fs->nclusters + 2) return -EIO;
    off = fs->type == 12 ? c + c / 2 : fs->type == 16 ? c * 2 : c * 4;
    o = off % 512;
    for(i = 0; i < fs->nfats; i++) {
        u32 lba = fs->fat_lba + i * fs->spf + off / 512, end = fs->fat_lba + (i + 1) * fs->spf;
        struct buf *b = breada(fs->dev, lba, end - lba < META_AHEAD ? end - lba : META_AHEAD), *b2 = NULL;
        if(!b) return -EIO;
        if(fs->type == 12) {
            u8 *lo = b->data + o, *hi;
            if(o == 511) {
                b2 = bread(fs->dev, lba + 1);
                if(!b2) { brelse(b); return -EIO; }
                hi = b2->data;
            } else hi = b->data + o + 1;
            if(c & 1) { *lo = (*lo & 0x0F) | ((val << 4) & 0xF0); *hi = (val >> 4) & 0xFF; }
            else { *lo = val & 0xFF; *hi = (*hi & 0xF0) | ((val >> 8) & 0x0F); }
            if(b2) { bdirty(b2); brelse(b2); }
        } else if(fs->type == 16) put16(b->data + o, (u16)val);
        else put32(b->data + o, (le32(b->data + o) & 0xF0000000) | (val & 0x0FFFFFFF));
        bdirty(b);
        brelse(b);
    }
    return 0;
}

static int zero_cluster(struct fat_fs *fs, u32 c) {
    u32 s;
    for(s = 0; s < fs->spc; s++) {
        struct buf *b = bget(fs->dev, clus_lba(fs, c) + s);
        if(!b) return -EIO;
        memset(b->data, 0, 512);
        bdirty(b);
        brelse(b);
    }
    return 0;
}

/* A free cluster, chained after prev (0: a new chain): prev + 1 when that is free, so files
   stay contiguous, else the first free one from the hint on. Only a directory's is zeroed: a
   file's bytes past its end are never read, and a gap is written as zeros (grow_zeros).
   0, -ENOSPC or -EIO. */
static int fat_alloc(struct fat_fs *fs, u32 prev, bool dir, u32 *out) {
    u32 n, c, v;
    int r;
    if(fs->free == 0) return -ENOSPC;                        /* known to be full */
    if(prev >= 2 && prev + 1 < fs->nclusters + 2) {
        r = fat_get(fs, prev + 1, &v);
        if(r) return r;
        if(!v) { c = prev + 1; goto found; }
    }
    for(n = 0, c = fs->next_free; n < fs->nclusters; n++, c++) {
        if(c < 2 || c >= fs->nclusters + 2) c = 2;
        r = fat_get(fs, c, &v);
        if(r) return r;
        if(!v) goto found;
    }
    fs->free = 0;
    return -ENOSPC;
found:
    r = fat_set(fs, c, fs->eoc);
    if(r == 0 && dir) r = zero_cluster(fs, c);
    if(r == 0 && prev) r = fat_set(fs, prev, c);
    if(r) return r;
    fs->next_free = c + 1;
    if(fs->free != FREE_UNKNOWN && fs->free) fs->free--;
    fs->fsinfo_dirty = true;
    *out = c;
    return 0;
}

static int fat_free_chain(struct fat_fs *fs, u32 c) {
    u32 n = 0, next;
    while(!is_eoc(fs, c) && n++ < fs->nclusters) {
        if(fat_get(fs, c, &next) != 0) return -EIO;
        if(fat_set(fs, c, 0) != 0) return -EIO;
        if(c < fs->next_free) fs->next_free = c;
        if(fs->free != FREE_UNKNOWN) fs->free++;
        fs->fsinfo_dirty = true;
        c = next;
    }
    return 0;
}

/* the free clusters, counted in one pass over the first FAT, read 64 KB at a time */
static int count_free(struct fat_fs *fs) {
    u32 c, v, n = 0, first = 0;
    if(fs->type == 12) {                                     /* entries straddle sectors; the FAT is small */
        for(c = 2; c < fs->nclusters + 2; c++) {
            if(fat_get(fs, c, &v) != 0) return -EIO;
            if(!v) { n++; if(!first) first = c; }
        }
    } else {
        u32 per = fs->type == 16 ? 256 : 128, nsec = (fs->nclusters + 2 + per - 1) / per, sec, k, i;
        u8 *chunk = (u8 *)kalloc(BCACHE_RUN * 512);
        if(!chunk) return -ENOMEM;
        for(sec = 0; sec < nsec; sec += k) {
            k = nsec - sec < BCACHE_RUN ? nsec - sec : BCACHE_RUN;
            if(bread_blocks(fs->dev, fs->fat_lba + sec, k, chunk) != 0) { kfree(chunk); return -EIO; }
            for(i = 0; i < k * per; i++) {
                c = sec * per + i;
                if(c < 2) continue;
                if(c >= fs->nclusters + 2) break;
                v = fs->type == 16 ? le16(chunk + i * 2) : le32(chunk + i * 4) & 0x0FFFFFFF;
                if(!v) { n++; if(!first) first = c; }
            }
        }
        kfree(chunk);
    }
    fs->free = n;
    if(first) fs->next_free = first;
    return 0;
}

/* FAT[1]'s clean-shutdown bit, in every copy of the FAT (FAT12 has none) */
static int fat_mark(struct fat_fs *fs, bool clean) {
    u32 i;
    if(fs->type == 12) return 0;
    for(i = 0; i < fs->nfats; i++) {
        struct buf *b = bread(fs->dev, fs->fat_lba + i * fs->spf);
        if(!b) return -EIO;
        if(fs->type == 32) {
            u32 v = le32(b->data + 4);
            put32(b->data + 4, clean ? v | FAT32_CLEAN : v & ~FAT32_CLEAN);
        } else {
            u16 v = le16(b->data + 2);
            put16(b->data + 2, (u16)(clean ? v | FAT16_CLEAN : v & ~FAT16_CLEAN));
        }
        bdirty(b);
        brelse(b);
    }
    return 0;
}

/* the free count and the next-free hint back into FSInfo (FAT32) */
static int write_fsinfo(struct fat_fs *fs) {
    struct buf *b;
    if(!fs->fsinfo || !fs->fsinfo_dirty) return 0;
    b = bread(fs->dev, fs->fsinfo);
    if(!b) return -EIO;
    if(le32(b->data) == FSINFO_LEAD && le32(b->data + 484) == FSINFO_STRUCT) {
        put32(b->data + 488, fs->free);
        put32(b->data + 492, fs->next_free >= 2 && fs->next_free < fs->nclusters + 2 ? fs->next_free : 2);
        bdirty(b);
    }
    brelse(b);
    fs->fsinfo_dirty = false;
    return 0;
}

/**** directory iteration ****/

static void diter_init(struct diter *it, struct fat_fs *fs, struct fat_node *dir) {
    it->fs = fs;
    it->idx = 0;
    it->off = 0;
    if(dir->is_root && fs->type != 32) {                     /* the fixed FAT12/16 root */
        it->first = it->clus = 0;
        it->lba = fs->root_lba;
        it->at_end = fs->root_sectors == 0;
    } else {                                                 /* a cluster chain; a broken entry is empty */
        it->first = it->clus = dir->first;
        it->at_end = dir->first < 2 || dir->first >= fs->nclusters + 2;
        it->lba = it->at_end ? 0 : clus_lba(fs, dir->first);
    }
}

/* step to the next entry; at_end becomes true when the directory's space is used up */
static int diter_next(struct diter *it) {
    struct fat_fs *fs = it->fs;
    it->idx++;
    if(it->idx >= DIR_MAX_ENTRIES) { it->at_end = true; return 0; }   /* a looping chain ends here too */
    it->off += 32;
    if(it->off < 512) return 0;
    it->off = 0;
    it->lba++;
    if(!it->first) {
        if(it->lba >= fs->root_lba + fs->root_sectors) it->at_end = true;
    } else if(it->lba - clus_lba(fs, it->clus) >= fs->spc) {
        u32 next;
        int r = fat_get(fs, it->clus, &next);
        if(r) return r;
        if(is_eoc(fs, next)) { it->lba--; it->at_end = true; }   /* stay on the last valid sector */
        else { it->clus = next; it->lba = clus_lba(fs, next); }
    }
    return 0;
}

/* give the directory one more (zeroed) cluster and point the iterator at its first entry */
static int diter_extend(struct diter *it) {
    struct fat_fs *fs = it->fs;
    u32 c;
    int r;
    if(!it->first || it->idx >= DIR_MAX_ENTRIES) return -ENOSPC;    /* the FAT12/16 root cannot grow */
    r = fat_alloc(fs, it->clus, true, &c);
    if(r) return r;
    it->clus = c;
    it->lba = clus_lba(fs, c);
    it->off = 0;
    it->at_end = false;
    return 0;
}

static u8 lfn_checksum(const u8 *short_name) {
    u8 sum = 0;
    int i;
    for(i = 0; i < 11; i++) sum = (u8)(((sum & 1) << 7) + (sum >> 1) + short_name[i]);
    return sum;
}

/* the 8.3 name as UTF-8 (code page 437 above 0x7F; NT's lower-case flags honoured) */
static void short_to_name(const u8 *raw, char *out) {
    u32 n = 0;
    int i, base = 8, ext = 3;
    while(base && raw[base - 1] == ' ') base--;
    while(ext && raw[8 + ext - 1] == ' ') ext--;
    for(i = 0; i < base; i++) {
        u8 c = (i == 0 && raw[0] == 0x05) ? 0xE5 : raw[i];  /* 0x05 stands for a leading 0xE5 */
        if(raw[12] & NTRES_LOWER_BASE) c = (u8)lower(c);
        n += utf8_put(out + n, c < 0x80 ? c : cp437[c - 0x80]);
    }
    if(ext) {
        out[n++] = '.';
        for(i = 0; i < ext; i++) {
            u8 c = raw[8 + i];
            if(raw[12] & NTRES_LOWER_EXT) c = (u8)lower(c);
            n += utf8_put(out + n, c < 0x80 ? c : cp437[c - 0x80]);
        }
    }
    out[n] = 0;
}

/* the directory block the iterator is in, read with the rest of its cluster (or root area) */
static struct buf *dir_block(struct diter *it) {
    struct fat_fs *fs = it->fs;
    u32 end = it->first ? clus_lba(fs, it->clus) + fs->spc : fs->root_lba + fs->root_sectors;
    return breada(fs->dev, it->lba, end > it->lba ? (end - it->lba < META_AHEAD ? end - it->lba : META_AHEAD) : 1);
}

/* the next logical entry (long name resolved, free and label entries skipped): 0, 1 at the end, < 0 */
static int dir_read(struct diter *it, struct fat_dirent *e) {
    u16 lfn[LFN_MAX_ENTRIES * LFN_CHARS];
    u32 lfn_start = 0, len;
    int lfn_expected = 0, lfn_seen = 0;
    u8 lfn_sum = 0;

    for(;;) {
        struct buf *b;
        u8 *p;
        int r;
        if(it->at_end) return 1;
        b = dir_block(it);
        if(!b) return -EIO;
        p = b->data + it->off;
        if(p[0] == 0) { brelse(b); it->at_end = true; return 1; }
        if(p[0] == 0xE5) {
            lfn_expected = lfn_seen = 0;
        } else if((p[11] & ATTR_LFN) == ATTR_LFN) {
            int seq = p[0] & 0x1F, k;
            if(p[0] & 0x40) { lfn_expected = seq; lfn_seen = 0; lfn_sum = p[13]; lfn_start = it->idx; memset(lfn, 0, sizeof(lfn)); }
            if(lfn_expected && seq >= 1 && seq <= LFN_MAX_ENTRIES && p[13] == lfn_sum) {
                static const u8 pos[LFN_CHARS] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
                for(k = 0; k < LFN_CHARS; k++) lfn[(seq - 1) * LFN_CHARS + k] = le16(p + pos[k]);
                lfn_seen++;
            } else lfn_expected = 0;
        } else if(p[11] & ATTR_LABEL) {
            lfn_expected = lfn_seen = 0;
        } else {
            memcpy(e->raw, p, 32);
            e->lba = it->lba;
            e->off = it->off;
            e->idx = it->idx;
            for(len = 0; lfn_expected && len < (u32)lfn_expected * LFN_CHARS && lfn[len] && lfn[len] != 0xFFFF; len++);
            if(lfn_expected && lfn_seen == lfn_expected && lfn_checksum(p) == lfn_sum && len &&
               utf16_to_utf8(lfn, len, e->name, sizeof(e->name))) {   /* else (too long as UTF-8, say) the 8.3 name */
                e->lfn_idx = lfn_start;
            } else {
                short_to_name(p, e->name);
                e->lfn_idx = it->idx;
            }
            e->first = le16(p + 26) | (it->fs->type == 32 ? (u32)le16(p + 20) << 16 : 0);
            e->size = le32(p + 28);
            brelse(b);
            r = diter_next(it);
            return r < 0 ? r : 0;
        }
        brelse(b);
        r = diter_next(it);
        if(r) return r;
    }
}

static int dir_find(struct fat_fs *fs, struct fat_node *dir, const char *name, struct fat_dirent *e) {
    struct diter it;
    int r;
    diter_init(&it, fs, dir);
    while((r = dir_read(&it, e)) == 0) {
        if(name_eq(e->name, name)) return 0;
    }
    return r < 0 ? r : -ENOENT;
}

/* a run of `need` free entries, extending the directory if it has to; pos ends at the first one */
static int dir_find_free(struct fat_fs *fs, struct fat_node *dir, u32 need, struct diter *pos) {
    struct diter it, start;
    u32 run = 0;
    diter_init(&it, fs, dir);
    for(;;) {
        struct buf *b;
        bool free;
        int r;
        if(it.at_end) { r = diter_extend(&it); if(r) return r; }
        b = bread(fs->dev, it.lba);
        if(!b) return -EIO;
        free = (b->data[it.off] == 0 || b->data[it.off] == 0xE5);
        brelse(b);
        if(free) {
            if(!run) start = it;
            if(++run == need) { *pos = start; return 0; }
        } else run = 0;
        r = diter_next(&it);
        if(r) return r;
    }
}

/* write n raw entries at pos (the space was found by dir_find_free); *lba and *off (when
   given) say where the last one, the short entry, went */
static int dir_write_entries(struct diter *pos, const u8 *entries, u32 n, u32 *lba, u32 *off) {
    u32 i;
    for(i = 0; i < n; i++) {
        struct buf *b;
        int r;
        if(pos->at_end) { r = diter_extend(pos); if(r) return r; }
        b = bread(pos->fs->dev, pos->lba);
        if(!b) return -EIO;
        memcpy(b->data + pos->off, entries + i * 32, 32);
        bdirty(b);
        brelse(b);
        if(i == n - 1 && lba) { *lba = pos->lba; *off = pos->off; }
        r = diter_next(pos);
        if(r) return r;
    }
    return 0;
}

/* mark the entries idx_from..idx_to of dir as deleted */
static int dir_delete_entries(struct fat_fs *fs, struct fat_node *dir, u32 idx_from, u32 idx_to) {
    struct diter it;
    diter_init(&it, fs, dir);
    while(!it.at_end && it.idx <= idx_to) {
        if(it.idx >= idx_from) {
            struct buf *b = bread(fs->dev, it.lba);
            if(!b) return -EIO;
            b->data[it.off] = 0xE5;
            bdirty(b);
            brelse(b);
        }
        if(diter_next(&it)) return -EIO;
    }
    return 0;
}

/* the short entry of a file after its size, first cluster or time changed */
static int update_entry(struct fat_fs *fs, struct fat_node *n, u32 size, bool touch) {
    struct buf *b;
    u8 *p;
    if(n->is_root) return 0;
    b = bread(fs->dev, n->dir_lba);
    if(!b) return -EIO;
    p = b->data + n->dir_off;
    put32(p + 28, size);
    put16(p + 26, (u16)(n->first & 0xFFFF));
    if(fs->type == 32) put16(p + 20, (u16)(n->first >> 16));
    if(touch) {
        u16 d, t;
        fat_now(&d, &t);
        put16(p + 22, t);
        put16(p + 24, d);
        put16(p + 18, d);
        p[11] |= ATTR_ARCHIVE;
    }
    bdirty(b);
    brelse(b);
    return 0;
}

/**** names ****/

static bool legal_char(int c) {
    if(c < 0x20 || c == 0x7F) return false;
    return !strchr("\\/:*?\"<>|", c);
}

/* a name a new entry may have: well-formed UTF-8 of at most 255 UTF-16 units, none of
   \\ / : * ? " < > | or control characters, not "." or "..", not ending in a space or a dot */
static int validate_name(const char *name) {
    u16 units[LFN_MAX_ENTRIES * LFN_CHARS];
    u32 count;
    size_t n = strlen(name);
    size_t i;
    if(!n) return -EINVAL;
    if(n > VFS_NAME_MAX) return -ENAMETOOLONG;
    if(!strcmp(name, ".") || !strcmp(name, "..")) return -EINVAL;
    for(i = 0; i < n; i++) if(!legal_char((u8)name[i])) return -EINVAL;
    if(name[n - 1] == ' ' || name[n - 1] == '.') return -EINVAL;
    return name_to_utf16(name, units, &count);
}

static bool short_char_ok(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("$%'-_@~`!(){}^#&", c) != NULL;
}

/* is raw11 the short name of an entry of dir other than the one at index skip? */
static bool dir_has_short(struct fat_fs *fs, struct fat_node *dir, const u8 *raw11, u32 skip) {
    struct diter it;
    struct fat_dirent e;
    diter_init(&it, fs, dir);
    while(dir_read(&it, &e) == 0) if(e.idx != skip && !memcmp(e.raw, raw11, 11)) return true;
    return false;
}

/* the alias character for code point cp: upper case, or '_' for what an 8.3 name cannot
   hold, everything beyond ASCII included */
static int short_char(i32 cp) {
    if(cp > 0 && cp < 0x80 && short_char_ok(upper((int)cp))) return upper((int)cp);
    return '_';
}

/* The 8.3 form of name in raw[11] with the lowercase flags in *ntres, unique in dir (the
   entry at index skip, which is being renamed, does not count). *lfn says whether long-name
   entries are needed as well (the name is not representable as 8.3). A name that is not
   8.3 gets fatgen103's basis name: upper case, '_' for what cannot be held, spaces and
   leading dots dropped, the base up to the first dot that is left (8 at most), the extension
   after the last dot (3 at most); then a ~N tail, unless only the case was in the way.
   -ENOSPC when every ~N tail is taken. */
static int make_short_name(struct fat_fs *fs, struct fat_node *dir, const char *name, u32 skip, u8 *raw, u8 *ntres, bool *lfn) {
    const char *dot = strrchr(name, '.'), *q, *last;
    size_t blen = dot ? (size_t)(dot - name) : strlen(name), elen = dot ? strlen(dot + 1) : 0;
    bool fits = true, base_lower = false, base_upper = false, ext_lower = false, ext_upper = false;
    size_t i, n, nb = 0, ne = 0;
    char base[9], ext[4];

    memset(raw, ' ', 11);
    *ntres = 0;
    *lfn = true;
    if(dot == name) { fits = false; blen = 0; }              /* ".name" */
    if(blen > 8 || elen > 3 || (dot && !elen)) fits = false;
    for(i = 0; i < blen && fits; i++) {
        int c = (u8)name[i];
        if(c == '.' || c == ' ') fits = false;
        if(c >= 'a' && c <= 'z') base_lower = true;
        else if(c >= 'A' && c <= 'Z') base_upper = true;
        if(!short_char_ok(upper(c))) fits = false;           /* bytes beyond ASCII are never 8.3 */
    }
    for(i = 0; i < elen && fits; i++) {
        int c = (u8)dot[1 + i];
        if(c == '.' || c == ' ') fits = false;
        if(c >= 'a' && c <= 'z') ext_lower = true;
        else if(c >= 'A' && c <= 'Z') ext_upper = true;
        if(!short_char_ok(upper(c))) fits = false;
    }
    if(fits) {                                               /* 8.3 already: upper case, and flags for lower case */
        for(i = 0; i < blen; i++) raw[i] = upper((u8)name[i]);
        for(i = 0; i < elen; i++) raw[8 + i] = upper((u8)dot[1 + i]);
        if(!dir_has_short(fs, dir, raw, skip)) {             /* else another entry's alias: a ~N name and a long name */
            if((base_lower && base_upper) || (ext_lower && ext_upper)) return 0;   /* mixed case: the long name keeps it */
            if(base_lower) *ntres |= NTRES_LOWER_BASE;
            if(ext_lower) *ntres |= NTRES_LOWER_EXT;
            *lfn = false;
            return 0;
        }
    }
    /* the basis name, then BASENA~1.EXT */
    for(q = name; *q == '.'; q++);
    last = strrchr(q, '.');
    while(*q && *q != '.' && nb < 8) {
        i32 cp = utf8_next(&q);
        if(cp < 0) break;
        if(cp != ' ') base[nb++] = (char)short_char(cp);
    }
    if(!nb) base[nb++] = '_';
    base[nb] = 0;
    for(q = last ? last + 1 : ""; *q && ne < 3; ) {
        i32 cp = utf8_next(&q);
        if(cp < 0) break;
        if(cp != ' ') ext[ne++] = (char)short_char(cp);
    }
    ext[ne] = 0;
    for(n = 1; n < 1000; n++) {
        char tail[8];
        size_t bl = nb, tl;
        snprintf(tail, sizeof(tail), "~%u", (u_int)n);
        tl = strlen(tail);
        if(bl + tl > 8) bl = 8 - tl;
        memset(raw, ' ', 11);
        memcpy(raw, base, bl);
        memcpy(raw + bl, tail, tl);
        memcpy(raw + 8, ext, ne);
        if(!dir_has_short(fs, dir, raw, skip)) return 0;
    }
    return -ENOSPC;
}

/* Raw entries for a name: the LFN entries (if any) followed by the short entry; *count
   of them. tmpl, when given, is the short entry of the object being renamed (at index skip
   of dir): its attributes, times, cluster and size carry over to the new entry. */
static int build_entries(struct fat_fs *fs, struct fat_node *dir, const char *name, u8 attr, u32 first,
                         const u8 *tmpl, u32 skip, u8 *out, u32 *count) {
    u8 raw[11], ntres, sum;
    u32 n = 0, nlfn = 0, k;
    bool lfn;
    u16 d, t;
    u8 *p;
    u16 units[LFN_MAX_ENTRIES * LFN_CHARS];
    u32 len;
    int r = name_to_utf16(name, units, &len);

    if(r == 0) r = make_short_name(fs, dir, name, skip, raw, &ntres, &lfn);
    if(r) return r;
    if(lfn) {
        nlfn = (len + LFN_CHARS - 1) / LFN_CHARS;
        sum = lfn_checksum(raw);
        for(k = nlfn; k >= 1; k--) {
            static const u8 pos[LFN_CHARS] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
            u32 j;
            p = out + n * 32;
            memset(p, 0, 32);
            p[0] = (u8)(k | (k == nlfn ? 0x40 : 0));
            p[11] = ATTR_LFN;
            p[13] = sum;
            for(j = 0; j < LFN_CHARS; j++) {
                u32 ci = (k - 1) * LFN_CHARS + j;
                put16(p + pos[j], ci < len ? units[ci] : (ci == len ? 0 : 0xFFFF));
            }
            n++;
        }
    }
    p = out + n * 32;
    if(tmpl) memcpy(p, tmpl, 32);                            /* a rename: everything but the name stays */
    else {
        memset(p, 0, 32);
        p[11] = attr;
        fat_now(&d, &t);
        put16(p + 14, t); put16(p + 16, d);                  /* created */
        put16(p + 18, d);                                    /* accessed */
        put16(p + 20, (u16)(fs->type == 32 ? first >> 16 : 0));
        put16(p + 22, t); put16(p + 24, d);                  /* modified */
        put16(p + 26, (u16)(first & 0xFFFF));
    }
    memcpy(p, raw, 11);
    p[12] = (p[12] & ~(NTRES_LOWER_BASE | NTRES_LOWER_EXT)) | ntres;
    *count = n + 1;
    return 0;
}

/**** vnodes ****/

/* the in-use node of the entry at lba/off, or NULL */
static struct fat_node *live_node(struct fat_fs *fs, u32 lba, u32 off) {
    struct fat_node *n;
    for(n = fs->live; n; n = n->next) if(n->dir_lba == lba && n->dir_off == off && n->v->refs) return n;
    return NULL;
}

/* the vnode of a directory entry: the one in use already, else a new one (holds sb->lock) */
static struct vnode *fat_vnode(struct superblock *sb, const struct fat_dirent *e) {
    struct fat_fs *fs = (struct fat_fs *)sb->priv;
    struct fat_node *n;
    struct vnode *v;
    for(n = fs->live; n; n = n->next) {
        if(n->dir_lba == e->lba && n->dir_off == e->off) {
            u_long f = irq_save();
            bool alive = n->v->refs > 0;                     /* 0: vput is releasing it right now */
            if(alive) n->v->refs++;
            irq_restore(f);
            if(alive) return n->v;
        }
    }
    n = (struct fat_node *)kcalloc(1, sizeof(struct fat_node));
    if(!n) return NULL;
    v = vnode_alloc(sb, &fat_vops, (e->raw[11] & ATTR_DIR) ? VN_DIR : VN_FILE);
    if(!v) { kfree(n); return NULL; }
    n->v = v;
    n->first = e->first;
    n->dir_lba = e->lba;
    n->dir_off = e->off;
    v->size = (e->raw[11] & ATTR_DIR) ? 0 : e->size;
    v->mtime = fat_to_unix(le16(e->raw + 24), le16(e->raw + 22));
    v->ino = (e->lba << 4) | (e->off / 32);
    v->priv = n;
    n->next = fs->live;
    fs->live = n;
    return v;
}

static int fat_lookup(struct vnode *dir, const char *name, struct vnode **out) {
    struct fat_fs *fs = (struct fat_fs *)dir->sb->priv;
    struct fat_dirent e;
    int r;
    if(fs->dev->gone) return -EIO;                           /* unplugged */
    mutex_lock(&dir->sb->lock);
    r = dir_find(fs, (struct fat_node *)dir->priv, name, &e);
    if(r == 0) {
        *out = fat_vnode(dir->sb, &e);
        if(!*out) r = -ENOMEM;
    }
    mutex_unlock(&dir->sb->lock);
    return r;
}

/* entry index of the directory; a listing asks for 0, 1, 2... and each call resumes where the
   last one stopped instead of reading the directory from its start again */
static int fat_readdir(struct vnode *dir, u_int index, struct dirent *de) {
    struct fat_fs *fs = (struct fat_fs *)dir->sb->priv;
    struct fat_node *dn = (struct fat_node *)dir->priv;
    struct diter it;
    struct fat_dirent e;
    u_int skip = index;
    int r;
    if(fs->dev->gone) return -EIO;
    mutex_lock(&dir->sb->lock);
    if(dn->rd_valid && dn->rd_index == index) { it = dn->rd_it; skip = 0; }
    else diter_init(&it, fs, dn);
    dn->rd_valid = false;
    while((r = dir_read(&it, &e)) == 0) {
        if(!strcmp(e.name, ".") || !strcmp(e.name, "..")) continue;
        if(skip-- == 0) {
            strlcpy(de->name, e.name, sizeof(de->name));
            de->type = (e.raw[11] & ATTR_DIR) ? VN_DIR : VN_FILE;
            de->size = (e.raw[11] & ATTR_DIR) ? 0 : e.size;
            de->mtime = fat_to_unix(le16(e.raw + 24), le16(e.raw + 22));
            dn->rd_it = it;
            dn->rd_index = index + 1;
            dn->rd_valid = true;
            break;
        }
    }
    mutex_unlock(&dir->sb->lock);
    return r;
}

/* the cluster after c in its chain; on a write past the end of the chain a new one (c + 1 when
   that is free). 0, 1 when a read reaches the end, < 0. */
static int next_cluster(struct fat_fs *fs, u32 c, bool grow, u32 *out) {
    u32 next;
    int r = fat_get(fs, c, &next);
    if(r) return r;
    if(is_eoc(fs, next)) return grow ? fat_alloc(fs, c, false, out) : 1;
    *out = next;
    return 0;
}

/* The cluster at index ci of n's chain, walked to from the position n remembers when that is
   not past ci (sequential I/O takes one step per cluster, not a walk from the start). With
   grow the chain is extended as needed. 0, 1 when ci is past the end (reading), < 0. */
static int seek_cluster(struct fat_fs *fs, struct fat_node *n, u32 ci, bool grow, u32 *out) {
    u32 c, i;
    int r;
    if(!n->first) {
        if(!grow) return 1;
        r = fat_alloc(fs, 0, false, &c);
        if(r) return r;
        n->first = c;
        n->pos_c = 0;
    }
    if(n->pos_c && n->pos_ci <= ci) { i = n->pos_ci; c = n->pos_c; }
    else { i = 0; c = n->first; }
    while(i < ci) {
        r = next_cluster(fs, c, grow, &c);
        if(r) return r;
        i++;
    }
    n->pos_ci = ci;
    n->pos_c = c;
    *out = c;
    return 0;
}

/* How many blocks from block s of cluster c on are next to each other on the disk and in the
   file (the chain going c, c + 1, ...), up to max; with grow the chain is extended as far as
   that needs (c + 1 first). */
static int run_blocks(struct fat_fs *fs, u32 c, u32 s, u32 max, bool grow, u32 *count) {
    u32 n = fs->spc - s, next;
    while(n < max) {
        int r = next_cluster(fs, c, grow, &next);
        if(r < 0) return r;
        if(r == 1 || next != c + 1) break;
        c = next;
        n += fs->spc;
    }
    *count = n < max ? n : max;
    return 0;
}

/* chunk bytes at so in block s of cluster c through the cache; pos is the file offset of the
   first of them. A read that misses reads ahead along the file; a write to a whole block, or
   to one that starts past the end of the file, does not read it first. */
static int block_rw(struct fat_fs *fs, struct vnode *v, u32 c, u32 s, u32 so, u8 *buf, u32 chunk, u32 pos, int mode) {
    u32 lba = clus_lba(fs, c) + s;
    struct buf *b;
    if(mode == RW_READ) {
        b = bpeek(fs->dev, lba);
        if(!b) {
            u32 ahead, left = (v->size - (pos - so) + 511) / 512;
            if(run_blocks(fs, c, s, FAT_READAHEAD, false, &ahead) != 0) ahead = 1;
            b = breada(fs->dev, lba, ahead < left ? ahead : (left ? left : 1));
        }
        if(!b) return -EIO;
        memcpy(buf, b->data + so, chunk);
    } else {
        b = (chunk == 512 || pos - so >= v->size) ? bget(fs->dev, lba) : bread(fs->dev, lba);
        if(!b) return -EIO;
        if(mode == RW_WRITE) memcpy(b->data + so, buf, chunk);
        else memset(b->data + so, 0, chunk);
        bdirty(b);
    }
    brelse(b);
    return 0;
}

/* len bytes at off of the file: read into buf, written from buf, or (RW_ZERO, buf NULL) zeroed;
   writes grow the chain. Runs of FAT_DIRECT whole blocks or more that lie next to each other
   on the disk move straight between the device and buf, up to 64 KB a request; the rest goes
   through the cache. Holds sb->lock. The bytes done, else < 0. */
static int rw_locked(struct fat_fs *fs, struct vnode *v, u32 off, u8 *buf, u32 len, int mode) {
    struct fat_node *n = (struct fat_node *)v->priv;
    bool grow = mode != RW_READ;
    u32 done = 0, c, within = off % fs->cluster_bytes;
    int r = seek_cluster(fs, n, off / fs->cluster_bytes, grow, &c);

    while(r == 0 && done < len) {
        u32 s = within / 512, so = within % 512, left = len - done, chunk = 512 - so, nb = 0;
        u8 *p = buf ? buf + done : NULL;
        if(!so && left >= 512) {
            r = run_blocks(fs, c, s, left / 512 < BCACHE_RUN ? left / 512 : BCACHE_RUN, grow, &nb);
            if(r) break;
        }
        if(nb >= FAT_DIRECT) {
            u32 lba = clus_lba(fs, c) + s;
            r = mode == RW_READ ? bread_blocks(fs->dev, lba, nb, p) : mode == RW_WRITE ? bwrite_blocks(fs->dev, lba, nb, p)
                                                                      : bzero_blocks(fs->dev, lba, nb);
            if(r) { r = -EIO; break; }
            chunk = nb * 512;
        } else {
            if(chunk > left) chunk = left;
            r = block_rw(fs, v, c, s, so, p, chunk, off + done, mode);
            if(r) break;
        }
        done += chunk;
        within += chunk;
        while(within >= fs->cluster_bytes && done < len) {   /* on to the next cluster (several after a run) */
            within -= fs->cluster_bytes;
            r = next_cluster(fs, c, grow, &c);
            if(r) break;
            n->pos_ci++;
            n->pos_c = c;
        }
    }
    if(r == 1) r = 0;                                        /* a read reached the end of the chain */
    return done ? (int)done : r;
}

/* the file grows to size with zeros (a write past its end, a truncate that grows); sb->lock */
static int grow_zeros(struct fat_fs *fs, struct vnode *v, u32 size) {
    int r = rw_locked(fs, v, v->size, NULL, size - v->size, RW_ZERO);
    if(r > 0) v->size += r;
    return v->size == size ? 0 : r < 0 ? r : -ENOSPC;
}

static int fat_rw(struct vnode *v, u32 off, u8 *buf, u32 len, bool write) {
    struct fat_fs *fs = (struct fat_fs *)v->sb->priv;
    struct fat_node *n = (struct fat_node *)v->priv;
    u32 size;
    int r = 0;

    if(!write) {
        if(off >= v->size) return 0;
        if(len > v->size - off) len = v->size - off;
    }
    if(!len) return 0;
    if(write && off + len < off) return -EFBIG;
    if(fs->dev->gone) return -EIO;
    mutex_lock(&v->sb->lock);
    size = v->size;
    if(write && off > v->size) r = grow_zeros(fs, v, off);   /* the gap reads as zeros */
    if(r == 0) r = rw_locked(fs, v, off, buf, len, write ? RW_WRITE : RW_READ);
    if(write && r > 0 && off + (u32)r > v->size) v->size = off + r;
    if(write && (r > 0 || v->size != size) && update_entry(fs, n, v->size, true) != 0 && r >= 0) r = -EIO;
    mutex_unlock(&v->sb->lock);
    return r;
}

static int fat_read(struct vnode *v, u32 off, void *buf, u32 len) {
    return fat_rw(v, off, (u8 *)buf, len, false);
}

static int fat_write(struct vnode *v, u32 off, const void *buf, u32 len) {
    return fat_rw(v, off, (u8 *)buf, len, true);
}

static int fat_truncate(struct vnode *v, u32 size) {
    struct fat_fs *fs = (struct fat_fs *)v->sb->priv;
    struct fat_node *n = (struct fat_node *)v->priv;
    int r = 0;
    if(fs->dev->gone) return -EIO;
    mutex_lock(&v->sb->lock);
    if(size > v->size) {                                     /* growing: zeros */
        r = grow_zeros(fs, v, size);
        if(update_entry(fs, n, v->size, true) != 0 && !r) r = -EIO;
        mutex_unlock(&v->sb->lock);
        return r;
    }
    n->pos_c = 0;                                            /* the remembered position may be freed */
    if(size == 0) {
        if(n->first) r = fat_free_chain(fs, n->first);
        n->first = 0;
    } else {
        u32 keep = (size + fs->cluster_bytes - 1) / fs->cluster_bytes, c = n->first, next;
        while(--keep && !r) { if(fat_get(fs, c, &next) != 0 || is_eoc(fs, next)) r = -EIO; else c = next; }
        if(!r && fat_get(fs, c, &next) == 0 && !is_eoc(fs, next)) {
            r = fat_set(fs, c, fs->eoc);
            if(!r) r = fat_free_chain(fs, next);
        }
    }
    if(!r) { v->size = size; r = update_entry(fs, n, size, true); }
    mutex_unlock(&v->sb->lock);
    return r;
}

static int fat_create(struct vnode *dir, const char *name, int type, struct vnode **out) {
    struct fat_fs *fs = (struct fat_fs *)dir->sb->priv;
    struct fat_node *dn = (struct fat_node *)dir->priv;
    struct fat_dirent e;
    struct diter pos;
    u8 entries[(LFN_MAX_ENTRIES + 1) * 32];
    u32 n, first = 0;
    int r = validate_name(name);

    if(r) return r;
    if(fs->dev->gone) return -EIO;
    mutex_lock(&dir->sb->lock);
    dn->rd_valid = false;
    r = dir_find(fs, dn, name, &e);
    if(r == 0) { r = -EEXIST; goto out; }
    if(r != -ENOENT) goto out;
    if(type == VN_DIR) {                                     /* a directory starts with "." and ".." */
        struct buf *b;
        u8 *p;
        u16 d, t;
        u32 parent = dn->is_root ? 0 : dn->first;
        r = fat_alloc(fs, 0, true, &first);
        if(r) goto out;
        b = bread(fs->dev, clus_lba(fs, first));
        if(!b) { r = -EIO; goto out; }
        fat_now(&d, &t);
        p = b->data;
        memset(p, ' ', 11); p[0] = '.'; p[11] = ATTR_DIR;
        put16(p + 22, t); put16(p + 24, d); put16(p + 26, (u16)first); put16(p + 20, (u16)(fs->type == 32 ? first >> 16 : 0));
        p += 32;
        memset(p, ' ', 11); p[0] = '.'; p[1] = '.'; p[11] = ATTR_DIR;
        put16(p + 22, t); put16(p + 24, d); put16(p + 26, (u16)parent); put16(p + 20, (u16)(fs->type == 32 ? parent >> 16 : 0));
        bdirty(b);
        brelse(b);
    }
    r = build_entries(fs, dn, name, type == VN_DIR ? ATTR_DIR : ATTR_ARCHIVE, first, NULL, NO_IDX, entries, &n);
    if(r == 0) r = dir_find_free(fs, dn, n, &pos);
    if(r == 0) r = dir_write_entries(&pos, entries, n, NULL, NULL);
    if(r) { if(first) fat_free_chain(fs, first); goto out; }
    r = dir_find(fs, dn, name, &e);                          /* read it back for the vnode */
    if(r == 0) {
        *out = fat_vnode(dir->sb, &e);
        if(!*out) r = -ENOMEM;
    }
out:
    mutex_unlock(&dir->sb->lock);
    return r;
}

/* 0 when the directory starting at cluster first holds nothing but "." and "..", else -ENOTEMPTY */
static int dir_empty(struct fat_fs *fs, u32 first) {
    struct fat_node sub;
    struct diter it;
    struct fat_dirent x;
    int r;
    memset(&sub, 0, sizeof(sub));
    sub.first = first;
    diter_init(&it, fs, &sub);
    while((r = dir_read(&it, &x)) == 0) {
        if(strcmp(x.name, ".") && strcmp(x.name, "..")) return -ENOTEMPTY;
    }
    return r < 0 ? r : 0;
}

static int fat_unlink(struct vnode *dir, const char *name) {
    struct fat_fs *fs = (struct fat_fs *)dir->sb->priv;
    struct fat_node *dn = (struct fat_node *)dir->priv;
    struct fat_dirent e;
    int r;
    if(fs->dev->gone) return -EIO;
    mutex_lock(&dir->sb->lock);
    dn->rd_valid = false;
    r = dir_find(fs, dn, name, &e);
    if(r) goto out;
    if((e.raw[11] & ATTR_DIR) && (r = dir_empty(fs, e.first)) != 0) goto out;   /* only an empty directory goes */
    if(e.first) r = fat_free_chain(fs, e.first);
    if(r == 0) r = dir_delete_entries(fs, dn, e.lfn_idx, e.idx);
out:
    mutex_unlock(&dir->sb->lock);
    return r;
}

/* do two directory nodes stand for the same directory? */
static bool dir_same(const struct fat_node *a, const struct fat_node *b) {
    if(a->is_root || b->is_root) return a->is_root && b->is_root;
    return a->first == b->first;
}

/* point the ".." entry of the directory at cluster first to its new parent (0 for the root) */
static int set_dotdot(struct fat_fs *fs, u32 first, const struct fat_node *parent) {
    u32 pc = parent->is_root ? 0 : parent->first;
    struct buf *b;
    u8 *p;
    if(first < 2 || first >= fs->nclusters + 2) return 0;
    b = bread(fs->dev, clus_lba(fs, first));
    if(!b) return -EIO;
    p = b->data + 32;
    if(p[0] == '.' && p[1] == '.' && p[2] == ' ' && (p[11] & ATTR_DIR)) {
        put16(p + 26, (u16)(pc & 0xFFFF));
        put16(p + 20, (u16)(fs->type == 32 ? pc >> 16 : 0));
        bdirty(b);
    }
    brelse(b);
    return 0;
}

/* 10/5/26: rename within the volume. The new entries (a fresh short alias, the long name if
   one is needed, everything else copied from the old short entry) are written before the
   old ones are deleted, so an interruption leaves two names rather than none. A file at the
   target is replaced (POSIX), so is an empty directory; a directory that moves to another
   parent gets its ".." pointed there. The VFS has refused moving a directory into itself. */
static int fat_rename(struct vnode *odir, const char *oname, struct vnode *ndir, const char *nname) {
    struct fat_fs *fs = (struct fat_fs *)odir->sb->priv;
    struct fat_node *on = (struct fat_node *)odir->priv, *nn = (struct fat_node *)ndir->priv, *x;
    struct fat_dirent src, dst;
    struct diter pos;
    u8 entries[(LFN_MAX_ENTRIES + 1) * 32];
    u32 n, new_lba = 0, new_off = 0;
    bool same = dir_same(on, nn), is_dir, replace = false;
    int r = validate_name(nname);

    if(r) return r;
    if(fs->dev->gone) return -EIO;
    mutex_lock(&odir->sb->lock);
    on->rd_valid = nn->rd_valid = false;
    r = dir_find(fs, on, oname, &src);
    if(r) goto out;
    is_dir = (src.raw[11] & ATTR_DIR) != 0;
    r = dir_find(fs, nn, nname, &dst);
    if(r == 0 && !(same && dst.idx == src.idx)) {            /* another object has the name (else: a change of case) */
        if(live_node(fs, dst.lba, dst.off)) { r = -EBUSY; goto out; }   /* in use: not deleted under its user */
        if(is_dir && !(dst.raw[11] & ATTR_DIR)) { r = -ENOTDIR; goto out; }
        if(!is_dir && (dst.raw[11] & ATTR_DIR)) { r = -EISDIR; goto out; }
        if(is_dir && (r = dir_empty(fs, dst.first)) != 0) goto out;
        replace = true;
    } else if(r != 0 && r != -ENOENT) goto out;
    r = build_entries(fs, nn, nname, 0, 0, src.raw, same ? src.idx : NO_IDX, entries, &n);
    if(r == 0) r = dir_find_free(fs, nn, n, &pos);
    if(r == 0) r = dir_write_entries(&pos, entries, n, &new_lba, &new_off);
    if(r) goto out;
    if(replace) {
        r = dir_delete_entries(fs, nn, dst.lfn_idx, dst.idx);
        if(r == 0 && dst.first) r = fat_free_chain(fs, dst.first);
        if(r) goto out;
    }
    r = dir_delete_entries(fs, on, src.lfn_idx, src.idx);
    if(r == 0 && is_dir && !same) r = set_dotdot(fs, src.first, nn);
    for(x = fs->live; x; x = x->next) {                      /* an open file or directory follows its entry */
        if(x->dir_lba == src.lba && x->dir_off == src.off) { x->dir_lba = new_lba; x->dir_off = new_off; }
    }
out:
    mutex_unlock(&odir->sb->lock);
    return r;
}

/* the last reference is gone: forget the node (the root's goes at unmount) */
static void fat_release(struct vnode *v) {
    struct fat_node *n = (struct fat_node *)v->priv, **pp;
    struct fat_fs *fs = (struct fat_fs *)v->sb->priv;
    if(n && !n->is_root) {
        mutex_lock(&v->sb->lock);
        for(pp = &fs->live; *pp; pp = &(*pp)->next) if(*pp == n) { *pp = n->next; break; }
        mutex_unlock(&v->sb->lock);
    }
    kfree(n);
}

static const struct vnode_ops fat_vops = {
    fat_lookup, fat_readdir, fat_read, fat_write, fat_truncate, fat_create, fat_unlink, fat_release, fat_rename
};

/**** mount ****/

static bool bpb_sane(const u8 *bs) {
    u16 bps = le16(bs + 11);
    u8 spc = bs[13], nfats = bs[16];
    u16 rsv = le16(bs + 14);
    if(bs[510] != 0x55 || bs[511] != 0xAA) return false;
    if(bs[0] != 0xEB && bs[0] != 0xE9) return false;
    if(bps != 512 || !spc || (spc & (spc - 1)) || !rsv || !nfats || nfats > 2) return false;
    if(!le16(bs + 19) && !le32(bs + 32)) return false;
    return true;
}

static bool fat_probe(struct bdev *dev) {
    struct buf *b = bread(dev, 0);
    bool ok;
    if(!b) return false;
    ok = bpb_sane(b->data);
    brelse(b);
    return ok;
}

static void copy_label(char *out, const u8 *raw);

/* 10/5/26: what the boot sector says, for mkfs and mount: "FAT32, label TESTVOL" */
int fat_describe(struct bdev *d, char *buf, size_t n) {
    struct buf *b = bread(d, 0);
    const u8 *bs;
    u32 total, spf, data, clusters;
    char label[12];
    if(!b) return -1;
    bs = b->data;
    if(!bpb_sane(bs)) { brelse(b); return -1; }
    total = le16(bs + 19) ? le16(bs + 19) : le32(bs + 32);
    spf = le16(bs + 22) ? le16(bs + 22) : le32(bs + 36);
    data = le16(bs + 14) + bs[16] * spf + (le16(bs + 17) * 32 + 511) / 512;
    clusters = total > data ? (total - data) / bs[13] : 0;
    label[0] = 0;
    if(le16(bs + 22) && bs[38] == 0x29) copy_label(label, bs + 43);
    else if(!le16(bs + 22) && bs[66] == 0x29) copy_label(label, bs + 71);
    brelse(b);
    snprintf(buf, n, "FAT%i%s%s", clusters < 4085 ? 12 : clusters < 65525 ? 16 : 32, label[0] ? ", label " : "", label);
    return 0;
}

/* a label for people: trailing spaces gone, "NO NAME" is none, anything unusual a '_' */
static void copy_label(char *out, const u8 *raw) {
    int i, n = 11;
    while(n && raw[n - 1] == ' ') n--;
    for(i = 0; i < n; i++) out[i] = (raw[i] >= 0x20 && raw[i] < 0x7F) ? raw[i] : '_';
    out[n] = 0;
    if(!strcmp(out, "NO NAME")) out[0] = 0;
}

/* the root directory's volume-label entry, which wins over the boot sector's copy */
static void read_root_label(struct fat_fs *fs, struct fat_node *root) {
    struct diter it;
    diter_init(&it, fs, root);
    while(!it.at_end) {
        struct buf *b = bread(fs->dev, it.lba);
        const u8 *p;
        if(!b) return;
        p = b->data + it.off;
        if(p[0] == 0) { brelse(b); return; }
        if(p[0] != 0xE5 && (p[11] & ATTR_LFN) != ATTR_LFN && (p[11] & ATTR_LABEL)) {
            copy_label(fs->label, p);
            brelse(b);
            return;
        }
        brelse(b);
        if(diter_next(&it)) return;
    }
}

/* the volume's state reaches the disk: FSInfo, then the dirty blocks */
static int fat_sync(struct superblock *sb) {
    struct fat_fs *fs = (struct fat_fs *)sb->priv;
    int r = 0;
    if(fs->dev->gone) { bsync(sb->dev); return -EIO; }       /* drops what can no longer be written */
    if(!sb->readonly) {
        mutex_lock(&sb->lock);
        r = write_fsinfo(fs);
        mutex_unlock(&sb->lock);
    }
    if(bsync(sb->dev)) r = -EIO;
    return r;
}

/* after the VFS has synced: mark the volume clean once everything else is on the disk */
static int fat_unmount(struct superblock *sb) {
    struct fat_fs *fs = (struct fat_fs *)sb->priv;
    if(fs->marked && !fs->dev->gone) {
        if(write_fsinfo(fs) == 0 && bsync(fs->dev) == 0 && fat_mark(fs, true) == 0 && bsync(fs->dev) == 0) bdev_flush(fs->dev);
        else kprintf("+FAT%i: %s: write errors, the volume stays marked as not cleanly unmounted\n", fs->type, fs->dev->name);
    }
    kfree(fs);
    return 0;
}

static int fat_statfs(struct superblock *sb, struct statfs *st) {
    struct fat_fs *fs = (struct fat_fs *)sb->priv;
    int r = 0;
    if(fs->dev->gone) return -EIO;
    mutex_lock(&sb->lock);
    if(fs->free == FREE_UNKNOWN) r = count_free(fs);         /* a read-only mount counts when asked */
    st->unit = fs->cluster_bytes;
    st->units = fs->nclusters;
    st->free_units = fs->free == FREE_UNKNOWN ? 0 : fs->free;
    snprintf(st->fs, sizeof(st->fs), "fat%i", fs->type);
    strlcpy(st->label, fs->label, sizeof(st->label));
    mutex_unlock(&sb->lock);
    return r;
}

static const struct superblock_ops fat_sops = { fat_sync, fat_unmount, fat_statfs };

static int fat_mount(struct superblock *sb) {
    struct fat_fs *fs;
    struct fat_node *root;
    struct buf *b;
    const u8 *bs;
    u32 tot16, tot32, spf16, spf32, fsi = 0, hint_free = FREE_UNKNOWN, hint_next = 0;
    u8 bpb_label[11];
    bool clean = true, trusted;
    const char *from = "counted";

    if(!sb->dev) return -ENODEV;
    if(sb->dev->gone) return -EIO;
    b = bread(sb->dev, 0);
    if(!b) return -EIO;
    bs = b->data;
    if(!bpb_sane(bs)) { brelse(b); return -EINVAL; }
    fs = (struct fat_fs *)kcalloc(1, sizeof(struct fat_fs));
    root = (struct fat_node *)kcalloc(1, sizeof(struct fat_node));
    if(!fs || !root) { brelse(b); kfree(fs); kfree(root); return -ENOMEM; }
    fs->sb = sb;
    fs->dev = sb->dev;
    fs->spc = bs[13];
    fs->rsv = le16(bs + 14);
    fs->nfats = bs[16];
    fs->root_entries = le16(bs + 17);
    tot16 = le16(bs + 19);
    spf16 = le16(bs + 22);
    tot32 = le32(bs + 32);
    spf32 = le32(bs + 36);
    fs->total = tot16 ? tot16 : tot32;
    fs->spf = spf16 ? spf16 : spf32;
    fs->root_cluster = spf16 ? 0 : le32(bs + 44);
    memset(bpb_label, ' ', 11);
    if(spf16 && bs[38] == 0x29) memcpy(bpb_label, bs + 43, 11);          /* extended boot record: FAT12/16 */
    else if(!spf16 && bs[66] == 0x29) memcpy(bpb_label, bs + 71, 11);    /* and FAT32 */
    if(!spf16) fsi = le16(bs + 48);
    brelse(b);

    if(fs->total > sb->dev->nblocks) fs->total = sb->dev->nblocks;
    fs->root_sectors = (fs->root_entries * 32 + 511) / 512;
    fs->fat_lba = fs->rsv;
    fs->root_lba = fs->rsv + fs->nfats * fs->spf;
    fs->data_lba = fs->root_lba + fs->root_sectors;
    if(!fs->spf || fs->data_lba >= fs->total) { kfree(fs); kfree(root); return -EINVAL; }
    fs->nclusters = (fs->total - fs->data_lba) / fs->spc;
    fs->type = fs->nclusters < 4085 ? 12 : fs->nclusters < 65525 ? 16 : 32;
    if(fs->type == 32 && fs->root_cluster < 2) { kfree(fs); kfree(root); return -EINVAL; }
    fs->cluster_bytes = fs->spc * 512;
    fs->eoc = fs->type == 12 ? 0xFFF : fs->type == 16 ? 0xFFFF : 0x0FFFFFFF;
    fs->next_free = 2;
    fs->free = FREE_UNKNOWN;
    copy_label(fs->label, bpb_label);

    if(fs->type == 32 && fsi >= 1 && fsi < fs->rsv && (b = bread(sb->dev, fsi)) != NULL) {   /* FSInfo */
        if(le32(b->data) == FSINFO_LEAD && le32(b->data + 484) == FSINFO_STRUCT && le32(b->data + 508) == FSINFO_TRAIL) {
            fs->fsinfo = fsi;
            hint_free = le32(b->data + 488);
            hint_next = le32(b->data + 492);
        }
        brelse(b);
    }
    if(fs->type != 12 && (b = bread(sb->dev, fs->fat_lba)) != NULL) {     /* the clean-shutdown bit */
        clean = fs->type == 32 ? (le32(b->data + 4) & FAT32_CLEAN) != 0 : (le16(b->data + 2) & FAT16_CLEAN) != 0;
        brelse(b);
    }
    root->is_root = true;
    root->first = fs->type == 32 ? fs->root_cluster : 0;
    sb->priv = fs;
    read_root_label(fs, root);

    if(!clean) kprintf("+FAT%i: %s was not unmounted cleanly (check it on another computer if files look wrong)\n",
                       fs->type, fs->dev->name);
    if(!sb->readonly) {                                      /* mounted read-write: dirty until unmounted */
        if(fat_mark(fs, false) != 0 || bsync(fs->dev) != 0) {
            binvalidate(fs->dev);                            /* nothing of this attempt is written later */
            kfree(fs); kfree(root); sb->priv = NULL;
            return -EIO;
        }
        fs->marked = true;
    }
    trusted = fs->fsinfo && clean && hint_free <= fs->nclusters;
    if(hint_next >= 2 && hint_next < fs->nclusters + 2) fs->next_free = hint_next;
    if(trusted) { fs->free = hint_free; from = "FSInfo"; }
    else if(!sb->readonly && count_free(fs) != 0) { kfree(fs); kfree(root); sb->priv = NULL; return -EIO; }
    if(fs->free == FREE_UNKNOWN) from = "not counted yet";

    sb->ops = &fat_sops;
    sb->root = vnode_alloc(sb, &fat_vops, VN_DIR);
    if(!sb->root) { kfree(fs); kfree(root); sb->priv = NULL; return -ENOMEM; }
    sb->root->priv = root;
    kprintf("+FAT%i: %s: %s%s%s%lu clusters of %lu bytes, %lu FAT sectors x %lu, data at block %lu; ", fs->type, fs->dev->name,
            fs->label[0] ? "label " : "", fs->label, fs->label[0] ? ", " : "", (u_long)fs->nclusters, (u_long)fs->cluster_bytes,
            (u_long)fs->spf, (u_long)fs->nfats, (u_long)fs->data_lba);
    if(fs->free == FREE_UNKNOWN) kprintf("free space %s\n", from);
    else kprintf("%lu free (%s)\n", (u_long)fs->free, from);
    return 0;
}

static const struct fs_type fat_type = { "fat", fat_probe, fat_mount, NULL };

void fatfs_init(void) {
    vfs_register(&fat_type);
}
