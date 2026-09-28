/*
    TURF.H
    Written by Martin McCormick (surfos@wazer.net) (c) 2004
    for SurfOS

    This is pretty much my idea of critical sections.
    Instead of actually halting task / proccess switches, only threads trying to enter the
    same critical section (turf) will be held up (in a task yield loop).  In fact,
    this is not even always true.  If two or more threads try to enter the same 'turf'
    with only READ access, they will all be allowed (no interference), however write
    and 'Ownership / create' require no other threads to be in the section.

    The biggest rule to remember is first come first serve - all others have to wwaaaait!!

    The turf rules below probably shouldn't be messed with unless there is a good reason.
--------------
    -Modified  7/1/04 by Brandon to make match SurfOS style
*/
#ifndef _SURFOS_TURF_H
#define _SURFOS_TURF_H

#include <surfos/types.h>
struct sTurf {
/*RE*/  /* number of types of access */
/*AD*/  u_int numAccess;
/*ON*/  /* the turf rules */
/*LY*/  u_int * access_rules; /* 2d array of size numAccess * numAccess */
/* READ / WRITE: */
        /* pointer to an array of access controllers (which keep track of access) */
        u_int * access_counts; /* should be type that can be atomically accessed (int) */
};

//Comment added 7/1/04 by Brandon: ^^ what the hell is the READONLY sidebar for??? :P


/*  turfVarRules - 2D array that specifies which access types can and cannot be active when
    requesting a given type of access. 1 = denied, 0 = allowed
    This is a default access for variables, and structures */
#define TURF_VAR_RULE_COUNT 3
extern u_int turfVarRules[TURF_VAR_RULE_COUNT][TURF_VAR_RULE_COUNT];

/* There are 3 types of access */
/*
    0 = Read access
    1 = Write access
    2 = Ownership (Creation, freeing, etc...)
    NOTE: All higher types include lower access.  For example, 1 is allowed to write AND read.
*/
#define TURF_VAR_READ 0
#define TURF_VAR_WRITE 1
#define TURF_VAR_CONTROL 2

u_int turfTread(void *vturf, u_int type);
u_int turfLeave(void *vturf, u_int type);
void *turfCreate(u_int numRules, u_int *rules);
void turfBurn(void * vturf);

#endif
