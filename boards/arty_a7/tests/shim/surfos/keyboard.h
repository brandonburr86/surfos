/*
SurfOS host-test shim: surfos/keyboard.h
----------------------
File: keyboard.h    Date: 10/15/26 (SurfCPU)
----------------------
The kernel's own keyboard.h (the key codes F1.., HOME, UP, ...), reached by its path so the
host test does not put the kernel's libc headers (include/stdio.h, ...) ahead of the host's.
*/
#include "../../../../../include/surfos/keyboard.h"
