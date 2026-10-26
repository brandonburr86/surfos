/*
SurfOS toobig
--------------------
File: toobig.c  Date: 10/25/26 (the review of B1)
--------------------
A program bigger than the memory programs may have under QEMU: 1000 MB of zeroed data, so its
loading fails for want of memory partway (the kernel marks the task TF_LOADNOMEM). The smoke test
starts it as every console's shell ('@boot surfsh=toobig'): init must say that it tries again and
keep trying, never give the consoles up for the recovery console. Where it does load (a machine with
more than 1 GB for programs) it says so and exits 3.
*/

#include <stdio.h>

static volatile char big[1000u << 20];      /* volatile: -O2 would drop an array nothing reads */

int main(void) {
    big[sizeof(big) - 1] = 1;
    printf("toobig: loaded (%u MB)\n", (unsigned)(sizeof(big) >> 20));
    return 3;
}
