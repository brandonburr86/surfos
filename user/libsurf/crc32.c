/*
SurfOS user library: CRC-32
--------------------
File: crc32.c   Date: 11/01/26
--------------------
A byte at a time from a table made at the first use, which spares the program 1 KB of
numbers and costs it nothing it would notice.
*/

#include <crc32.h>

static uint32_t table[256];

static void make_table(void) {
    uint32_t i;
    for(i = 0; i < 256; i++) {
        uint32_t c = i;
        int k;
        for(k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        table[i] = c;
    }
}

uint32_t crc32(uint32_t crc, const void *p, size_t n) {
    const unsigned char *s = p;
    if(!table[1]) make_table();
    crc = ~crc;
    while(n--) crc = table[(crc ^ *s++) & 255] ^ (crc >> 8);
    return ~crc;
}
