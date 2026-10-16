/*
SurfBoot: MBR and FAT32, read only
--------------------
File: fat32.c Date: 10/15/26 (SurfCPU)
--------------------
See fat32.h. The FAT type is decided as Microsoft's FAT specification says, by the count
of data clusters (fewer than 65525 is not FAT32 whatever the boot sector claims), and the
volume limits checked at mount time (volume inside the partition, partition inside the
card, FAT large enough for every cluster, root cluster in range) bound every later
calculation, so no sector number can overflow or leave the volume. On-disk fields are read
a byte at a time: SurfCPU traps on misaligned loads.
*/
#include <string.h>
#include "fat32.h"
#include "sberr.h"

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int is_exfat(const uint8_t *b) { return memcmp(b + 3, "EXFAT   ", 8) == 0; }
static int is_ntfs(const uint8_t *b) { return memcmp(b + 3, "NTFS    ", 8) == 0; }

static int looks_like_boot_sector(const uint8_t *b)
{
    if (b[0] != 0xEB && b[0] != 0xE9)
        return 0;
    uint32_t bps = rd16(b + 11);
    if (bps != 512 && bps != 1024 && bps != 2048 && bps != 4096)
        return 0;
    uint8_t spc = b[13];
    return spc != 0 && (spc & (spc - 1)) == 0 && b[16] != 0;
}

const char *fat_part_type_name(uint8_t type)
{
    switch (type) {
    case 0x00: return "empty";
    case 0x01: return "FAT12";
    case 0x04: case 0x06: case 0x0E: return "FAT16";
    case 0x05: case 0x0F: return "extended";
    case 0x07: return "exFAT/NTFS";
    case 0x0B: return "FAT32 (CHS)";
    case 0x0C: return "FAT32 (LBA)";
    case 0x83: return "Linux";
    case 0xAF: return "macOS HFS+";
    case 0xEE: return "GPT protective";
    case 0xEF: return "EFI system";
    default: return "other";
    }
}

int fat_read_mbr(fat_read_fn read, void *ctx, uint8_t b[512], struct mbr_part p[4])
{
    int e = read(ctx, 0, b);
    if (e)
        return e;
    if (b[510] != 0x55 || b[511] != 0xAA)
        return sb_fail(SB_E_MBR, "sector 0 has no partition table signature (55 aa expected, found "
                       "%02x %02x): a blank or unformatted card?", b[510], b[511]);
    if (is_exfat(b))
        return sb_fail(SB_E_EXFAT, "the card is formatted exFAT without a partition table; SurfBoot "
                       "needs FAT32 in an MBR partition (docs/CARD.md)");
    int valid = 1, used = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t *q = b + 446 + 16 * i;
        p[i].status = q[0];
        p[i].type = q[4];
        p[i].lba = rd32(q + 8);
        p[i].sectors = rd32(q + 12);
        if (q[0] != 0x00 && q[0] != 0x80)
            valid = 0;
        if (q[4] != 0)
            used = 1;
    }
    if (!valid || !used) {
        if (looks_like_boot_sector(b))
            return sb_fail(SB_E_SUPERFLOPPY, "sector 0 is a FAT boot sector, not a partition table: "
                           "the card has no MBR partition table (docs/CARD.md formats it with one)");
        if (!valid)
            return sb_fail(SB_E_MBR, "the partition table is invalid (status bytes %02x %02x %02x %02x)",
                           p[0].status, p[1].status, p[2].status, p[3].status);
        return sb_fail(SB_E_NO_FAT32_PART, "the partition table is empty");
    }
    return SB_OK;
}

