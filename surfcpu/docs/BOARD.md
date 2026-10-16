# SurfCPU on the Arty A7: the board

The board track's part of SurfCPU: the clocks and resets, the pins and constraints, the
milestone 1 design, the top for the whole SoC, the Vivado and open-flow builds and the
programming wrapper.
Pins and adapters are in [WIRING.md](WIRING.md); the build host in [BUILD-HOST.md](BUILD-HOST.md).

**Status (2026-10-16).** Built into bitstreams for both boards with the open-source flow
(`make build-fpga FLOW=openxc7`), every check of that flow passing (section 8): the milestone 1
design, and the whole SoC without DDR in two designs, with the BRAM diagnostics (milestone 3)
or SurfBoot in its boot ROM; their timing is nextpnr's estimate. The SoC's synthesized netlist,
simulated from its pins, prints the same characters as the RTL: the diagnostics' PASS line and
SurfBoot's stop in its monitor. The two DDR designs (milestone 4, LiteDRAM, section 7) place
and, mostly, route, PHY included, but **miss nextpnr's timing for LiteDRAM's 100 MHz clock
(63 to 81 MHz), so the open flow gives them no bitstream** (section 8); in simulation their top
passes the BRAM diagnostics' DDR tests with LiteDRAM's controller and its DRAM model (section
12). **Nothing has been built with Vivado** (there is no Vivado host
yet, gap G2) and **nothing has been programmed** (the board is not connected, G1/G3): every
physical result is pending. Section 12 is the owner's checklist for the first power-up, with
what the serial port should show according to simulation. The first real run of `build.tcl`
will also be the first time Vivado reads these constraints; section 8 lists what that run must
confirm.

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
| `surfcpu_arty_a7_ddr_bramdiag` | the same top with `ARTY_HAS_DDR` | the same + the DDR3L (bank 34) + ChipKit IO1 | the SoC with the DDR3L memory (LiteDRAM, section 7) and the BRAM diagnostics, which then test the DDR3L: milestone 4 |
| `surfcpu_arty_a7_ddr` | the same | the same | the SoC with the DDR3L and SurfBoot, which loads SurfOS into it |

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
shield's RST pin and, through jumper JP2, the FT2232: keep JP2 open (WIRING.md U7).

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
  dirty).
* **Clocks**: one input buffer on the oscillator pin E3 (synthesis puts it on `clk100`). It
  feeds `arty_a7_clocks`' PLL and MMCM as before and, through one more BUFG
  (`u_dram_clkbuf`), LiteDRAM's PLLE2_ADV. The generator takes `clk` straight into its PLL with
  no buffer of its own, so the board top chooses: through a BUFG the third clock tile may sit
  in any CMT, with no dedicated-route constraint, and the pin has one IBUF. LiteDRAM's PLL
  (VCO 1600 MHz) makes 100 MHz (its core and user port: the bridge's `ui_clk`, `dram_clk` in
  the netlist), 400 MHz and 400 MHz at 90 degrees (the PHY's serializers; DDR3 at 800 MT/s) and
  200 MHz (the IDELAYCTRL reference), each through its own BUFG. **Clock tiles: three** (two
  PLLE2, one MMCME2) in three of the 6 (100T) or 5 (35T) CMTs; BUFGs: 7 of 32; one IDELAYCTRL
  (bank 34).
* **Resets**: the RESET button resets all three tiles; LiteDRAM's own resets follow its PLL's
  lock, so its BIOS calibrates again after RESET. The bridge resets with sys_rst on one side
  and LiteDRAM's `user_rst` on the other (its never-reset in-flight flags keep the crossing
  consistent, docs/DDR.md section 5).
