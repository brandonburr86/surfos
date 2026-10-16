/*
SurfCPU firmware start-up
--------------------
File: start.S Date: 10/15/26 (SurfCPU)
--------------------
The first instructions of every firmware image. ROM images (SurfBoot, the BRAM
diagnostics) start here at the reset vector; RAM images (a SURFOS.BIN such as the DDR
diagnostic, built with -DFW_RAM_IMAGE) start here from SurfBoot's handoff with the hart ID
in a0, the boot information in a1 and its magic in a2 (docs/BOOT-ABI.md section 4), which
are passed on to main(hart, bootinfo, magic).

Order: interrupts off, gp, sp, the trap vector (mscratch = FW_TRAP_MAGIC marks firmware
context for trap.S), registers zeroed (their reset value is undefined, ISA.md section 3),
.data copied from its load address when that differs, .bss cleared, then main().
*/
#include "fwdefs.h"

    .section .text.start, "ax"
    .globl _start
    .type _start, @function
_start:
    .option push
    .option norelax
#ifdef FW_RAM_IMAGE
    mv      s0, a0
    mv      s1, a1
    mv      s2, a2
#endif
    csrw    mie, zero
    csrci   mstatus, 8              /* MIE = 0 */
    la      gp, __global_pointer$
    .option pop
    la      sp, __stack_top
    la      t0, trap_entry
    csrw    mtvec, t0
    li      t0, FW_TRAP_MAGIC
    csrw    mscratch, t0

    li      ra, 0
    li      tp, 0
    li      t1, 0
    li      t2, 0
    li      t3, 0
    li      t4, 0
    li      t5, 0
    li      t6, 0
    li      a0, 0
    li      a1, 0
    li      a2, 0
    li      a3, 0
    li      a4, 0
    li      a5, 0
    li      a6, 0
    li      a7, 0
#ifndef FW_RAM_IMAGE
    li      s0, 0
    li      s1, 0
    li      s2, 0
#endif
    li      s3, 0
    li      s4, 0
    li      s5, 0
    li      s6, 0
    li      s7, 0
    li      s8, 0
    li      s9, 0
    li      s10, 0
    li      s11, 0

    /* .data: from its load address in ROM (same address in a RAM image: nothing to do) */
    la      t0, __data_load
    la      t1, __data_start
    la      t2, __data_end
    beq     t0, t1, 2f
1:  bgeu    t1, t2, 2f
    lw      t3, 0(t0)
    sw      t3, 0(t1)
    addi    t0, t0, 4
    addi    t1, t1, 4
    j       1b
2:
    /* .bss */
    la      t0, __bss_start
    la      t1, __bss_end
3:  bgeu    t0, t1, 4f
    sw      zero, 0(t0)
    addi    t0, t0, 4
    j       3b
4:
    li      t0, 0
    li      t1, 0
    li      t2, 0
    li      t3, 0
#ifdef FW_RAM_IMAGE
    mv      a0, s0
    mv      a1, s1
    mv      a2, s2
#endif
    call    main
    /* main returned: stop quietly (main is not meant to return) */
5:  wfi
    j       5b
    .size _start, . - _start
