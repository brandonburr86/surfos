# SurfCPU on the Arty A7: the board

The board track's part of SurfCPU: the clocks and resets, the pins and constraints, the
milestone 1 design, the top for the whole SoC, the Vivado and open-flow builds and the
programming wrapper.
Pins and adapters are in [WIRING.md](WIRING.md); the build host in [BUILD-HOST.md](BUILD-HOST.md).

**Status (2026-10-16, round ddr-3).** Built into bitstreams for both boards with the
open-source flow (`make build-fpga FLOW=openxc7`), every check of that flow passing (section 8):
the milestone 1 design; the whole SoC without DDR in two designs, with the BRAM diagnostics
(milestone 3) or SurfBoot in its boot ROM; and **the two DDR designs (milestone 4, section 7)**:
LiteDRAM without its calibration CPU, its controller at 80 MHz (100 MHz does not close in
nextpnr), initialized and read-leveled by SurfCPU's firmware through the controller's
registers. Their timing is nextpnr's estimate. The synthesized netlists, simulated from their
pins, print the same characters as the RTL. **On the owner's Arty A7-100T** (2026-10-16,
PHYSICAL-TESTS.md, `docs/evidence/physical/`) the baseline (m1) and the BRAM diagnostics (m3)
passed, and SurfBoot without DDR ran to its monitor, read SurfOS's files from the owner's card
and drove the VGA monitor, all loaded with `make program-volatile` from bitstreams of the earlier
SoC (0.1.0). **The DDR designs have not been on a board yet**:
section 12 has their first test, with what the serial port should show according to simulation.
**Nothing has been built with Vivado** (there is no Vivado host yet, gap G2). The first real run
of `build.tcl` will also be the first time Vivado reads these constraints; section 8 lists what
that run must confirm.

| | Where |
|---|---|
| Clock plan and its checker | `ip/clocking/arty_a7_clocks.toml`, `ip/clocking/clockplan.py` |
| Board RTL (vendor primitives only here) | `boards/arty_a7/rtl/` |
| Constraints | `boards/arty_a7/xdc/` (pins, timing, configuration; `cdc/` scoped per module) |
| Design file lists | `boards/arty_a7/designs/*.files` |
| Board variants (part, IDCODE, openFPGALoader name) | `boards/arty_a7/boards.txt` |
| Vivado flow | `boards/arty_a7/vivado/build.tcl` |
| Open flow: the constraints nextpnr applies | `boards/arty_a7/nextpnr-xdc.awk` (the flow itself: `tools/openxc7-build.sh`, OPEN-FLOW.md) |
| Wrappers | `tools/fpga-build.sh`, `tools/program.sh`, `tools/check-wiring.py` |
| The SoC builds' own checks: every RTL memory still in RAM cells and the multiplier in its DSP48E1s; the boot ROM's contents | `boards/arty_a7/netlist_check.py`; `boards/arty_a7/romcheck.py` with `sim/board/tb_rom_read.v` and the block RAM models `sim/board/prims/RAMB36E1.v`, `RAMB18E1.v` |
| Digilent's master XDC (in git) and manuals and schematics (a local cache, `fetch.sh fetch`), with SHA-256 | `boards/arty_a7/digilent/` |
| Simulation: primitive models, stand-in SoC, tests; the expected serial output and the post-synthesis simulation; the open-flow evidence scripts | `sim/board/` (`uart_expect.sh`; `tb_netlist.v`, `netlist_sim.sh`; `openxc7_evidence.sh`, `openxc7_soc_evidence.sh`) |
| Make targets | `mk/area-board.mk` |
| Evidence | `docs/evidence/board/` |

## 1. The designs

| Design | Top | Pins | Use |
|---|---|---|---|
| `arty_a7_baseline` | `boards/arty_a7/rtl/arty_a7_baseline.v` | the base set: clock, RESET, switches, buttons, LEDs, UART | milestone 1: no CPU; shows the board, the clocks, the reset and the serial link work (section 5) |
| `surfcpu_arty_a7_top` | `boards/arty_a7/rtl/surfcpu_arty_a7_top.v` | base + Pmod microSD (JA), VGA (JB+JC), PS/2 (JD) | the whole SoC (section 6) with SurfBoot (`build/fw/surfboot.mem`) in its boot ROM |
| `surfcpu_arty_a7_bramdiag` | the same top | the same | the same SoC with the BRAM diagnostics (`build/fw/bramdiag.mem`) in its boot ROM: milestone 3 (section 6) |
| `surfcpu_arty_a7_ddr_bramdiag` | the same top with `ARTY_HAS_DDR` | the same + the DDR3L (bank 34) | the SoC with the DDR3L memory (LiteDRAM, section 7) and the BRAM diagnostics, which initialize the DDR3L and then test it: milestone 4 |
| `surfcpu_arty_a7_ddr` | the same | the same | the SoC with the DDR3L and SurfBoot, which initializes it and loads SurfOS into it |

