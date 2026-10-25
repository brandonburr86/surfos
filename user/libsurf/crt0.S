/* SurfOS user program entry: the kernel leaves argc and argv on the stack, and since
   10/24/26 envp after them (SYS_spawnx): environ, and main's third argument.
   10/25/26 (L12): main is called with the stack 16-aligned, as the i386 ABI has it, so
   that a program built for SSE (-march=pentium4 -mfpmath=sse) may keep 16-byte values
   on it; the kernel starts _start aligned too (kernel/process.c). */
    .section .text.startup
    .globl _start
_start:
    movl (%esp), %eax           /* argc */
    movl 4(%esp), %ecx          /* argv */
    movl 8(%esp), %edx          /* envp */
    movl %edx, environ
    andl $-16, %esp
    subl $4, %esp               /* 4 bytes and the three arguments: 16 at the call */
    pushl %edx
    pushl %ecx
    pushl %eax
    call main
    movl %eax, (%esp)           /* exit's argument, the stack still aligned */
    call exit
1:  jmp 1b
    .section .note.GNU-stack,"",@progbits
