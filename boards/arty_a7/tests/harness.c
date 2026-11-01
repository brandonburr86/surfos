/*
SurfOS host test: the Arty board's pure code -- the harness
--------------------
File: harness.c Date: 10/15/26 (SurfCPU)
--------------------
  test_arty [seed...]       every registered test once per seed; exit status 0 when every check passed
*/

#include <stdio.h>
#include <stdlib.h>
#include "harness.h"

#define MAX_TESTS 32

static struct { const char *name; void (*fn)(unsigned); } tests[MAX_TESTS];
static int ntests, checks, fails;
static unsigned rng;
static const char *current;

void test_register(const char *name, void (*fn)(unsigned seed)) {
    if(ntests < MAX_TESTS) {
        tests[ntests].name = name;
        tests[ntests].fn = fn;
        ntests++;
    }
}

void check(int ok, const char *what) {
    checks++;
    if(!ok) {
        fails++;
        printf("FAIL %s: %s\n", current, what);
    }
}

unsigned rnd(void) {
    rng = rng * 1664525u + 1013904223u;
    return rng >> 8;
}

int main(int argc, char **argv) {
    unsigned seeds[8] = { 1, 7, 123 };
    int nseeds = 3, i, t;
    if(argc > 1) {
        nseeds = 0;
        for(i = 1; i < argc && nseeds < 8; i++) seeds[nseeds++] = (unsigned)strtoul(argv[i], NULL, 0);
    }
    for(i = 0; i < nseeds; i++) {
        for(t = 0; t < ntests; t++) {
            rng = seeds[i] * 2654435761u + (unsigned)t;
            current = tests[t].name;
            tests[t].fn(seeds[i]);
        }
    }
    printf("test_arty: %d of %d checks passed, %d tests x %d seeds (", checks - fails, checks, ntests, nseeds);
    for(i = 0; i < nseeds; i++) printf("%s%u", i ? " " : "", seeds[i]);
    printf(")\n");
    return fails ? 1 : 0;
}
