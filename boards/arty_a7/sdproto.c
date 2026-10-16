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
/* 10/16/26: a byte at a time from a table (x^16 + x^12 + x^5 + 1, MSB first): the bit loop cost about
   40 instructions a byte, a large part of every block's time on SurfCPU */
static const u16 crc16_table[256] = {
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
    0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF,
    0x1231, 0x0210, 0x3273, 0x2252, 0x52B5, 0x4294, 0x72F7, 0x62D6,
    0x9339, 0x8318, 0xB37B, 0xA35A, 0xD3BD, 0xC39C, 0xF3FF, 0xE3DE,
    0x2462, 0x3443, 0x0420, 0x1401, 0x64E6, 0x74C7, 0x44A4, 0x5485,
    0xA56A, 0xB54B, 0x8528, 0x9509, 0xE5EE, 0xF5CF, 0xC5AC, 0xD58D,
    0x3653, 0x2672, 0x1611, 0x0630, 0x76D7, 0x66F6, 0x5695, 0x46B4,
    0xB75B, 0xA77A, 0x9719, 0x8738, 0xF7DF, 0xE7FE, 0xD79D, 0xC7BC,
    0x48C4, 0x58E5, 0x6886, 0x78A7, 0x0840, 0x1861, 0x2802, 0x3823,
    0xC9CC, 0xD9ED, 0xE98E, 0xF9AF, 0x8948, 0x9969, 0xA90A, 0xB92B,
    0x5AF5, 0x4AD4, 0x7AB7, 0x6A96, 0x1A71, 0x0A50, 0x3A33, 0x2A12,
    0xDBFD, 0xCBDC, 0xFBBF, 0xEB9E, 0x9B79, 0x8B58, 0xBB3B, 0xAB1A,
    0x6CA6, 0x7C87, 0x4CE4, 0x5CC5, 0x2C22, 0x3C03, 0x0C60, 0x1C41,
    0xEDAE, 0xFD8F, 0xCDEC, 0xDDCD, 0xAD2A, 0xBD0B, 0x8D68, 0x9D49,
    0x7E97, 0x6EB6, 0x5ED5, 0x4EF4, 0x3E13, 0x2E32, 0x1E51, 0x0E70,
    0xFF9F, 0xEFBE, 0xDFDD, 0xCFFC, 0xBF1B, 0xAF3A, 0x9F59, 0x8F78,
    0x9188, 0x81A9, 0xB1CA, 0xA1EB, 0xD10C, 0xC12D, 0xF14E, 0xE16F,
    0x1080, 0x00A1, 0x30C2, 0x20E3, 0x5004, 0x4025, 0x7046, 0x6067,
    0x83B9, 0x9398, 0xA3FB, 0xB3DA, 0xC33D, 0xD31C, 0xE37F, 0xF35E,
    0x02B1, 0x1290, 0x22F3, 0x32D2, 0x4235, 0x5214, 0x6277, 0x7256,
    0xB5EA, 0xA5CB, 0x95A8, 0x8589, 0xF56E, 0xE54F, 0xD52C, 0xC50D,
    0x34E2, 0x24C3, 0x14A0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
    0xA7DB, 0xB7FA, 0x8799, 0x97B8, 0xE75F, 0xF77E, 0xC71D, 0xD73C,
    0x26D3, 0x36F2, 0x0691, 0x16B0, 0x6657, 0x7676, 0x4615, 0x5634,
    0xD94C, 0xC96D, 0xF90E, 0xE92F, 0x99C8, 0x89E9, 0xB98A, 0xA9AB,
    0x5844, 0x4865, 0x7806, 0x6827, 0x18C0, 0x08E1, 0x3882, 0x28A3,
    0xCB7D, 0xDB5C, 0xEB3F, 0xFB1E, 0x8BF9, 0x9BD8, 0xABBB, 0xBB9A,
    0x4A75, 0x5A54, 0x6A37, 0x7A16, 0x0AF1, 0x1AD0, 0x2AB3, 0x3A92,
    0xFD2E, 0xED0F, 0xDD6C, 0xCD4D, 0xBDAA, 0xAD8B, 0x9DE8, 0x8DC9,
    0x7C26, 0x6C07, 0x5C64, 0x4C45, 0x3CA2, 0x2C83, 0x1CE0, 0x0CC1,
    0xEF1F, 0xFF3E, 0xCF5D, 0xDF7C, 0xAF9B, 0xBFBA, 0x8FD9, 0x9FF8,
    0x6E17, 0x7E36, 0x4E55, 0x5E74, 0x2E93, 0x3EB2, 0x0ED1, 0x1EF0,
};

u16 sd_crc16(const u8 *data, u_int len) {
    u16 crc = 0;
    while(len--) crc = (u16)((crc << 8) ^ crc16_table[((crc >> 8) ^ *data++) & 0xFF]);
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
