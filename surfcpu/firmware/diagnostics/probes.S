/*
SurfCPU diagnostics: probes that trap on purpose
--------------------
File: probes.S Date: 10/15/26 (SurfCPU)
--------------------
Each probe causes exactly one trap of a known kind at a known address; the diagnostics'
trap hook checks mcause, mtval and mepc and steps over it (or, for a fetch fault, resumes
at ra). The encodings SurfCPU must refuse are written as .word, which the assembler marks
as data, so tools/isa-audit.py does not count them as instructions. A probe that takes a
sentinel returns it unchanged when the faulting instruction wrote no register, as
docs/ISA.md requires.

ddr_probe_start..ddr_probe_end is a position-independent leaf function (x * 3 + 0x234)
that the diagnostics copy into DDR and scratch RAM and run there after FENCE.I.
*/
    .section .text.probes, "ax"
    .balign 4

    .globl probe_illegal_zero, probe_illegal_zero_at
probe_illegal_zero:
probe_illegal_zero_at:
    .word   0x00000000              /* all zeros: illegal */
    ret

    .globl probe_illegal_ones
probe_illegal_ones:
    .word   0xffffffff              /* all ones: illegal */
    ret

    .globl probe_csr_write_ro
probe_csr_write_ro:
    .word   0xf1401073              /* csrw mhartid, zero: write to a read-only CSR */
    ret

    .globl probe_csr_unknown
probe_csr_unknown:
    .word   0x18002573              /* csrr a0, satp: no such CSR here; a0 keeps the sentinel */
    ret

    .globl probe_slli_shamt5
probe_slli_shamt5:
    .word   0x02051513              /* slli a0, a0, 32: imm[5] = 1 is illegal on RV32 */
    ret

    .globl probe_sret
probe_sret:
    .word   0x10200073              /* sret: no S-mode */
    ret

    .globl probe_ecall
probe_ecall:
    ecall
    ret

    .globl probe_ebreak, probe_ebreak_at
probe_ebreak:
probe_ebreak_at:
    ebreak
    ret

    .globl probe_load_word
probe_load_word:                    /* (addr, sentinel) -> the loaded word, or the sentinel */
    lw      a1, 0(a0)
    mv      a0, a1
    ret

    .globl probe_load_half
probe_load_half:
    lh      a1, 0(a0)
    mv      a0, a1
    ret

    .globl probe_load_byte
probe_load_byte:
    lb      a1, 0(a0)
    mv      a0, a1
    ret

    .globl probe_store_word
probe_store_word:                   /* (addr, value) */
    sw      a1, 0(a0)
    ret

    .globl probe_store_half
probe_store_half:
    sh      a1, 0(a0)
    ret

    .globl probe_jump
probe_jump:                         /* (addr): the fetch at addr faults; the hook resumes at ra */
    addi    sp, sp, -16
    sw      ra, 12(sp)
    jalr    ra, 0(a0)
    lw      ra, 12(sp)
    addi    sp, sp, 16
    ret

    .globl probe_misaligned_jump, probe_misaligned_jump_at, probe_misaligned_target
probe_misaligned_jump:              /* (sentinel): the JALR traps itself and must not write t1 */
    la      t0, probe_misaligned_target
    mv      t1, a0
probe_misaligned_jump_at:
    jalr    t1, 2(t0)
    mv      a0, t1
    ret
probe_misaligned_target:
    nop
    nop
    ret

    .globl probe_misaligned_branch, probe_misaligned_branch_at
probe_misaligned_branch:
probe_misaligned_branch_at:
    .word   0x00000363              /* beq zero, zero, .+6: a taken branch to a 2-aligned target */
    ret

    .balign 4
    .globl ddr_probe_start, ddr_probe_end, ddr_probe_imm
ddr_probe_start:
    slli    a1, a0, 1
    add     a0, a0, a1
ddr_probe_imm:
    addi    a0, a0, 0x234
    ret
ddr_probe_end:
