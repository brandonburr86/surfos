/*
SurfOS Port I/O
----------------------
File: io.h      Date: Prior to 4/23/04, inline since 10/2026
----------------------
The 2004 versions were functions in kernel/assem.asm. Same names and argument
order (port first), now inline so a port access is one instruction.
*/

#ifndef _ASM_IO_H
#define _ASM_IO_H

#include <surfos/types.h>

static inline void outb(u_short port, u_char val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void outw(u_short port, u_short val) {
    asm volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline void outd(u_short port, u_int val) {
    asm volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline u_char inb(u_short port) {
    u_char val;
    asm volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline u_short inw(u_short port) {
    u_short val;
    asm volatile("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline u_int ind(u_short port) {
    u_int val;
    asm volatile("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

/* a write to the POST diagnostic port takes about a microsecond on every PC */
static inline void io_wait(void) {
    outb(0x80, 0);
}

#define inl ind
#define outl outd

#endif
