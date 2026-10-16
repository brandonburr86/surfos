# SurfCPU on the Arty A7: the board

The board track's part of SurfCPU: the clocks and resets, the pins and constraints, the
milestone 1 design, the top for the whole SoC, the Vivado flow and the programming wrapper.
Pins and adapters are in [WIRING.md](WIRING.md); the build host in [BUILD-HOST.md](BUILD-HOST.md).

**Status (2026-10-15).** Everything here is proven in simulation, by Yosys synthesis and by
dry runs only. **No bitstream has been built** (there is no Vivado host yet, gap G2) and
**nothing has been programmed** (the board is not connected, G1/G3): every FPGA and physical
result is pending. The first real run of `build.tcl` will also be the first time Vivado reads
these constraints; section 8 lists what that run must confirm.

| | Where |
|---|---|
| Clock plan and its checker | `ip/clocking/arty_a7_clocks.toml`, `ip/clocking/clockplan.py` |
| Board RTL (vendor primitives only here) | `boards/arty_a7/rtl/` |
| Constraints | `boards/arty_a7/xdc/` (pins, timing, configuration; `cdc/` scoped per module) |
| Design file lists | `boards/arty_a7/designs/*.files` |
| Board variants (part, IDCODE, openFPGALoader name) | `boards/arty_a7/boards.txt` |
| Vivado flow | `boards/arty_a7/vivado/build.tcl` |
| Wrappers | `tools/fpga-build.sh`, `tools/program.sh`, `tools/check-wiring.py` |
| Digilent's master XDC (in git) and manuals and schematics (a local cache, `fetch.sh fetch`), with SHA-256 | `boards/arty_a7/digilent/` |
| Simulation: primitive models, stand-in SoC, tests | `sim/board/` |
| Make targets | `mk/area-board.mk` |
| Evidence | `docs/evidence/board/` |

## 1. The two designs

| Design | Top | Pins | Use |
|---|---|---|---|
| `arty_a7_baseline` | `boards/arty_a7/rtl/arty_a7_baseline.v` | the base set: clock, RESET, switches, buttons, LEDs, UART | milestone 1: no CPU; shows the board, the clocks, the reset and the serial link work (section 5) |
| `surfcpu_arty_a7_top` | `boards/arty_a7/rtl/surfcpu_arty_a7_top.v` | base + Pmod microSD (JA), VGA (JB+JC), PS/2 (JD) | the whole SoC (section 6); buildable once the soc, cpu and io tracks are merged |

