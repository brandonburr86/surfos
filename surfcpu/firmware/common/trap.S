/*
SurfCPU firmware trap entry
--------------------
File: trap.S Date: 10/15/26 (SurfCPU)
--------------------
mtvec points here (direct mode, docs/ISA.md section 3). Two paths:

* Firmware context (mscratch == FW_TRAP_MAGIC): switch to the trap stack, save x1..x31 and
  mepc/mcause/mtval/mstatus in a struct trap_frame, call fw_trap_dispatch() (trap.c), which
  may change the frame's mepc, then restore everything and MRET. While the handler runs
  mscratch is 0, so a trap inside the handler takes the second path instead of looping.

* Any other mscratch (a kernel that SurfBoot handed over to, before it installs its own
  mtvec, or a trap inside the trap handler): the stackless reporter. It touches no RAM and
  no stack, prints mcause, mepc, mtval and every register except t0..t6 (which it needs)
  on the UART, then blinks LED code 8 forever. This is the "fault reporter in ROM" of the
  boot ABI's handoff (docs/BOOT-ABI.md section 4).

Assembled with relaxation off: nothing here may depend on gp being sane.
*/
#include "surfcpu/soc.h"
#include "fwdefs.h"

    .option norelax

    .section .text.trap, "ax"
    .balign 4
    .globl trap_entry
    .type trap_entry, @function
trap_entry:
    csrrw   sp, mscratch, sp
    addi    sp, sp, -FW_TRAP_MAGIC  /* zero only in firmware context */
    bnez    sp, trap_foreign
    la      sp, __trap_stack_top
    addi    sp, sp, -FW_FRAME_SIZE
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
    csrr    t0, mscratch            /* sp at the trap */
    sw      t0, 8(sp)
    csrw    mscratch, zero
    csrr    t0, mepc
    sw      t0, FW_FRAME_MEPC(sp)
    csrr    t0, mcause
    sw      t0, FW_FRAME_MCAUSE(sp)
    csrr    t0, mtval
    sw      t0, FW_FRAME_MTVAL(sp)
    csrr    t0, mstatus
    sw      t0, FW_FRAME_MSTATUS(sp)
    la      gp, __global_pointer$
    mv      a0, sp
    call    fw_trap_dispatch
    lw      t0, FW_FRAME_MEPC(sp)
    csrw    mepc, t0
    li      t0, FW_TRAP_MAGIC
    csrw    mscratch, t0
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
    lw      sp, 8(sp)
    mret
    .size trap_entry, . - trap_entry

/* ---- the stackless reporter: only t0..t6 are used; t6 is the link register ---- */

#define PUTS(label)   la t5, label; jal t6, f_puts
#define HEX(reg)      mv t5, reg; jal t6, f_hex
#define REGOUT(label, reg) PUTS(label); HEX(reg)

trap_foreign:
    PUTS(m_head)
    PUTS(m_mcause)
    csrr    t5, mcause
    jal     t6, f_hex
    PUTS(m_mepc)
    csrr    t5, mepc
    jal     t6, f_hex
    PUTS(m_mtval)
    csrr    t5, mtval
    jal     t6, f_hex
    PUTS(m_sp)
    csrr    t5, mscratch            /* the sp of the code that trapped */
    jal     t6, f_hex
    REGOUT(m_ra, ra)
    REGOUT(m_gp, gp)
    REGOUT(m_tp, tp)
    REGOUT(m_s0, s0)
    REGOUT(m_s1, s1)
    REGOUT(m_a0, a0)
    REGOUT(m_a1, a1)
    REGOUT(m_a2, a2)
    REGOUT(m_a3, a3)
    REGOUT(m_a4, a4)
    REGOUT(m_a5, a5)
    REGOUT(m_a6, a6)
    REGOUT(m_a7, a7)
    REGOUT(m_s2, s2)
    REGOUT(m_s3, s3)
    REGOUT(m_s4, s4)
    REGOUT(m_s5, s5)
    REGOUT(m_s6, s6)
    REGOUT(m_s7, s7)
    REGOUT(m_s8, s8)
    REGOUT(m_s9, s9)
    REGOUT(m_s10, s10)
    REGOUT(m_s11, s11)
    PUTS(m_tail)
    /* blink LED code 8 (LD7) with LD0 red, about 2 Hz at 25 MHz, forever */
    li      t0, SURF_GPIO_BASE
