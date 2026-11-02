/*
SurfOS user library: the mathematical functions
----------------------
File: math.h    Date: 11/01/26
----------------------
user/libsurf/math.c, portable C without assembly. Results are within an ulp of the true
value, and the special cases are those of C's Annex F. errno is EDOM for an argument
outside a function's domain and ERANGE for a result too large or too small (a pole too).
*/

#ifndef _MATH_H
#define _MATH_H

#define HUGE_VAL    (__builtin_huge_val())
#define INFINITY    (__builtin_inff())
#define NAN         (__builtin_nanf(""))

#define M_E         2.7182818284590452354
#define M_LOG2E     1.4426950408889634074
#define M_LOG10E    0.43429448190325182765
#define M_LN2       0.69314718055994530942
#define M_LN10      2.30258509299404568402
#define M_PI        3.14159265358979323846
#define M_PI_2      1.57079632679489661923
#define M_PI_4      0.78539816339744830962
#define M_1_PI      0.31830988618379067154
#define M_2_PI      0.63661977236758134308
#define M_SQRT2     1.41421356237309504880
#define M_SQRT1_2   0.70710678118654752440

/* for any floating type, as C has them */
#define isnan(x)    __builtin_isnan(x)
#define isinf(x)    __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)

double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double exp(double x);
double log(double x);
double log2(double x);
double log10(double x);
double pow(double x, double y);
double sqrt(double x);
double cbrt(double x);
double hypot(double x, double y);
double floor(double x);
double ceil(double x);
double round(double x);                 /* a half away from zero */
double trunc(double x);
double fabs(double x);
double fmod(double x, double y);        /* the sign of x, exact */
double modf(double x, double *whole);
double frexp(double x, int *e);
double ldexp(double x, int e);

#endif
