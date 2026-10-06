/* Host-side unit tests for driver/parttab.c, the partition table code: MBR with an extended
   partition and its chain of logical partitions, GPT with both headers, the CRC32s, and the
   FAT volume label that marks the SurfOS boot drive. The tables are built by hand in a RAM
   disk, with a CRC32 of this file's own, then read back through part_read() and checked
   field by field, intact and damaged. Built like the kernel objects and run by `make unittest`. */
#include <stdio.h>
#include <string.h>
#include <surfos/types.h>
#include <sys/part.h>

void exit(int status);

static int failures, checks;

static void check(int ok, const char *what) {
    checks++;
    if(!ok) { failures++; printf("FAIL: %s\n", what); }
}

/**** a RAM disk ****/

#define DISK_BLOCKS 32768                           /* 16 MB */
static u8 disk[DISK_BLOCKS * 512];
static u32 nblocks = DISK_BLOCKS;
static u32 bad_block = 0xFFFFFFFF;                  /* reading this block fails */
static char notes[4096];

static int ram_read(void *arg, u32 lba, u32 count, void *buf) {
    if((u64)lba + count > nblocks) return -1;
    if(bad_block >= lba && bad_block < lba + count) return -1;
    memcpy(buf, disk + (u64)lba * 512, (u64)count * 512);
    return 0;
}

static void ram_note(void *arg, const char *msg) {
    size_t l = strlen(notes);
    if(l + strlen(msg) + 2 < sizeof(notes)) { memcpy(notes + l, msg, strlen(msg)); notes[l + strlen(msg)] = '\n'; notes[l + strlen(msg) + 1] = 0; }
}

static const struct part_io io = { NULL, ram_read, ram_note };
static struct part_table t;

static void blank(void) {
    memset(disk, 0, sizeof(disk));
    nblocks = DISK_BLOCKS;
    bad_block = 0xFFFFFFFF;
}

static int scan(void) {
    notes[0] = 0;
    return part_read(&io, nblocks, &t);
}

static u8 *sector(u32 lba) { return disk + (u64)lba * 512; }
static void put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v) { put16(p, (u16)v); put16(p + 2, (u16)(v >> 16)); }
static void put64(u8 *p, u64 v) { put32(p, (u32)v); put32(p + 4, (u32)(v >> 32)); }