All take `BOARD` (`arty_a7_35t` or `arty_a7_100t`, the spec's `[boards]` names; the top turns
it into sysinfo's `BOARD_ID` from `surfcpu_soc.vh`) and the build identity `BUILD_ID`,
`BUILD_FLAGS`, `BUILD_TIME`, which `tools/fpga-build.sh` passes as Vivado generics, or for the
open flow as the defines `ARTY_BUILD_*` that the tops take as their parameters' defaults
(section 8). The pins of both variants are the same (checked against both master XDC files);
only the part differs.

## 2. Clocks

The board has one 100 MHz oscillator on pin E3, an MRCC input of bank 35 (RM §7, schematic
sheet 5). It feeds one PLL and one MMCM directly (the two halves of the CMT next to bank 35);
each output reaches its domain through a BUFG.

| Clock | Spec value | Made by | Settings | VCO | PFD |
|---|---|---|---|---|---|
| `clk100` | `clocks.osc_hz` = 100 MHz | oscillator | `create_clock -period 10.000` (generated) | | |
| `sys_clk` | `clocks.sys_hz` = 25,000,000 Hz, exactly | PLLE2_BASE | D = 1, M = 10, O0 = 40 | 1000 MHz | 100 MHz |
| `pix_clk` | `clocks.pix_hz` = 25,173,611 Hz (25,173,611.11) | MMCME2_BASE | D = 2, M = 18.125, O0 = 36 | 906.25 MHz | 50 MHz |

The numbers are not typed anywhere by hand: `ip/clocking/arty_a7_clocks.toml` chooses the
settings, and `ip/clocking/clockplan.py` checks them and generates
`boards/arty_a7/rtl/arty_a7_clock_params.vh` and `boards/arty_a7/xdc/arty_a7_clocks.xdc`
(`make check` fails if they are stale). It checks, for **both** parts:

| Limit | MMCM | PLL | Source |
|---|---|---|---|
| Input clock | 10..800 MHz | 19..800 MHz | DS181 v1.27.1 tables 37/38 |
| Phase detector (CLKIN / D) | 10..450 MHz | 19..450 MHz | same, -1 and -1LI columns |
| VCO | 600..1200 MHz | 800..1600 MHz | same |
| Output | 4.69..800 MHz | 6.25..800 MHz | same |
| M | 2.000..64.000 in steps of 0.125 | 2..64 | UG472 v1.14 tables 3-7/3-8 |
| D | 1..106 | 1..56 | same |
| O0 | 1..128, or 2.000..128.000 in steps of 0.125 | 1..128 | same |
| CLKIN1_PERIOD | 0.938..100 ns | 0.938..52.631 ns | same |
| Fractional M or O0 (open flow) | integer part even, not 2.125 | - | docs/OPEN-FLOW.md 6.2 |

The Arty A7-100T carries XC7A100TCSG324-1 (the -1 grade at VCCINT 1.0 V) and the A7-35T
XC7A35TICSG324-1L (the -1LI grade, VCCINT 0.95 V; RM §1 and §3, schematic sheet 11). DS181
gives both grades the same clock-tile limits. Margins: the MMCM's VCO is 306.25 MHz above its
minimum and 293.75 MHz below its maximum, the PLL's 200 and 600 MHz. Only CLKOUT0 and the
feedback of an MMCM divide fractionally, and 906.25 MHz has no integer divide giving 25 MHz,
hence the separate PLL for `sys_clk`. Its VCO of 1000 MHz also divides to 200 MHz (/5) and
166.667 MHz (/6), the reference and system clocks AMD's memory controller wants on this board
(RM §5.1), should the ddr track take them from here.

`pix_clk` is -55.2 ppm from the 25.175 MHz standard; a line lasts 800 pixels and a frame
800 x 525, so the monitor sees 31.4670 kHz and 59.9372 Hz (VESA allows +-0.5 %).

**Changed 2026-10-15** (the contract `clocks.pix_hz`, with the integrator's authorization):
`pix_clk` was 100 MHz x 8.875 / 35.25 = 25,177,305 Hz (+91.6 ppm, 59.946 Hz). The open flow's
nextpnr 1.0.0 writes an MMCM fractional divider whose integer part is odd (35.25) into the
bitstream differently from AMD's XAPP888 algorithm and from Vivado (docs/OPEN-FLOW.md section
6.2; the flow's `cmtcheck.py` catches it, and what the chip would make of it is unknown). The
new setting, 100 MHz / 2 x 18.125 / 36, has an even integer part in its one fractional divider
and an integer output divider, so both flows encode it alike; the flow track proved it through
`cmtcheck.py` in a real build (evidence `../flow/mmcm.txt`), and the baseline's open-flow
builds check it again (evidence `openxc7.txt`). `clockplan.py` now refuses a fractional M or
O0 with an odd integer part (or 2.125). `clockplan.py report` lists closer
settings (evidence `clocks.txt`), such as D = 5, M = 58.375, O0 = 46.375 at +8.0 ppm, but with
two fractional dividers and a VCO 32.5 MHz below its maximum; 55 ppm is far inside what a
monitor accepts.
The one-MMCM-for-both option noted before (D = 1, M = 9, O0 = 35.75: -6.9 ppm, its 900 MHz VCO
also divides to 25 MHz) has an odd fractional divider too, so it is out while the open flow is
in use.

**Simulation.** `sim/board/prims/` models MMCME2_BASE, PLLE2_BASE, BUFG and IOBUF with AMD's
names, parameters and ports, for Icarus and Verilator (`--timing`). The tile models check their
settings against the limits above at time 0 (`$fatal` on a violation; `tb_prims_bad` proves six
violations are caught), measure the input period, lock after the input is good and the
feedback toggles, drop LOCKED on reset or a stopped input, and make output edges at exact
absolute times (no drift over any run). Not modelled: phase against the input, jitter, duty
cycles other than 50 %, the dynamic ports. The models are never read by synthesis.

## 3. Resets

`arty_a7_clocks.v` makes one synchronous active-high reset per domain with
`rtl/lib/surf_reset_sync.v`:

* `sys_rst` asserts at once (asynchronously) while the PLL is unlocked or the RESET button is
  pressed, and is released on the third `sys_clk` edge after both are gone;
* `pix_rst` likewise from the MMCM's lock and the button, on `pix_clk`.

The RESET button (pin C2, net CK_RST: a 10 kΩ pull-up, low while pressed, schematic sheet 5)
also resets both clock tiles, which is how to recover a tile that lost lock (UG472: a tile must
be reset after LOCKED falls); the outputs stop and both domains stay in reset until the tiles
lock again (at most 100 µs, DS181). `tb_clocks` checks the release on the third edge, the
asynchronous assertion, the button and a stopped oscillator. The CK_RST net also reaches the
shield's RST pin and, through jumper JP2, the FT2232: keep JP2 off. Fitted, it lets the FT2232's DTR
line pulse RESET, so every program that opens the serial port resets the design (seen on the owner's
board, which came with JP2 fitted: WIRING.md U7).

The tops never use a reset from another domain; everything that crosses between `sys_clk` and
`pix_clk` goes through an `rtl/lib` synchronizer whose two sides recover on their own resets.

## 4. Clock-domain crossings and timing constraints

There are **no clock groups**. `sys_clk` and `pix_clk` both derive from `clk100`, so Vivado
would time paths between them as synchronous; instead every crossing is a synchronizer from
`rtl/lib` and gets its own bound, and a clock group would override those bounds (Vivado's
exception priority). Any other path between the domains is a design error that must show up
in `report_cdc` and `report_clock_interaction`, not disappear.

| Crossing structure | Constraint (scoped to every instance, `boards/arty_a7/xdc/cdc/`) |
|---|---|
| `surf_cdc_word` (word by request/acknowledge handshake) | `set_max_delay -datapath_only` of the smaller period on `hold` → `dst_data`, `req` → `req_sync[0]`, `ack` → `ack_sync[0]` |
| `surf_pulse_sync` (toggle into a two-flop synchronizer) | the same on `src_toggle` → `dst_sync[0]` |
| `surf_sync` (level into a two-flop synchronizer) | the same, this domain's period, from every other clock into `sync_ff[0]`; a pin's false path covers pins |
| `surf_reset_sync` | false path to the presets (asynchronous assertion) |

The synchronizer flops carry `ASYNC_REG` in the RTL, and the scoped files set it again (so a
lost attribute cannot split a chain). The files are Tcl (`read_xdc -ref
<module> -unmanaged`) and processed late, after the clocks exist; they compare the clocks at
the instance's ports and add nothing when both sides share a clock. `cdc_xdc_test.tcl` runs
them with stand-in queries and checks every constraint they issue; `check-wiring.py` checks
the register and port names against the RTL (Vivado names a register `<name>_reg`, or
`<name>_reg[i]` for a vector).

Crossings in the designs: the baseline sends its one-second gate into the pixel domain
(`surf_pulse_sync`), brings the pixel count back (`surf_cdc_word`) and the pixel heartbeat
(`surf_sync`). In the full top every crossing is inside the SoC: `surf_video` (IO.md section 3)
sends its 214-bit settings word (CTRL, CURSOR, SCROLL, palette) from sys to pix through
`surf_cdc_word` (so `hold` to `dst_data` is bounded by one pixel period, 39.718 ns, the smaller
period), and brings the vertical blanking level (`surf_sync`) and the start-of-blanking pulse
(`surf_pulse_sync`) back; the text RAM (soc track) has one port per clock. As a true dual-port
block RAM it has no timing path between its ports; built from distributed RAM it would have
one, which `report_cdc` would flag (to check at integration).

**I/O.** Every port has a timing constraint (`check-wiring.py` proves it statically):

| Ports | Constraint | Why it is enough |
|---|---|---|
| `clk100` | `create_clock` 10 ns | the oscillator |
| `sw`, `btn`, `ck_rst_n`, `uart_rx` | false path from | no relation to any FPGA clock; each enters a synchronizer |
| `led`, `rgb`, `uart_tx` | false path to | eyes, and the FT2232's own 8.7 µs bit clock |
| `sd_sclk`, `sd_mosi` | output delay on `sys_clk`, -max 28 ns, -min 0; MOSI's flop in the IOB | each pin within 12 ns of its `sys_clk` edge (the SPI budget below) |
| `sd_miso` | input delay on `sys_clk`, -max 32 ns, -min 2 ns; the sampling flop in the IOB | the round trip SCLK out, card, MISO in must fit one `sys_clk` cycle at DIV = 0 (below) |
| `sd_cs_n` | output delay on `sys_clk`, -max 20 ns, -min 0 | changes only between transfers, when software writes CTRL |
| `sd_cd_n`, `sd_dat1`, `sd_dat2` | false path from | a mechanical switch (through `surf_sync`); parked lines |
| `vga_r/g/b`, `vga_hs`, `vga_vs` | output delay on `pix_clk`, -max 20 ns, -min 0 | every color bit and sync settles within 20 ns of the pixel edge (a pixel is 39.7 ns) |
| `ps2_clk`, `ps2_data` (and the parked spare pair) | false path from and to | open drain at 10-16.7 kHz, clocked by the keyboard; `surf_ps2` synchronizes |

**The SPI budget.** `surf_spi` samples MISO, without a synchronizer, on the `sys_clk` edge
that raises SCLK (IO.md section 1). At DIV = 0 each SCLK half period is one 40 ns cycle, and the
card changes MISO after SCLK's falling edge, so within one cycle: the edge to the SCLK pin
(at most 12 ns, the output delay), the board both ways (traces, JA's 200 Ω, the Pmod, about
10 pF: estimated at most 3 ns each way), the card's output delay after SCLK falls (at most
14 ns in data transfer mode, the SD specification), so MISO is at the pin by 32 ns (the input
delay), leaving the pin-to-IOB-flop path the rest. MOSI changes with SCLK's falling edge and is
sampled by the card at the next rising edge, 40 ns later: over 20 ns of setup (5 ns needed).
The board delays are estimates, to be confirmed with the Pmod MicroSD and a card; at DIV 1
(6.25 MHz) the round trip has two cycles.

Vivado's `check_timing` reports the ports that have a false path separately from unconstrained
ones (as ports "with no input delay but user has a false path constraint", and likewise for
outputs); that category is deliberate and the only one `build.tcl` accepts. Any other `check_timing` finding
fails the build (section 8).

## 5. Milestone 1: `arty_a7_baseline`

No CPU. After configuration it shows:

| Board | Shows |
|---|---|
| LD4..LD7 (green) | switches SW0..SW3 |
| LD0..LD3 blue | buttons BTN0..BTN3, while pressed |
| LD0 green | `sys_clk` heartbeat: 1 Hz, lit for the first half of each second |
| LD1 green | `pix_clk` heartbeat: 1 Hz, counted in the pixel domain (the MMCM runs) |
| UART (USB, 115200 8N1) | one line a second, the first one second after reset |

The RGB LEDs are lit 1/8 of the time (RM §9.1 advises against full drive). A line looks like:

```
SurfCPU baseline arty_a7_100t 0x00000100 build 1a2b3c4d dirty 1760560000 | up 000012 s | pix 25173611 Hz | sw 0101 btn 0000 rx 41
```

board and sysinfo `BOARD_ID`; the build's commit, `clean` or `dirty` (`sim` in a simulation)
and build time (seconds since 1970); seconds since reset; the pixel clock measured against
`sys_clk` over the last second (25,173,611 ± 1 when the MMCM is right; the first line covers
the time since reset and may be off by a few counts); the switches and buttons (SW3..SW0,
BTN3..BTN0); the last byte received on the RX pin in hex (`--` before the first one), which
checks the FPGA's receive pin too. The UART runs at `sys_hz / round(sys_hz / 115200)` =
115,207 baud (+0.006 %).

**On the board (pending).** With `make build-fpga BOARD=arty_a7_100t DESIGN=arty_a7_baseline`
and `make program-volatile BOARD=arty_a7_100t DESIGN=arty_a7_baseline`: LD0 and LD1 blink
green together, once a second; `screen /dev/cu.usbserial-<serial> 115200` on the FT2232's
UART channel (its second port; `make doctor` lists the ports) shows a line a second with the
build's commit; the switches and buttons follow on the
LEDs and in the line; a typed key appears after `rx`; RESET restarts the count at `up 000001`.
Milestone 1 closes only with that physical result plus the post-route timing and DRC reports.

**Tests.** `baseline_rt` (Verilator, C++) runs the logic with its real one-second timing for
2.05 s at the exact `sys_clk` and `pix_clk` frequencies: heartbeat periods of exactly
25,000,000 cycles, the pixel heartbeat within 3 cycles, two decoded lines exactly one second
apart with the exact text, the pixel figure 25,173,611 ± 2, and the switch, button and RX echo.
`tb_baseline` runs the whole top from its pins with the clock tile models and a 13 ms "second"
(Verilator, with a RESET press; Icarus, a shorter run): lines decoded at the real 115200 baud,
the pixel count for the shortened second ± 1, the 1/8 RGB duty cycle, the heartbeat lit only in
the first half, and RESET restarting the uptime.

## 6. The SoC top: `surfcpu_arty_a7_top`

`arty_a7_clocks` + `surfcpu_soc` (MODULES.md section 4) + the board's pins:

* `surfcpu_soc` gets `SYS_HZ`, `PIX_HZ`, `DDR_SIZE` from `surfcpu_soc.vh`, `BOARD_ID` from
  `BOARD`, the build identity and `BOOTROM_INIT` from the build, and `HAS_DDR = 0`.
* PS/2: one `IOBUF` per line with `I = 0` and `T = !oe`, so the FPGA pulls a line low or lets
  it go and never drives it high; the adapter's and keyboard's pull-ups (3.3 V, WIRING.md
  section 4) and the pin's weak pull-up make the high level. Drive 4 mA, slow slew (XDC).
* The microSD's DAT1/DAT2 and the PS/2 connector's spare pair are "parked": inputs with
  pull-ups and nothing behind them, so they do not fight the adapters' pull-ups (Vivado's
  default for unused pins is a pull-down).
