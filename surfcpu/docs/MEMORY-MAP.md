# SurfCPU memory map

<!-- Generated from spec/soc.toml by tools/socspec.py gen -- do not edit. -->

Memory map version **2**, SoC version **0.2.0**, ISA `rv32im_zicsr_zifencei`, ABI `ilp32`, reset PC `0x0000_0000`, one hart (mhartid 0). Little-endian. The source of truth is [`spec/soc.toml`](../spec/soc.toml); the Verilog, C and linker definitions are generated from it ([`rtl/generated/surfcpu_soc.vh`](../rtl/generated/surfcpu_soc.vh), [`firmware/include/surfcpu/soc.h`](../firmware/include/surfcpu/soc.h), [`firmware/ld/surfcpu_soc.ld`](../firmware/ld/surfcpu_soc.ld)).

Unmapped addresses, fetches from regions without *fetch*, and stores to regions without *write* get a SurfBus error, which the CPU raises as an access fault ([SURFBUS.md](SURFBUS.md)). Inside an MMIO window, an offset with no register also answers with an error. Registers are 32-bit and are accessed as aligned words.

## Clocks

| Clock | Hz |
|---|---|
| `osc_hz` | 100,000,000 |
| `sys_hz` | 25,000,000 |
| `timebase_hz` | 1,000,000 |
| `pix_hz_nominal` | 25,175,000 |
| `pix_hz` | 25,173,611 |

## Regions

| Base | End | Size | Name | Kind | Fetch | Write | Purpose |
|---|---|---|---|---|---|---|---|
| `0x0000_0000` | `0x0000_FFFF` | 64 KiB | `bootrom` | rom | yes | no | Boot ROM, 64 KiB of block RAM initialized from the SurfBoot build; reset vector at 0 |
| `0x0001_0000` | `0x0001_FFFF` | 64 KiB | `scratch` | ram | yes | yes | Scratch RAM, 64 KiB of block RAM: SurfBoot's data and stack, the boot info page, BRAM-only tests |
| `0x1000_0000` | `0x1000_0FFF` | 4 KiB | `uart` | mmio | no | yes | UART (115200 8N1 at reset) |
| `0x1000_1000` | `0x1000_1FFF` | 4 KiB | `timer` | mmio | no | yes | Machine timer: 64-bit mtime/mtimecmp, msip |
| `0x1000_2000` | `0x1000_2FFF` | 4 KiB | `intc` | mmio | no | yes | Interrupt controller: level sources masked into mip.MEIP |
| `0x1000_3000` | `0x1000_3FFF` | 4 KiB | `gpio` | mmio | no | yes | LEDs, RGB LEDs, switches, buttons |
| `0x1000_4000` | `0x1000_4FFF` | 4 KiB | `spi` | mmio | no | yes | SPI master for the microSD card |
| `0x1000_5000` | `0x1000_5FFF` | 4 KiB | `ps2` | mmio | no | yes | PS/2 keyboard port |
| `0x1000_6000` | `0x1000_6FFF` | 4 KiB | `video` | mmio | no | yes | VGA text display control, cursor, palette |
| `0x1000_7000` | `0x1000_7FFF` | 4 KiB | `sysinfo` | mmio | no | yes | Board and build identity, clocks, DDR status |
| `0x1000_8000` | `0x1000_8FFF` | 4 KiB | `eth` | reserved | no | no | Reserved for a later Ethernet MAC (decodes as unmapped until then) |
| `0x1000_A000` | `0x1000_BFFF` | 8 KiB | `ddrc` | mmio | no | yes | DDR controller registers, through which software initializes and calibrates the DDR3L when sysinfo.DDR_STATUS.SWINIT is 1 (LiteDRAM without its own CPU); unmapped (every access an error) when SWINIT is 0 |
| `0x2000_0000` | `0x2000_3FFF` | 16 KiB | `textram` | ram | no | yes | Text memory: 4096 32-bit cells; cells 0..2399 are the 80x30 screen (cell = row*80 + col) |
| `0x8000_0000` | `0x8FFF_FFFF` | 256 MiB | `ddr` | ddr | yes | yes | 256 MiB DDR3L; every access is an error until calibration has succeeded (sysinfo.DDR_STATUS) |

