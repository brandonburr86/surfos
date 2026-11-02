/*
SurfOS user program: bc
--------------------
File: lib.c     Date: 11/01/26
--------------------
The library -l defines, in bc's own language: s, c, e, l, a and j. It runs on the same machine as a
person's functions, so it needs nothing of its own but the three things parse.c and vm.c give a
function of the library: its constants are in base 10 whatever ibase says; names that start with _
are out of a program's reach; and when it returns, scale is its caller's again and the result is
cut to it, also when something fails halfway. So each function here sets scale to what it needs
and does not put it back. It works with about a dozen guard digits past the caller's scale, so that
truncating gives the digits of the true value; tests/host/test_bc.c holds it to that by working the
same value out further. The one limit, every bc's: a value whose digits past the scale start with a
longer run of 0s or 9s can come out a unit off in the last place. sin(1.570796) at scale 0 is 1,
for .99999999999994...

The mathematics. pi: Machin's formula. a and s, c: the series of the arctangent, the sine and the
cosine, after halving the angle or taking off quarter turns. l: for a large s, K(4/s), the complete
elliptic integral, is ln s to about 1/s^2, and Gauss's AGM gives K: so a logarithm is means and no
series (Salamin and Brent). ln 2: the factors of 2 in 16/15, 25/24 and 81/80. e: x = k ln 2 + f, and
f's digits cut in runs that double. j: Miller's downward recurrence, which needs no series and loses
no digits to cancelling, whatever the size of x.

What guards against what. e: k ln 2 needs ln 2 to the digits of k as well, and 2^k multiplies
what is left by its own digits. l: the small number 4/s needs its digits counted from its first,
and each of ln 2 and the mean carries an error that k - m multiplies. s and c: the quarter turns
taken off are a whole number times pi, so pi needs the argument's digits too. a: each halving of
the angle doubles what the series' error becomes. j: the recurrence starts where the values are
below the last digit wanted, so it ends with that many digits more than it needs.
*/

#include "bc.h"

const char bc_library_text[] =
/* atan(1/n) for a whole n above 1 */
"define _at(n) {\n"
"    auto u, t, v, q, k, g\n"
"    u = 1 / n; v = u; q = n * n; k = 1; g = -1\n"
"    while(1) {\n"
"        u /= q; k += 2; t = u / k\n"
"        if(t == 0) return(v)\n"
"        v += g * t; g = -g\n"
"    }\n"
"}\n"
/* pi to the current scale, by Machin's formula; kept, as s and c ask each time. The scale is
   never 0 here, so an empty _ps cannot pass for a value. */
"define _pi() {\n"
"    auto s\n"
"    if(_ps >= scale) return(_pv)\n"
"    s = scale; scale += 8\n"
"    _pv = 4 * (4 * _at(5) - _at(239)); _ps = s\n"
"    return(_pv)\n"
"}\n"
/* the arithmetic-geometric mean of 1 and b, at the current scale: the two means close in on each
   other, and the gap is squared at each turn, so one more mean after half the digits is all of them */
"define _ag(b) {\n"
"    auto a, c, g\n"
"    a = 1; g = sqrt(1 / 10 ^ scale)\n"
"    while(a - b > g) { c = (a + b) / 2; b = sqrt(a * b); a = c }\n"
"    return((a + b) / 2)\n"
"}\n"
/* ln(m / (m - 1)) = 1/m + 1/(2 m^2) + 1/(3 m^3) + ... for a whole m: each term is a short division */
"define _lm(m) {\n"
"    auto t, v, k\n"
"    t = 1 / m; v = t\n"
"    for(k = 2; t != 0; k++) { t /= m; v += t / k }\n"
"    return(v)\n"
"}\n"
/* ln 2 to the current scale, kept as pi is, as e and l ask each time: 2 = (16/15)^7 (25/24)^5 (81/80)^3,
   whose powers of 3 and 5 cancel */
"define _l2() {\n"
"    auto s\n"
"    if(_ls >= scale) return(_lv)\n"
"    s = scale; scale += 8\n"
"    _lv = 7 * _lm(16) + 5 * _lm(25) + 3 * _lm(81); _ls = s\n"
"    return(_lv)\n"
"}\n"
/* e^x: x = k ln 2 + f with f from 0 up to ln 2, so it is 2^k e^f. The digits of f are cut in runs, each
   twice as long as the last: e^f is the product of the e^r of the runs, each r is short, so a term of
   its series is cheap, and each is further out, so it has fewer terms (Brent's method). 1.4427 is
   1 / ln 2, near enough to count k's digits and 2^k's, of which there are under .31 k. A result under
   10^-(scale+1) is 0. */
"define e(x) {\n"
"    auto s, w, k, f, p, r, t, u, v, i\n"
"    if(x == 0) return(1)\n"
"    s = scale\n"
"    if(x + 3 * (s + 2) < 0) return(0)\n"
"    scale = 0\n"
"    k = x * 14427 / 10000; w = s + 14\n"
"    if(k > 0) w += k * 31 / 100\n"
"    w += length(k) + 2\n"
"    scale = w\n"
"    t = _l2()\n"
"    scale = 0\n"
"    k = x / t\n"
"    scale = w\n"
"    f = (x - k * t) / 1\n"
"    if(f < 0) { f += t; k -= 1 }\n"
"    v = 1; p = 4\n"
"    while(f != 0) {\n"
"        scale = p; r = f / 1; scale = w\n"
"        f -= r; p *= 2; t = 1; u = 1\n"
"        for(i = 1; t != 0; i++) { t = t * r / i; u += t }\n"
"        v *= u\n"
"    }\n"
"    if(k < 0) return(v / 2 ^ -k)\n"
"    return(v * 2 ^ k)\n"
"}\n"
/* ln x. k is a power of 2 that brings x near 1, which its count of digits gives: 3321928094887 / 10^12
   is log2 10 less 3.6e-13, so for the 2^31 digits a number can have at most, k is out by under a
   thousandth of a bit, and x / 2^k lies from .1 to 2. 2^m then makes it large: ln x =
   pi / (2 AGM(1, 4 / (x 2^(m-k)))) - (m - k) ln 2. Near x = 1 that is two long numbers that nearly
   cancel, which costs nothing, as a scale counts digits after the point. */
