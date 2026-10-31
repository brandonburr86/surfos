/*
SurfOS ACPI Export Tests
--------------------
File: test_acpidump.c Date: 10/12/26
--------------------
Real ACPI discovery and copying over a replaceable simulated mapping window.
*/
#include <surfos/acpi.h>
#include <surfos/multiboot.h>
#include <surfos/task.h>
#include <mm/memory.h>
#include <mm/kalloc.h>
#include <string.h>
#include <stdio.h>

void *malloc(unsigned long size);
void free(void *pointer);
int cmd_acpidump(int argc, char **argv);
int cmd_acpinvs(int argc, char **argv);
struct boot_info bootinfo;
static surf_task task;
surf_task *curTask = &task;
static unsigned checks, failures, critical, maps, live, allocations;
static bool exporting, allocation_failed, change_length, change_signature, interrupt_allocation;
static u64 fail_after, mutate_header_after, corrupt_after;
#define CAPACITY (TMPMAP_END - TMPMAP_START)
struct region { u64 address; u8 data[1048576U + 8]; };
static struct region regions[5];
static u8 window[CAPACITY];

static void check(bool ok, const char *what) {
    checks++;
    if(!ok) { failures++; printf("FAIL: %s\n", what); }
}
void kcritical_enter(void) { critical++; }
void kcritical_leave(void) { check(critical == 1, "balanced non-nested mapping critical section"); critical--; }
void kprintf(const char *format, ...) { (void)format; }
/* 10/25/26 (robustness, B4): soft off's entry (kernel/acpi.c) is built with the rest and never run here */
void mdelay(u_long ms) { (void)ms; }
void *ioremap(u_long physical, u_long size, bool cached) { (void)physical; (void)size; (void)cached; return NULL; }
static void put32(u8 *p, u32 value) { memcpy(p, &value, 4); }
static void put64(u8 *p, u64 value) { memcpy(p, &value, 8); }
static void checksum(u8 *p, u32 size) {
    u32 i;
    u8 sum = 0;
    p[9] = 0;
    for(i = 0; i < size; i++) sum += p[i];
    p[9] = -sum;
}
static void table(unsigned index, const char *signature, u32 size) {
    u32 i;
    memset(regions[index].data, 0, sizeof(regions[index].data));
    for(i = 36; i < size; i++) regions[index].data[i] = (u8)(i * 17 + i / 1024);
    memcpy(regions[index].data, signature, 4);
    put32(regions[index].data + 4, size);
    regions[index].data[8] = 2;
    checksum(regions[index].data, size);
}
void *vmm_temp_map(u64 physical, u_long size) {
    unsigned i;
    maps++;
    if(exporting) check(critical == 1, "every export mapping is inside the critical section");
    if(!physical || size + (physical & (PAGE_SIZE - 1)) > CAPACITY || physical > 0x100000000ULL - size)
        return NULL;
    if(exporting && fail_after && physical >= fail_after) return NULL;
    if(exporting && mutate_header_after && physical >= mutate_header_after) {
        regions[1].data[8]++;
        checksum(regions[1].data, 2 * CAPACITY);
        mutate_header_after = 0;
    }
    for(i = 5; i-- > 0;) if(physical >= regions[i].address &&
            physical - regions[i].address < sizeof(regions[i].data) &&
            size <= sizeof(regions[i].data) - (physical - regions[i].address)) {
        u32 offset = physical - regions[i].address;
        if(exporting && corrupt_after && physical >= corrupt_after) {
            regions[i].data[offset] ^= 1;
            corrupt_after = 0;
        }
        memset(window, 0xa5, sizeof(window));
        memcpy(window, regions[i].data + offset, size);
        return window;
    }
    return NULL;
}
void *kalloc(size_t size) {
    void *result;
    allocations++;
    check(!critical, "allocation never occurs while holding the mapping critical section");
    if(allocation_failed) return NULL;
    if(change_length) {
        put32(regions[1].data + 4, 80);
        checksum(regions[1].data, 80);
    }
    if(change_signature) {
        memcpy(regions[1].data, "MSDM", 4);
        checksum(regions[1].data, 64);
    }
    result = malloc(size);
    if(result) live++;
    if(interrupt_allocation) task.intr = TI_INT;
    return result;
}
void kfree(void *data) {
    check(!critical, "free never occurs while holding the mapping critical section");
    if(data) { live--; free(data); }
}

