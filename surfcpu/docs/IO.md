# SurfCPU I/O: microSD over SPI, the PS/2 keyboard, VGA text

The io track's part of milestone 6 (brief sections 6, 8 and 10): three SurfBus peripherals,
the simulation models that stand in for the card, the keyboard and the monitor, and their
tests. Registers are those of [MEMORY-MAP.md](MEMORY-MAP.md) (spi, ps2, video), ports and
model interfaces those of [MODULES.md](MODULES.md) sections 3 and 7.

**State (2026-10-15).** Simulation: all tests pass in Verilator 5.032 on five seeds, and the
three blocks also run in Icarus 12 ([evidence/io/tests.txt](evidence/io/tests.txt)).
Synthesis: Yosys `synth_xilinx` estimates only ([evidence/io/synth.txt](evidence/io/synth.txt));
no Vivado run, no bitstream. On the board: **pending** (no board, adapters, card, keyboard or
monitor are connected; INVENTORY.md gaps G1, G4, G5, G6, G8).

| Path | What |
|---|---|
| `rtl/peripherals/surf_spi.v` | SPI master for the microSD card |
| `rtl/peripherals/surf_ps2.v` | PS/2 port: receiver, FIFO, host-to-device sender |
| `rtl/peripherals/surf_video.v` | VGA 640x480 timing, test pattern, 80x30 text, cursor, palette |
| `rtl/peripherals/surf_video_font.v` | the font ROM, generated (do not edit) |
| `tools/fontrom.py` | the font ROM generator and checker |
| `sim/models/sdcard_model.{h,cpp}` | SPI-mode SD card |
| `sim/models/ps2_keyboard_model.{h,cpp}` | PS/2 keyboard |
| `sim/models/vga_capture.{h,cpp}` | VGA monitor: timing, frames, PPM, text decoding |
| `sim/io/` | testbenches, the SurfBus master BFM, the test SD host, `ppm2png.py`, `evidence.sh` |
| `mk/area-io.mk` | make targets (section 6) |

## 1. surf_spi: the microSD card's SPI port

Mode 0 (SCLK idles low; both sides sample on the rising edge and change on the falling
edge), MSB first, one byte per transfer, chip select by software (CTRL.CS).

* **Clock.** SCLK = SYS_HZ / (2 x (DIV + 1)); each half period is DIV + 1 clk cycles and a
  byte takes exactly 16 x (DIV + 1) cycles. At 25 MHz: DIV 31 (the reset value, the smallest
  giving at most 400 kHz) = 390.625 kHz for identification; DIV 0 = 12.5 MHz, DIV 1 = 6.25
  MHz for data (default-speed cards allow 25 MHz). The DIV in force is copied when a transfer
  starts: writing DIV during a transfer changes the next one.
* **Exchanging a byte.** Write DATA: MOSI shows bit 7 at once, half a period before the first
  rising edge. Read DATA: the read is accepted at once and answered when the transfer ends
  with the byte received, so firmware does `DATA = b; r = DATA;`. Only while such a read waits
  is `s_req_ready` low. Reading DATA when idle returns the last byte at once. A DATA write
  during a transfer is ignored and sets STATUS.OVR.
* **Idle levels.** Between transfers MOSI is high (the card sees 0xFF filler) and SCLK low. CS
  changes only when software writes CTRL; the tests check it never changes with SCLK high
  when used as firmware does.
* **MISO** is sampled on the clk edge that raises SCLK, half a period after the card changed it
  on the previous falling edge. It is synchronous to our own SCLK, so it has no synchronizer;
  at DIV 0 the round trip (SCLK out, card output delay, MISO in) must fit in one 40 ns clk
  period. The board track constrains this path (input delay relative to sys_clk).
* **Card detect** `cd_n` (low = card present, tie 0 without a switch) goes through
  `surf_sync`; STATUS.CARD = not cd_n.
* **Interrupt** while STATUS.DONE and CTRL.DONE_IE. DONE and OVR are write-1-to-clear; an event
  in the same cycle as the clear wins.
