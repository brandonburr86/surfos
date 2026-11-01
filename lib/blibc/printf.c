/*
SurfOS - printf()
Infamous printf!!!
(C)2004 Brandon Burr

Since 10/2026 a thin layer over vsnprintf() (roadmap K5): the output goes through
putch() to the current task's console, and from there to the serial mirror.
10/16/26: a call is drawn whole, another task's line never inside it (audit C10).
*/

#include <blibc_common.h>
#include <stdarg.h>
#include <surfos/console.h>
#include <surfos/task.h>

int vprintf(const char *format, va_list ap) {
    char buf[512];
    char *p;
    int n = vsnprintf(buf, sizeof(buf), format, ap);
    int line = console_print_begin(curTask ? curTask->con : NULL);
    for(p = buf; *p; p++) putch(*p);
    console_print_end(line);
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