static void setup(u32 fadt_length, u64 extended, u32 legacy) {
    unsigned i;
    exporting = false;
    allocation_failed = change_length = change_signature = interrupt_allocation = false;
    fail_after = mutate_header_after = corrupt_after = 0;
    task.intr = 0;
    for(i = 0; i < 5; i++) regions[i].address = (i + 1) * 0x100000;
    table(0, "XSDT", 60);
    table(1, "DSDT", 64);
    table(2, "SSDT", 48);
    table(3, "FACP", fadt_length);
    table(4, "SSDT", 52);
    if(fadt_length >= 44) put32(regions[3].data + 40, legacy);
    if(fadt_length >= 148) put64(regions[3].data + 140, extended);
    checksum(regions[3].data, fadt_length);
    put64(regions[0].data + 36, regions[3].address);
    put64(regions[0].data + 44, regions[2].address);
    put64(regions[0].data + 52, regions[4].address);
    checksum(regions[0].data, 60);
    memset(&bootinfo, 0, sizeof(bootinfo));
    bootinfo.efi = true;
    bootinfo.acpi.present = true;
    bootinfo.acpi.revision = 2;
    bootinfo.acpi.xsdt = regions[0].address;
    check(init_acpi() == 0, "real ACPI discovery accepts the valid root");
    exporting = true;
}

/* 10/26/26 (L21b): the FADT's event and GPE blocks for the next s5_setup(), as the tests set them: the legacy ports
   and lengths, the extended forms of PM1a's event block and of GPE0 (space, address), and the sleep status register */
static struct {
    u32 pm1a, pm1b, gpe0, gpe1;
    u8 pm1_len, gpe0_len, gpe1_len;
    u8 x_pm1a_space, x_gpe0_space, sts_space;
    u64 x_pm1a, x_gpe0, sts;
} blocks;

/* 10/25/26 (robustness, B4; the review of B4): soft off's values from a FADT and a DSDT built here (the FADT's other
   fields zero, so nothing but what is set reads as a register): the PM1 control blocks, SMI_CMD, the flags (bit 20:
   hardware-reduced) and the sleep control register; the DSDT is the header and aml */
static void s5_setup(u32 flags, u16 pm1a, u16 pm1b, const u8 *aml, u32 aml_len, u8 sleep_space, u64 sleep_addr) {
    unsigned i;
    exporting = false;
    allocation_failed = change_length = change_signature = interrupt_allocation = false;
    fail_after = mutate_header_after = corrupt_after = 0;
    task.intr = 0;
    for(i = 0; i < 5; i++) regions[i].address = (i + 1) * 0x100000;
    table(0, "XSDT", 60);
    table(1, "DSDT", 36 + aml_len);
    memcpy(regions[1].data + 36, aml, aml_len);
    checksum(regions[1].data, 36 + aml_len);
    table(2, "SSDT", 48);
    table(3, "FACP", 268);
    table(4, "SSDT", 52);
    memset(regions[3].data + 36, 0, 268 - 36);
    put64(regions[3].data + 140, regions[1].address);  /* X_DSDT */
    put32(regions[3].data + 48, 0xb2);                  /* SMI_CMD, ACPI_ENABLE */
    regions[3].data[52] = 0xf1;
    put32(regions[3].data + 64, pm1a);
    put32(regions[3].data + 68, pm1b);
    regions[3].data[89] = 2;                            /* PM1_CNT_LEN */
    put32(regions[3].data + 112, flags);
    regions[3].data[244] = sleep_space;                 /* SLEEP_CONTROL_REG: space, width, offset, access, address */
    put64(regions[3].data + 248, sleep_addr);
    put32(regions[3].data + 56, blocks.pm1a);           /* 10/26/26 (L21b): PM1a/PM1b_EVT_BLK, GPE0/GPE1_BLK */
    put32(regions[3].data + 60, blocks.pm1b);
    put32(regions[3].data + 80, blocks.gpe0);
    put32(regions[3].data + 84, blocks.gpe1);
    regions[3].data[88] = blocks.pm1_len;               /* PM1_EVT_LEN, GPE0_BLK_LEN, GPE1_BLK_LEN */
    regions[3].data[92] = blocks.gpe0_len;
    regions[3].data[93] = blocks.gpe1_len;
    regions[3].data[148] = blocks.x_pm1a_space;         /* X_PM1a_EVT_BLK, X_GPE0_BLK, SLEEP_STATUS_REG */
    put64(regions[3].data + 152, blocks.x_pm1a);
    regions[3].data[220] = blocks.x_gpe0_space;
    put64(regions[3].data + 224, blocks.x_gpe0);
    regions[3].data[256] = blocks.sts_space;
    put64(regions[3].data + 260, blocks.sts);
    checksum(regions[3].data, 268);
    put64(regions[0].data + 36, regions[3].address);
    put64(regions[0].data + 44, regions[2].address);
    put64(regions[0].data + 52, regions[4].address);
    checksum(regions[0].data, 60);
    memset(&bootinfo, 0, sizeof(bootinfo));
    bootinfo.efi = true;
    bootinfo.acpi.present = true;
    bootinfo.acpi.revision = 2;
    bootinfo.acpi.xsdt = regions[0].address;
    check(init_acpi() == 0, "soft off's tables: the root is accepted");
}