* **Bus.** One-cycle answers except the waiting DATA read; unknown offsets, byte enables other
  than 1111 and fetches get an error response and change nothing.

The SD protocol itself (CMD0, CMD8, ACMD41, CMD58, CMD17/18/24/25, tokens, busy, CRC) is
firmware's job; `sim/io/sd_host.h` is a small C++ version of it used by the tests, and a
useful reference for SurfBoot.

## 2. surf_ps2: the keyboard port

Both lines are open drain: the block has only `ps2_clk_oe` and `ps2_data_oe` (1 pulls the line
low). The board top maps each to an IOBUF with I = 0 and T = not oe; the Pmod PS2's pull-ups
make the high level. It never drives a line high.

**Receiving (device to host).**
* Both lines go through `surf_sync`, then a glitch filter: a level must hold for 2 us (50 clk
  cycles at 25 MHz) before it counts. PS/2 clock phases are 30-50 us, so the filter costs
  nothing and removes spikes and the ringing of slow edges (a 0.2 us spike in mid-frame is
  tested).
* On each falling edge of the filtered clock the filtered data is sampled: start 0, d0..d7,
  odd parity, stop 1.
* A good byte enters the 16-byte FIFO (`surf_fifo`); with the FIFO full it is lost and counted
  (STATUS.OVF, ERRCOUNT.OVF). A bad stop bit is FRAME, a bad parity PARITY; the byte is
  dropped. A start bit of 1 is refused at its first edge (FRAME) and the receiver stays idle,
  so a stop bit mistaken for a start bit after a lost edge puts it back in step at the next
  frame.
* A frame with no falling edge for 2 ms is abandoned (TIMEOUT). This is the general recovery:
  any loss of step (noise, enabling the receiver mid-frame) ends at the next pause in typing.
* ERRCOUNT holds four saturating 8-bit counters since reset. STATUS stickies are
  write-1-to-clear.
* CTRL.RX_EN = 0 makes the receiver ignore the lines (frames sent meanwhile are lost; the
  keyboard is not inhibited).
* Interrupt: (RX_IE and FIFO not empty) or (ERR_IE and any sticky error, TX_NAK included).

**Sending (host to device), TXDATA.**

| Step | Lines | Time |
|---|---|---|
| inhibit | clock pulled low | 125 us |
| request to send | data pulled low (start bit), clock still low | 5 us |
| release | clock released; the device clocks from now on | device starts within 15 ms |
| bits | after each falling edge of the device's clock: d0..d7, odd parity, then data released (stop) | 10 edges |
| acknowledge | at the 11th falling edge the device must hold data low | |
| end | both lines back high | the whole frame within 2 ms |

