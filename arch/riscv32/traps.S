/*
SurfOS riscv32 trap entry and exit
--------------------
File: traps.S   Date: 10/15/26 (SurfCPU)
--------------------
mtvec points here (direct mode) for every trap: exceptions, the machine timer and
external interrupts, and ECALL, which is yield(). The kernel only ever runs in
machine mode, so a trap arrives on the stack of whatever was running: a task's
kernel stack, or the boot stack of the idle task.

Entry: a struct trapframe (include/arch/riscv32/surfos/trap.h) below the interrupted
sp, aligned down to 16 bytes whatever sp was (mscratch holds t0 for the two
instructions that need a register), with all 31 registers, the interrupted sp,
mepc, mstatus, mcause and mtval. gp is reloaded before C runs, so a trap works
even if the interrupted code had lost it. trap_dispatch(frame) returns the frame to
resume: the same, or another task's.

Exit: mepc and mstatus from the frame (its MIE is 0: interrupts stay off until MRET,
which takes MIE from MPIE), every register, sp last, MRET. A new task's first frame
(arch_task_frame()) is built so that this exit starts it.
*/

#include <surfos/trap.h>
#include <riscv/csr.h>

        .section .text.trap, "ax"
        .balign 4
        .globl  trap_entry
trap_entry:
        csrw    mscratch, t0
        addi    t0, sp, -TF_SIZE
        andi    t0, t0, -16
        sw      sp, TF_SP(t0)
        mv      sp, t0
        csrr    t0, mscratch
        sw      zero, 0(sp)
        sw      x1, 4(sp)
        sw      x3, 12(sp)
        sw      x4, 16(sp)
        sw      x5, 20(sp)
        sw      x6, 24(sp)
        sw      x7, 28(sp)
        sw      x8, 32(sp)
        sw      x9, 36(sp)
        sw      x10, 40(sp)
        sw      x11, 44(sp)
        sw      x12, 48(sp)
        sw      x13, 52(sp)
        sw      x14, 56(sp)
        sw      x15, 60(sp)
        sw      x16, 64(sp)
        sw      x17, 68(sp)
        sw      x18, 72(sp)
        sw      x19, 76(sp)
        sw      x20, 80(sp)
        sw      x21, 84(sp)
        sw      x22, 88(sp)
        sw      x23, 92(sp)
        sw      x24, 96(sp)
        sw      x25, 100(sp)
        sw      x26, 104(sp)
        sw      x27, 108(sp)
        sw      x28, 112(sp)
        sw      x29, 116(sp)
        sw      x30, 120(sp)
        sw      x31, 124(sp)
        csrr    t0, mepc
        sw      t0, TF_MEPC(sp)
        csrr    t0, mstatus
        sw      t0, TF_MSTATUS(sp)
        csrr    t0, mcause
        sw      t0, TF_MCAUSE(sp)
        csrr    t0, mtval
        sw      t0, TF_MTVAL(sp)
        .option push
        .option norelax
        la      gp, __global_pointer$
        .option pop
        mv      a0, sp
        call    trap_dispatch
        mv      sp, a0

        .globl  trap_exit
trap_exit:
        lw      t0, TF_MEPC(sp)
        csrw    mepc, t0
        lw      t0, TF_MSTATUS(sp)
        csrw    mstatus, t0
        lw      x1, 4(sp)
        lw      x3, 12(sp)
        lw      x4, 16(sp)
        lw      x5, 20(sp)
        lw      x6, 24(sp)
        lw      x7, 28(sp)
        lw      x8, 32(sp)
        lw      x9, 36(sp)
        lw      x10, 40(sp)
        lw      x11, 44(sp)
        lw      x12, 48(sp)
        lw      x13, 52(sp)
        lw      x14, 56(sp)
        lw      x15, 60(sp)
        lw      x16, 64(sp)
        lw      x17, 68(sp)
        lw      x18, 72(sp)
        lw      x19, 76(sp)
        lw      x20, 80(sp)
        lw      x21, 84(sp)
        lw      x22, 88(sp)
        lw      x23, 92(sp)
        lw      x24, 96(sp)
        lw      x25, 100(sp)
        lw      x26, 104(sp)
        lw      x27, 108(sp)
        lw      x28, 112(sp)
        lw      x29, 116(sp)
        lw      x30, 120(sp)
        lw      x31, 124(sp)
        lw      sp, TF_SP(sp)
        mret
        .globl  trap_end
trap_end:
