/*
BLIBC Standard I/O
----------------------
File: stdio.h   Date: Prior to 4/23/04
----------------------
(C)2004 Brandon Burr
*/
//Rebuilt 9/28/2026 - not in the printout, see REBUILD-NOTES.md

#ifndef _STDIO_H
#define _STDIO_H

#include <surfos/types.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

void printf(const char *format, ...);

u_char getc(); //reads the key buffer, doesn't wait

void puts(char *s);
void cputs(u_char atr, char *str);
char *gets(char *str);
char *cgets(int color, char *str);

#endif
