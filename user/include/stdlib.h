/*
SurfOS user library: memory, numbers, exit
----------------------
File: stdlib.h  Date: 10/5/26
----------------------
malloc() keeps a free list over memory from sbrk(); freed blocks are reused and
merged with free neighbours (user/libsurf/malloc.c). exit() runs the atexit()
functions, flushes stdio and ends the process.
*/

#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 0x7fffffff

void *malloc(size_t n);
void free(void *p);
void *calloc(size_t n, size_t size);
void *realloc(void *p, size_t n);

void exit(int code) __attribute__((noreturn));
int atexit(void (*fn)(void));                   /* up to 32 */
void abort(void) __attribute__((noreturn));     /* exit status 134, no flushing */

int atoi(const char *s);
long atol(const char *s);
long strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
int abs(int v);
long labs(long v);

void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
char *getenv(const char *name);                 /* there is no environment: always NULL */
int rand(void);
void srand(unsigned int seed);

#endif
