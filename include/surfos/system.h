/*
SurfOS System Header
----------------------
File: system.h  Date: Prior to 4/23/04
----------------------
(C)2004 Brandon Burr
*/

#ifndef _SURFOS_SYSTEM_H
#define _SURFOS_SYSTEM_H

#include <surfos/types.h>

void halt();
void sleep(u_long usec);
void BUG();
void reboot();
void reset(void);   /* 10/25/26: the machine reset at once (kernel/sys.c): reboot -f, and the shutdown's last step */

#endif
