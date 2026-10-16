/*
SurfCPU directed test: WFI
--------------------
File: wfi.S Date: 10/15/26 (SurfCPU)
--------------------
WFI waits until (mip & mie) != 0, whatever mstatus.MIE is. With MIE = 0 it then simply
completes (no trap), so code after it sees the timer has passed; with MIE = 1 an interrupt
that comes while the WFI waits is taken with mepc = the instruction after the WFI. An
interrupt already pending and enabled in mie lets WFI complete at once when MIE = 0; when
MIE = 1 (set by MRET just before), it is taken before the WFI, mepc = the WFI (privileged
spec 3.1.9). A pending interrupt that is not enabled in mie does not wake WFI; a later
enabled one does.
*/
// surf-test: spike=no
#define TEST_IRQ_HANDLER
#include "surf_test.h"

#define LOGGED(n) addi x7, x22, 8 * (n); bne x24, x7, fail;

TEST_BEGIN
test_main:
        la      x24, irq_log
        li      x9, TB_BASE
        li      x5, CLINT_MSIP
        li      x6, CLINT_MTIMECMP

        // ---------------------------------------------------------- MIE = 0: WFI just completes
        li      TESTNUM, 1
        mv      x22, x24
        li      x7, MIP_MTIP
        csrw    mie, x7
        li      x7, CLINT_MTIME
        lw      x12, 0(x7)
        addi    x12, x12, 500
        sw      x0, 4(x6)
        sw      x12, 0(x6)                      // the timer fires in 500 cycles
        wfi
        li      x7, CLINT_MTIME                 // and WFI waited for it
        lw      x13, 0(x7)
        bltu    x13, x12, fail
        LOGGED(0)                               // no trap
        csrr    x14, mip
        li      x7, MIP_MTIP
        bne     x14, x7, fail
        li      x11, -1
        sw      x11, 4(x6)
        sw      x11, 0(x6)

        // ---------------------------------------------------------- MIE = 1: taken after the WFI
        li      TESTNUM, 10
        mv      x22, x24
        li      x7, MIP_MEIP
        csrw    mie, x7
        csrsi   mstatus, MSTATUS_MIE
        li      x11, 200
        sw      x11, TB_IRQ_DELAY(x9)           // the external interrupt in 200 cycles
        wfi
wfi_next_10:
        nop
        csrci   mstatus, MSTATUS_MIE
        LOGGED(1)
        lw      x14, 0(x22)
        li      x7, 0x8000000b
        bne     x14, x7, fail
        lw      x14, 4(x22)                     // mepc = the instruction after the WFI
        la      x7, wfi_next_10
        bne     x14, x7, fail

        // ---------------------------------------------------------- already pending
        li      TESTNUM, 20                     // MIE = 0: WFI completes at once
        mv      x22, x24
        li      x7, MIP_MSIP
        csrw    mie, x7
        li      x11, 1
        sw      x11, 0(x5)
        wfi
        LOGGED(0)
        sw      x0, 0(x5)
        li      TESTNUM, 22                     // a CSR write that sets MIE: taken at once
        mv      x22, x24
        csrw    mie, x0
        li      x11, 1
        sw      x11, 0(x5)                      // MSIP pending, not enabled
        li      x7, MIP_MSIP
        csrw    mie, x7                         // enabled, MIE still 0
        csrsi   mstatus, MSTATUS_MIE            // taken right after this csrsi...
        nop
        csrci   mstatus, MSTATUS_MIE
        LOGGED(1)
        li      TESTNUM, 23                     // an interrupt pending and enabled when the WFI
        mv      x22, x24                        // arrives is taken before it (mepc = the WFI);
        li      x7, MIP_MSIP | MIP_MEIP         // one that comes while the WFI waits is taken
        csrw    mie, x7                         // after it (mepc = the WFI + 4)
        li      x11, 1
        sw      x11, 0(x5)                      // MSIP pending and enabled, MIE still 0
        li      x11, 20000                      // the external interrupt long after the
        sw      x11, TB_IRQ_DELAY(x9)           // software one's handler has returned
        la      x7, wfi_23
        csrw    mepc, x7
        li      x7, MSTATUS_MPIE
        csrs    mstatus, x7
        mret                                    // MIE = 1 from the WFI on
wfi_23: wfi
wfi_next_23:
        nop
        csrci   mstatus, MSTATUS_MIE
        LOGGED(2)
        li      TESTNUM, 24                     // first the software interrupt, at the WFI
        lw      x14, 0(x22)
        li      x7, 0x80000003
        bne     x14, x7, fail
        lw      x14, 4(x22)
        la      x7, wfi_23
        bne     x14, x7, fail
        li      TESTNUM, 25                     // then the external one, after the WFI
        lw      x14, 8(x22)
        li      x7, 0x8000000b
        bne     x14, x7, fail
        lw      x14, 12(x22)
        la      x7, wfi_next_23
        bne     x14, x7, fail

        // ---------------------------------------------------------- only enabled ones wake it
        li      TESTNUM, 30
        mv      x22, x24
        li      x11, 1
        sw      x11, 0(x5)                      // MSIP pending, but not enabled
        li      x7, MIP_MEIP
        csrw    mie, x7
        li      x7, CLINT_MTIME
        lw      x12, 0(x7)                      // the time before arming the delay
        li      x11, 300
        sw      x11, TB_IRQ_DELAY(x9)
        wfi                                     // waits for the external one
        lw      x13, 0(x7)
        sub     x13, x13, x12
        li      x7, 300
        bltu    x13, x7, fail
        LOGGED(0)                               // MIE = 0: no trap
        csrr    x14, mip
        li      x7, MIP_MEIP | MIP_MSIP
        bne     x14, x7, fail
        sw      x0, 0(x5)
        sw      x0, TB_IRQ_EXT(x9)
        j       pass

        .balign 4
irq_handler:
        sw      t5, 0(x24)
        sw      t6, 4(x24)
        addi    x24, x24, 8
        li      s10, 0x8000000b
        beq     t5, s10, 1f
        li      s10, 0x80000003
        beq     t5, s10, 2f
        li      s10, 0x80000007
        beq     t5, s10, 3f
        j       fail
1:      li      s10, TB_BASE
        sw      x0, TB_IRQ_EXT(s10)
        mret
2:      li      s10, CLINT_MSIP
        sw      x0, 0(s10)
        mret
3:      li      s10, CLINT_MTIMECMP
        li      x23, -1
        sw      x23, 4(s10)
        sw      x23, 0(s10)
        mret

        .data
        .balign 4
irq_log:
        .space  1024

TEST_DATA
