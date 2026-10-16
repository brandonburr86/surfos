/*
SurfCPU diagnostics: link script of the BRAM program
--------------------
File: bramdiag.ld Date: 10/15/26 (SurfCPU)
--------------------
A boot ROM image like SurfBoot, but with its data, trap stack and stack in the top 8 KiB of
scratch RAM, so that the 56 KiB below (__diag_scratch_lo..__diag_scratch_hi) can be tested
destructively.
*/
INCLUDE surfcpu_soc.ld

FW_RAM_BASE  = SURF_SCRATCH_BASE + SURF_SCRATCH_SIZE - 0x2000;
FW_RAM_END   = SURF_SCRATCH_BASE + SURF_SCRATCH_SIZE;
FW_STACK_MIN = 0xC00;

__diag_scratch_lo = SURF_SCRATCH_BASE;
__diag_scratch_hi = FW_RAM_BASE;

INCLUDE rom.ld
