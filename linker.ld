/* SurfOS linker script.
 *
 * The kernel is a Multiboot 1 ELF loaded at 1 MB. boot/boot.o is linked first so the
 * Multiboot header is inside the first 8 KB of the file. Sections a 2004 kernel does
 * not want from a 2026 toolchain (.note.gnu.property, .eh_frame, .comment) are
 * discarded so they cannot end up as extra loadable segments.
 */
OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)
ENTRY(start)

SECTIONS
{
    . = 0x100000;

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
