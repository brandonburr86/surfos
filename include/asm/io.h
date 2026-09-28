#ifndef _ASM_IO_H
#define _ASM_IO_H

#include <surfos/types.h>

void outb(const u_short port, const u_char b);
void outw(const u_short port, const u_short w);
void outd(const u_short port, const u_int d);

u_char inb(const u_short port);
u_short inw(const u_short port);
u_int ind(const u_short port);

#define inl ind
#define outl outd

#endif
