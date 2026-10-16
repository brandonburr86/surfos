# SurfCPU on the Arty A7: the wiring table

Every pin the SurfCPU designs use, from the adapter's pin to the FPGA ball, with its direction
and I/O standard (brief section 4). It is checked by `tools/check-wiring.py` (on `make check`)
against the designs' top-level ports, their constraint files (`boards/arty_a7/xdc/`) and
Digilent's master XDC files for both board variants: every port has exactly one pin, no two
ports share a pin, and pin, direction, I/O standard and pin options agree everywhere.

**Nothing here has been checked on the hardware.** The board and the adapters are not
connected and their revisions are not confirmed (INVENTORY.md gaps G1, G4). The allocation
(microSD on JA, VGA on JB + JC, PS/2 on JD) is the brief's proposal, not permission to plug
anything in. Read section 4 before connecting the PS/2 adapter.

## 1. Sources

All kept unmodified, with their SHA-256, in `boards/arty_a7/digilent/` (`fetch.sh` says where
each came from).

| Key | Document | Used for |
|---|---|---|
| XDC | Digilent master XDC, `Arty-A7-100-Master.xdc` and `Arty-A7-35-Master.xdc`, github.com/Digilent/digilent-xdc at commit `c8644a9fe31c` ("Rev. D and Rev. E"; MIT) | every FPGA pin and its I/O standard; the two files give the same pins |
| RM | Arty A7 Reference Manual (Digilent; the PDF, and the web page as archived 2024-11-07) | §1 parts, §7 oscillator, §8 USB-UART, §9 buttons, switches and LEDs, §10 Pmod connectors, §11.1 I/O voltage limits, §5.2 flash parts |
| SCH | Arty A7 schematic Rev E.0 (doc 500-319, 2018-01-09) | sheet 1 Pmod series resistors, buttons, switches; 2 LEDs; 4-5 bank pins, reset button, oscillator; 6 configuration (CFGBVS, mode jumper, flash); 7 bank supplies; 9 DDR3 reset pull-down |
| SD-RM | Pmod MicroSD Reference Manual (Revision A) | J1 pin table; 2.7-3.6 V supply |
| SD-SCH | Pmod MicroSD schematic A.1 (500-380, 2018-03-07) | 20 kΩ pull-ups on every card line and on CD; CD through 200 Ω to the socket's detect switch; supply = the Pmod's 3.3 V |
| VGA-RM | Pmod VGA Reference Manual (Rev. C, 2018-01-22) | J1 and J2 pin tables |
| VGA-SCH | Pmod VGA schematic C.0 (500-345, 2016-12-06) | inputs buffered by SN74ALVC245 at 3.3 V with 100 kΩ pull-downs; R-2R ladders (bit 3 = most significant); HS/VS through 100 Ω |
| PS2-RM | Pmod PS2 Reference Manual (Revision C) | J2 (Pmod), J1 (mini-DIN), JP1 and JP2 tables; the JP2 silkscreen error |
| PS2-SCH | Pmod PS2 schematic Rev C (500-094, 2006-12-20) | 10 kΩ pull-ups from DATA, DATA-, CLK, CLK- to the PS/2 supply chosen by JP1; 200 Ω in series to the connector |

Facts every row relies on: all used pins are in banks 14, 15, 16 and 35, whose VCCO is the
board's 3.3 V (SCH sheet 7), so every pin is LVCMOS33; bank 34 is the DDR3L bank at 1.35 V and
no SurfCPU pin may be placed there (the checker refuses it). JA and JD connect through 200 Ω
series resistors, JB and JC through 0 Ω (SCH sheet 1, RM §10). Pmod pins 6 and 12 are the
board's 3.3 V, pins 5 and 11 ground (RM §10). The FPGA pins' limits are -0.2..3.4 V
recommended and 3.75 V absolute maximum while powered, 0.55 V while the board is off (RM §11.1).

Status: **checked** = the master XDC, the reference manual and the schematic agree on it;
**unverified** = it also depends on something only the hardware can show (named in section 3).