1:  li      t1, LED_CODE_TRAP
    sw      t1, SURF_GPIO_LED(t0)
    li      t1, 1
    sw      t1, SURF_GPIO_RGB(t0)
    li      t2, 2000000
2:  addi    t2, t2, -1
    bnez    t2, 2b
    sw      zero, SURF_GPIO_LED(t0)
    sw      zero, SURF_GPIO_RGB(t0)
    li      t2, 2000000
3:  addi    t2, t2, -1
    bnez    t2, 3b
    j       1b

/* f_puts: t5 = NUL-terminated string; returns through t6. Uses t0..t3. A byte that cannot
 * be sent within about 200000 polls (a stuck UART) is dropped rather than waited for. */
f_puts:
    li      t1, SURF_UART_BASE
1:  lbu     t0, 0(t5)
    beqz    t0, 4f
    li      t2, 200000
2:  lw      t3, SURF_UART_TXDATA(t1)
    bgez    t3, 3f                  /* bit 31 (FULL) clear: room in the TX FIFO */
    addi    t2, t2, -1
    bnez    t2, 2b
    j       5f
3:  sw      t0, SURF_UART_TXDATA(t1)
5:  addi    t5, t5, 1
    j       1b
4:  jr      t6

/* f_hex: t5 = value, printed as 8 hex digits; returns through t6. Uses t0..t4. */
f_hex:
    li      t1, SURF_UART_BASE
    li      t4, 8
1:  srli    t0, t5, 28
    li      t3, 10
    blt     t0, t3, 2f
    addi    t0, t0, 'a' - '0' - 10
2:  addi    t0, t0, '0'
    li      t2, 200000
3:  lw      t3, SURF_UART_TXDATA(t1)
    bgez    t3, 4f
    addi    t2, t2, -1
    bnez    t2, 3b
    j       5f
4:  sw      t0, SURF_UART_TXDATA(t1)
5:  slli    t5, t5, 4
    addi    t4, t4, -1
    bnez    t4, 1b
    jr      t6

    .section .rodata.trap, "a"
m_head:   .asciz "\r\n*** SurfBoot fault reporter: trap with no handler (after the handoff, or inside a trap handler)\r\n"
m_mcause: .asciz "mcause "
m_mepc:   .asciz "  mepc "
m_mtval:  .asciz "  mtval "
m_sp:     .asciz "  sp "
m_ra:     .asciz "\r\nra "
m_gp:     .asciz "  gp "
m_tp:     .asciz "  tp "
m_s0:     .asciz "  s0 "
m_s1:     .asciz "  s1 "
m_a0:     .asciz "\r\na0 "
m_a1:     .asciz "  a1 "
m_a2:     .asciz "  a2 "
m_a3:     .asciz "  a3 "
m_a4:     .asciz "\r\na4 "
m_a5:     .asciz "  a5 "
m_a6:     .asciz "  a6 "
m_a7:     .asciz "  a7 "
m_s2:     .asciz "\r\ns2 "
m_s3:     .asciz "  s3 "
m_s4:     .asciz "  s4 "
m_s5:     .asciz "  s5 "
m_s6:     .asciz "\r\ns6 "
m_s7:     .asciz "  s7 "
m_s8:     .asciz "  s8 "
m_s9:     .asciz "  s9 "
m_s10:    .asciz "\r\ns10 "
m_s11:    .asciz "  s11 "
m_tail:   .asciz "\r\n(t0..t6 were used by this report) stopped; LEDs blink code 8\r\n"

    .section .bss.trapstack, "aw", @nobits
    .balign 16
    .globl __trap_stack_bottom
__trap_stack_bottom:
    .space 1536
    .globl __trap_stack_top
__trap_stack_top:
