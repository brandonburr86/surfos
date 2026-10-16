/*
SurfCPU reference test: CSR instructions and counters
--------------------
File: csr.S Date: 10/15/26 (SurfCPU)
--------------------
All six CSR instructions on mscratch (old value returned, set and clear masks, the
immediate forms, rd = x0, rs1 = x0); the writable bits of mstatus, mie, mepc, mtval and
mcause (legal values only: WLRL), mtvec written with its own handler address; the fixed
read-only CSRs whose values Spike and the contract share (mvendorid, mhartid, mconfigptr,
misa read, mstatush); minstret written then read (a CSR write to minstret is not counted;
a trapping instruction does not retire; the MRET does), and mcountinhibit.IR stopping it.
Not here, because Spike differs from docs/ISA.md (docs/VERIFICATION.md section 4): marchid,
mimpid, pmp*, tselect/tdata*, writes to misa, mtvec MODE, mcountinhibit bits 3-31.
*/
#include "ref_env.h"

REF_BEGIN
    li a1, 0x12345678
    csrrw a2, mscratch, a1                     /* old: 0 (reset) */
    SAVE(a2, 0)
    li a1, 0x0000ff00
    csrrs a2, mscratch, a1
    SAVE(a2, 1)
    li a1, 0x10000008
    csrrc a2, mscratch, a1
    SAVE(a2, 2)
    csrrwi a2, mscratch, 0x15
    SAVE(a2, 3)
    csrrsi a2, mscratch, 0x0a
    SAVE(a2, 4)
    csrrci a2, mscratch, 0x01
    SAVE(a2, 5)
    csrrs a2, mscratch, x0                     /* read only */
    SAVE(a2, 6)
    csrrw x0, mscratch, a1                     /* write only */
    csrr a2, mscratch
    SAVE(a2, 7)
    /* mstatus: MIE and MPIE writable, MPP fixed at M, the rest zero */
    li a1, 0xffffffff
    csrw mstatus, a1
    csrr a2, mstatus
    SAVE(a2, 8)
    csrw mstatus, zero
    csrr a2, mstatus
    SAVE(a2, 9)
    csrsi mstatus, 8                           /* MIE = 1 with mie = 0: nothing happens */
    ecall                                      /* MPIE <- MIE, MIE <- 0; MRET restores */
    csrr a2, mstatus
    SAVE(a2, 10)
    csrci mstatus, 8
    /* mie: MSIE, MTIE, MEIE (written while mstatus.MIE = 0, then cleared) */
    li a1, 0xffffffff
    csrw mie, a1
    csrr a2, mie
    SAVE(a2, 11)
    csrw mie, zero
    /* mepc: bits 1:0 read as zero; mtval: all bits; mcause: legal values round-trip */
    csrw mepc, a1
    csrr a2, mepc
    SAVE(a2, 12)
    csrw mtval, a1
    csrr a2, mtval
    SAVE(a2, 13)
    li a1, 0x8000000b
    csrw mcause, a1
    csrr a2, mcause
    SAVE(a2, 14)
    li a1, 0x00000006
    csrw mcause, a1
    csrr a2, mcause
    SAVE(a2, 15)
    /* mtvec: its own handler (direct mode) */
    la a1, ref_trap
    csrrw a2, mtvec, a1
    sub a2, a2, a1
    SAVE(a2, 16)
    /* fixed values shared by Spike and the contract */
    csrr a2, mvendorid
    SAVE(a2, 17)
    csrr a2, mhartid
    SAVE(a2, 18)
    csrr a2, mconfigptr
    SAVE(a2, 19)
    csrr a2, misa
    SAVE(a2, 20)
    csrr a2, mstatush
    SAVE(a2, 21)
    csrr a2, mhpmcounter3
    SAVE(a2, 22)
    csrr a2, hpmcounter31h
    SAVE(a2, 23)
    /* minstret: written, then counted */
    csrw minstret, zero                        /* not counted itself */
    nop
    nop
    csrr a2, minstret                          /* 2 */
    SAVE(a2, 24)
    csrw minstret, zero
    ecall                                      /* does not retire; the handler's do */
    csrr a2, minstret
    SAVE(a2, 25)
    csrw minstret, zero
    csrr a3, instret                           /* the user-level alias */
    SAVE(a3, 26)
    /* mcountinhibit.IR stops minstret */
    csrwi mcountinhibit, 4
    csrw minstret, zero
    nop
    nop
    csrr a2, minstret                          /* 0 */
    SAVE(a2, 27)
    csrr a2, mcountinhibit
    SAVE(a2, 28)
    csrwi mcountinhibit, 0
    /* minstret carry into minstreth */
    li a1, 0xfffffffe
    csrw minstret, a1
    csrr a4, minstreth                         /* minstret was 0xfffffffe: no carry yet */
    nop
    csrr a5, minstreth                         /* carried by now */
    sub a5, a5, a4
    SAVE(a5, 29)
    REF_PASS

REF_TRAP_HANDLER
REF_DATA(32, 4)