## Software reservations

Not decoded by hardware; SurfBoot and the kernel keep to them.

| Base | Size | Name | Purpose |
|---|---|---|---|
| `0x0001_0000` | 60 KiB | `surfboot_data` | SurfBoot .data/.bss, heap-free buffers and its stack (stack top = bootinfo base) |
| `0x0001_F000` | 4 KiB | `bootinfo` | struct surf_boot_info from SurfBoot; preserved until the kernel has copied it |
| `0x8000_0000` | 240 MiB | `kernel` | Where SURFOS.BIN payloads load by default (the image header decides); the kernel owns the rest of DDR |
| `0x8F00_0000` | 16 MiB | `ddr_top` | Top 16 MiB of DDR: kept free by SurfBoot (later display buffers, loader staging, crash records) |

## Interrupts

`mip.MTIP` comes from the timer (mtime >= mtimecmp), `mip.MSIP` from timer.MSIP, and `mip.MEIP` from the interrupt controller, which ORs these level sources:

| Source | Bit |
|---|---|
| uart | 0 |
| ps2 | 1 |
| gpio | 2 |
| video | 3 |
| spi | 4 |

## Boards

| Board | sysinfo.BOARD_ID |
|---|---|
| sim | `0x0000_00F0` |
| arty_a7_35t | `0x0000_0135` |
| arty_a7_100t | `0x0000_0100` |

## Registers

Access: ro read only, rw read/write, wo write only (reads 0), w1c write 1 to clear, rc reading has a side effect (pops a FIFO). Bits not listed read 0 and ignore writes.

### uart at `0x1000_0000`

8N1 UART. TX FIFO 16 bytes, RX FIFO 64 bytes. Reset divisor = round(sys_hz/115200). Interrupt source 0 (uart).

| Offset | Register | Access | Fields | Description |
|---|---|---|---|---|
| `0x000` | `TXDATA` | rw | 7:0 `DATA` byte to send (write)<br>31 `FULL` TX FIFO full (read) | Write: bits 7:0 go into the TX FIFO (dropped, setting STATUS.TX_OVF, when it is full). Read: bit 31 = TX FIFO full. |
| `0x004` | `RXDATA` | rc | 7:0 `DATA` received byte<br>31 `EMPTY` RX FIFO was empty | Read pops the RX FIFO. EMPTY = 1 means there was nothing (DATA is then 0). |
| `0x008` | `STATUS` | w1c | 0 `TX_FULL` TX FIFO full<br>1 `TX_IDLE` TX FIFO empty and the shifter idle (everything sent)<br>2 `RX_AVAIL` RX FIFO not empty<br>3 `RX_FULL` RX FIFO full<br>4 `RX_OVF` sticky: a byte arrived with the RX FIFO full and was lost<br>5 `RX_FRAME` sticky: a stop bit was 0 (that byte was dropped)<br>6 `TX_OVF` sticky: a write to TXDATA found the TX FIFO full<br>7 `RX_BREAK` sticky: a break (all-zero frame) was seen | Live state and sticky errors; write 1 to clear a sticky bit. |
| `0x00C` | `CTRL` | rw | 0 `RX_IE` interrupt while RX_AVAIL<br>1 `TX_IE` interrupt while the TX FIFO is empty<br>2 `ERR_IE` interrupt while any sticky error bit is set | Interrupt enables. |
| `0x010` | `DIV` | rw | 15:0 `DIV` cycles per bit | Bit time in sys_clk cycles (minimum 16). Baud = sys_hz / DIV. Reset: round(sys_hz / 115200). |
| `0x014` | `LEVEL` | ro | 7:0 `TX` bytes waiting in the TX FIFO<br>23:16 `RX` bytes waiting in the RX FIFO | FIFO fill levels. |