/* an independent CRC32, one bit at a time */
static u32 crc32_bits(const u8 *p, u32 n) {
    u32 crc = 0xFFFFFFFF;
    int k;
    while(n--) {
        crc ^= *p++;
        for(k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
    return ~crc;
}

/**** MBR ****/

static void mbr_entry(u32 lba, int slot, u8 status, u8 type, u32 start, u32 count) {
    u8 *e = sector(lba) + 446 + slot * 16;
    e[0] = status;
    e[4] = type;
    put32(e + 8, start);
    put32(e + 12, count);
    sector(lba)[510] = 0x55;
    sector(lba)[511] = 0xAA;
}

static void test_names(void) {
    static const u8 efi[16] = { 0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B };
    static const u8 linux_fs[16] = { 0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47, 0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4 };
    static const u8 basic[16] = { 0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44, 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7 };
    static const u8 apfs[16] = { 0xEF, 0x57, 0x34, 0x7C, 0x00, 0x00, 0xAA, 0x11, 0xAA, 0x11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC };
    static const u8 other[16] = { 1, 2, 3 };
    char s[40];
    u8 buf[1000];
    u32 i;

    check(part_crc32(0, "123456789", 9) == 0xCBF43926, "CRC32 of \"123456789\" is CBF43926");
    check(part_crc32(0, "", 0) == 0, "CRC32 of nothing is 0");
    for(i = 0; i < sizeof(buf); i++) buf[i] = (u8)(i * 31 + 7);
    check(part_crc32(0, buf, sizeof(buf)) == crc32_bits(buf, sizeof(buf)), "CRC32 agrees with a bitwise CRC32");
    check(part_crc32(part_crc32(0, buf, 300), buf + 300, 700) == part_crc32(0, buf, 1000), "CRC32 chains");

    check(!strcmp(part_mbr_type_name(0x0C), "FAT32 LBA") && !strcmp(part_mbr_type_name(0xEF), "EFI system"), "MBR type names");
    check(part_mbr_type_name(0x42) == NULL, "an unknown MBR type has no name");
    check(!strcmp(part_gpt_type_name(efi, false), "EFI system") && part_gpt_mbr_type(efi) == 0xEF, "GPT: EFI system, MBR 0xEF");
    check(!strcmp(part_gpt_type_name(basic, false), "Microsoft basic data") && !strcmp(part_gpt_type_name(basic, true), "basic data") &&
          part_gpt_mbr_type(basic) == 0x07, "GPT: Microsoft basic data, brief name, MBR 0x07");
    check(!strcmp(part_gpt_type_name(linux_fs, false), "Linux filesystem") && part_gpt_mbr_type(linux_fs) == 0x83, "GPT: Linux filesystem, MBR 0x83");
    check(!strcmp(part_gpt_type_name(apfs, false), "Apple APFS") && part_gpt_mbr_type(apfs) == 0, "GPT: Apple APFS, no MBR type");
    check(part_gpt_type_name(other, false) == NULL && part_gpt_mbr_type(other) == 0, "an unknown GPT type");
    part_guid_str(efi, s);
    check(!strcmp(s, "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"), "GUID text: mixed-endian groups");

    part_fmt_size(0, s, sizeof(s));          check(!strcmp(s, "0K"), "size 0 blocks: 0K");
    part_fmt_size(2047, s, sizeof(s));       check(!strcmp(s, "1023K"), "size 2047 blocks: 1023K");
    part_fmt_size(2048, s, sizeof(s));       check(!strcmp(s, "1M"), "size 2048 blocks: 1M");
    part_fmt_size(32768, s, sizeof(s));      check(!strcmp(s, "16M"), "size 32768 blocks: 16M");
    part_fmt_size(3145728, s, sizeof(s));    check(!strcmp(s, "1.5G"), "size 1.5 GB");
    part_fmt_size(60751872, s, sizeof(s));   check(!strcmp(s, "28G"), "size of a 32 GB stick: 28G (rounded down)");
    part_fmt_size(0xFFFFFFFF, s, sizeof(s)); check(!strcmp(s, "1.9T"), "size 2^32 - 1 blocks: 1.9T");
}

static void test_blank_and_errors(void) {
    blank();
    check(scan() == 0 && t.scheme == PART_SCHEME_NONE && t.nparts == 0, "a blank disk: no table, no partitions");
    bad_block = 0;
    check(scan() == -1, "block 0 unreadable: -1");
    blank();
    mbr_entry(0, 0, 0x00, 0x0C, 2048, 4096);
    sector(0)[446] = 0x12;                           /* a status byte that is neither 00 nor 80 */
    check(scan() == 0 && t.scheme == PART_SCHEME_NONE, "a status byte other than 00/80: not a partition table");
    blank();
    sector(0)[0] = 0xEB; sector(0)[1] = 0x58; sector(0)[2] = 0x90;           /* a FAT32 boot sector at block 0 */
    memcpy(sector(0) + 446, "\x0e\x1f\xbe\x77\x7c\xac\x22\xc0\x74\x0b\x56\xb4\x0e\xbb\x07\x00", 16);
    sector(0)[510] = 0x55; sector(0)[511] = 0xAA;
    check(scan() == 0 && t.scheme == PART_SCHEME_NONE && t.nparts == 0, "a volume boot sector at block 0: no partitions");
}

static void test_mbr_primary(void) {
    const struct part_entry *e;
    blank();
    mbr_entry(0, 0, 0x80, 0x06, 2048, 8192);
    mbr_entry(0, 2, 0x00, 0x0C, 10240, 20480);
    put32(sector(0) + 440, 0xA1B2C3D4);
    check(scan() == 0 && t.scheme == PART_SCHEME_MBR && t.nparts == 2 && t.disk_sig == 0xA1B2C3D4, "MBR: two primary partitions, disk signature");
    e = part_find(&t, 1);
    check(e && e->start == 2048 && e->count == 8192 && e->type == 0x06 && e->active && !e->logical && !e->extended &&
          e->slot == 0 && e->table_lba == 0, "MBR partition 1: every field");
    e = part_find(&t, 3);
    check(e && e->start == 10240 && e->count == 20480 && e->type == 0x0C && !e->active && e->slot == 2, "MBR partition 3 (slot 2): every field");
    check(part_find(&t, 2) == NULL && part_find(&t, 4) == NULL, "MBR: empty slots are not partitions");
    check(!strcmp(part_type_name(t.scheme, e, false), "FAT32 LBA"), "MBR: type name");

    mbr_entry(0, 1, 0x00, 0x83, 30000, 10000);       /* past the end of the 32768-block disk */
    mbr_entry(0, 3, 0x00, 0x83, 0, 100);             /* over the table itself */
    check(scan() == 0 && t.nparts == 2 && strstr(notes, "partition 2 (type 83, blocks 30000-39999) lies outside the disk") &&
          strstr(notes, "partition 4 (type 83, blocks 0-99) lies outside the disk"), "MBR: partitions outside the disk are left out with a note");
    blank();
    mbr_entry(0, 0, 0x00, 0xEE, 2, 1000);           /* 0xEE that does not start at block 1: not a protective MBR */
    check(scan() == 0 && t.scheme == PART_SCHEME_MBR && t.nparts == 0 && strstr(notes, "GPT protective"),
          "MBR: a stray 0xEE entry is left out");
}

/* p1 0c, p2 extended (0f) holding logical partitions 5, 6, 7 in three EBRs, p3 83 */
static void build_logical(void) {
    blank();
    mbr_entry(0, 0, 0x00, 0x0C, 2048, 8192);
    mbr_entry(0, 1, 0x00, 0x0F, 10240, 20480);       /* blocks 10240-30719 */
    mbr_entry(0, 2, 0x00, 0x83, 30720, 2048);
    mbr_entry(10240, 0, 0x00, 0x83, 2048, 4096);     /* logical 5 at 12288 */
    mbr_entry(10240, 1, 0x00, 0x05, 8192, 6144);     /* next EBR at 18432 */
    mbr_entry(18432, 0, 0x80, 0x0C, 2048, 4096);     /* logical 6 at 20480, active */
    mbr_entry(18432, 1, 0x00, 0x05, 14336, 6144);    /* next EBR at 24576 */
    mbr_entry(24576, 0, 0x00, 0x07, 1024, 5120);     /* logical 7 at 25600 */
}

static void test_mbr_logical(void) {
    const struct part_entry *e;
    build_logical();
    check(scan() == 0 && t.scheme == PART_SCHEME_MBR && t.nparts == 6 && !notes[0], "logical: three primary and three logical partitions, no notes");
    e = part_find(&t, 2);
    check(e && e->extended && e->type == 0x0F && e->start == 10240 && e->count == 20480, "logical: the extended partition is listed as such");
    e = part_find(&t, 5);
    check(e && e->logical && !e->extended && e->start == 12288 && e->count == 4096 && e->type == 0x83 && e->table_lba == 10240 &&
          e->slot == 0, "logical partition 5: every field");
    e = part_find(&t, 6);
    check(e && e->logical && e->start == 20480 && e->count == 4096 && e->type == 0x0C && e->active && e->table_lba == 18432,
          "logical partition 6: every field");
    e = part_find(&t, 7);
    check(e && e->logical && e->start == 25600 && e->count == 5120 && e->type == 0x07 && e->table_lba == 24576, "logical partition 7: every field");
    e = part_find(&t, 3);
    check(e && !e->logical && e->start == 30720, "logical: primary partition 3 after the extended one");

    build_logical();
    put32(sector(24576) + 446 + 16 + 8, 0);          /* EBR 3 links back to EBR 1 */
    sector(24576)[446 + 16 + 4] = 0x05;
    put32(sector(24576) + 446 + 16 + 12, 4096);
    check(scan() == 0 && t.nparts == 6 && strstr(notes, "loops back to block 10240"), "logical: a chain that loops ends, each partition once");

    build_logical();
    put32(sector(18432) + 446 + 16 + 8, 30000);     /* EBR 2 links past the end of the extended partition */
    check(scan() == 0 && t.nparts == 5 && part_find(&t, 6) && !part_find(&t, 7) && strstr(notes, "lies outside the extended partition; the chain ends"),
          "logical: a link outside the extended partition ends the chain");

    build_logical();
    put32(sector(18432) + 446 + 12, 20000);         /* logical 6 runs past the extended partition */
    check(scan() == 0 && t.nparts == 5 && part_find(&t, 5) && part_find(&t, 6) && part_find(&t, 6)->start == 25600 &&
          strstr(notes, "lies outside the extended partition; left out"), "logical: a partition outside the extended one is left out and numbering goes on");

    build_logical();
    memset(sector(10240), 0, 512);                  /* an empty extended partition */
    check(scan() == 0 && t.nparts == 3 && !notes[0], "logical: an extended partition with no logical partitions");
}

/**** GPT ****/

static const u8 G_EFI[16] = { 0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B };
static const u8 G_BASIC[16] = { 0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44, 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7 };
static const u8 G_LINUX[16] = { 0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47, 0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4 };
static const u8 G_DISK[16] = { 0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF };

struct tpart { u32 index; const u8 *type; u64 first, last; u64 attrs; const char *name; };

static u8 entries[128 * 128];

static void gpt_header(u32 lba, u32 alt, u32 elba, u64 first, u64 last, u32 nent, u32 ecrc) {
    u8 *h = sector(lba);
    memset(h, 0, 512);
    memcpy(h, "EFI PART", 8);
    put32(h + 8, 0x00010000);
    put32(h + 12, 92);
    put64(h + 24, lba);
    put64(h + 32, alt);
    put64(h + 40, first);
    put64(h + 48, last);
    memcpy(h + 56, G_DISK, 16);
    put64(h + 72, elba);
    put32(h + 80, nent);
    put32(h + 84, 128);
    put32(h + 88, ecrc);
    put32(h + 16, crc32_bits(h, 92));
}

/* a GPT the way every tool lays it out: protective MBR, header at 1, entries at 2-33, the backup
   entries and header in the last 33 blocks; usable blocks 34 to nblocks - 34 */
static void build_gpt(const struct tpart *p, int n) {
    u32 ecrc, i;
    int k;
    blank();
    memset(entries, 0, sizeof(entries));
    for(k = 0; k < n; k++) {
        u8 *e = entries + (p[k].index - 1) * 128;
        memcpy(e, p[k].type, 16);
        for(i = 0; i < 16; i++) e[16 + i] = (u8)(p[k].index * 16 + i);    /* a unique GUID */
        put64(e + 32, p[k].first);
        put64(e + 40, p[k].last);
        put64(e + 48, p[k].attrs);
        for(i = 0; p[k].name[i] && i < 36; i++) put16(e + 56 + 2 * i, (u8)p[k].name[i]);
    }
    ecrc = crc32_bits(entries, sizeof(entries));
    mbr_entry(0, 0, 0x00, 0xEE, 1, nblocks - 1);
    memcpy(sector(2), entries, sizeof(entries));
    memcpy(sector(nblocks - 33), entries, sizeof(entries));
    gpt_header(1, nblocks - 1, 2, 34, nblocks - 34, 128, ecrc);
    gpt_header(nblocks - 1, 1, nblocks - 33, 34, nblocks - 34, 128, ecrc);
}

static const struct tpart three[] = {
    { 1, G_EFI, 2048, 18431, 0, "EFI system partition" },
    { 2, G_BASIC, 18432, 26623, 0x8000000000000000ull, "SurfOS data" },
    { 5, G_LINUX, 26624, 32734, 0, "a name that is exactly 36 characters" },
};

static int same_three(void) {
    const struct part_entry *a = part_find(&t, 1), *b = part_find(&t, 2), *c = part_find(&t, 5);
    return t.nparts == 3 && a && b && c &&
           a->start == 2048 && a->count == 16384 && a->type == 0xEF && !memcmp(a->type_guid, G_EFI, 16) && a->guid[0] == 16 &&
           a->guid[15] == 31 && !strcmp(a->name, "EFI system partition") && a->attrs == 0 &&
           b->start == 18432 && b->count == 8192 && b->type == 0x07 && !memcmp(b->type_guid, G_BASIC, 16) &&
           !strcmp(b->name, "SurfOS data") && b->attrs == 0x8000000000000000ull &&
           c->start == 26624 && c->count == 6111 && c->type == 0x83 && !strcmp(c->name, "a name that is exactly 36 characters");
}

static void test_gpt(void) {
    static const struct tpart bad[] = {
        { 1, G_BASIC, 2048, 4095, 0, "fine" },
        { 2, G_BASIC, 4096, 0x100000000ull, 0, "beyond 2^32" },
        { 3, G_BASIC, 0x100000000ull, 0x100000800ull, 0, "far beyond" },
        { 4, G_BASIC, 9000, 8999, 0, "backwards" },
        { 5, G_BASIC, 20, 2047, 0, "over the entries" },
        { 6, G_BASIC, 30000, 32766, 0, "over the backup" },
        { 7, G_BASIC, 6000, 40000, 0, "past the end" },
    };
    u8 save[512];

    build_gpt(three, 3);
    check(scan() == 0 && t.scheme == PART_SCHEME_GPT && !t.backup_used && !t.backup_bad && !t.damaged && !notes[0],
          "GPT: found, both copies valid, no notes");
    check(same_three(), "GPT: entries 1, 2 and 5 with every field (types, GUIDs, blocks, attributes, names)");
    check(!memcmp(t.disk_guid, G_DISK, 16) && t.first_usable == 34 && t.last_usable == DISK_BLOCKS - 34 &&
          t.entries_lba == 2 && t.nentries == 128, "GPT: header fields");

    sector(1)[60] ^= 1;                              /* the disk GUID changes under the header CRC */
    check(scan() == 0 && t.backup_used && same_three() && strstr(notes, "primary GPT is damaged (the header CRC does not match): using the backup"),
          "GPT: a damaged primary header: the backup is used, with a warning");
    sector(1)[60] ^= 1;
    sector(5)[100] ^= 0x40;                          /* a primary entry changes under the entries CRC */
    check(scan() == 0 && t.backup_used && same_three() && strstr(notes, "do not match their CRC"), "GPT: damaged primary entries: the backup is used");
    memcpy(save, sector(1), 512);
    memset(sector(1), 0, 512);
    check(scan() == 0 && t.backup_used && same_three() && strstr(notes, "no header at block 1"), "GPT: no primary header: the backup is used");
    memcpy(sector(1), save, 512);
    sector(5)[100] ^= 0x40;

    sector(DISK_BLOCKS - 1)[60] ^= 1;
    check(scan() == 0 && !t.backup_used && t.backup_bad && same_three() && strstr(notes, "backup GPT at block 32767 is damaged"),
          "GPT: a damaged backup: the primary is used, with a note");
    sector(1)[60] ^= 1;
    check(scan() == 0 && t.damaged && t.nparts == 0 && strstr(notes, "no valid GPT"), "GPT: both copies damaged: no partitions");

    build_gpt(three, 3);
    put32(sector(1) + 12, 600);                       /* header size beyond a sector */
    put32(sector(1) + 16, 0);
    put32(sector(1) + 16, crc32_bits(sector(1), 92));
    check(scan() == 0 && t.backup_used && strstr(notes, "a header size of 600"), "GPT: a header size beyond the sector is refused");
    build_gpt(three, 3);
    put64(sector(1) + 24, 7);                        /* MyLBA wrong */
    put32(sector(1) + 16, 0);
    put32(sector(1) + 16, crc32_bits(sector(1), 92));
    check(scan() == 0 && t.backup_used && strstr(notes, "it says it is at block 7"), "GPT: a header that is not where it says is refused");
    build_gpt(three, 3);
    put32(sector(1) + 84, 256);                      /* 256-byte entries */
    put32(sector(1) + 16, 0);
    put32(sector(1) + 16, crc32_bits(sector(1), 92));
    check(scan() == 0 && t.backup_used && strstr(notes, "entries of 256 bytes"), "GPT: an entry size other than 128 is refused");

    build_gpt(bad, 7);
    check(scan() == 0 && t.nparts == 1 && part_find(&t, 1) &&
          strstr(notes, "partition 2 (blocks 4096-4294967296) lies beyond block 4294967295") &&
          strstr(notes, "partition 3 (blocks 4294967296-4294969344) lies beyond block 4294967295") &&
          strstr(notes, "partition 4 ends before it starts") &&
          strstr(notes, "partition 5 (blocks 20-2047) lies outside the usable blocks 34-32734") &&
          strstr(notes, "partition 6 (blocks 30000-32766) lies outside the usable blocks") &&
          strstr(notes, "partition 7 (blocks 6000-40000) runs past the end of the disk"),
          "GPT: entries beyond 2^32, backwards, outside the usable area or the disk are refused");

    build_gpt(three, 3);
    memset(sector(0) + 446, 0, 16);                  /* an MBR with a FAT32 partition and a stale GPT behind it */
    mbr_entry(0, 0, 0x00, 0x0C, 2048, 8192);
    check(scan() == 0 && t.scheme == PART_SCHEME_MBR && t.nparts == 1 && part_find(&t, 1)->type == 0x0C,
          "GPT without a protective MBR entry: the MBR is used, the stale GPT ignored");
    mbr_entry(0, 1, 0x00, 0xEE, 1, 100);            /* a hybrid MBR: 0xEE plus another entry */
    check(scan() == 0 && t.scheme == PART_SCHEME_GPT && same_three(), "a hybrid MBR: the GPT is used");

    build_gpt(three, 3);                             /* an image made for a bigger disk, cut short */
    nblocks = DISK_BLOCKS - 4000;
    check(scan() == 0 && t.scheme == PART_SCHEME_GPT && !t.backup_used && t.backup_bad && t.last_usable == nblocks - 1 &&
          t.nparts == 2 && part_find(&t, 1) && part_find(&t, 2) && strstr(notes, "partition 5 (blocks 26624-32734) runs past the end") &&
          strstr(notes, "describes a bigger disk"), "GPT on a disk smaller than the table says: what fits is used");
}

/**** the boot drive's label ****/

static void fat_boot_sector(u8 *bs, bool fat32, const char *label) {
    memset(bs, 0, 512);
    bs[0] = 0xEB; bs[1] = fat32 ? 0x58 : 0x3C; bs[2] = 0x90;
    memcpy(bs + 3, "mkfs.fat", 8);
    put16(bs + 11, 512);
    bs[13] = 8;
    put16(bs + 14, fat32 ? 32 : 4);
    bs[16] = 2;
    if(fat32) {
        put32(bs + 32, 500000);
        put32(bs + 36, 3900);
        bs[64] = 0x80; bs[66] = 0x29;
        memcpy(bs + 71, label, 11);
        memcpy(bs + 82, "FAT32   ", 8);
    } else {
        put16(bs + 17, 512);
        put16(bs + 19, 30720);
        put16(bs + 22, 32);
        bs[36] = 0x80; bs[38] = 0x29;
        memcpy(bs + 43, label, 11);
        memcpy(bs + 54, "FAT16   ", 8);
    }
    bs[510] = 0x55; bs[511] = 0xAA;
}

static void test_fat_label(void) {
    u8 bs[512];
    char label[12];
    fat_boot_sector(bs, true, "SURFOSUSB  ");
    check(part_fat_label(bs, label) && !strcmp(label, PART_BOOT_LABEL), "FAT32: the label at offset 71");
    fat_boot_sector(bs, false, "SURFOSUSB  ");
    check(part_fat_label(bs, label) && !strcmp(label, PART_BOOT_LABEL), "FAT16: the label at offset 43");
    fat_boot_sector(bs, true, "DATA       ");
    check(part_fat_label(bs, label) && !strcmp(label, "DATA       "), "FAT32: another label");
    fat_boot_sector(bs, true, "SURFOSUSB  ");
    bs[66] = 0x28;
    check(!part_fat_label(bs, label), "FAT32 without the extended boot signature: no label");
    fat_boot_sector(bs, false, "SURFOSUSB  ");
    bs[0] = 0x00;
    check(!part_fat_label(bs, label), "no jump instruction: not a FAT boot sector");
    fat_boot_sector(bs, false, "SURFOSUSB  ");
    put16(bs + 11, 1000);
    check(!part_fat_label(bs, label), "1000 bytes per sector: not a FAT boot sector");
    fat_boot_sector(bs, true, "SURFOSUSB  ");
    bs[511] = 0;
    check(!part_fat_label(bs, label), "no 55 AA: not a FAT boot sector");
    memset(bs, 0, 512);
    check(!part_fat_label(bs, label), "zeros: not a FAT boot sector");
}

int main(void) {
    test_names();
    test_blank_and_errors();
    test_mbr_primary();
    test_mbr_logical();
    test_gpt();
    test_fat_label();
    printf("PARTITION UNIT TESTS %s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    exit(failures ? 1 : 0);
    return 0;
}