static void expect_s5(bool ok, u8 a, u8 b, const char *why, const char *what) {
    struct acpi_s5 s5;
    int r = acpi_s5_find(&s5);
    check(r == (ok ? 0 : -1) && s5.ok == ok && (!ok || (s5.typa == a && s5.typb == b)) &&
          (ok || (s5.why && strstr(s5.why, why))), what);
    check(!critical && !live, "soft off's look leaks no mapping critical section or allocation");
}

static void test_s5(void) {
    static const u8 ovmf[] = { 0x08, '_', 'S', '5', '_', 0x12, 0x06, 0x04, 0x00, 0x00, 0x00, 0x00 };
    static const u8 intel[] = { 0x08, '_', 'S', '5', '_', 0x12, 0x08, 0x04, 0x0a, 0x07, 0x0a, 0x07, 0x00, 0x00 };
    static const u8 wide[] = { 0x08, '_', 'S', '5', '_', 0x12, 0x08, 0x04, 0x0a, 0x0d, 0x0a, 0x0d, 0x00, 0x00 };
    static const u8 s4[] = { 0x08, '_', 'S', '4', '_', 0x12, 0x06, 0x04, 0x00, 0x00, 0x00, 0x00 };
    s5_setup(0, 0x604, 0, ovmf, sizeof(ovmf), 0, 0);
    check(acpi.pm1a_cnt == 0x604 && !acpi.pm1b_cnt && acpi.smi_cmd == 0xb2 && acpi.acpi_enable == 0xf1,
          "the FADT's PM1a control block, SMI_CMD and ACPI_ENABLE");
    expect_s5(true, 0, 0, NULL, "OVMF's \\_S5_: SLP_TYP 0 and 0");
    s5_setup(0, 0x1804, 0x1904, intel, sizeof(intel), 0, 0);
    expect_s5(true, 7, 7, NULL, "Intel's: 7 and 7, with a PM1b");
    s5_setup(0, 0x1804, 0x1904, wide, sizeof(wide), 0, 0);
    expect_s5(false, 0, 0, "out of range", "SLP_TYP 0x0D is refused, never cut to 5 (S3 on Intel's)");
    s5_setup(0, 0, 0, intel, sizeof(intel), 0, 0);
    expect_s5(false, 0, 0, "no PM1 control block", "no PM1a control block: no soft off");
    s5_setup(0, 0x604, 0, s4, sizeof(s4), 0, 0);
    expect_s5(false, 0, 0, "declares no", "a DSDT without \\_S5_: no soft off");
    s5_setup(1u << 20, 0, 0, intel, sizeof(intel), 1, 0x404);
    expect_s5(true, 7, 7, NULL, "hardware-reduced ACPI: the sleep control register at a port");
    s5_setup(1u << 20, 0, 0, intel, sizeof(intel), 1, 0x10060);
    expect_s5(false, 0, 0, "can write", "a sleep control register past port 0xFFFF is refused (it would be port 0x60)");
    s5_setup(1u << 20, 0, 0, intel, sizeof(intel), 0, 0xfed01000);
    expect_s5(true, 7, 7, NULL, "hardware-reduced ACPI: the sleep control register in memory");
}

/* 10/26/26 (L21b): soft off's preparation, as ACPICA's: the writes acpi_s5_clear() hands out, in order */
static struct { unsigned n; struct { u8 space, bits; u64 addr; u16 value; } w[600]; } writes;
static void record(void *arg, u8 space, u64 addr, u8 bits, u16 value) {
    (void)arg;
    if(writes.n < 600) {
        writes.w[writes.n].space = space;
        writes.w[writes.n].bits = bits;
        writes.w[writes.n].addr = addr;
        writes.w[writes.n].value = value;
    }
    writes.n++;
}

/* acpi_s5_clear() after s5_setup(): its count is the writes made; write k is (space, addr, bits, value) */
static unsigned clear_writes(void) {
    unsigned n;
    memset(&writes, 0, sizeof(writes));
    n = acpi_s5_clear(record, NULL);
    check(n == writes.n, "acpi_s5_clear()'s count is the writes it made");
    return n;
}
static bool wrote(unsigned k, u8 space, u64 addr, u8 bits, u16 value) {
    return k < writes.n && writes.w[k].space == space && writes.w[k].addr == addr && writes.w[k].bits == bits &&
           writes.w[k].value == value;
}
/* the writes from k on: each byte of n from port, 8 bits, value */
static bool wrote_bytes(unsigned k, u16 port, unsigned n, u16 value) {
    unsigned i;
    for(i = 0; i < n; i++) if(!wrote(k + i, 1, port + i, 8, value)) return false;
    return true;
}

static void test_s5_clear(void) {
    static const u8 intel[] = { 0x08, '_', 'S', '5', '_', 0x12, 0x08, 0x04, 0x0a, 0x07, 0x0a, 0x07, 0x00, 0x00 };
    unsigned n;
    /* an Intel PCH's: PM1a's event block at PMBASE 0x1800, GPE0's 32 bytes at 0x1860 (16 status, then 16 enables) */
    memset(&blocks, 0, sizeof(blocks));
    blocks.pm1a = 0x1800; blocks.pm1_len = 4;
    blocks.gpe0 = 0x1860; blocks.gpe0_len = 32;
    s5_setup(0, 0x1804, 0, intel, sizeof(intel), 0, 0);
    check(acpi.pm1a_evt == 0x1800 && !acpi.pm1b_evt && acpi.gpe_blk[0] == 0x1860 && acpi.gpe_len[0] == 32 &&
          !acpi.gpe_blk[1], "the FADT's PM1a event block and GPE0 block are read");
    n = clear_writes();
    check(n == 33 && wrote_bytes(0, 0x1870, 16, 0) && wrote(16, 1, 0x1800, 16, ACPI_PM1_STS_FIXED) &&
          wrote_bytes(17, 0x1860, 16, 0xff),
          "Intel's: GPE0's 16 enable bytes zeroed, then the PM1 status cleared (0xC731), then the 16 status bytes");
    check(ACPI_PM1_STS_FIXED == 0xC731 && (ACPI_PM1_STS_FIXED & 0x8000), "the fixed status bits written include WAK_STS");
    /* QEMU's PIIX4: PM1a's event block at 0x600, GPE0's 4 bytes at 0xafe0 */
    memset(&blocks, 0, sizeof(blocks));
    blocks.pm1a = 0x600; blocks.pm1_len = 4;
    blocks.gpe0 = 0xafe0; blocks.gpe0_len = 4;
    s5_setup(0, 0x604, 0, intel, sizeof(intel), 0, 0);
    n = clear_writes();
    check(n == 5 && wrote_bytes(0, 0xafe2, 2, 0) && wrote(2, 1, 0x600, 16, ACPI_PM1_STS_FIXED) &&
          wrote_bytes(3, 0xafe0, 2, 0xff), "QEMU's PIIX4: 2 enable bytes, the PM1 status, 2 status bytes");
    /* PM1b and GPE1 too: both GPE blocks' enables first, both PM1 statuses, then both GPE blocks' status */
    memset(&blocks, 0, sizeof(blocks));
    blocks.pm1a = 0x400; blocks.pm1b = 0x500; blocks.pm1_len = 4;
    blocks.gpe0 = 0x420; blocks.gpe0_len = 8;
    blocks.gpe1 = 0x430; blocks.gpe1_len = 4;
    s5_setup(0, 0x404, 0x504, intel, sizeof(intel), 0, 0);
    n = clear_writes();
    check(n == 14 && wrote_bytes(0, 0x424, 4, 0) && wrote_bytes(4, 0x432, 2, 0) &&
          wrote(6, 1, 0x400, 16, ACPI_PM1_STS_FIXED) && wrote(7, 1, 0x500, 16, ACPI_PM1_STS_FIXED) &&
          wrote_bytes(8, 0x420, 4, 0xff) && wrote_bytes(12, 0x430, 2, 0xff),
          "with PM1b and GPE1: the enables of both blocks, both PM1 statuses, then both blocks' status");
    /* refused: a block running past port 0xFFFF, an odd GPE length, PM1_EVT_LEN under 4 */
    memset(&blocks, 0, sizeof(blocks));
    blocks.pm1a = 0xfffe; blocks.pm1_len = 4;
    blocks.gpe0 = 0xfff8; blocks.gpe0_len = 16;
    blocks.gpe1 = 0x430; blocks.gpe1_len = 5;
    s5_setup(0, 0x404, 0, intel, sizeof(intel), 0, 0);
    check(!acpi.pm1a_evt && !acpi.gpe_blk[0] && !acpi.gpe_blk[1] && clear_writes() == 0,
          "blocks past port 0xFFFF and an odd GPE length are refused: nothing written");
    memset(&blocks, 0, sizeof(blocks));
    blocks.pm1a = 0x400; blocks.pm1_len = 2;
    s5_setup(0, 0x404, 0, intel, sizeof(intel), 0, 0);
    check(!acpi.pm1a_evt && clear_writes() == 0, "PM1_EVT_LEN under 4: no PM1 status write");
    /* the extended forms: X_GPE0_BLK naming a port is taken where GPE0_BLK is 0; X_PM1a_EVT_BLK in memory is not,
       PM1a_EVT_BLK stays; an extended block past 0xFFFF leaves the legacy one */
    memset(&blocks, 0, sizeof(blocks));
    blocks.pm1a = 0x1800; blocks.pm1_len = 4;
    blocks.x_pm1a_space = 0; blocks.x_pm1a = 0xfed00000;
    blocks.gpe0_len = 32; blocks.x_gpe0_space = 1; blocks.x_gpe0 = 0x1860;
    s5_setup(0, 0x1804, 0, intel, sizeof(intel), 0, 0);
    check(acpi.pm1a_evt == 0x1800 && acpi.gpe_blk[0] == 0x1860, "X_GPE0_BLK at a port is used; X_PM1a_EVT_BLK in memory is not");
    memset(&blocks, 0, sizeof(blocks));
    blocks.gpe0 = 0x1860; blocks.gpe0_len = 32; blocks.x_gpe0_space = 1; blocks.x_gpe0 = 0xfff0;
    s5_setup(0, 0x1804, 0, intel, sizeof(intel), 0, 0);
    check(acpi.gpe_blk[0] == 0x1860, "an X_GPE0_BLK running past 0xFFFF leaves GPE0_BLK");
    /* hardware-reduced ACPI: WAK_STS in the sleep status register, and nothing of the PM1 or GPE blocks */
    memset(&blocks, 0, sizeof(blocks));
    blocks.pm1a = 0x400; blocks.pm1_len = 4; blocks.gpe0 = 0x420; blocks.gpe0_len = 8;
    blocks.sts_space = 1; blocks.sts = 0x405;
    s5_setup(1u << 20, 0, 0, intel, sizeof(intel), 1, 0x404);
    check(clear_writes() == 1 && wrote(0, 1, 0x405, 8, ACPI_SLEEP_WAK_STS),
          "hardware-reduced ACPI: WAK_STS (0x80) in the sleep status register at a port, and nothing else");
    blocks.sts_space = 0; blocks.sts = 0xfed01004;
    s5_setup(1u << 20, 0, 0, intel, sizeof(intel), 0, 0xfed01000);
    check(clear_writes() == 1 && wrote(0, 0, 0xfed01004, 8, ACPI_SLEEP_WAK_STS),
          "hardware-reduced ACPI: the sleep status register in memory");
    blocks.sts_space = 1; blocks.sts = 0x10405;
    s5_setup(1u << 20, 0, 0, intel, sizeof(intel), 1, 0x404);
    check(clear_writes() == 0, "a sleep status register past port 0xFFFF is refused");
    memset(&blocks, 0, sizeof(blocks));
}

