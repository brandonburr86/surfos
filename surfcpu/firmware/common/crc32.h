/*
SurfCPU firmware: CRC-32
--------------------
File: crc32.h Date: 10/15/26 (SurfCPU)
--------------------
CRC-32/ISO-HDLC (zlib, PNG, Ethernet), the checksum of the boot ABI (docs/BOOT-ABI.md
section 3): polynomial 0x04C11DB7 reflected (0xEDB88320), initial value 0xFFFFFFFF, input
and output reflected, final XOR 0xFFFFFFFF. crc32_update(0, ...) starts a CRC and chains:
crc32_update(crc32_update(0, a, n), b, m) is the CRC of a followed by b.
*/
#ifndef SURFCPU_CRC32_H
#define SURFCPU_CRC32_H

#include <stddef.h>
#include <stdint.h>

uint32_t crc32_update(uint32_t crc, const void *data, size_t n);
uint32_t crc32(const void *data, size_t n);

/* The four test vectors of BOOT-ABI.md; returns 0 when all match, else the failing index + 1. */
int crc32_selftest(void);

#endif /* SURFCPU_CRC32_H */
