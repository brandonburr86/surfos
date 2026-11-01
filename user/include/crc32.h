/*
SurfOS user library: CRC-32
----------------------
File: crc32.h   Date: 11/01/26
----------------------
The CRC of gzip and zip (the reflected polynomial 0xEDB88320, starting from and ending in
all ones). crc32(0, ...) starts one; the result goes in again for the bytes that follow, so
crc32(crc32(0, a, n), b, m) is the CRC of a then b.
*/

#ifndef _CRC32_H
#define _CRC32_H

#include <stddef.h>
#include <stdint.h>

uint32_t crc32(uint32_t crc, const void *p, size_t n);

#endif
