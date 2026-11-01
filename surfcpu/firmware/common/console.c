/*
SurfCPU firmware: console
--------------------
File: console.c Date: 10/15/26 (SurfCPU)
--------------------
Output goes to the UART (with "\n" sent as "\r\n") and to the VGA text screen; input comes
from the UART.
*/
#include "fw.h"

void con_putc(char c)
{
    if (c == '\n')
        uart_putc('\r');
    uart_putc(c);
    video_putc(c);
}

void con_puts(const char *s)
{
    while (*s)
        con_putc(*s++);
}

static void con_out(void *ctx, char c)
{
    (void)ctx;
    con_putc(c);
}

int con_vprintf(const char *fmt, va_list ap)
{
    return fmt_vformat(con_out, 0, fmt, ap);
}

int con_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = con_vprintf(fmt, ap);
    va_end(ap);
    return n;
}

int con_getc(void)
{
    return uart_getc();
}
