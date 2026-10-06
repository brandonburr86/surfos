/*
SurfOS Partition Tables
--------------------
File: parttab.c Date: 10/5/26 (USB storage)
--------------------
Reading MBR and GPT partition tables from 512-byte sectors handed over by a
callback. No I/O, no allocation and no kernel calls: driver/part.c registers
what this finds, and tests/host/test_part.c runs it on the host against tables
built by hand and by sfdisk and sgdisk. Everything read from the disk is
checked before it is used: a partition outside the disk, a looping chain of
logical partitions or a GPT whose CRC does not match is left out with a note
instead of being believed.
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

static void note(const struct part_io *io, const char *fmt, ...) {
    char msg[160];
    va_list ap;
    if(!io->note) return;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    io->note(io->arg, msg);
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
        if(i < nseen) { note(io, "the chain of logical partitions loops back to block %lu; it ends there", (u_long)ebr); return; }
        if(nseen == PART_MAX) { note(io, "more than %u logical partition tables; the rest are left out", PART_MAX); return; }
        seen[nseen++] = ebr;
        if(io->read(io->arg, ebr, 1, sec) != 0) { note(io, "block %lu (logical partitions) cannot be read", (u_long)ebr); return; }
        if(!mbr_signature(sec)) {
            if(ebr != ext_start) note(io, "the logical partition table at block %lu has no signature; the chain ends there", (u_long)ebr);
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
                note(io, "a logical partition (type %02x, blocks %llu-%llu) lies outside the extended partition; left out",
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
            note(io, "the next logical partition table (block %llu) lies outside the extended partition; the chain ends there",
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
            note(io, "partition %d (type %02x, blocks %lu-%llu) lies outside the disk; left out", i + 1, type,
                 (u_long)start, (u64)start + count - 1);
            continue;
        }
        if(type == 0xEE) {                          /* would cover a GPT: never a partition of its own */
            note(io, "partition %d has the GPT protective type but does not start at block 1; left out", i + 1);
            continue;
        }
        if(is_extended(type) && ext_count) { note(io, "partition %d is a second extended partition; left out", i + 1); continue; }
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
    if(first > last) { note(io, "partition %u ends before it starts; left out", index); return; }
    if(last > 0xFFFFFFFFull) {
        note(io, "partition %u (blocks %llu-%llu) lies beyond block 4294967295, which SurfOS cannot address; left out",
             index, first, last);
        return;
    }
    if(last >= t->nblocks) { note(io, "partition %u (blocks %llu-%llu) runs past the end of the disk; left out", index, first, last); return; }
    if(first < t->first_usable || last > t->last_usable) {
        note(io, "partition %u (blocks %llu-%llu) lies outside the usable blocks %lu-%lu; left out", index, first, last,
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
        if(io->read(io->arg, h.entries_lba + s, 1, sec) != 0) { note(io, "the GPT entries cannot be read"); t->nparts = 0; return; }
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
