# The open-source bitstream flow (openXC7)

Option C of [BUILD-HOST.md](BUILD-HOST.md): Yosys, nextpnr and Project X-Ray in an arm64 Linux
container on the MacBook, with no AMD software. This page says what was built and checked, what
is wrong with the tools and how the flow guards against it, and what only the board can show.
Evidence: [`evidence/flow/`](evidence/flow/). Written 2026-10-16 by the flow track; the
placer seed lists, the three checker fixes and the clock check's phases of round ddr-3
(sections 3, 4 and 6.5) by the ddr track the same day.

**Bitstreams built here run on the owner's Arty A7-100T** (the baseline, the SoC and both DDR
designs, 2026-10-16/17, BOARD.md section 12). Two placements of the DDR diagnostics that passed
every check here failed on the silicon until the route-through fix (section 6.8): a bitstream
that was generated and checked is not a bitstream that works on the hardware.

## 1. Verdict

| Question | Answer |
|---|---|
| Can bitstreams for both Arty A7 parts be built locally, with open tools only? | **Yes.** A pinned image builds natively for arm64 in about 6 minutes (661 MB); a small design goes from Verilog to `.bit` with every check in about 10 s, the feature probe (64 KiB ROM, DSP, MMCM, PLL) in under 2 minutes. |
| Are the bitstreams well formed for the right device? | **Yes**, by our own parser of UG470's format: the exact UG470 length, the device's JTAG IDCODE (35T and 100T told apart), whole frames, the start-up commands. |
| Do they encode what the tools placed and routed? | **Yes, as far as Project X-Ray's database describes the silicon**: the bitstream decoded back matches the routed FASM bit for bit (every difference accounted for), block RAM contents match the netlist, and the routed design is proved equivalent to the synthesized one (LVS). |
| Does synthesis keep the RTL's logic? | **Not always: Yosys deletes logic** when it packs a multiplied counter into a DSP48 (every version tried, 0.52 to 0.70). The flow avoids that packing by default and checks every netlist against the RTL (section 6.1). |
| Are the clocks right? | **Not always: nextpnr mis-encodes** an MMCM fractional divider with an odd integer part, the planned pixel clock's 35.25 among them. Every build checks the MMCM/PLL settings against AMD's algorithm; 100 MHz × 18.125 / 2 / 36 avoids it (section 6.2). |
| Is timing met? | nextpnr says so with wide margins (the demo: 200.6 MHz at 100 MHz; the probe: 127 MHz at 25 MHz). Its timing model is an estimate, not Vivado signoff (section 6.4). |
| DDR3? | Only through LiteDRAM (no MIG). Its PHY places, routes and passes every check; its controller closes nextpnr's timing at 80 MHz, not at 100, so the DDR designs run the DDR3L at 640 MT/s, with a list of placer seeds (section 6.7). |
| Vivado in a container (option B)? | Docker Desktop here runs `linux/amd64` through Rosetta; Vivado itself is unproven (section 7). |
| Does it work on the board? | **Pending** (section 8). |

Recommendation, for the owner to decide: option C is usable now for development builds, with
the checks this flow runs on every build. Keep a Vivado path (option A, or B once proven) as the
reference for timing and for MIG, and to compare against when the board misbehaves.

## 2. The image

`surfcpu/devenv/openxc7/Dockerfile` builds `surfcpu-openxc7:2026-10-15` from pinned sources on
the main image's Debian snapshot, every tool compiled for arm64:

