/*
SurfCPU diagnostics: DDR tests
--------------------
File: ddrtest.c Date: 10/15/26 (SurfCPU)
--------------------
The milestone 4 memory tests (brief section 10), for any part of DDR the caller may
overwrite ([lo, hi) inside [ram_base, ram_base + ram_size)):

  data lines     walking one and walking zero at three addresses
  byte lanes     byte and halfword stores into a word, read back at every width
  address lines  a base word and its partner at base ^ 2^k for every address bit, all
                 written before any is read (a stuck or shorted line overwrites a word),
                 then each partner inverted in turn with every other word re-checked
  boundaries     word runs across the ends of the area and its 1 MiB and 128 MiB lines,
                 with every access width at the very top of DDR
  patterns       address-in-address and its inverse over a block, and a sampled pass over
                 the whole area (two words per 64 KiB) that finds large aliases
  copies         sustained copies between two blocks, checked by CRC-32, with the rate
  code in DDR    a position-independent routine copied to the start, middle and end of
                 the area and run after FENCE.I, then patched in place (self-modifying
                 code) and run again

The base of the address test is ram_base + ram_size / 2 + (lo - ram_base), so the partner of
the top address bit is lo itself and every other partner lies in the upper half; when the
area is limited and that base falls outside it, the base is lo and only the address lines
the area spans are tested (the others are reported as skipped).
*/
#include <string.h>
#include "diag.h"

#define W(addr) (*(volatile uint32_t *)(uintptr_t)(addr))

/* The hot loops run from block RAM (FW_FAST: scratch RAM in the SURFOS.BIN, ROM in the BRAM
 * program), so instruction fetches do not compete with the DDR accesses being tested. Only
 * code goes there: the CRC table is ordinary .bss. */
static uint32_t fast_crc_table[256];

