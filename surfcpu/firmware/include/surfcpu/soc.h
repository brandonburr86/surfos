/*
 * Generated from spec/soc.toml by tools/socspec.py gen -- do not edit.
 * SurfCPU memory map version 1, SoC version 0.1.0.
 */

#ifndef SURFCPU_SOC_H
#define SURFCPU_SOC_H

/* Register access: SURF_REG(SURF_UART_BASE, SURF_UART_STATUS). */
#define SURF_REG(base, off) (*(volatile unsigned int *)((base) + (off)))

#define SURF_MAP_VERSION 1 /* memory map version */
#define SURF_SOC_VERSION 0x00000100u /* SoC version 0.1.0 */
#define SURF_RESET_PC 0x00000000u /* reset vector */
#define SURF_HART_ID 0 /* mhartid */
#define SURF_OSC_HZ 100000000 /* Hz */
#define SURF_SYS_HZ 25000000 /* Hz */
#define SURF_TIMEBASE_HZ 1000000 /* Hz */
#define SURF_PIX_HZ_NOMINAL 25175000 /* Hz */
#define SURF_PIX_HZ 25177305 /* Hz */
#define SURF_BOARD_SIM 0x000000F0u /* sysinfo.BOARD_ID */
#define SURF_BOARD_ARTY_A7_35T 0x00000135u /* sysinfo.BOARD_ID */
#define SURF_BOARD_ARTY_A7_100T 0x00000100u /* sysinfo.BOARD_ID */
#define SURF_BOOTROM_BASE 0x00000000u /* Boot ROM, 64 KiB of block RAM initialized from the SurfBoot build; reset vector at 0 */
#define SURF_BOOTROM_SIZE 0x00010000u
#define SURF_BOOTROM_MASK 0x0000FFFFu /* address bits inside the region */
#define SURF_SCRATCH_BASE 0x00010000u /* Scratch RAM, 64 KiB of block RAM: SurfBoot's data and stack, the boot info page, BRAM-only tests */
#define SURF_SCRATCH_SIZE 0x00010000u
#define SURF_SCRATCH_MASK 0x0000FFFFu /* address bits inside the region */
#define SURF_UART_BASE 0x10000000u /* UART (115200 8N1 at reset) */
#define SURF_UART_SIZE 0x00001000u
#define SURF_UART_MASK 0x00000FFFu /* address bits inside the region */
#define SURF_TIMER_BASE 0x10001000u /* Machine timer: 64-bit mtime/mtimecmp, msip */
#define SURF_TIMER_SIZE 0x00001000u
#define SURF_TIMER_MASK 0x00000FFFu /* address bits inside the region */
#define SURF_INTC_BASE 0x10002000u /* Interrupt controller: level sources masked into mip.MEIP */
#define SURF_INTC_SIZE 0x00001000u
#define SURF_INTC_MASK 0x00000FFFu /* address bits inside the region */
#define SURF_GPIO_BASE 0x10003000u /* LEDs, RGB LEDs, switches, buttons */
#define SURF_GPIO_SIZE 0x00001000u
#define SURF_GPIO_MASK 0x00000FFFu /* address bits inside the region */
#define SURF_SPI_BASE 0x10004000u /* SPI master for the microSD card */
#define SURF_SPI_SIZE 0x00001000u
#define SURF_SPI_MASK 0x00000FFFu /* address bits inside the region */
#define SURF_PS2_BASE 0x10005000u /* PS/2 keyboard port */
#define SURF_PS2_SIZE 0x00001000u
#define SURF_PS2_MASK 0x00000FFFu /* address bits inside the region */
#define SURF_VIDEO_BASE 0x10006000u /* VGA text display control, cursor, palette */
#define SURF_VIDEO_SIZE 0x00001000u
#define SURF_VIDEO_MASK 0x00000FFFu /* address bits inside the region */
#define SURF_SYSINFO_BASE 0x10007000u /* Board and build identity, clocks, DDR status */
#define SURF_SYSINFO_SIZE 0x00001000u
#define SURF_SYSINFO_MASK 0x00000FFFu /* address bits inside the region */
#define SURF_ETH_BASE 0x10008000u /* Reserved for a later Ethernet MAC (decodes as unmapped until then) */
#define SURF_ETH_SIZE 0x00001000u
#define SURF_ETH_MASK 0x00000FFFu /* address bits inside the region */
#define SURF_TEXTRAM_BASE 0x20000000u /* Text memory: 4096 32-bit cells; cells 0..2399 are the 80x30 screen (cell = row*80 + col) */
#define SURF_TEXTRAM_SIZE 0x00004000u
#define SURF_TEXTRAM_MASK 0x00003FFFu /* address bits inside the region */
#define SURF_DDR_BASE 0x80000000u /* 256 MiB DDR3L; every access is an error until calibration has succeeded (sysinfo.DDR_STATUS) */
#define SURF_DDR_SIZE 0x10000000u
#define SURF_DDR_MASK 0x0FFFFFFFu /* address bits inside the region */
#define SURF_SURFBOOT_DATA_BASE 0x00010000u /* SurfBoot .data/.bss, heap-free buffers and its stack (stack top = bootinfo base) */
#define SURF_SURFBOOT_DATA_SIZE 0x0000F000u
#define SURF_BOOTINFO_BASE 0x0001F000u /* struct surf_boot_info from SurfBoot; preserved until the kernel has copied it */
#define SURF_BOOTINFO_SIZE 0x00001000u
#define SURF_KERNEL_BASE 0x80000000u /* Where SURFOS.BIN payloads load by default (the image header decides); the kernel owns the rest of DDR */
#define SURF_KERNEL_SIZE 0x0F000000u
#define SURF_DDR_TOP_BASE 0x8F000000u /* Top 16 MiB of DDR: kept free by SurfBoot (later display buffers, loader staging, crash records) */
#define SURF_DDR_TOP_SIZE 0x01000000u
#define SURF_IRQ_UART 0 /* interrupt controller source */
#define SURF_IRQ_PS2 1 /* interrupt controller source */
#define SURF_IRQ_GPIO 2 /* interrupt controller source */
#define SURF_IRQ_VIDEO 3 /* interrupt controller source */
#define SURF_IRQ_SPI 4 /* interrupt controller source */
#define SURF_IRQ_COUNT 5 /* sources in use */
#define SURF_UART_TXDATA 0x00000000u /* rw: Write: bits 7:0 go into the TX FIFO (dropped, setting STATUS.TX_OVF, when it is full). Read: bit 31 = TX FIFO full. */
#define SURF_UART_TXDATA_DATA_LSB 0 /* byte to send (write) */
#define SURF_UART_TXDATA_DATA_WIDTH 8
#define SURF_UART_TXDATA_DATA_MASK 0x000000FFu /* in place */
#define SURF_UART_TXDATA_FULL_LSB 31 /* TX FIFO full (read) */
#define SURF_UART_TXDATA_FULL_WIDTH 1
#define SURF_UART_TXDATA_FULL_MASK 0x80000000u /* in place */
#define SURF_UART_RXDATA 0x00000004u /* rc: Read pops the RX FIFO. EMPTY = 1 means there was nothing (DATA is then 0). */
#define SURF_UART_RXDATA_DATA_LSB 0 /* received byte */
#define SURF_UART_RXDATA_DATA_WIDTH 8
#define SURF_UART_RXDATA_DATA_MASK 0x000000FFu /* in place */
#define SURF_UART_RXDATA_EMPTY_LSB 31 /* RX FIFO was empty */
#define SURF_UART_RXDATA_EMPTY_WIDTH 1
#define SURF_UART_RXDATA_EMPTY_MASK 0x80000000u /* in place */
#define SURF_UART_STATUS 0x00000008u /* w1c: Live state and sticky errors; write 1 to clear a sticky bit. */
#define SURF_UART_STATUS_TX_FULL_LSB 0 /* TX FIFO full */
#define SURF_UART_STATUS_TX_FULL_WIDTH 1
#define SURF_UART_STATUS_TX_FULL_MASK 0x00000001u /* in place */
#define SURF_UART_STATUS_TX_IDLE_LSB 1 /* TX FIFO empty and the shifter idle (everything sent) */
#define SURF_UART_STATUS_TX_IDLE_WIDTH 1
#define SURF_UART_STATUS_TX_IDLE_MASK 0x00000002u /* in place */
#define SURF_UART_STATUS_RX_AVAIL_LSB 2 /* RX FIFO not empty */
#define SURF_UART_STATUS_RX_AVAIL_WIDTH 1
#define SURF_UART_STATUS_RX_AVAIL_MASK 0x00000004u /* in place */
#define SURF_UART_STATUS_RX_FULL_LSB 3 /* RX FIFO full */
#define SURF_UART_STATUS_RX_FULL_WIDTH 1
#define SURF_UART_STATUS_RX_FULL_MASK 0x00000008u /* in place */
#define SURF_UART_STATUS_RX_OVF_LSB 4 /* sticky: a byte arrived with the RX FIFO full and was lost */
#define SURF_UART_STATUS_RX_OVF_WIDTH 1
#define SURF_UART_STATUS_RX_OVF_MASK 0x00000010u /* in place */
#define SURF_UART_STATUS_RX_FRAME_LSB 5 /* sticky: a stop bit was 0 (that byte was dropped) */
#define SURF_UART_STATUS_RX_FRAME_WIDTH 1
#define SURF_UART_STATUS_RX_FRAME_MASK 0x00000020u /* in place */
#define SURF_UART_STATUS_TX_OVF_LSB 6 /* sticky: a write to TXDATA found the TX FIFO full */
#define SURF_UART_STATUS_TX_OVF_WIDTH 1
#define SURF_UART_STATUS_TX_OVF_MASK 0x00000040u /* in place */
#define SURF_UART_STATUS_RX_BREAK_LSB 7 /* sticky: a break (all-zero frame) was seen */
#define SURF_UART_STATUS_RX_BREAK_WIDTH 1
#define SURF_UART_STATUS_RX_BREAK_MASK 0x00000080u /* in place */
#define SURF_UART_CTRL 0x0000000Cu /* rw: Interrupt enables. */
#define SURF_UART_CTRL_RX_IE_LSB 0 /* interrupt while RX_AVAIL */
#define SURF_UART_CTRL_RX_IE_WIDTH 1
#define SURF_UART_CTRL_RX_IE_MASK 0x00000001u /* in place */
#define SURF_UART_CTRL_TX_IE_LSB 1 /* interrupt while the TX FIFO is empty */
#define SURF_UART_CTRL_TX_IE_WIDTH 1
#define SURF_UART_CTRL_TX_IE_MASK 0x00000002u /* in place */
#define SURF_UART_CTRL_ERR_IE_LSB 2 /* interrupt while any sticky error bit is set */
#define SURF_UART_CTRL_ERR_IE_WIDTH 1
#define SURF_UART_CTRL_ERR_IE_MASK 0x00000004u /* in place */
#define SURF_UART_DIV 0x00000010u /* rw: Bit time in sys_clk cycles (minimum 16). Baud = sys_hz / DIV. Reset: round(sys_hz / 115200). */
#define SURF_UART_DIV_DIV_LSB 0 /* cycles per bit */
#define SURF_UART_DIV_DIV_WIDTH 16
#define SURF_UART_DIV_DIV_MASK 0x0000FFFFu /* in place */
#define SURF_UART_LEVEL 0x00000014u /* ro: FIFO fill levels. */
#define SURF_UART_LEVEL_TX_LSB 0 /* bytes waiting in the TX FIFO */
#define SURF_UART_LEVEL_TX_WIDTH 8
#define SURF_UART_LEVEL_TX_MASK 0x000000FFu /* in place */
#define SURF_UART_LEVEL_RX_LSB 16 /* bytes waiting in the RX FIFO */
#define SURF_UART_LEVEL_RX_WIDTH 8
#define SURF_UART_LEVEL_RX_MASK 0x00FF0000u /* in place */
#define SURF_TIMER_MTIME_LO 0x00000000u /* rw: mtime bits 31:0 */
#define SURF_TIMER_MTIME_HI 0x00000004u /* rw: mtime bits 63:32 */
#define SURF_TIMER_MTIMECMP_LO 0x00000008u /* rw: mtimecmp bits 31:0 (reset 0xFFFFFFFF) */
#define SURF_TIMER_MTIMECMP_HI 0x0000000Cu /* rw: mtimecmp bits 63:32 (reset 0xFFFFFFFF) */
#define SURF_TIMER_MSIP 0x00000010u /* rw: Machine software interrupt pending: bit 0 drives mip.MSIP. */
#define SURF_TIMER_MSIP_MSIP_LSB 0 /* software interrupt */
#define SURF_TIMER_MSIP_MSIP_WIDTH 1
#define SURF_TIMER_MSIP_MSIP_MASK 0x00000001u /* in place */
#define SURF_TIMER_TIMEBASE 0x00000014u /* ro: mtime frequency in Hz (timebase_hz) */
#define SURF_INTC_PENDING 0x00000000u /* ro: Raw level of every source, enabled or not. */
#define SURF_INTC_ENABLE 0x00000004u /* rw: Per-source enable (reset 0). */
#define SURF_INTC_ACTIVE 0x00000008u /* ro: PENDING & ENABLE. */
#define SURF_INTC_NEXT 0x0000000Cu /* ro: Number of the lowest active source, or 0xFFFFFFFF when none is active. */
#define SURF_GPIO_LED 0x00000000u /* rw: Green LEDs LD4..LD7 (bit 0 = LD4). */
#define SURF_GPIO_LED_LED_LSB 0 /* 1 = on */
#define SURF_GPIO_LED_LED_WIDTH 4
#define SURF_GPIO_LED_LED_MASK 0x0000000Fu /* in place */
#define SURF_GPIO_RGB 0x00000004u /* rw: RGB LEDs LD0..LD3: LED n uses bits 3n (red), 3n+1 (green), 3n+2 (blue). */
#define SURF_GPIO_RGB_RGB_LSB 0 /* 1 = on */
#define SURF_GPIO_RGB_RGB_WIDTH 12
#define SURF_GPIO_RGB_RGB_MASK 0x00000FFFu /* in place */
#define SURF_GPIO_SW 0x00000008u /* ro: Slide switches SW0..SW3 (bit 0 = SW0), 1 = up. */
#define SURF_GPIO_SW_SW_LSB 0 /* switch state */
#define SURF_GPIO_SW_SW_WIDTH 4
#define SURF_GPIO_SW_SW_MASK 0x0000000Fu /* in place */
#define SURF_GPIO_BTN 0x0000000Cu /* ro: Push buttons BTN0..BTN3, debounced, 1 = pressed. */
#define SURF_GPIO_BTN_BTN_LSB 0 /* button state */
#define SURF_GPIO_BTN_BTN_WIDTH 4
#define SURF_GPIO_BTN_BTN_MASK 0x0000000Fu /* in place */
#define SURF_GPIO_BTN_EDGE 0x00000010u /* w1c: Latched presses (debounced rising edges); write 1 to clear. */
#define SURF_GPIO_BTN_EDGE_BTN_LSB 0 /* pressed since last cleared */
#define SURF_GPIO_BTN_EDGE_BTN_WIDTH 4
#define SURF_GPIO_BTN_EDGE_BTN_MASK 0x0000000Fu /* in place */
#define SURF_GPIO_IE 0x00000014u /* rw: Interrupt while a BTN_EDGE bit with its enable is set. */
#define SURF_GPIO_IE_BTN_LSB 0 /* per-button enable */
#define SURF_GPIO_IE_BTN_WIDTH 4
#define SURF_GPIO_IE_BTN_MASK 0x0000000Fu /* in place */
#define SURF_SPI_DATA 0x00000000u /* rw: Write: start exchanging bits 7:0 (ignored, setting STATUS.OVR, if a transfer is running). Read: the last byte received (waits for a running transfer). */
#define SURF_SPI_DATA_DATA_LSB 0 /* byte */
#define SURF_SPI_DATA_DATA_WIDTH 8
#define SURF_SPI_DATA_DATA_MASK 0x000000FFu /* in place */
#define SURF_SPI_STATUS 0x00000004u /* w1c: State; write 1 to clear a sticky bit. */
#define SURF_SPI_STATUS_BUSY_LSB 0 /* a transfer is running */
#define SURF_SPI_STATUS_BUSY_WIDTH 1
#define SURF_SPI_STATUS_BUSY_MASK 0x00000001u /* in place */
#define SURF_SPI_STATUS_CARD_LSB 1 /* card present (from the socket's card-detect switch; 1 if the adapter has none) */
#define SURF_SPI_STATUS_CARD_WIDTH 1
#define SURF_SPI_STATUS_CARD_MASK 0x00000002u /* in place */
#define SURF_SPI_STATUS_DONE_LSB 2 /* sticky: a transfer finished (interrupt source when CTRL.DONE_IE) */
#define SURF_SPI_STATUS_DONE_WIDTH 1
#define SURF_SPI_STATUS_DONE_MASK 0x00000004u /* in place */
#define SURF_SPI_STATUS_OVR_LSB 3 /* sticky: DATA was written during a transfer */
#define SURF_SPI_STATUS_OVR_WIDTH 1
#define SURF_SPI_STATUS_OVR_MASK 0x00000008u /* in place */
#define SURF_SPI_CTRL 0x00000008u /* rw: Chip select and interrupt enable. */
#define SURF_SPI_CTRL_CS_LSB 0 /* 1 = assert the card's chip select (drive CS low) */
#define SURF_SPI_CTRL_CS_WIDTH 1
#define SURF_SPI_CTRL_CS_MASK 0x00000001u /* in place */
#define SURF_SPI_CTRL_DONE_IE_LSB 1 /* interrupt while STATUS.DONE */
#define SURF_SPI_CTRL_DONE_IE_WIDTH 1
#define SURF_SPI_CTRL_DONE_IE_MASK 0x00000002u /* in place */
#define SURF_SPI_DIV 0x0000000Cu /* rw: SCLK = sys_hz / (2 * (DIV + 1)). Reset: the smallest DIV giving at most 400 kHz (identification speed). */
#define SURF_SPI_DIV_DIV_LSB 0 /* clock divider */
#define SURF_SPI_DIV_DIV_WIDTH 8
#define SURF_SPI_DIV_DIV_MASK 0x000000FFu /* in place */
#define SURF_PS2_DATA 0x00000000u /* rc: Read pops the FIFO. EMPTY = 1 means there was nothing. */
#define SURF_PS2_DATA_DATA_LSB 0 /* received byte (scan code set 2 from a keyboard) */
#define SURF_PS2_DATA_DATA_WIDTH 8
#define SURF_PS2_DATA_DATA_MASK 0x000000FFu /* in place */
#define SURF_PS2_DATA_EMPTY_LSB 31 /* FIFO was empty */
#define SURF_PS2_DATA_EMPTY_WIDTH 1
#define SURF_PS2_DATA_EMPTY_MASK 0x80000000u /* in place */
#define SURF_PS2_STATUS 0x00000004u /* w1c: State and sticky errors; write 1 to clear a sticky bit. */
#define SURF_PS2_STATUS_RX_AVAIL_LSB 0 /* FIFO not empty */
#define SURF_PS2_STATUS_RX_AVAIL_WIDTH 1
#define SURF_PS2_STATUS_RX_AVAIL_MASK 0x00000001u /* in place */
#define SURF_PS2_STATUS_RX_FULL_LSB 1 /* FIFO full */
#define SURF_PS2_STATUS_RX_FULL_WIDTH 1
#define SURF_PS2_STATUS_RX_FULL_MASK 0x00000002u /* in place */
#define SURF_PS2_STATUS_OVF_LSB 2 /* sticky: a byte was lost because the FIFO was full */
#define SURF_PS2_STATUS_OVF_WIDTH 1
#define SURF_PS2_STATUS_OVF_MASK 0x00000004u /* in place */
#define SURF_PS2_STATUS_PARITY_LSB 3 /* sticky: a frame failed odd parity (dropped) */
#define SURF_PS2_STATUS_PARITY_WIDTH 1
#define SURF_PS2_STATUS_PARITY_MASK 0x00000008u /* in place */
#define SURF_PS2_STATUS_FRAME_LSB 4 /* sticky: bad start or stop bit (dropped) */
#define SURF_PS2_STATUS_FRAME_WIDTH 1
#define SURF_PS2_STATUS_FRAME_MASK 0x00000010u /* in place */
#define SURF_PS2_STATUS_TIMEOUT_LSB 5 /* sticky: a partial frame was abandoned */
#define SURF_PS2_STATUS_TIMEOUT_WIDTH 1
#define SURF_PS2_STATUS_TIMEOUT_MASK 0x00000020u /* in place */
#define SURF_PS2_STATUS_TX_BUSY_LSB 6 /* a host-to-device byte is being sent */
#define SURF_PS2_STATUS_TX_BUSY_WIDTH 1
#define SURF_PS2_STATUS_TX_BUSY_MASK 0x00000040u /* in place */
#define SURF_PS2_STATUS_TX_NAK_LSB 7 /* sticky: the device did not acknowledge, or the send timed out */
#define SURF_PS2_STATUS_TX_NAK_WIDTH 1
#define SURF_PS2_STATUS_TX_NAK_MASK 0x00000080u /* in place */
#define SURF_PS2_CTRL 0x00000008u /* rw: Receiver enable and interrupt enable. */
#define SURF_PS2_CTRL_RX_EN_LSB 0 /* 1 = receive (reset 1); 0 ignores the lines (the keyboard is not inhibited) */
#define SURF_PS2_CTRL_RX_EN_WIDTH 1
#define SURF_PS2_CTRL_RX_EN_MASK 0x00000001u /* in place */
#define SURF_PS2_CTRL_RX_IE_LSB 1 /* interrupt while RX_AVAIL */
#define SURF_PS2_CTRL_RX_IE_WIDTH 1
#define SURF_PS2_CTRL_RX_IE_MASK 0x00000002u /* in place */
#define SURF_PS2_CTRL_ERR_IE_LSB 2 /* interrupt while a sticky error is set */
#define SURF_PS2_CTRL_ERR_IE_WIDTH 1
#define SURF_PS2_CTRL_ERR_IE_MASK 0x00000004u /* in place */
#define SURF_PS2_TXDATA 0x0000000Cu /* wo: Write bits 7:0 to send a command byte to the device (ignored while TX_BUSY). */
#define SURF_PS2_TXDATA_DATA_LSB 0 /* command byte */
#define SURF_PS2_TXDATA_DATA_WIDTH 8
#define SURF_PS2_TXDATA_DATA_MASK 0x000000FFu /* in place */
#define SURF_PS2_ERRCOUNT 0x00000010u /* ro: Saturating error counters since reset (diagnostics). */
#define SURF_PS2_ERRCOUNT_PARITY_LSB 0 /* parity errors */
#define SURF_PS2_ERRCOUNT_PARITY_WIDTH 8
#define SURF_PS2_ERRCOUNT_PARITY_MASK 0x000000FFu /* in place */
#define SURF_PS2_ERRCOUNT_FRAME_LSB 8 /* framing errors */
#define SURF_PS2_ERRCOUNT_FRAME_WIDTH 8
#define SURF_PS2_ERRCOUNT_FRAME_MASK 0x0000FF00u /* in place */
#define SURF_PS2_ERRCOUNT_TIMEOUT_LSB 16 /* abandoned frames */
#define SURF_PS2_ERRCOUNT_TIMEOUT_WIDTH 8
#define SURF_PS2_ERRCOUNT_TIMEOUT_MASK 0x00FF0000u /* in place */
#define SURF_PS2_ERRCOUNT_OVF_LSB 24 /* bytes lost to a full FIFO */
#define SURF_PS2_ERRCOUNT_OVF_WIDTH 8
#define SURF_PS2_ERRCOUNT_OVF_MASK 0xFF000000u /* in place */
#define SURF_VIDEO_CTRL 0x00000000u /* rw: Display mode. Reset: ENABLE=1, MODE=0 (test pattern), so a monitor shows bars before any software runs. */
#define SURF_VIDEO_CTRL_ENABLE_LSB 0 /* 0 = black picture (syncs keep running) */
#define SURF_VIDEO_CTRL_ENABLE_WIDTH 1
#define SURF_VIDEO_CTRL_ENABLE_MASK 0x00000001u /* in place */
#define SURF_VIDEO_CTRL_MODE_LSB 1 /* 0 = color-bar test pattern, 1 = text, 2..3 reserved (black) */
#define SURF_VIDEO_CTRL_MODE_WIDTH 2
#define SURF_VIDEO_CTRL_MODE_MASK 0x00000006u /* in place */
#define SURF_VIDEO_CTRL_CURSOR_EN_LSB 3 /* show the cursor */
#define SURF_VIDEO_CTRL_CURSOR_EN_WIDTH 1
#define SURF_VIDEO_CTRL_CURSOR_EN_MASK 0x00000008u /* in place */
#define SURF_VIDEO_CTRL_CURSOR_BLINK_LSB 4 /* blink it (about 2 Hz) */
#define SURF_VIDEO_CTRL_CURSOR_BLINK_WIDTH 1
#define SURF_VIDEO_CTRL_CURSOR_BLINK_MASK 0x00000010u /* in place */
#define SURF_VIDEO_STATUS 0x00000004u /* w1c: Frame state. */
#define SURF_VIDEO_STATUS_VBLANK_LSB 0 /* in vertical blanking now */
#define SURF_VIDEO_STATUS_VBLANK_WIDTH 1
#define SURF_VIDEO_STATUS_VBLANK_MASK 0x00000001u /* in place */
#define SURF_VIDEO_STATUS_VBLANK_EV_LSB 1 /* sticky: a vertical blanking interval began (interrupt source when IE.VBLANK) */
#define SURF_VIDEO_STATUS_VBLANK_EV_WIDTH 1
#define SURF_VIDEO_STATUS_VBLANK_EV_MASK 0x00000002u /* in place */
#define SURF_VIDEO_STATUS_FRAME_LSB 16 /* frame counter (wraps); counts when vertical blanking begins */
#define SURF_VIDEO_STATUS_FRAME_WIDTH 16
#define SURF_VIDEO_STATUS_FRAME_MASK 0xFFFF0000u /* in place */
#define SURF_VIDEO_CURSOR 0x00000008u /* rw: Cursor position on the screen (display coordinates, after SCROLL). */
#define SURF_VIDEO_CURSOR_COL_LSB 0 /* column 0..79 */
#define SURF_VIDEO_CURSOR_COL_WIDTH 7
#define SURF_VIDEO_CURSOR_COL_MASK 0x0000007Fu /* in place */
#define SURF_VIDEO_CURSOR_ROW_LSB 8 /* row 0..29 */
#define SURF_VIDEO_CURSOR_ROW_WIDTH 5
#define SURF_VIDEO_CURSOR_ROW_MASK 0x00001F00u /* in place */
#define SURF_VIDEO_SCROLL 0x0000000Cu /* rw: First text row shown at the top of the screen. */
#define SURF_VIDEO_SCROLL_ROW_LSB 0 /* 0..29 (values 30..31 act as 0) */
#define SURF_VIDEO_SCROLL_ROW_WIDTH 5
#define SURF_VIDEO_SCROLL_ROW_MASK 0x0000001Fu /* in place */
#define SURF_VIDEO_IE 0x00000010u /* rw: Interrupt enable. */
#define SURF_VIDEO_IE_VBLANK_LSB 0 /* interrupt while STATUS.VBLANK_EV */
#define SURF_VIDEO_IE_VBLANK_WIDTH 1
#define SURF_VIDEO_IE_VBLANK_MASK 0x00000001u /* in place */
#define SURF_VIDEO_INFO 0x00000014u /* ro: Text geometry: 80 columns, 30 rows, 8x16 glyphs. */
#define SURF_VIDEO_INFO_COLS_LSB 0 /* 80 */
#define SURF_VIDEO_INFO_COLS_WIDTH 8
#define SURF_VIDEO_INFO_COLS_MASK 0x000000FFu /* in place */
#define SURF_VIDEO_INFO_ROWS_LSB 8 /* 30 */
#define SURF_VIDEO_INFO_ROWS_WIDTH 8
#define SURF_VIDEO_INFO_ROWS_MASK 0x0000FF00u /* in place */
#define SURF_VIDEO_INFO_GLYPH_W_LSB 16 /* 8 */
#define SURF_VIDEO_INFO_GLYPH_W_WIDTH 4
#define SURF_VIDEO_INFO_GLYPH_W_MASK 0x000F0000u /* in place */
#define SURF_VIDEO_INFO_GLYPH_H_LSB 24 /* 16 */
#define SURF_VIDEO_INFO_GLYPH_H_WIDTH 5
#define SURF_VIDEO_INFO_GLYPH_H_MASK 0x1F000000u /* in place */
#define SURF_VIDEO_PIXCLK 0x00000018u /* ro: Actual pixel clock in Hz (clocks.pix_hz). */
#define SURF_VIDEO_PALETTE 0x00000040u /* rw: Palette entry n at 0x40 + 4n: RGB444 as bits 11:8 red, 7:4 green, 3:0 blue. Reset: the 16 CGA colors. */
#define SURF_VIDEO_PALETTE_COUNT 16 /* consecutive words */
#define SURF_VIDEO_PALETTE_B_LSB 0 /* blue */
#define SURF_VIDEO_PALETTE_B_WIDTH 4
#define SURF_VIDEO_PALETTE_B_MASK 0x0000000Fu /* in place */
#define SURF_VIDEO_PALETTE_G_LSB 4 /* green */
#define SURF_VIDEO_PALETTE_G_WIDTH 4
#define SURF_VIDEO_PALETTE_G_MASK 0x000000F0u /* in place */
#define SURF_VIDEO_PALETTE_R_LSB 8 /* red */
#define SURF_VIDEO_PALETTE_R_WIDTH 4
#define SURF_VIDEO_PALETTE_R_MASK 0x00000F00u /* in place */
#define SURF_SYSINFO_MAGIC 0x00000000u /* ro: 0x46525553, the bytes "SURF" in memory order. */
#define SURF_SYSINFO_SOC_VERSION 0x00000004u /* ro: soc.soc_version as major << 16 | minor << 8 | patch. */
#define SURF_SYSINFO_MAP_VERSION 0x00000008u /* ro: soc.map_version: which version of this memory map the hardware implements. */
#define SURF_SYSINFO_BOARD_ID 0x0000000Cu /* ro: A value from [boards]. */
#define SURF_SYSINFO_SYS_HZ 0x00000010u /* ro: sys_clk frequency in Hz. */
#define SURF_SYSINFO_BUILD_ID 0x00000014u /* ro: First 8 hex digits of the source commit the bitstream was built from. */
#define SURF_SYSINFO_BUILD_FLAGS 0x00000018u /* ro: Build properties. */
#define SURF_SYSINFO_BUILD_FLAGS_DIRTY_LSB 0 /* the source tree had uncommitted changes */
#define SURF_SYSINFO_BUILD_FLAGS_DIRTY_WIDTH 1
#define SURF_SYSINFO_BUILD_FLAGS_DIRTY_MASK 0x00000001u /* in place */
#define SURF_SYSINFO_BUILD_FLAGS_SIM_LSB 1 /* this is a simulation, not a bitstream */
#define SURF_SYSINFO_BUILD_FLAGS_SIM_WIDTH 1
#define SURF_SYSINFO_BUILD_FLAGS_SIM_MASK 0x00000002u /* in place */
#define SURF_SYSINFO_FEATURES 0x0000001Cu /* ro: Which blocks were built in. */
#define SURF_SYSINFO_FEATURES_UART_LSB 0
#define SURF_SYSINFO_FEATURES_UART_WIDTH 1
#define SURF_SYSINFO_FEATURES_UART_MASK 0x00000001u /* in place */
#define SURF_SYSINFO_FEATURES_TIMER_LSB 1
#define SURF_SYSINFO_FEATURES_TIMER_WIDTH 1
#define SURF_SYSINFO_FEATURES_TIMER_MASK 0x00000002u /* in place */
#define SURF_SYSINFO_FEATURES_IRQ_LSB 2
#define SURF_SYSINFO_FEATURES_IRQ_WIDTH 1
#define SURF_SYSINFO_FEATURES_IRQ_MASK 0x00000004u /* in place */
#define SURF_SYSINFO_FEATURES_GPIO_LSB 3
#define SURF_SYSINFO_FEATURES_GPIO_WIDTH 1
#define SURF_SYSINFO_FEATURES_GPIO_MASK 0x00000008u /* in place */
#define SURF_SYSINFO_FEATURES_SPI_LSB 4
#define SURF_SYSINFO_FEATURES_SPI_WIDTH 1
#define SURF_SYSINFO_FEATURES_SPI_MASK 0x00000010u /* in place */
#define SURF_SYSINFO_FEATURES_PS2_LSB 5
#define SURF_SYSINFO_FEATURES_PS2_WIDTH 1
#define SURF_SYSINFO_FEATURES_PS2_MASK 0x00000020u /* in place */
#define SURF_SYSINFO_FEATURES_VIDEO_LSB 6
#define SURF_SYSINFO_FEATURES_VIDEO_WIDTH 1
#define SURF_SYSINFO_FEATURES_VIDEO_MASK 0x00000040u /* in place */
#define SURF_SYSINFO_FEATURES_DDR_LSB 7
#define SURF_SYSINFO_FEATURES_DDR_WIDTH 1
#define SURF_SYSINFO_FEATURES_DDR_MASK 0x00000080u /* in place */
#define SURF_SYSINFO_DDR_STATUS 0x00000020u /* ro: DDR controller state. The DDR window answers with errors until CALIB_DONE. */
#define SURF_SYSINFO_DDR_STATUS_CALIB_DONE_LSB 0 /* calibration succeeded; DDR is usable */
#define SURF_SYSINFO_DDR_STATUS_CALIB_DONE_WIDTH 1
#define SURF_SYSINFO_DDR_STATUS_CALIB_DONE_MASK 0x00000001u /* in place */
#define SURF_SYSINFO_DDR_STATUS_CALIB_FAIL_LSB 1 /* sticky until reset: calibration did not finish within the bridge's limit (CALIB_TIMEOUT_CYCLES); CALIB_DONE then stays 0 */
#define SURF_SYSINFO_DDR_STATUS_CALIB_FAIL_WIDTH 1
#define SURF_SYSINFO_DDR_STATUS_CALIB_FAIL_MASK 0x00000002u /* in place */
#define SURF_SYSINFO_DDR_STATUS_PRESENT_LSB 2 /* a DDR controller is built in */
#define SURF_SYSINFO_DDR_STATUS_PRESENT_WIDTH 1
#define SURF_SYSINFO_DDR_STATUS_PRESENT_MASK 0x00000004u /* in place */
#define SURF_SYSINFO_DDR_STATUS_TIMEOUT_LSB 3 /* sticky until reset: a DDR access was abandoned by the bridge's watchdog */
#define SURF_SYSINFO_DDR_STATUS_TIMEOUT_WIDTH 1
#define SURF_SYSINFO_DDR_STATUS_TIMEOUT_MASK 0x00000008u /* in place */
#define SURF_SYSINFO_DDR_SIZE 0x00000024u /* ro: Bytes of DDR behind the window (0 when not present). */
#define SURF_SYSINFO_PIX_HZ 0x00000028u /* ro: Pixel clock in Hz (0 without video). */
#define SURF_SYSINFO_SCRATCH 0x0000002Cu /* rw: Free read/write word for bus tests (reset 0). */
#define SURF_SYSINFO_BUILD_TIME 0x00000030u /* ro: Build time, seconds since 1970-01-01 UTC (0 if unknown). */
#define SURF_IMAGE_MAGIC 0x474D4953u /* boot ABI */
#define SURF_IMAGE_HEADER_VERSION 0x00000001u /* boot ABI */
#define SURF_IMAGE_HEADER_SIZE 0x00000040u /* boot ABI */
#define SURF_BOOTINFO_MAGIC 0x49465253u /* boot ABI */
#define SURF_BOOTINFO_VERSION 0x00000001u /* boot ABI */
#define SURF_PLATFORM_ABI 0x00000001u /* boot ABI */
#define SURF_ISA_I 0x00000001u /* boot ABI */
#define SURF_ISA_M 0x00000002u /* boot ABI */
#define SURF_ISA_ZICSR 0x00000004u /* boot ABI */
#define SURF_ISA_ZIFENCEI 0x00000008u /* boot ABI */
#define SURF_BOOT_DEVICE_SD 0x00000001u /* boot ABI */

#endif /* SURFCPU_SOC_H */