int fat_mount(struct fat_vol *v, fat_read_fn read, void *ctx, uint32_t dev_sectors)
{
    memset(v, 0, sizeof *v);
    v->read = read;
    v->ctx = ctx;
    v->dev_sectors = dev_sectors;
    uint8_t *b = v->sec;
    struct mbr_part p[4];
    int e = fat_read_mbr(read, ctx, b, p);
    if (e)
        return e;

    int idx = -1;
    for (int i = 0; i < 4 && idx < 0; i++)
        if (p[i].type == 0x0B || p[i].type == 0x0C)
            idx = i;
    if (idx < 0) {
        for (int i = 0; i < 4; i++)
            if (p[i].type == 0xEE)
                return sb_fail(SB_E_GPT, "the card has a GPT partition table (protective MBR, type 0xee); "
                               "SurfBoot reads MBR partition tables only (docs/CARD.md)");
        for (int i = 0; i < 4; i++) {
            if (p[i].type != 0x07 || p[i].lba == 0 || (dev_sectors && p[i].lba >= dev_sectors))
                continue;
            if (read(ctx, p[i].lba, b) == SB_OK) {
                if (is_exfat(b))
                    return sb_fail(SB_E_EXFAT, "partition %d is exFAT; SurfBoot needs FAT32 (cards over "
                                   "32 GB come formatted exFAT: docs/CARD.md)", i + 1);
                if (is_ntfs(b))
                    return sb_fail(SB_E_NTFS, "partition %d is NTFS; SurfBoot needs FAT32", i + 1);
            }
        }
        for (int i = 0; i < 4; i++)
            if (p[i].type == 0x01 || p[i].type == 0x04 || p[i].type == 0x06 || p[i].type == 0x0E)
                return sb_fail(SB_E_FAT1216, "partition %d has type 0x%02x (%s); SurfBoot needs FAT32 "
                               "(type 0x0b or 0x0c)", i + 1, p[i].type, fat_part_type_name(p[i].type));
        return sb_fail(SB_E_NO_FAT32_PART, "no FAT32 partition (type 0x0b or 0x0c); the types are "
                       "%02x %02x %02x %02x", p[0].type, p[1].type, p[2].type, p[3].type);
    }

    v->part_index = idx;
    v->part_type = p[idx].type;
    v->part_lba = p[idx].lba;
    v->part_sectors = p[idx].sectors;
    if (v->part_lba == 0 || v->part_sectors == 0)
        return sb_fail(SB_E_PART_RANGE, "partition %d starts at sector %u with %u sectors", idx + 1,
                       v->part_lba, v->part_sectors);
    if ((uint64_t)v->part_lba + v->part_sectors > 0x100000000ull)
        return sb_fail(SB_E_PART_RANGE, "partition %d ends beyond sector 2^32", idx + 1);
    if (dev_sectors && v->part_lba + v->part_sectors > dev_sectors)
        return sb_fail(SB_E_PART_RANGE, "partition %d ends at sector %u, beyond the card's %u sectors",
                       idx + 1, v->part_lba + v->part_sectors - 1, dev_sectors);

    e = read(ctx, v->part_lba, b);
    if (e)
        return e;
    if (is_exfat(b))
        return sb_fail(SB_E_EXFAT, "partition %d (type 0x%02x) holds exFAT; SurfBoot needs FAT32",
                       idx + 1, v->part_type);
    if (is_ntfs(b))
        return sb_fail(SB_E_NTFS, "partition %d (type 0x%02x) holds NTFS; SurfBoot needs FAT32",
                       idx + 1, v->part_type);
    if (b[510] != 0x55 || b[511] != 0xAA)
        return sb_fail(SB_E_BPB, "the boot sector of partition %d (sector %u) has no 55 aa signature",
                       idx + 1, v->part_lba);
    if (b[0] != 0xEB && b[0] != 0xE9)
        return sb_fail(SB_E_BPB, "the boot sector does not start with a jump instruction (0x%02x)", b[0]);

    uint32_t bps = rd16(b + 11);
    uint32_t spc = b[13];
    uint32_t rsvd = rd16(b + 14);
    uint32_t nfats = b[16];
    uint32_t root_ent = rd16(b + 17);
    uint32_t tot16 = rd16(b + 19);
    uint32_t fatsz16 = rd16(b + 22);
    uint32_t tot32 = rd32(b + 32);
    uint32_t fatsz32 = rd32(b + 36);
    if (bps != 512)
        return sb_fail(SB_E_BPB, "%u-byte sectors: SurfBoot supports 512-byte sectors only", bps);
    if (spc == 0 || (spc & (spc - 1)) != 0)
        return sb_fail(SB_E_BPB, "%u sectors per cluster is not a power of two", spc);
    if (rsvd == 0)
        return sb_fail(SB_E_BPB, "the boot sector says there are no reserved sectors");
    if (nfats < 1 || nfats > 2)
        return sb_fail(SB_E_BPB, "%u FATs (1 or 2 expected)", nfats);
    uint32_t fatsz = fatsz16 ? fatsz16 : fatsz32;
    uint32_t totsec = tot16 ? tot16 : tot32;
    if (fatsz == 0 || totsec == 0)
        return sb_fail(SB_E_BPB, "FAT size %u and volume size %u: neither may be 0", fatsz, totsec);
    uint32_t rootdir_sectors = (root_ent * 32u + 511u) / 512u;
    uint64_t meta = rsvd + (uint64_t)nfats * fatsz + rootdir_sectors;
    if (meta >= totsec)
        return sb_fail(SB_E_BPB, "the reserved sectors and FATs (%u sectors) leave no room for data in "
                       "a %u-sector volume", (uint32_t)(meta > 0xFFFFFFFFu ? 0xFFFFFFFFu : meta), totsec);
    uint32_t clusters = (totsec - (uint32_t)meta) / spc;    /* meta < totsec: fits 32 bits */
    if (clusters < 4085)
        return sb_fail(SB_E_FAT1216, "the volume is FAT12 (%u clusters); SurfBoot needs FAT32", clusters);
    if (clusters < 65525)
        return sb_fail(SB_E_FAT1216, "the volume is FAT16 (%u clusters); SurfBoot needs FAT32, which "
                       "has at least 65525 clusters", clusters);
    if (root_ent != 0 || tot16 != 0 || fatsz16 != 0)
        return sb_fail(SB_E_BPB, "a FAT32-sized volume with FAT16 fields set (root entries %u, 16-bit "
                       "size %u, 16-bit FAT size %u)", root_ent, tot16, fatsz16);
    uint32_t fsver = rd16(b + 42);
    if (fsver != 0)
        return sb_fail(SB_E_BPB, "FAT32 version %u.%u (only 0.0 exists)", fsver >> 8, fsver & 0xFFu);
    if (totsec > v->part_sectors)
        return sb_fail(SB_E_BPB, "the volume (%u sectors) is larger than its partition (%u sectors)",
                       totsec, v->part_sectors);
    if (clusters > 0x0FFFFFF5u)
        return sb_fail(SB_E_BPB, "%u clusters: more than FAT32 can number", clusters);
    if ((uint64_t)fatsz32 * 128u < (uint64_t)clusters + 2u)
        return sb_fail(SB_E_BPB, "the FAT (%u sectors) is too small for %u clusters", fatsz32, clusters);
    uint32_t root = rd32(b + 44);
    if (root < 2 || root > clusters + 1)
        return sb_fail(SB_E_BPB, "the root directory starts at cluster %u, outside 2..%u", root,
                       clusters + 1);
    uint32_t ext = rd16(b + 40);
    uint32_t active = (ext & 0x80u) ? (ext & 0x0Fu) : 0;
    if (active >= nfats)
        return sb_fail(SB_E_BPB, "the boot sector makes FAT %u active, but there are %u", active, nfats);

    v->spc = spc;
    v->cluster_bytes = spc * 512u;
    v->reserved = rsvd;
    v->nfats = nfats;
    v->fat_sectors = fatsz32;
    v->total_sectors = totsec;
    v->root_cluster = root;
    v->active_fat = active;
    v->fat_lba = v->part_lba + rsvd + active * fatsz32;
    v->data_lba = v->part_lba + rsvd + nfats * fatsz32;
    v->clusters = clusters;
    v->max_cluster = clusters + 1;
    memcpy(v->oem, b + 3, 8);
    v->oem[8] = 0;
    if (b[66] == 0x29) {
        v->serial = rd32(b + 67);
        memcpy(v->label, b + 71, 11);
        int n = 11;
        while (n > 0 && v->label[n - 1] == ' ')
            n--;
        v->label[n] = 0;
    }
    v->mounted = 1;
    return SB_OK;
}

