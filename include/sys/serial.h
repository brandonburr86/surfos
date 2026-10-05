/*
SurfOS Serial Console Header
----------------------
File: serial.h  Date: 10/3/26
----------------------
*/

#ifndef _SYS_SERIAL_H
#define _SYS_SERIAL_H

#include <surfos/types.h>

#define SERIAL_COM1      0x3F8
#define SERIAL_COM2      0x2F8
#define SERIAL_IRQ_COM1  4
#define SERIAL_IRQ_COM2  3

#define SERIAL_BAUD      115200
#define SERIAL_DIVISOR   (115200 / SERIAL_BAUD)

void init_serial(void);      /* program the UART if there is one; needs nothing else, call it first */
int init_serial_irq(void);   /* hook IRQ 4 so received bytes become keystrokes; needs init_task(); 1: no UART */
bool serial_present(void);

void serial_putc(char c);
void serial_puts(const char *s);

/* console mirror: translates the console's \n and backspace for a terminal */
void serial_console_putc(int c);
void serial_console_clear(void);

int serial_isr(u_int irq, void *param);

#endif
