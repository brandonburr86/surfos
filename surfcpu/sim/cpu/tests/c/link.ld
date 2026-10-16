/*
SurfCPU C tests: link script
--------------------
File: link.ld Date: 10/15/26 (SurfCPU)
--------------------
The C programs of sim/cpu/tests/c/ in the 4 MiB RAM of build/cpu/surfcpu_tb (and Spike's
-m0x80000000:0x400000): code from the reset pc 0x80000000, the global pointer in the small
data, .bss cleared by crt0.S, the stack growing down from the top of the RAM.
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
    .text : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.* .srodata .srodata.*) }
    . = ALIGN(16);
    .data : { *(.data .data.*) }
    .sdata : {
        __global_pointer$ = . + 0x800;
        *(.sdata .sdata.*)
    }
    . = ALIGN(4);
    __bss_start = .;
    .sbss : { *(.sbss .sbss.*) }
    .bss : { *(.bss .bss.*) *(COMMON) }
    . = ALIGN(4);
    __bss_end = .;
    _end = .;
    __stack_top = 0x80400000;
}
