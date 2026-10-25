/*
SurfOS user program: seq
--------------------
File: seq.c     Date: 10/25/26
--------------------
seq [first [step]] last: the whole numbers from first (1) to last by step (1; one below 0
counts down), a line each.
*/

#include <stdio.h>
#include <stdlib.h>

static int number(const char *s, long *v) {
    char *end;
    *v = strtol(s, &end, 10);
    if(!*s || *end) {
        fprintf(stderr, "seq: %s: not a whole number\n", s);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    long first = 1, step = 1, last, v;
    if(argc < 2 || argc > 4) {
        fprintf(stderr, "usage: seq [first [step]] last\n");
        return 2;
    }
    if(number(argv[argc - 1], &last) < 0) return 2;
    if(argc > 2 && number(argv[1], &first) < 0) return 2;
    if(argc > 3 && number(argv[2], &step) < 0) return 2;
    if(!step) {
        fprintf(stderr, "seq: a step of 0\n");
        return 2;
    }
    for(v = first; step > 0 ? v <= last : v >= last; v += step) {
        printf("%ld\n", v);
        if((step > 0 && v > last - step) || (step < 0 && v < last - step)) break;   /* no wrap round */
    }
    return 0;
}