| Component | Version | Notes |
|---|---|---|
| Yosys | 0.69 (release tarball, SHA-256 checked) | openXC7's installer pin; the main image keeps its 0.52 |
| openXC7/nextpnr | 1.0.0 (`e860c9c8`) | `nextpnr-himbaechel`, xilinx micro-architecture. It replaced nextpnr-xilinx, archived on 2026-09-25 (last release 0.9.8). A `nextpnr-xilinx` shim keeps the old command line |
| fasm2netlist | `0948889f` (nextpnr's submodule) | `tileverilog`, `lvs_equiv`, `fasm2netlist` for the LVS and block RAM checks |
| openXC7/prjxray | `ed3331c6` | `xc7frames2bit`, `bitread`, `fasm2frames`, `bit2fasm` |
| openXC7/prjxray-db | `a90f27c1` (Artix-7 only) | the pins nextpnr 1.0.0's CI uses with that engine |
| fasm, textX | `2f57ccb1`, 4.2.3 | pure-Python FASM parser |
| Chip databases | xc7a50t 29.6 MB, xc7a100t 39.2 MB | generated in the build: 26 s / 1.8 GB and 45 s / 3.1 GB peak memory, so the 7.7 GiB Docker VM is enough and no prebuilt databases are needed |

The xc7a35t has no database of its own: it is the xc7a50t die (section 6.3). Build the image
with `surfcpu/devenv/openxc7/build-image.sh` (331 s cold on the M4 Max with 6 jobs). It uses a
BuildKit builder of its own, so its cache never mixes with other projects' Docker caches;
`build-image.sh --rm-builder` deletes that builder and its 2.4 GB cache afterwards.
`/etc/surfcpu-openxc7.txt` in the image lists every version and hash, and each build copies it
into its manifest. Details: [evidence/flow/image.txt](evidence/flow/image.txt).

## 3. Using it

```sh
surfcpu/tools/openxc7-build.sh --top <module> --board arty_a7_35t|arty_a7_100t --xdc <file> \
    [-D NAME[=VALUE]] [-I dir] [--seed N[,N...]] [--dsp safe|full|none] [--expect file] \
    [--manifest-include file] [--out dir] [--lvs-timeout s] [--no-lvs] [--no-check] [--synth-only] \
    [--dry-run] <source.v>...
```

It runs on the Mac and re-runs itself in the image with the repository at `/work` (paths must
lie inside `surfcpu/`). Steps: Yosys (`synth_xilinx -flatten -abc9 -arch xc7`, openXC7's recipe),
nextpnr (`--router router2 -o hold-fix --placer-heap-timingweight 60`, the settings openXC7's CI
uses for hardware-tested designs), `fasm2frames` and `xc7frames2bit` (with SOURCE_DATE_EPOCH
set, as `make release` sets it, the `.bit` header's date and time are then stamped with that
time by `tools/bitfile.py`, so two builds of one commit are the same bytes: RELEASE.md section
4), then the checks of section 4. Outputs in `build/openxc7/<top>-<board>/`: `<top>.bit`, `timing.txt`,
`utilization.txt`, one `check-*.txt` per check, the logs, the netlist with Yosys's simulation
models, and `manifest.txt` (source revision, part, hashes of the sources and the XDC, DSP mode,
check results, timing, utilization against the device's documented capacity, step times, tool
versions, and any `--manifest-include` file such as ROM hashes). The exit status is 0 only if
every step and every check passed. LVS gives up after 1800 s (`--lvs-timeout`, or
`OPENXC7_LVS_TIMEOUT` when the script is run directly), and a proof that times out fails the
build; the whole SoC needs 9 to 12 minutes, and its design lists allow 7200 s.

**A list of placer seeds** (`--seed 1,2,3`; a design list's `seed 1 2 3` line): placement and
routing run with each seed in turn until nextpnr succeeds, that is until routing converges and
every clock meets its constraint after routing, the same gate for every seed; the rest of the
flow then runs on that placement. Every attempt is kept (`nextpnr-seed<n>.log`) and summarized
in `seeds.txt`, the flow's manifest and `tools/fpga-build.sh`'s (`placer_seeds`, `placer_seed`:
the one used, `placer_attempts`). nextpnr is deterministic, so a seed's result depends on the
whole netlist, the build ID included: one seed that passes at one commit can miss at the next,
which a list absorbs. Only the DDR designs use one (section 6.7); a list never relaxes a check.

* `tools/openxc7-demo.sh` is the proof run behind this page (both parts, the demo, the feature
  probe, the expected failures, gate-level simulation, a determinism rebuild; about 7 minutes).
* `make lint` includes `lint-flow`, `make check` includes `test-flow` (about 10 s: the demos'
  RTL simulations, the self-tests of the bitstream inspector and the clock check, the build
  script's argument handling, and the Yosys reproducer through the main image's Yosys). No open
  flow build is part of `make check`. `make flow-xdc-check` regenerates the demos' XDC from
  Digilent's master files (fetched by `tools/openxc7-fetch.sh`, SHA-256 checked).
* **`make build-fpga` with `FLOW=openxc7`**: `mk/host.mk` runs `tools/fpga-build.sh` (board
  track), which would dispatch on `FLOW` read from `~/.config/surfcpu/build-host.conf` or the
  environment, for example:

  ```sh
  openxc7) exec tools/openxc7-build.sh --board "$BOARD" --top surfcpu_arty_a7_top \
               --xdc boards/arty_a7/<the board's XDC> -I rtl/generated -I rtl/cpu -I rtl/lib \
               --manifest-include <ROM/kernel manifest> <the SoC's sources> ;;
  ```

  `tools/fpga-build.sh` now does this translation (board-2 round, 2026-10-16): `make build-fpga
  FLOW=openxc7 DESIGN=...` builds a design from its `boards/arty_a7/designs/*.files` list.
* The XDC reader takes `set_property` (with `-dict`) on `[get_ports {name}]` or `[get_cells]`,
  `create_clock -period` (it warns about and ignores `-add`, `-name`, `-waveform`), and
  setup `set_multicycle_path`. It ignores `set_false_path`, `set_max_delay`, `set_clock_groups`
  and `INTERNAL_VREF` with a warning. Brace lists must have no inner padding and lines no
  trailing `;`: `tools/openxc7-xdc.sh` turns Digilent's master lines into that form, which
  Vivado reads too.
* Avoid one-bit vector ports (`[0:0]`) at the top level: nextpnr finds them under Vivado's
  `name[0]`, but the LVS check reports a false difference.

## 4. The checks, and how far each reaches

| Check | Compares | Shows | Does not show |
|---|---|---|---|
| synthesis (`synthcheck.py`) | the netlist against the elaborated RTL: flip-flops per instance (with credit for DSP/RAM/SRL registers; register bits Yosys already merged into a memory's read port are counted apart), large memories against block RAM, outputs that became constant; `--expect` adds minimum flip-flops per instance | that synthesis did not drop logic on a large scale | subtle functional changes (no formal equivalence) |
| bitstream (`bitinfo.py`, no vendor or prjxray code) | the `.bit` against UG470: length, IDCODE against the device's JTAG IDCODE, whole frames, WCFG/START/DESYNC, every CRC check | a complete bitstream for that device | whether its frames mean what the tools intend |
| round trip (`bit2fasm`, `fasmcheck.py`) | the bitstream decoded back against the routed FASM, bit by bit | the assembly wrote exactly what nextpnr chose; every difference is a connection without bits, an all-clear default or the STEPDOWN that `fasm2frames` spreads to a bank as Vivado does | anything the database itself gets wrong (same database both ways) |
| block RAM (`fasm2netlist`, `bramcheck.py`) | initial contents decoded from the bitstream against the netlist, block for block | SurfBoot's ROM would arrive intact (as far as prjxray-db's bit positions are right) | |
| LVS (`tileverilog`, `bramwe.py`, `lvs_equiv`) | the routed FASM read into a fabric netlist by an independent tile model, each RAMB18E1 byte write enable read from the site pins the silicon reads it from (with every other pin it ties to that enable checked), proved equal to the synthesized netlist register by register | place, route and FASM writing kept the logic | MMCM/PLL (not modelled), block RAM and DSP internals (cut at their pins), the clock tree (assumed) |
| clocks (`cmtcheck.py`, `gclkcheck.py`) | every MMCM/PLL divider field and every output's phase fields (DELAY_TIME, PHASE_MUX, MX) in the FASM against AMD's XAPP888 algorithm, itself checked against register values Vivado produced; the parameters read as Yosys writes them (a constant as its bits, at any width); and the routed FASM for any net on the global clock network: a clock line driven by an unconfigured BUFGCTRL, logic into one, a clock line into a data input (section 6.8) | the clock managers are configured as Vivado would; no ordinary net rides the clock network | duty cycles other than 50 % and phases of fractional dividers (reported unchecked) |
| simulation (demo only) | the synthesized netlist with Yosys's `cells_sim.v` under the RTL testbench | synthesis kept the behaviour (the UART line, the reset) | |

None of these can show whether Project X-Ray's database is right for this silicon, whether the
timing model holds, or how the I/O electrically behaves: only the board can. Details:
[evidence/flow/bitstream-checks.txt](evidence/flow/bitstream-checks.txt).

## 5. Results

Both designs are the flow track's own: `devenv/openxc7/demo/flow_demo.v` (LD4 heartbeat, line
counter on LD5-LD7, `SurfCPU openXC7 flow demo #nnnn` at 115200 8N1 every second, pins from
Digilent's master XDC at commit c8644a9) and `flow_features.v` (section 6.1, 6.2; 64 KiB ROM in
block RAM with a checksum, a 32×32 multiply, PLL and MMCM clocks, an open-drain pin).

| | flow_demo 35T | flow_demo 100T | flow_features 35T | flow_features 100T |
|---|---|---|---|---|
| timing (nextpnr) | 200.60 MHz at 100 | 193.87 MHz at 100 | sys 127.5 at 25, pix 283.9 at 25.17 | sys 113.7 at 25, pix 287.9 at 25.17 |
| Yosys cells | 123 LUT, 78 FF, 22 CARRY4 | same netlist | 507 LUT, 445 FF, 4 DSP48E1, 29 RAMB18E1 | same |
| share of the 35T | 0.6 % LUTs | | 2.4 % LUTs, 29 % block RAM, 4 % DSP | |
| bitstream | 2,192,155 B | 3,825,933 B | 2,192,163 B | 3,825,941 B |
| checks | all PASS | all PASS | all PASS | all PASS |
| LVS | 83 proved | 83 proved | 1494 proved | 1494 proved |
| wall clock | 10 s | 12 s | 111 s | 107 s |

The demo's synthesized netlist passes its testbench at gate level at the real rates (a full
line and a RESET press), a second build gives byte-identical FASM and frames, and the testbench
fails each of four deliberately broken copies of the design. Full numbers:
[evidence/flow/builds.txt](evidence/flow/builds.txt).

## 6. Defects and limits

### 6.1 Yosys deletes logic when it packs a multiplied counter into a DSP48

Found by the io track in `surf_video` (`hcnt * 205`; fixed there in io commit 534de7b, merged as
fde641f), reproduced here: Yosys's `xilinx_dsp` pass takes the counter's increment as the
DSP48E1 pre-adder and the counter register as its AD register, so the fabric loses the counter
and everything that reads it, without a warning. `devenv/openxc7/test/dsp_counter.v` reduces it
to ten lines: its netlist drives the counter outputs with constants and is wrong on 1997 of
2000 clocks, in **Yosys 0.52, 0.69 and 0.70** (the newest release, 2026-10-13), with or without
`-abc9`. Two settings give a correct netlist, demonstrated on the reproducer and on the pre-fix
`surf_video` (852 of 852 flip-flops instead of 236, the font ROM kept):

* `-nodsp`: multipliers in LUTs.
* `scratchpad -set xilinx_dsp.multonly 1`, documented in `help xilinx_dsp`: multipliers still
  map to DSP48E1, but Yosys packs no registers or pre/post-adders into them.

The build uses the second by default (`--dsp safe`; `--dsp full` is openXC7's plain recipe,
`--dsp none` uses `-nodsp`), and the synthesis check fails a netlist that lost logic. It fails
the reproducer and the pre-fix `surf_video` with full packing, passes the fixed `surf_video`
and the probe with full packing, and passes every design here in the safe mode.

**For the CPU's multiplier**: in the safe mode the RV32M multiplies still use DSP48E1, with
their pipeline registers and partial-product sums in fabric (for the probe's 32×32 multiply:
4 DSP48E1 either way, 507 LUTs instead of 446), which costs nothing that matters at 25 MHz.
Whatever the mode, every build's synthesis check covers the core. The same Yosys defect affects
the main image's Yosys 0.52, which the other tracks use for resource estimates: an estimate of a
design with this pattern can be silently too small. Evidence:
[evidence/flow/yosys-dsp.txt](evidence/flow/yosys-dsp.txt).

### 6.2 nextpnr 1.0.0 mis-encodes odd fractional MMCM dividers

For `CLKFBOUT_MULT_F` or `CLKOUT0_DIVIDE_F` with a fraction and an odd integer part, nextpnr
writes a high time one lower, the wrong `FRAC_WF_R`/`FRAC_WF_F`, and a `PHASE_MUX_F` without
its bit 2, compared with AMD XAPP888's reference functions (as ported in F4PGA's arch-defs) and
with the register values Vivado itself produced (published by E. Billauer for 4.000 to 5.875).
35 of 64 ratios from 2.000 to 9.875 agree: every integer ratio and every fractional ratio
with an even integer part except 2.125 (a special case in XAPP888).
The planned pixel clock, 100 MHz × 8.875 / 35.25, is hit (35.25: high time 16 instead of 17,
`FRAC_WF_R` 1 instead of 0, `FRAC_WF_F` 1 instead of 0, `PHASE_MUX_F` 1 instead of 5). What the
chip then does is unknown; a wrong frequency or duty cycle is likely.

