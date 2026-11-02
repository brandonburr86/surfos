/* What the cap costs (11/01/26): on input of several shapes, how much longer the edit dcore.c finds is than the shortest, and
   how long it takes, at several caps. Not part of make unittest; the account of the diff track has its tables. Build and run on
   any host (the times are the best of three):
       cc -O2 -Iuser/diff tests/host/cli/measure_dcore.c user/diff/dcore.c -o measure
       ./measure [-s seed] [-t shapes] [lines [caps...]]
   -s gives other files of the same shapes, -t picks shapes by their numbers below, as digits. The shortest is found with a cap no
   edit reaches. Beyond 40000 lines that is too slow for most shapes, so there only the sorted numbers, whose search for it stays
   affordable, and the drifting files, for which the edit they were made by stands in for it, are measured. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dcore.h"

#define SHAPES 9
#define EXACT_LINES 40000

static unsigned long long seed;
static unsigned files;                  /* -s */

static unsigned rnd(unsigned n) {
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return (unsigned)((seed >> 11) % n);
}

static char (*keys)[12];

static int by_text(const void *p, const void *q) {
    return strcmp(keys[*(const unsigned *)p], keys[*(const unsigned *)q]);
}

/* Each shape makes a against b. The result: the length of the shortest edit when it is known without searching, -1 when it is
   not, or minus that length when it is only known to be no longer. */
static long long make(int shape, unsigned n, unsigned *a, unsigned *na, unsigned *b, unsigned *nb) {
    unsigned i, j, k = 0, t, on = 0;
    long long made = 0;
    seed = 88172645463325252ULL + (unsigned long long)shape + 1000ULL * files;
    *na = n;
    switch(shape) {
    case 0:                                     /* the numbers against their order as text: seq | sort */
        keys = malloc((size_t)n * 12);
        for(i = 0; i < n; i++) {
            snprintf(keys[i], 12, "%u", i + 1);
            a[i] = b[i] = i;
        }
        qsort(b, n, sizeof(*b), by_text);
        free(keys);
        k = n;
        made = -1;
        break;
    case 1: {                                   /* a file's blocks moved about */
        unsigned *start = malloc(sizeof(unsigned) * (n + 1)), *order, blocks = 0;
        for(i = 0; i < n; i += 1 + rnd(100)) start[blocks++] = i;
        start[blocks] = n;
        order = malloc(sizeof(unsigned) * blocks);
        for(i = 0; i < blocks; i++) order[i] = i;
        for(i = blocks - 1; i > 0; i--) {
            j = rnd(i + 1);
            t = order[i];
            order[i] = order[j];
            order[j] = t;
        }
        for(i = 0; i < n; i++) a[i] = i;
        for(i = 0; i < blocks; i++)
            for(j = start[order[i]]; j < start[order[i] + 1]; j++) b[k++] = j;
        free(start);
        free(order);
        made = -1;
        break;
    }
    case 2:                                     /* heavy editing of a file of repeated lines */
        for(i = 0; i < n; i++) a[i] = rnd(800);
        for(i = 0; i < n; i++) {
            unsigned r = rnd(10);
            if(r < 6) b[k++] = a[i];
            else if(r < 8) b[k++] = rnd(800);
            else if(r == 8) {
                b[k++] = a[i];
                b[k++] = rnd(800);
            }
        }
        made = -1;
        break;
    case 3:                                     /* two files of the same few hundred lines, unrelated */
        for(i = 0; i < n; i++) {
            a[i] = rnd(300);
            b[i] = rnd(300);
        }
        k = n;
        made = -1;
        break;
    case 4:                                     /* a permutation of unique lines */
        for(i = 0; i < n; i++) a[i] = b[i] = i;
        for(i = n - 1; i > 0; i--) {
            j = rnd(i + 1);
            t = b[i];
            b[i] = b[j];
            b[j] = t;
        }
        k = n;
        made = -1;
        break;
    case 5:                                     /* stretches rewritten, losing half their lines: the files drift apart */
    case 6:                                     /* the same, the new lines being copies of lines from elsewhere */
        for(i = 0; i < n; i++) a[i] = i;
        for(i = 0; i < n; i++) {
            if(!rnd(1500)) on = !on;
            if(!on) b[k++] = a[i];
            else {
                made += 1;
                if(rnd(2)) {
                    b[k++] = shape == 5 ? n + i : a[rnd(n)];
                    made += 1;
                }
            }
        }
        if(shape == 6) made = -made;            /* a copy may match: the shortest is no longer than this */
        break;
    case 7:                                     /* a quarter of the lines and one more, moved from the start to the end */
    case 8:                                     /* the same moved from the end to the start */
        t = n / 4 + 1;
        for(i = 0; i < n; i++) a[i] = i;
        for(i = 0; i < n; i++) b[k++] = shape == 7 ? (i + t) % n : (i + n - t) % n;
        made = 2LL * t;                         /* the block out and in again: the rest is in order */
        break;
    }
    *nb = k;
    return made;
}

