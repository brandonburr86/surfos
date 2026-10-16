/*
SurfOS stdint.h (riscv32)
----------------------
File: stdint.h  Date: 10/15/26 (SurfCPU)
----------------------
The kernel is built with -nostdinc, as on x86; the SurfCPU contract headers
(surfcpu/firmware/include/surfcpu/bootabi.h) want <stdint.h>. ILP32: int and long are
32 bits. uintptr_t and intptr_t are the same types surfos/types.h gives them.
*/

#ifndef _STDINT_H
#define _STDINT_H

typedef signed char        int8_t;
typedef short              int16_t;
typedef int                int32_t;
typedef long long          int64_t;
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef unsigned long      uintptr_t;
typedef long               intptr_t;

#define UINT8_MAX  0xFF
#define UINT16_MAX 0xFFFF
#define UINT32_MAX 0xFFFFFFFFu
#define UINT64_MAX 0xFFFFFFFFFFFFFFFFull
#define INT32_MAX  0x7FFFFFFF

#endif
