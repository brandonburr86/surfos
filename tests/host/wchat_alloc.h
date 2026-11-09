/* Included ahead of each of the Chat's model files (user/wchat/) for tests/host/test_wchat.c: their memory goes
   through the test, which counts it and can refuse any one allocation. */
#include <stdlib.h>
void *wchat_malloc(size_t n);
void *wchat_calloc(size_t n, size_t size);
void *wchat_realloc(void *p, size_t n);
void wchat_free(void *p);
#define malloc wchat_malloc
#define calloc wchat_calloc
#define realloc wchat_realloc
#define free wchat_free
