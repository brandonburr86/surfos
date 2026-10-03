/* SurfOS user programs: static ELF, loaded at the bottom of the user range. */
OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)
ENTRY(_start)

SECTIONS
{
    . = 0x40000000;
    .text ALIGN(4096) : { *(.text.startup) *(.text .text.*) }
    .rodata ALIGN(4096) : { *(.rodata .rodata.*) }
    .data ALIGN(4096) : { *(.data .data.*) }
    .bss ALIGN(4) : { *(COMMON) *(.bss .bss.*) }
    /DISCARD/ : { *(.note.*) *(.comment) *(.eh_frame) *(.rel.eh_frame) }
}
