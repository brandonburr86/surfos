/*
BLIBC String Functions
----------------------
File: string.h  Date: Prior to 4/23/04
----------------------
(C)2004 Brandon Burr
*/
//Rebuilt 9/28/2026 - not in the printout, see REBUILD-NOTES.md

#ifndef _STRING_H
#define _STRING_H

#include <surfos/types.h>

u_int strlen(const char str[]);
int strcmp(const char str1[], const char str2[]);
char *strncpy(char *str1, const char *str2, int size);
char *strcpy(char *str1, const char *str2);
char *strchr(const char *str, int ch);

#endif
