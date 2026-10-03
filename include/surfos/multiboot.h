/*
SurfOS Multiboot 1 Definitions
----------------------
File: multiboot.h   Date: 10/3/26 (roadmap M1); replaces boot/multiboot.h
----------------------
Shared between boot/boot.S (the header) and kernel/multiboot.c (the info block).
*/

#ifndef _SURFOS_MULTIBOOT_H
#define _SURFOS_MULTIBOOT_H

#define MULTIBOOT_HEADER_MAGIC     0x1BADB002
#define MULTIBOOT_HEADER_FLAGS     0x00000003  /* page-align modules, pass memory information */
#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

#define BOOT_STACK_SIZE 0x4000

#ifndef __ASSEMBLER__

#include <surfos/types.h>

/* what the boot loader leaves in EBX (only the fields we read) */
struct multiboot_info {
    u32 flags;
    u32 mem_lower, mem_upper;   /* KB below 1 MB, KB above 1 MB */
    u32 boot_device;
    u32 cmdline;
    u32 mods_count, mods_addr;
    u32 syms[4];
    u32 mmap_length, mmap_addr;
} __attribute__((packed));

#define MB_INFO_MEM      0x001
#define MB_INFO_BOOTDEV  0x002
#define MB_INFO_CMDLINE  0x004
#define MB_INFO_MODS     0x008
#define MB_INFO_MMAP     0x040

struct multiboot_mmap_entry {
    u32 size;                   /* of the rest of the entry */
    u64 addr;
    u64 len;
    u32 type;                   /* 1 = available RAM */
} __attribute__((packed));

#define MB_MEMORY_AVAILABLE 1

struct multiboot_module {
    u32 mod_start, mod_end;
    u32 string;
    u32 reserved;
};

/* The kernel's own copy, taken before anything can overwrite the loader's block */
#define MB_MAX_REGIONS 32
#define MB_MAX_MODULES 8

struct mb_region {
    u64 base, len;
    u32 type;
};

struct mb_module {
    u32 start, end;
    char name[64];
};

struct boot_info {
    bool valid;                 /* booted by a Multiboot loader */
    u32 mem_lower_kb, mem_upper_kb;
    char cmdline[256];
    u_int nregions;
    struct mb_region regions[MB_MAX_REGIONS];
    u_int nmods;
    struct mb_module mods[MB_MAX_MODULES];
};

extern struct boot_info bootinfo;

void mb_init(u_long magic, u_long addr);
void mb_print(void);

#endif /* !__ASSEMBLER__ */
#endif
