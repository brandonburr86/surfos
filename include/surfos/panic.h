/*
SurfOS Exception Prototypes
---------------------------
File: template.h    Date: 4/23/04
---------------------------
(C)2004 Brandon Burr
*/

#ifndef _SURFOS_PANIC_H
#define _SURFOS_PANIC_H

#include <surfos/task.h>

void exDivZero(surf_regs *curReg);
void exDebug(surf_regs *curReg);
void exNMI(surf_regs *curReg);
void exBreakpoint(surf_regs *curReg);
void exOverflow(surf_regs *curReg);
void exBoundRange(surf_regs *curReg);
void exInvalOpcode(surf_regs *curReg);
void exDevNotAvailable(surf_regs *curReg);
void exDoubleFault(surf_regs *curReg);
void exCoprocSeg(surf_regs *curReg);
void exInvalTSS(surf_regs *curReg);
void exSegNotPresent(surf_regs *curReg);
void exStackFault(surf_regs *curReg);
void exGPF(surf_regs *curReg);
void exFPError(surf_regs *curReg);
void exAlignCheck(surf_regs *curReg);
void exMachineCheck(surf_regs *curReg);
void exSIMDFP(surf_regs *curReg);

/***** Assembly ISR Stubs ****/
void ex0();
void ex1();
void ex2();
void ex3();
void ex4();
void ex5();
void ex6();
void ex7();
void ex8();
void ex9();
void ex10();
void ex11();
void ex12();
void ex13();
void ex14();
void ex15();
void ex16();
void ex17();
void ex18();
/*************************/

void init_exceptions();
void panic(surf_task *task,char *msg);
void BUG();
void dump_regs();
#endif