## 2. The table

Directions are the FPGA's. *Options* are the extra pin properties in the XDC (the defaults
otherwise: no pull, 12 mA, slow slew). The baseline design uses the base pins only; the full
top (`surfcpu_arty_a7_top`) uses all of them.

### 2.1 Base: clock, reset, switches, buttons, LEDs, UART

| Port | Adapter pin | Board pin | FPGA pin | Dir | I/O standard | Options | Master XDC | Status |
|---|---|---|---|---|---|---|---|---|
| `clk100` | - | 100 MHz oscillator IC2, net GCLK100 (SCH 5, RM §7) | E3 | in | LVCMOS33 | - | `CLK100MHZ` | checked |
| `ck_rst_n` | - | RESET button (red), net CK_RST: 10 kΩ pull-up, low while pressed (SCH 5, RM §9) | C2 | in | LVCMOS33 | - | `ck_rst` | checked |
| `sw[0]` | - | SW0 (through 10 kΩ; up = 1) | A8 | in | LVCMOS33 | - | `sw[0]` | checked |
| `sw[1]` | - | SW1 | C11 | in | LVCMOS33 | - | `sw[1]` | checked |
| `sw[2]` | - | SW2 | C10 | in | LVCMOS33 | - | `sw[2]` | checked |
| `sw[3]` | - | SW3 | A10 | in | LVCMOS33 | - | `sw[3]` | checked |
| `btn[0]` | - | BTN0 (through 10 kΩ, 10 kΩ pull-down; pressed = 1) | D9 | in | LVCMOS33 | - | `btn[0]` | checked |
| `btn[1]` | - | BTN1 | C9 | in | LVCMOS33 | - | `btn[1]` | checked |
| `btn[2]` | - | BTN2 | B9 | in | LVCMOS33 | - | `btn[2]` | checked |
| `btn[3]` | - | BTN3 | B8 | in | LVCMOS33 | - | `btn[3]` | checked |
| `led[0]` | - | LD4 (330 Ω; 1 = on) | H5 | out | LVCMOS33 | - | `led[0]` | checked |
| `led[1]` | - | LD5 | J5 | out | LVCMOS33 | - | `led[1]` | checked |
| `led[2]` | - | LD6 | T9 | out | LVCMOS33 | - | `led[2]` | checked |
| `led[3]` | - | LD7 | T10 | out | LVCMOS33 | - | `led[3]` | checked |
| `rgb[0]` | - | LD0 red (through a transistor; 1 = on) | G6 | out | LVCMOS33 | - | `led0_r` | checked |
| `rgb[1]` | - | LD0 green | F6 | out | LVCMOS33 | - | `led0_g` | checked |
| `rgb[2]` | - | LD0 blue | E1 | out | LVCMOS33 | - | `led0_b` | checked |
| `rgb[3]` | - | LD1 red | G3 | out | LVCMOS33 | - | `led1_r` | checked |
| `rgb[4]` | - | LD1 green | J4 | out | LVCMOS33 | - | `led1_g` | checked |
| `rgb[5]` | - | LD1 blue | G4 | out | LVCMOS33 | - | `led1_b` | checked |
| `rgb[6]` | - | LD2 red | J3 | out | LVCMOS33 | - | `led2_r` | checked |
| `rgb[7]` | - | LD2 green | J2 | out | LVCMOS33 | - | `led2_g` | checked |
| `rgb[8]` | - | LD2 blue | H4 | out | LVCMOS33 | - | `led2_b` | checked |
| `rgb[9]` | - | LD3 red | K1 | out | LVCMOS33 | - | `led3_r` | checked |
| `rgb[10]` | - | LD3 green | H6 | out | LVCMOS33 | - | `led3_g` | checked |
| `rgb[11]` | - | LD3 blue | K2 | out | LVCMOS33 | - | `led3_b` | checked |
| `uart_rx` | - | FT2232HQ channel B TXD (to the FPGA; RM §8) | A9 | in | LVCMOS33 | - | `uart_txd_in` | checked |
| `uart_tx` | - | FT2232HQ channel B RXD (from the FPGA; RM §8) | D10 | out | LVCMOS33 | - | `uart_rxd_out` | checked |