### timer at `0x1000_1000`

Machine timer. mtime counts at timebase_hz from reset. MTIP = (mtime >= mtimecmp), unsigned 64-bit. 64-bit values are read hi/lo/hi (retry if hi changed); mtimecmp is written lo=0xFFFFFFFF, hi, lo (no spurious interrupt).

| Offset | Register | Access | Fields | Description |
|---|---|---|---|---|
| `0x000` | `MTIME_LO` | rw | 31:0 | mtime bits 31:0 |
| `0x004` | `MTIME_HI` | rw | 31:0 | mtime bits 63:32 |
| `0x008` | `MTIMECMP_LO` | rw | 31:0 | mtimecmp bits 31:0 (reset 0xFFFFFFFF) |
| `0x00C` | `MTIMECMP_HI` | rw | 31:0 | mtimecmp bits 63:32 (reset 0xFFFFFFFF) |
| `0x010` | `MSIP` | rw | 0 `MSIP` software interrupt | Machine software interrupt pending: bit 0 drives mip.MSIP. |
| `0x014` | `TIMEBASE` | ro | 31:0 | mtime frequency in Hz (timebase_hz) |

### intc at `0x1000_2000`

Interrupt controller. mip.MEIP = OR of (PENDING & ENABLE). Sources are levels (see [irqs]); there is no claim/complete state.

| Offset | Register | Access | Fields | Description |
|---|---|---|---|---|
| `0x000` | `PENDING` | ro | 31:0 | Raw level of every source, enabled or not. |
| `0x004` | `ENABLE` | rw | 31:0 | Per-source enable (reset 0). |
| `0x008` | `ACTIVE` | ro | 31:0 | PENDING & ENABLE. |
| `0x00C` | `NEXT` | ro | 31:0 | Number of the lowest active source, or 0xFFFFFFFF when none is active. |

### gpio at `0x1000_3000`

Arty LEDs, RGB LEDs, switches, buttons. Inputs are synchronized; buttons are debounced (about 10 ms) before edge detection. RGB LEDs are driven at a fixed 1/8 duty cycle (they are too bright at full drive). Interrupt source 2 (gpio).

| Offset | Register | Access | Fields | Description |
|---|---|---|---|---|
| `0x000` | `LED` | rw | 3:0 `LED` 1 = on | Green LEDs LD4..LD7 (bit 0 = LD4). |
| `0x004` | `RGB` | rw | 11:0 `RGB` 1 = on | RGB LEDs LD0..LD3: LED n uses bits 3n (red), 3n+1 (green), 3n+2 (blue). |
| `0x008` | `SW` | ro | 3:0 `SW` switch state | Slide switches SW0..SW3 (bit 0 = SW0), 1 = up. |
| `0x00C` | `BTN` | ro | 3:0 `BTN` button state | Push buttons BTN0..BTN3, debounced, 1 = pressed. |
| `0x010` | `BTN_EDGE` | w1c | 3:0 `BTN` pressed since last cleared | Latched presses (debounced rising edges); write 1 to clear. |
| `0x014` | `IE` | rw | 3:0 `BTN` per-button enable | Interrupt while a BTN_EDGE bit with its enable is set. |

### spi at `0x1000_4000`

SPI master, mode 0 (CPOL=0, CPHA=0), MSB first, one byte per transfer, for the microSD card. Reading DATA while a transfer runs holds the bus until it ends (at most 16 x (DIV+1) sys_clk cycles), so write DATA then read DATA is the whole byte exchange. Interrupt source 4 (spi).

