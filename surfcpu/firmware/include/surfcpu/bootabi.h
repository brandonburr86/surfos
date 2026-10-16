/*
SurfCPU boot ABI
--------------------
File: bootabi.h Date: 10/15/26 (SurfCPU)
--------------------
Version 1 (surfcpu/docs/BOOT-ABI.md): the SURFOS.BIN header and the boot information
SurfBoot hands to a kernel. Shared by SurfBoot, the SurfOS RISC-V port and the host tests;
tools/surfimg.py packs the same layout. Constants come from spec/soc.toml.
*/
#ifndef SURFCPU_BOOTABI_H
#define SURFCPU_BOOTABI_H

#include <stdint.h>
#include "surfcpu/soc.h"

#define SURF_IMG_OFF_PAYLOAD 64u

struct surf_img_header {
    uint32_t magic;            /* 0x00 SURF_IMAGE_MAGIC */
    uint16_t version;          /* 0x04 SURF_IMAGE_HEADER_VERSION */
    uint16_t header_size;      /* 0x06 SURF_IMAGE_HEADER_SIZE */
    uint32_t isa;              /* 0x08 SURF_ISA_* bits the payload needs */
    uint32_t platform;         /* 0x0C SURF_PLATFORM_ABI */
    uint32_t load_addr;        /* 0x10 */
    uint32_t payload_size;     /* 0x14 */
    uint32_t mem_size;         /* 0x18 payload + BSS */
    uint32_t entry;            /* 0x1C */
    uint32_t payload_crc32;    /* 0x20 CRC-32/ISO-HDLC of the payload */
    uint32_t flags;            /* 0x24 0 */
    uint32_t image_version;    /* 0x28 major << 16 | minor << 8 | patch */
    uint32_t build_id;         /* 0x2C source commit, 8 hex digits */
    char     name[12];         /* 0x30 NUL-padded */
    uint32_t header_crc32;     /* 0x3C CRC-32 of bytes 0x00..0x3B */
};

#define SURF_BOOTFLAG_BACKUP          0x1u  /* the image was /BOOT/BACKUP.BIN */
#define SURF_BOOTFLAG_CMDLINE_TRUNC   0x2u  /* CMDLINE.TXT did not fit */
#define SURF_BOOTINFO_CMDLINE_LEN     128u

struct surf_boot_info {
    uint32_t magic;            /* 0x00 SURF_BOOTINFO_MAGIC */
    uint16_t version;          /* 0x04 SURF_BOOTINFO_VERSION */
    uint16_t size;             /* 0x06 sizeof(struct surf_boot_info) = 256 */
    uint32_t hart_id;          /* 0x08 */
    uint32_t board_id;         /* 0x0C sysinfo.BOARD_ID */
    uint32_t soc_version;      /* 0x10 */
    uint32_t map_version;      /* 0x14 */
    uint32_t sys_hz;           /* 0x18 */
    uint32_t timebase_hz;      /* 0x1C */
    uint32_t ram_base;         /* 0x20 */
    uint32_t ram_size;         /* 0x24 verified usable DDR bytes */
    uint32_t image_base;       /* 0x28 header.load_addr */
    uint32_t image_mem_size;   /* 0x2C header.mem_size */
    uint32_t boot_device;      /* 0x30 SURF_BOOT_DEVICE_SD */
    uint32_t part_lba;         /* 0x34 first sector of the FAT32 partition */
    uint32_t boot_flags;       /* 0x38 SURF_BOOTFLAG_* */
    uint32_t surfboot_version; /* 0x3C */
    uint32_t surfboot_build_id;/* 0x40 */
    uint32_t bootinfo_crc32;   /* 0x44 CRC-32 of the structure with this field 0 */
    uint32_t reserved[14];     /* 0x48 0 */
    char     cmdline[SURF_BOOTINFO_CMDLINE_LEN]; /* 0x80 NUL-terminated */
};

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(struct surf_img_header) == SURF_IMAGE_HEADER_SIZE, "image header is 64 bytes");
_Static_assert(sizeof(struct surf_boot_info) == 256, "boot info is 256 bytes");
_Static_assert(__builtin_offsetof(struct surf_img_header, header_crc32) == 0x3C, "header_crc32 at 0x3C");
_Static_assert(__builtin_offsetof(struct surf_boot_info, cmdline) == 0x80, "cmdline at 0x80");
#endif

#endif /* SURFCPU_BOOTABI_H */