The clock check recomputes every MMCM/PLL divider from the netlist and fails such a build. On
the probe built with the planned setting (`-D FLOW_PIX_PLANNED`), the bitstream, round-trip and
LVS checks all pass and only this check catches it. **For the open flow, use 100 MHz × 18.125 /
2 / 36 = 25.17361 MHz** (VCO 906.25 MHz, PFD 50 MHz). It is 0.0055 % from VGA's 25.175 MHz (the
planned setting is 0.009 % off) and both of its dividers encode correctly. Alternatively keep
the planned setting under Vivado. Adopted for both flows on 2026-10-16: `clocks.pix_hz` is now
25,173,611 Hz (BOARD.md has the note).
Evidence: [evidence/flow/mmcm.txt](evidence/flow/mmcm.txt).

### 6.3 The xc7a35t is placed on the whole xc7a50t die

Project X-Ray has no separate 35T: its part files equal the 50T's except for the IDCODE, and
nextpnr offers the 50T's 65,200 LUT positions for a 35T. Vivado restricts a 35T to its
documented 5,200 slices. AMD does not document whether the rest works on every 35T chip, and
nextpnr may place logic there. The flow therefore holds a 35T design to the 35T's documented
capacity (`utilization.txt`, AMD DS180). It cannot keep placement inside Vivado's 35T region:
the XDC reader has no placement constraints. None of this applies to a 100T. The Arty A7-35T's
real part is the XC7A35TICSG324-**1L**: its bitstream is the same, but its timing is not
modelled (the flow uses the -1 model).