"define l(x) {\n"
"    auto s, w, t, k, m, v\n"
"    if(x <= 0) _fail \"not above zero\"\n"
"    if(x == 1) return(0)\n"
"    s = scale; scale = 0\n"
"    w = s + 14; t = w / 2 + length(w) + 2; m = t * 3321928094887 / 1000000000000 + 1\n"
"    if(x < 1) k = length(x) - scale(x) - 1 else k = length(x / 1)\n"
"    k = k * 3321928094887 / 1000000000000\n"
"    scale = w + t + length(w) + 2\n"
"    if(k < 0) x *= 2 ^ -k else x /= 2 ^ k\n"
"    v = _pi() / (2 * _ag(4 / (x * 2 ^ m)))\n"
"    scale = w + length(m - k) + 2\n"
"    return(v + (k - m) * _l2())\n"
"}\n"
/* atan x: below 1 by the reflection, smaller by halving the angle, then the series */
"define a(x) {\n"
"    auto s, m, c, q, j, t, u, v, k, w, g\n"
"    if(x == 0) return(0)\n"
"    s = scale; scale = 0\n"
"    q = sqrt(s + 12) / 2 + 1\n"
"    scale = s + 16 + q / 3\n"
"    m = 0; c = 0; j = 0; g = -1\n"
"    if(x < 0) { m = 1; x = -x }\n"
"    if(x > 1) { c = 1; x = 1 / x } else x /= 1\n"
"    t = 1 / 2 ^ q\n"
"    while(x > t) { x = x / (1 + sqrt(1 + x * x)); j += 1 }\n"
"    t = x * x; u = x; v = x; k = 1\n"
"    while(1) {\n"
"        u *= t; k += 2; w = u / k\n"
"        if(w == 0) break\n"
"        v += g * w; g = -g\n"
"    }\n"
"    v *= 2 ^ j\n"
"    if(c) v = _pi() / 2 - v\n"
"    if(m) v = -v\n"
"    return(v)\n"
"}\n"
/* sin x (o is 0) or cos x (o is 1): quarter turns off, so that f says which series and g the
   sign; then the argument at most pi/4 */
"define _sc(x, o) {\n"
"    auto s, h, d, w, p, k, t, f, g, r, u, v, i\n"
"    s = scale; h = 0\n"
"    if(x < 0) { x = -x; h = 1 - o }\n"
"    scale = 0\n"
"    d = length(x / 1); w = s + 12 + d\n"
"    scale = w\n"
"    p = _pi() / 2\n"
"    scale = 0\n"
"    k = x / p; t = (k + o) % 4; f = t % 2; g = t / 2\n"
"    scale = w\n"
"    r = x - k * p\n"
"    if(r > p / 2) { r = p - r; f = 1 - f }\n"
"    if(f) { v = 1; u = 1; i = 0 } else { v = r; u = r; i = 1 }\n"
"    r *= r\n"
"    while(1) {\n"
"        u = -u * r / ((i + 1) * (i + 2)); i += 2\n"
"        if(u == 0) break\n"
"        v += u\n"
"    }\n"
"    if(g) v = -v\n"
"    if(h) v = -v\n"
"    return(v)\n"
"}\n"
"define s(x) { return(_sc(x, 0)) }\n"
"define c(x) { return(_sc(x, 1)) }\n"
/* The Bessel function of the whole order n, by Miller's method. J(k-1) = 2k/x J(k) - J(k+1) is stable
   going down, where J grows, so from any start far enough above n it gives the values up to one
   factor, and 1 = J(0) + 2 J(2) + 2 J(4) + ... says what the factor is. Far enough: J(m) is at most
   (x/2)^m / m!, t, so the value of J at m is below the digits wanted, and the first term there is
   above them by as many: that is why whole numbers are enough for the way down. The signs:
   J(-n) and J(n) at -x are each (-1)^n J(n). */
"define j(n, x) {\n"
"    auto s, f, m, t, i, p, q, r, c, v\n"
"    s = scale; scale = 0\n"
"    n /= 1; f = 0\n"
"    if(n < 0) { n = -n; f = n % 2 }\n"
"    if(x < 0) { x = -x; if(n % 2) f = 1 - f }\n"
"    if(x == 0) return(n == 0)\n"
"    scale = s + 14; t = 1\n"
"    while(t != 0) { m += 1; t = t * x / (2 * m) }\n"
"    if(m <= n) return(0)\n"
"    scale = 0; p = 1\n"
"    for(i = m; i > 0; i--) {\n"
"        r = 2 * i * p / x - q; q = p; p = r\n"
"        if(i % 2) c += 2 * p\n"
"        if(i - 1 == n) v = p\n"
"    }\n"
"    scale = s + 14\n"
"    v /= c - p\n"
"    if(f) v = -v\n"
"    return(v)\n"
"}\n";
