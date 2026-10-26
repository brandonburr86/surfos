/*
SurfOS Memory Hog
--------------------
File: memhog.c  Date: 10/25/26 (robustness, track B: L1-L9)
--------------------
memhog: takes memory in 64 KB blocks, every page of each written, until malloc() fails; says how
much it got, gives it all back and exits 0. What stops it is the reserve the kernel keeps from
programs (mm/pmm.h), not the machine: everything else goes on.

memhog stack: the same, then a recursion that needs a stack that can no longer grow. The kernel
ends it with "out of memory: its stack could not grow", and nothing else breaks.

memhog hold MS: the same, then it holds it all for MS milliseconds before giving it back, so that
others meet a full machine meanwhile (a program fails to start or to allocate; the kernel's own
work goes on).
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <surf.h>

#define BLOCK (64 * 1024)

struct block {                          /* each block begins with a link to the block taken before it */
    struct block *next;
};

static struct block *hog(unsigned long *blocks) {
    struct block *list = NULL, *b;
    unsigned long n = 0;
    while((b = (struct block *)malloc(BLOCK)) != NULL) {
        memset(b, 0x5a, BLOCK);         /* every page written: its frame really is taken */
        b->next = list;
        list = b;
        n++;
    }
    *blocks = n;
    return list;
}

static void give_back(struct block *list) {
    while(list) {
        struct block *next = list->next;
        free(list);
        list = next;
    }
}

static int deeper(int n) {              /* 512 bytes a level, until a stack page cannot be had */
    volatile char pad[512];
    if(n < 0) return 0;                 /* never: the stack gives out long before n wraps */
    pad[n & 511] = (char)n;
    return deeper(n + 1) + pad[0];
}

int main(int argc, char **argv) {
    unsigned long blocks;
    struct block *list;
    if(argc == 1) {
        list = hog(&blocks);
        printf("memhog: %lu KB in %lu blocks of 64 KB before malloc failed\n", blocks * (BLOCK / 1024), blocks);
        give_back(list);
        printf("memhog: given back\n");
        return 0;
    }
    if(argc == 2 && !strcmp(argv[1], "stack")) {
        list = hog(&blocks);
        printf("memhog stack: %lu KB taken; now a stack that cannot grow\n", blocks * (BLOCK / 1024));
        return deeper(0);               /* the kernel ends it in there */
    }
    if(argc == 3 && !strcmp(argv[1], "hold") && atoi(argv[2]) > 0) {
        list = hog(&blocks);
        printf("memhog hold: %lu KB taken; holding them %d ms\n", blocks * (BLOCK / 1024), atoi(argv[2]));
        msleep((unsigned)atoi(argv[2]));
        give_back(list);
        printf("memhog hold: given back\n");
        return 0;
    }
    fprintf(stderr, "usage: memhog [stack | hold MS]\n");
    return 2;
}
