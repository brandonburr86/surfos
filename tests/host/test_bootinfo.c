/* Host-side unit tests for the boot information the USB boot depends on:
   kernel/multiboot2.c (the Multiboot 2 parser) and mm/memrange.c (which frames are free
   once the kernel, modules and framebuffer are reserved). Built like the kernel objects
   (SurfOS headers, blibc renamed by surf_names.h) and run by `make unittest`. */
#include <stdio.h>
#include <string.h>
#include <surfos/types.h>
#include <surfos/multiboot.h>
#include <mm/memrange.h>

void exit(int status);

static int failures, checks;

static void check(int ok, const char *what) {
    checks++;
    if(!ok) { failures++; printf("FAIL: %s\n", what); }
}

/**** a Multiboot 2 information block builder ****/

static u8 buf[16384] __attribute__((aligned(8)));
static u32 used;

static void put32(u32 off, u32 v) { memcpy(buf + off, &v, 4); }

static void mbi_begin(void) {
    memset(buf, 0, sizeof(buf));
    used = 8;
}

/* a tag with `len` body bytes; returns the body's offset */
static u32 tag(u32 type, const void *body, u32 len) {
    u32 at = used;
    put32(at, type);
    put32(at + 4, 8 + len);
    if(body && len) memcpy(buf + at + 8, body, len);
    used = (at + 8 + len + 7) & ~7u;
    return at + 8;
}

static u32 mbi_end(void) {
    tag(MB2_TAG_END, NULL, 0);
    put32(0, used);
    return used;
}

static u32 str_tag(u32 type, const char *s) { return tag(type, s, strlen(s) + 1); }

static void module_tag(u32 start, u32 end, const char *name) {
    u8 b[80];
    u32 n = strlen(name) + 1;
    memcpy(b, &start, 4);
    memcpy(b + 4, &end, 4);
    memcpy(b + 8, name, n);
    tag(MB2_TAG_MODULE, b, 8 + n);
}

struct e820 { u64 base, len; u32 type; };

static void mmap_tag(const struct e820 *e, u32 n, u32 esize) {
    u8 b[8 + 600 * 32];
    u32 i, v = esize, zero = 0;
    memset(b, 0, sizeof(b));
    memcpy(b, &v, 4);
    memcpy(b + 4, &zero, 4);
    for(i = 0; i < n; i++) {
        memcpy(b + 8 + i * esize, &e[i].base, 8);
        memcpy(b + 8 + i * esize + 8, &e[i].len, 8);
        memcpy(b + 8 + i * esize + 16, &e[i].type, 4);
    }
    tag(MB2_TAG_MMAP, b, 8 + n * esize);
}

static void fb_tag(u64 addr, u32 pitch, u32 w, u32 h, u8 bpp, u8 type, u32 body_len) {
    u8 b[32];
    memset(b, 0, sizeof(b));
    memcpy(b, &addr, 8);
    memcpy(b + 8, &pitch, 4);
    memcpy(b + 12, &w, 4);
    memcpy(b + 16, &h, 4);
    b[20] = bpp;
    b[21] = type;
    b[24] = 16; b[25] = 8; b[26] = 8; b[27] = 8; b[28] = 0; b[29] = 8;   /* red 16:8 green 8:8 blue 0:8 */
    tag(MB2_TAG_FRAMEBUFFER, b, body_len);
}

static void rsdp(u8 *r, int v2, int break_sum) {
    u32 i, length = 36;
    u64 xsdt = 0x7FFE1000ULL;
    u32 rsdt = 0x7FFE0000;
    u8 s = 0;
    memset(r, 0, 36);
    memcpy(r, "RSD PTR ", 8);
    memcpy(r + 9, "LENOVO", 6);
    r[15] = v2 ? 2 : 0;
    memcpy(r + 16, &rsdt, 4);
    for(i = 0; i < 20; i++) s += r[i];
    r[8] = (u8)-s;
    if(v2) {
        memcpy(r + 20, &length, 4);
        memcpy(r + 24, &xsdt, 8);
        for(s = 0, i = 0; i < 36; i++) s += r[i];
        r[32] = (u8)-s;
    }
    if(break_sum) r[10] ^= 0x55;
}

static struct boot_info bi;

