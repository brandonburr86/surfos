/*
SurfCPU firmware: CRC-32
--------------------
File: crc32.c Date: 10/15/26 (SurfCPU)
--------------------
See crc32.h. A 256-entry table, built on first use in RAM (1 KiB of .bss rather than ROM):
about seven instructions per byte, so checking a 2 MiB kernel twice takes well under a
second at 25 MHz.
*/
#include "crc32.h"

static uint32_t crc_table[256];
static int crc_ready;

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
    crc_ready = 1;
}

uint32_t crc32_update(uint32_t crc, const void *data, size_t n)
{
    const uint8_t *p = data;
    if (!crc_ready)
        crc_init();
    crc = ~crc;
    while (n--)
        crc = crc_table[(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

uint32_t crc32(const void *data, size_t n)
{
    return crc32_update(0, data, n);
}

int crc32_selftest(void)
{
    uint8_t seq[256];
    for (int i = 0; i < 256; i++)
        seq[i] = (uint8_t)i;
    if (crc32("", 0) != 0x00000000u) return 1;
    if (crc32("a", 1) != 0xE8B7BE43u) return 2;
    if (crc32("123456789", 9) != 0xCBF43926u) return 3;
    if (crc32(seq, sizeof seq) != 0x29058C73u) return 4;
    /* chaining gives the same result as one pass */
    if (crc32_update(crc32("1234", 4), "56789", 5) != 0xCBF43926u) return 5;
    return 0;
}
