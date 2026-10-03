/*
BLIBC Variable Arguments
----------------------
File: stdarg.h  Date: 10/3/26 (roadmap K5)
----------------------
Thin wrappers over the compiler builtins; the kernel is built with -nostdinc.
*/

#ifndef _STDARG_H
#define _STDARG_H

typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)
#define va_end(ap)         __builtin_va_end(ap)
#define va_copy(d, s)      __builtin_va_copy(d, s)

#endif