static void test_valid_block(void) {
    struct e820 map[3] = {
        { 0x0, 0x9F000, MB_MEMORY_AVAILABLE },
        { 0x100000, 0x7FE00000, MB_MEMORY_AVAILABLE },
        { 0x100000000ULL, 0x180000000ULL, MB_MEMORY_AVAILABLE },
    };
    u8 r[36], unknown[12] = "ignore me!";
    u32 meminfo[2] = { 639, 2096128 };
    u64 systab = 0x7F9E7018ULL;
    u32 n;

    mbi_begin();
    str_tag(MB2_TAG_CMDLINE, "profile=laptop kbd=poll");
    str_tag(MB2_TAG_LOADER_NAME, "GRUB 2.06-13+deb12u1");
    module_tag(0x2000000, 0x2039800, "initrd.tar");
    tag(MB2_TAG_BASIC_MEMINFO, meminfo, 8);
    mmap_tag(map, 3, 24);
    fb_tag(0x4000000000ULL, 7680, 1920, 1080, 32, MB_FB_RGB, 30);
    rsdp(r, 1, 0);
    tag(MB2_TAG_ACPI_NEW, r, 36);
    tag(99, unknown, sizeof(unknown));                          /* an optional tag nobody knows */
    tag(MB2_TAG_EFI64, &systab, 8);
    n = mbi_end();

    check(mb2_parse(buf, n, &bi) == MB2_OK, "a valid block parses");
    check(bi.valid && bi.protocol == 2 && bi.error == 0, "valid, protocol 2");
    check(!strcmp(bi.cmdline, "profile=laptop kbd=poll"), "command line copied");
    check(!strcmp(bi.loader, "GRUB 2.06-13+deb12u1"), "loader name copied");
    check(bootinfo_has_arg(&bi, "profile=laptop") && bootinfo_has_arg(&bi, "kbd=poll"), "command line words found");
    check(!bootinfo_has_arg(&bi, "profile=lap") && !bootinfo_has_arg(&bi, "laptop") && !bootinfo_has_arg(&bi, ""),
          "only whole words match");
    check(bi.nmods == 1 && bi.mods[0].start == 0x2000000 && bi.mods[0].end == 0x2039800 && !strcmp(bi.mods[0].name, "initrd.tar"),
          "module start, end and name");
    check(bi.mem_lower_kb == 639 && bi.mem_upper_kb == 2096128, "basic memory information");
    check(bi.nregions == 3 && bi.regions[2].base == 0x100000000ULL && bi.regions[2].len == 0x180000000ULL,
          "memory map keeps 64-bit bases and lengths above 4 GB");
    check(bi.fb.present && bi.fb.addr == 0x4000000000ULL && bi.fb.pitch == 7680 && bi.fb.width == 1920 &&
          bi.fb.height == 1080 && bi.fb.bpp == 32 && bi.fb.type == MB_FB_RGB, "framebuffer above 4 GB, not truncated");
    check(bi.fb.red_pos == 16 && bi.fb.red_size == 8 && bi.fb.green_pos == 8 && bi.fb.blue_pos == 0 && bi.fb.blue_size == 8,
          "framebuffer colour fields");
    check(bi.acpi.present && bi.acpi.revision == 2 && bi.acpi.rsdt == 0x7FFE0000 && bi.acpi.xsdt == 0x7FFE1000ULL &&
          !strcmp(bi.acpi.oem, "LENOVO"), "ACPI 2.0 root pointer");
    check(bi.efi && bi.efi_systab == 0x7F9E7018ULL && !bi.efi_bs, "EFI system table tag");
}