* With `HAS_DDR = 0` the SoC answers the DDR window with errors and its DDR master port is idle
  (ready, response and calibration inputs tied low). The DDR3L pins are not in this top; the
  DDR3L's RESET# has its own pull-down on the board, so the memory stays in reset. The DDR
  designs build the same top with `ARTY_HAS_DDR` (section 7).

The soc track's `rtl/soc/surfcpu_soc.v` is merged. `sim/board/stubs/surfcpu_soc.v`, a stand-in
with exactly the contract's parameters and ports and its outputs at idle levels, is used by
make only if that file is absent, and `tools/fpga-build.sh` never builds with it. `tb_top`
checks the clocks, resets and parameters the SoC receives, every input pin into its SoC port
(200 random patterns), the open-drain PS/2 lines both ways and the RESET button (Icarus on
seeds 1, 7, 123, Verilator `--timing` on seed 5); with the stand-in it also drives every SoC
output onto its pin.

Two designs build this top. Their lists are identical but for the boot ROM image (the `rom`
line; `board-tools` compares them), and both images come from `make build-rom` (fw track):

* `surfcpu_arty_a7_top`: SurfBoot. Without DDR it reports the DDR controller absent and stops
  in its monitor, which still works the CPU, the UART both ways, the card over SPI and the VGA
  console (section 12). Not the PS/2 port: no ROM program reads the keyboard yet (SurfBoot's
  monitor takes input from the UART only, FIRMWARE.md).
* `surfcpu_arty_a7_bramdiag`: the BRAM diagnostics (`firmware/diagnostics/bramdiag.c`), the
  milestone 3 design. It needs only the CPU, block RAM, the UART and the timer, checks them
  (203 checks: the instructions, CSRs, scratch RAM with March C-, every exception, the
  interrupts and WFI, FENCE.I) and prints PASS or FAIL.

## 7. The DDR top (`ARTY_HAS_DDR`)

`surfcpu_arty_a7_top.v` built with the define `ARTY_HAS_DDR` (the DDR design lists' `define`
line: Yosys `-D`, Vivado `-verilog_define`) is the top of the two DDR designs. The define adds
the DDR3L ports and the controller and makes `HAS_DDR` 1 (a localparam that follows the port
list, so the two cannot disagree); without it the top is exactly the HAS_DDR = 0 top of section
6. The ddr track wrote this variant (docs/DDR.md section 10):

* **The path**: `surfcpu_soc`'s DDR master port -> `surf_ddr_bridge` (sys_clk to the
  controller's clock; `TIMEOUT_CYCLES` 4096, `CALIB_TIMEOUT_CYCLES` from `DDR_CALIB_LIMIT`) ->
  `surf_ddr_litedram_adapter` -> `litedram_core`, the LiteDRAM controller and Artix-7 PHY
  generated from `ip/litedram/arty.yml` (`make litedram-gen`; a build product in
  `build/litedram/arty/`, never committed: `ip/litedram/arty.sha256` holds the SHA-256 of the
  files the designs read, and `tools/fpga-build.sh` counts a build from files that differ as
  dirty). Since round ddr-3 the core has no calibration CPU, and a second path reaches its
  control registers: the SoC's `ddrc` master port (`DDR_SWINIT` 1) -> `surf_ddr_csr` (sys_clk
  to the controller's clock, whole words only, a watchdog on each side) -> the core's Wishbone
  control port `wb_ctrl`. SurfBoot and the BRAM diagnostics initialize the DDR3L and level its
  read path through it (docs/DDR.md 10.1, FIRMWARE.md section 5).
* **Clocks**: one input buffer on the oscillator pin E3 (synthesis puts it on `clk100`). It feeds
  `arty_a7_clocks`' PLL and MMCM as before and, through one more BUFG (`u_dram_clkbuf`),
  LiteDRAM's PLLE2_ADV. The generator takes `clk` straight into its PLL with no buffer of its
  own, so the board top chooses: through a BUFG the third clock tile may sit in any CMT, with no
  dedicated-route constraint, and the pin has one IBUF. LiteDRAM's PLL (VCO 1600 MHz) makes 80
  MHz (its core, user port and control port: the bridge's `ui_clk`, `surf_ddr_csr`'s `wb_clk`,
  `dram_clk` in the netlist), 320 MHz and 320 MHz at 90 degrees (the PHY's serializers; DDR3 at
  640 MT/s) and 200 MHz (the IDELAYCTRL reference), each through its own BUFG. Round ddr-2's core
  ran at 100 MHz, which nextpnr cannot close (section 8). **Clock tiles: three** (two PLLE2, one
  MMCME2) in three of the 6 (100T) or 5 (35T) CMTs; BUFGs: 7 of 32; one IDELAYCTRL (bank 34).
* **Resets**: the RESET button resets all three tiles; LiteDRAM's own resets follow its PLL's
  lock, so its registers come back to their reset values after RESET and the firmware
  initializes it again. The bridge and `surf_ddr_csr` reset with sys_rst on one side and
  LiteDRAM's `user_rst` on the other (their never-reset in-flight flags keep the crossings
  consistent, docs/DDR.md section 5).
* **Calibration status**: the firmware writes `init_done` and, on a failure, `init_error`,
  through `surf_ddr_csr`; the adapter turns them into `init_calib_complete` (done and not
  error), which the bridge synchronizes into sys_clk as `calib_done`, or turns into
  `calib_fail` when calibration has not succeeded `DDR_CALIB_LIMIT` sys_clk cycles (1.5 s)
  after reset, the firmware's start and its initialization included. Both reach
  `sysinfo.DDR_STATUS` (CALIB_DONE, CALIB_FAIL) through the SoC's ports. The core's user port
  stays blocked until init_done without init_error (LiteDRAM's `block_until_ready`).
* **Pins**: `arty_a7_ddr3_pins.xdc` (WIRING.md section 2.5): Digilent's MIG pin list, SSTL135
  and DIFF_SSTL135, fast slew, UNTUNED_SPLIT_40 termination on DQ and DQS, and bank 34's
  internal VREF (0.675 V, `arty_a7_ddr3_timing.xdc`). The controller's own console on ChipKit
  IO1 (`dram_uart_tx`, round ddr-2) is gone with its CPU: the read leveling's report is on the
  board's UART, from the BRAM diagnostics (section 12).
* **Constraints and CDC**: `arty_a7_ddr3_timing.xdc` declares the DDR3L pins' paths false (the
  PHY's serializers and the firmware's read leveling own their timing; the fabric paths into the
  I/O tiles stay timed) and sets the VREF; the bridge's crossings are bounded by its scoped
  `rtl/soc/ddr/surf_ddr_bridge.xdc` (`cdc surf_ddr_bridge`, seven `set_max_delay
  -datapath_only`), `surf_ddr_csr`'s by `rtl/soc/ddr/surf_ddr_csr.xdc` (`cdc surf_ddr_csr`,
  six); LiteX's own reset synchronizers by the generated `litedram_core.xdc`.
  **The open flow checks none of these**: nextpnr's XDC reader ignores `set_max_delay`,
  `set_false_path` and `INTERNAL_VREF` (it sets the bank's VREF to 675 mV by itself when an
  SSTL135 input is used); they are listed as not checked in every build (34 lines for a DDR
  design). nextpnr times the paths between sys_clk and LiteDRAM's clock as unconstrained
  cross-domain paths and reports their delays (about 1.5 ns) without a requirement; only Vivado
  checks the bounds.
