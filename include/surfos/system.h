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

#endif
