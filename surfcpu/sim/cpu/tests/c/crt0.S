/*
SurfCPU C tests: start-up code
--------------------
File: crt0.S Date: 10/15/26 (SurfCPU)
--------------------
For the C programs in sim/cpu/tests/c/: sets the stack and the global pointer, clears
.bss, installs a trap vector that fails the test, calls main and reports its result
through tohost: main() returning 0 is a pass, n > 0 a failure with test number n.
*/
        .section .text.init, "ax", @progbits
        .globl  _start
_start:
        .option push
        .option norelax
        la      gp, __global_pointer$
        .option pop
        la      sp, __stack_top
        la      t0, trap
        csrw    mtvec, t0
        la      t0, __bss_start
        la      t1, __bss_end
1:      bgeu    t0, t1, 2f
        sw      zero, 0(t0)
        addi    t0, t0, 4
        j       1b
2:      call    main
        bnez    a0, 3f
        li      a0, 1                           // pass
        j       report
3:      slli    a0, a0, 1                       // fail: (n << 1) | 1
        ori     a0, a0, 1
report: la      t0, tohost
        sw      a0, 0(t0)
4:      j       4b

        .balign 4
trap:   li      a0, (1022 << 1) | 1             // an unexpected trap
        j       report

        .section .tohost, "aw", @progbits
        .balign 64
        .globl  tohost
tohost: .dword  0
        .balign 64
        .globl  fromhost
fromhost: .dword 0
