/*
BLIBC String Functions
----------------------
File: string.h  Date: Prior to 4/23/04
----------------------
(C)2004 Brandon Burr
*/
//Rebuilt 9/28/2026 - not in the printout, see REBUILD-NOTES.md
//Standard semantics since 10/2026 (roadmap K5): the 2004 strcmp compared lengths first
//and strncpy always wrote a terminator at dst[n].

#ifndef _STRING_H
#define _STRING_H

#include <surfos/types.h>

size_t strlen(const char *s);
size_t strnlen(const char *s, size_t maxlen);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
size_t strlcpy(char *dst, const char *src, size_t size);
char *strcat(char *dst, const char *src);
size_t strlcat(char *dst, const char *src, size_t size);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
char *strtok_r(char *s, const char *delim, char **save);

void *memcpy(void *dest, const void *src, size_t count);
void *memmove(void *dest, const void *src, size_t count);
void *memset(void *dest, int val, size_t count);
int memcmp(const void *a, const void *b, size_t count);
void *memchr(const void *s, int c, size_t count);

#endif
