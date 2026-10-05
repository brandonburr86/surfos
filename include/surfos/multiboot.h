/*
SurfOS Multiboot Definitions
----------------------
File: multiboot.h   Date: 10/3/26 (roadmap M1); replaces boot/multiboot.h
                    Multiboot 2 and the framebuffer/ACPI fields 10/5/26 (USB boot)
----------------------
Shared between boot/boot.S (both headers) and kernel/multiboot.c, kernel/multiboot2.c
(the information blocks). Both protocols end up in one struct boot_info that the
kernel owns; nothing reads the loader's block after mb_init().
*/

#ifndef _SURFOS_MULTIBOOT_H
#define _SURFOS_MULTIBOOT_H

/* Multiboot 1: QEMU -kernel and the GRUB ISO */
#define MULTIBOOT_HEADER_MAGIC     0x1BADB002
#define MULTIBOOT_HEADER_FLAGS     0x00000003  /* page-align modules, pass memory information */
#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

/* Multiboot 2: GRUB on UEFI (the USB image) */
#define MULTIBOOT2_HEADER_MAGIC     0xE85250D6
#define MULTIBOOT2_ARCH_I386        0
#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36D76289

/* header tags (boot.S) */
#define MB2_HEADER_TAG_END          0
#define MB2_HEADER_TAG_FRAMEBUFFER  5
#define MB2_HEADER_TAG_MODULE_ALIGN 6

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

/* memory map types (the same numbers in both protocols) */
#define MB_MEMORY_AVAILABLE        1
#define MB_MEMORY_RESERVED         2
#define MB_MEMORY_ACPI_RECLAIMABLE 3
#define MB_MEMORY_NVS              4
#define MB_MEMORY_BADRAM           5

struct multiboot_module {
    u32 mod_start, mod_end;
    u32 string;
    u32 reserved;
};

/* Multiboot 2 information tags (the ones the parser looks at) */
#define MB2_TAG_END              0
#define MB2_TAG_CMDLINE          1
#define MB2_TAG_LOADER_NAME      2
#define MB2_TAG_MODULE           3
#define MB2_TAG_BASIC_MEMINFO    4
#define MB2_TAG_MMAP             6
#define MB2_TAG_FRAMEBUFFER      8
#define MB2_TAG_EFI32            11
#define MB2_TAG_EFI64            12
#define MB2_TAG_ACPI_OLD         14
#define MB2_TAG_ACPI_NEW         15
#define MB2_TAG_EFI_BS           18  /* boot services still running: SurfOS never asks for this */

#define MB2_INFO_MAX   0x40000      /* a sane upper bound for the whole block (GRUB's are a few KB) */

/* mb2_parse() results */
#define MB2_OK           0
#define MB2_E_ENVELOPE  -1          /* total size missing, too small or larger than the bound */
#define MB2_E_TAG       -2          /* a tag header or body runs past the end */
#define MB2_E_TAGSIZE   -3          /* tag size below 8, or wrong for its type */
#define MB2_E_NOEND     -4          /* no end tag */
#define MB2_E_STRING    -5          /* a string without its terminator */
#define MB2_E_MMAP      -6          /* malformed memory map (entry size, overflow) */
#define MB2_E_TOOMANY   -7          /* more memory map entries than MB_MAX_REGIONS */
#define MB2_E_MODULE    -8          /* a module that ends before it starts */

/* The kernel's own copy, taken before anything can overwrite the loader's block */
#define MB_MAX_REGIONS 512        /* EFI maps are long: GRUB passes one entry per EFI region */
#define MB_MAX_MODULES 8

struct mb_region {
    u64 base, len;
    u32 type;
};

struct mb_module {
    u32 start, end;
    char name[64];
};

/* Framebuffer as the loader set it up (Multiboot 2 tag 8). Only direct RGB is drawn on. */
#define MB_FB_INDEXED 0
#define MB_FB_RGB     1
#define MB_FB_EGA     2

struct boot_fb {
    bool present;               /* the loader described one */
    u64 addr;                   /* physical; may be above 4 GB */
    u32 pitch, width, height;
    u8 bpp, type;
    u8 red_pos, red_size, green_pos, green_size, blue_pos, blue_size;
};

/* ACPI root pointer, copied by the loader (tags 14/15) or found in the BIOS area */
struct boot_acpi {
    bool present;
    u8 revision;                /* 0: ACPI 1.0 (RSDT only), 2+: XSDT */
    char oem[7];
    u32 rsdt;
    u64 xsdt;
};

struct boot_info {
    bool valid;                 /* booted by a Multiboot loader */
    u32 protocol;               /* 1 or 2 */
    int error;                  /* MB2_E_* when the Multiboot 2 block was rejected */
    u32 mem_lower_kb, mem_upper_kb;
    char cmdline[256];
    char loader[64];
    u_int nregions;
    struct mb_region regions[MB_MAX_REGIONS];
    u_int nmods;
    u_int mods_dropped;         /* modules beyond MB_MAX_MODULES, reported and not used */
    struct mb_module mods[MB_MAX_MODULES];
    struct boot_fb fb;
    struct boot_acpi acpi;
    bool efi;                   /* the firmware is UEFI (the loader passed an EFI system table) */
    u64 efi_systab;
    bool efi_bs;                /* boot services were not terminated (unsupported) */
    bool laptop;                /* profile=laptop: native hardware, see kernel/main.c */
};

extern struct boot_info bootinfo;

void mb_init(u_long magic, u_long addr);
void mb_print(void);
int mb2_parse(const void *data, u32 bytes, struct boot_info *out);  /* 0 or MB2_E_* */
const char *mb2_strerror(int err);
bool bootinfo_has_arg(const struct boot_info *bi, const char *arg); /* a word of the command line */
const char *mb_region_type(u32 type);

#endif /* !__ASSEMBLER__ */
#endif