uint32_t fat_cluster_lba(const struct fat_vol *v, uint32_t cluster)
{
    return v->data_lba + (cluster - 2u) * v->spc;
}

int fat_next(struct fat_vol *v, uint32_t cl, uint32_t *next)
{
    if (cl < 2 || cl > v->max_cluster)
        return sb_fail(SB_E_FAT_CHAIN, "cluster %u is outside 2..%u", cl, v->max_cluster);
    uint32_t lba = v->fat_lba + cl / 128u;
    if (!v->fat_cache_valid || v->fat_cache_lba != lba) {
        v->fat_cache_valid = 0;
        int e = v->read(v->ctx, lba, v->fat_cache);
        if (e)
            return e;
        v->fat_cache_lba = lba;
        v->fat_cache_valid = 1;
    }
    uint32_t val = rd32(v->fat_cache + (cl % 128u) * 4u) & 0x0FFFFFFFu;
    if (val >= 0x0FFFFFF8u) {
        *next = FAT_EOC;
        return SB_OK;
    }
    if (val == 0x0FFFFFF7u)
        return sb_fail(SB_E_FAT_CHAIN, "cluster %u is in a chain but its FAT entry marks it bad", cl);
    if (val < 2)
        return sb_fail(SB_E_FAT_CHAIN, "the FAT entry of cluster %u points to %s cluster %u", cl,
                       val == 0 ? "the free" : "the reserved", val);
    if (val > v->max_cluster)
        return sb_fail(SB_E_FAT_CHAIN, "the FAT entry of cluster %u points to cluster %u, beyond the "
                       "last cluster %u", cl, val, v->max_cluster);
    *next = val;
    return SB_OK;
}

