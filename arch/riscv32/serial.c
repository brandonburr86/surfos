/*
SurfOS riscv32 Serial Console
--------------------
File: serial.c  Date: 10/15/26 (SurfCPU)
--------------------
The interface of include/sys/serial.h on the board's console UART (a 16550 on QEMU
virt, surf_uart on the Arty) instead of COM1. What it does with the bytes is
driver/serial.c's: everything written to the active console is mirrored here
(newline as CR LF, backspace as BS SP BS, escape sequences and UTF-8 passed through),
and received bytes go to the active tty through tty_term_input(), the serial line's
input rules (kernel/tty.c). Input comes from the UART's receive interrupt, or, on a
board without one, from a kernel timer every 10 ms.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/interrupt.h>
#include <surfos/tty.h>
#include <surfos/ktimer.h>
#include <sys/serial.h>
#include <riscv/board.h>
#include <blibc_common.h>

static bool serial_up;
static u_char esc;              /* the line's escape state for tty_term_input() */
static struct ktimer poll_timer;
static u_long rx_bytes;

bool serial_present(void) {
    return serial_up;
}

void init_serial(void) {
    serial_up = board_uart_present();
}

void serial_putc(char c) {
    if(serial_up) board_uart_putc(c);
}

void serial_puts(const char *s) {
    while(*s) serial_putc(*s++);
}

/* as driver/serial.c: what the screen does with a character, done for a terminal */
void serial_console_putc(int c) {
    switch(c) {
    case '\n':
        serial_putc('\r');
        serial_putc('\n');
        break;
    case 0x08:
        serial_putc('\b');
        serial_putc(' ');
        serial_putc('\b');
        break;
    case '\r':
    case 0x1B:
        serial_putc((char)c);
        break;
    default:
        if((c >= 32 && c <= 126) || c < 0 || c >= 0x80) serial_putc((char)c);
        break;
    }
}

void serial_console_clear(void) {
    serial_puts("\033[2J\033[H");
}

/* received bytes, as typed on the active console: a bounded number per call */
int serial_isr(u_int irq, void *param) {
    int c, n;
    (void)irq;
    (void)param;
    for(n = 0; n < 256; n++) {
        c = board_uart_getc();
        if(c < 0) break;
        rx_bytes++;
        tty_term_input(tty_active, &esc, (u_char)c);
    }
    return 0;
}

static void serial_poll(void *arg) {
    (void)arg;
    serial_isr(0, NULL);
}

int init_serial_irq(void) {
    char buf[96];
    int irq;
    kprintf("\nSerial Console\n");
    if(!serial_up) {
        kprintf("*no UART\n");
        return 1;
    }
    kprintf("*%s\n", board_uart_describe(buf, sizeof(buf)));
    irq = board_uart_irq();
    if(irq >= 0) {
        add_irq_handler((u_int)irq, serial_isr, 0);
        board_uart_rx_irq(true);
        kprintf("*receive interrupt, source %i\n", irq);
    } else {
        ktimer_init(&poll_timer, serial_poll, NULL);
        ktimer_start(&poll_timer, 10, true);
        kprintf("*no receive interrupt: polled every 10 ms\n");
    }
    kprintf("*DONE\n");
    return 0;
}
