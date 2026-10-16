/*
SurfCPU firmware: UART
--------------------
File: uart.c Date: 10/15/26 (SurfCPU)
--------------------
The SoC UART (docs/MEMORY-MAP.md, uart at 0x1000_0000), polled, at its reset setting of
115200 8N1. Output never waits without bound: a byte that finds the TX FIFO full for about
40 ms is dropped, so a broken UART cannot hang the firmware.
*/
#include "fw.h"

#define UART(off) REG(SURF_UART_BASE, off)
#define UART_SPIN_LIMIT 200000u

void uart_putc(char c)
{
    for (uint32_t spins = 0; UART(SURF_UART_TXDATA) & SURF_UART_TXDATA_FULL_MASK; spins++)
        if (spins >= UART_SPIN_LIMIT)
            return;
    UART(SURF_UART_TXDATA) = (uint8_t)c;
}

int uart_getc(void)
{
    uint32_t v = UART(SURF_UART_RXDATA);
    if (v & SURF_UART_RXDATA_EMPTY_MASK)
        return -1;
    return (int)(v & SURF_UART_RXDATA_DATA_MASK);
}

void uart_drain(uint32_t timeout_us)
{
    uint32_t t0 = timer_now32();
    while (!(UART(SURF_UART_STATUS) & SURF_UART_STATUS_TX_IDLE_MASK))
        if ((uint32_t)(timer_now32() - t0) >= timeout_us)
            return;
}
