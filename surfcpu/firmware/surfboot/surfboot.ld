/*
SurfBoot: link script
--------------------
File: surfboot.ld Date: 10/15/26 (SurfCPU)
--------------------
Code and constants in the 64 KiB boot ROM; .data, .bss and the stack inside the
surfboot_data reservation of scratch RAM (0x0001_0000-0x0001_EFFF), the stack growing down
from the boot information page at 0x0001_F000, which SurfBoot fills just before the
handoff and never otherwise touches.
*/
INCLUDE surfcpu_soc.ld

FW_RAM_BASE  = SURF_SURFBOOT_DATA_BASE;
FW_RAM_END   = SURF_SURFBOOT_DATA_BASE + SURF_SURFBOOT_DATA_SIZE;
FW_STACK_MIN = 0x2000;

INCLUDE rom.ld

ASSERT(FW_RAM_END == SURF_BOOTINFO_BASE, "surfboot.ld: the stack must end at the boot information page")