* **Lint and simulation**: `make lint` lints the DDR top with `sim/board/stubs/litedram_core.v`,
  the generated core's ports without behaviour (`inspect_core.py` checks they match). `make
  board-ddr-sim` runs the DDR top in RTL with LiteDRAM's own controller and DRAM model, and with
  the board's generated core (section 12); `make board-netlist-sim` takes a DDR design's
  netlist too.

## 8. Building a bitstream

```sh
make build-fpga BOARD=arty_a7_100t DESIGN=arty_a7_baseline      # milestone 1
make build-fpga BOARD=arty_a7_100t                              # the whole SoC (needs build/fw/surfboot.mem)
tools/fpga-build.sh --dry-run --design arty_a7_baseline arty_a7_35t   # or: make build-fpga ... DRY_RUN=1
make build-fpga FLOW=openxc7 BOARD=arty_a7_35t DESIGN=arty_a7_baseline   # the open flow, on this Mac
```

`tools/fpga-build.sh` takes `FLOW` from the environment (`make build-fpga FLOW=...`) or else
from `~/.config/surfcpu/build-host.conf` (BUILD-HOST.md), which also holds the Vivado flows'
settings; `FLOW=openxc7` needs no such file:

| Key | Flows | Meaning |
|---|---|---|
| `FLOW` | all | `vivado-ssh`, `vivado-docker` or `openxc7` (the open flow, `tools/openxc7-build.sh`; below) |
| `HOST` | vivado-ssh | an ssh alias; key authentication only (`BatchMode=yes`), and its host key must already be known (`StrictHostKeyChecking=yes`) |
| `REMOTE_DIR` | vivado-ssh | working directory on the host (default `surfcpu-build`) |
| `VIVADO` | vivado-* | the pinned Vivado binary |
| `IMAGE` | vivado-docker | an x86-64 Linux image containing Vivado (new in this track; BUILD-HOST.md lists only the first four keys) |
| `DOCKER` | vivado-docker | the docker binary (default `docker`) |

Values are checked against a safe character set before they reach a shell. The script stages
the design (`boards/arty_a7/designs/<design>.files`: sources, include directories, `$readmemh`
files, constraints, scoped CDC files, generics) with `build.tcl` and a generated `params.tcl`
in `build/fpga/<board>/<design>/stage`, then runs `vivado -mode batch -source
surfcpu/boards/arty_a7/vivado/build.tcl -tclargs params.tcl` there: over ssh (copying the
stage with tar, fetching `out/` back without checkpoints), or in an amd64 container
(`docker run --platform linux/amd64`). `--dry-run` stages and prints every command, contacting
nothing. A new build removes the previous bitstream and results first. The build counts as
dirty if a tracked file changed or a file it stages is not in git.

Over ssh, every connection gives up after 20 s to connect or two minutes of silence
(`ConnectTimeout=20`, `ServerAliveInterval=30`, `ServerAliveCountMax=4`), and Vivado runs
under `nohup`, writing its pid, output and exit status to `out/` on the host. A connection
lost during the run (the Mac sleeps, the network changes) therefore does not stop Vivado:
the script exits 3, and `tools/fpga-build.sh --fetch [--design D] BOARD` later brings back
the results of that run and gates them as usual (exit 4 while Vivado still runs). The
manifest of a run that has not been fetched has no result, so `tools/program.sh` refuses it.

**The open flow** (`FLOW=openxc7`, docs/OPEN-FLOW.md) runs on this Mac in the
`surfcpu-openxc7` image (`make openxc7-image`). The script turns the design list into
`tools/openxc7-build.sh`'s arguments: `--top`, the sources, `-I` for each include directory,
the top's build values as defines (`ARTY_BUILD_BOARD`, `ARTY_BUILD_ID`, `ARTY_BUILD_FLAGS`,
`ARTY_BUILD_TIME`, `ARTY_BOOTROM_INIT`: the open flow has no option for top-level parameters,
so the tops take these defines as their parameters' defaults, and Vivado's generics still
override them), `--board`, `--dsp safe` (Yosys's DSP packing can delete logic, OPEN-FLOW.md
6.1) and `--out build/fpga/<board>/<design>/openxc7`. nextpnr's XDC reader applies only pin
placement and options and `create_clock`, and passes over the rest, mostly with a warning,
sometimes silently; so `boards/arty_a7/nextpnr-xdc.awk` gives it only what it applies:
`PACKAGE_PIN`, `IOSTANDARD`, `DRIVE`, `PULLTYPE` (Vivado's `PULLUP true` becomes `PULLTYPE
PULLUP`: nextpnr 1.0.0 drops `PULLUP true` without a word, as a probe build's FASM showed)
and the oscillator's `create_clock`. Everything else is **not checked by the open flow** and
is listed in `nextpnr-not-checked.txt`, in the build's output and in the manifest: the
derived clocks' names, the false paths, the I/O delays (SPI, VGA), `IOB TRUE` (the SPI
registers stay in the fabric), the configuration properties (`CFGBVS`, `CONFIG_VOLTAGE`,
compression: the bitstream keeps its defaults) and every scoped CDC file (the
`set_max_delay -datapath_only` bounds and `ASYNC_REG`): for the baseline, 11 entries; for the
SoC, 24. The build passes only if `tools/openxc7-build.sh` exits 0, all six of its checks pass
(synthesis, bitstream, round trip, block RAM, LVS, clocks; block RAM and clocks may be absent
from a design, but an LVS that could not be proved fails), every clock meets nextpnr's timing
estimate, and, for a design with a boot ROM or a `dsp` line, the board's two checks of the
SoC builds pass (below). It is not Vivado signoff; OPEN-FLOW.md section 6 lists what it cannot
show.

**Milestone 1 bitstreams, open flow** (evidence `openxc7.txt`, 2026-10-16, source 10807ee1,
clean): `make build-fpga FLOW=openxc7 DESIGN=arty_a7_baseline` for each board, with no
build-host.conf.

| | arty_a7_100t | arty_a7_35t (placed as xc7a35tcsg324-1) |
|---|---|---|
| result, wall clock | pass, 23 s | pass, 19 s |
| bitstream | 3,825,952 bytes | 2,192,174 bytes |
| IDCODE in the bitstream | 0x03631093 (XC7A100T) | 0x0362D093 (XC7A35T) |
| checks | synthesis, bitstream, round trip, LVS (429 proved, 0 differ) and clocks PASS; no block RAM | the same |
| clock tiles (cmtcheck) | MMCM 2 / 18.125 / 36 and PLL 1 / 10 / 40: 34 fields as XAPP888 | the same |
| timing, nextpnr's estimate | pix_clk 202.6 MHz, sys_clk 122.3 MHz (need 25.17 and 25) | 203.1 and 135.4 MHz |
| Yosys LUTs, flip-flops | 303, 412 (0.5 %) | 293, 412 (1.4 %) |

The SHA-256 of each file is in the evidence; it changes with every build, because the build
identity and time are part of the banner (and so the LUT count moves by a few). Not loaded on a
board: that is the owner's step (section 9), with the baseline as the first design because it
drives no Pmod pin (WIRING.md U2).

**The whole SoC, open flow** (evidence `openxc7-soc.txt`, 2026-10-16, source e3961772, clean;
HAS_DDR = 0): `make build-rom`, then `make build-fpga FLOW=openxc7 BOARD=<board>
DESIGN=<design>` for both SoC designs on both boards. Their design lists add three kinds of
line: `rom FILE INSTANCE` (the boot ROM image, relative to `surfcpu/`, and the ROM's instance;
required with `generic BOOTROM_INIT`, and `ROM=` overrides the file), `dsp INSTANCE N` (the
multiplier's DSP48E1 count) and `lvs-timeout S` (LVS of the SoC takes 9 to 12 minutes; the
lists allow 7200 s). The ROM is copied to `build/fpga/<board>/<design>/rom/` first, and
synthesis, the manifest's `rom_sha256` and the checks all read that copy, which stays beside
the bitstream: a `make build-rom` during a build cannot slip a second ROM into it (a trial
build showed it could, and the boot ROM check caught it). After the flow's six checks,
`tools/fpga-build.sh` runs two of its own on the SoC:

* **netlist** (`boards/arty_a7/netlist_check.py`): every memory of the elaborated RTL is still in
  RAM cells of the synthesized netlist, with at least its bits (boot ROM 15 RAMB36E1, scratch
  RAM 16, text RAM 4, font ROM 1; the register file in 12 RAM32M, the fetch, UART and PS/2
  FIFOs in 15 RAM32M and 3 RAM64M), and the 4 DSP48E1 the multiplier needs under `--dsp safe`
  take their operands from `u_soc.u_core.muldiv`;
* **boot ROM** (`boards/arty_a7/romcheck.py`): the flow's Yosys synthesizes `surf_bootrom` alone
  from the `.mem` with the flow's commands; its 15 blocks must be the design's ROM blocks
  (contents, widths and modes), and Icarus reads all 16384 words of that ROM netlist on both
  ports, through the block RAM models of `sim/board/prims` (Yosys's have no behaviour), against
  the `.mem`. With bramcheck (the bitstream's block RAM contents are the netlist's, block for
  block) this carries the `.mem` file into the bitstream.

| | bramdiag, 100T | bramdiag, 35T | SurfBoot (top), 100T | SurfBoot (top), 35T |
|---|---|---|---|---|
| result, wall clock | pass, 746 s | pass, 745 s | pass, 878 s | pass, 649 s |
| bitstream | 3,825,963 bytes | 2,192,185 bytes | 3,825,958 bytes | 2,192,180 bytes |
| SHA-256, first 12 digits | `92523a72a3ae` | `d34e495e2153` | `a9d016b78b5e` | `278a71a75b50` |
| boot ROM, SHA-256 first 12 digits | `bramdiag.mem`, `28c25a5dadbd` | `bramdiag.mem`, `28c25a5dadbd` | `surfboot.mem`, `7f3bf1a84179` | `surfboot.mem`, `7f3bf1a84179` |
| checks | the six of the flow, netlist and boot ROM: PASS | the same | the same | the same |
| LVS | 6119 proved, 0 differ (554 s) | 6119 proved, 0 differ (570 s) | 6119 proved, 0 differ (686 s) | 6119 proved, 0 differ (463 s) |
| timing, nextpnr's estimate (need 25.00 and 25.17 MHz) | sys 62.2, pix 133.5 MHz | sys 67.8, pix 161.6 MHz | sys 70.1, pix 149.4 MHz | sys 64.3, pix 157.9 MHz |
| Yosys LUTs, flip-flops | 4,261 (6.7 %), 2,666 (2.1 %) | 4,215 (20.3 %), 2,666 (6.4 %) | 4,223 (6.7 %), 2,666 (2.1 %) | 4,275 (20.6 %), 2,666 (6.4 %) |
| block RAM, DSP48E1 | 36 of 135 (26.7 %), 4 of 240 | 36 of 50 (72.0 %), 4 of 90 | 36 of 135 (26.7 %), 4 of 240 | 36 of 50 (72.0 %), 4 of 90 |
| post-synthesis simulation (below) | `BRAMDIAG: PASS (203 checks)` at 164 ms, in 150 s; 919 UART characters as the RTL | `BRAMDIAG: PASS (203 checks)` at 164 ms, in 145 s; 918 UART characters as the RTL | SurfBoot stops in its monitor (LED code 1) at 51 ms, in 46 s; 499 UART characters as the RTL | SurfBoot stops in its monitor (LED code 1) at 51 ms, in 46 s; 498 UART characters as the RTL |

The bitstreams carry BUILD_ID e3961772 (clean); their full SHA-256 and the manifests' are in the
evidence. Each one passes `make program-volatile` against the fake openFPGALoader and is refused
on the other board (section 9). The 35T fits: a fifth of its LUTs and 36 of its 50 block RAMs
(section 10).

nextpnr's critical paths, each with more than twice the margin needed:

* sys_clk: 14.3 to 16.1 ns of its 40 ns (62.2 to 70.1 MHz), 2.6 to 3.0 ns of it logic and the
  rest routing, into the CPU's pipeline control (`id_advance` in `rtl/cpu/surfcpu_core.v`) or
  the fetch FIFO's flush, from its instruction and write-back registers or a UART FIFO pointer;
* pix_clk: 6.2 to 7.5 ns of its 39.7 ns (133.5 to 161.6 MHz), the text address arithmetic of
  `rtl/peripherals/surf_video.v` (scroll row to cell index).

(Figures of the builds at e3961772; earlier builds of the same RTL placed differently, sys_clk
57.2 to 72.4 MHz.) nextpnr also reports the paths between the two clock domains (1.0 to 1.8 ns)
but checks them against nothing: the crossings are the synchronizers of section 4, whose bounds
only Vivado checks. LVS matches all 2666 registers by name, checks 126 memories at their boundary
and the 4 DSP48E1 and 36 RAMB36E1 at their pins; it does not model the MMCM and PLL, whose
settings cmtcheck checks.

**Post-synthesis simulation** (`make board-netlist-sim BOARD=<board> DESIGN=<design>`,
`sim/board/tb_netlist.v`): the synthesized netlist of a build, the one LVS proves the bitstream
equal to, with its ROM inside as block RAM contents, runs from its pins in Verilator with
Yosys's cell models and the block RAM and clock tile models of `sim/board/prims`, clocked at
100 MHz on `clk100`; `uart_tx` is decoded at 115200 8N1. All four netlists print exactly the
characters the RTL simulation prints (section 12) and stop where it stops: the two bramdiag
netlists run all 203 checks to `BRAMDIAG: PASS` with LD4-LD7 lit, in about 150 s for 164 ms of
simulated time (1.1 ms a second, about 27,000 sys_clk cycles); the two SurfBoot netlists report
the DDR controller absent and stop in the monitor with LED code 1, in about 45 s; the VGA syncs
run at 31.779 µs and 16.684 ms (640 x 480 at 59.94 Hz). Icarus runs the same netlist about 90
times slower (12 µs of simulated time a second: the banner line after 7 minutes). This catches
synthesis breaking the logic, which RTL simulation cannot; it does not cover timing (nextpnr's
estimate only), the real clock tiles or the I/O. A DDR design's netlist runs the same way, with
models of LiteDRAM's primitives and nothing on its memory pins (26 minutes for the diagnostics'
0.42 s, docs/DDR.md 10.4).

**What the first SoC builds found** (trial builds of the same RTL, before these): four gaps in
the flow's checkers and LVS, all on correct bitstreams, fixed in the flow track's files
(OPEN-FLOW.md 6.5): synthcheck credited the output registers to the top instead of
`u_soc`; fasmcheck called the all-clear NOCLKINV of an empty CLB tile unexplained; bramcheck did
not pair the two RAMB18 halves a RAMB36 decodes into; and LVS could not prove a pin fed by an
undefined (`x`) bit of the fetch FIFO's unused width, now tied to 0 by `setundef -zero` after
synthesis. And a `make build-rom` run during a build rewrote the ROM it was reading: the boot
ROM check failed (7 of the 15 ROM blocks differed from the new file), which led to the ROM copy.

**The DDR designs, open flow** (evidence `openxc7-ddr.txt`, 2026-10-16, source 92a2b510,
clean; HAS_DDR = 1): `make litedram-gen`, then `make build-fpga FLOW=openxc7 BOARD=<board>
DESIGN=surfcpu_arty_a7_ddr_bramdiag` (or `surfcpu_arty_a7_ddr`). Their lists add to the SoC
lists `define ARTY_HAS_DDR`, the generated LiteDRAM core and its constraints checked through
`generated ip/litedram/arty.sha256`, the DDR3L XDC files, `cdc surf_ddr_bridge`, `cdc
surf_ddr_csr`, `lvs-timeout 14400` and `seed 1 2 3 4 5 6 7 8`. Since round ddr-3 the core has
no calibration CPU and runs at 80 MHz (section 7; docs/DDR.md 10.3 for why). **All four builds
pass every check**: the flow's six, the board's netlist and boot ROM checks, and every clock
after routing:

| | ddr_bramdiag, 100T | ddr_bramdiag, 35T | ddr (SurfBoot), 100T | ddr (SurfBoot), 35T |
|---|---|---|---|---|
| result | pass | pass | pass | pass |
| placement (seeds tried) | seed 1 | seed 1 | seed 1 | seed 1 |
| routing | completed | completed | completed | completed |
| `dram_clk` fmax, slack (need 80 MHz) | 81.72 MHz, +0.263 ns | 84.94 MHz, +0.727 ns | 88.04 MHz, +1.141 ns | 93.35 MHz, +1.788 ns |
| `sys_clk` fmax, slack (need 25 MHz) | 61.39 MHz, +23.712 ns | 71.66 MHz, +26.045 ns | 56.55 MHz, +22.318 ns | 74.37 MHz, +26.554 ns |
| `pix_clk` fmax, slack (need 25.17 MHz) | 135.03 MHz, +32.318 ns | 153.68 MHz, +33.217 ns | 146.63 MHz, +32.904 ns | 150.63 MHz, +33.085 ns |
| `u_dram.iodelay_clk` fmax, slack (need 200 MHz) | 352.11 MHz, +2.160 ns | 350.63 MHz, +2.148 ns | 346.14 MHz, +2.111 ns | 392.31 MHz, +2.451 ns |
| `clk100_g` fmax, slack (need 100 MHz) | 1013.17 MHz, +9.013 ns | 1028.81 MHz, +9.028 ns | 990.10 MHz, +8.990 ns | 990.10 MHz, +8.990 ns |
| checks: synthesis, bitstream, round trip, block RAM, LVS, clocks; netlist, boot ROM | all PASS | all PASS | all PASS | all PASS |
| LVS | 10878 proved, 0 differ (998 s) | 10878 proved, 0 differ (1134 s) | 10878 proved, 0 differ (1088 s) | 10878 proved, 0 differ (1031 s) |
| bitstream SHA-256 | `e96e12eff3cf44402d2aa4636c9f34145f929a7c8f150123add8dea5e942d790` | `0912e8cdecd0a6b8b12a32fedcd8f450980d83819f8ea848cb10b602ad257606` | `180555d7527922e9b865463b4abfc03fc8eccf851364c92faf26142bf6d69f5d` | `5458de0e547e0a0783246ccbe0bc4e8770e0f991c77ba284bc3d4e9daeb8dc36` |
| bytes | 3,825,967 | 2,192,189 | 3,825,958 | 2,192,180 |
| LUTs, flip-flops (Yosys) | 7,604 (12.0 %), 5,214 (4.1 %) | 7,663 (36.8 %), 5,214 (12.5 %) | 7,672 (12.1 %), 5,214 (4.1 %) | 7,603 (36.6 %), 5,214 (12.5 %) |
| block RAM (RAMB36), DSP48E1 | 36 of 135 (26.7 %), 4 of 240 | 36 of 50 (72.0 %), 4 of 90 | 36 of 135 (26.7 %), 4 of 240 | 36 of 50 (72.0 %), 4 of 90 |

The bitstreams carry BUILD_ID 92a2b510 (clean); their full SHA-256 and the manifests' are in the
evidence. Each passes `make program-volatile` against the fake openFPGALoader and is refused on
the other board. The SurfBoot design differs from the diagnostics design only in its boot ROM's
contents, so their builds differ only through nextpnr's placement. The first gated builds of
the diagnostics design, at 0964f875, passed too, with seed 1 on both boards (`dram_clk` 90.62
and 82.81 MHz).

nextpnr's critical paths: `dram_clk`'s is LiteDRAM's controller every time, from the ZQ
calibration timer through the refresher and the command multiplexer into a bank machine, 14 to
19 LUT levels deep with 2.4 to 3.4 ns of logic, so its margin at 80 MHz is between +0.26 and
+1.79 ns over the four builds and depends on the placement: that is why the lists give eight
seeds, tried in turn with the same gate (OPEN-FLOW.md section 3; each attempt is in the build's
`seeds.txt` and manifest). sys_clk's and pix_clk's paths are the SoC's, as without DDR.
LiteDRAM's 320 MHz clocks reach only the serializers' clock pins: nextpnr derives them but has
no register-to-register path on them to time. The 200 MHz IDELAYCTRL reference clocks only its
reset logic. nextpnr also reports the paths between sys_clk and `dram_clk` (the bridge's and
`surf_ddr_csr`'s crossings) but checks them against nothing; only Vivado checks their bounds.

| Use (Yosys 0.69, `--dsp safe`; the four builds) | 100T | 35T |
|---|---|---|
| LUTs (Yosys LUT1-6 and INV) | 7,604 to 7,672 (12.0 %) | 7,603 to 7,663 (36.8 %) |
| flip-flops | 5,214 (4.1 %) | 5,214 (12.5 %) |
| block RAM (RAMB36 + RAMB18 / 2): the SoC's; LiteDRAM has none since round ddr-3 | 36 of 135 (26.7 %) | 36 of 50 (72 %) |
| DSP48E1 (the SoC's multiplier) | 4 | 4 |
| clock tiles: PLLE2 (the board's sys, LiteDRAM's), MMCME2 (pix) | 2 + 1 of 6 CMTs | 2 + 1 of 5 |
| BUFG | 7 of 32 | 7 of 32 |
| I/O logic: ISERDESE2, OSERDESE2, IDELAYE2, IDELAYCTRL | 16, 45, 16, 1 | the same |

Round ddr-2's DDR designs (LiteDRAM with its own VexRiscv and BIOS, at 100 MHz) never met
`dram_clk` (63.5 to 81.0 MHz), and one did not route, so they had no bitstream; diagnostic
builds that let nextpnr finish showed one LVS difference (WEBWE[3] of the CPU's tag RAM) and
two checks misreading correct results. All three were the checks' own errors, fixed in round
ddr-3 where they went wrong, each with a regression (OPEN-FLOW.md 6.5,
`docs/evidence/flow/checkers.txt`): the LVS extractor's reading of a RAMB18E1's byte write
enables, `cmtcheck.py`'s reading of Yosys's short bit strings, and `synthcheck.py`'s count of
registers merged into a block RAM's read port. The clock check now also compares every
output's phase: LiteDRAM's 90-degree clock for the DQS strobes, reported unchecked before,
matches XAPP888 in every build.

`build.tcl` (non-project): `read_verilog`, `read_mem`, `read_xdc` (then the CDC files scoped and
late), `synth_design` with the generics and `-flatten_hierarchy rebuilt`, `opt_design`,
`place_design`, `phys_opt_design`, `route_design`, a post-route checkpoint, then the reports
in `out/reports/`: timing summary (with unconstrained paths), utilization (flat and
hierarchical), DRC, clock interaction, CDC, methodology, clocks, I/O, exceptions, power and
`check_timing`. It writes the bitstream only if: setup and hold slack are not negative; no DRC
error or critical warning; no critical CDC finding; no `check_timing` finding other than the
deliberate false-path I/O; and no critical warning anywhere in the run. Nothing is waived; a
failed check fails the build with the reason. The two reports it reads as text are read
fail-closed: `check_timing.rpt` must have a section for each of the twelve checks UG835 lists
for `check_timing` (`no_clock` to `latch_loops`), each with its "There is/are N ..." lines,
and `cdc.rpt` its summary table (Severity, ID, Count); a missing section, header or number
fails the build rather than reading as "no findings", and a row starting with Critical counts
in any letter case.

**The manifest** `build/fpga/<board>/<design>/manifest.txt` (TOML), the same keys for every
flow: run id, design, top, board, part, flow, build host alias, Vivado binary (empty for the
open flow), source revision and dirty state, build ID and time, SHA-256 of the sources, of
the constraint files (in reading order), the ROM's source path and SHA-256 (`rom_image`,
`rom_sha256`: of the copy the build read) and of the kernel package if present,
ISA, ABI, map and SoC versions, platform ABI, the Mac; the result (`pass` or `fail` with a
`reason`) and, with a bitstream, its SHA-256 and size, which `tools/program.sh` compares with
the file. Then the flow's table: `[vivado]` from `out/build-results.txt` (Vivado version,
build host name, WNS, WHS, LUT/FF/BRAM/DSP/BUFG/MMCM/PLL/IO use, DRC errors, critical warnings,
critical CDC findings, `check_timing` issues) or `[openxc7]` (image and tool versions, the part
placed, the DSP mode, the six checks and the board's `check_netlist` and `check_boot_rom`,
nextpnr's timing per clock and the worst slack (or, when nextpnr stopped the flow,
`nextpnr_fmax`, its last report per clock, and `nextpnr_routed`: a build stopped before routing
converged has only the placer's estimates, and its reason says routing did not finish), use
against the part's documented capacity, the IDCODE and part in the bitstream, and the
constraints given and not checked).

**To confirm on the first real Vivado run** (none of this can be run here):
the generic syntax (`-generic BOARD="arty_a7_100t"`, `-generic BUILD_ID=32'h...`); the names
`build.tcl` reads from Vivado's reports (`check_timing`'s section titles and "There are N ..."
lines, the CDC summary's header and "Critical" rows, the utilization rows); that renaming the derived clocks with
`create_generated_clock -name` finds `u_clocks/u_sys_pll/CLKOUT0` after synthesis (the
PLLE2_BASE becomes a PLLE2_ADV); that the scoped CDC files see the derived clocks; and how
Vivado's CDC report classifies `surf_cdc_word`'s data bus (an enable-controlled capture after a
synchronizer). Any of these that fails stops the build, which is the intent: the fix is to
the script or the constraints, never a waiver to get a bitstream.

