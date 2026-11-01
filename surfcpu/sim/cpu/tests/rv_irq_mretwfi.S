/*
SurfCPU directed test: MRET into a WFI with an interrupt pending (review)
--------------------
File: rv_irq_mretwfi.S Date: 10/15/26 (SurfCPU)
--------------------
Reviewer test: rv_irq_wfi.S without the csrsi/WFI case, so the MRET-to-WFI case (tests 7..9) is checked on its own. */
#define SKIP_W1
#include "rv_irq_wfi.S"
