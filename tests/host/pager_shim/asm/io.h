/* tests/host/test_pager: no port I/O on the host (the console's CRTC cursor, the UART) */
#ifndef _ASM_IO_H
#define _ASM_IO_H
#include <surfos/types.h>
static inline void outb(u_short port, u_char val) { (void)port; (void)val; }
static inline void outw(u_short port, u_short val) { (void)port; (void)val; }
static inline void outd(u_short port, u_int val) { (void)port; (void)val; }
static inline u_char inb(u_short port) { (void)port; return 0; }
static inline u_short inw(u_short port) { (void)port; return 0; }
static inline u_int ind(u_short port) { (void)port; return 0; }
static inline void io_wait(void) {}
#define inl ind
#define outl outd
#endif
