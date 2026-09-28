/*
SurfOS Kernel
----------------------
File: kernel.h  Date: Prior to 4/23/04
----------------------
(C)2004 Brandon Burr
*/

#ifndef _SURFOS_KERNEL_H
#define _SURFOS_KERNEL_H

//#define BOCHS //Compile for bochs?

void reboot(void);

/* Initialization Functions */
void init_console(void);
void init_interrupt(void);
/*********************/

void do_banner(void);
void sysbeep(u_long frequency, u_long duration);

#endif