void fat_chain_start(struct fat_chain *w, uint32_t first, uint32_t limit)
{
    w->cur = w->saved = first;
    w->power = w->lam = 1;
    w->steps = 1;
    w->limit = limit;
}

int fat_chain_step(struct fat_vol *v, struct fat_chain *w, uint32_t *out)
{
    uint32_t next;
    int e = fat_next(v, w->cur, &next);
    if (e)
        return e;
    if (next == FAT_EOC) {
        *out = FAT_EOC;
        return SB_OK;
    }
    if (++w->steps > w->limit)
        return sb_fail(SB_E_FAT_CHAIN, "the cluster chain is longer than %u clusters", w->limit);
    if (next == w->saved)
        return sb_fail(SB_E_FAT_CYCLE, "the cluster chain loops: cluster %u comes round again after "
                       "%u more clusters", next, w->lam);
    if (w->power == w->lam) {                       /* Brent: move the saved cluster up */
        w->saved = next;
        w->power <<= 1;
        w->lam = 0;
    }
    w->lam++;
    w->cur = next;
    *out = next;
    return SB_OK;
}

int fat_dir_scan(struct fat_vol *v, uint32_t dir, fat_dir_fn fn, void *ctx)
{
    if (dir < 2 || dir > v->max_cluster)
        return sb_fail(SB_E_DIR, "a directory starts at cluster %u, outside 2..%u", dir, v->max_cluster);
    uint32_t limit = (FAT_MAX_DIR_ENTRIES * 32u + v->cluster_bytes - 1u) / v->cluster_bytes;
    if (limit > v->clusters)
        limit = v->clusters;
    struct fat_chain w;
    fat_chain_start(&w, dir, limit);
    uint32_t cl = dir;
    for (;;) {
        uint32_t lba = fat_cluster_lba(v, cl);
        for (uint32_t s = 0; s < v->spc; s++) {
            int e = v->read(v->ctx, lba + s, v->sec);
            if (e)
                return e;
            for (unsigned off = 0; off < 512u; off += 32u) {
                const uint8_t *q = v->sec + off;
                if (q[0] == 0x00)
                    return SB_OK;                   /* end of the directory */
                if (q[0] == 0xE5 || (q[11] & 0x3Fu) == FAT_ATTR_LFN)
                    continue;                       /* deleted, or part of a long name */
                struct fat_dirent d;
                memcpy(d.name, q, 11);
                if ((uint8_t)d.name[0] == 0x05)
                    d.name[0] = (char)0xE5;
                d.attr = q[11];
                d.ntres = q[12];
                d.cluster = (rd16(q + 20) << 16) | rd16(q + 26);
                d.size = rd32(q + 28);
                if (fn(ctx, &d))
                    return SB_OK;
            }
        }
        int e = fat_chain_step(v, &w, &cl);
        if (e) {
            if (sb_last_err == SB_E_FAT_CHAIN && w.steps > w.limit)
                return sb_fail(SB_E_DIR, "a directory is longer than %u clusters (FAT allows %u entries)",
                               w.limit, FAT_MAX_DIR_ENTRIES);
            return e;
        }
        if (cl == FAT_EOC)
            return SB_OK;
    }
}