* **Calibration status**: the core's BIOS writes `init_done` and, on a failure, `init_error`;
  the adapter turns them into `init_calib_complete` (done and not error), which the bridge
  synchronizes into sys_clk as `calib_done`, or turns into `calib_fail` when calibration has
  not succeeded `DDR_CALIB_LIMIT` sys_clk cycles after reset. Both reach
  `sysinfo.DDR_STATUS` (CALIB_DONE, CALIB_FAIL) through the SoC's ports. The core's user port
  stays blocked until init_done without init_error (LiteDRAM's `block_until_ready`).
* **Pins**: `arty_a7_ddr3_pins.xdc` (WIRING.md section 2.5): Digilent's MIG pin list, SSTL135
  and DIFF_SSTL135, fast slew, UNTUNED_SPLIT_40 termination on DQ and DQS, bank 34's internal
  VREF (0.675 V, `arty_a7_ddr3_timing.xdc`), and `dram_uart_tx` on ChipKit IO1: the
  controller's own console (115200 8N1, output only; its input is held idle so nobody can
  type into the controller). A 3.3 V USB-serial adapter there shows the BIOS's calibration
  report (section 12).
* **Constraints and CDC**: `arty_a7_ddr3_timing.xdc` declares the DDR3L pins' paths false (the
  PHY's serializers and the BIOS's read leveling own their timing; the fabric paths into the
  I/O tiles stay timed) and sets the VREF; the bridge's crossings are bounded by its scoped
  `rtl/soc/ddr/surf_ddr_bridge.xdc` (`cdc surf_ddr_bridge`, seven `set_max_delay
  -datapath_only`); LiteX's own reset synchronizers by the generated `litedram_core.xdc`.
  **The open flow checks none of these**: nextpnr's XDC reader ignores `set_max_delay`,
  `set_false_path` and `INTERNAL_VREF` (it sets the bank's VREF to 675 mV by itself when an
  SSTL135 input is used); they are listed as not checked in every build (34 lines for a DDR
  design). nextpnr times the paths between sys_clk and LiteDRAM's clock as unconstrained
  cross-domain paths and reports their delays (about 1.5 ns) without a requirement; only Vivado
  checks the bounds.
