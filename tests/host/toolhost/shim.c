/* What user/libsurf/regex.c, built with surf_ names, wants of the rest of libsurf, from the host's C library: a program of
   user/bin built for the host (surf.h, beside this) has no other libsurf, and so runs under the sanitizers on any host. */
#include <stdlib.h>
#include <string.h>

void *surf_malloc(size_t n) { return malloc(n); }
void surf_free(void *p) { free(p); }
void *surf_calloc(size_t n, size_t k) { return calloc(n, k); }
void *surf_realloc(void *p, size_t n) { return realloc(p, n); }
void *surf_memset(void *d, int c, size_t n) { return memset(d, c, n); }
void *surf_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
int surf_memcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
size_t surf_strlen(const char *s) { return strlen(s); }