The wrappers are tested without Vivado (`make check`, `board-tools`): a fake `vivado` runs the
real `build.tcl` in Yosys's Tcl with stand-in Vivado commands, fake `ssh` and `docker` run
locally, a fake `openxc7-build.sh` writes the open flow's outputs (its dry runs use the real
script), and a fake `openFPGALoader` answers like the real one, with a temporary HOME and
output directory. 248 checks with fetch.sh's: refusals, dry runs, all three flows, the staged
files, `params.tcl`, the TOML manifest, each gate stopping the bitstream (a report format
Vivado might change among them), the dropped ssh connection and `--fetch`, the dirty flag
from a scratch git repository, the open flow's constraint subset and manifest, the SoC
designs' ROM selection, ROM copy and two board checks (a fake `romcheck`, a netlist that lost
a memory), and `tools/program.sh`'s cable, IDCODE, header and manifest checks.

## 9. Programming

```sh
make program-volatile BOARD=arty_a7_100t DESIGN=arty_a7_baseline     # configuration RAM only
make program-volatile BOARD=arty_a7_100t DESIGN=surfcpu_arty_a7_bramdiag   # then the SoC (section 12)
tools/program.sh detect arty_a7_100t
make program-volatile BOARD=arty_a7_100t CABLE_SERIAL=210319A28C7F    # with two boards on USB
```

