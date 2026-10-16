/*
SurfOS board: SurfCPU on the Arty A7 -- SD card protocol helpers
--------------------
File: sdproto.c Date: 10/15/26 (SurfCPU)
--------------------
See sdproto.h. No hardware, no kernel calls: built into the kernel and into the host test.
*/

#include "sdproto.h"

/* CRC7: x^7 + x^3 + 1, from 0, most significant bit first */
u8 sd_crc7(const u8 *data, u_int len) {
    u8 crc = 0;
    u_int i, b;
    for(i = 0; i < len; i++) {
        u8 d = data[i];
        for(b = 0; b < 8; b++) {
            crc <<= 1;
            if((d ^ crc) & 0x80) crc ^= 0x09;
            d <<= 1;
        }
    }
    return crc & 0x7F;
}

/* CRC16: x^16 + x^12 + x^5 + 1 (0x1021), from 0, most significant bit first */
u16 sd_crc16(const u8 *data, u_int len) {
    u16 crc = 0;
    u_int i, b;
    for(i = 0; i < len; i++) {
        crc ^= (u16)data[i] << 8;
        for(b = 0; b < 8; b++) crc = (crc & 0x8000) ? (u16)((crc << 1) ^ 0x1021) : (u16)(crc << 1);
    }
    return crc;
}

void sd_frame(u8 cmd, u32 arg, u8 out[6]) {
    out[0] = (u8)(0x40 | (cmd & 0x3F));
    out[1] = (u8)(arg >> 24);
    out[2] = (u8)(arg >> 16);
    out[3] = (u8)(arg >> 8);
    out[4] = (u8)arg;
    out[5] = (u8)((sd_crc7(out, 5) << 1) | 1);
}

/* xxx0sss1: sss 010 accepted, 101 CRC error, 110 write error */
int sd_data_response(u8 token) {
    if((token & 0x11) != 0x01) return SD_DATA_BAD;
    switch((token >> 1) & 0x07) {
    case 0x2: return SD_DATA_ACCEPTED;
    case 0x5: return SD_DATA_CRC_ERROR;
    case 0x6: return SD_DATA_WRITE_ERROR;
    }
    return SD_DATA_BAD;
}

u32 sd_csd_bits(const u8 csd[16], u_int msb, u_int lsb) {
    u32 v = 0;
    u_int bit;
    for(bit = msb + 1; bit-- > lsb;) {
        u_int byte = 15 - bit / 8;           /* csd[0] holds bits 127..120 */
        v = (v << 1) | ((csd[byte] >> (bit % 8)) & 1);
    }
    return v;
}

int sd_csd_version(const u8 csd[16]) {
    u32 s = sd_csd_bits(csd, 127, 126);
    return s == 0 ? 1 : (s == 1 ? 2 : 0);
}

u64 sd_csd_blocks(const u8 csd[16]) {
    switch(sd_csd_version(csd)) {
    case 1: {                                /* SDSC: (C_SIZE + 1) x 2^(C_SIZE_MULT + 2) blocks of 2^READ_BL_LEN bytes */
        u32 c_size = sd_csd_bits(csd, 73, 62), mult = sd_csd_bits(csd, 49, 47), bl_len = sd_csd_bits(csd, 83, 80);
        u64 bytes = (u64)(c_size + 1) << (mult + 2 + bl_len);
        return bl_len >= 9 && bl_len <= 11 ? bytes / 512 : 0;
    }
    case 2:                                  /* SDHC/SDXC: (C_SIZE + 1) x 512 KB */
        return ((u64)sd_csd_bits(csd, 69, 48) + 1) * 1024;
    }
    return 0;
}

static char *put_str(char *p, char *end, const char *s) {
    while(*s && p < end) *p++ = *s++;
    return p;
}

static char *put_num(char *p, char *end, u32 v, u_int base, u_int digits) {
    char tmp[12];
    u_int n = 0;
    do { tmp[n++] = "0123456789abcdef"[v % base]; v /= base; } while(v && n < sizeof(tmp));
    while(n < digits && n < sizeof(tmp)) tmp[n++] = '0';
    while(n && p < end) *p++ = tmp[--n];
    return p;
}

void sd_cid_describe(const u8 cid[16], char *buf, u_int size) {
    char *p = buf, *end = buf + size - 1, name[6], oem[3];
    u_int i;
    if(!size) return;
    for(i = 0; i < 5; i++) name[i] = (cid[3 + i] >= 32 && cid[3 + i] < 127) ? (char)cid[3 + i] : '?';
    name[5] = 0;
    oem[0] = (cid[1] >= 32 && cid[1] < 127) ? (char)cid[1] : '?';
    oem[1] = (cid[2] >= 32 && cid[2] < 127) ? (char)cid[2] : '?';
    oem[2] = 0;
    p = put_str(p, end, name);
    p = put_str(p, end, " rev ");
    p = put_num(p, end, cid[8] >> 4, 10, 1);
    p = put_str(p, end, ".");
    p = put_num(p, end, cid[8] & 15, 10, 1);
    p = put_str(p, end, ", manufacturer 0x");
    p = put_num(p, end, cid[0], 16, 2);
    p = put_str(p, end, " '");
    p = put_str(p, end, oem);
    p = put_str(p, end, "', serial ");
    p = put_num(p, end, ((u32)cid[9] << 24) | ((u32)cid[10] << 16) | ((u32)cid[11] << 8) | cid[12], 16, 8);
    p = put_str(p, end, ", made ");
    p = put_num(p, end, 2000 + (((cid[13] & 0x0F) << 4) | (cid[14] >> 4)), 10, 4);
    p = put_str(p, end, "-");
    p = put_num(p, end, cid[14] & 0x0F, 10, 2);
    *p = 0;
}
