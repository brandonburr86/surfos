/*
SurfCPU directed tests: link script
--------------------
File: link.ld Date: 10/15/26 (SurfCPU)
--------------------
Everything in the 4 MiB RAM of build/cpu/surfcpu_tb (and Spike's -m0x80000000:0x400000),
starting at the reset pc 0x80000000. Code is writable: the FENCE.I tests patch it.
*/
OUTPUT_ARCH("riscv")
ENTRY(_start)

SECTIONS
{
    . = 0x80000000;
    .text.init : { *(.text.init) }
    . = ALIGN(0x100);
    .tohost : { *(.tohost) }
    . = ALIGN(0x100);
    .text : { *(.text) *(.text.*) }
    . = ALIGN(0x100);
    .data : { *(.data) *(.data.*) *(.sdata .sdata.*) *(.rodata .rodata.*) *(.srodata .srodata.*) }
    .bss : { *(.bss) *(.bss.*) *(.sbss .sbss.*) *(COMMON) }
    . = ALIGN(16);
    _end = .;
}
