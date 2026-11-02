/* Included ahead of each of awk's files for tests/host/test_awk.c. Their memory goes through the test, which holds it to
   a limit. A command's status is the exit status, as libsurf's system() and pclose() give it, not the host's wait
   status, and output is flushed first, as there. fclose() can be told to fail, as on a full disk. main() is the test's
   to call. */
#include <stdio.h>
#include <stdlib.h>
void *t_malloc(size_t n);
void *t_realloc(void *p, size_t n);
void t_free(void *p);
int t_system(const char *cmd);
FILE *t_popen(const char *cmd, const char *mode);
int t_pclose(FILE *f);
int t_fclose(FILE *f);
#define malloc t_malloc
#define realloc t_realloc
#define free t_free
#define system t_system
#define popen t_popen
#define pclose t_pclose
#define fclose t_fclose
#define main awk_main
