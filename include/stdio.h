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
#include <stdarg.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int vprintf(const char *format, va_list ap);
int printf(const char *format, ...);

u_char getc(); //reads the key buffer, doesn't wait

void puts(char *s);
void cputs(u_char atr, char *str);
char *gets(char *str);
char *cgets(int color, char *str);

#endif
