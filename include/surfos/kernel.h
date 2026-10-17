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
void init_autonet(void);   /* 10/9/26: the kernel word autonet: network and telnetd at boot (kernel/autonet.c) */
void init_netman(void);    /* 10/17/26: the settings on the stick and the network they ask for (kernel/netman.c) */
/*********************/

void do_banner(void);
void sysbeep(u_long frequency, u_long duration);

#endif
