/*
    TURF.C
    Written by Martin McCormick (surfos@wazer.net) (c) 2004
    for the Surf Operating System

    Note: read note in turf.h for more information on workings.
----------
    -Modified 7/1/04 by Brandon to match syles...
*/

#include <surfos/types.h>
#include <blibc_common.h>
#include <surfos/turf.h>
#include <surfos/task.h>
#include <mm/kalloc.h>

u_int turfVarRules[TURF_VAR_RULE_COUNT][TURF_VAR_RULE_COUNT] =
{       /* requested */  /* read? */ /* write? */ /* owner? */
        /* 0 - read */     {0,          1,           1},
        /* 1 - write */    {1,          1,           1},
        /* 2 - owner */    {1,          1,           1}
};

/* Enter turf (insure turfLeave is called when done!) */
u_int turfTread(void * vturf, u_int type) {
    u_int i;
    struct sTurf * pTurf = (struct sTurf *)vturf;

    /* sanity check */
    if (!pTurf || type >= pTurf->numAccess) return 0;

retry:
    pTurf->access_counts[type]++;

    for(i=0;i<pTurf->numAccess;i++) {
        if( ((u_int *)pTurf->access_rules + (type * pTurf->numAccess))[i]
            && (pTurf->access_counts[i] - (i==type ? 1 : 0)) ) {
            pTurf->access_counts[type]--;
            goto wait;
        }
    }
    /* made it through all the rules, allows to carry on (returns current
access count) */
    return pTurf->access_counts[type];

wait:
    yield(); //DoEvents(); (important!)
    kprintf("TURF: HAD TO WAIT!\n");
    goto retry;
}

/* Leave the turf (should be called once for every turfEnter!) */
u_int turfLeave(void * vturf, u_int type) {
    struct sTurf * pTurf = (struct sTurf *)vturf;

    /* sanity check */
    if (!pTurf || type >= pTurf->numAccess) return 0;

    pTurf->access_counts[type]--;
    return pTurf->access_counts[type];
}

/* Returns a void * to a sTurf structure,
   Takes in the rules */
void *turfCreate(u_int numRules, u_int *rules) {
    u_int i, j;
    struct sTurf * pTurf; /* returned */

    /* Make one malloc() call and use for both sTurf, its rules, and its counters. */
    u_char * allocMem = (char *)kalloc(sizeof(struct sTurf) + (numRules * numRules) * sizeof(u_int) + numRules * sizeof(u_int));
    pTurf = (struct sTurf *) allocMem;
    pTurf->access_rules = (u_int *)((char*)allocMem + sizeof(struct sTurf));
    pTurf->access_counts = (u_int *)((char*)allocMem + sizeof(struct sTurf) + (numRules * numRules)* sizeof(u_int));


    for(i=0;i<numRules;i++) {
        pTurf->access_counts[i] = 0;

        for(j=0;j<numRules;j++) {
            ((u_int *)pTurf->access_rules + (i * numRules))[j] =
            ((u_int *)rules               + (i * numRules))[j];
        }
    }

    return (void *)pTurf;
}

/* Note, this function should only be called after insuring that it will never be needed again
 (or attempted to be accessed) */
void turfBurn(void * vturf) {
    struct sTurf * pTurf = (struct sTurf *)vturf;

    /* sanity check */
    if (!vturf) return;

    kfree((void *)pTurf);
}
