/*
SurfCPU firmware: constants shared by C and assembly
--------------------
File: fwdefs.h Date: 10/15/26 (SurfCPU)
--------------------
Plain #defines only, so start.S and trap.S can include it as well as the C files.
*/
#ifndef SURFCPU_FWDEFS_H
#define SURFCPU_FWDEFS_H

/* mscratch while firmware owns the machine: odd, so never a valid stack pointer, and
 * small enough for one ADDI (trap.S compares without a spare register). */
#define FW_TRAP_MAGIC 0x5A1

/* Trap frame layout (struct trap_frame in fw.h): x0..x31 at 4*n, then the CSRs. */
#define FW_FRAME_MEPC     128
#define FW_FRAME_MCAUSE   132
#define FW_FRAME_MTVAL    136
#define FW_FRAME_MSTATUS  140
#define FW_FRAME_SIZE     144

/* Failure codes shown on the green LEDs LD4..LD7 (docs/FIRMWARE.md). */
#define LED_CODE_DDR       1
#define LED_CODE_NO_CARD   2
#define LED_CODE_SD        3
#define LED_CODE_PARTITION 4
#define LED_CODE_FS        5
#define LED_CODE_NOT_FOUND 6
#define LED_CODE_IMAGE     7
#define LED_CODE_TRAP      8
#define LED_CODE_MONITOR   9

#endif /* SURFCPU_FWDEFS_H */
