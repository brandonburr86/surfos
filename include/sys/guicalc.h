/*
SurfOS Desktop: the Calculator's Arithmetic
--------------------
File: guicalc.h Date: 10/25/26
--------------------
The Calculator's whole-number arithmetic (shell/guicalc.c), shared by the desktop's own Calculator
(shell/guicore.c) and the Calculator as a program (user/bin/wcalc.c, through libsurf's guicalc.c;
user/include/surfos/calc.h has the same declarations in standard types). Milestone M12, stage C6
of docs/superpowers/plans/2026-10-25-app-windows.md.
*/
#ifndef _SYS_GUICALC_H
#define _SYS_GUICALC_H
#include <surfos/types.h>

/* the value shown, the operand and operator waiting, and whether the next digit starts a new value, an error is shown,
   and a value was entered since the operator */
struct gui_calc { i32 value, operand; char op; bool fresh, error, entered; };
/* a key: a digit, + - * /, = or Enter, Backspace, C (clears) */
void gui_calc_key(struct gui_calc *c, int key);
#endif
