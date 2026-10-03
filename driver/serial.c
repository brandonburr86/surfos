/*
SurfOS Serial Console (COM1, 16550 UART)
--------------------
File: serial.c  Date: 10/3/26
--------------------
Output: everything written to the active console is mirrored here, plus the kernel
messages printed before the video console exists (they used to be lost).
Input: received bytes go into the keyboard queue, so the shell can be driven from a
terminal: qemu -nographic, or a null-modem cable on real hardware.
*/

#include <surfos/types.h>
#include <asm/io.h>
#include <surfos/console.h>
#include <surfos/interrupt.h>
#include <surfos/keyboard.h>
#include <surfos/tty.h>
#include <sys/serial.h>

#define UART_RBR  0 /* receive buffer (read) / transmit holding register (write) */
#define UART_IER  1 /* interrupt enable */
#define UART_FCR  2 /* FIFO control (write) */
#define UART_LCR  3 /* line control */
#define UART_MCR  4 /* modem control */
#define UART_LSR  5 /* line status */

#define LSR_DATA_READY 0x01
#define LSR_THR_EMPTY  0x20

static u_int uart = SERIAL_COM1;
static bool serial_up = false;

void init_serial(void) {
    outb(uart + UART_IER, 0x00);                 /* no interrupts until init_serial_irq() */
    outb(uart + UART_LCR, 0x80);                 /* DLAB on: the next two writes set the divisor */
    outb(uart + UART_RBR, SERIAL_DIVISOR & 0xFF);
    outb(uart + UART_IER, SERIAL_DIVISOR >> 8);
    outb(uart + UART_LCR, 0x03);                 /* 8 data bits, no parity, 1 stop bit, DLAB off */
    outb(uart + UART_FCR, 0xC7);                 /* enable and clear both FIFOs, 14-byte threshold */
    outb(uart + UART_MCR, 0x0B);                 /* DTR, RTS, OUT2 (OUT2 gates the IRQ line) */
    serial_up = true;
}

void serial_putc(char c) {
    u_int spins;
    if(!serial_up) return;
    /* bounded wait: a missing UART reads 0xFF (so this passes at once); a stalled one (QEMU with
       nobody reading the other end) must not hang the kernel, only slow its console down */
    for(spins = 0; spins < 8192; spins++) {
        if(inb(uart + UART_LSR) & LSR_THR_EMPTY) break;
    }
    outb(uart + UART_RBR, c);
}

void serial_puts(const char *s) {
    while(*s) serial_putc(*s++);
}

/* What the VGA console does with a character, done for a terminal instead */
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
    default:
        if(c >= 32 && c <= 126) serial_putc((char)c);
        break;
    }
}

void serial_console_clear(void) {
    serial_puts("\033[2J\033[H"); /* ANSI: clear screen, cursor home */
}

/*
    IRQ 4: feed received bytes to the keyboard queue as if they were typed.
    CR becomes newline and DEL becomes backspace so terminals behave; ANSI escape
    sequences (arrow keys etc.) are swallowed until there is something to do with them.
*/
int serial_isr(u_int irq, void *param) {
    static int esc = 0; /* 0 = normal, 1 = got ESC, 2 = inside ESC [ or ESC O */
    u_char c;

    while(inb(uart + UART_LSR) & LSR_DATA_READY) {
        c = inb(uart + UART_RBR);
        if(esc == 1) {
            esc = (c == '[' || c == 'O') ? 2 : 0;
            continue;
        }
        if(esc == 2) {
            if(c >= 0x40 && c <= 0x7E) { /* final byte: arrows and friends become key codes */
                esc = 0;
                switch(c) {
                case 'A': tty_input(tty_active, UP); break;
                case 'B': tty_input(tty_active, DOWN); break;
                case 'C': tty_input(tty_active, RT); break;
                case 'D': tty_input(tty_active, LEFT); break;
                case 'H': tty_input(tty_active, HOME); break;
                case 'F': tty_input(tty_active, END); break;
                default: break;
                }
            }
            continue;
        }
        if(c == 0x1B) { esc = 1; continue; }
        if(c == '\r') c = '\n';
        if(c == 0x7F) c = 0x08;
        tty_input(tty_active, c);
    }
    return 0;
}

void init_serial_irq(void) {
    kprintf("\nSerial Console\n");
    kprintf("*COM1 at 0x%x, %i 8N1, IRQ %i\n", uart, SERIAL_BAUD, SERIAL_IRQ_COM1);
    add_irq_handler(SERIAL_IRQ_COM1, serial_isr, 0);
    outb(uart + UART_IER, 0x01);                 /* received-data-available interrupt */
    kprintf("*DONE\n");
}
