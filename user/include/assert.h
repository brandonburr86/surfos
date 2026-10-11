/*
SurfOS user library: diagnostics
----------------------
File: assert.h  Date: 10/11/26
----------------------
assert(e): when e is false, the program prints the expression, the file, the
line and the function on stderr and ends as abort() does (exit status 134).
With NDEBUG defined where this header is included, assert() does nothing and
e is not evaluated. As C requires, the header may be included again after
NDEBUG changes, and assert() follows the last inclusion. static_assert is
C11's _Static_assert. (Added for Mbed TLS, whose sources include this header.)
*/

#undef assert
#ifdef NDEBUG
#define assert(e) ((void)0)
#else
#define assert(e) ((e) ? (void)0 : __assert_fail(#e, __FILE__, __LINE__, __func__))
#endif

#ifndef _ASSERT_H
#define _ASSERT_H

void __assert_fail(const char *expr, const char *file, int line, const char *func) __attribute__((noreturn));

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(static_assert)
#define static_assert _Static_assert
#endif

#endif
