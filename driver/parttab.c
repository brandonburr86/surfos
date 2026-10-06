/*
SurfOS Partition Tables
--------------------
File: parttab.c Date: 10/5/26 (USB storage)
--------------------
Reading MBR and GPT partition tables from 512-byte sectors handed over by a
callback, and planning changes to them. No I/O, no allocation and no kernel
calls: driver/part.c registers what this finds and writes what it plans, and
tests/host/test_part.c runs it on the host against tables built by hand and
by sfdisk and sgdisk. Everything read from the disk is checked before it is
used: a partition outside the disk, a looping chain of logical partitions or
a GPT whose CRC does not match is left out with a note instead of being
believed, and a table with anything left out is not changed.
*/

#include <surfos/types.h>
#include <sys/part.h>
#include <stdarg.h>
#include <blibc_common.h>

#define MBR_SIG_OFFSET   510
#define MBR_TABLE_OFFSET 446    /* four 16-byte entries */
#define MBR_DISK_SIG     440
#define GPT_HEADER_MIN   92

static u16 get16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 get32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static u64 get64(const u8 *p) { return (u64)get32(p) | ((u64)get32(p + 4) << 32); }

static void put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v) { put16(p, (u16)v); put16(p + 2, (u16)(v >> 16)); }
static void put64(u8 *p, u64 v) { put32(p, (u32)v); put32(p + 4, (u32)(v >> 32)); }

static void vnote(const struct part_io *io, const char *fmt, va_list ap) {
    char msg[160];
    if(!io->note) return;
    vsnprintf(msg, sizeof(msg), fmt, ap);
    io->note(io->arg, msg);
}

static void note(const struct part_io *io, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vnote(io, fmt, ap);
    va_end(ap);
}

/* an entry (or a whole chain) is left out: say why, and remember the table is not fully understood */
static void skip(const struct part_io *io, struct part_table *t, const char *fmt, ...) {
    va_list ap;
    t->skipped++;
    va_start(ap, fmt);
    vnote(io, fmt, ap);
    va_end(ap);
}

/**** CRC32: the reflected polynomial 0xEDB88320, four bits at a time ****/

static const u32 crc_nibble[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C
};

u32 part_crc32(u32 crc, const void *buf, size_t len) {
    const u8 *p = (const u8 *)buf;
    crc = ~crc;
    while(len--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ crc_nibble[crc & 15];
        crc = (crc >> 4) ^ crc_nibble[crc & 15];
    }
    return ~crc;
}

/**** names ****/

static const struct { u8 type; const char *name; } mbr_types[] = {
    { 0x01, "FAT12" },      { 0x04, "FAT16 <32M" }, { 0x05, "extended" },     { 0x06, "FAT16" },
    { 0x07, "NTFS/exFAT" }, { 0x0B, "FAT32" },      { 0x0C, "FAT32 LBA" },    { 0x0E, "FAT16 LBA" },
    { 0x0F, "extended LBA" }, { 0x27, "WinRE" },    { 0x82, "Linux swap" },   { 0x83, "Linux" },
    { 0x85, "Linux extended" }, { 0xAF, "HFS+" },   { 0xEE, "GPT protective" }, { 0xEF, "EFI system" },
};

const char *part_mbr_type_name(u8 type) {
    u_int i;
    for(i = 0; i < sizeof(mbr_types) / sizeof(mbr_types[0]); i++) if(mbr_types[i].type == type) return mbr_types[i].name;
    return NULL;
}

/* a GUID as it is stored: the first three groups little-endian, the last two as written */
#define GUID(a, b, c, d, e0, e1, e2, e3, e4, e5) { \
    (u8)(a), (u8)((a) >> 8), (u8)((a) >> 16), (u8)((a) >> 24), (u8)(b), (u8)((b) >> 8), \
    (u8)(c), (u8)((c) >> 8), (u8)((d) >> 8), (u8)(d), e0, e1, e2, e3, e4, e5 }

/* the GPT types worth a name; mbr is the type byte the same partition would have on an MBR disk
   (basic data covers FAT and NTFS alike: 0x07, as gdisk maps it) */
static const struct { u8 guid[16]; u8 mbr; const char *name, *brief; } gpt_types[] = {
    { GUID(0xC12A7328, 0xF81F, 0x11D2, 0xBA4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B), 0xEF, "EFI system", "EFI system" },
    { GUID(0xEBD0A0A2, 0xB9E5, 0x4433, 0x87C0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7), 0x07, "Microsoft basic data", "basic data" },
    { GUID(0xE3C9E316, 0x0B5C, 0x4DB8, 0x817D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE), 0x00, "Microsoft reserved", "MS reserved" },
    { GUID(0xDE94BBA4, 0x06D1, 0x4D40, 0xA16A, 0xBF, 0xD5, 0x01, 0x79, 0xD6, 0xAC), 0x27, "Windows recovery", "WinRE" },
    { GUID(0x0FC63DAF, 0x8483, 0x4772, 0x8E79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4), 0x83, "Linux filesystem", "Linux" },
    { GUID(0x0657FD6D, 0xA4AB, 0x43C4, 0x84E5, 0x09, 0x33, 0xC8, 0x4B, 0x4F, 0x4F), 0x82, "Linux swap", "Linux swap" },
    { GUID(0x7C3457EF, 0x0000, 0x11AA, 0xAA11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC), 0x00, "Apple APFS", "APFS" },
    { GUID(0x48465300, 0x0000, 0x11AA, 0xAA11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC), 0xAF, "Apple HFS+", "HFS+" },
    { GUID(0x21686148, 0x6449, 0x6E6F, 0x744E, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49), 0x00, "BIOS boot", "BIOS boot" },
};
#define NGPT_TYPES (sizeof(gpt_types) / sizeof(gpt_types[0]))

