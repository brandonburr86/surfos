/*
SurfCPU firmware: string.h
--------------------
File: string.h Date: 10/15/26 (SurfCPU)
--------------------
The firmware has no C library. This header (on the RV32 include path only; host builds use
the system's) declares the functions firmware/common/string.c provides.
*/
#ifndef SURFCPU_STRING_H
#define SURFCPU_STRING_H

#include <stddef.h>

void *memcpy(void *restrict dst, const void *restrict src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);

#endif /* SURFCPU_STRING_H */