FW_FAST static uint32_t fast_crc32(const uint8_t *p, uint32_t n)
{
    if (!fast_crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            fast_crc_table[i] = c;
        }
    uint32_t crc = 0xFFFFFFFFu;
    while (n--)
        crc = fast_crc_table[(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

FW_FAST static void fast_copy(uint32_t dst, uint32_t src, uint32_t bytes)
{
    volatile uint32_t *d = (volatile uint32_t *)(uintptr_t)dst;
    const volatile uint32_t *s = (const volatile uint32_t *)(uintptr_t)src;
    for (uint32_t n = bytes / 16u; n; n--) {
        uint32_t a = s[0], b = s[1], c = s[2], e = s[3];
        d[0] = a; d[1] = b; d[2] = c; d[3] = e;
        d += 4;
        s += 4;
    }
}

FW_FAST static uint32_t fast_fill_check(uint32_t lo, uint32_t end, uint32_t x, uint32_t *first_bad)
{
    uint32_t bad = 0;
    for (uint32_t ad = lo; ad < end; ad += 4)
        W(ad) = ad ^ x;
    for (uint32_t ad = lo; ad < end; ad += 4)
        if (W(ad) != (ad ^ x) && bad++ == 0)
            *first_bad = ad;
    return bad;
}
#define H(addr) (*(volatile uint16_t *)(uintptr_t)(addr))
#define B(addr) (*(volatile uint8_t *)(uintptr_t)(addr))

int ddr_wait_calibration(uint32_t timeout_us)
{
    uint32_t t0 = timer_now32();
    for (;;) {
        uint32_t st = sysinfo(SURF_SYSINFO_DDR_STATUS);
        if (st & SURF_SYSINFO_DDR_STATUS_CALIB_DONE_MASK)
            return 1;
        if (st & SURF_SYSINFO_DDR_STATUS_CALIB_FAIL_MASK)
            return 0;
        if ((uint32_t)(timer_now32() - t0) >= timeout_us)
            return 0;
    }
}

void ddr_test_data_lines(const struct ddr_area *a)
{
    diag_section("DDR data lines");
    uint32_t where[3] = { a->lo, (a->lo + (a->hi - a->lo) / 2) & ~3u, a->hi - 4u };
    for (int i = 0; i < 3; i++) {
        uint32_t ad = where[i];
        for (int bit = 0; bit < 32; bit++) {
            uint32_t v = 1u << bit;
            W(ad) = v;
            uint32_t r = W(ad);
            if (!diag_check(r == v, "walking one at 0x%08x: wrote 0x%08x, read 0x%08x (data line D%d)", ad, v, r, bit))
                break;
            W(ad) = ~v;
            r = W(ad);
            if (!diag_check(r == ~v, "walking zero at 0x%08x: wrote 0x%08x, read 0x%08x (data line D%d)", ad, ~v, r,
                            bit))
                break;
        }
    }
    diag_section_end();
}

void ddr_test_byte_lanes(const struct ddr_area *a)
{
    diag_section("DDR byte lanes");
    uint32_t ad = a->lo + 16u;
    W(ad) = 0;
    for (int k = 0; k < 4; k++) {
        B(ad + k) = (uint8_t)(0xA0 + k);
        uint32_t want = 0;
        for (int j = 0; j <= k; j++)
            want |= (uint32_t)(0xA0 + j) << (8 * j);
        uint32_t r = W(ad);
        diag_check(r == want, "byte store %d at 0x%08x: word reads 0x%08x, expected 0x%08x", k, ad + k, r, want);
    }
    H(ad + 2) = 0x5AA5;
    diag_check(W(ad) == 0x5AA5A1A0u, "halfword store at 0x%08x: word reads 0x%08x", ad + 2, W(ad));
    H(ad) = 0x1234;
    diag_check(W(ad) == 0x5AA51234u, "halfword store at 0x%08x: word reads 0x%08x", ad, W(ad));
    diag_check(B(ad + 3) == 0x5A && B(ad + 1) == 0x12, "byte loads from 0x%08x", ad);
    int8_t sb;
    int16_t sh;
    __asm__ volatile ("lb %0, 3(%1)" : "=r"(sb) : "r"(ad) : "memory");
    __asm__ volatile ("lh %0, 2(%1)" : "=r"(sh) : "r"(ad) : "memory");
    diag_check(sb == 0x5A && sh == 0x5AA5, "signed loads (lb %d, lh %d)", sb, sh);
    W(ad) = 0x80FF8001u;
    __asm__ volatile ("lb %0, 3(%1)" : "=r"(sb) : "r"(ad) : "memory");
    __asm__ volatile ("lh %0, 0(%1)" : "=r"(sh) : "r"(ad) : "memory");
    diag_check(sb == -128 && sh == -32767, "sign extension of DDR loads (lb %d, lh %d)", sb, sh);
    diag_section_end();
}

static uint32_t apat(uint32_t k) { return 0xA5000000u ^ (k * 0x01010101u) ^ (k << 20); }

void ddr_test_address_lines(const struct ddr_area *a)
{
    diag_section("DDR address lines and aliases");
    uint32_t off = a->lo - a->ram_base, half = a->ram_size / 2u;
    if (!diag_check(off < half, "the test area starts above the middle of DDR")) {
        diag_section_end();
        return;
    }
    uint32_t base = a->ram_base + half + off;
    if (base < a->lo || base + 4u > a->hi)
        base = a->lo;                               /* a limited area: test the lines it spans */
    uint32_t partner[32];
    int bits = 0, skipped = 0;
    for (uint32_t k = 2; (1u << k) < a->ram_size && k < 32; k++) {
        uint32_t p = base ^ (1u << k);
        if (p < a->lo || p + 4u > a->hi) {
            partner[k] = 0;
            skipped++;
            continue;
        }
        partner[k] = p;
        bits = (int)k + 1;
    }
    for (int k = 2; k < bits; k++)
        if (partner[k])
            W(partner[k]) = apat((uint32_t)k);
    W(base) = apat(0);
    for (int k = 2; k < bits; k++)
        if (partner[k]) {
            uint32_t r = W(partner[k]);
            diag_check(r == apat((uint32_t)k), "0x%08x (base ^ A%d) reads 0x%08x, expected 0x%08x: address line A%d "
                       "stuck or shorted", partner[k], k, r, apat((uint32_t)k), k);
        }
    diag_check(W(base) == apat(0), "the base word 0x%08x reads 0x%08x: an address line aliases it", base, W(base));
    for (int k = 2; k < bits; k++) {
        if (!partner[k])
            continue;
        W(partner[k]) = ~apat((uint32_t)k);
        if (W(base) != apat(0)) {
            diag_check(0, "writing 0x%08x (A%d) changed the base word 0x%08x", partner[k], k, base);
        } else {
            for (int j = 2; j < bits; j++)
                if (j != k && partner[j] && W(partner[j]) != apat((uint32_t)j)) {
                    diag_check(0, "writing 0x%08x (A%d) changed 0x%08x (A%d): shorted address lines", partner[k], k,
                               partner[j], j);
                    break;
                }
        }
        W(partner[k]) = apat((uint32_t)k);
        diag_check(1, "address line %d", k);
    }
    if (skipped > 1)
        con_printf("(%d address lines skipped: outside the test area) ", skipped);
    diag_section_end();
}

static void run_check(uint32_t from, uint32_t to, uint32_t lo, uint32_t hi)
{
    if (from < lo)
        from = lo;
    if (to > hi)
        to = hi;
    for (uint32_t ad = from; ad < to; ad += 4)
        W(ad) = ad ^ 0x5C5C5C5Cu;
    for (uint32_t ad = from; ad < to; ad += 4) {
        uint32_t r = W(ad);
        if (!diag_check(r == (ad ^ 0x5C5C5C5Cu), "0x%08x reads 0x%08x, expected 0x%08x", ad, r, ad ^ 0x5C5C5C5Cu))
            break;
    }
}

void ddr_test_boundaries(const struct ddr_area *a)
{
    diag_section("DDR boundaries");
    run_check(a->lo, a->lo + 64u, a->lo, a->hi);
    run_check(a->hi - 64u, a->hi, a->lo, a->hi);
    uint32_t mib = ((a->lo + (a->hi - a->lo) / 2u) & ~0xFFFFFu);
    run_check(mib - 64u, mib + 64u, a->lo, a->hi);
    uint32_t big = a->ram_base + (128u << 20);
    if (big > a->lo && big < a->hi)
        run_check(big - 64u, big + 64u, a->lo, a->hi);
    uint32_t top = a->ram_base + a->ram_size - 4u;
    if (top >= a->lo && top + 4u <= a->hi) {          /* the last word of DDR, every width */
        W(top) = 0x01234567u;
        diag_check(W(top) == 0x01234567u, "last word of DDR 0x%08x", top);
        B(top + 3) = 0xEE;
        H(top) = 0xBEEF;
        diag_check(W(top) == 0xEE23BEEFu, "byte and halfword at the top of DDR: 0x%08x", W(top));
    }
    diag_section_end();
}

void ddr_test_patterns(const struct ddr_area *a, uint32_t full_bytes, int sampled)
{
    diag_section("DDR patterns");
    uint32_t end = a->lo + full_bytes;
    if (end > a->hi || end < a->lo)
        end = a->hi;
    uint32_t bad = 0;
    for (int pass = 0; pass < 2; pass++) {
        uint32_t x = pass ? 0xFFFFFFFFu : 0, first = 0;
        uint32_t n = fast_fill_check(a->lo, end, x, &first);
        if (n)
            diag_check(0, "0x%08x reads 0x%08x, expected 0x%08x (%u words wrong in this pass)", first, W(first),
                       first ^ x, n);
        bad += n;
    }
    diag_check(bad == 0, "%u words wrong in [0x%08x, 0x%08x)", bad, a->lo, end);
    if (sampled) {
        uint32_t blocks = 0;
        for (uint32_t ad = a->lo & ~0xFFFFu; ad < a->hi; ad += 0x10000u) {
            if (ad >= a->lo)
                W(ad) = ~ad;
            if (ad + 0xFFFCu < a->hi && ad + 0xFFFCu >= a->lo)
                W(ad + 0xFFFCu) = ad * 7u;
        }
        bad = 0;
        for (uint32_t ad = a->lo & ~0xFFFFu; ad < a->hi; ad += 0x10000u) {
            blocks++;
            if (ad >= a->lo && W(ad) != ~ad && bad++ < 4)
                diag_check(0, "sampled word 0x%08x reads 0x%08x: aliasing?", ad, W(ad));
            if (ad + 0xFFFCu < a->hi && ad + 0xFFFCu >= a->lo && W(ad + 0xFFFCu) != ad * 7u && bad++ < 4)
                diag_check(0, "sampled word 0x%08x reads 0x%08x: aliasing?", ad + 0xFFFCu, W(ad + 0xFFFCu));
        }
        diag_check(bad == 0, "%u sampled words wrong in %u blocks of 64 KiB", bad, blocks);
        con_printf("(%u KiB in full, %u blocks sampled) ", (end - a->lo) >> 10, blocks);
    } else {
        con_printf("(%u KiB) ", (end - a->lo) >> 10);
    }
    diag_section_end();
}

void ddr_test_copies(const struct ddr_area *a, uint32_t bytes, uint32_t loops)
{
    diag_section("DDR sustained copies");
    if (a->hi - a->lo < 2u * bytes + 4096u)
        bytes = ((a->hi - a->lo) / 2u - 4096u) & ~0xFFFu;
    uint32_t src = (a->lo + 4096u) & ~0xFFFu, dst = src + bytes;
    uint32_t x = 0x1234567u;
    for (uint32_t ad = src; ad < src + bytes; ad += 4) {
        x = x * 1664525u + 1013904223u;
        W(ad) = x;
    }
    uint32_t want = fast_crc32((const uint8_t *)(uintptr_t)src, bytes);
    uint32_t t0 = timer_now32();
    for (uint32_t i = 0; i < loops; i++) {
        fast_copy(dst, src, bytes);
        uint32_t got = fast_crc32((const uint8_t *)(uintptr_t)dst, bytes);
        if (!diag_check(got == want, "copy %u of %u KiB to 0x%08x: CRC 0x%08x, expected 0x%08x", i, bytes >> 10, dst,
                        got, want))
            break;
        uint32_t t = src;                           /* back and forth */
        src = dst;
        dst = t;
    }
    uint32_t us = timer_now32() - t0;
    con_printf("(%u x %u KiB copied and checked, %u KiB/s) ", loops, bytes >> 10,
               us ? (uint32_t)((uint64_t)loops * (bytes >> 10) * 1000000u / us) : 0u);
    diag_section_end();
}

typedef uint32_t (*probe_fn)(uint32_t);

void ddr_test_exec(const struct ddr_area *a)
{
    diag_section("code running from DDR");
    uint32_t n = (uint32_t)(ddr_probe_end - ddr_probe_start);
    uint32_t imm_off = (uint32_t)(ddr_probe_imm - ddr_probe_start);
    uint32_t where[3] = { (a->lo + 4096u) & ~15u, (a->lo + (a->hi - a->lo) / 2u) & ~15u, (a->hi - 64u) & ~15u };
    for (int i = 0; i < 3; i++) {
        uint32_t ad = where[i];
        memcpy((void *)(uintptr_t)ad, ddr_probe_start, n);
        fw_fence_i();
        uint32_t r = ((probe_fn)(uintptr_t)ad)(5);
        diag_check(r == 5u * 3u + 0x234u, "the routine copied to 0x%08x returned 0x%08x", ad, r);
        uint32_t insn = W(ad + imm_off);            /* addi a0, a0, 0x234 -> 0x345 */
        W(ad + imm_off) = (insn & 0x000FFFFFu) | (0x345u << 20);
        fw_fence_i();
        r = ((probe_fn)(uintptr_t)ad)(5);
        diag_check(r == 5u * 3u + 0x345u, "after patching it at 0x%08x and FENCE.I it returned 0x%08x", ad, r);
    }
    diag_section_end();
}
