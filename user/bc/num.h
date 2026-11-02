/*
SurfOS user program: bc
--------------------
File: num.h     Date: 11/01/26
--------------------
The numbers of bc (num.c): decimal, of any length, with their scale, the digits kept after the
point. A number never changes once it is made, so that a variable, a stack slot and an array
element share one by counting references. Every call that makes a number gives back a new
reference, or NULL with the reason in num_err. There are no system calls here:
tests/host/test_bc.c runs it on the host as it is.
*/

#ifndef _BC_NUM_H
#define _BC_NUM_H

#include <stdint.h>

#define NUM_BASE     10000u             /* a limb holds four decimal digits */
#define NUM_DIGITS   4
#define NUM_MAX_SCALE 2147483647u       /* BC_SCALE_MAX */
#define NUM_MAX_LIMBS (1u << 29)        /* with the scale, so that sizes fit 32 bits */
#define NUM_MAX_BASE 100000u            /* BC_BASE_MAX: num.c's short division stays in 32 bits */
#define NUM_EXACT    0xffffffffu        /* as a scale for num_mul(): keep every digit */

typedef unsigned short limb_t;

struct num {
    unsigned refs;
    unsigned len;                       /* limbs in d[], the top one not 0; 0 for the value zero */
    unsigned scale;                     /* decimal digits after the point */
    int neg;                            /* below zero; never set on zero */
    limb_t d[];                         /* the lowest limb first, the point on a limb's edge */
};

enum {
    NUM_OK,
    NUM_NOMEM,
    NUM_TOOLONG,                        /* more digits than the limits above let a number have */
    NUM_DIVZERO,
    NUM_NEGROOT,
    NUM_BADEXP,                         /* an exponent with a fraction */
    NUM_BIGEXP                          /* an exponent that does not fit 32 bits */
};

extern int num_err;
const char *num_strerror(int err);

struct num *num_ref(struct num *n);
void num_put(struct num *n);            /* NULL is fine */

struct num *num_uint(unsigned v);
struct num *num_parse(const char *s, unsigned n, unsigned base);   /* digits 0-9 A-F, each at its own value, at most one point */

struct num *num_add(const struct num *a, const struct num *b);
struct num *num_sub(const struct num *a, const struct num *b);
struct num *num_mul(const struct num *a, const struct num *b, unsigned scale);
struct num *num_div(const struct num *a, const struct num *b, unsigned scale);
struct num *num_mod(const struct num *a, const struct num *b, unsigned scale);
struct num *num_pow(const struct num *a, const struct num *b, unsigned scale);
struct num *num_sqrt(const struct num *a, unsigned scale);
struct num *num_neg(const struct num *a);
struct num *num_rescale(const struct num *a, unsigned scale);      /* cut or padded to that scale */

int num_cmp(const struct num *a, const struct num *b);             /* below, equal, above: -1, 0, 1 */
int num_whole(const struct num *n, unsigned *v);    /* the digits before the point; -1 when they do not fit */
int num_hasfrac(const struct num *n);               /* any digit after the point that is not 0? */
unsigned num_length(const struct num *n);           /* length(): the significant digits */

/* Writes n in the base, 2 to NUM_MAX_BASE, a character at a time; -1 when memory runs out. */
int num_print(const struct num *n, unsigned base, void (*put)(void *ctx, int c), void *ctx);

#endif