`rgb[3n]`, `rgb[3n+1]`, `rgb[3n+2]` are LED n's red, green and blue, as the GPIO block's RGB
register (MEMORY-MAP.md). The RGB LEDs are driven at a low duty cycle (RM §9.1 warns against
full drive).

### 2.2 Pmod JA: Pmod MicroSD (12-pin), SPI mode

| Port | Adapter pin | Board pin | FPGA pin | Dir | I/O standard | Options | Master XDC | Status |
|---|---|---|---|---|---|---|---|---|
| `sd_cs_n` | J1-1 ~CS (CS/CD/DAT3) | JA1 (200 Ω) | G13 | out | LVCMOS33 | - | `ja[0]` | unverified (adapter revision) |
| `sd_mosi` | J1-2 MOSI (CMD/DI) | JA2 (200 Ω) | B11 | out | LVCMOS33 | - | `ja[1]` | unverified (adapter revision) |
| `sd_miso` | J1-3 MISO (DAT0/DO) | JA3 (200 Ω) | A11 | in | LVCMOS33 | PULLUP | `ja[2]` | unverified (adapter revision) |
| `sd_sclk` | J1-4 SCK | JA4 (200 Ω) | D12 | out | LVCMOS33 | - | `ja[3]` | unverified (adapter revision) |
| `sd_dat1` | J1-7 DAT1 (unused in SPI mode; parked) | JA7 (200 Ω) | D13 | in | LVCMOS33 | PULLUP | `ja[4]` | unverified (adapter revision) |
| `sd_dat2` | J1-8 DAT2 (unused in SPI mode; parked) | JA8 (200 Ω) | B18 | in | LVCMOS33 | PULLUP | `ja[5]` | unverified (adapter revision) |
| `sd_cd_n` | J1-9 CD (card detect, low = card present) | JA9 (200 Ω) | A18 | in | LVCMOS33 | PULLUP | `ja[6]` | unverified (detect polarity, U3) |

J1-10 (JA10, K16) is not connected on the adapter and not used. J1-5/11 ground, J1-6/12 the
board's 3.3 V, which is also the card's supply (SD-SCH: the inrush limiter is not fitted and
a 0 Ω link feeds VCC3V3 straight through; SD-RM: 2.7-3.6 V).

### 2.3 Pmod JB + JC: Pmod VGA (two 12-pin headers)

| Port | Adapter pin | Board pin | FPGA pin | Dir | I/O standard | Options | Master XDC | Status |
|---|---|---|---|---|---|---|---|---|
| `vga_r[0]` | J1-1 R0 | JB1 (0 Ω) | E15 | out | LVCMOS33 | - | `jb[0]` | unverified (orientation, U2) |
| `vga_r[1]` | J1-2 R1 | JB2 | E16 | out | LVCMOS33 | - | `jb[1]` | unverified (orientation, U2) |
| `vga_r[2]` | J1-3 R2 | JB3 | D15 | out | LVCMOS33 | - | `jb[2]` | unverified (orientation, U2) |
| `vga_r[3]` | J1-4 R3 | JB4 | C15 | out | LVCMOS33 | - | `jb[3]` | unverified (orientation, U2) |
| `vga_b[0]` | J1-7 B0 | JB7 | J17 | out | LVCMOS33 | - | `jb[4]` | unverified (orientation, U2) |
| `vga_b[1]` | J1-8 B1 | JB8 | J18 | out | LVCMOS33 | - | `jb[5]` | unverified (orientation, U2) |
| `vga_b[2]` | J1-9 B2 | JB9 | K15 | out | LVCMOS33 | - | `jb[6]` | unverified (orientation, U2) |
| `vga_b[3]` | J1-10 B3 | JB10 | J15 | out | LVCMOS33 | - | `jb[7]` | unverified (orientation, U2) |
| `vga_g[0]` | J2-1 G0 | JC1 (0 Ω) | U12 | out | LVCMOS33 | - | `jc[0]` | unverified (orientation, U2) |
| `vga_g[1]` | J2-2 G1 | JC2 | V12 | out | LVCMOS33 | - | `jc[1]` | unverified (orientation, U2) |
| `vga_g[2]` | J2-3 G2 | JC3 | V10 | out | LVCMOS33 | - | `jc[2]` | unverified (orientation, U2) |
| `vga_g[3]` | J2-4 G3 | JC4 | V11 | out | LVCMOS33 | - | `jc[3]` | unverified (orientation, U2) |
| `vga_hs` | J2-7 HS | JC7 | U14 | out | LVCMOS33 | - | `jc[4]` | unverified (orientation, U2) |
| `vga_vs` | J2-8 VS | JC8 | V14 | out | LVCMOS33 | - | `jc[5]` | unverified (orientation, U2) |