static int gpt_type(const u8 *guid) {
    u_int i;
    for(i = 0; i < NGPT_TYPES; i++) if(!memcmp(gpt_types[i].guid, guid, 16)) return (int)i;
    return -1;
}

const char *part_gpt_type_name(const u8 *guid, bool brief) {
    int i = gpt_type(guid);
    if(i < 0) return NULL;
    return brief ? gpt_types[i].brief : gpt_types[i].name;
}

u8 part_gpt_mbr_type(const u8 *guid) {
    int i = gpt_type(guid);
    return i < 0 ? 0 : gpt_types[i].mbr;
}

const char *part_type_name(int scheme, const struct part_entry *e, bool brief) {
    const char *n = scheme == PART_SCHEME_GPT ? part_gpt_type_name(e->type_guid, brief) : part_mbr_type_name(e->type);
    return n ? n : "unknown";
}

void part_guid_str(const u8 *g, char *out) {
    snprintf(out, 37, "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", (u_long)get32(g), get16(g + 4), get16(g + 6),
             g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

/* 512-byte blocks for people: always rounded down, so a free region is never shown bigger than it is */
void part_fmt_size(u32 blocks, char *buf, size_t size) {
    u32 mib = blocks / 2048;
    if(blocks < 2048) snprintf(buf, size, "%luK", (u_long)(blocks / 2));
    else if(mib < 1000) snprintf(buf, size, "%luM", (u_long)mib);
    else if(mib < 10 * 1024) snprintf(buf, size, "%lu.%luG", (u_long)(mib / 1024), (u_long)(mib % 1024 * 10 / 1024));
    else if(mib < 1000 * 1024) snprintf(buf, size, "%luG", (u_long)(mib / 1024));
    else snprintf(buf, size, "%lu.%luT", (u_long)(mib >> 20), (u_long)((mib & 0xFFFFF) * 10 >> 20));
}

/**** FAT boot sectors ****/

bool part_fat_label(const u8 *bs, char *label) {
    u16 bps = get16(bs + 11), rsv = get16(bs + 14);
    u8 spc = bs[13], nfats = bs[16];
    const u8 *ext;
    if(bs[MBR_SIG_OFFSET] != 0x55 || bs[MBR_SIG_OFFSET + 1] != 0xAA) return false;
    if(bs[0] != 0xEB && bs[0] != 0xE9) return false;                /* a jump to the boot code */
    if(bps < 512 || bps > 4096 || (bps & (bps - 1)) || !spc || (spc & (spc - 1)) || !rsv || !nfats || nfats > 2)
        return false;
    ext = get16(bs + 22) ? bs + 36 : bs + 64;     /* FAT12/16 extended BPB at 36; FAT32 (no 16-bit FAT size) at 64 */
    if(ext[2] != 0x29) return false;              /* no extended boot signature: the label field is not there */
    memcpy(label, ext + 7, 11);
    label[11] = 0;
    return true;
}

/**** reading: common ****/

static struct part_entry *add_part(const struct part_io *io, struct part_table *t, u_int index, u32 start, u32 count) {
    struct part_entry *e;
    if(t->nparts >= PART_MAX) {
        if(!t->full) note(io, "more than %u partitions; the rest are left out", PART_MAX);
        t->full = 1;
        return NULL;
    }
    e = &t->part[t->nparts++];
    memset(e, 0, sizeof(*e));
    e->index = index;
    e->start = start;
    e->count = count;
    return e;
}

const struct part_entry *part_find(const struct part_table *t, u_int index) {
    u_int i;
    for(i = 0; i < t->nparts; i++) if(t->part[i].index == index) return &t->part[i];
    return NULL;
}

static bool mbr_signature(const u8 *s) {
    return s[MBR_SIG_OFFSET] == 0x55 && s[MBR_SIG_OFFSET + 1] == 0xAA;
}

static bool is_extended(u8 type) {
    return type == 0x05 || type == 0x0F || type == 0x85;
}

/**** MBR ****/

/* The logical partitions: a chain of EBRs, each with a data entry (start relative to the EBR)
   and a link to the next EBR (relative to the start of the extended partition). Linux numbers
   them from 5 in chain order, and so does this. A chain that comes back to an EBR it has seen,
   or leaves the extended partition, ends there. */
static void read_logical(const struct part_io *io, struct part_table *t, u32 ext_start, u32 ext_count, u8 *sec) {
    u32 seen[PART_MAX];
    u_int nseen = 0, index = 5, i;
    u64 ext_end = (u64)ext_start + ext_count;       /* the first block after it */
    u32 ebr = ext_start;

    for(;;) {
        const u8 *link = NULL;
        int s;
        for(i = 0; i < nseen && seen[i] != ebr; i++);
        if(i < nseen) { skip(io, t, "the chain of logical partitions loops back to block %lu; it ends there", (u_long)ebr); return; }
        if(nseen == PART_MAX) { skip(io, t, "more than %u logical partition tables; the rest are left out", PART_MAX); return; }
        seen[nseen++] = ebr;
        if(io->read(io->arg, ebr, 1, sec) != 0) { skip(io, t, "block %lu (logical partitions) cannot be read", (u_long)ebr); return; }
        if(!mbr_signature(sec)) {
            if(ebr != ext_start) skip(io, t, "the logical partition table at block %lu has no signature; the chain ends there", (u_long)ebr);
            return;                                 /* the first one blank: an empty extended partition */
        }
        for(s = 0; s < 4; s++) {
            const u8 *p = sec + MBR_TABLE_OFFSET + s * 16;
            u8 type = p[4];
            u32 rel = get32(p + 8), count = get32(p + 12);
            u64 start = (u64)ebr + rel;
            struct part_entry *e;
            if(!type || !count) continue;
            if(is_extended(type)) { if(!link) link = p; continue; }
            if(!rel || start + count > ext_end) {
                skip(io, t, "a logical partition (type %02x, blocks %llu-%llu) lies outside the extended partition; left out",
                     type, start, start + count - 1);
                continue;
            }
            e = add_part(io, t, index++, (u32)start, count);
            if(!e) return;
            e->type = type;
            e->active = p[0] == 0x80;
            e->logical = 1;
            e->slot = (u8)s;
            e->table_lba = ebr;
        }
        if(!link) return;
        if((u64)ext_start + get32(link + 8) >= ext_end) {
            skip(io, t, "the next logical partition table (block %llu) lies outside the extended partition; the chain ends there",
                 (u64)ext_start + get32(link + 8));
            return;
        }
        ebr = ext_start + get32(link + 8);
    }
}

static void read_mbr(const struct part_io *io, struct part_table *t, u8 *sec) {
    u32 ext_start = 0, ext_count = 0;
    int i;
    t->scheme = PART_SCHEME_MBR;
    t->disk_sig = get32(sec + MBR_DISK_SIG);
    for(i = 0; i < 4; i++) {
        const u8 *p = sec + MBR_TABLE_OFFSET + i * 16;
        u8 type = p[4];
        u32 start = get32(p + 8), count = get32(p + 12);
        struct part_entry *e;
        if(!type || !count) continue;
        if(!start || (u64)start + count > t->nblocks) {
            skip(io, t, "partition %d (type %02x, blocks %lu-%llu) lies outside the disk; left out", i + 1, type,
                 (u_long)start, (u64)start + count - 1);
            continue;
        }
        if(type == 0xEE) {                          /* would cover a GPT: never a partition of its own */
            skip(io, t, "partition %d has the GPT protective type but does not start at block 1; left out", i + 1);
            continue;
        }
        if(is_extended(type) && ext_count) { skip(io, t, "partition %d is a second extended partition; left out", i + 1); continue; }
        e = add_part(io, t, (u_int)(i + 1), start, count);
        if(!e) return;
        e->type = type;
        e->active = p[0] == 0x80;
        e->slot = (u8)i;
        if(is_extended(type)) {
            e->extended = 1;
            ext_start = start;
            ext_count = count;
        }
    }
    if(ext_count) read_logical(io, t, ext_start, ext_count, sec);     /* sec is reused: block 0 is done with */
}

/**** GPT ****/

struct gpt_hdr {
    u64 alt;                    /* where it says the other copy is */
    u32 first_usable, last_usable, entries_lba, nentries, entries_crc;
    u8 disk_guid[16];
    bool clipped;               /* it describes a disk bigger than this one (or than 2^32 blocks) */
};

static u32 entry_sectors(u32 nentries) {
    return (nentries * GPT_ENTRY_SIZE + PART_SECTOR - 1) / PART_SECTOR;
}

/* The header at lba and its entries, checked as UEFI 2.10 section 5.3.2 says: signature, header
   CRC (computed with its own CRC field zeroed), MyLBA, and the CRC of the entries. 0 when valid,
   else -1 with the reason in why. */
static int gpt_check(const struct part_io *io, u32 nblocks, u32 lba, u8 *sec, struct gpt_hdr *h, char *why, size_t n) {
    u32 hsize, crc, esize, nsect, left, s;
    u64 my, first, last, elba;

    if(io->read(io->arg, lba, 1, sec) != 0) { snprintf(why, n, "block %lu cannot be read", (u_long)lba); return -1; }
    if(memcmp(sec, "EFI PART", 8)) { snprintf(why, n, "no header at block %lu", (u_long)lba); return -1; }
    hsize = get32(sec + 12);
    if(hsize < GPT_HEADER_MIN || hsize > PART_SECTOR) { snprintf(why, n, "a header size of %lu", (u_long)hsize); return -1; }
    crc = get32(sec + 16);
    memset(sec + 16, 0, 4);
    if(part_crc32(0, sec, hsize) != crc) { snprintf(why, n, "the header CRC does not match"); return -1; }
    my = get64(sec + 24);
    h->alt = get64(sec + 32);
    first = get64(sec + 40);
    last = get64(sec + 48);
    memcpy(h->disk_guid, sec + 56, 16);
    elba = get64(sec + 72);
    h->nentries = get32(sec + 80);
    esize = get32(sec + 84);
    h->entries_crc = get32(sec + 88);
    if(my != lba) { snprintf(why, n, "it says it is at block %llu", my); return -1; }
    if(esize != GPT_ENTRY_SIZE) { snprintf(why, n, "entries of %lu bytes", (u_long)esize); return -1; }
    if(!h->nentries || h->nentries > GPT_MAX_ENTRIES) { snprintf(why, n, "%lu entries", (u_long)h->nentries); return -1; }
    nsect = entry_sectors(h->nentries);
    if(first > last || first >= nblocks) { snprintf(why, n, "usable blocks %llu-%llu", first, last); return -1; }
    h->clipped = last >= nblocks;
    if(h->clipped) last = nblocks - 1;
    if(elba < 1 || elba + nsect > nblocks) { snprintf(why, n, "its entries (block %llu) lie outside the disk", elba); return -1; }
    if((elba + nsect > first && elba <= last) || (lba >= first && lba <= last) || (lba >= elba && lba < elba + nsect)) {
        snprintf(why, n, "it or its entries overlap the usable blocks");
        return -1;
    }
    h->first_usable = (u32)first;
    h->last_usable = (u32)last;
    h->entries_lba = (u32)elba;
    crc = 0;
    left = h->nentries * GPT_ENTRY_SIZE;
    for(s = 0; s < nsect; s++) {
        u32 bytes = left < PART_SECTOR ? left : PART_SECTOR;
        if(io->read(io->arg, h->entries_lba + s, 1, sec) != 0) { snprintf(why, n, "its entries cannot be read"); return -1; }
        crc = part_crc32(crc, sec, bytes);
        left -= bytes;
    }
    if(crc != h->entries_crc) { snprintf(why, n, "the partition entries do not match their CRC"); return -1; }
    return 0;
}

static void gpt_entry(const struct part_io *io, struct part_table *t, u_int index, const u8 *p) {
    u64 first = get64(p + 32), last = get64(p + 40);
    struct part_entry *e;
    int i;
    for(i = 0; i < 16 && !p[i]; i++);
    if(i == 16) return;                             /* a zero type GUID: an unused entry */
    if(first > last) { skip(io, t, "partition %u ends before it starts; left out", index); return; }
    if(last > 0xFFFFFFFFull) {
        skip(io, t, "partition %u (blocks %llu-%llu) lies beyond block 4294967295, which SurfOS cannot address; left out",
             index, first, last);
        return;
    }
    if(last >= t->nblocks) { skip(io, t, "partition %u (blocks %llu-%llu) runs past the end of the disk; left out", index, first, last); return; }
    if(first < t->first_usable || last > t->last_usable) {
        skip(io, t, "partition %u (blocks %llu-%llu) lies outside the usable blocks %lu-%lu; left out", index, first, last,
             (u_long)t->first_usable, (u_long)t->last_usable);
        return;
    }
    e = add_part(io, t, index, (u32)first, (u32)(last - first + 1));
    if(!e) return;
    memcpy(e->type_guid, p, 16);
    memcpy(e->guid, p + 16, 16);
    e->attrs = get64(p + 48);
    e->type = part_gpt_mbr_type(p);
    for(i = 0; i < PART_NAME_LEN - 1; i++) {
        u16 c = get16(p + 56 + 2 * i);
        if(!c) break;
        e->name[i] = (c >= 32 && c < 127) ? (char)c : '?';
    }
    e->name[i] = 0;
}

static void read_gpt(const struct part_io *io, struct part_table *t, u8 *sec) {
    struct gpt_hdr h, b;
    char why[80], bwhy[80];
    u32 last = t->nblocks - 1, nsect, s, i, index = 1;

    t->scheme = PART_SCHEME_GPT;
    if(gpt_check(io, t->nblocks, 1, sec, &h, why, sizeof(why)) == 0) {
        if(h.alt != last) {
            t->backup_bad = 1;
            note(io, "the primary GPT puts its backup at block %llu, not at the last block (%lu)", h.alt, (u_long)last);
        } else if(gpt_check(io, t->nblocks, last, sec, &b, bwhy, sizeof(bwhy)) != 0) {
            t->backup_bad = 1;
            note(io, "the backup GPT at block %lu is damaged (%s)", (u_long)last, bwhy);
        } else if(b.entries_crc != h.entries_crc || b.nentries != h.nentries || memcmp(b.disk_guid, h.disk_guid, 16)) {
            t->backup_bad = 1;
            note(io, "the backup GPT at block %lu differs from the primary", (u_long)last);
        }
    } else if(gpt_check(io, t->nblocks, last, sec, &h, bwhy, sizeof(bwhy)) == 0) {
        t->backup_used = 1;
        note(io, "the primary GPT is damaged (%s): using the backup at block %lu", why, (u_long)last);
    } else {
        t->damaged = 1;
        note(io, "a protective MBR but no valid GPT: the primary (%s) and the backup (%s) are damaged", why, bwhy);
        return;
    }
    if(h.clipped) note(io, "the GPT describes a bigger disk; only blocks up to %lu can be reached", (u_long)last);
    memcpy(t->disk_guid, h.disk_guid, 16);
    t->first_usable = h.first_usable;
    t->last_usable = h.last_usable;
    t->entries_lba = h.entries_lba;
    t->nentries = h.nentries;
    nsect = entry_sectors(h.nentries);
    for(s = 0; s < nsect; s++) {                    /* a second pass: the entries are believed once their CRC matched */
        if(io->read(io->arg, h.entries_lba + s, 1, sec) != 0) { skip(io, t, "the GPT entries cannot be read"); t->nparts = 0; return; }
        for(i = 0; i < PART_SECTOR / GPT_ENTRY_SIZE && index <= h.nentries; i++, index++)
            gpt_entry(io, t, index, sec + i * GPT_ENTRY_SIZE);
    }
}

/**** the table ****/

int part_read(const struct part_io *io, u32 nblocks, struct part_table *t) {
    u8 sec[PART_SECTOR];
    bool gpt = false, sane = true;
    int i;

    memset(t, 0, sizeof(*t));
    t->nblocks = nblocks;
    if(!nblocks || io->read(io->arg, 0, 1, sec) != 0) return -1;
    if(!mbr_signature(sec)) return 0;               /* no partition table: blank, or a volume without one */
    for(i = 0; i < 4; i++) {
        const u8 *p = sec + MBR_TABLE_OFFSET + i * 16;
        if(p[4] == 0xEE && get32(p + 8) == 1) gpt = true;   /* the protective entry starts at block 1 (UEFI 5.2.3) */
        if(p[0] != 0x00 && p[0] != 0x80) sane = false;
    }
    if(gpt) read_gpt(io, t, sec);
    else if(sane) read_mbr(io, t, sec);
    /* else: boot code where the entries would be, so block 0 is a FAT or NTFS boot sector, not a table */
    return 0;
}

/**** changes: where a new partition goes ****/

/* GPT: up to where the backup goes once fdisk has rewritten the table, which is the header's own
   end of the usable area unless the disk is bigger (an image written to a bigger stick) or smaller
   than the table was made for */
static u32 usable_lo(const struct part_table *t) { return t->scheme == PART_SCHEME_GPT ? t->first_usable : 1; }
static u32 usable_hi(const struct part_table *t) {
    return t->scheme == PART_SCHEME_GPT ? t->nblocks - 2 - entry_sectors(t->nentries) : t->nblocks - 1;
}

/* the first free stretch at or after pos where an aligned partition of PART_ALIGN blocks fits;
   logical partitions count through the extended partition around them */
static bool free_after(const struct part_table *t, u64 pos, struct part_region *r) {
    u64 hi = usable_hi(t);
    u_int i, step;
    if(pos < usable_lo(t)) pos = usable_lo(t);
    for(step = 0; step < 2 * t->nparts + 2; step++) {       /* each step passes a partition or a gap */
        u64 s = (pos + PART_ALIGN - 1) & ~(u64)(PART_ALIGN - 1), e = hi, past = 0;
        if(s > hi) return false;
        for(i = 0; i < t->nparts; i++) {
            const struct part_entry *p = &t->part[i];
            u64 ps = p->start, pe = ps + p->count - 1;
            if(p->logical) continue;
            if(ps <= s && s <= pe) { if(pe + 1 > past) past = pe + 1; }
            else if(ps > s && ps - 1 < e) e = ps - 1;
        }
        if(past) { pos = past; continue; }
        if(e - s + 1 >= PART_ALIGN) {
            r->start = (u32)s;
            r->count = (u32)(e - s + 1);
            return true;
        }
        pos = e + 1;
    }
    return false;
}

u_int part_free(const struct part_table *t, struct part_region *r, u_int max) {
    struct part_region g;
    u64 pos = 0;
    u_int n = 0;
    if(t->scheme == PART_SCHEME_NONE || t->damaged) return 0;
    while(n < max && free_after(t, pos, &g)) {
        r[n++] = g;
        pos = (u64)g.start + g.count;
    }
    return n;
}

/* the largest free region (want 0), or the first one want blocks fit in */
static int choose_region(const struct part_table *t, u32 want, struct part_region *out, char *err, size_t n) {
    struct part_region g, best = { 0, 0 };
    char a[8], b[8];
    u64 pos = 0;
    while(free_after(t, pos, &g)) {
        if(want && g.count >= want) {
            out->start = g.start;
            out->count = want;
            return 0;
        }
        if(g.count > best.count) best = g;
        pos = (u64)g.start + g.count;
    }
    if(!want && best.count) { *out = best; return 0; }
    if(!best.count) snprintf(err, n, "no free space (a partition needs 1M, starting on a 1M boundary)");
    else {
        part_fmt_size(want, a, sizeof(a));
        part_fmt_size(best.count, b, sizeof(b));
        snprintf(err, n, "no free region of %s (the largest is %s)", a, b);
    }
    return -1;
}

/**** changes: words ****/

#define GPT_EFI   0             /* indexes into gpt_types */
#define GPT_BASIC 1
#define GPT_LINUX 4

int part_parse_size(const char *s, u32 *blocks) {
    u64 v = 0;
    int digits = 0, shift;
    if(!strcmp(s, "all")) { *blocks = 0; return 0; }
    for(; *s >= '0' && *s <= '9'; s++) {
        if(++digits > 9) return -1;
        v = v * 10 + (u64)(*s - '0');
    }
    switch(*s++) {
    case 'K': case 'k': shift = 1; break;       /* 1024 bytes are 2 blocks */
    case 'M': case 'm': shift = 11; break;
    case 'G': case 'g': shift = 21; break;
    case 'T': case 't': shift = 31; break;
    default: return -1;
    }
    if(!digits || (*s && strcmp(s, "B") && strcmp(s, "iB"))) return -1;
    v <<= shift;
    if(v < PART_ALIGN || v > 0xFFFFFFFFull) return -1;
    *blocks = (u32)v;
    return 0;
}

int part_parse_type(const char *s, struct part_kind *k) {
    static const struct { const char *word; u8 mbr; int gpt; } words[] = {
        { "fat32", 0x0C, GPT_BASIC }, { "fat16", 0x0E, GPT_BASIC }, { "linux", 0x83, GPT_LINUX }, { "efi", 0xEF, GPT_EFI },
    };
    u_int i, v = 0, digits = 0;
    u8 like;
    for(i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        if(!strcmp(s, words[i].word)) {
            k->mbr = words[i].mbr;
            k->gpt = gpt_types[words[i].gpt].guid;
            return 0;
        }
    }
    if(s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for(; *s; s++, digits++) {
        if(digits == 2 || !isxdigit(*s)) return -1;
        v = v * 16 + (u_int)(isdigit(*s) ? *s - '0' : (*s | 0x20) - 'a' + 10);
    }
    if(!digits || !v || v == 0xEE || is_extended((u8)v)) return -1;     /* unused, GPT protective, extended */
    k->mbr = (u8)v;
    k->gpt = NULL;
    like = (v == 0x01 || v == 0x04 || v == 0x06 || v == 0x0B || v == 0x0C || v == 0x0E) ? 0x07 : (u8)v;   /* FAT: basic data */
    for(i = 0; i < NGPT_TYPES; i++) if(gpt_types[i].mbr == like) { k->gpt = gpt_types[i].guid; break; }
    return 0;
}

/**** changes: building blocks ****/

static void plan_begin(struct part_plan *p, int scheme) {
    p->nwrites = 0;
    p->what[0] = 0;
    p->scheme = scheme;
    p->index = 0;
    p->present = 0;
    p->start = p->count = 0;
    p->type = 0;
    memset(p->type_guid, 0, 16);
}

static void plan_write(struct part_plan *p, u32 lba, u32 count, const u8 *data) {
    if(p->nwrites >= PART_PLAN_WRITES) return;          /* cannot happen: no plan needs more */
    p->w[p->nwrites].lba = lba;
    p->w[p->nwrites].count = count;
    p->w[p->nwrites].data = data;
    p->nwrites++;
}

/* cylinder/head/sector as everyone fills them in for LBA disks: 255 heads, 63 sectors, and
   1023/254/63 beyond what that reaches */
static void chs(u8 *p, u32 lba) {
    u32 c = lba / (255 * 63), h = lba / 63 % 255, s = lba % 63 + 1;
    if(c > 1023) { c = 1023; h = 254; s = 63; }
    p[0] = (u8)h;
    p[1] = (u8)(s | ((c >> 2) & 0xC0));
    p[2] = (u8)c;
}

static void mbr_set(u8 *e, u8 type, u32 start, u32 count) {
    memset(e, 0, 16);
    chs(e + 1, start);
    e[4] = type;
    chs(e + 5, start + count - 1);
    put32(e + 8, start);
    put32(e + 12, count);
}

static void random_guid(const struct part_io *io, u8 *g) {
    u_int i;
    for(i = 0; i < 16; i += 4) put32(g + i, io->random(io->arg));
    g[7] = (u8)((g[7] & 0x0F) | 0x40);      /* version 4: the top of the third group, stored little-endian */
    g[8] = (u8)((g[8] & 0x3F) | 0x80);      /* the RFC 4122 variant */
}

static const char *mbr_name(u8 type) {
    const char *name = part_mbr_type_name(type);
    return name ? name : "unknown";
}

static u8 *plan_entries(struct part_plan *p) { return p->buf + 3 * PART_SECTOR; }
static u8 *gpt_slot(struct part_plan *p, u_int index) { return plan_entries(p) + (index - 1) * GPT_ENTRY_SIZE; }

static bool gpt_unused(const u8 *e) {
    int i;
    for(i = 0; i < 16; i++) if(e[i]) return false;
    return true;
}

static void gpt_header(u8 *h, u32 my, u32 alt, u32 first, u32 last, const u8 *guid, u32 elba, u32 nentries, u32 ecrc) {
    memset(h, 0, PART_SECTOR);
    memcpy(h, "EFI PART", 8);
    put32(h + 8, 0x00010000);               /* revision 1.0 */
    put32(h + 12, GPT_HEADER_MIN);
    put64(h + 24, my);
    put64(h + 32, alt);
    put64(h + 40, first);
    put64(h + 48, last);
    memcpy(h + 56, guid, 16);
    put64(h + 72, elba);
    put32(h + 80, nentries);
    put32(h + 84, GPT_ENTRY_SIZE);
    put32(h + 88, ecrc);
    put32(h + 16, part_crc32(0, h, GPT_HEADER_MIN));
}

/* Both copies of a GPT from the entries in the plan's buffer: the primary header at block 1 with
   its entries from block 2, the backup entries and header in the last blocks of the disk, which
   also become the end of the usable area. The primary goes first, so a write cut short leaves
   one copy that is whole. */
static void gpt_plan(struct part_plan *p, u32 nblocks, const u8 *guid, u32 first, u32 nentries) {
    u8 *h1 = p->buf + PART_SECTOR, *h2 = p->buf + 2 * PART_SECTOR, *ent = plan_entries(p);
    u32 nsect = entry_sectors(nentries), ecrc = part_crc32(0, ent, nentries * GPT_ENTRY_SIZE);
    gpt_header(h1, 1, nblocks - 1, first, nblocks - 2 - nsect, guid, 2, nentries, ecrc);
    gpt_header(h2, nblocks - 1, 1, first, nblocks - 2 - nsect, guid, nblocks - 1 - nsect, nentries, ecrc);
    plan_write(p, 2, nsect, ent);
    plan_write(p, 1, 1, h1);
    plan_write(p, nblocks - 1 - nsect, nsect, ent);
    plan_write(p, nblocks - 1, 1, h2);
}

/* a table fdisk may change: one it understood completely */
static int editable(const struct part_table *t, char *err, size_t n) {
    if(t->scheme == PART_SCHEME_NONE) { snprintf(err, n, "there is no partition table (new mbr or new gpt makes one)"); return -1; }
    if(t->damaged) { snprintf(err, n, "the GPT is damaged in both copies (new mbr or new gpt replaces it)"); return -1; }
    if(t->skipped || t->full) { snprintf(err, n, "the table has entries SurfOS left out (new mbr or new gpt replaces it)"); return -1; }
    if(t->scheme == PART_SCHEME_GPT && t->nentries > GPT_ENTRIES) {
        snprintf(err, n, "this GPT has %lu entries; fdisk changes tables of up to %u", (u_long)t->nentries, GPT_ENTRIES);
        return -1;
    }
    return 0;
}

/* the entries of the GPT copy the table was read from, into the plan's buffer */
static int gpt_load(const struct part_io *io, const struct part_table *t, struct part_plan *p, char *err, size_t n) {
    u32 nsect = entry_sectors(t->nentries), last = t->nblocks - 2 - nsect;
    u_int i;
    memset(plan_entries(p), 0, GPT_ENTRIES * GPT_ENTRY_SIZE);
    if(io->read(io->arg, t->entries_lba, nsect, plan_entries(p)) != 0) { snprintf(err, n, "the GPT entries cannot be read"); return -1; }
    if(t->first_usable < 2 + nsect) {
        snprintf(err, n, "the usable blocks start at %lu, where the GPT entries go (new gpt replaces it)", (u_long)t->first_usable);
        return -1;
    }
    for(i = 0; i < t->nparts; i++) {
        if(t->part[i].start + t->part[i].count - 1 > last) {
            snprintf(err, n, "partition %u reaches into the last %lu blocks, where the backup GPT goes", t->part[i].index,
                     (u_long)(t->nblocks - 1 - last));
            return -1;
        }
    }
    return 0;
}

/**** changes ****/

int part_plan_new(const struct part_io *io, u32 nblocks, int scheme, struct part_plan *p, char *err, size_t n) {
    u8 *mbr = p->buf, *e = p->buf + MBR_TABLE_OFFSET, guid[16];
    char g[37];
    u32 sig;

    plan_begin(p, scheme);
    if(scheme != PART_SCHEME_MBR && scheme != PART_SCHEME_GPT) { snprintf(err, n, "no such kind of table"); return -1; }
    if(nblocks < 2 * PART_ALIGN) { snprintf(err, n, "the disk is too small for a partition table (2M at least)"); return -1; }
    memset(mbr, 0, PART_SECTOR);
    mbr[MBR_SIG_OFFSET] = 0x55;
    mbr[MBR_SIG_OFFSET + 1] = 0xAA;
    if(scheme == PART_SCHEME_MBR) {
        sig = io->random(io->arg);
        if(!sig) sig = 1;                           /* 0 means "none" to some systems */
        put32(mbr + MBR_DISK_SIG, sig);
        plan_write(p, nblocks - 33, 33, NULL);      /* where a backup GPT would be, */
        plan_write(p, 1, 33, NULL);                 /* and a primary one */
        plan_write(p, 0, 1, mbr);
        snprintf(p->what, sizeof(p->what), "a new, empty MBR (disk signature %08lx)", (u_long)sig);
        return 0;
    }
    random_guid(io, guid);
    e[1] = 0x00; e[2] = 0x02; e[3] = 0x00;          /* the protective entry: from block 1 (CHS 0/0/2) */
    e[4] = 0xEE;                                    /* to the end of the disk, CHS FF FF FF (UEFI 5.2.3) */
    e[5] = e[6] = e[7] = 0xFF;
    put32(e + 8, 1);
    put32(e + 12, nblocks - 1);
    memset(plan_entries(p), 0, GPT_ENTRIES * GPT_ENTRY_SIZE);
    gpt_plan(p, nblocks, guid, 2 + entry_sectors(GPT_ENTRIES), GPT_ENTRIES);
    plan_write(p, 0, 1, mbr);
    part_guid_str(guid, g);
    snprintf(p->what, sizeof(p->what), "a new, empty GPT (disk GUID %s)", g);
    return 0;
}

int part_plan_add(const struct part_io *io, const struct part_table *t, u32 count, const struct part_kind *k,
                  struct part_plan *p, char *err, size_t n) {
    struct part_region r;
    char size[8];
    u_int index;

    plan_begin(p, t->scheme);
    if(editable(t, err, n)) return -1;
    if(t->scheme == PART_SCHEME_GPT && !k->gpt) {
        snprintf(err, n, "type %02x has no GPT counterpart (fat32, fat16, linux and efi do)", k->mbr);
        return -1;
    }
    if(t->scheme == PART_SCHEME_MBR) {
        u8 *e = NULL;
        if(io->read(io->arg, 0, 1, p->buf) != 0) { snprintf(err, n, "block 0 cannot be read"); return -1; }
        for(index = 1; index <= 4; index++) {
            e = p->buf + MBR_TABLE_OFFSET + (index - 1) * 16;
            if(!e[4] || !get32(e + 12)) break;
        }
        if(index > 4) { snprintf(err, n, "all four primary entries are in use (fdisk makes no logical partitions)"); return -1; }
        if(choose_region(t, count, &r, err, n)) return -1;
        mbr_set(e, k->mbr, r.start, r.count);
        plan_write(p, 0, 1, p->buf);
        p->type = k->mbr;
        part_fmt_size(r.count, size, sizeof(size));
        snprintf(p->what, sizeof(p->what), "add partition %u: blocks %lu-%lu (%s), type %02x %s", index, (u_long)r.start,
                 (u_long)(r.start + r.count - 1), size, k->mbr, mbr_name(k->mbr));
    } else {
        const char *name = !memcmp(k->gpt, gpt_types[GPT_BASIC].guid, 16) ? "SurfOS data" : part_gpt_type_name(k->gpt, false);
        u8 *e = NULL;
        u_int i;
        if(!name) name = "SurfOS";
        if(gpt_load(io, t, p, err, n)) return -1;
        for(index = 1; index <= t->nentries; index++) if(gpt_unused(e = gpt_slot(p, index))) break;
        if(index > t->nentries) { snprintf(err, n, "all %lu GPT entries are in use", (u_long)t->nentries); return -1; }
        if(choose_region(t, count, &r, err, n)) return -1;
        memset(e, 0, GPT_ENTRY_SIZE);
        memcpy(e, k->gpt, 16);
        random_guid(io, e + 16);
        put64(e + 32, r.start);
        put64(e + 40, (u64)r.start + r.count - 1);
        for(i = 0; name[i] && i < PART_NAME_LEN - 1; i++) put16(e + 56 + 2 * i, (u8)name[i]);
        gpt_plan(p, t->nblocks, t->disk_guid, t->first_usable, t->nentries);
        memcpy(p->type_guid, k->gpt, 16);
        part_fmt_size(r.count, size, sizeof(size));
        snprintf(p->what, sizeof(p->what), "add partition %u: blocks %lu-%lu (%s), %s \"%s\"", index, (u_long)r.start,
                 (u_long)(r.start + r.count - 1), size, part_gpt_type_name(k->gpt, true), name);
    }
    p->index = index;
    p->present = 1;
    p->start = r.start;
    p->count = r.count;
    return 0;
}

int part_plan_delete(const struct part_io *io, const struct part_table *t, u_int index, struct part_plan *p,
                     char *err, size_t n) {
    const struct part_entry *e = part_find(t, index);
    char size[8];
    u_int i;

    plan_begin(p, t->scheme);
    if(editable(t, err, n)) return -1;
    if(!e) { snprintf(err, n, "there is no partition %u", index); return -1; }
    if(t->scheme == PART_SCHEME_MBR) {
        if(e->logical) {
            for(i = 0; i < t->nparts && !t->part[i].extended; i++);
            snprintf(err, n, "logical partitions are not deleted one by one (delete extended partition %u)",
                     i < t->nparts ? t->part[i].index : 0);
            return -1;
        }
        if(io->read(io->arg, 0, 1, p->buf) != 0) { snprintf(err, n, "block 0 cannot be read"); return -1; }
        memset(p->buf + MBR_TABLE_OFFSET + e->slot * 16, 0, 16);
        plan_write(p, 0, 1, p->buf);
    } else {
        if(gpt_load(io, t, p, err, n)) return -1;
        memset(gpt_slot(p, index), 0, GPT_ENTRY_SIZE);
        gpt_plan(p, t->nblocks, t->disk_guid, t->first_usable, t->nentries);
    }
    p->index = index;
    part_fmt_size(e->count, size, sizeof(size));
    snprintf(p->what, sizeof(p->what), "delete partition %u: blocks %lu-%lu (%s), %s%s", index, (u_long)e->start,
             (u_long)(e->start + e->count - 1), size, part_type_name(t->scheme, e, false),
             e->extended ? ", and the logical partitions in it" : "");
    return 0;
}

int part_plan_type(const struct part_io *io, const struct part_table *t, u_int index, const struct part_kind *k,
                   struct part_plan *p, char *err, size_t n) {
    const struct part_entry *e = part_find(t, index);
    plan_begin(p, t->scheme);
    if(editable(t, err, n)) return -1;
    if(!e) { snprintf(err, n, "there is no partition %u", index); return -1; }
    if(t->scheme == PART_SCHEME_MBR) {
        u8 *slot = p->buf + MBR_TABLE_OFFSET + e->slot * 16;
        if(e->extended) { snprintf(err, n, "partition %u is the extended partition; its type stays", index); return -1; }
        if(io->read(io->arg, e->table_lba, 1, p->buf) != 0) { snprintf(err, n, "block %lu cannot be read", (u_long)e->table_lba); return -1; }
        if(slot[4] != e->type) { snprintf(err, n, "the table changed since it was read"); return -1; }
        slot[4] = k->mbr;
        plan_write(p, e->table_lba, 1, p->buf);
        p->type = k->mbr;
        snprintf(p->what, sizeof(p->what), "partition %u: type %02x %s becomes %02x %s", index, e->type,
                 part_type_name(t->scheme, e, false), k->mbr, mbr_name(k->mbr));
    } else {
        if(!k->gpt) { snprintf(err, n, "type %02x has no GPT counterpart (fat32, fat16, linux and efi do)", k->mbr); return -1; }
        if(gpt_load(io, t, p, err, n)) return -1;
        memcpy(gpt_slot(p, index), k->gpt, 16);
        gpt_plan(p, t->nblocks, t->disk_guid, t->first_usable, t->nentries);
        memcpy(p->type_guid, k->gpt, 16);
        snprintf(p->what, sizeof(p->what), "partition %u: %s becomes %s", index, part_type_name(t->scheme, e, false),
                 part_gpt_type_name(k->gpt, false));
    }
    p->index = index;
    p->present = 1;
    return 0;
}

int part_plan_check(const struct part_plan *p, const struct part_table *t, char *err, size_t n) {
    const struct part_entry *e;
    if(t->scheme != p->scheme) { snprintf(err, n, "the disk does not read back as %s", p->scheme == PART_SCHEME_GPT ? "GPT" : "MBR"); return -1; }
    if(t->damaged || t->backup_used || t->backup_bad || t->skipped || t->full) { snprintf(err, n, "the table does not read back whole"); return -1; }
    if(!p->index) {
        if(t->nparts) { snprintf(err, n, "the new table is not empty"); return -1; }
        return 0;
    }
    e = part_find(t, p->index);
    if(!p->present) {
        if(e) { snprintf(err, n, "partition %u is still there", p->index); return -1; }
        return 0;
    }
    if(!e) { snprintf(err, n, "partition %u is not there", p->index); return -1; }
    if(p->count && (e->start != p->start || e->count != p->count)) {
        snprintf(err, n, "partition %u reads back at blocks %lu-%lu", p->index, (u_long)e->start, (u_long)(e->start + e->count - 1));
        return -1;
    }
    if(p->scheme == PART_SCHEME_GPT ? memcmp(e->type_guid, p->type_guid, 16) != 0 : e->type != p->type) {
        snprintf(err, n, "partition %u reads back with another type", p->index);
        return -1;
    }
    return 0;
}