static int valid_83_char(unsigned char c)
{
    if (c <= 0x20 || c == 0x7F)
        return 0;
    if (c >= 0x80)
        return 1;
    return strchr("\"*+,./:;<=>?[\\]|", c) == 0;
}

int fat_to_83(const char *s, unsigned n, char out[11])
{
    memset(out, ' ', 11);
    unsigned i = 0, k = 0;
    for (; i < n && s[i] != '.'; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'a' && c <= 'z')
            c = (unsigned char)(c - 'a' + 'A');
        if (k >= 8 || !valid_83_char(c))
            return -1;
        out[k++] = (char)c;
    }
    if (k == 0)
        return -1;
    if (i < n) {
        i++;                                        /* the dot */
        unsigned x = 0;
        for (; i < n; i++) {
            unsigned char c = (unsigned char)s[i];
            if (c >= 'a' && c <= 'z')
                c = (unsigned char)(c - 'a' + 'A');
            if (x >= 3 || !valid_83_char(c))
                return -1;
            out[8 + x++] = (char)c;
        }
        if (x == 0)
            return -1;
    }
    if ((unsigned char)out[0] == 0xE5)
        out[0] = 0x05;                              /* as stored on disk */
    return 0;
}

void fat_83_to_str(const char name[11], uint8_t ntres, char out[13])
{
    int n = 0;
    for (int i = 0; i < 8 && name[i] != ' '; i++) {
        char c = (i == 0 && (uint8_t)name[0] == 0x05) ? (char)0xE5 : name[i];
        if ((ntres & 0x08u) && c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        out[n++] = c;
    }
    if (name[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && name[i] != ' '; i++) {
            char c = name[i];
            if ((ntres & 0x10u) && c >= 'A' && c <= 'Z')
                c = (char)(c - 'A' + 'a');
            out[n++] = c;
        }
    }
    out[n] = 0;
}

struct find_ctx {
    char name[11];
    struct fat_dirent *out;
    int found;
};

static int find_cb(void *p, const struct fat_dirent *d)
{
    struct find_ctx *f = p;
    if (d->attr & FAT_ATTR_VOL)
        return 0;
    if (memcmp(d->name, f->name, 11) != 0)
        return 0;
    *f->out = *d;
    f->found = 1;
    return 1;
}

int fat_lookup(struct fat_vol *v, const char *path, struct fat_dirent *out)
{
    const char *p = path;
    uint32_t dir = v->root_cluster;
    while (*p == '/')
        p++;
    memset(out, 0, sizeof *out);
    memset(out->name, ' ', 11);
    out->name[0] = '/';
    out->attr = FAT_ATTR_DIR;
    out->cluster = v->root_cluster;
    while (*p) {
        const char *end = p;
        while (*end && *end != '/')
            end++;
        struct find_ctx f;
        if (fat_to_83(p, (unsigned)(end - p), f.name) != 0)
            return sb_fail(SB_E_BAD_PATH, "\"%.*s\" is not an 8.3 name (SurfBoot reads short names "
                           "only)", (int)(end - p), p);
        f.out = out;
        f.found = 0;
        int e = fat_dir_scan(v, dir, find_cb, &f);
        if (e)
            return e;
        if (!f.found)
            return sb_fail(SB_E_NOT_FOUND, "%.*s does not exist", (int)(end - path), path);
        p = end;
        while (*p == '/')
            p++;
        if (*p) {
            if (!(out->attr & FAT_ATTR_DIR))
                return sb_fail(SB_E_NOT_DIR, "%.*s is a file, not a directory", (int)(end - path), path);
            dir = out->cluster ? out->cluster : v->root_cluster;    /* ".." of a top directory is 0 */
        }
    }
    return SB_OK;
}

int fat_open(struct fat_vol *v, const struct fat_dirent *d, struct fat_file *f)
{
    memset(f, 0, sizeof *f);
    if (d->attr & (FAT_ATTR_DIR | FAT_ATTR_VOL))
        return sb_fail(SB_E_NOT_FILE, "it is a directory, not a file");
    f->d = *d;
    if (d->size == 0)
        return SB_OK;
    if (d->cluster < 2 || d->cluster > v->max_cluster)
        return sb_fail(SB_E_FAT_CHAIN, "its directory entry gives first cluster %u, outside 2..%u",
                       d->cluster, v->max_cluster);
    f->nclusters = (d->size - 1u) / v->cluster_bytes + 1u;
    if (f->nclusters > v->clusters)
        return sb_fail(SB_E_FAT_CHAIN, "its size (%u bytes) needs more clusters than the volume has",
                       d->size);
    fat_chain_start(&f->walk, d->cluster, f->nclusters);
    f->cluster = d->cluster;
    f->fragments = 1;
    return SB_OK;
}

int fat_read_sector(struct fat_vol *v, struct fat_file *f, uint8_t *buf, uint32_t *len)
{
    *len = 0;
    if (f->pos >= f->d.size)
        return SB_OK;
    if (f->sector == v->spc) {
        uint32_t next;
        int e = fat_chain_step(v, &f->walk, &next);
        if (e)
            return e;
        if (next == FAT_EOC)
            return sb_fail(SB_E_FAT_CHAIN, "its cluster chain ends after %u clusters, but %u bytes need "
                           "%u", f->walk.steps, f->d.size, f->nclusters);
        if (next != f->cluster + 1u)
            f->fragments++;
        f->cluster = next;
        f->sector = 0;
    }
    int e = v->read(v->ctx, fat_cluster_lba(v, f->cluster) + f->sector, buf);
    if (e)
        return e;
    f->sector++;
    uint32_t n = f->d.size - f->pos;
    if (n > 512u)
        n = 512u;
    f->pos += n;
    *len = n;
    if (f->pos == f->d.size) {                      /* the chain must end exactly here */
        uint32_t next;
        e = fat_next(v, f->cluster, &next);
        if (e)
            return e;
        if (next != FAT_EOC)
            return sb_fail(SB_E_FAT_CHAIN, "its cluster chain goes on after the last cluster %u (to %u): "
                           "a loop, or a size that does not match the chain", f->cluster, next);
    }
    return SB_OK;
}