| Offset | Register | Access | Fields | Description |
|---|---|---|---|---|
| `0x000` | `DATA` | rw | 7:0 `DATA` byte | Write: start exchanging bits 7:0 (ignored, setting STATUS.OVR, if a transfer is running). Read: the last byte received (waits for a running transfer). |
| `0x004` | `STATUS` | w1c | 0 `BUSY` a transfer is running<br>1 `CARD` card present (from the socket's card-detect switch; 1 if the adapter has none)<br>2 `DONE` sticky: a transfer finished (interrupt source when CTRL.DONE_IE)<br>3 `OVR` sticky: DATA was written during a transfer | State; write 1 to clear a sticky bit. |
| `0x008` | `CTRL` | rw | 0 `CS` 1 = assert the card's chip select (drive CS low)<br>1 `DONE_IE` interrupt while STATUS.DONE | Chip select and interrupt enable. |
| `0x00C` | `DIV` | rw | 7:0 `DIV` clock divider | SCLK = sys_hz / (2 * (DIV + 1)). Reset: the smallest DIV giving at most 400 kHz (identification speed). |

### ps2 at `0x1000_5000`

PS/2 port (keyboard). Receives device-to-host frames into a 16-byte FIFO, checking start, odd parity and stop bits; a frame that stalls for 2 ms is abandoned. The host-to-device direction (TXDATA) drives clock and data only through open-drain outputs. Interrupt source 1 (ps2).

| Offset | Register | Access | Fields | Description |
|---|---|---|---|---|
| `0x000` | `DATA` | rc | 7:0 `DATA` received byte (scan code set 2 from a keyboard)<br>31 `EMPTY` FIFO was empty | Read pops the FIFO. EMPTY = 1 means there was nothing. |
| `0x004` | `STATUS` | w1c | 0 `RX_AVAIL` FIFO not empty<br>1 `RX_FULL` FIFO full<br>2 `OVF` sticky: a byte was lost because the FIFO was full<br>3 `PARITY` sticky: a frame failed odd parity (dropped)<br>4 `FRAME` sticky: bad start or stop bit (dropped)<br>5 `TIMEOUT` sticky: a partial frame was abandoned<br>6 `TX_BUSY` a host-to-device byte is being sent<br>7 `TX_NAK` sticky: the device did not acknowledge, or the send timed out | State and sticky errors; write 1 to clear a sticky bit. |
| `0x008` | `CTRL` | rw | 0 `RX_EN` 1 = receive (reset 1); 0 ignores the lines (the keyboard is not inhibited)<br>1 `RX_IE` interrupt while RX_AVAIL<br>2 `ERR_IE` interrupt while a sticky error is set | Receiver enable and interrupt enable. |
| `0x00C` | `TXDATA` | wo | 7:0 `DATA` command byte | Write bits 7:0 to send a command byte to the device (ignored while TX_BUSY). |
| `0x010` | `ERRCOUNT` | ro | 7:0 `PARITY` parity errors<br>15:8 `FRAME` framing errors<br>23:16 `TIMEOUT` abandoned frames<br>31:24 `OVF` bytes lost to a full FIFO | Saturating error counters since reset (diagnostics). |

### video at `0x1000_6000`

VGA 640x480 at about 60 Hz (800x525 total, negative syncs), RGB444. Text mode shows 80x30 cells of 8x16 glyphs from text RAM; a cell is glyph 7:0, foreground 11:8, background 15:12 (palette indices), bits 31:16 reserved (stored, not shown). Display row r shows text row (r + SCROLL) mod 30. Writes to CTRL, CURSOR, SCROLL and PALETTE take effect together at the start of the next frame. Interrupt source 3 (video).

| Offset | Register | Access | Fields | Description |
|---|---|---|---|---|
| `0x000` | `CTRL` | rw | 0 `ENABLE` 0 = black picture (syncs keep running)<br>2:1 `MODE` 0 = color-bar test pattern, 1 = text, 2..3 reserved (black)<br>3 `CURSOR_EN` show the cursor<br>4 `CURSOR_BLINK` blink it (about 2 Hz) | Display mode. Reset: ENABLE=1, MODE=0 (test pattern), so a monitor shows bars before any software runs. |
| `0x004` | `STATUS` | w1c | 0 `VBLANK` in vertical blanking now<br>1 `VBLANK_EV` sticky: a vertical blanking interval began (interrupt source when IE.VBLANK)<br>31:16 `FRAME` frame counter (wraps); counts when vertical blanking begins | Frame state. |
| `0x008` | `CURSOR` | rw | 6:0 `COL` column 0..79<br>12:8 `ROW` row 0..29 | Cursor position on the screen (display coordinates, after SCROLL). |
| `0x00C` | `SCROLL` | rw | 4:0 `ROW` 0..29 (values 30..31 act as 0) | First text row shown at the top of the screen. |
| `0x010` | `IE` | rw | 0 `VBLANK` interrupt while STATUS.VBLANK_EV | Interrupt enable. |
| `0x014` | `INFO` | ro | 7:0 `COLS` 80<br>15:8 `ROWS` 30<br>19:16 `GLYPH_W` 8<br>28:24 `GLYPH_H` 16 | Text geometry: 80 columns, 30 rows, 8x16 glyphs. |
| `0x018` | `PIXCLK` | ro | 31:0 | Actual pixel clock in Hz (clocks.pix_hz). |
| `0x040`..`0x07C` | `PALETTE` | rw | 3:0 `B` blue<br>7:4 `G` green<br>11:8 `R` red | Palette entry n at 0x40 + 4n: RGB444 as bits 11:8 red, 7:4 green, 3:0 blue. Reset: the 16 CGA colors. |

### sysinfo at `0x1000_7000`

Identity of the board and the build, clock rates and the DDR state. All read only except SCRATCH.

| Offset | Register | Access | Fields | Description |
|---|---|---|---|---|
| `0x000` | `MAGIC` | ro | 31:0 | 0x46525553, the bytes "SURF" in memory order. |
| `0x004` | `SOC_VERSION` | ro | 31:0 | soc.soc_version as major << 16 | minor << 8 | patch. |
| `0x008` | `MAP_VERSION` | ro | 31:0 | soc.map_version: which version of this memory map the hardware implements. |
| `0x00C` | `BOARD_ID` | ro | 31:0 | A value from [boards]. |
| `0x010` | `SYS_HZ` | ro | 31:0 | sys_clk frequency in Hz. |
| `0x014` | `BUILD_ID` | ro | 31:0 | First 8 hex digits of the source commit the bitstream was built from. |
| `0x018` | `BUILD_FLAGS` | ro | 0 `DIRTY` the source tree had uncommitted changes<br>1 `SIM` this is a simulation, not a bitstream | Build properties. |
| `0x01C` | `FEATURES` | ro | 0 `UART`<br>1 `TIMER`<br>2 `IRQ`<br>3 `GPIO`<br>4 `SPI`<br>5 `PS2`<br>6 `VIDEO`<br>7 `DDR` | Which blocks were built in. |
| `0x020` | `DDR_STATUS` | ro | 0 `CALIB_DONE` calibration succeeded; DDR is usable<br>1 `CALIB_FAIL` sticky until reset: calibration had not succeeded within the bridge's limit (CALIB_TIMEOUT_CYCLES, counted from reset, software initialization included); CALIB_DONE then stays 0<br>2 `PRESENT` a DDR controller is built in<br>3 `TIMEOUT` sticky until reset: a DDR access was abandoned by the bridge's watchdog<br>4 `SWINIT` the controller waits for software to initialize and calibrate the DDR3L through the ddrc registers (LiteDRAM without its own CPU); 0: it does so by itself (MIG, the simulation model) | DDR controller state. The DDR window answers with errors until CALIB_DONE. With SWINIT, the controller calibrates only when software initializes it through the ddrc registers (FIRMWARE.md); without, it calibrates by itself. |
| `0x024` | `DDR_SIZE` | ro | 31:0 | Bytes of DDR behind the window (0 when not present). |
| `0x028` | `PIX_HZ` | ro | 31:0 | Pixel clock in Hz (0 without video). |
| `0x02C` | `SCRATCH` | rw | 31:0 | Free read/write word for bus tests (reset 0). |
| `0x030` | `BUILD_TIME` | ro | 31:0 | Build time, seconds since 1970-01-01 UTC (0 if unknown). |

### ddrc at `0x1000_A000`

LiteDRAM's control and status registers (litedram_core generated with cpu None from ip/litedram/arty.yml), reached through surf_ddr_csr across the controller's clock domain: each access takes about half a microsecond. Offsets are LiteDRAM's (make litedram-check compares them with the generator's csr.csv); only the DFII and init registers are LiteDRAM's simulation core's too (its PHY model has none of the PHY_ ones). Present only while sysinfo.DDR_STATUS.SWINIT is 1. The sequence that uses them is firmware/common/ddrinit.c (FIRMWARE.md, DDR.md).

