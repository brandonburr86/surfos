/*
SurfBoot: the jump to the kernel
--------------------
File: handoff.S Date: 10/15/26 (SurfCPU)
--------------------
surfboot_handoff(entry, bootinfo): the last instructions SurfBoot runs, giving the state of
docs/BOOT-ABI.md section 4. main.c has already cleared intc.ENABLE, set mtimecmp to all
ones, released the card's chip select and drained the UART. Here: interrupts off
(mie = 0, mstatus.MIE = 0), mtvec = SurfBoot's fault reporter in ROM with mscratch = 0 so
that a trap before the kernel installs its own handler gets the stackless report, a FENCE
so every store (payload, boot information) is complete, FENCE.I so instruction fetch sees
the loaded payload, then a0 = hart ID, a1 = boot information, a2 = its magic.
*/
#include "surfcpu/soc.h"

    .option norelax
    .section .text.surfboot_handoff, "ax"
    .globl surfboot_handoff
    .type surfboot_handoff, @function
surfboot_handoff:
    csrw    mie, zero
    csrci   mstatus, 8
    la      t0, trap_entry
    csrw    mtvec, t0
    csrw    mscratch, zero
    fence   rw, rw
    fence.i
    mv      t1, a0
    li      a0, SURF_HART_ID
    li      a2, SURF_BOOTINFO_MAGIC
    li      ra, 0
    jr      t1
    .size surfboot_handoff, . - surfboot_handoff
