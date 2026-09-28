/* boot.S - bootstrap the kernel */

     #define ASM     1
     #include "multiboot.h"

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

     multiboot_entry:
             /* Initialize the stack pointer. */
             movl    $(stack + STACK_SIZE), %esp

             /* Reset EFLAGS. */
             pushl   $0
             popf

             /* Push the pointer to the Multiboot information structure. */
             pushl   %ebx
             /* Push the magic value. */
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
             .comm   stack, STACK_SIZE

/*.data
_edata:*/
