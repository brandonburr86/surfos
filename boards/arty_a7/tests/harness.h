/*
SurfOS host test: the Arty board's pure code -- the harness
----------------------
File: harness.h Date: 10/15/26 (SurfCPU)
----------------------
Each test_*.c names its tests with TEST(fn); fn(seed) runs once per seed given to the
program (1 7 123 by default). The board files are built from the same sources the kernel
links, with the host compiler and the shim types (tests/shim) (make -C surfcpu test-os-host).
*/

#ifndef _HARNESS_H
#define _HARNESS_H

#include <surfos/types.h>

void check(int ok, const char *what);
unsigned rnd(void);                         /* a deterministic generator, reseeded per seed */
void test_register(const char *name, void (*fn)(unsigned seed));

#define TEST(fn) \
    static void __attribute__((constructor)) fn##_register(void) { test_register(#fn, fn); }

#endif
