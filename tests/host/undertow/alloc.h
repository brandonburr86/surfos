/*
SurfOS Undertow: the allocator of the host tests
----------------------
File: alloc.h  Date: 10/11/26
----------------------
The Makefile compiles Undertow's modules for host tests with malloc, calloc,
realloc, free, strdup and strndup renamed to these, so a test can count what a
module still holds and make any allocation fail.
*/

#ifndef UT_HOST_ALLOC_H
#define UT_HOST_ALLOC_H

#include <stddef.h>

extern long ut_alloc_live;          /* blocks allocated and not freed yet */
extern long ut_alloc_calls;         /* allocations asked for since the start */
extern long ut_alloc_fail_after;    /* >= 0: that many more allocations succeed, then all fail; -1: none fails */
extern long ut_alloc_fail_one;      /* >= 0: that many more succeed, then one fails, and the rest succeed (11/1/26) */
extern int ut_alloc_fill;           /* >= 0: what malloc gives starts as this byte (11/1/26) */
extern void (*ut_alloc_on_free)(void *p);   /* when set: each block, as free takes it (11/1/26) */

void *ut_test_malloc(size_t n);
void *ut_test_calloc(size_t n, size_t size);
void *ut_test_realloc(void *p, size_t n);
void ut_test_free(void *p);
char *ut_test_strdup(const char *s);
char *ut_test_strndup(const char *s, size_t n);

#endif