### 6.4 Timing is an estimate

nextpnr's delays come from Project X-Ray's timing data and a delay matrix it measures on the
chip database; they are not Vivado's signoff models. Treat a pass as "probably fine with a wide
margin" (the demo's critical path takes half of its 10 ns period, the probe's a fifth of its
40 ns). The DDR designs' `dram_clk` has no such margin: 0.26 to 1.79 ns of its 12.5 ns at
80 MHz in the round ddr-3 builds, nextpnr's estimate on a path of 14 to 19 LUT levels (section
6.7), so there a pass is the flow's gate and no more; Vivado's timing for the same design, or
the board, has to confirm it. Paths between clock domains are
reported (`timing.txt`, for example 1.5 ns from the pixel to the system clock in the probe) but
checked against nothing: the XDC reader has no `set_max_delay -datapath_only` or
`set_clock_groups`. So cross-domain safety rests on the `rtl/lib` synchronizers, and the delays
are for a reader to judge. Hold violations are fixed after routing (`-o hold-fix`).

### 6.5 Other limits

* The bitstreams carry no CRC check packets: prjxray's writer, like openXC7's newer
  fpga-assembler, resets the CRC instead of checking it, so a corrupted transfer is not caught
  by the FPGA. Vivado's bitstreams have one.
* The flow refuses FASM features missing from the database (`XRAY_ALLOW_MISSING_FEATURES` is
  never set). openXC7's CI shows why: such features were once dropped silently.
