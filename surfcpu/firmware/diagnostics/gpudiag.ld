/*
SurfGPU diagnostics: link script of the GPU diagnostic
--------------------
File: gpudiag.ld Date: 10/18/26 (SurfGPU)
--------------------
A boot ROM image like SurfBoot (code and constants in the boot ROM, .data, .bss and the stack in
the surfboot_data reservation of scratch RAM): gpudiag tests VRAM and the GPU, not scratch RAM.
*/
INCLUDE surfcpu_soc.ld

FW_RAM_BASE  = SURF_SURFBOOT_DATA_BASE;
FW_RAM_END   = SURF_SURFBOOT_DATA_BASE + SURF_SURFBOOT_DATA_SIZE;
FW_STACK_MIN = 0x2000;

INCLUDE rom.ld
