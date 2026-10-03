/* SurfOS user program entry: the kernel leaves argc and argv on the stack. */
    .section .text.startup
    .globl _start
_start:
    movl (%esp), %eax           /* argc */
    movl 4(%esp), %ecx          /* argv */
    pushl %ecx
    pushl %eax
    call main
    pushl %eax
    call exit
1:  jmp 1b
    .section .note.GNU-stack,"",@progbits
