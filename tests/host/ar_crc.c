/* The tests' own CRC-32 (11/01/26): a fixture's checksum must not come from the code under test. Bit by bit, from the
   definition. */
#include <stddef.h>

unsigned int ar_crc32(unsigned int crc, const void *p, size_t n) {
    const unsigned char *s = p;
    crc = ~crc;
    while(n--) {
        int k;
        crc ^= *s++;
        for(k = 0; k < 8; k++) crc = crc & 1 ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
    }
    return ~crc;
}

/* the same by a table, for gigabytes of a test's zeros: checked against the bits above by the first bytes it is given */
unsigned int crc32_fast(unsigned int crc, const void *p, size_t n) {
    static unsigned int table[256];
    const unsigned char *s = p;
    if(!table[1]) {
        unsigned int i;
        for(i = 0; i < 256; i++) {
            unsigned int c = i;
            int k;
            for(k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0xEDB88320u : c >> 1;
            table[i] = c;
        }
    }
    crc = ~crc;
    while(n--) crc = table[(crc ^ *s++) & 255] ^ (crc >> 8);
    return ~crc;
}
