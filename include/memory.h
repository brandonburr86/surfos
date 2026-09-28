/*
BLIBC Memory Functions
----------------------
File: memory.h  Date: Prior to 4/23/04
----------------------
(C)2004 Brandon Burr
*/
//Rebuilt 9/28/2026 - not in the printout, see REBUILD-NOTES.md

#ifndef _MEMORY_H
#define _MEMORY_H

#include <surfos/types.h>

void *memcpy(void *dest, const void *src, size_t count);
void *memset(void *dest, int val, size_t count);

#endif
