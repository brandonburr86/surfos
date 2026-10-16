/*
SurfOS host-test shim: surfos/types.h
----------------------
File: types.h   Date: 10/15/26 (SurfCPU)
----------------------
The kernel's type names on the host compiler (64-bit, with its own size_t), for the
Arty board's pure files under test (boards/arty_a7/tests/test_arty.c). The shim directory
comes first on the include path; nothing else of the kernel is pulled in.
*/

#ifndef _SURFOS_TYPES_H
#define _SURFOS_TYPES_H

#include <stddef.h>
#include <stdint.h>

typedef unsigned int bool;
#define false 0
#define true 1

typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned int u_int;
typedef unsigned long u_long;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
typedef u_char TEXTCOLOR;

#endif
