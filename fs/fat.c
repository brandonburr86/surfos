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
no extended partition walking, ASCII names only.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <sys/bdev.h>
#include <fs/vfs.h>
#include <fs/bcache.h>
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

struct fat_fs {
    struct superblock *sb;
    struct bdev *dev;
    int type;                       /* 12, 16 or 32 */
    u32 spc, rsv, nfats, root_entries, spf, total;
    u32 fat_lba, root_lba, root_sectors, data_lba, nclusters, root_cluster;
    u32 cluster_bytes, eoc, next_free;
};

struct fat_node {
    u32 first;                      /* first cluster; 0 = empty file, or the FAT12/16 root */
    u32 dir_lba, dir_off;           /* where the short entry lives (not for the root) */
    bool is_root;
};

/* one logical directory entry as the iterator returns it */
struct fat_dirent {
    char name[VFS_NAME_MAX + 1];
    u8 raw[32];                     /* the short entry */
    u32 first, size;
    u32 lba, off;                   /* short entry location */
    u32 idx, lfn_idx;               /* entry indexes: the short entry and the first LFN entry */
};

/* position inside a directory */
struct diter {
    struct fat_fs *fs;
    u32 first;                      /* 0: the fixed FAT12/16 root */
    u32 idx, clus, lba, off;
    bool at_end;
};

static const struct vnode_ops fat_vops;

/**** little helpers ****/

