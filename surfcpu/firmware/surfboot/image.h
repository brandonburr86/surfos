/*
SurfBoot: SURFOS.BIN packages
--------------------
File: image.h Date: 10/15/26 (SurfCPU)
--------------------
The acceptance checks of docs/BOOT-ABI.md section 2, in its order, each failing with its
own code and a sentence that gives the numbers: (1) size, magic, version, header size and
header CRC; (2) ISA bits and platform; (3) file size = 64 + payload_size; (4) alignment,
mem_size >= payload_size and the load range inside usable DDR, below ddr_top, computed
without overflow; (5) the entry inside the payload; (6) the payload CRC as read from the
card, and again as read back from DDR after copying.
*/
#ifndef SURFBOOT_IMAGE_H
#define SURFBOOT_IMAGE_H

#include <stdint.h>
#include "surfcpu/bootabi.h"
#include "fat32.h"

#define SURFBOOT_ISA (SURF_ISA_I | SURF_ISA_M | SURF_ISA_ZICSR | SURF_ISA_ZIFENCEI)

struct img_info {
    struct surf_img_header h;
    uint32_t file_size;
    uint32_t crc_card, crc_ddr;
    uint32_t fragments;
    uint32_t load_us;           /* time spent reading and copying */
};

/* Copies the 64 header bytes into h (byte by byte) and runs checks 1-5.
 * Usable DDR is [ram_base, ram_limit). */
int img_check_header(const uint8_t *bytes, uint32_t file_size, uint32_t ram_base,
                     uint32_t ram_limit, struct surf_img_header *h);

/* Opens and loads a package: checks 1-5, the payload into DDR, then check 6.
 * progress(done, total) is called every 64 KiB (may be NULL). */
typedef void (*img_progress_fn)(uint32_t done, uint32_t total);
int img_load(struct fat_vol *v, const struct fat_dirent *d, uint32_t ram_base, uint32_t ram_limit,
             struct img_info *out, img_progress_fn progress);

void img_isa_string(uint32_t isa, char *buf, unsigned size);

#endif /* SURFBOOT_IMAGE_H */