Both take `BOARD` (`arty_a7_35t` or `arty_a7_100t`, the spec's `[boards]` names; the top turns
it into sysinfo's `BOARD_ID` from `surfcpu_soc.vh`) and the build identity `BUILD_ID`,
`BUILD_FLAGS`, `BUILD_TIME`, which `tools/fpga-build.sh` passes as Vivado generics. The pins of
both variants are the same (checked against both master XDC files); only the part differs.

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
`cmtcheck.py` in a real build (evidence `../flow/mmcm.txt`). `clockplan.py` now refuses a
fractional M or O0 with an odd integer part (or 2.125). `clockplan.py report` lists closer
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
  (ready, response and calibration inputs tied low). The DDR3L pins are not in the top yet; the
  DDR3L's RESET# has its own pull-down on the board, so the memory stays in reset.

Until the soc track merges `rtl/soc/surfcpu_soc.v`, `sim/board/stubs/surfcpu_soc.v` stands
in, with exactly the contract's parameters and ports, its outputs at idle levels. Make uses
it only while the real file is absent, and `tools/fpga-build.sh` refuses to build the full top
without the real one. `tb_top` checks the clocks, resets and parameters the SoC receives, every
input pin into its SoC port (200 random patterns), the open-drain PS/2 lines both ways, the
RESET button and, with the stand-in, every SoC output onto its pin (Icarus on seeds 1, 7, 123,
Verilator `--timing` on seed 5). With the real SoC the same test runs without the output part
(the stand-in's driver registers are gone).

## 7. For the ddr track

The top will need: the DDR3L pins (bank 34, 1.35 V: the MIG generates their constraints), the
MIG instance and `surf_ddr_bridge` between `surfcpu_soc`'s DDR port and it, `HAS_DDR = 1`, and
the controller's clocks. The sys PLL's VCO (1000 MHz) divides to 166.667 MHz and 200 MHz on
its spare outputs if a clock wizard is not wanted; the MIG's own MMCM and PLL take the CMT of
bank 34's clock region, separate from this design's (bank 35's). Its UI clock is a third
domain: every crossing into it needs the same treatment as section 4 (and a constraint file
for whatever synchronizer it uses), and its calibration status reaches sysinfo through
`surf_sync`.

## 8. Building a bitstream

```sh
make build-fpga BOARD=arty_a7_100t DESIGN=arty_a7_baseline      # milestone 1
make build-fpga BOARD=arty_a7_100t                              # the whole SoC (needs build/fw/surfboot.mem)
tools/fpga-build.sh --dry-run --design arty_a7_baseline arty_a7_35t   # or: make build-fpga ... DRY_RUN=1
```

`tools/fpga-build.sh` reads `~/.config/surfcpu/build-host.conf` (BUILD-HOST.md):

| Key | Flows | Meaning |
|---|---|---|
| `FLOW` | all | `vivado-ssh`, `vivado-docker` or `openxc7` (the flow track's `tools/openxc7-build.sh`) |
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

**The manifest** `build/fpga/<board>/<design>/manifest.txt` (TOML): run id, design, top,
board, part, flow, build host alias, Vivado binary, source revision and dirty state, build ID
and time, SHA-256 of the sources, of the constraint files (in reading order), of the ROM and
of the kernel package if present, ISA, ABI, map and SoC versions, platform ABI, the Mac; then a
`[vivado]` table from `out/build-results.txt`: Vivado version, build host name, WNS, WHS,
LUT/FF/BRAM/DSP/BUFG/MMCM/PLL/IO use, DRC errors, critical warnings, critical CDC findings,
`check_timing` issues, the result, and the bitstream's SHA-256.

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
locally, with a temporary HOME and output directory (100 checks with fetch.sh's: refusals, dry runs, both flows,
the staged files, `params.tcl`, the TOML manifest, and each gate stopping the bitstream).

## 9. Programming

```sh
make program-volatile BOARD=arty_a7_100t DESIGN=arty_a7_baseline     # configuration RAM only
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
  accepted for a volatile load when every check line says PASS.

`volatile` runs `openFPGALoader -b arty_a7_100t <bit>`: configuration RAM, lost at power-off
(PROG or a power cycle returns to the flash). `flash` (milestone 9) writes the configuration
flash with `-f --verify` only with `SURFCPU_FLASH_CONFIRM=write-flash-<board>` or the board name
typed at the prompt, and only a bitstream whose `tools/fpga-build.sh` manifest says
`result = "pass"` for this file (`bitstream_sha256`), this board and its part; the board's
flash part depends on its revision (RM §5.2: Micron N25Q128A13ESF40, Spansion
S25FL128SAG[M|N]FI00 or S25FL127SABMFx00; gap G7). Tested with a fake openFPGALoader that
answers like the real one (in the checks above). Nothing has been programmed.

## 10. Budgets

Yosys `synth_xilinx -family xc7` estimates (evidence `resources.txt`; Vivado will differ): the
baseline uses 286 LUTs, 412 flip-flops, no block RAM or DSP, 1 MMCM, 1 PLL and 2 BUFGs: 1.4 %
of the 35T's LUTs, 0.5 % of the 100T's. The SoC is not on this branch yet; `make
estimate-board` estimates the whole top once it is.

Block RAM implied by the memory map (a RAMB36 holds 1024 x 32 data bits):

| User | RAMB36 |
|---|---|
| boot ROM, 64 KiB, true dual port | 16 |
| scratch RAM, 64 KiB, true dual port | 16 |
| text RAM, 16 KiB, dual clock | 4 |
| font ROM, 4 KiB (256 x 16 x 8 bits) | 1 |
| register file, FIFOs (distributed RAM) | 0 |
| DDR3L controller (to be confirmed by the ddr track) | 0 |
| **total** | **37** |

The XC7A35T has 50 RAMB36 (74 % used, 13 free), the XC7A100T 135 (27 %, 98 free) (DS180
v2.6.1 table 4). The 35T fits with little room: caches or a frame buffer would need the 100T
or a smaller boot ROM. Clock resources: this design uses one MMCM, one PLL and two BUFGs; the
memory controller adds its own MMCM and PLL in bank 34's CMT; the 35T has 5 CMTs and the 100T
6, each with one MMCM and one PLL, and every 7-series device has 32 BUFGs (UG472).

## 11. Make targets and tests

| Target | On | What |
|---|---|---|
| `lint-board` | `lint` | Verilator `-Wall` lint of `arty_a7_clocks`, the UART helpers, the baseline core and top, and the SoC top (with the stand-in or the real SoC) |
| `test-board` | `test-soc` | `board-clocks` (Icarus and Verilator), `board-prims-bad`, `board-baseline-rt`, `board-baseline` (Verilator and Icarus), `board-top` (Icarus, 3 seeds, and Verilator) |
| `check-board` | `check` | `board-clockplan` (settings and generated files), `board-digilent` (the XDC files and licence; the manuals when fetched), `board-wiring` (pins, WIRING.md, master XDC, I/O timing, CDC names; self-test 18 mutations), `board-cdc-xdc` (15 checks), `board-tools` (100 wrapper and fetch.sh checks) |
| `estimate-board` | (explicit) | Yosys estimates and the block RAM budget into `docs/evidence/board/resources.txt` |
