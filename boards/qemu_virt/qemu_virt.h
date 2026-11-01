/*
SurfOS on QEMU's RISC-V virt machine: its devices
----------------------
File: qemu_virt.h   Date: 10/15/26 (SurfCPU)
----------------------
The addresses of QEMU's `virt` machine (hw/riscv/virt.c, QEMU 10.0), which has nothing
to do with SurfCPU's memory map: this board exists to test the kernel under emulation.
*/

#ifndef _QEMU_VIRT_H
#define _QEMU_VIRT_H

#define QV_TEST_BASE      0x00100000UL  /* sifive_test: write to power off or reset */
#define QV_RTC_BASE       0x00101000UL  /* Goldfish RTC: nanoseconds since 1970 */
#define QV_CLINT_BASE     0x02000000UL  /* ACLINT: msip, mtimecmp, mtime */
#define QV_CLINT_MTIMECMP (QV_CLINT_BASE + 0x4000)     /* hart 0 */
#define QV_CLINT_MTIME    (QV_CLINT_BASE + 0xBFF8)
#define QV_TIMEBASE_HZ    10000000UL    /* RISCV_ACLINT_DEFAULT_TIMEBASE_FREQ */
#define QV_PLIC_BASE      0x0C000000UL
#define QV_PLIC_PRIORITY  (QV_PLIC_BASE + 0x000000)    /* 4 bytes per source */
#define QV_PLIC_PENDING   (QV_PLIC_BASE + 0x001000)
#define QV_PLIC_ENABLE    (QV_PLIC_BASE + 0x002000)    /* context 0: hart 0, M mode */
#define QV_PLIC_THRESHOLD (QV_PLIC_BASE + 0x200000)
#define QV_PLIC_CLAIM     (QV_PLIC_BASE + 0x200004)
#define QV_PLIC_SOURCES   96
#define QV_UART_BASE      0x10000000UL  /* ns16550a, byte registers */
#define QV_UART_IRQ       10            /* its PLIC source */

#define QV_TEST_PASS      0x5555
#define QV_TEST_FAIL      0x3333        /* | code << 16 */
#define QV_TEST_RESET     0x7777

/* the RAM disk window (board.ld: the kernel heap ends here): the test harness loads a disk
   image (MBR + FAT) at its start with -device loader,file=...,addr=0x88000000 */
#define QV_RAMDISK_BASE   0x88000000UL
#define QV_RAMDISK_MAX    0x04000000UL  /* 64 MB */

#endif
