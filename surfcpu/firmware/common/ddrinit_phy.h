/*
SurfCPU: what the DDR initialization needs from LiteDRAM's generator
--------------------
File: ddrinit_phy.h Date: 10/16/26 (SurfCPU)
--------------------
The values LiteDRAM's generator writes into software/include/generated/sdram_phy.h for
ip/litedram/arty.yml (the Arty's MT41K128M16 behind the A7DDRPHY, 4 phases): the PHY's
geometry, its read and write phases, and the mode-register values of the generated
init_sequence(). ddrinit.c uses them; ip/litedram/inspect_core.py (make litedram-gen and
make litedram-check) compares every one with the generated header, so a change of the
generator's configuration that moves any of them fails the check until this file follows.
*/
#ifndef SURFCPU_DDRINIT_PHY_H
#define SURFCPU_DDRINIT_PHY_H

#define DDRINIT_PHASES        4     /* SDRAM_PHY_PHASES: DFI phases per controller cycle */
#define DDRINIT_MODULES       2     /* SDRAM_PHY_MODULES: byte lanes (DQ7:0, DQ15:8) */
#define DDRINIT_DELAYS        32    /* SDRAM_PHY_DELAYS: input delay taps per lane */
#define DDRINIT_BITSLIPS      8     /* SDRAM_PHY_BITSLIPS: read bitslip steps per lane */
#define DDRINIT_DFI_DATABITS  32    /* SDRAM_PHY_DFI_DATABITS: data bits per phase */
#define DDRINIT_XDR           2     /* SDRAM_PHY_XDR: two data edges per DFI word */
#define DDRINIT_DQ_DQS_RATIO  8     /* SDRAM_PHY_DQ_DQS_RATIO: DQ lines per lane */
#define DDRINIT_RDPHASE       3     /* SDRAM_PHY_RDPHASE: the phase reads are injected on */
#define DDRINIT_WRPHASE       3     /* SDRAM_PHY_WRPHASE: the phase writes are injected on */
#define DDRINIT_CL            6     /* SDRAM_PHY_CL (in MR0) */
#define DDRINIT_CWL           5     /* SDRAM_PHY_CWL (in MR2) */

/* init_sequence(): the mode registers in the order it loads them, then ZQ calibration */
#define DDRINIT_MR2           0x200 /* CWL=5, Rtt_WR 60 ohm */
#define DDRINIT_MR3           0x0
#define DDRINIT_MR1           0x6   /* Rtt_nom 60 ohm, output drive 34 ohm, DLL on */
#define DDRINIT_MR0           0x920 /* CL=6, BL=8, DLL reset, write recovery */
#define DDRINIT_ZQCL          0x400 /* A10 high: ZQ calibration long */

#endif /* SURFCPU_DDRINIT_PHY_H */
