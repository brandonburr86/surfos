/*
SurfOS riscv32 Driver Table
--------------------
File: drivers.c Date: 10/15/26 (SurfCPU), after driver/drivers.c
--------------------
The x86 table probes PC hardware (PCI, ATA, the 8042, USB, ...), none of which a
RISC-V board has. Here the serial console's input comes first, then the board's own
table (boards/<board>/board.c): the RAM disk on QEMU virt; the LEDs, the PS/2
keyboard and the microSD card on the Arty. Status as on x86: 0 up, 1 not present,
below 0 failed.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/timer.h>
#include <sys/driver.h>
#include <sys/serial.h>
#include <riscv/board.h>
#include <blibc_common.h>

static int serial_status = 1;

static const char *status_name(int status) {
    if(status == DRV_SKIPPED) return "skipped";
    return status == 0 ? "ok" : (status > 0 ? "not present" : "failed");
}

void init_drivers(void) {
    struct driver *d;
    u_int i, n;
    serial_status = init_serial_irq();
    d = board_drivers(&n);
    for(i = 0; i < n; i++) d[i].status = d[i].init();
    kprintf("\nDrivers: serial%s", serial_status == 0 ? "" : "(absent)");
    for(i = 0; i < n; i++) {
        int s = d[i].status;
        kprintf(" %s%s", d[i].name, s == 0 ? "" : (s == DRV_SKIPPED ? "(skipped)" : (s > 0 ? "(absent)" : "(FAILED)")));
    }
    kprintf("\n");
}

void print_drivers(void) {
    char buf[160];
    struct driver *d;
    u_int i, n;
    d = board_drivers(&n);
    printf("    driver    status\n");
    printf("    %-9s %s\n", "serial", status_name(serial_status));
    for(i = 0; i < n; i++) printf("    %-9s %s\n", d[i].name, status_name(d[i].status));
    printf("    board     %s (%s)\n", board_name(), board_machine());
    printf("    console   %s\n", console_describe(buf, sizeof(buf)));
    printf("    serial    %s\n", serial_present() ? board_uart_describe(buf, sizeof(buf)) : "none");
    printf("    timer     %s\n", timer_describe(buf, sizeof(buf)));
    printf("    irq       %s\n", board_irq_describe(buf, sizeof(buf)));
    board_print_devices();
}
