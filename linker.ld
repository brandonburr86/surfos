/* SurfOS linker script.
 *
 * The kernel is a Multiboot 1 and Multiboot 2 ELF loaded at 1 MB. boot/boot.o is linked
 * first so both headers are near the start of the file (Multiboot 1 wants its header in
 * the first 8 KB, Multiboot 2 in the first 32 KB). Sections a 2004 kernel does not want
 * from a 2026 toolchain (.note.gnu.property, .eh_frame, .comment) are discarded so they
 * cannot end up as extra loadable segments.
 *
 * The page tables, the free-frame stack and the DMA heap are not part of the image:
 * pmm_init() places them in RAM the memory map says is free (mm/pmm.c).
 */
OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)
ENTRY(start)

SECTIONS
{
    . = 0x100000;
    __kernel_start = .;

    .text ALIGN(4096) :
    {
        *(.multiboot)
        *(.text .text.*)
    }

    .rodata ALIGN(4096) :
    {
        *(.rodata .rodata.*)
    }

    .data ALIGN(4096) :
    {
        *(.data .data.*)
    }

    /* kernel symbol table (tools/gensyms.py), after everything whose address it records */
    .ksyms ALIGN(4096) :
    {
        *(.ksyms)
    }

    .bss ALIGN(4096) :
    {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        __bss_end = .;
    }

    __kernel_end = .;

    /DISCARD/ :
    {
        *(.note.*)
        *(.comment)
        *(.eh_frame)
        *(.rel.eh_frame)
    }
}

/* the kernel runs from the 1:1 map of the first 16 MB */
ASSERT(__kernel_end <= 0x1000000, "SurfOS: the kernel image must end below 16 MB")
