/*
SurfOS user library: standard definitions
----------------------
File: stddef.h  Date: 10/5/26
----------------------
User programs are built with -nostdinc: these headers in user/include are the
whole C library interface they see (the kernel's headers stay out of reach).
*/

#ifndef _STDDEF_H
#define _STDDEF_H

typedef __SIZE_TYPE__ size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;

#ifndef NULL
#define NULL ((void *)0)
#endif

#define offsetof(type, member) __builtin_offsetof(type, member)

#endif
