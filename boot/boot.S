/* boot.S - bootstrap the kernel */

     #include <surfos/multiboot.h>

             .text

             .globl  start, _start
     start:
     _start:
             jmp     multiboot_entry

             /* Align 32 bits boundary. */
             .align  4

             /* Multiboot header. */
     multiboot_header:
             /* magic */
             .long   MULTIBOOT_HEADER_MAGIC
             /* flags */
             .long   MULTIBOOT_HEADER_FLAGS
             /* checksum */
             .long   -(MULTIBOOT_HEADER_MAGIC + MULTIBOOT_HEADER_FLAGS)
     /*#ifndef __ELF__
             .long   multiboot_header
             .long   _start
             .long   _edata
             .long   _end
             .long   multiboot_entry
     /*#endif*/

             /* Multiboot 2 header (10/5/26): GRUB on UEFI boots the USB image with it.
                8-byte aligned and inside the first 32 KB; no EFI tags, so the loader
                exits boot services and enters in 32-bit protected mode like Multiboot 1. */
             .align  8
     multiboot2_header:
             .long   MULTIBOOT2_HEADER_MAGIC
             .long   MULTIBOOT2_ARCH_I386
             .long   multiboot2_header_end - multiboot2_header
             .long   0x100000000 - (MULTIBOOT2_HEADER_MAGIC + MULTIBOOT2_ARCH_I386 + (multiboot2_header_end - multiboot2_header))
             /* a linear framebuffer: keep the firmware's mode (0 x 0), 32 bits per pixel preferred */
             .align  8
             .short  MB2_HEADER_TAG_FRAMEBUFFER
             .short  0
             .long   20
             .long   0
             .long   0
             .long   32
             /* page-aligned modules (the initrd becomes ramdisk rd0) */
             .align  8
             .short  MB2_HEADER_TAG_MODULE_ALIGN
             .short  0
             .long   8
             .align  8
             .short  MB2_HEADER_TAG_END
             .short  0
             .long   8
     multiboot2_header_end:

     multiboot_entry:
             /* Initialize the stack pointer. */
             movl    $(stack + BOOT_STACK_SIZE), %esp

             /* Reset EFLAGS. */
             pushl   $0
             popf

             /* Push the pointer to the Multiboot information structure. */
             pushl   %ebx
             /* Push the magic value (Multiboot 1 or 2: kmain tells them apart). */
             pushl   %eax

             /* Now enter the C main function... */
             call       kmain
             /* Halt. */
             pushl   $halt_message
             call    kprintf

     loop:   hlt
             jmp     loop

     halt_message:
             .asciz  "System Halted."

             /* Our stack area. */
             .comm   stack, BOOT_STACK_SIZE

/*.data
_edata:*/
