/*
SurfOS Multiboot Information
--------------------
File: multiboot.c   Date: 10/3/26 (roadmap M1)
--------------------
kmain() received the loader's magic and info pointer since 2004 and ignored both.
This copies what the kernel needs (memory map, modules, command line) into its own
memory before paging is enabled, while the loader's block is still addressable.
*/

#include <surfos/types.h>
#include <surfos/multiboot.h>
#include <surfos/console.h>
#include <blibc_common.h>

struct boot_info bootinfo;

void mb_init(u_long magic, u_long addr) {
    struct multiboot_info *mbi = (struct multiboot_info *)addr;

    memset(&bootinfo, 0, sizeof(bootinfo));
    if(magic != MULTIBOOT_BOOTLOADER_MAGIC || !mbi) {
        kprintf("multiboot: no information block (magic 0x%lx)\n", magic);
        return;
    }
    bootinfo.valid = true;

    if(mbi->flags & MB_INFO_MEM) {
        bootinfo.mem_lower_kb = mbi->mem_lower;
        bootinfo.mem_upper_kb = mbi->mem_upper;
    }
    if((mbi->flags & MB_INFO_CMDLINE) && mbi->cmdline) {
        strlcpy(bootinfo.cmdline, (const char *)mbi->cmdline, sizeof(bootinfo.cmdline));
    }
    if(mbi->flags & MB_INFO_MMAP) {
        u_long p = mbi->mmap_addr, end = mbi->mmap_addr + mbi->mmap_length;
        while(p < end && bootinfo.nregions < MB_MAX_REGIONS) {
            struct multiboot_mmap_entry *e = (struct multiboot_mmap_entry *)p;
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
        for(i = 0; i < mbi->mods_count && i < MB_MAX_MODULES; i++) {
            bootinfo.mods[i].start = m[i].mod_start;
            bootinfo.mods[i].end = m[i].mod_end;
            if(m[i].string) strlcpy(bootinfo.mods[i].name, (const char *)m[i].string, sizeof(bootinfo.mods[i].name));
            bootinfo.nmods++;
        }
    }
}

void mb_print(void) {
    u_int i;
    if(!bootinfo.valid) {
        kprintf("multiboot: not booted by a Multiboot loader\n");
        return;
    }
    kprintf("multiboot: %u KB low, %u KB high", bootinfo.mem_lower_kb, bootinfo.mem_upper_kb);
    if(bootinfo.cmdline[0]) kprintf(", cmdline \"%s\"", bootinfo.cmdline);
    kprintf("\n");
    for(i = 0; i < bootinfo.nregions; i++) {
        kprintf("  region 0x%08lx-0x%08lx %s\n", (u_long)bootinfo.regions[i].base,
                (u_long)(bootinfo.regions[i].base + bootinfo.regions[i].len - 1),
                bootinfo.regions[i].type == MB_MEMORY_AVAILABLE ? "available" : "reserved");
    }
    for(i = 0; i < bootinfo.nmods; i++) {
        kprintf("  module 0x%08x-0x%08x \"%s\"\n", bootinfo.mods[i].start, bootinfo.mods[i].end, bootinfo.mods[i].name);
    }
}