static long long edits(const unsigned char *ma, unsigned na, const unsigned char *mb, unsigned nb) {
    long long e = 0;
    unsigned j;
    for(j = 0; j < na; j++) e += ma[j];
    for(j = 0; j < nb; j++) e += mb[j];
    return e;
}

int main(int argc, char **argv) {
    static const char *const names[SHAPES] = { "sorted", "blocks", "edited", "unrelated", "permuted", "drifting", "drift+copies",
                                               "moved down", "moved up" };
    const char *shapes = "012345678";
    unsigned n = 20000, caps[16], ncaps = 0, i;
    unsigned *a, *b;
    unsigned char *ma, *mb;
    int shape, arg = 1;
    for(; arg + 1 < argc && argv[arg][0] == '-'; arg += 2) {
        if(!strcmp(argv[arg], "-s")) files = (unsigned)atoi(argv[arg + 1]);
        else if(!strcmp(argv[arg], "-t")) shapes = argv[arg + 1];
        else break;
    }
    if(arg < argc) n = (unsigned)atoi(argv[arg++]);
    for(; arg < argc && ncaps < 16; arg++) caps[ncaps++] = (unsigned)atoi(argv[arg]);
    a = malloc(n * 2 * sizeof(*a));
    b = malloc(n * 2 * sizeof(*b));
    ma = malloc(n * 2);
    mb = malloc(n * 2);
    if(!ncaps) {
        caps[ncaps++] = 2048;
        caps[ncaps++] = 4096;
        caps[ncaps++] = 5000;
        caps[ncaps++] = 8000;
    }
    printf("%u lines; for each cap the edit's length over the shortest, and milliseconds\n%-12s %9s", n, "", "shortest");
    for(i = 0; i < ncaps; i++) printf("  %15u", caps[i]);
    printf("\n");
    for(shape = 0; shape < SHAPES; shape++) {
        unsigned na, nb, run;
        if(!strchr(shapes, '0' + shape)) continue;
        long long best = make(shape, n, a, &na, b, &nb);
        const char *sign = " ";
        if(best < -1) {
            best = -best;
            sign = "~";
        }
        if(best == -1) {
            if(n > EXACT_LINES && shape) continue;
            memset(ma, 0, na);
            memset(mb, 0, nb);
            dc_compare_limit(a, na, b, nb, ma, mb, na + nb + 1);
            best = edits(ma, na, mb, nb);
        }
        printf("%-12s %8lld%s", names[shape], best, sign);
        for(i = 0; i < ncaps; i++) {
            double fastest = 0;
            long long e = 0;
            for(run = 0; run < 3; run++) {
                struct timespec t0, t1;
                double ms;
                memset(ma, 0, na);
                memset(mb, 0, nb);
                clock_gettime(CLOCK_MONOTONIC, &t0);
                dc_compare_limit(a, na, b, nb, ma, mb, caps[i]);
                clock_gettime(CLOCK_MONOTONIC, &t1);
                ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
                if(!run || ms < fastest) fastest = ms;
                e = edits(ma, na, mb, nb);
            }
            printf("  %+7.1f%% %6.0f", 100.0 * (e - best) / best, fastest);
            fflush(stdout);
        }
        printf("\n");
    }
    return 0;
}
