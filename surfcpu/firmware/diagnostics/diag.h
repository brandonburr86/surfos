/*
SurfCPU diagnostics: shared declarations
--------------------
File: diag.h Date: 10/15/26 (SurfCPU)
--------------------
The milestone 3 BRAM program (bramdiag.c, in the boot ROM) and the DDR diagnostic
(ddrdiag.c, a SURFOS.BIN that SurfBoot loads) share the result bookkeeping (report.c), the
CPU self-checks (cputest.c), the DDR tests (ddrtest.c) and the hand-written probes that
trap on purpose (probes.S).
*/
#ifndef SURFCPU_DIAG_H
#define SURFCPU_DIAG_H

#include <stdint.h>
#include "fw.h"

#define DIAG_VERSION "1.0.0"

/* ---- results (report.c) ---- */
extern uint32_t diag_checks, diag_failures;
void diag_section(const char *name);
/* Counts one check; prints a FAIL line (with the formatted detail) when ok is 0. */
int diag_check(int ok, const char *fmt, ...) FMT_PRINTF(2, 3);
void diag_section_end(void);
void diag_summary(const char *program);
void diag_guard(void (*fn)(uint32_t), uint32_t arg);   /* run a section; survive a stray trap */

/* ---- CPU self-checks (cputest.c) ---- */
void cpu_test_alu(void);
void cpu_test_muldiv(void);
void cpu_test_branches(void);
void cpu_test_loadstore(uint32_t scratch_addr);
void cpu_test_csrs(void);

/* ---- DDR (ddrtest.c) ---- */
struct ddr_area {
    uint32_t ram_base, ram_size;    /* the whole DDR */
    uint32_t lo, hi;                /* the part these tests may overwrite */
};
int ddr_wait_calibration(uint32_t timeout_us);      /* 1 = calibrated */
void ddr_test_data_lines(const struct ddr_area *a);
void ddr_test_byte_lanes(const struct ddr_area *a);
void ddr_test_address_lines(const struct ddr_area *a);
void ddr_test_boundaries(const struct ddr_area *a);
void ddr_test_patterns(const struct ddr_area *a, uint32_t full_bytes, int sampled);
void ddr_test_copies(const struct ddr_area *a, uint32_t bytes, uint32_t loops);
void ddr_test_exec(const struct ddr_area *a);

/* ---- probes (probes.S): each makes exactly one trap of its kind and returns ---- */
uint32_t probe_illegal_zero(uint32_t sentinel);     /* .word 0 */
uint32_t probe_illegal_ones(uint32_t sentinel);     /* .word 0xffffffff */
uint32_t probe_csr_write_ro(uint32_t sentinel);     /* csrw mhartid */
uint32_t probe_csr_unknown(uint32_t sentinel);      /* csrr satp */
uint32_t probe_slli_shamt5(uint32_t sentinel);      /* slli with imm[5] = 1 */
uint32_t probe_sret(uint32_t sentinel);             /* SRET */
void probe_ecall(void);
void probe_ebreak(void);
extern char probe_ebreak_at[], probe_illegal_zero_at[], probe_misaligned_jump_at[], probe_misaligned_branch_at[];
extern char probe_misaligned_target[];
uint32_t probe_load_word(uint32_t addr, uint32_t sentinel);
uint32_t probe_load_half(uint32_t addr, uint32_t sentinel);
uint32_t probe_load_byte(uint32_t addr, uint32_t sentinel);
void probe_store_word(uint32_t addr, uint32_t value);
void probe_store_half(uint32_t addr, uint32_t value);
void probe_jump(uint32_t addr);                     /* jalr to addr; a fetch fault comes back via ra */
uint32_t probe_misaligned_jump(uint32_t sentinel);  /* jalr to a 2-aligned target: rd must stay */
void probe_misaligned_branch(void);                 /* a taken branch to pc + 6 */
extern char ddr_probe_start[], ddr_probe_end[], ddr_probe_imm[];   /* position-independent: x * 3 + 0x234 */

#endif /* SURFCPU_DIAG_H */