static void expect_copy(const char *signature, unsigned instance, int wanted, u32 size) {
    u8 *data = (u8 *)1;
    u32 length = 123;
    int result = acpi_aml_copy(signature, instance, &data, &length);
    check(result == wanted, "export status matches validation outcome");
    if(result == ACPI_COPY_OK) {
        const u8 *source = !strcmp(signature, "DSDT") ? regions[1].data : regions[instance ? 4 : 2].data;
        check(data && length == size && !memcmp(data, signature, 4), "owned copy has the requested table and length");
        check(!memcmp(data, source, size), "all copied bytes match, including chunk boundaries");
        memset(window, 0, sizeof(window));
        check(!memcmp(data, signature, 4), "returned bytes survive remapping the temporary window");
        kfree(data);
    } else check(!data && !length, "failed exports return no pointer or length");
    check(!critical && !live, "no critical section or heap allocation leaks");
}

static void nvs_setup(void) {
    setup(148, 0x200000, 0);
    bootinfo.valid = bootinfo.laptop = true;
    bootinfo.nregions = 1;
    bootinfo.regions[0].base = regions[1].address;
    bootinfo.regions[0].len = 4096;
    bootinfo.regions[0].type = 4;
}

static void expect_nvs(u64 address, u32 length, int wanted, bool may_map) {
    u8 data[256], empty[256];
    unsigned before = maps;
    int result;
    memset(data, 0xa5, sizeof(data));
    memcpy(empty, data, sizeof(data));
    result = acpi_nvs_copy(address, length, data);
    check(result == wanted, "NVS read status matches validation outcome");
    if(result == ACPI_COPY_OK) {
        check(!memcmp(data, regions[1].data + (address - regions[1].address), length),
              "NVS owned copy contains the exact requested bytes");
        memset(window, 0, sizeof(window));
        check(!memcmp(data, regions[1].data + (address - regions[1].address), length),
              "NVS result survives the next mapping");
    } else check(!memcmp(data, empty, sizeof(data)), "failed NVS reads leave the destination untouched");
    if(!may_map) check(maps == before, "refused NVS ranges never map physical memory");
    check(!critical && !live, "NVS read leaks no mapping critical section or allocation");
}

static void test_nvs(void) {
    unsigned before;
    char *good[] = { "acpinvs", "0x200020", "16" };
    char *refused[] = { "acpinvs", "0", "16" };
    char *large[] = { "acpinvs", "1000", "257" };
    char *badhex[] = { "acpinvs", "0xG000", "16" };
    char *negative[] = { "acpinvs", "-1", "16" };
    char *overflow[] = { "acpinvs", "10000000000000000", "16" };
    char *empty[] = { "acpinvs", "0x", "16" };
    char *badcount[] = { "acpinvs", "200020", "16x" };
    char *zero[] = { "acpinvs", "200020", "0" };
    char *huge[] = { "acpinvs", "200020", "4294967296" };
    nvs_setup();
    expect_nvs(0x200020, 37, ACPI_COPY_OK, true);
    expect_nvs(0x200f00, 256, ACPI_COPY_OK, true);
    expect_nvs(0x200fff, 1, ACPI_COPY_OK, true);
    expect_nvs(0x200fff, 2, ACPI_COPY_INVALID, false);
    expect_nvs(0x1fffff, 2, ACPI_COPY_INVALID, false);
    expect_nvs(0x200000, 0, ACPI_COPY_REQUEST, false);
    expect_nvs(0x200000, 257, ACPI_COPY_REQUEST, false);
    before = maps;
    check(acpi_nvs_copy(0x200000, 1, NULL) == ACPI_COPY_REQUEST && maps == before,
          "NVS requires an output buffer before mapping");
    expect_nvs(0xffffffffffffffffULL, 2, ACPI_COPY_REQUEST, false);
    expect_nvs(0xffffffffULL, 2, ACPI_COPY_REQUEST, false);
    expect_nvs(0x100000000ULL, 1, ACPI_COPY_REQUEST, false);
    bootinfo.laptop = false;
    expect_nvs(0x200000, 1, ACPI_COPY_ABSENT, false);
    bootinfo.laptop = true; bootinfo.valid = false;
    expect_nvs(0x200000, 1, ACPI_COPY_ABSENT, false);
    bootinfo.valid = true; acpi.present = false;
    expect_nvs(0x200000, 1, ACPI_COPY_ABSENT, false);
    acpi.present = true; bootinfo.regions[0].type = 1;
    expect_nvs(0x200000, 1, ACPI_COPY_INVALID, false);
    bootinfo.regions[0].type = 4;
    bootinfo.nregions = 2;
    bootinfo.regions[1] = bootinfo.regions[0];
    bootinfo.regions[1].base = 0x200010; bootinfo.regions[1].len = 2;
    bootinfo.regions[1].type = 2;
    expect_nvs(0x200000, 32, ACPI_COPY_INVALID, false);
    expect_nvs(0x200012, 1, ACPI_COPY_OK, true);
    bootinfo.regions[0].len = 16;
    bootinfo.regions[1].type = 4; bootinfo.regions[1].len = 16;
    expect_nvs(0x200008, 16, ACPI_COPY_INVALID, false);
    bootinfo.regions[1].base = 0xfffffffffffffff0ULL; bootinfo.regions[1].len = 32;
    expect_nvs(0x200000, 1, ACPI_COPY_INVALID, false);
    bootinfo.nregions = MB_MAX_REGIONS + 1;
    expect_nvs(0x200000, 1, ACPI_COPY_INVALID, false);
    nvs_setup();
    fail_after = 0x200000;
    expect_nvs(0x200000, 1, ACPI_COPY_INVALID, true);
    fail_after = 0;
    check(cmd_acpinvs(3, good) == 0, "NVS shell exports an allowed range");
    check(cmd_acpinvs(3, refused) == 1, "NVS shell reports a refused range as failure");
    before = maps;
    check(cmd_acpinvs(2, good) == 2 && cmd_acpinvs(3, large) == 2 &&
          cmd_acpinvs(3, badhex) == 2 && cmd_acpinvs(3, negative) == 2 &&
          cmd_acpinvs(3, overflow) == 2 && cmd_acpinvs(3, empty) == 2 &&
          cmd_acpinvs(3, badcount) == 2 && cmd_acpinvs(3, zero) == 2 &&
          cmd_acpinvs(3, huge) == 2, "NVS shell rejects malformed or excessive arguments");
    task.intr = TI_INT;
    check(cmd_acpinvs(3, good) == 1 && maps == before, "interrupted NVS command does not map");
    task.intr = 0;
    regions[1].address = 0xffffff00ULL;
    bootinfo.regions[0].base = regions[1].address; bootinfo.regions[0].len = 256;
    expect_nvs(regions[1].address, 256, ACPI_COPY_OK, true);
}