static void test_rejects(void) {
    u32 n;
    u8 r[36];

    check(mb2_parse(NULL, 64, &bi) == MB2_E_ENVELOPE, "NULL block");
    mbi_begin();
    n = mbi_end();
    check(mb2_parse(buf, n, &bi) == MB2_OK && bi.nregions == 0, "an empty block (just the end tag) is valid");
    check(mb2_parse(buf, n - 1, &bi) == MB2_E_ENVELOPE, "total size larger than the bytes available");
    check(mb2_parse(buf, 12, &bi) == MB2_E_ENVELOPE, "fewer than 16 bytes");
    put32(0, 8);
    check(mb2_parse(buf, sizeof(buf), &bi) == MB2_E_ENVELOPE, "total size below 16");

    mbi_begin();
    str_tag(MB2_TAG_CMDLINE, "profile=laptop");
    n = mbi_end();
    put32(8 + 4, 200);                                          /* the command line tag claims 200 bytes */
    check(mb2_parse(buf, n, &bi) == MB2_E_TAG, "a tag running past the end");
    check(!bi.valid && bi.error == MB2_E_TAG && bi.cmdline[0] == 0, "a rejected block leaves nothing half-parsed");

    mbi_begin();
    str_tag(MB2_TAG_CMDLINE, "x");
    n = mbi_end();
    put32(8 + 4, 4);
    check(mb2_parse(buf, n, &bi) == MB2_E_TAGSIZE, "a tag smaller than its header");

    mbi_begin();
    str_tag(MB2_TAG_CMDLINE, "abc");                            /* size 12: the next tag starts at 24, not 20 */
    n = mbi_end();
    check(mb2_parse(buf, n, &bi) == MB2_OK && !strcmp(bi.cmdline, "abc"), "tags padded to 8 bytes");
    memmove(buf + 20, buf + 24, 8);                             /* end tag where padding is ignored */
    put32(0, 28);
    check(mb2_parse(buf, 28, &bi) != MB2_OK, "an end tag that is not 8-byte aligned is not found");

    mbi_begin();
    str_tag(MB2_TAG_CMDLINE, "no end tag");
    put32(0, used);
    check(mb2_parse(buf, used, &bi) == MB2_E_NOEND, "missing end tag");

    mbi_begin();
    tag(MB2_TAG_END, "12345678", 8);                            /* an end tag of 16 bytes */
    put32(0, used);
    check(mb2_parse(buf, used, &bi) == MB2_E_TAGSIZE, "end tag with the wrong size");

    mbi_begin();
    tag(MB2_TAG_CMDLINE, "unterminated", 12);
    n = mbi_end();
    check(mb2_parse(buf, n, &bi) == MB2_E_STRING, "a string without its terminator");

    mbi_begin();
    module_tag(0x3000000, 0x2000000, "backwards");
    n = mbi_end();
    check(mb2_parse(buf, n, &bi) == MB2_E_MODULE, "a module that ends before it starts");

    mbi_begin();
    fb_tag(0xC0000000, 4096, 1024, 768, 32, MB_FB_RGB, 24);     /* type RGB but no colour fields */
    n = mbi_end();
    check(mb2_parse(buf, n, &bi) == MB2_E_TAGSIZE, "an RGB framebuffer tag without colour fields");

    mbi_begin();
    rsdp(r, 1, 1);
    tag(MB2_TAG_ACPI_NEW, r, 36);
    n = mbi_end();
    check(mb2_parse(buf, n, &bi) == MB2_OK && !bi.acpi.present, "an ACPI pointer with a bad checksum is ignored");

    mbi_begin();
    rsdp(r, 1, 0);
    tag(MB2_TAG_ACPI_NEW, r, 36);
    rsdp(r, 0, 0);
    tag(MB2_TAG_ACPI_OLD, r, 20);
    n = mbi_end();
    check(mb2_parse(buf, n, &bi) == MB2_OK && bi.acpi.present && bi.acpi.xsdt == 0x7FFE1000ULL,
          "an ACPI 1.0 pointer does not replace the 2.0 one");

    mbi_begin();
    rsdp(r, 0, 0);
    tag(MB2_TAG_ACPI_OLD, r, 20);
    n = mbi_end();
    check(mb2_parse(buf, n, &bi) == MB2_OK && bi.acpi.present && bi.acpi.revision == 0 && bi.acpi.xsdt == 0,
          "ACPI 1.0 pointer alone");
}

static void test_mmap_limits(void) {
    static struct e820 map[600];
    u32 i, n;

    for(i = 0; i < 600; i++) {
        map[i].base = 0x100000ULL * (i + 1);
        map[i].len = 0x80000;
        map[i].type = (i & 1) ? MB_MEMORY_RESERVED : MB_MEMORY_AVAILABLE;
    }
    mbi_begin();
    mmap_tag(map, MB_MAX_REGIONS, 24);
    n = mbi_end();
    check(mb2_parse(buf, n, &bi) == MB2_OK && bi.nregions == MB_MAX_REGIONS, "MB_MAX_REGIONS (512) memory map entries fit");
    mbi_begin();
    mmap_tag(map, MB_MAX_REGIONS + 1, 24);
    n = mbi_end();
    check(mb2_parse(buf, n, &bi) == MB2_E_TOOMANY, "one entry more fails instead of being dropped");

    mbi_begin();
    mmap_tag(map, 4, 32);                                       /* a bigger entry size is allowed */
    n = mbi_end();
    check(mb2_parse(buf, n, &bi) == MB2_OK && bi.nregions == 4 && bi.regions[3].base == 0x400000, "32-byte entries");
    mbi_begin();
    mmap_tag(map, 4, 20);
    n = mbi_end();
    check(mb2_parse(buf, n, &bi) == MB2_E_MMAP, "an entry size below 24");

    map[0].base = 0xFFFFFFFFFFFFF000ULL;
    map[0].len = 0x2000;
    mbi_begin();
    mmap_tag(map, 1, 24);
    n = mbi_end();
    check(mb2_parse(buf, n, &bi) == MB2_E_MMAP, "an entry that wraps past 2^64");

    mbi_begin();
    for(i = 0; i < 10; i++) module_tag(0x2000000 + i * 0x10000, 0x2008000 + i * 0x10000, "m");
    n = mbi_end();
    check(mb2_parse(buf, n, &bi) == MB2_OK && bi.nmods == MB_MAX_MODULES && bi.mods_dropped == 2,
          "modules beyond eight are counted, not used");
}

/**** free frames ****/

static u64 got_base[16], got_end[16];
static u_int got;

static void emit(u64 base, u64 end, void *arg) {
    if(got < 16) { got_base[got] = base; got_end[got] = end; }
    got++;
}

static u_int pieces(u64 base, u64 len, u64 lo, u64 hi, const struct mem_range *r, u_int n) {
    got = 0;
    return memrange_free_pieces(base, len, lo, hi, r, n, emit, NULL);
}

static void test_memrange(void) {
    struct mem_range res[4];
    const u64 top = 0xFFFFF000ULL;

    check(pieces(0x100000, 0x100000, 0, top, NULL, 0) == 1 && got_base[0] == 0x100000 && got_end[0] == 0x200000,
          "a region with nothing reserved is one piece");
    res[0].base = 0; res[0].end = 0xE85000;                     /* low memory, kernel and arenas */
    check(pieces(0x100000, 0x3F00000, 0, top, res, 1) == 1 && got_base[0] == 0xE85000 && got_end[0] == 0x4000000,
          "low memory and the kernel are left out");
    res[1].base = 0x2000100; res[1].end = 0x2040010;            /* an unaligned module */
    check(pieces(0x100000, 0x3F00000, 0, top, res, 2) == 2 && got_end[0] == 0x2000000 && got_base[1] == 0x2041000,
          "a page that touches a module is left out on both sides");
    check(pieces(0x1000000, 0x800, 0, top, NULL, 0) == 0, "less than a page is nothing");
    check(pieces(0x1000800, 0x1800, 0, top, NULL, 0) == 1 && got_base[0] == 0x1001000 && got_end[0] == 0x1002000,
          "unaligned region edges round inwards");
    check(pieces(0x100000000ULL, 0x100000000ULL, 0, top, NULL, 0) == 0, "RAM above 4 GB gives no frames (no PAE)");
    check(pieces(0xC0000000ULL, 0x80000000ULL, 0, top, NULL, 0) == 1 && got_end[0] == top,
          "a region across 4 GB stops below the last page");
    check(pieces(0xFFFFFFFFFFFFF000ULL, 0x2000, 0, ~0ULL, NULL, 0) == 0, "a region wrapping past 2^64 gives nothing");
    check(pieces(0x1000000, 0x1000000, 0x1800000, top, NULL, 0) == 1 && got_base[0] == 0x1800000, "lower bound clips");
    res[0].base = 0x2000000; res[0].end = 0x1000000;            /* empty: end before base */
    check(pieces(0x1000000, 0x2000000, 0, top, res, 1) == 1, "an empty reservation is ignored");
    res[0].base = 0x1000000; res[0].end = 0x1800000;
    res[1].base = 0x1400000; res[1].end = 0x2000000;            /* overlapping reservations */
    res[2].base = 0x2800000; res[2].end = 0x2801000;
    check(pieces(0x1000000, 0x2000000, 0, top, res, 3) == 2 && got_base[0] == 0x2000000 && got_end[0] == 0x2800000 &&
          got_base[1] == 0x2801000 && got_end[1] == 0x3000000, "overlapping reservations and a hole between them");
    check(memrange_overlaps(0x17FF000, 0x1800000, res, 3) && !memrange_overlaps(0x2000000, 0x2800000, res, 3),
          "overlap test");
    res[0].base = 0; res[0].end = 0x100000000ULL;
    check(pieces(0x100000, 0x7FF00000, 0, top, res, 1) == 0, "no usable low RAM at all");
}

int main(void) {
    test_valid_block();
    test_rejects();
    test_mmap_limits();
    test_memrange();
    printf("%s: %d checks, %d failures\n", failures ? "BOOTINFO UNIT TESTS FAILED" : "BOOTINFO UNIT TESTS PASSED", checks, failures);
    exit(failures ? 1 : 0);
    return 0;
}