`tools/program.sh` needs openFPGALoader on the Mac (`brew install openfpgaloader`, gap G3; it
exits 2 "pending" without it). Before loading anything it checks, and refuses on a mismatch:

* **The cable.** openFPGALoader takes the first FT2232H (USB 0403:6010) it finds, so with a
  second board or another FT2232H device connected it could load the wrong one. Unless a
  serial number is given (`CABLE_SERIAL=` or `--ftdi-serial`, passed on to every
  openFPGALoader call), `openFPGALoader --scan-usb` must list at most one.
* **The chain.** `openFPGALoader -c digilent --detect` must show exactly one device, with the
  board's IDCODE in bits 27:0: `0x0362D093` for the A7-35T, `0x03631093` for the A7-100T
  (UG470 v1.17 table 1-1; bits 31:28 are the silicon revision). openFPGALoader prints it as
  `idcode 0x%x`, without leading zeros (`0x362d093`), and for these parts without the revision.
* **The bitstream.** The part in the `.bit` header (field b), without `xc` and the speed grade,
  must be one of the board's `bit_parts` in `boards.txt`: device and package, so a bitstream
  for another package of the same die is refused. The A7-35T accepts `7a35ticsg324` (its part)
  and `7a35tcsg324`, which the open flow writes (Project X-Ray has no -1L part; same die,
  package and bitstream).
* **The manifest** next to the bitstream, if there is one: it must say every check passed and
  carry the file's SHA-256. `tools/openxc7-build.sh`'s own manifest (a flow track build) is
  accepted for a volatile load when every check line says PASS and every clock met nextpnr's
  timing (a diagnostic run that let nextpnr finish has its checks but a late clock).

`volatile` runs `openFPGALoader -b arty_a7_100t <bit>`: configuration RAM, lost at power-off
(PROG or a power cycle returns to the flash). `flash` (milestone 9) writes the configuration
flash with `-f --verify` only with `SURFCPU_FLASH_CONFIRM=write-flash-<board>` or the board name
typed at the prompt, and only a bitstream whose `tools/fpga-build.sh` manifest says `result =
"pass"` for this file (`bitstream_sha256`), this board and its part; the board's flash part
depends on its revision (RM §5.2: Micron N25Q128A13ESF40, Spansion S25FL128SAG[M|N]FI00 or
S25FL127SABMFx00; gap G7). Tested with a fake openFPGALoader that answers like the real one (in
the checks above), and the four SoC bitstreams of section 8 pass `make program-volatile BOARD=...
DESIGN=...` against it, each refused on the other board (evidence `openxc7-soc.txt`). On
2026-10-16 the owner's A7-100T took the baseline, the BRAM diagnostics and SurfBoot without DDR
through `make program-volatile` (PHYSICAL-TESTS.md, `docs/evidence/physical/`); the DDR designs
have not been loaded yet. The release (milestone 9) keeps a build record next to each bitstream,
so `program-flash` takes its bitstreams as they are; the flash image, the flash's address map,
the configuration time and the recovery procedure are in [RELEASE.md](RELEASE.md) sections 6 and
7.

## 10. Budgets

Yosys `synth_xilinx -family xc7` estimates (evidence `resources.txt`; Vivado will differ): the
baseline uses 290 LUTs, 412 flip-flops, no block RAM or DSP, 1 MMCM, 1 PLL and 2 BUFGs: 1.4 %
of the 35T's LUTs, 0.5 % of the 100T's. The whole SoC without DDR, as the open flow built it
(section 8; Yosys 0.69, `--dsp safe`): about 4,200 LUTs, 2,666 flip-flops, 36 RAMB36, 4 DSP48E1,
27 RAM32M and 3 RAM64M, 1 MMCM, 1 PLL and 2 BUFGs: on the 35T 20 % of the LUTs, 6.4 % of the
flip-flops, 72 % of the block RAM and 4.4 % of the DSPs; on the 100T 6.7 %, 2.1 %, 26.7 % and
1.7 %.

Block RAM implied by the memory map (a RAMB36 holds 1024 x 32 data bits):

