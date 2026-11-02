/* For running tests/host/test_awk.c natively on a 64-bit host (the command is in its header): libsurf's string.c is not
   64-bit safe (its memset writes past the end there), so regex.c's calls go to the host's. Not in `make unittest`, which
   builds at -m32. */
#include <ctype.h>
#include <string.h>
void *surf_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
void *surf_memset(void *d, int c, size_t n) { return memset(d, c, n); }
int surf_memcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
size_t surf_strlen(const char *s) { return strlen(s); }
int surf_isalpha(int c) { return isalpha(c); }
int surf_isdigit(int c) { return isdigit(c); }
int surf_isalnum(int c) { return isalnum(c); }
int surf_isspace(int c) { return isspace(c); }
int surf_isupper(int c) { return isupper(c); }
int surf_islower(int c) { return islower(c); }
int surf_ispunct(int c) { return ispunct(c); }
int surf_iscntrl(int c) { return iscntrl(c); }
int surf_isgraph(int c) { return isgraph(c); }
int surf_isprint(int c) { return isprint(c); }
int surf_isxdigit(int c) { return isxdigit(c); }
