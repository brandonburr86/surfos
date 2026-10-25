/*
SurfOS user library: the Calculator's arithmetic
--------------------
File: calc.h    Date: 10/25/26
--------------------
The desktop's Calculator arithmetic (the kernel's include/sys/guicalc.h, shell/guicalc.c), for a
program (user/bin/wcalc.c): the same structure and call in standard types. libsurf builds the
kernel's file (user/libsurf/guicalc.c), whose structure is the one this declares again.
*/
#ifndef _SURFOS_CALC_H
#define _SURFOS_CALC_H

#ifndef _SYS_GUICALC_H                  /* the kernel's own, in libsurf's guicalc.c */
/* the value shown, the operand and operator waiting, and whether the next digit starts a new value, an error is shown,
   and a value was entered since the operator */
struct gui_calc { int value, operand; char op; unsigned fresh, error, entered; };
#endif
typedef char gui_calc_is_the_kernels[sizeof(struct gui_calc) == 24 ? 1 : -1];
/* a key: a digit, + - * /, = or Enter, Backspace, C (clears) */
void gui_calc_key(struct gui_calc *c, int key);

#endif