| User | RAMB36 |
|---|---|
| boot ROM, 64 KiB, true dual port | 16 (Yosys: 15, using the parity bits in 4K x 9 mode) |
| scratch RAM, 64 KiB, true dual port | 16 |
| text RAM, 16 KiB, dual clock | 4 |
| font ROM, 4 KiB (256 x 16 x 8 bits) | 1 |
| register file, FIFOs (distributed RAM) | 0 |
| **total without DDR** | **37** (the open flow: 36) |
| LiteDRAM, in the DDR designs: none since round ddr-3 (no calibration CPU, BIOS ROM or SRAM; round ddr-2's took 10.5); its FIFOs are distributed RAM | 0 |
| **total with DDR** (the open flow) | **36** |

The XC7A35T has 50 RAMB36 (74 % used, 13 free; the open flow's 36 leave 14), the XC7A100T 135
(27 %, 98 free) (DS180 v2.6.1 table 4). The 35T fits, with the DDR3L controller too: 72 % of its
block RAM is used (14 free); caches or a frame buffer would soon need the 100T or a smaller boot
ROM. Clock resources: the SoC without DDR uses one MMCM, one PLL and two BUFGs; the
DDR designs add LiteDRAM's PLL (a second PLLE2), five BUFGs (the oscillator's for LiteDRAM's PLL
and LiteDRAM's four) and an IDELAYCTRL (section 7); the 35T has 5 CMTs and the 100T 6, each with
one MMCM and one PLL, and every 7-series device has 32 BUFGs (UG472). The DDR designs as the open
flow builds them (section 8): about 7,600 LUTs and 5,214 flip-flops, 37 % of the 35T's LUTs
(round ddr-2's, with LiteDRAM's own CPU, took about 10,300 and 6,430).

## 11. Make targets and tests

| Target | On | What |
|---|---|---|
| `lint-board` | `lint` | Verilator `-Wall` lint of `arty_a7_clocks`, the UART helpers, the baseline core and top, and the SoC top (with the stand-in or the real SoC), with and without `ARTY_HAS_DDR` (the DDR top with `sim/board/stubs/litedram_core.v`, the generated core's ports) |
| `test-board` | `test-soc` | `board-clocks` (Icarus and Verilator), `board-prims-bad`, `board-baseline-rt`, `board-baseline` (Verilator and Icarus), `board-top` (Icarus, 3 seeds, and Verilator) |
| `check-board` | `check` | `board-clockplan` (settings and generated files), `board-digilent` (the XDC files and licence; the manuals when fetched), `board-wiring` (pins, WIRING.md, master XDC and, for the DDR3L, Digilent's MIG projects, I/O timing, CDC names including the DDR bridges' mailboxes; self-test 23 mutations), `board-cdc-xdc` (15 checks), `board-tools` (248 wrapper and fetch.sh checks, the DDR designs' defines, generated files and seed lists among them) |
| `estimate-board` | (explicit) | Yosys estimates and the block RAM budget into `docs/evidence/board/resources.txt` |
| `board-uart-expect` | (explicit) | what the serial port should show with the two SoC designs, from RTL simulation (`BOARD=`; section 12) |
| `board-netlist-sim` | (explicit) | the post-synthesis simulation of an open-flow build of the SoC (`BOARD=`, `DESIGN=`, a DDR design too; section 8) |
| `board-ddr-sim` | (explicit) | the DDR top in RTL (section 7): with LiteDRAM's controller and DRAM model the BRAM diagnostics initializing it and running their DDR tests, and SurfBoot initializing it; with the generated board core (no CPU) and no memory, the diagnostics to a failed read leveling (`BOARD=`; needs `make litedram-gen`; about 10 minutes) |
| `board-uart-expect-ddr` | (explicit) | SurfBoot booting SurfOS with the C++ DDR model, the SoC as the DDR designs configure it (`BOARD=`; section 12) |
| `sim/board/openxc7_ddr_evidence.sh` | (Mac, explicit) | the DDR designs through the open flow on both boards and `program-volatile` with the fake programmer: `docs/evidence/board/openxc7-ddr.txt` (about 50 minutes) |
| `sim/board/ddr_sim_evidence.sh` | (Mac, explicit) | `board-ddr-sim`, `board-uart-expect-ddr` and, with `DDR_SIM_NETLIST=1` or `--netlist-only`, the DDR diagnostics build's netlist: `docs/evidence/ddr/ddr-top-sim.txt`, `docs/evidence/board/uart-expect-ddr.txt` |
| `sim/board/openxc7_soc_evidence.sh` | (Mac, explicit) | both SoC designs for both boards with the open flow, both simulations and `program-volatile` with the fake programmer: `docs/evidence/board/openxc7-soc.txt`, `uart-expect.txt` (about 40 minutes) |

## 12. First power-up

A checklist for the owner. WIRING.md has the reasons and the adapters' pin tables; of these rows
only JP2's has been tried on the board so far (2026-10-16). Two jumpers are called JP1: **the Arty's JP1 is its MODE
jumper**; the Pmod PS2 has its own JP1, the PS/2 supply selector. "JP1 off" in earlier reports
meant the Arty's MODE jumper.

**Before the first power-up** (USB unplugged, nothing in the Pmod headers):

| Jumper | Set to | Why |
|---|---|---|
| Arty JP1 (MODE) | **off** (no jumper) | the FPGA is then not configured from the Quad-SPI flash at power-up and waits for a JTAG load (RM section 4), so nothing old or unknown drives the Pmod pins (WIRING.md U2) |
| Arty JP2 (CK_RST to the FT2232) | **off** (no jumper) | fitted, the FT2232's DTR line resets the SoC whenever a program opens the serial port: seen on the owner's board, which came with JP2 fitted (WIRING.md U7) |
| Pmod PS2 JP1 (PS/2 supply) | **VB** (3.3 V from the Arty) | VE with a 5 V supply puts 5 V on FPGA pins (WIRING.md section 4) |
| Pmod PS2 JP2 (external supply header) | nothing connected | its silkscreen has VE and GND swapped (WIRING.md section 4) |

**Adapters**, each plugged in only with the board unpowered, component side up, adapter pin 1
at the header's pin 1, and checked before power is applied:

* Pmod MicroSD in JA, with no card in the slot until it is checked (WIRING.md U8);
* Pmod VGA across JB (its J1, red and blue) and JC (its J2, green and syncs); **upside down it
  can damage the FPGA** (WIRING.md U2);
* Pmod PS2 in the top row of JD (pins 1-6) (WIRING.md U5).

**The order of the designs.** Each is built with `make build-fpga FLOW=openxc7 BOARD=<board>
DESIGN=<design>` and loaded with `make program-volatile BOARD=<board> DESIGN=<design>`
(configuration RAM only: a power cycle or PROG forgets it). The serial port is the FT2232's
second port at 115200 8N1 (`make doctor` lists the ports; `screen /dev/cu.usbserial-<serial>
115200`). The DDR designs need `make litedram-gen` once before their build (the generated core
is a build product, checked against `ip/litedram/arty.sha256`). Steps 1 to 3 have run on the
owner's A7-100T (2026-10-16: m1 and m3 passed, SurfBoot without DDR stopped in its monitor as
expected and read the card's files; PHYSICAL-TESTS.md) with bitstreams of the earlier SoC 0.1.0,
and the owner's card already holds SurfOS (`make deploy-sd`); the first DDR test
repeats step 2 with this SoC (0.2.0, memory map 2: `tools/board-test.py m3`), then goes on to
steps 4 and 5.

1. **`arty_a7_baseline`, with no adapters plugged in.** It drives none of the Pmod pins. What
   it shows is in section 5: LD0 and LD1 blink green once a second, one serial line a second.
2. **`surfcpu_arty_a7_bramdiag`**, the milestone 3 design: the CPU and every block RAM, with
   only the USB cable needed (the adapters may stay out). It prints the lines below, about
   0.17 s after the load, and ends with LD4-LD7 on and LD0 green (LD0 red on a failure); then
   it echoes typed characters and blinks LD7.
3. **`surfcpu_arty_a7_top`** (SurfBoot), with the adapters plugged in as above, then a
   FAT32-formatted microSD card in the slot. With no DDR controller in this bitstream
   SurfBoot stops in its monitor (`DDR_ABSENT`, LED code 1: LD4 on, LD0 red), which still
   exercises the CPU, the UART in both directions, the card over SPI (`sd`, `part`, `fs`, `ls`)
   and the VGA console (the same text on the monitor). Inserting a card retries the boot. The
   PS/2 port is not exercised: no ROM program reads the keyboard yet, so with the Pmod PS2
   plugged in the design only keeps its lines released.
4. **`surfcpu_arty_a7_ddr_bramdiag`**, milestone 4: the DDR3L (sections 1 and 7). Same
   adapters as step 3, card optional; nothing on the ChipKit header (round ddr-2's controller
   console on IO1 is gone). After the CPU and RAM sections of step 2 (0.16 s) the diagnostics
   initialize the DDR3L themselves, through the controller's registers (`DDR_STATUS.SWINIT`):
   the PHY's reset, the DRAM's reset and clock enable, its four mode registers and ZQ
   calibration, then read leveling. For each byte lane (`m0`, `m1`) and bitslip (`b00` to
   `b07`) they print a line with a 1 for each of the 32 read delays that read the test
   patterns back, then each lane's choice, the middle of its widest run (about 0.25 s in all).
   **This is the first thing only the board can show**: a working DDR3L gives each lane a run
   of 1s on one bitslip line (a delay step is 78 ps and a bit at 640 MT/s lasts 1.56 ns, so a
   run can be up to about 20 long), `DDR initialization ok (3 checks)`, then the DDR tests and
   `BRAMDIAG: PASS (572 checks)` about 3 s after the load. A lane with no run fails its
   check, the diagnostics write `init_error` and skip the DDR tests (`BRAMDIAG: FAIL`); the
   transcript of all 16 scan lines is what to send back either way. To record it, start
   `python3 tools/uart.py capture auto --until 'BRAMDIAG: (PASS|FAIL)' --out <file>` before the
   load; or run `tools/board-test.py m3 --design surfcpu_arty_a7_ddr_bramdiag`, which checks
   every section, the build identity against this design's manifest and the echo (it asks for
   the design without DDR to be loaded: load this one instead).
5. **`surfcpu_arty_a7_ddr`** (SurfBoot), with a card that holds SurfOS (`make deploy-sd`).
   SurfBoot initializes and levels the DDR3L the same way, reports the lanes on one line
   (`ddr: initialized by SurfBoot, read leveling lane 0: ...; lane 1: ...`), tests the DDR3L
   briefly, loads `/BOOT/SURFOS.BIN` into it and starts SurfOS, which ends at its shell prompt.
   A failed leveling stops it in its monitor (`DDR initialization failed: ...`, `DDR_CALIB`, LED
   code 1); the monitor's `ddr` command initializes and tests again, and `info` shows the
   status (`present, software init, calibrated`). `tools/board-test.py m5` and `m7` with
   `--design surfcpu_arty_a7_ddr` are the acceptance tests of this step (PHYSICAL-TESTS.md).

On an A7-35T the open flow places the design on the whole xc7a50t die, possibly outside the
region Vivado offers on a 35T, which AMD does not document as usable (OPEN-FLOW.md 6.3). If a
35T misbehaves where the simulations pass, suspect that first; the 100T has no such question.

**What the serial port should show, expected from simulation** (never observed on a board yet).
These come from `make board-uart-expect BOARD=arty_a7_100t` (sim/board/uart_expect.sh): the soc
track's surfsim with `surfcpu_soc` configured exactly as `surfcpu_arty_a7_top` configures it on
that board, running the same ROM images; the full transcripts for both boards are in
`docs/evidence/board/uart-expect.txt`. On the 35T the board reads `Arty A7-35T (0x135)` and
`info` says `board 00000135`; nothing else differs. `<id>` is the build's commit (the ROM's and
the bitstream's; `, dirty tree` or `dirty` follows when built from uncommitted changes), and
the bitstream's build time replaces `time 0`.

`surfcpu_arty_a7_bramdiag`, all of it (about 0.17 s):

```

SurfCPU BRAM diagnostics 1.0.0 (build <id>)
board Arty A7-100T (0x100), SoC 0.2.0, memory map 2, sys 25000000 Hz, timebase 1000000 Hz
bitstream build <id> flags 0, features 7f, DDR status 0 size 0 MiB
identity                           ok (7 checks)
ALU, immediates, x0                ok (33 checks)
M extension                        ok (23 checks)
branches and jumps                 ok (9 checks)
loads and stores                   ok (8 checks)
CSRs and counters                  ok (30 checks)
scratch RAM (March C-)             (56 KiB) ok (49 checks)
text RAM (cells 2400-4095)         ok (1 checks)
exceptions, each recovered         ok (34 checks)
interrupts, timer and WFI          (10 timer ticks in 10186 us) ok (7 checks)
FENCE.I, code written to RAM       ok (2 checks)
DDR                                skipped: no DDR controller in this bitstream

BRAMDIAG: PASS (203 checks)
```

`surfcpu_arty_a7_top`, SurfBoot, with a card in the socket and `info`, `sd` and `ls /` typed at
the monitor. The `sd` and `ls /` answers describe the simulation's 64 MiB card (its manufacturer,
OEM, product and serial are the card model's, its files those `tools/mkcard.py` made); a real
card answers with its own:

```

SurfBoot 1.1.0 (build <id>), the SurfCPU boot ROM
soc: Arty A7-100T (0x100), SoC 0.2.0, memory map 2, 25000000 Hz; bitstream <id>
ddr: this bitstream has no DDR controller (DDR_STATUS 0x00000000): there is nowhere to load a kernel

SurfBoot stopped: no DDR controller (DDR_ABSENT)
  this bitstream has no DDR controller (DDR_STATUS 0x00000000): there is nowhere to load a kernel
The LEDs LD4-LD7 show code 1. Type help for the monitor's commands; inserting a card retries the boot.
surfboot> info
SurfBoot 1.1.0 build <id>
sysinfo: magic 46525553, SoC 00000200, map 2, board 00000100, bitstream build <id> flags 0, time 0
clocks: sys 25000000 Hz, timebase 1000000 Hz, pixel 25173611 Hz; features 7f (uart timer irq gpio spi ps2 video ddr)
ddr: status 0 (absent), size 0 MiB
card: present; buttons 0
last boot failure: DDR_ABSENT: this bitstream has no DDR controller (DDR_STATUS 0x00000000): there is nowhere to load a kernel
surfboot> sd
sd: SDHC/SDXC, sector addressing, OCR c0ff8000, ready after 5 ms, data CRC on
sd: SPI 390 kHz for identification, 6250 kHz for data (DIV 1)
sd: CSD version 2, 131072 sectors (64 MiB)
sd: CID manufacturer 5c, OEM SF, product SURFC rev 1.0, serial 0badf00d, made 2026-10
surfboot> ls /
sd: SDHC/SDXC, 64 MiB; ready after 5 ms; SPI 6250 kHz, data CRC checked
fat32: partition 1 (type 0x0c) at sector 2048, 512-byte clusters, 126976 clusters, label "SURFOS"
  (volume label SURFOS     )
  d----  BOOT               <dir>  cluster 3
  d----  SYSTEM             <dir>  cluster 5
  d----  PROGRAMS           <dir>  cluster 4
  d----  USERS              <dir>  cluster 6
  0 files (0 bytes), 4 directories
surfboot>
```

**The DDR designs, expected from simulation** (never observed on a board; no DDR3 model exists
here, so these come from LiteDRAM's controller with its behavioural DRAM model, from the exact
generated core with nothing on its memory pins, from the firmware emulator with a model of
LiteDRAM's registers whose lanes read back only inside a chosen window, and from the C++ DDR
model; docs/DDR.md 10.4, evidence `docs/evidence/ddr/ddr-top-sim.txt`,
`docs/evidence/fw/emulator.txt` and `docs/evidence/board/uart-expect-ddr.txt`).

`surfcpu_arty_a7_ddr_bramdiag` with a working DDR3L. The header and the eleven CPU and RAM lines
are those of `surfcpu_arty_a7_bramdiag` but for `features ff, DDR status 14 size 256 MiB` (`14`
in hexadecimal: PRESENT and SWINIT, not yet calibrated). The DDR part, from the emulator
(`firmware/tests/run_emu.py`, `bramdiag_initializes_ddr`) with its lanes set to read back at
bitslip 3, delays 6 to 26 (lane 0) and bitslip 5, delays 10 to 20 (lane 1). **On the board
the scan lines, the bitslips, delays and windows, the time and the copy rate are the board's
own**; everything else should read as here:

```
DDR initialization (DDR_STATUS.SWINIT), read leveling:
  m0, b00: |00000000000000000000000000000000| delays: -
  m0, b01: |00000000000000000000000000000000| delays: -
  m0, b02: |00000000000000000000000000000000| delays: -
  m0, b03: |00000011111111111111111111100000| delays: 16+-10
  m0, b04: |00000000000000000000000000000000| delays: -
  ... m0 b05 to b07, m1 b00 to b04: all 0 ...
  m1, b05: |00000000001111111111100000000000| delays: 15+-05
  m1, b06: |00000000000000000000000000000000| delays: -
  m1, b07: |00000000000000000000000000000000| delays: -
  lane 0: bitslip 3, delay 16 (window +-10)
  lane 1: bitslip 5, delay 15 (window +-5)
  245 ms
DDR initialization                 ok (3 checks)
DDR calibration                    ok (1 checks)
DDR data lines                     ok (192 checks)
DDR byte lanes                     ok (9 checks)
DDR address lines and aliases      ok (54 checks)
DDR boundaries                     ok (98 checks)
DDR patterns                       (1024 KiB in full, 4096 blocks sampled) ok (2 checks)
DDR sustained copies               (4 x 256 KiB copied and checked, 785 KiB/s) ok (4 checks)
code running from DDR              ok (6 checks)

BRAMDIAG: PASS (572 checks)
```

The board top in RTL with LiteDRAM's controller and DRAM model (`make board-ddr-sim`) prints the
same DDR test lines and `BRAMDIAG: PASS (570 checks)` at 2.90 s of simulated time; its model PHY has no
delays, so it prints `lane N: not leveled (the PHY has no delay registers)` and
`DDR initialization ok (1 checks)` instead of the scan. With the exact generated core and nothing
on the DDR3L pins (`make board-ddr-sim`, the board core), the scan is all 0s and the end is
the failure the board would print with no working memory:

```
  lane 0: bitslip 0, no working delay
  lane 1: bitslip 0, no working delay
  227 ms
DDR initialization                 
  FAIL lane 0: no read delay works at bitslip 0, the best
  FAIL lane 1: no read delay works at bitslip 0, the best
  FAIL read leveling did not succeed: init_error written
  DDR initialization: 3 of 3 checks failed

BRAMDIAG: FAIL (3 of 206 checks failed)
```

(`the best` is the bitslip whose run was longest; with no run at all it is 0.) The header's
`DDR status` is in hexadecimal: `14` before initialization, `15` (PRESENT, SWINIT and
CALIB_DONE) after it, and `16` (CALIB_FAIL) once the bridge's 1.5 s limit has passed without
calibration, as after a failed leveling; Ctrl-R runs the diagnostics again, initialization
included.

`surfcpu_arty_a7_ddr`, SurfBoot with a card holding SurfOS, through to the shell prompt (`make
board-uart-expect-ddr`: the soc track's surfsim with the C++ DDR model and LiteDRAM's register
model behind `ddrc`, whose lanes read back inside the same windows as above; the card's read time
is the simulation's card model's; 4.2 s in all). On the board the lanes, the
`calibrated after` time and the read time are the board's own:

```

SurfBoot 1.1.0 (build <id>), the SurfCPU boot ROM
soc: Arty A7-100T (0x100), SoC 0.2.0, memory map 2, 25000000 Hz; bitstream <id>
ddr: initialized by SurfBoot, read leveling lane 0: bitslip 3, delay 16 (window +-10); lane 1: bitslip 5, delay 15 (window +-5)
ddr: calibrated after 240 ms, 256 MiB, quick test passed (data, address, byte lanes)
sd: SDHC/SDXC, 64 MiB; ready after 5 ms; SPI 6250 kHz, data CRC checked
fat32: partition 1 (type 0x0c) at sector 2048, 512-byte clusters, 126976 clusters, label "SURFOS"
boot: /BOOT/SURFOS.BIN
boot: loading 373 KiB .....
boot: SurfOS 1.0.0 build <id>: 382148 bytes in 1 fragment, read in 1864 ms
boot: load 0x80000000, entry 0x80000000, mem_size 1182896, ISA IM_Zicsr_Zifencei; payload CRC 0x482ae7db matches (card and DDR)
boot: command line "ascii"
boot: starting the kernel at 0x80000000 (a0 = 0, a1 = 0x0001f000, a2 = 0x49465253)

SurfOS: serial console on the arty_a7 UART (115200 8N1)
Memory: kernel 0x80000000-0x80120caf, DMA heap 0x80121000-0x80220fff, kernel heap 0x80221000-0x8effffff (237 MB)
Booting SurfOS Kernel.....
  ... the kernel's start-up messages (docs/evidence/board/uart-expect-ddr.txt) ...
Brandon Burr's SurfOS
Version 1.0 on a RISC-V (SurfCPU on the Arty A7), 2004-2026
SURF ~#> 
```

(Sizes, CRCs and the build depend on the kernel built; the prompt is coloured.) With no card,
SurfBoot initializes the DDR3L, tests it and stops: `SurfBoot stopped: no card (NO_CARD)`, LED
code 2.

The VGA monitor shows the same console text, 80 x 30 characters at 640 x 480 and 59.94 Hz (the
screen the simulation decoded from the pixels is in the same evidence file). The post-synthesis
simulation of every one of the bitstreams' netlists printed the same characters up to its stop
(section 8), and the netlist of the 100T DDR diagnostics build, with nothing on its memory pins, printed the same 2,166 characters at the same microseconds as its RTL simulation with the generated core
and the same ROM (docs/DDR.md 10.4). If the board prints something else, or nothing: the serial
settings and port first, then `make doctor`; the LEDs say how far it got (bramdiag lights LD4,
then LD5, then LD6 as it moves through its sections, and all four with LD0 green or red at the
end; SurfBoot shows its error code on LD4-LD7 with LD0 red).
