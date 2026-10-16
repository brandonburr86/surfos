/*
SurfOS riscv32 Boot Information
--------------------
File: bootinfo.c    Date: 10/15/26 (SurfCPU)
--------------------
SurfBoot hands over struct surf_boot_info (surfcpu/docs/BOOT-ABI.md section 4): its
address in a1, SURF_BOOTINFO_MAGIC in a2, the block itself in scratch RAM, preserved
only until the kernel has copied it. bootinfo_init() runs first thing in kmain(): it
takes the block only when a2 says so, the address is aligned and in RAM, and magic,
version, size and CRC all agree; then it copies it and never looks at scratch RAM
again. QEMU's virt machine has no SurfBoot (a2 is something else there): the board
fills in what it knows.

The rest of the kernel reads the x86 kernel's struct boot_info (surfos/multiboot.h),
which on riscv32 holds the command line (SurfBoot's /BOOT/CMDLINE.TXT), the RAM range
and the loader's name; nothing in it is Multiboot's. bootinfo_has_arg() is
kernel/multiboot2.c's.
*/

#include <surfos/types.h>
#include <surfos/multiboot.h>
#include <surfos/console.h>
#include <surfcpu/bootabi.h>
#include <riscv/arch.h>
#include <riscv/board.h>
#include <blibc_common.h>

struct boot_info bootinfo;
static struct surf_boot_info sbi;           /* the kernel's copy */
static bool have_sbi;
static const char *problem;
static u_long handoff_hart, handoff_addr, handoff_magic;

static bool addr_ok(u_long a) {
    if(a & 3) return false;
    if(a >= SURF_SCRATCH_BASE && a + sizeof(struct surf_boot_info) <= SURF_SCRATCH_BASE + SURF_SCRATCH_SIZE) return true;
    return a >= SURF_DDR_BASE && a + sizeof(struct surf_boot_info) <= SURF_DDR_BASE + SURF_DDR_SIZE;
}

void bootinfo_init(u_long hartid, u_long addr, u_long magic) {
    const struct surf_boot_info *bi = (const struct surf_boot_info *)addr;
    u32 want;

    handoff_hart = hartid;
    handoff_addr = addr;
    handoff_magic = magic;
    memset(&bootinfo, 0, sizeof(bootinfo));
    if(magic != SURF_BOOTINFO_MAGIC) problem = "no SurfBoot handoff (a2 is not SURF_BOOTINFO_MAGIC)";
    else if(!addr_ok(addr)) problem = "the boot information address is not aligned RAM";
    else if(bi->magic != SURF_BOOTINFO_MAGIC) problem = "the boot information's magic is wrong";
    else if(bi->version != SURF_BOOTINFO_VERSION) problem = "the boot information is not version 1";
    else if(bi->size != sizeof(struct surf_boot_info)) problem = "the boot information's size is not 256";
    else {
        memcpy(&sbi, bi, sizeof(sbi));
        want = sbi.bootinfo_crc32;
        sbi.bootinfo_crc32 = 0;
        if(crc32(&sbi, sizeof(sbi)) != want) problem = "the boot information's CRC does not match";
        else {
            sbi.bootinfo_crc32 = want;
            sbi.cmdline[SURF_BOOTINFO_CMDLINE_LEN - 1] = 0;
            have_sbi = true;
        }
    }
    if(have_sbi) {
        bootinfo.valid = true;
        snprintf(bootinfo.loader, sizeof(bootinfo.loader), "SurfBoot %lu.%lu.%lu (%08lx)",
                 (u_long)(sbi.surfboot_version >> 16), (u_long)((sbi.surfboot_version >> 8) & 0xFF),
                 (u_long)(sbi.surfboot_version & 0xFF), (u_long)sbi.surfboot_build_id);
        strlcpy(bootinfo.cmdline, sbi.cmdline, sizeof(bootinfo.cmdline));
        bootinfo.nregions = 1;
        bootinfo.regions[0].base = sbi.ram_base;
        bootinfo.regions[0].len = sbi.ram_size;
        bootinfo.regions[0].type = MB_MEMORY_AVAILABLE;
    }
    board_boot_info(have_sbi ? &sbi : NULL);
}

const struct surf_boot_info *surfboot_info(void) {
    return have_sbi ? &sbi : NULL;
}

const char *bootinfo_problem(void) {
    return have_sbi ? NULL : problem;
}

/* a whole word of the kernel command line (as kernel/multiboot2.c) */
bool bootinfo_has_arg(const struct boot_info *bi, const char *arg) {
    const char *s = bi->cmdline;
    size_t n = strlen(arg);
    while(*s) {
        const char *w;
        while(*s == ' ') s++;
        w = s;
        while(*s && *s != ' ') s++;
        if((size_t)(s - w) == n && n && !strncmp(w, arg, n)) return true;
    }
    return false;
}

static const char *board_id_name(u32 id) {
    switch(id) {
    case SURF_BOARD_SIM: return "simulation";
    case SURF_BOARD_ARTY_A7_35T: return "Arty A7-35T";
    case SURF_BOARD_ARTY_A7_100T: return "Arty A7-100T";
    }
    return "unknown board";
}

void bootinfo_print(void) {
    printf("  board: %s, handoff a0 %lu, a1 0x%08lx, a2 0x%08lx\n", board_name(), handoff_hart, handoff_addr, handoff_magic);
    if(!have_sbi) {
        printf("  boot information: none (%s)\n", problem ? problem : "?");
    } else {
        printf("  boot information: %s, version %u, CRC 0x%08lx ok\n", bootinfo.loader, sbi.version, (u_long)sbi.bootinfo_crc32);
        printf("  board id 0x%04lx (%s), SoC %lu.%lu.%lu, memory map %lu, sys clock %lu Hz, timebase %lu Hz\n",
               (u_long)sbi.board_id, board_id_name(sbi.board_id), (u_long)(sbi.soc_version >> 16),
               (u_long)((sbi.soc_version >> 8) & 0xFF), (u_long)(sbi.soc_version & 0xFF), (u_long)sbi.map_version,
               (u_long)sbi.sys_hz, (u_long)sbi.timebase_hz);
        printf("  RAM 0x%08lx + %lu MB (calibrated, SurfBoot's quick test passed)\n", (u_long)sbi.ram_base,
               (u_long)(sbi.ram_size >> 20));
        printf("  image 0x%08lx + %lu bytes; boot device %s, FAT32 at LBA %lu%s%s\n", (u_long)sbi.image_base,
               (u_long)sbi.image_mem_size,
               sbi.boot_device == SURF_BOOT_DEVICE_SD ? "SD" : "?", (u_long)sbi.part_lba,
               (sbi.boot_flags & SURF_BOOTFLAG_BACKUP) ? ", from BACKUP.BIN" : "",
               (sbi.boot_flags & SURF_BOOTFLAG_CMDLINE_TRUNC) ? ", command line truncated" : "");
    }
    printf("  command line: '%s'\n", bootinfo.cmdline);
    board_print_info();
}
