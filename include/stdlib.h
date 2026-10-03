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

long strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
int atoi(const char *s);
int abs(int v);

#define RAND_MAX 0x7fffffff
int rand(void);
void srand(u_int seed);

#endif
