/* SurfOS user program entry: the kernel leaves argc and argv on the stack, and since
   10/24/26 envp after them (SYS_spawnx): environ, and main's third argument. */
    .section .text.startup
    .globl _start
_start:
    movl (%esp), %eax           /* argc */
    movl 4(%esp), %ecx          /* argv */
    movl 8(%esp), %edx          /* envp */
    movl %edx, environ
    pushl %edx
    pushl %ecx
    pushl %eax
    call main
    pushl %eax
    call exit
1:  jmp 1b
    .section .note.GNU-stack,"",@progbits