* **Lint and simulation**: `make lint` lints the DDR top with `sim/board/stubs/litedram_core.v`,
  the generated core's ports without behaviour (`inspect_core.py` checks they match). `make
  board-ddr-sim` runs the DDR top in RTL with LiteDRAM's own controller and DRAM model, and with
  the board's generated core and its BIOS (section 12); `make board-netlist-sim` takes a DDR
  design's netlist too.

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

**The whole SoC, open flow** (evidence `openxc7-soc.txt`, 2026-10-16, source 8c70845e, clean;
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
| result, wall clock | pass, 860 s | pass, 788 s | pass, 662 s | pass, 849 s |
| bitstream | 3,825,963 bytes | 2,192,185 bytes | 3,825,958 bytes | 2,192,180 bytes |
| SHA-256, first 12 digits | `8fc81197e777` | `e705662e35f9` | `ed76520d309b` | `cd2a9ef4835d` |
| boot ROM, SHA-256 first 12 digits | `bramdiag.mem`, `d12f2822193b` | the same | `surfboot.mem`, `82b5466e5582` | the same |
| checks | the six of the flow, netlist and boot ROM: PASS | the same | the same | the same |
| LVS | 6119 proved, 0 differ (649 s) | 6119, 0 (588 s) | 6119, 0 (447 s) | 6119, 0 (648 s) |
| timing, nextpnr's estimate (need 25.00 and 25.17 MHz) | sys 57.2, pix 152.3 MHz | sys 64.9, pix 157.0 | sys 65.5, pix 143.5 | sys 72.4, pix 158.4 |
| Yosys LUTs, flip-flops | 4222 (6.7 %), 2666 (2.1 %) | 4274 (20.5 %), 2666 (6.4 %) | 4235 (6.7 %), 2666 | 4199 (20.2 %), 2666 |
| block RAM, DSP48E1 | 36 of 135 (26.7 %), 4 of 240 | 36 of 50 (72 %), 4 of 90 | as the bramdiag build | as the bramdiag build |
| post-synthesis simulation (below) | `BRAMDIAG: PASS (203 checks)` at 163.8 ms, in 149 s | the same at 163.7 ms, in 147 s | SurfBoot stops in its monitor at 51.2 ms, in 45 s | the same, in 47 s |

The bitstreams carry BUILD_ID 8c70845e (clean); their full SHA-256 and the manifests' are in the
evidence. Each one passes `make program-volatile` against the fake openFPGALoader and is refused
on the other board (section 9). The 35T fits: a fifth of its LUTs and 36 of its 50 block RAMs
(section 10).

nextpnr's critical paths, each with more than twice the margin needed:

* sys_clk: 13.8 to 17.5 ns of its 40 ns (57.2 to 72.4 MHz), 2.3 to 3.1 ns of it logic and the
  rest routing, into the CPU's pipeline control (`id_advance`, `ex_advance` in
  `rtl/cpu/surfcpu_core.v`) from its instruction, CSR and write-back registers or the timer's
  `mtime`;
* pix_clk: 6.3 to 7.0 ns of its 39.7 ns (143.5 to 158.4 MHz), the text address arithmetic of
  `rtl/peripherals/surf_video.v` (scroll row to cell index) or its cursor compare.

nextpnr also reports the paths between the two clock domains (1.1 to 2.4 ns) but checks them
against nothing: the crossings are the synchronizers of section 4, whose bounds only Vivado
checks. LVS matches all 2666 registers by name, checks 126 memories at their boundary and the 4
DSP48E1 and 36 RAMB36E1 at their pins; it does not model the MMCM and PLL, whose settings
cmtcheck checks.

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
estimate only), the real clock tiles or the I/O.

**What the first SoC builds found** (trial builds of the same RTL, before these): four gaps in
the flow's checkers and LVS, all on correct bitstreams, fixed in the flow track's files
(OPEN-FLOW.md 6.5): synthcheck credited the output registers to the top instead of
`u_soc`; fasmcheck called the all-clear NOCLKINV of an empty CLB tile unexplained; bramcheck did
not pair the two RAMB18 halves a RAMB36 decodes into; and LVS could not prove a pin fed by an
undefined (`x`) bit of the fetch FIFO's unused width, now tied to 0 by `setundef -zero` after
synthesis. And a `make build-rom` run during a build rewrote the ROM it was reading: the boot
ROM check failed (7 of the 15 ROM blocks differed from the new file), which led to the ROM copy.

**The DDR designs, open flow** (evidence `openxc7-ddr.txt`, 2026-10-16: the gated builds at
source a338e7a3, clean, the diagnostic builds at 383dba74; HAS_DDR = 1): `make litedram-gen`,
then `make build-fpga FLOW=openxc7 BOARD=<board> DESIGN=surfcpu_arty_a7_ddr_bramdiag` (or
`surfcpu_arty_a7_ddr`). Their lists add to the SoC lists `define ARTY_HAS_DDR`, the generated
LiteDRAM files (core, `VexRiscv_Lite.v`, its BIOS ROM and SRAM images, its constraints) checked
through `generated ip/litedram/arty.sha256`, the DDR3L XDC files, `cdc surf_ddr_bridge`,
`lvs-timeout 14400` and `seed 2`. **None of the builds passes, so there is no DDR bitstream:
nextpnr's estimate for LiteDRAM's 100 MHz clock (`dram_clk`) misses it on every build that
routes**, and nextpnr stops the flow after routing; one build did not route at all:

| | ddr_bramdiag, 100T | ddr_bramdiag, 35T | ddr (SurfBoot), 100T | ddr, 35T |
|---|---|---|---|---|
| result, wall clock | fail: **not routed** (router2 stopped after 100 iterations), 1537 s | fail (timing), 183 s | fail (timing), 159 s | fail (timing), 159 s |
| `dram_clk`: LiteDRAM, its CPU, the adapter, the bridge's controller side (need 100 MHz) | (84.1 MHz, the placer's estimate) | **77.3 MHz** | **63.5 MHz** | **73.8 MHz** |
| sys_clk (need 25 MHz), pix_clk (need 25.17) | (77.8, 153.6) | 65.9, 134.6 | 66.6, 156.8 | 59.9, 116.3 |
| LiteDRAM's 200 MHz (IDELAYCTRL reset logic), the oscillator's BUFG (LiteDRAM's PLL reset counter) | (350.4, 1003.0) | 373.0, 742.9 | 374.1, 956.9 | 373.1, 780.6 |
| `dram_clk` in the previous run (383dba74), where all four routed | 73.0 MHz | 81.0 | 77.0 | 78.4 |
| synthesis check; the board's netlist and boot ROM checks | FAIL (a false finding, below); PASS, PASS | the same | the same | the same |

LiteDRAM's 400 MHz clocks reach only the serializers' clock pins: nextpnr derives them but has no
register-to-register path on them to time. The worst `dram_clk` paths run through LiteDRAM's
calibration CPU and its bus (from its instruction or data bus address through LiteX's bus
decoder back into the CPU's fetch stage, 12.7 to 15.8 ns, 2.5 to 2.8 ns of it logic) and inside
LiteDRAM's controller (the ZQ timer through the refresher and the command multiplexer to a bank
machine, 12.1 to 13.6 ns, 13 to 17 LUT levels): a few hundred endpoints are late, and the figure
moves by several MHz with any change to the netlist, the build ID included. Seeds, LiteDRAM's
controller options, the 35T and a slower memory clock (80 MHz, the DDR3L's floor being about 76)
did not close it (docs/DDR.md 10.3, docs/evidence/ddr/openflow-timing.txt). The gate stays: the
fix is a flow or a design that meets the clock (Vivado closes LiteDRAM at 100 MHz on the Arty
routinely), not a waiver.

Routing converges only for some placements. With the placer's seed 1 the 35T did not route at an
earlier commit (router2 at 300 to 500 overused wires past 300 iterations), hence `seed 2`, with
which all four builds routed at 383dba74; at a338e7a3, whose netlist differs only by the build
ID, the 100T ddr_bramdiag build did not (750 to 980 overused wires through 100 iterations; the
others routed in 18 to 45). Its manifest, written by that commit's `tools/fpga-build.sh`, gives
the placer's timing estimate as the reason; the script now says when nextpnr did not finish
routing (`nextpnr_routed = false`). A seed is therefore no fix: nextpnr's congestion-driven
placement (`--placer-heap-congestion-spread`) is a flow setting for the flow track to try.

| Use (Yosys 0.69, `--dsp safe`; the four builds) | 100T | 35T |
|---|---|---|
| LUTs (Yosys LUT1-6 and INV) | 10,219 to 10,333 (16.1 to 16.3 %) | 10,247 to 10,262 (49.3 %) |
| flip-flops | 6,428 (5.1 %) | 6,428 (15.5 %) |
| block RAM (RAMB36 + RAMB18 / 2): the SoC's 36, LiteDRAM's BIOS ROM, SRAM, CPU and FIFOs 10.5 | 46.5 of 135 (34 %) | 46.5 of 50 (**93 %**) |
| DSP48E1 (the SoC's multiplier; lite's is iterative) | 4 | 4 |
| clock tiles: PLLE2 (the board's sys, LiteDRAM's), MMCME2 (pix) | 2 + 1 of 6 CMTs | 2 + 1 of 5 |
| BUFG | 7 of 32 | 7 of 32 |
| I/O logic: ISERDESE2, OSERDESE2, IDELAYE2, IDELAYCTRL | 16, 45, 16, 1 | the same |

**Diagnostic builds** (`sim/board/openxc7_ddr_evidence.sh`; not the designs' results): the same
flow with nextpnr allowed to finish (`--timing-allow-fail` in a copy of `tools/openxc7-build.sh`
the script makes and records), so that the checks after it run, on `surfcpu_arty_a7_ddr_bramdiag`
for both boards at 383dba74 (35 and 38 minutes, 30 of them LVS): `dram_clk` 73.7 and 79.0 MHz;
the bitstream checks pass (IDCODE and part, frames, commands; 3,825,972 and 2,192,194 bytes),
the FASM round trip passes, all 55 block RAMs (38 RAMB36E1, 17 RAMB18E1, LiteDRAM's BIOS ROM among
them) hold their contents in the bitstream, the board's netlist and boot ROM checks pass, the
clock-tile check fails only by misreading LiteDRAM's PLL (below), and **LVS proves 14,148 points
and finds one different**, on both boards: write enable WEBWE[3] of the calibration CPU's
instruction-cache tag memory (a RAMB18E1 in simple dual port mode, 23 of its 36 bits used), which
the netlist ties to 0 and the routed bitstream drives from the write enable of the other three
byte lanes. That lane's data inputs are 0 and nothing reads it, so the extra writes should change
nothing, but the bitstream is not the netlist there and LVS fails it rightly; whether nextpnr's
handling of that pin or the LVS tile model is at fault is the flow track's to find (docs/DDR.md
10.3). `tools/fpga-build.sh` fails these builds too, so they have no bitstream under the
design's name, and `tools/program.sh` refuses the flow's own bitstream (its manifest lists the
failing checks).

Two of the flow's checks misread these designs, and are reported for the flow track rather than
waived: `synthcheck.py` counts the 22 bits of the calibration CPU's instruction-cache tag read
register as lost (synthesis puts them in the tag memory's RAMB18E1, whose read port is
synchronous; the check credits a block RAM's output register only when its optional DOA_REG or
DOB_REG is on), and `cmtcheck.py` reads LiteDRAM's PLL parameters, which Yosys writes as short bit
strings (`5'd16` as "10000"), as decimal numbers (docs/evidence/ddr/openflow-cmtcheck.txt: read as
binary, every divider matches XAPP888).

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
output directory. 215 checks with fetch.sh's: refusals, dry runs, all three flows, the staged
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
typed at the prompt, and only a bitstream whose `tools/fpga-build.sh` manifest says
`result = "pass"` for this file (`bitstream_sha256`), this board and its part; the board's
flash part depends on its revision (RM §5.2: Micron N25Q128A13ESF40, Spansion
S25FL128SAG[M|N]FI00 or S25FL127SABMFx00; gap G7). Tested with a fake openFPGALoader that
answers like the real one (in the checks above), and the four SoC bitstreams of section 8 pass
`make program-volatile BOARD=... DESIGN=...` against it, each refused on the other board
(evidence `openxc7-soc.txt`). Nothing has been programmed.

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
| LiteDRAM, in the DDR designs: its BIOS ROM (48 KiB region, 27.3 KiB used), SRAM 8 KiB, the calibration CPU's instruction cache and FIFOs (the open flow: 2 RAMB36 + 17 RAMB18) | 10.5 |
| **total with DDR** (the open flow) | **46.5** |