| Offset | Register | Access | Fields | Description |
|---|---|---|---|---|
| `0x000` | `INIT_DONE` | rw | 0 `DONE` initialization ended | ddrctrl.init_done: software writes 1 when initialization has ended; with INIT_ERROR 0 the controller's user port opens and DDR_STATUS.CALIB_DONE follows. |
| `0x004` | `INIT_ERROR` | rw | 0 `ERROR` initialization failed | ddrctrl.init_error: software writes 1 (with INIT_DONE) when initialization failed; the user port stays closed. |
| `0x800` | `PHY_RST` | rw | 31:0 | ddrphy.rst: 1 holds the PHY in reset. |
| `0x804` | `PHY_DLY_SEL` | rw | 31:0 | ddrphy.dly_sel: one bit per byte lane (bit 0 DQ7:0, bit 1 DQ15:8): the lanes the delay and bitslip strobes act on. |
| `0x808` | `PHY_HALF_SYS8X_TAPS` | rw | 31:0 | ddrphy.half_sys8x_taps: delay taps of a quarter controller clock (write leveling; unused by the read-leveling sequence). |
| `0x80C` | `PHY_WLEVEL_EN` | rw | 31:0 | ddrphy.wlevel_en: write-leveling mode (unused: the Arty's PHY is not write-leveling capable). |
| `0x810` | `PHY_WLEVEL_STROBE` | rw | 31:0 | ddrphy.wlevel_strobe: write-leveling strobe (unused). |
| `0x814` | `PHY_RDLY_DQ_RST` | rw | 31:0 | ddrphy.rdly_dq_rst: writing 1 sets the selected lanes' read delays (IDELAYE2) back to tap 0. |
| `0x818` | `PHY_RDLY_DQ_INC` | rw | 31:0 | ddrphy.rdly_dq_inc: writing 1 moves the selected lanes' read delays one tap up. |
| `0x81C` | `PHY_RDLY_DQ_BITSLIP_RST` | rw | 31:0 | ddrphy.rdly_dq_bitslip_rst: writing 1 sets the selected lanes' read bitslip back to 0. |
| `0x820` | `PHY_RDLY_DQ_BITSLIP` | rw | 31:0 | ddrphy.rdly_dq_bitslip: writing 1 moves the selected lanes' read bitslip one step on (8 steps, then round). |
| `0x824` | `PHY_WDLY_DQ_BITSLIP_RST` | rw | 31:0 | ddrphy.wdly_dq_bitslip_rst: writing 1 sets the selected lanes' write bitslip back to 0. |
| `0x828` | `PHY_WDLY_DQ_BITSLIP` | rw | 31:0 | ddrphy.wdly_dq_bitslip: writing 1 moves the selected lanes' write bitslip one step on. |
| `0x82C` | `PHY_RDPHASE` | rw | 31:0 | ddrphy.rdphase: the DFI phase reads are issued on (the generator's SDRAM_PHY_RDPHASE). |
| `0x830` | `PHY_WRPHASE` | rw | 31:0 | ddrphy.wrphase: the DFI phase writes are issued on (the generator's SDRAM_PHY_WRPHASE). |
| `0x1000` | `DFII_CONTROL` | rw | 0 `SEL` 1: the controller (hardware control); 0: software, through the phase injectors below<br>1 `CKE` CKE while under software control<br>2 `ODT` ODT while under software control<br>3 `RESET_N` the DRAM's RESET# (1 = released) while under software control | sdram.dfii_control: who drives the DRAM, and its control lines while software does. |
| `0x1004` | `DFII_P0_COMMAND` | rw | 0 `CS` chip select<br>1 `WE` WE (active: the command's WE# is low)<br>2 `CAS` CAS (active: CAS# low)<br>3 `RAS` RAS (active: RAS# low)<br>4 `WRDATA` the command writes WRDATA<br>5 `RDDATA` the command's read data is captured into RDDATA | sdram.dfii_pi0_command: the command bits of phase 0's next injected command. |
| `0x1008` | `DFII_P0_COMMAND_ISSUE` | rw | 31:0 | sdram.dfii_pi0_command_issue: writing 1 issues phase 0's command (in the controller's next cycle). |
| `0x100C` | `DFII_P0_ADDRESS` | rw | 31:0 | sdram.dfii_pi0_address: the row or column address (or mode register value) of phase 0's command. |
| `0x1010` | `DFII_P0_BADDRESS` | rw | 31:0 | sdram.dfii_pi0_baddress: the bank address (or mode register number) of phase 0's command. |
| `0x1014` | `DFII_P0_WRDATA` | rw | 31:0 | sdram.dfii_pi0_wrdata: phase 0's write data, 32 bits: DQ15:0 on the first edge in bits 15:0, on the second in bits 31:16 (byte k of a little-endian word: lane k mod 2). |
| `0x1018` | `DFII_P0_RDDATA` | ro | 31:0 | sdram.dfii_pi0_rddata: phase 0's data from the last injected read, in WRDATA's layout. |
| `0x101C` | `DFII_P1_COMMAND` | rw | 0 `CS` chip select<br>1 `WE` WE (active: the command's WE# is low)<br>2 `CAS` CAS (active: CAS# low)<br>3 `RAS` RAS (active: RAS# low)<br>4 `WRDATA` the command writes WRDATA<br>5 `RDDATA` the command's read data is captured into RDDATA | sdram.dfii_pi1_command: the command bits of phase 1's next injected command. |
| `0x1020` | `DFII_P1_COMMAND_ISSUE` | rw | 31:0 | sdram.dfii_pi1_command_issue: writing 1 issues phase 1's command (in the controller's next cycle). |
| `0x1024` | `DFII_P1_ADDRESS` | rw | 31:0 | sdram.dfii_pi1_address: the row or column address (or mode register value) of phase 1's command. |
| `0x1028` | `DFII_P1_BADDRESS` | rw | 31:0 | sdram.dfii_pi1_baddress: the bank address (or mode register number) of phase 1's command. |
| `0x102C` | `DFII_P1_WRDATA` | rw | 31:0 | sdram.dfii_pi1_wrdata: phase 1's write data, 32 bits: DQ15:0 on the first edge in bits 15:0, on the second in bits 31:16 (byte k of a little-endian word: lane k mod 2). |
| `0x1030` | `DFII_P1_RDDATA` | ro | 31:0 | sdram.dfii_pi1_rddata: phase 1's data from the last injected read, in WRDATA's layout. |
| `0x1034` | `DFII_P2_COMMAND` | rw | 0 `CS` chip select<br>1 `WE` WE (active: the command's WE# is low)<br>2 `CAS` CAS (active: CAS# low)<br>3 `RAS` RAS (active: RAS# low)<br>4 `WRDATA` the command writes WRDATA<br>5 `RDDATA` the command's read data is captured into RDDATA | sdram.dfii_pi2_command: the command bits of phase 2's next injected command. |
| `0x1038` | `DFII_P2_COMMAND_ISSUE` | rw | 31:0 | sdram.dfii_pi2_command_issue: writing 1 issues phase 2's command (in the controller's next cycle). |
| `0x103C` | `DFII_P2_ADDRESS` | rw | 31:0 | sdram.dfii_pi2_address: the row or column address (or mode register value) of phase 2's command. |
| `0x1040` | `DFII_P2_BADDRESS` | rw | 31:0 | sdram.dfii_pi2_baddress: the bank address (or mode register number) of phase 2's command. |
| `0x1044` | `DFII_P2_WRDATA` | rw | 31:0 | sdram.dfii_pi2_wrdata: phase 2's write data, 32 bits: DQ15:0 on the first edge in bits 15:0, on the second in bits 31:16 (byte k of a little-endian word: lane k mod 2). |
| `0x1048` | `DFII_P2_RDDATA` | ro | 31:0 | sdram.dfii_pi2_rddata: phase 2's data from the last injected read, in WRDATA's layout. |
| `0x104C` | `DFII_P3_COMMAND` | rw | 0 `CS` chip select<br>1 `WE` WE (active: the command's WE# is low)<br>2 `CAS` CAS (active: CAS# low)<br>3 `RAS` RAS (active: RAS# low)<br>4 `WRDATA` the command writes WRDATA<br>5 `RDDATA` the command's read data is captured into RDDATA | sdram.dfii_pi3_command: the command bits of phase 3's next injected command. |
| `0x1050` | `DFII_P3_COMMAND_ISSUE` | rw | 31:0 | sdram.dfii_pi3_command_issue: writing 1 issues phase 3's command (in the controller's next cycle). |
| `0x1054` | `DFII_P3_ADDRESS` | rw | 31:0 | sdram.dfii_pi3_address: the row or column address (or mode register value) of phase 3's command. |
| `0x1058` | `DFII_P3_BADDRESS` | rw | 31:0 | sdram.dfii_pi3_baddress: the bank address (or mode register number) of phase 3's command. |
| `0x105C` | `DFII_P3_WRDATA` | rw | 31:0 | sdram.dfii_pi3_wrdata: phase 3's write data, 32 bits: DQ15:0 on the first edge in bits 15:0, on the second in bits 31:16 (byte k of a little-endian word: lane k mod 2). |
| `0x1060` | `DFII_P3_RDDATA` | ro | 31:0 | sdram.dfii_pi3_rddata: phase 3's data from the last injected read, in WRDATA's layout. |

## Boot ABI constants

Used by [BOOT-ABI.md](BOOT-ABI.md).

| Name | Value |
|---|---|
| `SURF_IMAGE_MAGIC` | `0x474D_4953` |
| `SURF_IMAGE_HEADER_VERSION` | `0x0000_0001` |
| `SURF_IMAGE_HEADER_SIZE` | `0x0000_0040` |
| `SURF_BOOTINFO_MAGIC` | `0x4946_5253` |
| `SURF_BOOTINFO_VERSION` | `0x0000_0001` |
| `SURF_PLATFORM_ABI` | `0x0000_0001` |
| `SURF_ISA_I` | `0x0000_0001` |
| `SURF_ISA_M` | `0x0000_0002` |
| `SURF_ISA_ZICSR` | `0x0000_0004` |
| `SURF_ISA_ZIFENCEI` | `0x0000_0008` |
| `SURF_BOOT_DEVICE_SD` | `0x0000_0001` |
