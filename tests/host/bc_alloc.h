/* Included ahead of each of bc's files for tests/host/test_bc.c: their memory goes through the test, which counts it,
   holds it to a limit, and can refuse any one allocation. */
#include <stdlib.h>
void *t_malloc(size_t n);
void *t_calloc(size_t n, size_t size);
void *t_realloc(void *p, size_t n);
void t_free(void *p);
#define malloc t_malloc
#define calloc t_calloc
#define realloc t_realloc
#define free t_free