The XC7A35T has 50 RAMB36 (74 % used, 13 free; the open flow's 36 leave 14), the XC7A100T 135
(27 %, 98 free) (DS180 v2.6.1 table 4). The 35T fits with little room: with the DDR3L controller
93 % of its block RAM is used (3.5 free); caches or a frame buffer would need the 100T or a
smaller boot ROM. Clock resources: the SoC without DDR uses one MMCM, one PLL and two BUFGs; the
DDR designs add LiteDRAM's PLL (a second PLLE2), five BUFGs (the oscillator's for LiteDRAM's PLL
and LiteDRAM's four) and an IDELAYCTRL (section 7); the 35T has 5 CMTs and the 100T 6, each with
one MMCM and one PLL, and every 7-series device has 32 BUFGs (UG472). The DDR designs as the open
flow builds them (section 8): about 10,300 LUTs and 6,430 flip-flops, half of the 35T's LUTs.

## 11. Make targets and tests

| Target | On | What |
|---|---|---|
| `lint-board` | `lint` | Verilator `-Wall` lint of `arty_a7_clocks`, the UART helpers, the baseline core and top, and the SoC top (with the stand-in or the real SoC), with and without `ARTY_HAS_DDR` (the DDR top with `sim/board/stubs/litedram_core.v`, the generated core's ports) |
| `test-board` | `test-soc` | `board-clocks` (Icarus and Verilator), `board-prims-bad`, `board-baseline-rt`, `board-baseline` (Verilator and Icarus), `board-top` (Icarus, 3 seeds, and Verilator) |
| `check-board` | `check` | `board-clockplan` (settings and generated files), `board-digilent` (the XDC files and licence; the manuals when fetched), `board-wiring` (pins, WIRING.md, master XDC and, for the DDR3L, Digilent's MIG projects, I/O timing, CDC names including the bridge's mailboxes; self-test 23 mutations), `board-cdc-xdc` (15 checks), `board-tools` (245 wrapper and fetch.sh checks, the DDR designs' defines and generated files among them) |
| `estimate-board` | (explicit) | Yosys estimates and the block RAM budget into `docs/evidence/board/resources.txt` |
| `board-uart-expect` | (explicit) | what the serial port should show with the two SoC designs, from RTL simulation (`BOARD=`; section 12) |
| `board-netlist-sim` | (explicit) | the post-synthesis simulation of an open-flow build of the SoC (`BOARD=`, `DESIGN=`, a DDR design too; section 8) |
| `board-ddr-sim` | (explicit) | the DDR top in RTL (section 7): with LiteDRAM's controller and DRAM model the BRAM diagnostics' DDR tests, a failed calibration and SurfBoot; with the generated core and its BIOS, no memory (`BOARD=`; needs `make litedram-gen`; about 25 minutes) |
| `board-uart-expect-ddr` | (explicit) | SurfBoot booting SurfOS with the C++ DDR model, the SoC as the DDR designs configure it (`BOARD=`; section 12) |
| `sim/board/openxc7_ddr_evidence.sh` | (Mac, explicit) | the DDR designs through the open flow on both boards, diagnostic builds, `program-volatile` with the fake programmer: `docs/evidence/board/openxc7-ddr.txt` |
| `sim/board/openxc7_soc_evidence.sh` | (Mac, explicit) | both SoC designs for both boards with the open flow, both simulations and `program-volatile` with the fake programmer: `docs/evidence/board/openxc7-soc.txt`, `uart-expect.txt` (about 40 minutes) |

## 12. First power-up

A checklist for the owner. WIRING.md has the reasons and the adapters' pin tables; nothing here
has been tried on a board yet. Two jumpers are called JP1: **the Arty's JP1 is its MODE
jumper**; the Pmod PS2 has its own JP1, the PS/2 supply selector. "JP1 off" in earlier reports
meant the Arty's MODE jumper.

**Before the first power-up** (USB unplugged, nothing in the Pmod headers):

| Jumper | Set to | Why |
|---|---|---|
| Arty JP1 (MODE) | **off** (no jumper) | the FPGA is then not configured from the Quad-SPI flash at power-up and waits for a JTAG load (RM section 4), so nothing old or unknown drives the Pmod pins (WIRING.md U2) |
| Arty JP2 (CK_RST to the FT2232) | **open** | shorted, the FT2232's DTR line could reset the SoC when a terminal opens the port (WIRING.md U7) |
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
115200`).

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
4. **`surfcpu_arty_a7_ddr_bramdiag`**, milestone 4: the DDR3L (sections 1 and 7). **Not
   available yet**: the open flow's DDR builds miss nextpnr's timing for LiteDRAM's 100 MHz
   clock (one did not even route) and stop without a bitstream (section 8), and LVS finds one
   block RAM pin of LiteDRAM's CPU different; this step waits for a build that passes
   (Vivado, BUILD-HOST.md, or a flow that closes that timing). Same adapters as step 3, card
   optional. Optionally a 3.3 V USB-serial adapter on the ChipKit header (its RXD to IO1, ground
   to GND) at 115200 8N1 shows the memory controller's own calibration log. The memory
   controller calibrates by itself in about 1 s; the diagnostics wait for it (up to 2 s), then
   test the DDR3L. If calibration fails, they say so at 1.5 s (`CALIB_FAIL`) and end with FAIL.
5. **`surfcpu_arty_a7_ddr`** (SurfBoot), with a card that holds SurfOS (`make deploy-sd`): SurfBoot
   waits for calibration, tests the DDR3L briefly, loads `/BOOT/SURFOS.BIN` into it and starts
   SurfOS, which ends at its shell prompt. Same wait as step 4 for a bitstream.

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
board Arty A7-100T (0x100), SoC 0.1.0, memory map 1, sys 25000000 Hz, timebase 1000000 Hz
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

SurfBoot 1.0.0 (build <id>), the SurfCPU boot ROM
soc: Arty A7-100T (0x100), SoC 0.1.0, memory map 1, 25000000 Hz; bitstream <id>
ddr: this bitstream has no DDR controller (DDR_STATUS 0x00000000): there is nowhere to load a kernel

SurfBoot stopped: no DDR controller (DDR_ABSENT)
  this bitstream has no DDR controller (DDR_STATUS 0x00000000): there is nowhere to load a kernel
The LEDs LD4-LD7 show code 1. Type help for the monitor's commands; inserting a card retries the boot.
surfboot> info
SurfBoot 1.0.0 build <id>
sysinfo: magic 46525553, SoC 00000100, map 1, board 00000100, bitstream build <id> flags 0, time 0
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
generated core with nothing on its memory pins, and from the C++ DDR model; docs/DDR.md 10.4,
evidence `docs/evidence/ddr/ddr-top-sim.txt` and `docs/evidence/board/uart-expect-ddr.txt`).

`surfcpu_arty_a7_ddr_bramdiag` with a working DDR3L (`make board-ddr-sim`: the board top in RTL,
LiteDRAM's controller and its DRAM model in place of the PHY and the chip, calibration done
2 ms after start where the board's BIOS takes about 1 s; about 2.8 s in all):

```

SurfCPU BRAM diagnostics 1.0.0 (build <id>)
board Arty A7-100T (0x100), SoC 0.1.0, memory map 1, sys 25000000 Hz, timebase 1000000 Hz
bitstream build <id> flags 0, features ff, DDR status 5 size 256 MiB
identity                           ok (7 checks)
  ... the same eleven lines as surfcpu_arty_a7_bramdiag ...
FENCE.I, code written to RAM       ok (2 checks)
DDR calibration                    ok (1 checks)
DDR data lines                     ok (192 checks)
DDR byte lanes                     ok (9 checks)
DDR address lines and aliases      ok (54 checks)
DDR boundaries                     ok (98 checks)
DDR patterns                       (1024 KiB in full, 4096 blocks sampled) ok (2 checks)
DDR sustained copies               (4 x 256 KiB copied and checked, 738 KiB/s) ok (4 checks)
code running from DDR              ok (6 checks)

BRAMDIAG: PASS (569 checks)
```

The copy rate is the simulation's (LiteDRAM's model answers at once; the real memory's latency
adds little at one 32-bit access at a time). If calibration fails, the DDR lines become, at
about 1.5 s (the same simulation with the BIOS's failure report, and the exact generated core
with nothing on its pins, both print this):

```
DDR calibration                    
  FAIL calibration did not succeed within 2 s (DDR_STATUS 0x00000006)
  DDR calibration: 1 of 1 checks failed

BRAMDIAG: FAIL (1 of 204 checks failed)
```

`DDR_STATUS 0x00000006` is PRESENT and CALIB_FAIL (the bridge's limit, 1.5 s, ran out); `5`
would be PRESENT and CALIB_DONE. The memory controller's console on IO1 in that simulation (the
generated core's own BIOS, nothing on the memory pins, so every leveling scan is empty; on a
working board each byte lane, `m0` and `m1`, shows a run of 1s in one bitslip line, a best
choice with delays, `Memtest OK` and two speed lines instead):

```
        __   _ __      _  __
       / /  (_) /____ | |/_/
      / /__/ / __/ -_)>  <
     /____/_/\__/\__/_/|_|
   Build your hardware, easily!
 (c) Copyright 2012-2026 Enjoy-Digital
 (c) Copyright 2007-2015 M-Labs
 BIOS built on Sep 24 2026 08:42:33
 BIOS CRC passed (137f0028)
 LiteX git sha1: b6ae9e0
--================ SoC =================--
CPU:		VexRiscv_Lite @ 100MHz
BUS:		wishbone 32-bit data/32-bit addr
CSR:		32-bit data big ordering
ROM:		48.0KiB
SRAM:		8.0KiB
L2:		32B
SDRAM:		256.0MiB 16-bit @ 800MT/s (CL-6 CWL-5)
MAIN RAM:	16.0MiB
--=========== Initialization ===========--
Initializing SDRAM @0x40000000...
Switching SDRAM to software control.
Read leveling:
  m0, b00: |00000000000000000000000000000000| delays: -
  ... b01 to b07 the same ...
  best: m0, b00 delays: -
  m1, b00: |00000000000000000000000000000000| delays: -
  ... b01 to b07 the same ...
  best: m1, b00 delays: -
Switching SDRAM to hardware control.
Memtest at 0x40000000 (2.0MiB)...
  bus errors:  256/256
  addr errors: 8191/8192
  data errors: 524288/524288
Memtest KO
Memory initialization failed
--============== Console ===============--
litex> 
```

(The banner and the section titles carry terminal colour codes; the `Write:`/`Read:` progress
lines of the memory test overwrite themselves with carriage returns.)

`surfcpu_arty_a7_ddr`, SurfBoot with a card holding SurfOS, through to the shell prompt (`make
board-uart-expect-ddr`: the soc track's surfsim with the C++ DDR model, calibrated after 0.5 s
where the board's BIOS takes about 1 s, so `calibrated after` reads about 1000 ms on the board;
the card's read time is the simulation's card model's; 5.4 s in all):

```

SurfBoot 1.0.0 (build <id>), the SurfCPU boot ROM
soc: Arty A7-100T (0x100), SoC 0.1.0, memory map 1, 25000000 Hz; bitstream <id>
ddr: calibrated after 479 ms, 256 MiB, quick test passed (data, address, byte lanes)
sd: SDHC/SDXC, 64 MiB; ready after 5 ms; SPI 6250 kHz, data CRC checked
fat32: partition 1 (type 0x0c) at sector 2048, 512-byte clusters, 126976 clusters, label "SURFOS"
boot: /BOOT/SURFOS.BIN
boot: loading 358 KiB .....
boot: SurfOS 1.0.0 build <id>: 367192 bytes in 1 fragment, read in 1791 ms
boot: load 0x80000000, entry 0x80000000, mem_size 1151152, ISA IM_Zicsr_Zifencei; payload CRC 0xb1726a48 matches (card and DDR)
boot: command line "ascii"
boot: starting the kernel at 0x80000000 (a0 = 0, a1 = 0x0001f000, a2 = 0x49465253)

SurfOS: serial console on the arty_a7 UART (115200 8N1)
Memory: kernel 0x80000000-0x801190af, DMA heap 0x8011a000-0x80219fff, kernel heap 0x8021a000-0x8effffff (237 MB)
Booting SurfOS Kernel.....
  ... the kernel's start-up messages (docs/evidence/board/uart-expect-ddr.txt) ...
Brandon Burr's SurfOS
Version 1.0 on a RISC-V (SurfCPU on the Arty A7), 2004-2026
SURF ~#> 
```

(Sizes, CRCs and the build depend on the kernel built; the prompt is coloured.) With no card,
SurfBoot calibrates and stops: `SurfBoot stopped: no card (NO_CARD)`, LED code 2.

The VGA monitor shows the same console text, 80 x 30 characters at 640 x 480 and 59.94 Hz (the
screen the simulation decoded from the pixels is in the same evidence file). The post-synthesis
simulation of every one of the four bitstreams' netlists printed the same characters up to its
stop (section 8), and the netlist of the DDR bramdiag design, with nothing on its memory pins,
printed the same lines on both consoles, at the same microseconds, as its RTL simulation with
the generated core (docs/DDR.md 10.4). If the board prints something else, or nothing: the serial settings and port
first, then `make doctor`; the LEDs say how far it got (bramdiag lights LD4, then LD5, then LD6
as it moves through its sections, and all four with LD0 green or red at the end; SurfBoot shows
its error code on LD4-LD7 with LD0 red).