* LVS does not model clock managers or the clock tree. Block RAM and DSP are checked only at
  their pins, with block RAM contents checked separately.
* LVS cannot check shift-register LUTs (SRL16E, SRLC32E): the extractor's tile model (the xcol
  module of lvs/tile_model.v) has no shift mode, and the values it does not implement resolve to
  0, so every signal an SRL feeds comes out different (two GPU builds failed so, docs/GPU.md
  section 12.2). Yosys infers an SRL from three or more registers in a row without a reset.
  Synthesizing with `-nosrl` would avoid them, but it changes every design's netlist, SRLs or
  not (the baseline 798 cells against 820, the 100T DDR design 13,642 against 14,070, by Yosys
  alone), so instead the flow refuses a netlist with SRL cells right after synthesis
  (check-srl.txt, devenv/openxc7/srlcheck.py), before anything is placed: give the chain a reset.
* Synthesis uses Yosys 0.69, the main image 0.52: resource estimates from the other tracks and
  open-flow builds differ slightly.
* The full SoC (HAS_DDR = 0) went through the flow at integration, 2026-10-16 (board-3 round;
  BOARD.md section 8, `docs/evidence/board/openxc7-soc.txt`): every check passes on both parts,
  LVS proving 6119 items. Its first build showed four gaps, since fixed in this flow's files:
  `synthcheck.py` credited a register driving a top-level output to the top instead of its own
  instance (it now prefers internal net names); `fasmcheck.py` called the NOCLKINV feature of an
  empty CLB tile, all of whose bits are clear, unexplained; `bramcheck.py` did not pair the two
  RAMB18 halves `fasm2netlist` decodes a RAMB36 into (bits 2k and 2k+1 interleaved); and LVS
  could not prove a routed pin equal to a synthesized input Yosys left undefined (`x`, an unused
  bit of a 63-bit FIFO), so `openxc7-build.sh` now runs `setundef -zero` after synthesis.
  `make test-flow-tools` tests the synthcheck, bramcheck and build script changes; the fasmcheck
  change was checked against the SoC build with a planted CLKINV feature, which still fails.
