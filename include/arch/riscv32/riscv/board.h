/*
SurfOS riscv32: the board interface
----------------------
File: board.h   Date: 10/15/26 (SurfCPU)
----------------------
What arch/riscv32 asks of a board (boards/<board>/). Two exist: arty_a7, SurfCPU's
SoC on the Digilent Arty A7 (surfcpu/docs/MEMORY-MAP.md), and qemu_virt, QEMU's
generic RISC-V virt machine, a separate HAL for testing the kernel under emulation.
QEMU virt does not model SurfCPU's peripherals: passing on it says nothing about the
FPGA.

Everything here is called with the kernel's own rules: the early functions before
the heap and the console exist, the interrupt functions with interrupts off.
*/

#ifndef _RISCV_BOARD_H
#define _RISCV_BOARD_H

#include <surfos/types.h>

struct surf_boot_info;

/* identity and start */
const char *board_name(void);               /* "arty_a7", "qemu_virt" */
const char *board_machine(void);            /* the banner's "on ..." words */
void board_early_init(void);                /* first thing in kmain(): the console UART, no memory needed */
/* the boot information: SurfBoot's (checked by arch/riscv32/bootinfo.c), or NULL when there is none
   (QEMU); the board fills in what it knows: RAM, the command line, ... */
void board_boot_info(const struct surf_boot_info *bi);
void board_print_info(void);                /* bootinfo: the board's lines */

/* the console UART (polled output, interrupt or polled input) */
bool board_uart_present(void);
void board_uart_putc(char c);               /* bounded wait for room */
int board_uart_getc(void);                  /* a received byte, or -1 */
int board_uart_irq(void);                   /* its interrupt source, or -1 (polled from the tick) */
void board_uart_rx_irq(bool on);            /* the receive interrupt at the UART itself */
const char *board_uart_describe(char *buf, size_t size);

/* the machine timer (64-bit mtime, mtimecmp) */
u64 board_mtime(void);
void board_set_mtimecmp(u64 when);
u32 board_timebase_hz(void);

/* the interrupt controller behind mip.MEIP */
void board_irq_init(void);
void board_irq_enable(u_int irq, bool on);
int board_irq_claim(void);                  /* the next pending enabled source, or -1 */
void board_irq_complete(u_int irq);
const char *board_irq_describe(char *buf, size_t size);

/* drivers: block devices, the keyboard, LEDs (after init_task(): they may use interrupts);
   init_drivers() runs them in order and fills in their status */
struct driver;
struct driver *board_drivers(u_int *count);
void board_print_devices(void);             /* drivers: the board's lines (keyboard, card, ...) */

/* the text display, NULL when there is none (QEMU virt, an Arty built without video);
   arch/riscv32/fbcon.c puts kernel/console.c's cells on it */
struct board_display {
    const char *name;
    u_int cols, rows;
    void (*init)(void);
    void (*draw_cell)(u32 col, u32 row, u8 ch, u8 attr);
    void (*present)(const u8 *cells, u32 cols, u32 rows);   /* every cell that differs; may scroll in hardware */
    void (*set_cursor)(u32 col, u32 row, bool visible);
    const char *(*describe)(char *buf, size_t size);
};
const struct board_display *board_display(void);

/* time of day: seconds since 1970 (UTC), false when the board has no clock */
bool board_rtc(u64 *seconds);

/* the end */
void board_reset(void) __attribute__((noreturn));      /* reboot */
void board_poweroff(int code) __attribute__((noreturn)); /* QEMU: leave with code; the Arty: stop */

/* the board's commands in the kernel shell (lsdev, leds, ...); NULL-terminated */
struct command;
const struct command *board_commands(void);

#endif
