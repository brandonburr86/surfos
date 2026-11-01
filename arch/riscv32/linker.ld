/* SurfOS riscv32 linker script (10/15/26, SurfCPU).
 *
 * The board's board.ld (found through -L boards/<board>) says where the image loads and
 * where its RAM and kernel heap end: KERNEL_LOAD, RAM_START, RAM_END, HEAP_END, and names
 * the memory region the image goes in, kernel_ram (REGION_ALIAS). The Arty's takes all of
 * them from the generated surfcpu/firmware/ld/surfcpu_soc.ld, never typed by hand.
 *
 * The image is one block from KERNEL_LOAD: .text (the entry first, at the load address,
 * where SurfBoot and QEMU's reset vector both jump), .rodata, .ksyms, .data. That block is
 * surfos.payload.bin (BOOT-ABI.md section 1); .bss follows and the kernel clears it.
 *
 * .ksyms is the symbol table for backtraces (tools/gensyms.py), linked in a second pass as
 * on x86. Its room is fixed (KSYMS_RESERVE), so nothing after it moves between the passes
 * even with linker relaxation, which shortens code depending on addresses; kernel.mk checks
 * that both passes put every function at the same address.
 *
 * gp (__global_pointer$) points 2 KB into the small data, so .sdata and .sbss are reached
 * gp-relative by relaxed loads and stores; start.S and traps.S set gp before any C runs.
 */
OUTPUT_ARCH(riscv)
ENTRY(_start)

INCLUDE board.ld

KSYMS_RESERVE = 0x20000;        /* 128 KB: the table of about 2000 functions takes half of it */
DMAHEAP_SIZE  = 0x100000;       /* kalloc_dma(): 1 MB */

SECTIONS
{
    . = KERNEL_LOAD;
    __kernel_start = .;

    .text :
    {
        KEEP(*(.text.entry))
        *(.text.trap)
        *(.text .text.*)
    } > kernel_ram

    .rodata ALIGN(16) :
    {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
    } > kernel_ram

    .ksyms ALIGN(16) :
    {
        __ksyms_start = .;
        *(.ksyms)
        __ksyms_used = .;
        . = __ksyms_start + KSYMS_RESERVE;
    } > kernel_ram

    .data ALIGN(16) :
    {
        *(.data .data.*)
        . = ALIGN(16);
        __sdata_start = .;
        *(.sdata .sdata.*)
    } > kernel_ram
    __global_pointer$ = __sdata_start + 0x800;
    . = ALIGN(16);
    __payload_end = .;

    .bss ALIGN(16) (NOLOAD) :
    {
        __bss_start = .;
        *(.sbss .sbss.* .scommon)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } > kernel_ram
    __kernel_end = .;

    . = ALIGN(4096);
    __dmaheap_start = .;
    . = . + DMAHEAP_SIZE;
    __dmaheap_end = .;
    __kheap_start = .;
    __kheap_end = HEAP_END;
    __ram_start = RAM_START;
    __ram_end = RAM_END;

    /DISCARD/ :
    {
        *(.note.*)
        *(.comment)
        *(.eh_frame)
        *(.eh_frame_hdr)
    }
}

ASSERT(__ksyms_used <= __ksyms_start + KSYMS_RESERVE, "SurfOS: the symbol table outgrew KSYMS_RESERVE")
ASSERT(__kheap_end >= __kheap_start + 0x400000, "SurfOS: less than 4 MB left for the kernel heap")
ASSERT(__kheap_end <= __ram_end, "SurfOS: the kernel heap ends beyond the board's RAM")
ASSERT(_start == KERNEL_LOAD, "SurfOS: the entry must be the first byte of the image")
