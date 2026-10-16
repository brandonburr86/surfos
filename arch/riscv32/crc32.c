/*
SurfOS riscv32 CRC-32
--------------------
File: crc32.c   Date: 10/15/26 (SurfCPU)
--------------------
CRC-32/ISO-HDLC, the CRC of zlib, PNG and Ethernet, as surfcpu/docs/BOOT-ABI.md
section 3 defines it for the boot information and the image header: polynomial
0x04C11DB7 reflected (0xEDB88320), initial value and final XOR 0xFFFFFFFF. A table of
16 nibbles keeps it small and fast enough for the 256 bytes it checks at boot. The
test vectors of BOOT-ABI.md are checked by the self test (arch/riscv32/rvtest.c).
*/

#include <surfos/types.h>
#include <riscv/arch.h>

static const u32 nibble[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C
};

/* crc is a running value: start with crc32_update(0, ...) and pass each result on */
u32 crc32_update(u32 crc, const void *data, u_long len) {
    const u8 *p = (const u8 *)data;
    crc = ~crc;
    while(len--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ nibble[crc & 15];
        crc = (crc >> 4) ^ nibble[crc & 15];
    }
    return ~crc;
}

u32 crc32(const void *data, u_long len) {
    return crc32_update(0, data, len);
}
