/*
BLIBC Standard Library
----------------------
File: stdlib.h  Date: Prior to 4/23/04
----------------------
(C)2004 Brandon Burr
*/
//Rebuilt 9/28/2026 - not in the printout, see REBUILD-NOTES.md

#ifndef _STDLIB_H
#define _STDLIB_H

#include <surfos/types.h>

void itoa (char *buf, u_int base, u_int d); //lives in kernel/console.c

#endif
