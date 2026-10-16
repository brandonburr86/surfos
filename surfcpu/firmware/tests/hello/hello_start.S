/*
SurfCPU firmware tests: the handoff test kernel's entry
--------------------
File: hello_start.S Date: 10/15/26 (SurfCPU)
--------------------
Records the machine state SurfBoot hands over (a0..a2, mtvec, mstatus, mie, mscratch,
before anything changes them), sets up gp, sp and .bss as a kernel must (docs/BOOT-ABI.md
section 4), then calls hello_main(state). It installs no trap handler on purpose: a trap
must reach SurfBoot's fault reporter in ROM.
*/
    .option norelax
    .section .text.start, "ax"
    .globl _start
_start:
    mv      s0, a0
    mv      s1, a1
    mv      s2, a2
    csrr    s3, mtvec
    csrr    s4, mstatus
    csrr    s5, mie
    csrr    s6, mscratch
    la      gp, __global_pointer$
    la      sp, __stack_top
    la      t0, __bss_start
    la      t1, __bss_end
1:  bgeu    t0, t1, 2f
    sw      zero, 0(t0)
    addi    t0, t0, 4
    j       1b
2:  la      t0, entry_state
    sw      s0, 0(t0)
    sw      s1, 4(t0)
    sw      s2, 8(t0)
    sw      s3, 12(t0)
    sw      s4, 16(t0)
    sw      s5, 20(t0)
    sw      s6, 24(t0)
    mv      a0, t0
    call    hello_main
3:  wfi
    j       3b

    .section .bss.entry_state, "aw", @nobits
    .balign 4
    .globl entry_state
entry_state:
    .space 28

    /* hello_illegal(): an all-zero word, the illegal instruction the "illegal" command line asks
     * for; in .probes, the output section where tools/isa-audit.py accepts data words */
    .section .probes, "ax"
    .globl hello_illegal
hello_illegal:
    .word   0x00000000
    ret
