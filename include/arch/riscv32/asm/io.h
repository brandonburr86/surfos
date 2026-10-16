/*
SurfOS I/O Access (riscv32)
----------------------
File: io.h      Date: 10/15/26 (SurfCPU), the riscv32 face of include/asm/io.h
----------------------
RISC-V has no I/O ports: every device is memory mapped, and SurfCPU's registers are
32-bit words accessed as aligned words (surfcpu/docs/MEMORY-MAP.md). mmio_read32()
and mmio_write32() are the accessors (volatile, in program order: SurfCPU completes
one access at a time, so no fence is needed between them; mmio_barrier() is a FENCE
for code that must order device accesses against normal memory on other hardware).

The x86 port I/O names are kept only so that shared files which mention them on a
path riscv32 never takes still compile (kernel/console.c's VGA cursor, for one); any
call that does happen stops the kernel with a panic naming the port, because it is
a porting bug.
*/

#ifndef _ASM_IO_H
#define _ASM_IO_H

#include <surfos/types.h>

static inline u32 mmio_read32(u_long addr) {
    return *(volatile u32 *)addr;
}

static inline void mmio_write32(u_long addr, u32 val) {
    *(volatile u32 *)addr = val;
}

static inline u8 mmio_read8(u_long addr) {
    return *(volatile u8 *)addr;
}

static inline void mmio_write8(u_long addr, u8 val) {
    *(volatile u8 *)addr = val;
}

static inline void mmio_barrier(void) {
    asm volatile("fence iorw, iorw" : : : "memory");
}

/* arch/riscv32/stubs.c: a panic, "port I/O on riscv32" */
void riscv32_port_io(const char *what, u_int port) __attribute__((noreturn));

static inline void outb(u_short port, u_char val) { (void)val; riscv32_port_io("outb", port); }
static inline void outw(u_short port, u_short val) { (void)val; riscv32_port_io("outw", port); }
static inline void outd(u_short port, u_int val) { (void)val; riscv32_port_io("outl", port); }
static inline u_char inb(u_short port) { riscv32_port_io("inb", port); }
static inline u_short inw(u_short port) { riscv32_port_io("inw", port); }
static inline u_int ind(u_short port) { riscv32_port_io("inl", port); }

#define inl ind
#define outl outd

#endif
