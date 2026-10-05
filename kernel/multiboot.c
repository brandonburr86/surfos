/*
SurfOS Multiboot Information
--------------------
File: multiboot.c   Date: 10/3/26 (roadmap M1), Multiboot 2 dispatch 10/5/26 (USB boot)
--------------------
kmain() received the loader's magic and info pointer since 2004 and ignored both.
This copies what the kernel needs (memory map, modules, command line, framebuffer,
ACPI root pointer) into its own memory before paging is enabled, while the loader's
block is still addressable. The magic number says which protocol: Multiboot 1 from
QEMU -kernel and the GRUB ISO, Multiboot 2 from GRUB on UEFI (kernel/multiboot2.c).
*/

#include <surfos/types.h>
#include <surfos/multiboot.h>
#include <surfos/console.h>
#include <blibc_common.h>

struct boot_info bootinfo;

static void mb1_init(struct multiboot_info *mbi) {
    bootinfo.valid = true;
    bootinfo.protocol = 1;

    if(mbi->flags & MB_INFO_MEM) {
        bootinfo.mem_lower_kb = mbi->mem_lower;
        bootinfo.mem_upper_kb = mbi->mem_upper;
    }
    if((mbi->flags & MB_INFO_CMDLINE) && mbi->cmdline) {
        strlcpy(bootinfo.cmdline, (const char *)mbi->cmdline, sizeof(bootinfo.cmdline));
    }
    if(mbi->flags & MB_INFO_MMAP) {
        u_long p = mbi->mmap_addr, end = mbi->mmap_addr + mbi->mmap_length;
        while(p < end) {
            struct multiboot_mmap_entry *e = (struct multiboot_mmap_entry *)p;
            if(bootinfo.nregions == MB_MAX_REGIONS) {
                kprintf("multiboot: more than %u memory map entries, the rest is ignored\n", MB_MAX_REGIONS);
                break;
            }
            bootinfo.regions[bootinfo.nregions].base = e->addr;
            bootinfo.regions[bootinfo.nregions].len = e->len;
            bootinfo.regions[bootinfo.nregions].type = e->type;
            bootinfo.nregions++;
            p += e->size + 4;
        }
    } else if(mbi->flags & MB_INFO_MEM) {
        /* no map: synthesize the two classic regions from the sizes */
        bootinfo.regions[0].base = 0;
        bootinfo.regions[0].len = (u64)mbi->mem_lower * 1024;
        bootinfo.regions[0].type = MB_MEMORY_AVAILABLE;
        bootinfo.regions[1].base = 0x100000;
        bootinfo.regions[1].len = (u64)mbi->mem_upper * 1024;
        bootinfo.regions[1].type = MB_MEMORY_AVAILABLE;
        bootinfo.nregions = 2;
    }
    if(mbi->flags & MB_INFO_MODS) {
        struct multiboot_module *m = (struct multiboot_module *)mbi->mods_addr;
        u_int i;
        for(i = 0; i < mbi->mods_count; i++) {
            if(bootinfo.nmods == MB_MAX_MODULES) { bootinfo.mods_dropped++; continue; }
            bootinfo.mods[bootinfo.nmods].start = m[i].mod_start;
            bootinfo.mods[bootinfo.nmods].end = m[i].mod_end;
            if(m[i].string) strlcpy(bootinfo.mods[bootinfo.nmods].name, (const char *)m[i].string, sizeof(bootinfo.mods[0].name));
            bootinfo.nmods++;
        }
    }
}

