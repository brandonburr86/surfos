/*
SurfCPU firmware: sections of a boot ROM image
--------------------
File: rom.ld Date: 10/15/26 (SurfCPU)
--------------------
Included by a program's own script after the generated firmware/ld/surfcpu_soc.ld (the
MEMORY regions and the SURF_* symbols) and after it defines:

    FW_RAM_BASE    where .data/.bss start in scratch RAM
    FW_RAM_END     the end of the program's RAM area; the stack grows down from here
    FW_STACK_MIN   bytes that must stay free between .bss and FW_RAM_END

Code and read-only data run from the boot ROM at the reset vector; .data is stored in the
ROM after them and copied by start.S. The image must fit the 64 KiB ROM.
*/
OUTPUT_ARCH(riscv)
ENTRY(_start)

SECTIONS
{
    .text SURF_BOOTROM_BASE : {
        KEEP(*(.text.start))
        *(.text.trap)
        *(.text .text.*)
        *(.fast .fast.*)                /* the ROM is block RAM already: nothing to copy */
    } > bootrom

    .rodata : ALIGN(4) {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
        . = ALIGN(4);
    } > bootrom

    .data FW_RAM_BASE : AT(LOADADDR(.rodata) + SIZEOF(.rodata)) ALIGN(4) {
        __data_start = .;
        *(.data .data.*)
        . = ALIGN(4);
        __global_pointer$ = . + 0x800;
        *(.sdata .sdata.*)
        . = ALIGN(4);
        __data_end = .;
    }
    __data_load = LOADADDR(.data);
    __rom_end = __data_load + SIZEOF(.data);

    /* An explicit address: with MEMORY regions, ld would otherwise put .bss at the start of
     * scratch RAM instead of after .data. */
    .bss (ADDR(.data) + SIZEOF(.data)) (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }
    __stack_bottom = __bss_end;
    __stack_top = FW_RAM_END;
    __fast_start = 0;
    __fast_end = 0;
    __fast_load = 0;

    /DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame .eh_frame_hdr) }
}

ASSERT(__rom_end <= SURF_BOOTROM_BASE + SURF_BOOTROM_SIZE, "rom.ld: the image does not fit the 64 KiB boot ROM")
ASSERT(__bss_end + FW_STACK_MIN <= FW_RAM_END, "rom.ld: .data + .bss + the minimum stack do not fit the RAM area")
ASSERT(FW_RAM_BASE >= SURF_SCRATCH_BASE && FW_RAM_END <= SURF_SCRATCH_BASE + SURF_SCRATCH_SIZE, "rom.ld: RAM area outside scratch RAM")
ASSERT(__bss_start >= __data_end && __bss_start < FW_RAM_END, "rom.ld: .bss is not after .data in the RAM area")
