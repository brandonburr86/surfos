/*
SurfCPU model macros for riscv-arch-test 4.x (ACT4)
--------------------
File: rvmodel_macros.h Date: 10/15/26 (SurfCPU)
--------------------
The DUT-specific operations ACT4 asks for, for build/cpu/surfcpu_tb (docs/MODULES.md
section 6) and for Spike, which run the same ELF: the run ends by storing to tohost (1 =
pass, 3 = fail); there is no console (the testbench's console register is listed in
docs/CPU.md, not known yet), so strings are not printed; 0x00000000 is unmapped on both
(load, store and fetch access faults); the CLINT-compatible timer is at 0x02000000 on both
(msip +0, mtimecmp +0x4000, mtime +0xBFF8). The external interrupt line is the testbench's
(docs/CPU.md): left empty here, so InterruptsSm stays out of scope until it is known.
In signature builds ACT4's sail_macros.h replaces the halt and interrupt macros with its
own, which use the same tohost protocol.
*/
#ifndef _RVMODEL_MACROS_H
#define _RVMODEL_MACROS_H

// SurfCPU has the standard machine mode (CSRs, traps, MRET), so ACT4 installs its trap handler.
#define STANDARD_SM_SUPPORTED

#define RVMODEL_DATA_SECTION \
        .pushsection .tohost,"aw",@progbits;                \
        .balign 8; .global tohost; tohost: .dword 0;         \
        .balign 8; .global fromhost; fromhost: .dword 0;     \
        .popsection

#define RVMODEL_HALT_PASS  \
  li x1, 1                ;\
  la t0, tohost           ;\
  write_tohost_pass:      ;\
    sw x1, 0(t0)          ;\
    sw x0, 4(t0)          ;\
    j write_tohost_pass   ;\

#define RVMODEL_HALT_FAIL \
  li x1, 3                ;\
  la t0, tohost           ;\
  write_tohost_fail:      ;\
    sw x1, 0(t0)          ;\
    sw x0, 4(t0)          ;\
    j write_tohost_fail   ;\

#define RVMODEL_IO_WRITE_STR(_R1, _R2, _R3, _STR_PTR)

#define RVMODEL_ACCESS_FAULT_ADDRESS 0x00000000

#define RVMODEL_MTIME_ADDRESS     0x0200BFF8
#define RVMODEL_MTIMECMP_ADDRESS  0x02004000
#define RVMODEL_INTERRUPT_LATENCY 10
#define RVMODEL_TIMER_INT_SOON_DELAY 100
#define RVMODEL_MAX_CYCLES_PER_TIMER_TICK 100

#define RVMODEL_SET_MEXT_INT(_R1, _R2)
#define RVMODEL_CLR_MEXT_INT(_R1, _R2)
#define RVMODEL_SET_MSW_INT(_R1, _R2) \
  li _R1, 1; \
  li _R2, 0x02000000; \
  sw _R1, 0(_R2);
#define RVMODEL_CLR_MSW_INT(_R1, _R2) \
  li _R2, 0x02000000; \
  sw zero, 0(_R2);
#define RVMODEL_SET_SEXT_INT(_R1, _R2)
#define RVMODEL_CLR_SEXT_INT(_R1, _R2)
#define RVMODEL_SET_SSW_INT(_R1, _R2)
#define RVMODEL_CLR_SSW_INT(_R1, _R2)

#endif
