/*
SurfOS Undertow: the allocator of the host tests
----------------------
File: alloc.c  Date: 10/11/26
----------------------
alloc.h. Built without the renames, so these are the host's own functions.
*/

#include <stdlib.h>
#include <string.h>
#include "alloc.h"

long ut_alloc_live, ut_alloc_calls, ut_alloc_fail_after = -1;
int ut_alloc_fill = -1;

static int refuse(void) {
    ut_alloc_calls++;
    if(ut_alloc_fail_after < 0) return 0;
    if(ut_alloc_fail_after == 0) return 1;
    ut_alloc_fail_after--;
    return 0;
}

void *ut_test_malloc(size_t n) {
    void *p;
    if(refuse()) return NULL;
    p = malloc(n ? n : 1);
    if(p) ut_alloc_live++;
    if(p && ut_alloc_fill >= 0) memset(p, ut_alloc_fill, n ? n : 1);
    return p;
}

void *ut_test_calloc(size_t n, size_t size) {
    void *p;
    if(refuse()) return NULL;
    if(size && n > (size_t)-1 / size) return NULL;
    p = calloc(n ? n : 1, size ? size : 1);
    if(p) ut_alloc_live++;
    return p;
}

void *ut_test_realloc(void *p, size_t n) {
    void *q;
    if(!p) return ut_test_malloc(n);
    if(refuse()) return NULL;
    q = realloc(p, n ? n : 1);
    return q;
}

void ut_test_free(void *p) {
    if(p) ut_alloc_live--;
    free(p);
}

char *ut_test_strdup(const char *s) {
    return ut_test_strndup(s, strlen(s));
}

char *ut_test_strndup(const char *s, size_t n) {
    size_t len = strnlen(s, n);
    char *p = ut_test_malloc(len + 1);
    if(!p) return NULL;
    memcpy(p, s, len);
    p[len] = 0;
    return p;
}
