/*
SurfCPU firmware: a SURFOS.BIN program in DDR
--------------------
File: ram.ld Date: 10/15/26 (SurfCPU)
--------------------
For programs SurfBoot loads from the card (the DDR diagnostic, the test kernels): linked at
the default kernel base 0x8000_0000 (the `kernel` reservation of docs/MEMORY-MAP.md), with
load address = run address, so the package payload is the bytes from the first section to
the end of .data, and mem_size (payload + .bss + stack) comes from the ELF. start.S
(built with -DFW_RAM_IMAGE) clears .bss itself: SurfBoot does not (BOOT-ABI.md section 2).

FW_STACK_SIZE may be set with --defsym; the default is 16 KiB.
*/
INCLUDE surfcpu_soc.ld
OUTPUT_ARCH(riscv)
ENTRY(_start)

FW_STACK_SIZE = DEFINED(FW_STACK_SIZE) ? FW_STACK_SIZE : 0x4000;

SECTIONS
{
    .text SURF_KERNEL_BASE : {
        KEEP(*(.text.start))
        *(.text.trap)
        *(.text .text.*)
    } > ddr

    .rodata : ALIGN(4) {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
        . = ALIGN(4);
    } > ddr

    .data : ALIGN(4) {
        __data_start = .;
        *(.data .data.*)
        . = ALIGN(4);
        __global_pointer$ = . + 0x800;
        *(.sdata .sdata.*)
        . = ALIGN(4);
        __data_end = .;
    } > ddr
    __data_load = __data_start;
    __payload_end = .;

    .bss : ALIGN(16) {
        __bss_start = .;
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } > ddr

    .stack (NOLOAD) : ALIGN(16) {
        __stack_bottom = .;
        . += FW_STACK_SIZE;
        __stack_top = .;
    } > ddr
    __image_end = .;
    __rom_end = __payload_end;

    /DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame .eh_frame_hdr) }
}

ASSERT(__image_end <= SURF_DDR_TOP_BASE, "ram.ld: the image reaches into ddr_top, which SurfBoot refuses")