J2-9 and J2-10 (JC9 T13, JC10 U13) are not connected on the adapter and not used. Bit 3 of
each color is the most significant (VGA-SCH: the R-2R ladder's bit 3 sits next to the output).
The adapter buffers every input with SN74ALVC245s powered from the Pmod's 3.3 V and pulls them
down with 100 kΩ, so the FPGA drives 3.3 V CMOS loads only and an unconfigured FPGA gives a
black screen (VGA-SCH). JB and JC have no series resistors (0 Ω): those pins must never be
driven against anything, which holds while the Pmod VGA is the only thing plugged in.

### 2.4 Pmod JD: Pmod PS2 (6-pin, in the top row of JD)

| Port | Adapter pin | Board pin | FPGA pin | Dir | I/O standard | Options | Master XDC | Status |
|---|---|---|---|---|---|---|---|---|
| `ps2_data` | J2-1 DATA | JD1 (200 Ω) | D4 | inout | LVCMOS33 | PULLUP DRIVE=4 | `jd[0]` | unverified (supply and level, U4; top row, U5) |
| `ps2_data2` | J2-2 DATA- (spare pair; parked) | JD2 (200 Ω) | D3 | in | LVCMOS33 | PULLUP | `jd[1]` | unverified (supply and level, U4; top row, U5) |
| `ps2_clk` | J2-3 CLK | JD3 (200 Ω) | F4 | inout | LVCMOS33 | PULLUP DRIVE=4 | `jd[2]` | unverified (supply and level, U4; top row, U5) |
| `ps2_clk2` | J2-4 CLK- (spare pair; parked) | JD4 (200 Ω) | F3 | in | LVCMOS33 | PULLUP | `jd[3]` | unverified (supply and level, U4; top row, U5) |

J2-5 is ground and J2-6 (VB) the board's 3.3 V. JD7..JD10 (E2, D2, H2, G2) are not used.
`ps2_clk` and `ps2_data` are open drain: an IOBUF drives 0 or lets go, never 1 (the high
level comes from the pull-ups; `surfcpu_arty_a7_top`).

### 2.5 Pins that are deliberately not used

Pmod pins without a signal (JA10, JC9, JC10, JD7..JD10), the ChipKit header, Ethernet, the
quad-SPI flash pins and every DDR3L pin (bank 34) are left unassigned: Vivado's default for
unused pins is a weak pull-down, and the DDR3L's RESET# has its own 4.7 kΩ pull-down (SCH
sheet 9), so the memory stays in reset while `HAS_DDR = 0`. The ddr track adds the DDR3L pins
with the memory controller.

## 3. Unverified points

