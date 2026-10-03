/*
SurfOS - printf()
Infamous printf!!!
(C)2004 Brandon Burr

Since 10/2026 a thin layer over vsnprintf() (roadmap K5): the output goes through
putch() to the current task's console, and from there to the serial mirror.
*/

#include <blibc_common.h>
#include <stdarg.h>

int vprintf(const char *format, va_list ap) {
    char buf[512];
    char *p;
    int n = vsnprintf(buf, sizeof(buf), format, ap);
    for(p = buf; *p; p++) putch(*p);
    return n;
}

int printf(const char *format, ...) {
    va_list ap;
    int n;
    va_start(ap, format);
    n = vprintf(format, ap);
    va_end(ap);
    return n;
}
