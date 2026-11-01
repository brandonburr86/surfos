/*
SurfCPU configuration for riscv-arch-test 4.x (ACT4)
--------------------
File: rvtest_config.h Date: 10/15/26 (SurfCPU)
--------------------
What ACT4's framework would generate with `udb-gen cfg-c-header` from surfcpu.yaml (next to
this file), written by hand because the framework needs Ruby and the UDB gem, which the
toolchain container does not have. Every macro the ACT4 environment (tests/env) and the
tests in scope read is here; tools/rvarch.py fails a build that uses one it does not know.
docs/ISA.md is the source: RV32I 2.1, M 2.0 (and so Zmmul), Zicsr, Zifencei, the Zicntr
counters (cycle, time, instret), hpmcounters hardwired to zero, Machine ISA 1.13, M-mode
only (no S, no U, no H), direct mtvec only, no PMP entries.
*/
#ifndef SURFCPU_RVTEST_CONFIG_H
#define SURFCPU_RVTEST_CONFIG_H

#define UDB_MXLEN 32

#define I_SUPPORTED
#define M_SUPPORTED
#define ZMMUL_SUPPORTED
#define ZICSR_SUPPORTED
#define ZIFENCEI_SUPPORTED
#define ZICNTR_SUPPORTED
#define ZIHPM_SUPPORTED
#define ZIHINTPAUSE_SUPPORTED
#define SM_SUPPORTED
#define SM1P13P0_SUPPORTED

#define UDB_MTVEC_MODES_0
#define UDB_MTVEC_BASE_ALIGNMENT_DIRECT 4
#define UDB_TIME_CSR_IMPLEMENTED
#define UDB_NUM_PMP_ENTRIES 0
#define UDB_NUM_USABLE_PMP_ENTRIES 0

#endif