| # | What | Why it matters | How to settle it |
|---|---|---|---|
| U1 | The board's revision (the master XDC covers Rev. D and E; the schematic is Rev. E.0) and variant (35T or 100T) | the pins are the same on both variants and both revisions; the FPGA part and flash differ | owner: the label; `make doctor`: the IDCODE (`tools/program.sh` refuses a mismatch) |
| U2 | Which Pmod VGA header goes into which Arty connector | the table assumes the adapter's J1 (red, blue) in JB and J2 (green, syncs) in JC; turned the other way, the syncs land on JB7/JB8 and nothing is displayed | owner: with the adapter plugged in, its J1 silkscreen must sit in JB. The color-bar test pattern (milestone 6) then shows red, green and blue in place |
| U3 | microSD card-detect polarity | CD is pulled up by 20 kΩ and the socket's switch connects it to the grounded shell (SD-SCH), so low = card present, as `surf_spi`'s `cd_n` expects; the socket's own data sheet was not checked | owner: read `spi.STATUS.CARD` with and without a card (SurfBoot prints it) |
| U4 | **PS/2 supply and signal levels** | see section 4: with the adapter's JP1 on external power the clock and data lines are pulled up to that supply | owner: the questions in section 4, before connecting |
| U5 | The 6-pin Pmod PS2 in the top row of JD (pins 1-6) | in the bottom row its DATA and CLK would land on JD7/JD9, which are not assigned | owner: plug it into the upper row, its pin 1 at JD pin 1 |
| U6 | Adapter models and revisions | the tables follow Pmod MicroSD rev. A (A.1 schematic), Pmod VGA rev. C, Pmod PS2 rev. C; another revision could differ | owner: the labels (INVENTORY.md G4) |
| U7 | The Arty's JP2 (CK_RST to the FT2232) | shorted, JP2 lets the FT2232's DTR line drive the RESET net (RM §8, §11); the USB/UART sheet of the schematic is not published, so whether opening a serial terminal would pulse or hold the reset is unknown | owner: leave JP2 open (not shorted) |

## 4. The PS/2 supply: questions for the owner

What the documents show (PS2-SCH, PS2-RM): the Pmod PS2 pulls its DATA and CLK lines (and the
spare DATA-/CLK-) up with 10 kΩ to "VCC", the mini-DIN's supply, which jumper JP1 takes either
from the Pmod (VB = the Arty's 3.3 V) or from an external supply on header JP2 (VE). The
keyboard itself only pulls the lines low (open collector) and usually has its own pull-ups to
its supply. The lines reach the FPGA through 200 Ω on the adapter and 200 Ω on the Arty.

* **JP1 on VB (3.3 V from the Arty): safe for the FPGA.** Every pull-up ends at 3.3 V, within
  the pins' limits. The open question is the keyboard: PS/2 keyboards are specified for 5 V and
  many, not all, work at 3.3 V.
* **JP1 on VE with a 5 V supply: not safe.** The adapter's pull-ups and the keyboard's own put
  5 V on JD1 and JD3 through 400 Ω, above the 3.75 V absolute maximum while the board is on and
  far above the 0.55 V allowed while it is off (RM §11.1). Do not do this without a level
  shifter.
* The adapter's JP2 silkscreen is wrong: VE and GND are swapped (PS2-RM).

Please tell us:

1. Which keyboard (make and model) and whether it is a true PS/2 keyboard (INVENTORY.md G5).
2. Where JP1 on your Pmod PS2 is set. **Recommended: VB (3.3 V from the Arty), and try the
   keyboard there first.**
3. If the keyboard does not work at 3.3 V: whether you can fit a bidirectional open-drain level
   shifter between the adapter and the keyboard (a 2-channel MOSFET type such as a BSS138
   board, 3.3 V on the FPGA side, 5 V on the keyboard side, 5 V supplying the keyboard). Until
   then the PS/2 port stays at 3.3 V.
4. That nothing else will be plugged into JB or JC while the VGA adapter is used (no series
   resistors on those two connectors).

The design side is already conservative: the FPGA never drives a PS/2 line high (open drain
through IOBUFs), its weak pull-ups only reach 3.3 V, and the receiver synchronizes the lines
before using them (`surf_ps2`, io track).