int main(void) {
    unsigned before;
    char *dsdt[] = { "acpidump", "DSDT" };
    char *wrong[] = { "acpidump", "MSDM" };
    char *negative[] = { "acpidump", "SSDT", "-1" };
    char *overflow[] = { "acpidump", "SSDT", "4294967296" };
    char *second[] = { "acpidump", "DSDT", "1" };

    test_s5();
    test_s5_clear();
    setup(148, 0x200000, 0xdead0000);
    check(acpi.dsdt_addr == 0x200000, "X_DSDT wins over the legacy address");
    expect_copy("DSDT", 0, ACPI_COPY_OK, 64);
    expect_copy("SSDT", 0, ACPI_COPY_OK, 48);
    expect_copy("SSDT", 1, ACPI_COPY_OK, 52);
    before = maps;
    expect_copy("SSDT", 2, ACPI_COPY_ABSENT, 0);
    expect_copy("SSDT", 0xffffffffU, ACPI_COPY_ABSENT, 0);
    expect_copy("DSDT", 1, ACPI_COPY_REQUEST, 0);
    expect_copy("MSDM", 0, ACPI_COPY_REQUEST, 0);
    expect_copy("DSDTextra", 0, ACPI_COPY_REQUEST, 0);
    expect_copy(NULL, 0, ACPI_COPY_REQUEST, 0);
    check(maps == before, "bad names and indices never map physical memory");
    setup(148, 0, 0x200000);
    check(acpi.dsdt_addr == 0x200000, "zero X_DSDT falls back to the legacy address");
    setup(147, 0, 0x200000);
    check(acpi.dsdt_addr == 0x200000, "incomplete X_DSDT field falls back to the legacy address");
    setup(44, 0, 0x200000);
    check(acpi.dsdt_addr == 0x200000, "short legacy FADT still supplies DSDT");
    regions[3].data[36] ^= 1;
    exporting = false; init_acpi(); exporting = true;
    expect_copy("DSDT", 0, ACPI_COPY_ABSENT, 0);
    table(3, "NOPE", 44);
    exporting = false; init_acpi(); exporting = true;
    expect_copy("DSDT", 0, ACPI_COPY_ABSENT, 0);
    setup(40, 0, 0);
    expect_copy("DSDT", 0, ACPI_COPY_ABSENT, 0);
    setup(148, 0x100200000ULL, 0x200000);
    check(acpi.dsdt_addr == 0x100200000ULL, "nonzero X_DSDT is never silently replaced");
    expect_copy("DSDT", 0, ACPI_COPY_INVALID, 0);
    setup(148, 0x200000, 0);
    before = maps;
    acpi.dsdt_addr = 0xfffffffffffffff0ULL;
    expect_copy("DSDT", 0, ACPI_COPY_INVALID, 0);
    check(maps == before, "overflowing physical addresses are rejected before mapping");
    acpi.dsdt_addr = 0xfff00000;
    expect_copy("DSDT", 0, ACPI_COPY_INVALID, 0);
    acpi.dsdt_addr = 0x200000;
    regions[1].data[36] ^= 1;
    expect_copy("DSDT", 0, ACPI_COPY_INVALID, 0);
    table(1, "MSDM", 64);
    expect_copy("DSDT", 0, ACPI_COPY_INVALID, 0);
    table(1, "DSDT", 64);
    put32(regions[1].data + 4, 35);
    expect_copy("DSDT", 0, ACPI_COPY_INVALID, 0);
    put32(regions[1].data + 4, 0xffffffffU);
    expect_copy("DSDT", 0, ACPI_COPY_INVALID, 0);
    table(1, "DSDT", CAPACITY);
    expect_copy("DSDT", 0, ACPI_COPY_OK, CAPACITY);
    regions[1].address += PAGE_SIZE - 1; acpi.dsdt_addr = regions[1].address;
    expect_copy("DSDT", 0, ACPI_COPY_OK, CAPACITY);
    table(1, "DSDT", 2 * CAPACITY);
    expect_copy("DSDT", 0, ACPI_COPY_OK, 2 * CAPACITY);
    table(1, "DSDT", 1048576U);
    expect_copy("DSDT", 0, ACPI_COPY_OK, 1048576U);
    table(1, "DSDT", 1048577U);
    before = allocations;
    expect_copy("DSDT", 0, ACPI_COPY_INVALID, 0);
    check(allocations == before, "over 1 MiB is refused before allocation");
    table(1, "DSDT", 2 * CAPACITY);
    fail_after = regions[1].address + 70000;
    expect_copy("DSDT", 0, ACPI_COPY_INVALID, 0);
    fail_after = 0;
    mutate_header_after = regions[1].address + 70000;
    expect_copy("DSDT", 0, ACPI_COPY_INVALID, 0);
    corrupt_after = regions[1].address + 70000;
    expect_copy("DSDT", 0, ACPI_COPY_INVALID, 0);
    table(1, "DSDT", 64);
    regions[1].address = acpi.dsdt_addr = 0x100000000ULL - 64;
    expect_copy("DSDT", 0, ACPI_COPY_OK, 64);
    regions[1].address++; acpi.dsdt_addr++;
    before = allocations;
    expect_copy("DSDT", 0, ACPI_COPY_INVALID, 0);
    check(allocations == before, "physical end overflow is refused before allocation");
    setup(148, 0x200000, 0);
    acpi.tables[1].ok = false;
    regions[2].data[36] ^= 1;
    expect_copy("SSDT", 0, ACPI_COPY_INVALID, 0);
    expect_copy("SSDT", 1, ACPI_COPY_OK, 52);
    table(2, "SSDT", CAPACITY + 17);
    expect_copy("SSDT", 0, ACPI_COPY_OK, CAPACITY + 17);
    allocation_failed = true;
    expect_copy("DSDT", 0, ACPI_COPY_NOMEM, 0);
    allocation_failed = false; change_length = true;
    expect_copy("DSDT", 0, ACPI_COPY_INVALID, 0);
    setup(148, 0x200000, 0);
    change_signature = true;
    expect_copy("DSDT", 0, ACPI_COPY_INVALID, 0);
    setup(148, 0x200000, 0);
    check(cmd_acpidump(2, wrong) == 2 && cmd_acpidump(3, negative) == 2 &&
          cmd_acpidump(3, overflow) == 2 && cmd_acpidump(3, second) == 2,
          "shell rejects sensitive signatures, negative/overflow indices and a second DSDT");
    task.intr = TI_INT;
    check(cmd_acpidump(2, dsdt) == 1 && !live && !critical, "interrupted export fails without leaking its owned copy");
    task.intr = 0;
    interrupt_allocation = true;
    check(cmd_acpidump(2, dsdt) == 1 && !live && !critical, "interruption after allocation still frees the table");
    interrupt_allocation = false; task.intr = 0;
    check(cmd_acpidump(2, dsdt) == 0 && !live && !critical, "normal shell export succeeds and frees its copy");
    test_nvs();
    printf("ACPIDUMP UNIT TESTS %s: %u checks, %u failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