/* The block is GRUB's: check the envelope before handing it to the bounded parser. */
static void mb2_init(u_long addr) {
    u32 total;
    int r;
    if(!addr || (addr & 7)) {
        kprintf("multiboot2: information block at 0x%lx is not 8-byte aligned\n", addr);
        bootinfo.error = MB2_E_ENVELOPE;
        bootinfo.protocol = 2;
        return;
    }
    total = *(volatile u32 *)addr;
    if(total < 16 || total > MB2_INFO_MAX) {
        kprintf("multiboot2: information block claims %u bytes\n", total);
        bootinfo.error = MB2_E_ENVELOPE;
        bootinfo.protocol = 2;
        return;
    }
    r = mb2_parse((const void *)addr, total, &bootinfo);
    if(r) {
        kprintf("multiboot2: information block rejected: %s\n", mb2_strerror(r));
        bootinfo.protocol = 2;                  /* kmain() stops before touching memory */
    }
}

void mb_init(u_long magic, u_long addr) {
    memset(&bootinfo, 0, sizeof(bootinfo));
    if(magic == MULTIBOOT2_BOOTLOADER_MAGIC) mb2_init(addr);
    else if(magic == MULTIBOOT_BOOTLOADER_MAGIC && addr) mb1_init((struct multiboot_info *)addr);
    else kprintf("multiboot: no information block (magic 0x%lx)\n", magic);
    bootinfo.laptop = bootinfo_has_arg(&bootinfo, "profile=laptop");
}

void mb_print(void) {
    u_int i;
    if(!bootinfo.valid) {
        kprintf("multiboot: not booted by a Multiboot loader%s%s\n", bootinfo.error ? ": " : "",
                bootinfo.error ? mb2_strerror(bootinfo.error) : "");
        return;
    }
    kprintf("multiboot%s: %u KB low, %u KB high", bootinfo.protocol == 2 ? "2" : "", bootinfo.mem_lower_kb, bootinfo.mem_upper_kb);
    if(bootinfo.loader[0]) kprintf(", loader \"%s\"", bootinfo.loader);
    if(bootinfo.cmdline[0]) kprintf(", cmdline \"%s\"", bootinfo.cmdline);
    kprintf("\n");
    kprintf("  profile %s, firmware %s\n", bootinfo.laptop ? "laptop" : "emulator",
            bootinfo.efi ? "UEFI (boot services exited)" : "BIOS or unknown");
    for(i = 0; i < bootinfo.nregions; i++) {
        kprintf("  region 0x%09llx-0x%09llx %s\n", bootinfo.regions[i].base,
                bootinfo.regions[i].base + bootinfo.regions[i].len - 1, mb_region_type(bootinfo.regions[i].type));
    }
    for(i = 0; i < bootinfo.nmods; i++) {
        kprintf("  module 0x%08x-0x%08x \"%s\"\n", bootinfo.mods[i].start, bootinfo.mods[i].end, bootinfo.mods[i].name);
    }
    if(bootinfo.mods_dropped) kprintf("  %u more modules ignored (at most %u are used)\n", bootinfo.mods_dropped, MB_MAX_MODULES);
    if(bootinfo.fb.present) {
        kprintf("  framebuffer 0x%llx, %ux%u, %u bpp, pitch %u, type %u", bootinfo.fb.addr, bootinfo.fb.width,
                bootinfo.fb.height, bootinfo.fb.bpp, bootinfo.fb.pitch, bootinfo.fb.type);
        if(bootinfo.fb.type == MB_FB_RGB)
            kprintf(", RGB %u:%u %u:%u %u:%u", bootinfo.fb.red_pos, bootinfo.fb.red_size, bootinfo.fb.green_pos,
                    bootinfo.fb.green_size, bootinfo.fb.blue_pos, bootinfo.fb.blue_size);
        kprintf("\n");
    }
    if(bootinfo.acpi.present) {
        kprintf("  ACPI RSDP revision %u, OEM \"%s\", RSDT 0x%08x, XSDT 0x%llx\n", bootinfo.acpi.revision,
                bootinfo.acpi.oem, bootinfo.acpi.rsdt, bootinfo.acpi.xsdt);
    }
    if(bootinfo.efi_bs) kprintf("  EFI boot services were left running (not supported)\n");
}