No clock edge within 15 ms, a frame longer than 2 ms, or data high at the 11th edge sets
STATUS.TX_NAK and releases both lines. TX_BUSY is set throughout; TXDATA writes while busy
are ignored. The clock is pulled low only for the 130 us of each send, never toggled by the
host. Data changes only while the clock line is low (the tests check every change; the one
exception is letting data go after a send nobody clocked). While a
byte is being sent the receiver rests; a frame the keyboard was sending is dropped, and the
keyboard sends it again after the inhibit (the protocol's rule; tested). The device's answer
(0xFA, or 0xFE for a resend) arrives as an ordinary received byte. There is no "send done"
interrupt: software polls TX_BUSY (a send takes about 1.1 ms plus the keyboard's delay in
starting its clock, at most 15 ms).

Scan code set 2 decoding (make/break, E0/E1, Shift/Ctrl/Alt, typematic) is software's job:
the os track's keyboard driver. `decode_set2()` in `sim/io/test_ps2.cpp` is a minimal example.

**Electrical (pending the owner).** A PS/2 keyboard is a 5 V device; the FPGA's pins are
3.3 V. Before connecting one, confirm the Pmod PS2's supply jumper and that its signals stay
within 3.3 V (INVENTORY.md G4, the board track's WIRING.md).

## 3. surf_video: VGA text display

**Timing** (from `pix_clk`; checked by the capture to the pixel):

| | Visible | Front porch | Sync | Back porch | Total |
|---|---|---|---|---|---|
| Horizontal (pixels) | 640 | 16 | 96 (negative) | 48 | 800 |
| Vertical (lines) | 480 | 10 | 2 (negative) | 33 | 525 |

With pix_clk = 25,177,305 Hz (`SURF_PIX_HZ`, 100 MHz x 8.875 / 35.25 from the board track's
MMCM) the line rate is 31.4716 kHz and the **refresh rate 59.9460 Hz** (the standard's 25.175
MHz gives 59.9405 Hz). These rates are computed from the measured totals and the nominal
clock; the clock itself is the board track's to verify.

**Modes** (CTRL): MODE 0 the colour-bar test pattern (the reset mode, so a monitor shows a
picture before any software runs), MODE 1 text, MODE 2 and 3 black; ENABLE = 0 black. The
syncs always run.

**The test pattern.** A one-pixel white border round the picture (to see its edges on a
monitor); above line 360 eight 80-pixel bars, white, yellow, cyan, green, magenta, red, blue,
black; below, four 30-line bands of sixteen 40-pixel steps of grey, red, green and blue (every
level of each 4-bit channel).

**Text mode.** 80 x 30 cells of 8 x 16 pixels from text RAM cells 0..2399 (cell = row * 80 +
col; glyph 7:0, foreground 11:8, background 15:12 as palette indices; 31:16 ignored). Display
row r shows text row (r + SCROLL) mod 30 (SCROLL 30 and 31 act as 0), so scrolling the screen
is: write the new bottom row, add 1 to SCROLL. The cursor is drawn in the cell's foreground
colour on glyph rows 14 and 15 at CURSOR (display coordinates, after SCROLL); a position
outside 80 x 30 shows none. CURSOR_BLINK blinks it with a period of 32 frames (16 on, 16 off:
1.87 Hz). The palette has 16 RGB444 entries, the CGA colours at reset:

| 0 black 000 | 1 blue 00A | 2 green 0A0 | 3 cyan 0AA | 4 red A00 | 5 magenta A0A | 6 brown A50 | 7 light grey AAA |
|---|---|---|---|---|---|---|---|
| **8 dark grey 555** | **9 light blue 55F** | **10 light green 5F5** | **11 light cyan 5FF** | **12 light red F55** | **13 light magenta F5F** | **14 yellow FF5** | **15 white FFF** |

**Frames and the interrupt.** STATUS.VBLANK is high during lines 480..524 (it reaches the sys
domain through `surf_sync`, a few cycles late). At the start of line 480 a pulse sets
STATUS.VBLANK_EV (the interrupt with IE.VBLANK) and adds 1 to STATUS.FRAME. CTRL, CURSOR and
SCROLL are taken once per frame, at the start of line 524, so a frame is drawn with one set
of values and software can change them any time during vertical blanking; palette writes
apply as they arrive.

**Scan-out pipeline**, one pixel per pix_clk. The counters run four cycles ahead of the pins
and the syncs go through the same four stages, so pixels and syncs line up exactly:

| Stage | Work |
|---|---|
| S0 | hcnt, vcnt; visible?, syncs; cell = ((row + SCROLL) mod 30) x 80 + col; cursor hit; pattern colour |
| S1 | `text_addr` registered: the text RAM reads the cell |
| S2 | `text_data` (the RAM's registered word, one pix_clk after text_addr): font ROM address {glyph, glyph row}; colours latched |
| S3 | font row (registered ROM output): pixel bit, palette lookup or pattern colour or black |
| S4 | `vga_r/g/b`, `vga_hs`, `vga_vs` registered |

**Clock crossings** (all through `rtl/lib`; the board track writes their constraints):

| Signal | Direction | Block | Notes |
|---|---|---|---|
| CTRL, CURSOR, SCROLL, palette (214 bits) | sys to pix | `surf_cdc_word` | one word, never torn; `hold` to `dst_data` needs `set_max_delay -datapath_only` |
| vertical blanking level | pix to sys | `surf_sync` | from a pix-domain register |
| start of vertical blanking | pix to sys | `surf_pulse_sync` | one pulse per frame |
| text RAM pixel port | pix only | (`surf_textram`, dual clock) | written from the sys side by the CPU |

Either domain may be reset alone (the pixel MMCM losing lock, a sys reset): the tests show the
picture restarts with its settings, and a sys reset leaves the syncs undisturbed. Resetting
one side of `surf_pulse_sync` can add one spurious count to FRAME (rtl/lib's documented
behaviour).

**The font ROM** (`surf_video_font.v`, 4096 x 8, registered read, one RAMB36) holds the 256
glyphs of code page 437 in the order of `cp437_unicode[]` in `driver/cp437.c`: Spleen 8x16
(`driver/fonts/spleen-8x16.bdf`) with SurfOS's own symbols (`driver/fonts/surfos-symbols.bdf`)
for the four Spleen lacks (0x10, 0x11, 0x1C, 0xF7); 0x00 is blank. `tools/fontrom.py gen`
writes it with its own BDF reader; every source is pinned by SHA-256, so a changed font stops
the build until the pin is updated on purpose. `fontrom.py check` (part of `make check`)
confirms the sources, that the ROM is current and that all 256 glyphs equal the x86 console's
`driver/font8x16.c` byte for byte. The ROM's header carries Spleen's BSD 2-Clause licence
(`driver/fonts/LICENSE-spleen`).

**Resources** (Yosys 0.52 `synth_xilinx`, an estimate; `make synth-io`):

| Block | LUTs | Flip-flops | LUTRAM | Block RAM |
|---|---|---|---|---|
| surf_spi | 83 | 70 | 0 | 0 |
| surf_ps2 | 209 | 189 | 2 (FIFO) | 0 |
| surf_video | 848 | 862 | 0 | 1 RAMB36 (font) |

Most of surf_video's flip-flops are the 214-bit settings crossing (hold and destination
copies) and the palette. The text RAM is the soc track's (`surf_textram`, 16 KiB).

## 4. The simulation models

All three are C++17 classes without any Verilator dependency, ticked once per clock by the
harness (MODULES.md section 7).

**`SdCardModel(image_path, options)`; `bool tick(cs_n, sclk, mosi)` returns MISO.** An SD card
in SPI mode reacting to the SCLK edges it sees:
* power-up: commands are ignored until 74 clocks with CS high and MOSI high;
* only CMD0 with a good CRC and CS low enters SPI mode (R1 0x01); then CMD8 (R7; its CRC is
  always checked; an SD v1 card answers "illegal"), CMD55 + ACMD41 (idle for `acmd41_busy`
  calls; an SDHC card never becomes ready without CMD8 and HCS), CMD58 (R3, OCR 0x00FF8000
  plus the power-up and CCS bits), CMD59 (CRC checking on/off), CMD9/CMD10 (CSD v2.0 for
  SDHC, v1.0 for SDSC with the image's capacity; CID with product name SURFC), CMD13 (R2,
  errors since the last CMD13), CMD16, CMD17, CMD18 (blocks until CMD12, which costs a stuff
  byte and an R1b), CMD24 and CMD25 (tokens 0xFE / 0xFC / stop 0xFD; data response 0xE5
  accepted, 0xEB CRC error, 0xED write error; then busy), ACMD23, ACMD51 (SCR); commands not
  legal in the idle state answer 0x05, unknown ones 0x04;
* errors: address beyond the card: R1 0x40 (parameter error); SDSC read crossing a block or
  misaligned write: 0x20 (address error); a CMD18 that runs off the end gets the data error
  token 0x08;
* timing: NCR filler bytes before a response and NAC before each data token are seeded random
  within `ncr_min..ncr_max` and `nac_min..nac_max`; busy after a write lasts
  `write_busy_min..max` clk cycles of time (whether the host clocks or not), after CMD12 and
  the stop token `stop_busy`;
* SDHC block addressing or SDSC byte addressing with CMD16 block lengths (partial reads);
* `present = false` or `set_present()`: no card (MISO pulled up, `cd_n()` high), removal loses
  the card's state;
* `faults`: one-shot no response, command CRC error, read error token, bad read CRC, write
  CRC rejection, write error, extra busy, initialization that never finishes;
* storage: the image file is opened read-only; written blocks stay in memory (the backdoor
  `read_block` sees them) until `flush()` writes them back, or at once with `write_through`;
  an empty path gives a blank card in memory. Nothing writes a device;
* host mistakes are counted in `stats.violations` and logged (a command while busy, a data
  token before the response was read, CS released inside a block...).

**`Ps2KeyboardModel(sys_hz, ps2_hz)`; `tick(clk_oe, data_oe, clk_line, data_line)`.** A
keyboard on the open-drain lines (the lines are low when either side pulls):
* `type_text()` (US layout, scan code set 2 make and break with Shift), `key_make/break/tap`
  for every key by name (letters, digits, punctuation, Enter, Backspace, Tab, Esc, the
  modifiers, arrows, Home/End/PgUp/PgDn/Insert/Delete, F1-F12, the keypad, Print Screen,
  Pause), `key_hold()` for typematic repeat, `queue_bytes()`, `queue_frame(byte, fault)`;
* frames on its own clock at `ps2_hz`: data changes a quarter period before each falling edge;
  a frame starts after 50 us of idle bus; if the host inhibits before the stop bit's clock
  pulse the frame is abandoned and sent again;
* host commands: request to send detected after an inhibit, eleven clock pulses, acknowledge,
  then the answer (0xFA; ED/F3/F0 take an argument; FF gives FA then AA after `bat_delay_us`;
  F2 gives FA AB 83; EE echoes; FE resends; a bad parity gets FE); `leds`, `host_bytes`;
* faults: bad parity, start or stop bit, a partial frame followed by a 3 ms pause, a 0.2 us
  clock glitch, `no_ack`, `present = false`;
* it checks the host: inhibit shorter than 100 us, the clock pulled while the keyboard clocks,
  data changed while the clock is high (`stats.violations`, `log`).

**`VgaCapture(timing)`; `tick(r4, g4, b4, hs, vs)` once per pix_clk.**
* measures from the syncs alone: line and frame totals, sync widths, polarity (the shorter
  phase of each sync is its pulse), the bounding box of non-black pixels (which, with the
  test pattern's border, gives the porches), pixels in the blanking, lines or frames whose
  length changes; `frame_hz(pix_hz)` gives the refresh rate;
* coordinates: a line starts at its hsync leading edge; the line containing the vsync leading
  edge is line 0; the picture is then pixels 144..783 of lines 35..514 for 640x480;
* keeps the last whole frame: `pixel(x, y)` (RGB444), `write_ppm(path)`
  (`sim/io/ppm2png.py` turns it into a PNG);
* `decode_text(font)` cuts the frame into 8x16 cells and names each cell's character by
  matching its two colours against the glyphs, either colour as the foreground (the frame's
  commonest colour decides between a glyph and its inverse); rows 14-15 solid over a glyph's
  rows 0-13 is reported as the cursor; one-colour cells read as spaces. `VgaFont` loads the
  glyphs from `driver/font8x16.c` or from `surf_video_font.v`.

## 5. Tests

| Target | What | Result (5 seeds) |
|---|---|---|
| `make lint-io` | Verilator `-Wall` lint of each block (no `lint_off`; deliberately unused bits are named in an `unused_ok` wire with a comment), Icarus `-g2005 -Wall` compile | clean |
| `make test-io-font` | font sources match their pins, ROM current, 256 glyphs equal `driver/font8x16.c` | pass |
| `make test-io-models` | SD model against a software SPI master (240 checks); keyboard model against a software host (36); capture against generated signals (36) | pass |
| `make test-io-spi` | surf_spi on SurfBus: register rules, mode 0 timing at DIV 0..255, then with the SD model: full initialization and block reads/writes for SDHC, SDSC v2 and SDSC v1 cards, injected errors, an absent card, interrupt-driven transfers (801 checks) | pass |
| `make test-io-ps2` | surf_ps2 with the keyboard model: typing at 10, 12.5 and 16.7 kHz decoded back to text, extended keys, every receive error with recovery, FIFO overflow, RX_EN, interrupts, counter saturation, host commands with acknowledge, TX_NAK, a command in mid-frame, the open-drain discipline | pass |
| `make test-io-video` | surf_video with the capture: timing and porches, the test pattern and random text pixel-exact, decode_text, scroll, cursor, blink, palette, black modes, VBLANK, VBLANK_EV, FRAME, single-domain resets; sys clock 25, 50, 10 or 30 MHz by seed | pass |
| `make test-io-icarus` | the three blocks together in Icarus | pass |
| `make test-io` | all of the above (`IO_SEEDS`, default 1 2 3) | on `test-soc` |
| `make check-io` | the font check | on `check` |
| `make synth-io` | Yosys estimates | section 3 |
| `make evidence-io` | reruns everything on seeds 1-5 and rewrites `docs/evidence/io/` | |

The whole of `make test-io` takes about 30 seconds. The testbenches check the SurfBus rules
from the master's side on every access (no response without a request, none in the
acceptance cycle, request fields scrambled after acceptance, back-to-back requests) and run
with Verilator's random initial values (`+verilator+rand+reset+2`). Mutation checks during
development: sampling MISO on the falling edge made 222 SPI checks fail; a 2-cycle PS/2
filter let the glitch through and the glitch test failed.

## 6. Notes for integration

* `surfsim` (soc track): tick `SdCardModel` with `sd_cs_n/sd_sclk/sd_mosi` after each clk edge
  and feed its MISO and `cd_n()` back before the next; tick `Ps2KeyboardModel` with the two
  `_oe` outputs and feed the returned levels to `ps2_clk_i/ps2_data_i`; tick `VgaCapture`
  after each pix_clk edge. `test_spi.cpp`, `test_ps2.cpp` and `test_video.cpp` show the order.
* `sim/io/tb_video.v` has a stand-in for `surf_textram` (sys-side write, pix-side registered
  read) only for these tests; the SoC uses the soc track's module.
* Board top: PS/2 `_oe` to IOBUF T inverted with I = 0; the SPI MISO input constraint (section
  1); the three crossings of section 3. The scan-out also reads text RAM cells 2400..2499
  during blanking (harmless reads of ordinary RAM).

## 7. Pending and limits

* Everything physical: SCLK and MISO timing with the Pmod MicroSD and a real card, the card
  detect switch's polarity, PS/2 levels and a real keyboard's timing, a monitor locking to
  59.946 Hz. Vivado synthesis and timing of these blocks: pending a build host.
* The SD model is not a particular card: real cards differ in NCR, NAC, busy times and in how
  they report errors such as an out-of-range address; SurfBoot should allow the spec's
  limits, as `sd_host.h` does. CMD1, CMD6 (high speed), CMD42 locking, erase commands and
  ACMD13 are not modelled.
* No inhibit-based flow control: a full PS/2 FIFO drops bytes (OVF) rather than holding the
  keyboard off. There is no "send done" interrupt (poll TX_BUSY).

## 8. Contract notes (proposals for the integrator)

1. SURFBUS.md section 2 rule 7 bounds the SPI DATA wait by 16 x (DIV + 1): with DIV copied at
   the start of a transfer, the bound is the DIV of the running transfer, not the current
   register. Suggested wording: "16 x (DIV + 1) cycles, DIV as it was when the transfer
   started".
2. MEMORY-MAP.md could say when STATUS.FRAME counts (at the start of vertical blanking, with
   VBLANK_EV) and that CTRL, CURSOR and SCROLL take effect at the next frame.
3. MEMORY-MAP.md ps2 CTRL.RX_EN: state that 0 ignores the lines (it does not inhibit the
   keyboard). A later version could add a "send finished" status bit and interrupt.