static u16 le16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 le32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static void put16(u8 *p, u16 v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void put32(u8 *p, u32 v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = v >> 24; }
static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int upper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

static bool name_eq(const char *a, const char *b) {
    while(*a && *b) { if(lower(*a) != lower(*b)) return false; a++; b++; }
    return *a == *b;
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
    b = bread(fs->dev, lba);
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
        u32 lba = fs->fat_lba + i * fs->spf + off / 512;
        struct buf *b = bread(fs->dev, lba), *b2 = NULL;
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

/* a free cluster, zeroed, chained after prev (0: a new chain); returns the cluster or 0 */
static u32 fat_alloc(struct fat_fs *fs, u32 prev) {
    u32 n, c = fs->next_free, v;
    for(n = 0; n < fs->nclusters; n++, c++) {
        if(c >= fs->nclusters + 2) c = 2;
        if(fat_get(fs, c, &v) != 0) return 0;
        if(v == 0) {
            if(fat_set(fs, c, fs->eoc) != 0 || zero_cluster(fs, c) != 0) return 0;
            if(prev && fat_set(fs, prev, c) != 0) return 0;
            fs->next_free = c + 1;
            return c;
        }
    }
    return 0;
}

static int fat_free_chain(struct fat_fs *fs, u32 c) {
    u32 n = 0, next;
    while(!is_eoc(fs, c) && n++ < fs->nclusters) {
        if(fat_get(fs, c, &next) != 0) return -EIO;
        if(fat_set(fs, c, 0) != 0) return -EIO;
        if(c < fs->next_free) fs->next_free = c;
        c = next;
    }
    return 0;
}

/**** directory iteration ****/

static void diter_init(struct diter *it, struct fat_fs *fs, struct fat_node *dir) {
    it->fs = fs;
    it->first = (dir->is_root && fs->type != 32) ? 0 : dir->first;
    it->idx = 0;
    it->clus = it->first;
    it->off = 0;
    if(it->first) { it->lba = clus_lba(fs, it->first); it->at_end = it->first < 2; }
    else { it->lba = fs->root_lba; it->at_end = fs->root_sectors == 0; }
}

/* step to the next entry; at_end becomes true when the directory's space is used up */
static int diter_next(struct diter *it) {
    struct fat_fs *fs = it->fs;
    it->idx++;
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
    if(!it->first) return -ENOSPC;                          /* the FAT12/16 root cannot grow */
    c = fat_alloc(fs, it->clus);
    if(!c) return -ENOSPC;
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

static void short_to_name(const u8 *raw, char *out) {
    int n = 0, i, base = 8, ext = 3;
    while(base && raw[base - 1] == ' ') base--;
    while(ext && raw[8 + ext - 1] == ' ') ext--;
    for(i = 0; i < base; i++) out[n++] = (raw[12] & NTRES_LOWER_BASE) ? lower(raw[i]) : raw[i];
    if(out[0] == 0x05) out[0] = (char)0xE5;
    if(ext) {
        out[n++] = '.';
        for(i = 0; i < ext; i++) out[n++] = (raw[12] & NTRES_LOWER_EXT) ? lower(raw[8 + i]) : raw[8 + i];
    }
    out[n] = 0;
}

/* the next logical entry (long name resolved, free and label entries skipped): 0, 1 at the end, < 0 */
static int dir_read(struct diter *it, struct fat_dirent *e) {
    char lfn[LFN_MAX_ENTRIES * LFN_CHARS + 1];
    u32 lfn_start = 0;
    int lfn_expected = 0, lfn_seen = 0;
    u8 lfn_sum = 0;

    for(;;) {
        struct buf *b;
        u8 *p;
        int r;
        if(it->at_end) return 1;
        b = bread(it->fs->dev, it->lba);
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
                for(k = 0; k < LFN_CHARS; k++) {
                    u16 ch = le16(p + pos[k]);
                    lfn[(seq - 1) * LFN_CHARS + k] = (ch == 0 || ch == 0xFFFF) ? 0 : (ch < 256 ? (char)ch : '_');
                }
                lfn_seen++;
            } else lfn_expected = 0;
        } else if(p[11] & ATTR_LABEL) {
            lfn_expected = lfn_seen = 0;
        } else {
            memcpy(e->raw, p, 32);
            e->lba = it->lba;
            e->off = it->off;
            e->idx = it->idx;
            if(lfn_expected && lfn_seen == lfn_expected && lfn_checksum(p) == lfn_sum && lfn[0]) {
                strlcpy(e->name, lfn, sizeof(e->name));
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

/* write n raw entries at pos (the space was found by dir_find_free) */
static int dir_write_entries(struct diter *pos, const u8 *entries, u32 n) {
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

static int validate_name(const char *name) {
    size_t n = strlen(name);
    size_t i;
    if(!n) return -EINVAL;
    if(n > VFS_NAME_MAX) return -ENAMETOOLONG;
    if(!strcmp(name, ".") || !strcmp(name, "..")) return -EINVAL;
    for(i = 0; i < n; i++) if(!legal_char((u8)name[i])) return -EINVAL;
    if(name[n - 1] == ' ' || name[n - 1] == '.') return -EINVAL;
    return 0;
}

static bool short_char_ok(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("$%'-_@~`!(){}^#&", c) != NULL;
}

static bool dir_has_short(struct fat_fs *fs, struct fat_node *dir, const u8 *raw11) {
    struct diter it;
    struct fat_dirent e;
    diter_init(&it, fs, dir);
    while(dir_read(&it, &e) == 0) if(!memcmp(e.raw, raw11, 11)) return true;
    return false;
}

/* The 8.3 form of name in raw[11] with the lowercase flags in *ntres; returns true when
   long-name entries are needed as well (the name is not representable as 8.3). */
static bool make_short_name(struct fat_fs *fs, struct fat_node *dir, const char *name, u8 *raw, u8 *ntres) {
    const char *dot = strrchr(name, '.');
    size_t blen = dot ? (size_t)(dot - name) : strlen(name), elen = dot ? strlen(dot + 1) : 0;
    bool fits = true, base_lower = false, base_upper = false, ext_lower = false, ext_upper = false;
    size_t i, n;
    char base[9], ext[4];

    memset(raw, ' ', 11);
    *ntres = 0;
    if(dot == name) { fits = false; blen = 0; }              /* ".name" */
    if(blen > 8 || elen > 3 || (dot && !elen)) fits = false;
    for(i = 0; i < blen && fits; i++) {
        int c = (u8)name[i];
        if(c == '.' || c == ' ') fits = false;
        if(c >= 'a' && c <= 'z') base_lower = true;
        else if(c >= 'A' && c <= 'Z') base_upper = true;
        if(!short_char_ok(upper(c))) fits = false;
    }
    for(i = 0; i < elen && fits; i++) {
        int c = (u8)dot[1 + i];
        if(c == '.' || c == ' ') fits = false;
        if(c >= 'a' && c <= 'z') ext_lower = true;
        else if(c >= 'A' && c <= 'Z') ext_upper = true;
        if(!short_char_ok(upper(c))) fits = false;
    }
    if(fits && !(base_lower && base_upper) && !(ext_lower && ext_upper)) {
        for(i = 0; i < blen; i++) raw[i] = upper((u8)name[i]);
        for(i = 0; i < elen; i++) raw[8 + i] = upper((u8)dot[1 + i]);
        if(base_lower) *ntres |= NTRES_LOWER_BASE;
        if(ext_lower) *ntres |= NTRES_LOWER_EXT;
        if(raw[0] == 0xE5) raw[0] = 0x05;
        return false;
    }
    /* BASENA~1.EXT from the legal characters */
    for(i = 0, n = 0; i < blen && n < 6; i++) {
        int c = upper((u8)name[i]);
        if(c == ' ' || c == '.') continue;
        base[n++] = short_char_ok(c) ? c : '_';
    }
    if(!n) base[n++] = '_';
    base[n] = 0;
    for(i = 0, n = 0; dot && i < elen && n < 3; i++) {
        int c = upper((u8)dot[1 + i]);
        if(c == ' ' || c == '.') continue;
        ext[n++] = short_char_ok(c) ? c : '_';
    }
    ext[n] = 0;
    for(n = 1; n < 1000; n++) {
        char tail[8];
        size_t bl = strlen(base), tl;
        snprintf(tail, sizeof(tail), "~%u", (u_int)n);
        tl = strlen(tail);
        if(bl + tl > 8) bl = 8 - tl;
        memset(raw, ' ', 11);
        memcpy(raw, base, bl);
        memcpy(raw + bl, tail, tl);
        memcpy(raw + 8, ext, strlen(ext));
        if(!dir_has_short(fs, dir, raw)) break;
    }
    return true;
}

/* raw entries for a new name: the LFN entries (if any) followed by the short entry */
static u32 build_entries(struct fat_fs *fs, struct fat_node *dir, const char *name, u8 attr, u32 first, u32 size, u8 *out) {
    u8 raw[11], ntres, sum;
    bool lfn = make_short_name(fs, dir, name, raw, &ntres);
    u32 n = 0, nlfn = 0, k;
    u16 d, t;
    u8 *p;

    if(lfn) {
        size_t len = strlen(name);
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
                size_t ci = (k - 1) * LFN_CHARS + j;
                u16 ch = ci < len ? (u8)name[ci] : (ci == len ? 0 : 0xFFFF);
                put16(p + pos[j], ch);
            }
            n++;
        }
    }
    p = out + n * 32;
    memset(p, 0, 32);
    memcpy(p, raw, 11);
    p[11] = attr;
    p[12] = ntres;
    fat_now(&d, &t);
    put16(p + 14, t); put16(p + 16, d);                      /* created */
    put16(p + 18, d);                                        /* accessed */
    put16(p + 20, (u16)(fs->type == 32 ? first >> 16 : 0));
    put16(p + 22, t); put16(p + 24, d);                      /* modified */
    put16(p + 26, (u16)(first & 0xFFFF));
    put32(p + 28, size);
    return n + 1;
}

/**** vnodes ****/

static struct vnode *fat_vnode(struct superblock *sb, const struct fat_dirent *e) {
    struct fat_node *n = (struct fat_node *)kcalloc(1, sizeof(struct fat_node));
    struct vnode *v;
    if(!n) return NULL;
    v = vnode_alloc(sb, &fat_vops, (e->raw[11] & ATTR_DIR) ? VN_DIR : VN_FILE);
    if(!v) { kfree(n); return NULL; }
    n->first = e->first;
    n->dir_lba = e->lba;
    n->dir_off = e->off;
    v->size = (e->raw[11] & ATTR_DIR) ? 0 : e->size;
    v->mtime = fat_to_unix(le16(e->raw + 24), le16(e->raw + 22));
    v->ino = (e->lba << 4) | (e->off / 32);
    v->priv = n;
    return v;
}

static int fat_lookup(struct vnode *dir, const char *name, struct vnode **out) {
    struct fat_fs *fs = (struct fat_fs *)dir->sb->priv;
    struct fat_dirent e;
    int r;
    mutex_lock(&dir->sb->lock);
    r = dir_find(fs, (struct fat_node *)dir->priv, name, &e);
    if(r == 0) {
        *out = fat_vnode(dir->sb, &e);
        if(!*out) r = -ENOMEM;
    }
    mutex_unlock(&dir->sb->lock);
    return r;
}

static int fat_readdir(struct vnode *dir, u_int index, struct dirent *de) {
    struct fat_fs *fs = (struct fat_fs *)dir->sb->priv;
    struct diter it;
    struct fat_dirent e;
    int r;
    mutex_lock(&dir->sb->lock);
    diter_init(&it, fs, (struct fat_node *)dir->priv);
    while((r = dir_read(&it, &e)) == 0) {
        if(!strcmp(e.name, ".") || !strcmp(e.name, "..")) continue;
        if(index-- == 0) {
            strlcpy(de->name, e.name, sizeof(de->name));
            de->type = (e.raw[11] & ATTR_DIR) ? VN_DIR : VN_FILE;
            de->size = (e.raw[11] & ATTR_DIR) ? 0 : e.size;
            de->mtime = fat_to_unix(le16(e.raw + 24), le16(e.raw + 22));
            break;
        }
    }
    mutex_unlock(&dir->sb->lock);
    return r;
}

/* read or write len bytes at off; the chain grows as needed when writing */
static int fat_rw(struct vnode *v, u32 off, u8 *buf, u32 len, bool write) {
    struct fat_fs *fs = (struct fat_fs *)v->sb->priv;
    struct fat_node *n = (struct fat_node *)v->priv;
    u32 done = 0, c, ci, within, prev = 0;
    int r = 0;

    if(!write) {
        if(off >= v->size) return 0;
        if(len > v->size - off) len = v->size - off;
    }
    if(!len) return 0;
    if(write && off + len < off) return -EFBIG;
    mutex_lock(&v->sb->lock);
    ci = off / fs->cluster_bytes;
    within = off % fs->cluster_bytes;
    c = n->first;
    if(!c) {
        if(!write) { mutex_unlock(&v->sb->lock); return 0; }
        c = fat_alloc(fs, 0);
        if(!c) { mutex_unlock(&v->sb->lock); return -ENOSPC; }
        n->first = c;
    }
    while(ci--) {                                            /* walk to the cluster that holds off */
        u32 next;
        if(fat_get(fs, c, &next) != 0) { r = -EIO; goto out; }
        if(is_eoc(fs, next)) {
            if(!write) goto out;
            next = fat_alloc(fs, c);
            if(!next) { r = -ENOSPC; goto out; }
        }
        c = next;
    }
    while(done < len) {
        u32 s = within / 512, so = within % 512, chunk = 512 - so;
        struct buf *b;
        if(chunk > len - done) chunk = len - done;
        b = (write && chunk == 512) ? bget(fs->dev, clus_lba(fs, c) + s) : bread(fs->dev, clus_lba(fs, c) + s);
        if(!b) { r = -EIO; goto out; }
        if(write) { memcpy(b->data + so, buf + done, chunk); bdirty(b); }
        else memcpy(buf + done, b->data + so, chunk);
        brelse(b);
        done += chunk;
        within += chunk;
        if(within == fs->cluster_bytes && done < len) {
            u32 next;
            within = 0;
            prev = c;
            if(fat_get(fs, c, &next) != 0) { r = -EIO; goto out; }
            if(is_eoc(fs, next)) {
                if(!write) goto out;
                next = fat_alloc(fs, prev);
                if(!next) { r = -ENOSPC; goto out; }
            }
            c = next;
        }
    }
out:
    if(write && done) {
        if(off + done > v->size) v->size = off + done;
        if(update_entry(fs, n, v->size, true) != 0 && r == 0) r = -EIO;
    }
    mutex_unlock(&v->sb->lock);
    if(done) return (int)done;
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
    if(size > v->size) return -EINVAL;                       /* growing happens by writing */
    mutex_lock(&v->sb->lock);
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
    mutex_lock(&dir->sb->lock);
    r = dir_find(fs, dn, name, &e);
    if(r == 0) { r = -EEXIST; goto out; }
    if(r != -ENOENT) goto out;
    if(type == VN_DIR) {                                     /* a directory starts with "." and ".." */
        struct buf *b;
        u8 *p;
        u16 d, t;
        u32 parent = dn->is_root ? 0 : dn->first;
        first = fat_alloc(fs, 0);
        if(!first) { r = -ENOSPC; goto out; }
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
    n = build_entries(fs, dn, name, type == VN_DIR ? ATTR_DIR : ATTR_ARCHIVE, first, 0, entries);
    r = dir_find_free(fs, dn, n, &pos);
    if(r == 0) r = dir_write_entries(&pos, entries, n);
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

static int fat_unlink(struct vnode *dir, const char *name) {
    struct fat_fs *fs = (struct fat_fs *)dir->sb->priv;
    struct fat_node *dn = (struct fat_node *)dir->priv;
    struct fat_dirent e;
    int r;
    mutex_lock(&dir->sb->lock);
    r = dir_find(fs, dn, name, &e);
    if(r) goto out;
    if(e.raw[11] & ATTR_DIR) {                               /* only an empty directory goes */
        struct fat_node sub;
        struct diter it;
        struct fat_dirent x;
        memset(&sub, 0, sizeof(sub));
        sub.first = e.first;
        diter_init(&it, fs, &sub);
        while((r = dir_read(&it, &x)) == 0) {
            if(strcmp(x.name, ".") && strcmp(x.name, "..")) { r = -ENOTEMPTY; goto out; }
        }
        if(r < 0) goto out;
        r = 0;
    }
    if(e.first) r = fat_free_chain(fs, e.first);
    if(r == 0) r = dir_delete_entries(fs, dn, e.lfn_idx, e.idx);
out:
    mutex_unlock(&dir->sb->lock);
    return r;
}

static void fat_release(struct vnode *v) {
    kfree(v->priv);
}

static const struct vnode_ops fat_vops = {
    fat_lookup, fat_readdir, fat_read, fat_write, fat_truncate, fat_create, fat_unlink, fat_release
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

static int fat_sync(struct superblock *sb) {
    return bsync(sb->dev) ? -EIO : 0;
}

static int fat_unmount(struct superblock *sb) {
    kfree(sb->priv);
    return 0;
}

static const struct superblock_ops fat_sops = { fat_sync, fat_unmount };

static int fat_mount(struct superblock *sb) {
    struct fat_fs *fs;
    struct fat_node *root;
    struct buf *b;
    const u8 *bs;
    u32 tot16, tot32, spf16, spf32;

    if(!sb->dev) return -ENODEV;
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

    root->is_root = true;
    root->first = fs->type == 32 ? fs->root_cluster : 0;
    sb->priv = fs;
    sb->ops = &fat_sops;
    sb->root = vnode_alloc(sb, &fat_vops, VN_DIR);
    if(!sb->root) { kfree(fs); kfree(root); return -ENOMEM; }
    sb->root->priv = root;
    kprintf("+FAT%i: %s: %lu clusters of %lu bytes, %lu FAT sectors x %lu, data at block %lu\n", fs->type, fs->dev->name,
            (u_long)fs->nclusters, (u_long)fs->cluster_bytes, (u_long)fs->spf, (u_long)fs->nfats, (u_long)fs->data_lba);
    return 0;
}

static const struct fs_type fat_type = { "fat", fat_probe, fat_mount, NULL };

void fatfs_init(void) {
    vfs_register(&fat_type);
}