* The DDR designs (round ddr-2) showed three more checks misreading correct results; round
  ddr-3 fixed each where it went wrong, with a self-test on `make check` and a regression
  through the real flow (`tools/openxc7-checks-test.sh`, which builds four small probes from
  `devenv/openxc7/test/` and one CPU; [evidence/flow/checkers.txt](evidence/flow/checkers.txt)):
  * **LVS and block RAM write enables.** `fasm2netlist`'s extractor (`src/bram_ports.hpp`)
    reads a RAMB18E1's byte write enables one to one from the site's first pins (WEA[1:0] from
    WEA1..0, WEBWE[3:0] from WEBWE3..0). The site has WEA0..3 and WEBWE0..7, and the silicon
    gives each byte of an 18-bit write port, and of the 36-bit simple dual port write port,
    two pins: byte i is pins 2i and 2i+1. That is how Vivado connects them (f4pga's
    fasm2bels, `bram_models.py`, which rebuilds Vivado netlists from Vivado bitstreams) and
    how nextpnr routes them (`himbaechel/uarch/xilinx/pack.cc`). So nextpnr, the FASM and
    the netlist were right, and the extractor read the second byte's enable from a pin
    carrying the first's: LiteDRAM's VexRiscv tag RAM (WEBWE = {0, we, we, we}) gave
    "DIFFER WEBWE[3]", and any 18-bit port with two byte enables gives "DIFFER WEA[1]".
    `devenv/openxc7/bramwe.py`, run between extraction and proof, makes the extracted
    netlist read each used enable where the silicon does, from the block's mode in the FASM,
    and checks that every pin the silicon ties to that enable carries the same net: a pin
    that disagrees is a difference and fails LVS. A probe of each shape now proves equal (104
    and 98 points, no difference); the extractor's own reading still fails, a pin pair that
    disagrees fails, and a byte lane really enabled against the netlist's 0 still fails the
    proof. The proper fix is in fasm2netlist (the tile model's port table); this step does
    its work until the image carries it. One shape nextpnr does get wrong: an
    18-bit write on port B in true dual port mode. A probe (`devenv/openxc7/test/bram_we_tdpb.v`,
    the checks' case 4) shows nextpnr routing its two byte enables to WEBWE0..3 as we0, we1,
    we0, we1, where the silicon pairs pins 0-1 and 2-3 (Vivado: we0, we0, we1, we1); LVS,
    through `bramwe.py`, names both pin pairs and fails the build, so such a design gets no
    bitstream. No SurfCPU design has one (the SoC's byte-written memories are RAMB36E1, whose
    byte writes passed on the board in m3).
  * **`cmtcheck.py` and Yosys's short bit strings.** Yosys's JSON writes a constant parameter
    as its bits at its own width (LiteDRAM's `5'd16` is "10000"), a real or a string as text
    (a space appended to a string that looks like bits). The check read bit strings of fewer
    than eight characters as decimal numbers, so LiteDRAM's PLL (16, 8, 20, 5) looked wrong.
    It now reads any bit string as binary; a probe with LiteDRAM's PLL as generated passes.
  * **`cmtcheck.py` and phases** (not a misreading, a gap): it reported every output with a
    phase as unchecked, LiteDRAM's 90-degree clock for the DQS strobes among them, and compared
    no output's phase fields. It now computes DELAY_TIME and PHASE_MUX as XAPP888's
    `mmcm_phase_calc` does (the phase in VCO cycles, rounded to the nearest eighth) for every
    integer divider and compares them, zero phases included; LiteDRAM's CLKOUT3 (divide 5, 90
    degrees: 1.25 VCO cycles, DELAY_TIME 1, PHASE_MUX 2) checks in every DDR build, and two
    wrong phases written into the probe's FASM fail it. nextpnr 1.0.0 floors the phase to an
    eighth where XAPP888 rounds (`fasm.cc`, `write_pll_clkout`), so a phase that is not a
    whole number of eighths of a VCO cycle would fail this check; no SurfCPU clock has one.
  * **`synthcheck.py` and registers merged into a memory read port.** When Yosys's
    `memory_dff` (run before the RTL is written for the check) moves a read-data register into
    a memory's read port, it reconnects that register's merged bits to `$ffmerge_disconnected`
    wires and keeps the cell if some bits are still used; the check counted those bits as RTL
    flip-flops that synthesis then lost (VexRiscv lite's 22 instruction-cache tag bits, which
    its block RAM's read port holds). They are now counted apart and printed;
    `VexRiscv_Lite.v` synthesized alone passes with 22 merged bits.

### 6.6 What scales

Place and route is fast (2 s for the demo, 8 s for the probe). Synthesis dominates for large
initialized memories (the probe's 16384-word ROM loop: 34 s to elaborate, 42 s to synthesize).
`fasm2frames` takes about 6 s for 300,000 feature bits with the pure-Python parser.

### 6.7 DDR3

The Arty's DDR3L needs LiteDRAM under this flow (AMD's MIG exists only in Vivado; that is the
ddr track's work). **Round ddr-3 (2026-10-16):** the DDR designs pass every check of this flow
(BOARD.md section 8, DDR.md section 10.3), with two choices this flow forced. LiteDRAM's
controller does not close nextpnr's timing at 100 MHz: its worst path, from the ZQ calibration
timer through the refresher and the command multiplexer into a bank machine, has 15 to 19 LUT
levels and 3 ns of logic, and nextpnr routes it in 8 to 10 ns; 20 placements at 100 MHz with
the calibration CPU gone, LiteDRAM's controller settings and nextpnr's options among them, gave
74.3 to 90.2 MHz, and 87.5 MHz missed on both seeds tried (docs/evidence/ddr/openflow-timing.txt). So the core runs at 80 MHz (DDR3L at 640 MT/s, above
its DLL's floor of 76), where about three placements in four meet the clock, and the DDR
design lists give seeds 1 to 8 (section 3). Vivado routinely closes LiteDRAM at 100 MHz on this
board; with it the core could go back to 100 MHz. The DDR3L's own I/O timing (the serializers
to the pins at 320 MHz, the strobes, the read path into the ISERDESE2s) is checked by nothing
here: the board's constraints declare those paths false, as LiteX's do, nextpnr has no
register-to-register path on the 320 MHz clocks, and the read leveling SurfCPU's firmware runs
on the board is what absorbs the read path's delay. The rest of this section is round 1's
record.

The PHY probe: openXC7's CI records LiteX's DDR3 design for the Arty S7 failing on an
OSERDESE2 in a "SING" I/O tile, whose divided-clock pip Project X-Ray never fuzzed for
Spartan-7. Two of the Arty A7's DDR3 pins (K6 `reset_n`, U8 `cs_n`) sit in such tiles. An
OSERDESE2 with both clocks on them (`devenv/openxc7/test/ddr_pins.v`) places, routes and passes
every check on both parts, and the divided-clock pips have their bits in the Artix-7 database.
nextpnr sets the bank's internal VREF to 675 mV whenever an SSTL input is used, the right value
for 1.35 V DDR3L, although its XDC reader ignores `INTERNAL_VREF`. Whether LiteDRAM's whole PHY
(ISERDESE2, IDELAYE2 and IDELAYCTRL, IOBUFDS for DQS, termination, read leveling) builds and
works is untested. Evidence: [evidence/flow/ddr-pins.txt](evidence/flow/ddr-pins.txt).

### 6.8 Global buffers as route-throughs: ordinary nets on the clock network

**Round ddr-4 (2026-10-16).** Two of five placements of `surfcpu_arty_a7_ddr_bramdiag` failed
on the board while passing every check here, LVS included: 92a2b51 seed 1 trapped at its first
printed character with a register dump no correct core can produce (mcause 0 with an odd mtval
on a load), and 69ee491 seed 3 printed hexadecimal numbers backwards, failed the scratch RAM's
walking one at bit 20, a patched instruction in FENCE.I and DDR read leveling on lane 0 only.
Seeds 1 and 2 at 69ee491 passed all 572 checks; SurfBoot's DDR build and every build without
DDR work. Comparing every FASM feature of the seven builds, tile coordinates removed, the two
failing ones share nothing that the five working ones lack except one construct: **ordinary
nets routed through unused global buffers onto the clock network**. The chip database lists
each BUFGCTRL's I0 -> O as a route-through pseudo pip (Project X-Ray: "always"; the generator
keeps it, `include_pip` filters only logic, CE and TFB route-throughs), so the router may take a
net from the fabric into a free BUFGCTRL (`CLK_BUFG_BUFGCTRLn_I0 <- CLK_BUFG_IMUXk`, the input
mux's all-clear state), onto a global line (`CLK_BUFG_CK_GCLKm <- BUFGCTRLn_O`), through the
clock rows and back into logic through an interconnect GFAN wire (`GFANn <- GCLK_*`). The FASM
then configures no buffer site (no `IN_USE`, CE0 and S0 left inverted), the round trip agrees
(no bits to differ), and LVS reads the route-through with the same database as a wire.

92a2b51 seed 1 had seven such routes (BUFGCTRL 4 and 10 to 15) into eight inputs of the core's
EX stage: the branch-target adders (`ex_pc` + `ex_imm`, bits 10 and 12), ALU-operation decode,
and the `ex_insn`, `mem_tval` and `trap_epc` registers, which is what an impossible trap needs.
69ee491 seed 3 had one: BUFGCTRL13, GCLK29, into one input of the ALU shifter's first stage for
bits 20 and 21. Every symptom of that board run is a shift through there: `fmt_vformat` decides
whether a conversion is numeric with `0x121021 >> (c - 'd')`, a shift by 20 for `x` (so `%x` took
the string path: digits in buffer order, spaces for zeros, `%u` fine); the walking one is
`1 << 20`; FENCE.I's patch masks the instruction with `slli 12; srli 12`; read leveling compares
lane 0's second byte with `t >> 16` and lane 1's with shifts by 8 and 24. Of the five working
builds four have no such route; seed 1 at 69ee491 has one, into the timer's MSIP flip-flop,
which the interrupt test set and cleared correctly, so a route-through can pass a signal. Which
ones do was not determined: the flow makes none now.

**The fix:** tools/openxc7-build.sh places and routes with a chip database regenerated by the
image's own generator without these 32 pips (devenv/openxc7/chipdb-gen.py; with
`--keep-bufg-rt` it writes the image's `chipdb-xc7a100t.bin` byte for byte), built on the first
use and kept in `build/openxc7/chipdb/<image tag>/`, its SHA-256 checked against
devenv/openxc7/chipdb.sha256. A real BUFG is a placed cell and routes through its own pins, so
clocks are untouched. The two failing netlists rebuilt with it keep their placement exactly
(`placement.json` identical), route the eight and the one net in the fabric, and pass every
check. **The check:** `gclkcheck.py` in the clock check fails a build on any clock line driven
by an unconfigured BUFGCTRL, logic into one, or a clock line into a data input; run on the
seven builds it finds exactly the three with route-throughs. The chip database still has other
site route-throughs: the clock rows' BUFHCE (every clock uses them), the I/O tiles' ILOGIC,
OLOGIC, IDELAY and IOB pass-throughs (every pin uses them), PHASER_IN and BUFR; none was seen
carrying an ordinary net. Evidence: [evidence/flow/bufg-routethrough.txt](evidence/flow/bufg-routethrough.txt).

## 7. Option B: Vivado in an x86-64 container on this Mac

Tested without Vivado ([evidence/flow/amd64-rosetta.txt](evidence/flow/amd64-rosetta.txt)).
Docker Desktop 4.93.0 on macOS 15.1.1 runs `linux/amd64` containers **translated by Rosetta**
(`/run/rosetta/rosetta` is mapped into every x86-64 process; no QEMU). `apt` and an x86-64
OpenJDK 21 work, and translated code ran at 0.82× native speed on a compression test. The CPU
seen by translated code has SSE4.2, AES and POPCNT but **no AVX, AVX2 or FMA**. The VM has 14
CPUs and 7.7 GiB of memory; its disk is sparse, so the Mac's free space (about 84 GiB) is the
limit. A third-party project, ichi4096/vivado-on-silicon-mac (unverified here), reports Vivado
2021.1 to 2024.1 working this way on macOS 15, with about 20 GB to download.

What the owner would do:

1. Keep "Use Rosetta for x86_64/amd64 emulation on Apple Silicon" on in Docker Desktop
   (Settings, General; it is on now). Raise the VM's memory in Settings, Resources (Vivado on a
   100T design may want more than 7.7 GiB) and free disk space for the installation.
2. With an AMD account, download the Vivado ML Standard "Linux Self Extracting Web Installer",
   preferably 2024.1, the newest version reported to work under Rosetta, and check its digest.
3. Install it in an amd64 Ubuntu container in batch mode with only Artix-7 device support,
   into a persistent volume. The board track's `vivado-docker` flow then runs the TCL scripts
   there.

Uncertain until tried: whether Vivado versions after 2024.1 run without AVX; stability and speed
of long runs under Rosetta; memory for the full SoC. The configuration is not supported by AMD.

## 8. What remains to prove on the board

The board is connected and openFPGALoader installed (2026-10-16), and item 6 is shown (below);
the flow's own demos (items 1 to 3) and items 4 and 5 are still to do. Load volatile only, with
the board's own part
(`make program-volatile BIT=build/openxc7/<design>-<board>/<top>.bit BOARD=<board>` once the
board track's `program.sh` exists, or `openFPGALoader -b arty_a7_100t <bit>`), and watch the
USB-UART at 115200 8N1:

1. **flow_demo**: LD4 blinks at 1 Hz; LD5-LD7 count lines; one line per second,
   `SurfCPU openXC7 flow demo #0000`, `#0001`, ...; RESET starts again at `#0000`.
2. **flow_features**: once a second `SurfCPU openXC7 features #nnnn rom=1B79ECB1 mul=...
   pix=...`. `rom=1B79ECB1` means the block RAM contents arrived. `mul` is the high word of
   `0x1B79ECB1 × {n, n}` (`00001B7A`, `000036F4`, `0000526E` for lines 1-3). From the second line
   on, `pix=01801E6B` within about ±8 means both clock managers run as configured: the count is
   a frequency ratio from one oscillator, so the oscillator's tolerance cancels. LD4 blinking
   at 1 Hz, LD7 on (both locked), LD6 blinking at about 0.75 Hz, SW1 inverting LD6, SW0 pulling
   `ck_ss` (shield J6) low and LD5 following that pin.
3. Optional: `flow_features` built with `-D FLOW_PIX_PLANNED`. If nextpnr's encoding were right
   it would read `pix=01802CD9` (25,177,305); any other value shows what the mis-encoding does.
4. The timing estimates against Vivado's for the same designs (option A or B).
5. On a 35T: whether logic placed outside Vivado's 35T region works (section 6.3).
6. The DDR designs (BOARD.md section 12, steps 4 and 5): whether LiteDRAM's PHY as this flow
   placed and routed it, with its I/O timing unchecked here (section 6.7), reads the DDR3L back
   inside a window of read delays on both byte lanes, and the DDR tests pass at 640 MT/s.
   **Shown on the owner's A7-100T** with the fixed flow (0bcadbc): both lanes at bitslip 2,
   delay 16 (window +-8), `BRAMDIAG: PASS (572 checks)`
   ([evidence](evidence/physical/ddr-fixed-flow-2026-10-17.txt)).

## 9. Files

| Path | What |
|---|---|
| `devenv/openxc7/Dockerfile`, `build-image.sh`, `versions.sh` | the image |
| `devenv/openxc7/bitinfo.py` | bitstream inspector (UG470), with `--self-test` |
| `devenv/openxc7/fasmcheck.py` | round-trip check |
| `devenv/openxc7/bramcheck.py` | block RAM contents check |
| `devenv/openxc7/cmtcheck.py` | MMCM/PLL divider and phase check, with `--self-test` and `--table` |
| `devenv/openxc7/synthcheck.py` | synthesis-against-RTL check, with `--self-test` |
| `devenv/openxc7/bramwe.py` | the RAMB18E1 write enables of the LVS proof, with `--self-test` |
| `devenv/openxc7/chipdb-gen.py`, `chipdb.sha256` | the chip database without BUFGCTRL route-throughs and its SHA-256 (section 6.8) |
| `devenv/openxc7/gclkcheck.py` | no ordinary net on the global clock network, with `--self-test` (section 6.8) |
| `devenv/openxc7/report.py` | `timing.txt` and `utilization.txt` |
| `devenv/openxc7/demo/` | flow_demo and flow_features (RTL, testbenches, XDC) |
| `devenv/openxc7/test/` | the Yosys DSP reproducer, the DDR3 pin experiment, and the probes of the checker fixes (`bram_we_sdp.v`, `bram_we_tdp.v`, `bram_we_tdpb.v`, `pll_sized.v`, `lvs_bram_*`, `cmt_phase_inner.sh`) |
| `tools/openxc7-build.sh`, `openxc7-demo.sh`, `openxc7-checks-test.sh` | the build entry, the proof run, the checkers' regression (round ddr-3) |
| `tools/openxc7-fetch.sh`, `openxc7-xdc.sh`, `openxc7-flow-test.sh` | Digilent's XDC (pinned), its conversion, the script tests |
| `mk/area-flow.mk` | `lint-flow`, `test-flow`, `flow-xdc-check` |
| `docs/evidence/flow/` | the measurements behind this page |
