/* For running tests/host/test_wcode_save.c natively on a 64-bit host (tests/host/wcode/mac-sanitize.sh, 11/05/26): the
   C library calls the files built against SurfOS's headers make under libsurf's names (tests/host/libsurf_names.h), sent
   to the host's, as libsurf's string.c is not 64-bit safe and its printf.c is x86 assembly in part. strerror() keeps
   libsurf's own words (user/libsurf/string.c, Linux's numbers and messages), which the test compares. Not in
   `make unittest`, which links libsurf's own objects at -m32. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
void *surf_malloc(size_t n) { return malloc(n); }
void surf_free(void *p) { free(p); }
void *surf_calloc(size_t n, size_t k) { return calloc(n, k); }
void *surf_realloc(void *p, size_t n) { return realloc(p, n); }
void *surf_memchr(const void *s, int c, size_t n) { return memchr(s, c, n); }
int surf_memcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
void *surf_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
void *surf_memmove(void *d, const void *s, size_t n) { return memmove(d, s, n); }
void *surf_memset(void *d, int c, size_t n) { return memset(d, c, n); }
int surf_strcasecmp(const char *a, const char *b) { return strcasecmp(a, b); }
char *surf_strchr(const char *s, int c) { return strchr(s, c); }
int surf_strcmp(const char *a, const char *b) { return strcmp(a, b); }
char *surf_strcpy(char *d, const char *s) { return strcpy(d, s); }
size_t surf_strlcpy(char *d, const char *s, size_t n) { return strlcpy(d, s, n); }
size_t surf_strlen(const char *s) { return strlen(s); }
int surf_strncmp(const char *a, const char *b, size_t n) { return strncmp(a, b, n); }
char *surf_strrchr(const char *s, int c) { return strrchr(s, c); }
int surf_vsnprintf(char *b, size_t n, const char *f, va_list ap) { return vsnprintf(b, n, f, ap); }
int surf_snprintf(char *b, size_t n, const char *f, ...) {
    va_list ap;
    int r;
    va_start(ap, f);
    r = vsnprintf(b, n, f, ap);
    va_end(ap);
    return r;
}
char *surf_strerror(int err) {
    static const char *const messages[] = {
        "Success", "Operation not permitted", "No such file or directory", "No such process",
        "Interrupted system call", "I/O error", "No such device or address", "Argument list too long",
        "Exec format error", "Bad file descriptor", "No child processes", "Try again", "Out of memory",
        "Permission denied", "Bad address", "Block device required", "Device or resource busy",
        "File exists", "Invalid cross-device link", "No such device", "Not a directory", "Is a directory",
        "Invalid argument", "File table overflow", "Too many open files", "Not a typewriter",
        "Text file busy", "File too large", "No space left on device", "Illegal seek",
        "Read-only file system", "Too many links", "Broken pipe", "Math argument out of domain",
        "Result too large", "Resource deadlock would occur", "File name too long",
        "No record locks available", "Function not implemented", "Directory not empty",
    };
    if(err < 0) err = -err;
    return (char *)(err < (int)(sizeof(messages) / sizeof(messages[0])) ? messages[err] : "Unknown error");
}
