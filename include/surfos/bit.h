/*
SurfOS Bit Handling Routines
----------------------
File: bit.h Date: Prior to 4/23/04
----------------------
(C)2004 Brandon Burr
*/

#ifndef _SURFOS_BIT_H
#define _SURFOS_BIT_H

#define set_bit(tab,pos) (tab)[(pos)/8] = ((tab)[(pos)/8] | (1 << ((pos)%8)))
#define clr_bit(tab,pos) ((tab)[(pos)/8] = ((tab)[(pos)/8] & ~(1 << ((pos)%8))))
#define test_bit(tab,pos) ((((tab)[(pos)/8] & (1 << ((pos)%8)))!=0)?1:0)

void print_bits(u_char tab[], u_int from, u_int to);

#endif
