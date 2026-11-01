# DDR3L: the bridge, the controller, and how they fit

The Arty A7 has 256 MB of DDR3L behind a 16-bit bus (Micron MT41K128M16JT-125 on the documented
board revisions; **the owner still has to confirm the part**, INVENTORY.md G1). SurfCPU reaches
it through the 256 MiB window at `0x8000_0000` (MEMORY-MAP.md). Three pieces sit in between:

```
 surfcpu_soc            surf_ddr_bridge                         memory controller
 ddr_* master  ---->  SurfBus slave | mailboxes |  MIG UI  ---->  MIG 7 Series (Vivado flow)
 port (sys_clk)       (sys_clk)     | (CDC)     | (ui_clk)   or   surf_ddr_litedram_adapter ->
                                                                LiteDRAM (open-source flow)
 ddrc_* master ---->  surf_ddr_csr: SurfBus slave | mailboxes | Wishbone master ----> LiteDRAM's
 port (sys_clk)       (LiteDRAM only: its control registers, for the firmware's   control port
                       initialization and read leveling)                          (wb_ctrl)
```

* `surf_ddr_bridge` (`rtl/soc/ddr/`): the SurfBus slave of MODULES.md, crossing from sys_clk
  (25 MHz) to the controller's clock, turning 32-bit accesses with byte enables into 128-bit MIG
  beats with byte masks, reporting calibration, and answering a stuck access with an error.
* The controller: AMD's MIG 7 Series for the Vivado flow (`ip/mig/`), or LiteDRAM for the open
  flow (`ip/litedram/`, with `surf_ddr_litedram_adapter` in front of its native port).
* The board top instantiates the controller and the bridge and owns the clocks: with LiteDRAM,
  `surfcpu_arty_a7_top` built with `ARTY_HAS_DDR` (BOARD.md section 7; the DDR designs
  `surfcpu_arty_a7_ddr_bramdiag` and `surfcpu_arty_a7_ddr`).
* With LiteDRAM, `surf_ddr_csr` (`rtl/soc/ddr/`) too: since round ddr-3 the core has no
  calibration CPU of its own, and SurfCPU initializes the DDR3L and levels its read path
  through the core's control registers (the SoC's `ddrc` window; section 10.1).

## Status (2026-10-17)

| Part | Simulation | Synthesis, place and route | On the board |
|---|---|---|---|
| `surf_ddr_bridge`, `surf_ddr_mailbox` | all tests pass (section 8): MIG model, LiteDRAM's controller, Icarus, 7 clock pairs, mutation check; formal proofs of the mailbox and of the bridge's SurfBus obligations | Yosys `synth_xilinx`: 247 LUTs, 387 flip-flops, no vendor primitives (an open-tool estimate); in the DDR designs, placed and routed by the open flow | **works on the board** (the owner's A7-100T, with LiteDRAM): the diagnostics' DDR tests pass through it (data lines, byte lanes, address lines and aliases, boundaries, patterns, sustained copies at 720 KiB/s, code run from DDR), and SurfOS runs from DDR through it [evidence](evidence/physical/ddr-fixed-flow-2026-10-17.txt) |
| MIG recipe (`ip/mig/`) | the bridge is tested against a behavioural model of the MIG's user interface, not the MIG | **pending**: needs Vivado (no build host, BUILD-HOST.md); recipe dry-run only | **pending** (needs Vivado) |
| LiteDRAM core (`ip/litedram/`): no CPU, 80 MHz (round ddr-3) | its real controller, generated from the board's configuration, passes the bridge tests through the adapter, and SurfCPU's firmware initializes it through its registers and runs the BRAM diagnostics' DDR tests in the whole board top; the exact generated core, PHY included, takes the firmware's initialization and fails the leveling cleanly with no memory on its pins (sections 10.2, 10.4) | generated reproducibly, its hashes, register map and PHY settings checked; **the DDR designs pass every check of the open flow on both boards at 80 MHz** (section 10.3, BOARD.md section 8); 100 MHz does not close in nextpnr | **works on the board** at 80 MHz (the DDR3L at 640 MT/s): initialized and read-leveled by the firmware, both lanes at bitslip 2, delay 16 (window +-8); SurfBoot calibrates in 187 ms; 256 MiB pass the diagnostics' 572 checks [evidence](evidence/physical/ddr-fixed-flow-2026-10-17.txt) |
| `surf_ddr_csr` (round ddr-3) | Icarus testbench, six runs at five clock pairs, 9 of 9 mutants caught; in the whole DDR top with both cores | in the DDR designs | **works on the board**: the firmware's initialization and read leveling go through it (the same evidence) |
| `firmware/common/ddrinit.c` (round ddr-3) | host model and emulator (SurfBoot and the diagnostics, lane windows, failures), RTL with LiteDRAM's controller and with the board core | in SurfBoot 1.1.0 and the BRAM diagnostics | **works on the board**: the diagnostics initialize and level the DDR3L in 229 ms, SurfBoot calibrates it in 187 ms, both lanes bitslip 2, delay 16 (window +-8) [evidence](evidence/physical/ddr-fixed-flow-2026-10-17.txt) |
| `surf_ddr_litedram_adapter` | tested against LiteDRAM's controller; 5 of 5 mutants caught | in the DDR designs | **works on the board** (in both DDR designs, the same evidence) |

On the owner's Arty A7-100T (2026-10-16/17) milestone 4's physical tests pass: initialization
and read leveling, calibration, data lines, byte lanes, address lines and aliases, boundaries,
patterns, sustained copies and code running from DDR (`BRAMDIAG: PASS (572 checks)`), and
SurfBoot loads SurfOS into the DDR3L and starts it ([evidence](evidence/physical/ddr-fixed-flow-2026-10-17.txt),
[SurfOS](evidence/physical/surfos-boot-2026-10-16.txt)). Two placements of the diagnostics
first failed on the silicon because nextpnr had routed ordinary nets through unused BUFGCTRL
route-throughs; the flow no longer does, and with that fix both pass (section 10.3, OPEN-FLOW.md
section 6.8, [evidence](evidence/physical/ddr-routethrough-fix-2026-10-16.txt)). Still open:
the MIG and Vivado's sign-off (section 11).

## 1. The bridge's ports

`surf_ddr_bridge` follows MODULES.md section 3. Sys side, in the `clk` domain:

| Port | Dir | Meaning |
|---|---|---|
| `clk`, `rst` | in | sys_clk and its synchronous reset (the SoC's) |
| SurfBus slave `s_req_*`, `s_rsp_*` | | MODULES.md section 2; connect to `surfcpu_soc`'s `ddr_*` master port |
| `calib_done` | out | `sysinfo.DDR_STATUS.CALIB_DONE`: the DDR is usable; to `surfcpu_soc.ddr_calib_done` |
| `calib_fail` | out | `DDR_STATUS.CALIB_FAIL`: calibration did not succeed in time; to `ddr_calib_fail` |
| `timeout` | out | `DDR_STATUS.TIMEOUT`: the watchdog abandoned an access; sticky until `rst`; to `ddr_timeout` |

Controller side, in the `ui_clk` domain: the MIG 7 Series native user interface (UG586), names
and meanings as the MIG has them, so the board top connects them one to one:

| Port | Width | Dir | Meaning |
|---|---|---|---|
| `ui_clk` | 1 | in | the controller's clock (MIG: 81.25 MHz; LiteDRAM: 80 MHz since round ddr-3) |
| `ui_rst` | 1 | in | MIG `ui_clk_sync_rst` (active high, synchronous to `ui_clk`); it must also reset the controller |
| `init_calib_complete` | 1 | in | MIG calibration done (`ui_clk` domain) |
| `app_addr` | 28 | out | rank (bit 27, always 0) + bank + row + column, counting 16-bit words; bits 2:0 are 0 (one BL8 burst) |
| `app_cmd` | 3 | out | `000` write, `001` read |
| `app_en` | 1 | out | command valid; held, with `app_addr`/`app_cmd`, until `app_rdy` |
| `app_rdy` | 1 | in | the controller takes the command in this cycle |
| `app_wdf_data` | 128 | out | write beat: the 32-bit word repeated in all four lanes |
| `app_wdf_mask` | 16 | out | byte mask, **1 = leave that byte unchanged** (MIG polarity) |
| `app_wdf_wren`, `app_wdf_end` | 1, 1 | out | write data valid; `app_wdf_end` with every beat (a beat is a whole burst at 4:1) |
| `app_wdf_rdy` | 1 | in | the controller takes the write data in this cycle |
| `app_rd_data` | 128 | in | read beat |
| `app_rd_data_valid`, `app_rd_data_end` | 1, 1 | in | read data valid (one beat per read; `_end` is unused) |
| `app_sr_req`, `app_ref_req`, `app_zq_req` | 1 each | out | tied 0: no self-refresh, user refresh or ZQ requests |

Parameters (MODULES.md names only `TIMEOUT_CYCLES`; the others are additions with defaults, see
"Proposed contract changes"):

| Parameter | Default | Meaning |
|---|---|---|
| `TIMEOUT_CYCLES` | 4096 | sys_clk cycles after acceptance at which the watchdog answers (164 us at 25 MHz; a normal access takes 5 to 20) |
| `CALIB_TIMEOUT_CYCLES` | 37,500,000 | sys_clk cycles after `rst` for calibration to succeed (1.5 s at 25 MHz; 0 = no limit): under SurfBoot's and the BRAM diagnostics' own 2 s wait, so they see `calib_fail` first; with LiteDRAM the firmware's own initialization and read leveling end 0.3 to 0.4 s after reset in simulation (section 10.1). The real time must be measured on the board (the MIG's too) |
| `APP_ADDR_WIDTH` | 28 | MIG `app_addr` width |
| `APP_DATA_WIDTH` | 128 | MIG app data width; must be 8 x `DQ_WIDTH` (one BL8 burst per beat, 4:1), else elaboration stops |
| `DQ_WIDTH` | 16 | memory data width |

## 2. What the SoC sees

* **One access at a time** (SURFBUS.md rule 6, and the SoC's DDR arbiter): `s_req_ready` is
  `!rst && !busy`; the next request can be offered in the cycle of the previous response.
* **Accepted while usable** (`calib_done` and no timeout): the access goes to the controller and
  is answered when the controller is done. Latency (sys cycles from the accepting edge to the
  response cycle, MIG model at 81.25 MHz with 12-32 cycles of read latency): 5 to 20, average 9;
  back to back, 6.8 cycles per write and 13.4 per read. Against LiteDRAM's controller at 80 MHz:
  6.7 per write and 10.3 per read. These are simulation figures with modelled controller latency.
* **Refused** before calibration, after calibration failed, and after a timeout: the response
  is an error in the cycle right after acceptance, and nothing reaches the controller. (The SoC
  refuses DDR accesses itself while `ddr_calib_done` is 0, MODULES.md section 4; the bridge
  refuses too.)
* **Reads** return the whole aligned word (byte enables are ignored, SURFBUS.md section 4);
  **writes** change exactly the enabled bytes.
* **The watchdog**: an access not answered `TIMEOUT_CYCLES` cycles after acceptance gets an
  error response in that cycle, `timeout` goes to 1 and stays until `rst`, and every later
  access is refused at once (fail-stop): a controller that stopped answering is reported, not
  waited on, and nothing it answers late can be taken for the answer to another access.
* **Calibration**: `calib_done` follows `init_calib_complete` (two-flop synchronizer, one more
  register). If it has not been seen within `CALIB_TIMEOUT_CYCLES` of `rst`, `calib_fail` goes to
  1 and `calib_done` stays 0 until `rst`, even if the controller finishes later: the DDR is
  declared failed rather than used after an abnormal calibration. `calib_done` and `calib_fail`
  are never both 1. If calibration is lost later (a controller reset), `calib_done` drops and
  comes back when the controller calibrates again; that is not a failure.

## 3. Address and data mapping

The window offset `off = addr[27:0]` (`SURF_DDR_MASK`) maps one to one onto the memory:

* `app_addr = {1'b0, off[27:4], 3'b000}`: 16-byte beats, BL8-aligned; the top bit is the rank.
* Lane `off[3:2]` of the beat holds the word; byte `k` of the beat is bits `8k+7:8k` of
  `app_wdf_data`/`app_rd_data` (DQ beat `i` is bits `16i+15:16i`, its low byte first: the
  little-endian order of SurfBus).
* Writes: `app_wdf_data = {4{wdata}}`, `app_wdf_mask = ~(be << 4*lane)`.
* Reads: `rdata = app_rd_data[32*lane +: 32]`.
* With the MIG's BANK_ROW_COLUMN mapping (Digilent's choice) the offset splits into bank
  `off[27:25]`, row `off[24:11]`, column `off[10:1]`: rows are 2 KiB, banks 32 MiB. The tests
  cross these boundaries; the mapping itself is the MIG's business.

LiteDRAM's native port counts 128-bit beats, so the adapter sends `app_addr[26:3]` as its 24-bit
`cmd_addr`.

## 4. How an access runs on the MIG interface

* **Write**: the write data goes first (`app_wdf_wren` until `app_wdf_rdy`), then the command
  (`app_en` until `app_rdy`). UG586 allows data before its command (it also allows up to two
  cycles after, which the bridge never needs). The response is sent when the command has been
  taken: from then on the write is ordered before anything sent later.
* **Read**: the command, then the bridge waits for `app_rd_data_valid` and takes its lane.
* The bridge never has more than one command in the controller (the model checks this).
* **Ordering**: our MIG configuration asks for strict ordering (section 9.1), so a read after a
  write to the same address can never pass it. With the controller's default ("Normal") that
  would still hold, because requests to the same rank and bank queue in one bank machine in
  order (MIG 7 `bank_queue.v`: "a new request places itself in the existing queue for a
  rank-bank hit"), but strict ordering removes any doubt and costs nothing with one access at a
  time.

## 5. Errors, resets and what they mean

SURFBUS.md section 3 says an error response means the access had no effect. That holds for
every refusal in the cycle after acceptance (before calibration, after a failure or a timeout)
and for the ui side's refusal when `init_calib_complete` is low as a request arrives. Two
errors are different:

* **The watchdog.** An access abandoned **while still waiting to be sent** is never carried out
  (tested: `abandon`). An access abandoned **after it was sent** may still be carried out later,
  in order, if the controller recovers. Software sees `DDR_STATUS.TIMEOUT` and the DDR refuses
  everything until reset, so nothing can observe it before the reset; after a reset (which does
  not erase DDR) the late write may be visible.
* **A controller reset in the middle of an access.** The ui side answers the access it was
  working on with an error as soon as its reset ends (it never leaves a request unanswered); a
  write may or may not have reached the memory before the controller was reset.

Both are documented exceptions to SURFBUS.md section 3 (proposed wording below). They only arise
when the controller stops answering or is reset, which on the board means a fault or the RESET
button.

Resets: `rst` (the SoC's) and `ui_rst` (the controller's) may come and go independently and at
any moment. Neither the mailboxes nor the two flags of section 6 have a reset, and each side
only sends and takes messages outside its own reset, so a reset of one side never invents, loses
or mixes up a message. A request already sent to the ui side when `rst` comes is still carried
out; its answer is dropped, and the first access after the reset waits until it has come
(SURFBUS.md rule 8 says no request survives a reset; this is the one exception, proposed wording
below). In the real board both resets come from the same button, so in practice they happen
together.

## 6. The clock-domain crossing

```
 sys_clk domain                                   ui_clk domain
 accept --> req_box.hold (request) ----------data------> capture u_write u_be u_waddr u_wdata
 inflight   req_box.req ---toggle--> 2-flop sync --> valid (take: flip ack, set u_owe)
            2-flop sync <--toggle--- req_box.ack
 rs_q  <------data------ rsp_box.hold (err, rdata) <---- the ui side's answer (clears u_owe)
            2-flop sync <--toggle--- rsp_box.req
            rsp_box.ack ---toggle--> 2-flop sync
```

* **Mailbox** (`surf_ddr_mailbox`): one message at a time, two-phase handshake. The source copies
  the message into `hold` and flips `req`; the destination sees `req` (through two flip-flops)
  differ from `ack`, takes `hold`, and flips `ack`; the source sees `ack` (through two
  flip-flops) and may send again. `hold` cannot change between a send and the source seeing the
  message taken, so the destination always reads a stable, whole message.
* **No reset in the mailbox** (deliberate): resetting a toggle on one side would look like a new
  message on the other. The toggles and synchronizers start from their initial values (FPGA
  configuration, or time 0 in simulation) and keep their relationship for ever.
* **One request in flight, across resets too**: the sys side keeps `inflight`, a flag with no
  reset that is set when a request is sent and cleared when an answer is taken, and sends a new
  request only while it is clear. So the next answer is always the current request's: an answer
  to a request cut short by `rst` or abandoned by the watchdog arrives (and is dropped) before
  the next request is even sent, and a new request after a reset waits for it.
* **Every request answered**: the ui side keeps `u_owe`, a flag with no reset that is set when it
  takes a request and cleared when its answer is in the mailbox. If its reset interrupts the
  request, it answers it with an error as soon as the reset ends, before taking anything new, so
  `inflight` always clears (unless the controller's clock stops for good, which the watchdog
  covers).
* An earlier version tagged requests with a 4-bit id instead. That was wrong in a corner: with
  the controller stuck through 15 resets, each with an access that timed out, the id wrapped
  around while the first request was still inside, and when the controller recovered its late
  answer was taken for the current request's. The `wrap` test reproduces this on the old RTL
  (k = 15) and passes on this one; the bridge proof (section 8) shows no stale answer is ever
  delivered, without a bound on the number of resets.
* **Capture registers**: crossing data lands only in registers enabled by synchronized signals
  (`u_*` on the ui side, `rs_q` on the sys side); no logic sits between `hold` and those
  registers. This is the form CDC reviews and Vivado's `report_cdc` expect.
* **Calibration status**: `init_calib_complete` crosses with `rtl/lib/surf_sync` (two flops).
* Every synchronizer flip-flop has `ASYNC_REG`. Each crossing costs two or three cycles of the
  destination clock; nothing assumes which clock is faster (tested from ui 2.4 times slower than
  sys to ui 4 times faster, and with 30% jitter on every half period).

What is proven, what is not: the mailbox protocol, and the bridge's SurfBus obligations
including "no stale answer", are proved for every interleaving of the two clocks' edges (any
frequencies and phases, edges at the same instant, either clock stopping), every input sequence
and every pattern of resets (section 8, formal). Simulation cannot show metastability: that
rests on the two-flop synchronizers with `ASYNC_REG` and on the timing constraints below.

## 7. Timing constraints for the board top

sys_clk (the board's PLL) and ui_clk (the MIG's MMCM, or LiteDRAM's PLL: 80 MHz) both come from
the 100 MHz oscillator, so Vivado sees them as related clocks and would time the bridge's
crossings as synchronous paths against the closest edges of 25 MHz and 81.25 (or 80) MHz, a
meaningless requirement. The same holds for `surf_ddr_csr`, whose crossings
`rtl/soc/ddr/surf_ddr_csr.xdc` bounds the same way (six lines, `cdc surf_ddr_csr`).
`rtl/soc/ddr/surf_ddr_bridge.xdc` gives each crossing `set_max_delay -datapath_only` instead:

| From | To | Bound |
|---|---|---|
| `req_box/hold_reg[*]` | the ui capture registers | one ui_clk period |
| `req_box/req_reg` | `req_box/req_sync_reg[0]` | one ui_clk period |
| `req_box/ack_reg` | `req_box/ack_sync_reg[0]` | one sys_clk period |
| `rsp_box/hold_reg[*]` | `rs_q` | one sys_clk period |
| `rsp_box/req_reg` | `rsp_box/req_sync_reg[0]` | one sys_clk period |
| `rsp_box/ack_reg` | `rsp_box/ack_sync_reg[0]` | one ui_clk period |
| ui_clk (the MIG's `init_calib_complete`) | `calib_sync/sync_ff_reg[0]` | one sys_clk period |

One destination period leaves a whole period of margin, since a message is read two or more
destination edges after its toggle is first sampled. The file is scoped to the module; the
board's Vivado script reads it with `read_xdc -ref surf_ddr_bridge rtl/soc/ddr/surf_ddr_bridge.xdc`,
and it finds the clocks through the module's `clk` and `ui_clk` ports. The DDR design lists
(`boards/arty_a7/designs/surfcpu_arty_a7_ddr*.files`) have the line `cdc surf_ddr_bridge
rtl/soc/ddr/surf_ddr_bridge.xdc`, beside the `cdc` lines for `rtl/lib`, and
`tools/check-wiring.py` checks that every cell it names is a register of the bridge or of its
mailboxes. Its `surf_sync.xdc` also bounds the bridge's `calib_sync` stage, with the same value as
the line above; the two agree. **The open flow checks none of this**: nextpnr's XDC reader
ignores `set_max_delay`, so the build lists the file as not checked; nextpnr reports the paths
between sys_clk and LiteDRAM's clock (about 1.5 ns) as unconstrained cross-domain paths, and
only Vivado would hold them to these bounds. Any other path between
sys_clk and ui_clk would still be timed as synchronous and fail, which is the point: there must
be none. At the first build, check `report_cdc` (only these crossings, no "unsafe" entries) and
`report_clock_interaction`. Not yet read by Vivado: `sim/ddr/vivado_dryrun.tcl` only checks that
it parses and that every cell it names is a real register of the RTL.

## 8. Tests

All run in the toolchain container; `make test-ddr` is on `test-soc`, `make lint-ddr` and
`lint-ddr-adapter` on `lint`, `make check-ddr` and `test-ddr-litedram` on `check`.

| Command | What runs | Result (2026-10-15; the rows marked ddr-3 2026-10-16) |
|---|---|---|
| `make lint-ddr`, `lint-ddr-adapter` | Verilator 5.032 `--lint-only -Wall` (default and four other parameter sets for the bridge; `surf_ddr_csr`), Icarus 12 `-g2005 -Wall` | clean |
| `make test-ddr-bridge` | `tb_ddr_bridge --test all`, seeds 1, 2, 3, at 25 MHz / 81.25 MHz | 3 x 13 tests pass |
| `make test-ddr-ratios` | random, sysreset, uireset and lanes at 8 more clock pairs (below; ddr-3 added LiteDRAM's 80 MHz) | 32 runs pass |
| `make test-ddr-defaults` | the bridge with its default parameters, as the board top instantiates it: calibration limit of 37,500,000 cycles (ui clock stopped), the 4096-cycle watchdog (18 cases), 5000 random accesses | pass; `calib_fail` after 37,500,001 cycles |
| `make test-ddr-icarus` | the mailbox in Icarus (5 clock pairs x 5000 messages, clock stops, misuse) and a four-state smoke test of the bridge with a Verilog MIG stand-in (3 seeds x 1094 accesses) | pass |
| `make test-ddr-csr` (ddr-3) | `sim/ddr/tb_ddr_csr.v` (Icarus): the register bridge with short limits at four clock pairs and the board's limits at two, against a Wishbone slave model (random latency, errors, silence): whole-word accesses checked against a reference, 200 non-word accesses and fetches refused at once and never on the bus, slave errors, the controller side's limit, a stopped controller clock (the SoC side's watchdog, the late access, no stale answer), resets of either side cutting accesses, every word read back | 6 runs pass |
| `make test-ddr-litedram` | `tb_ddr_litedram --test all`, seeds 1, 2, plus a jittered run: the bridge and adapter against LiteDRAM's controller generated from the board's `arty.yml` at 80 MHz (section 10.2) | 2 x 9 tests + 1 pass |
| `make board-ddr-sim` (board track's target; ddr-3) | the DDR top in RTL from its pins: LiteDRAM's controller and model with the diagnostics and SurfBoot initializing it, and the generated core with no memory (section 10.4) | pass: 570 diagnostic checks; the leveling with no memory fails as it must |
| `make synth-ddr` | Yosys 0.52 `synth_xilinx` of the bridge | 247 LUTs, 387 flip-flops, 11 CARRY4 (estimate) |
| `make check-mig` | the MIG recipe without Vivado (section 9.4) | pass |
| `make prove-ddr` | SymbiYosys 0.52: the mailbox and bridge proofs (ABC PDR, no depth bound) with covers | proved, covers reached (about 20 s) |
| `make test-ddr-mutants` | 41 deliberately broken copies of the RTL (slow, not on check; ddr-3 added nine of `surf_ddr_csr`) | 40 caught, 1 survives as expected |

The Verilator testbench (`sim/ddr/tb_ddr_bridge.cpp` with `surfbus_bench.h`) runs the bridge
against `sim/ddr/mig_ui_model.{h,cpp}`, a C++ model of the MIG user interface on its own
`ui_clk`: reset then a calibration delay (or none), `app_rdy`/`app_wdf_rdy` dropping at random
and when its queues are full, refresh stalls (14 cycles every 634), in-order read data after
12-32 cycles, one 128-bit beat per command, 256 MiB of sparse memory whose untouched bytes follow
a hash of their address, controller resets and three stuck faults; it checks the user's side of
the interface (commands and data held until taken, `app_wdf_end` with every beat, aligned in-range
addresses, rank 0, no command before calibration, write data no later than two cycles after its
command). A SurfBus master follows SURFBUS.md, and every response is checked against a reference
memory that tracks writes that may or may not have happened. In every test: a response only for
an accepted request, never in reset, never later than the watchdog; `calib_done` and `calib_fail`
never both 1; `timeout` sticky; at most one command in the controller; no acknowledged write lost.

| Test | Covers |
|---|---|
| `basic` | calibration, a write and read at the bottom and top of the window, an untouched word |
| `lanes` | 4 beats x 4 lanes x all 7 byte-enable patterns; after each write all 16 bytes of the beat in the controller's memory are compared with the reference, and all four words are read back with assorted byte enables |
| `bounds` | 108 addresses around the window start, beats, 2 KiB rows, 64 KiB, 1 MiB, 16/32/64/96/128/224 MiB and the window top: read before writing, write, read, and compare with the controller's memory |
| `alias` | walking ones and walking zeros over the 28-bit offset (54 addresses): no two map to the same storage |
| `random` | 20,000 accesses (hot set, anywhere, near boundaries, streams; every byte-enable pattern; gaps 0-4) |
| `stream` | 2048 back-to-back writes, 2048 reads, 512 write-read pairs on one word; cycles per access |
| `precal` | 370+ accesses before calibration: all refused in the next cycle, the controller sees nothing; then all work |
| `calfail` | calibration never comes: `calib_fail` exactly at the limit, accesses refused, a late calibration changes nothing, a reset recovers |
| `watchdog` | 6 faults x 3 seeds (`app_rdy` stuck for a read and a write, `app_wdf_rdy` stuck, read data dropped, ui clock stopped for a read and a write): error exactly at `TIMEOUT_CYCLES`, `timeout` set and kept, later accesses refused at once, the late answer dropped, recovery by reset, the abandoned write settled by a read |
| `abandon` | an access abandoned while the mailbox still holds an earlier one is never carried out; the earlier one is |
| `wrap` | a read stuck behind a stopped controller clock, then k = 13..18 resets each with an access that times out, then recovery: the next read gets its own data, never the stuck read's late answer |
| `sysreset` | 10,000 accesses with `rst` at random moments (about 490 times), cutting accesses before the controller (about 120), inside it (about 150) and after it (about 210) |
| `uireset` | 6000 accesses with about 300 controller resets: refusals at once and error answers from the ui side both occur (accesses cut short are answered, not timed out), and 200 accesses after the last reset all work |

Clock pairs (sys ns : ui ns : jitter): 40 : 12.308 : 0 (the MIG, default), 40 : 10 : 0
(LiteDRAM at 100 MHz, round ddr-2), 40 : 12.5 : 0 (LiteDRAM at 80 MHz), 40 : 40 : 0, 40 : 37.3 : 0, 40 : 97 : 0 (ui slower than sys), 10 : 12.3077 : 0 (sys
faster than ui), 40 : 12.3077 : 0.3 and 13.1 : 41.7 : 0.3 (every half period jittered by up to
30%).

`tb_ddr_litedram.cpp` runs basic, lanes, bounds, alias, random (10,000), stream, precal (before
`init_done`), calerror (a failed initialization as the firmware reports it: `init_error` then
`init_done` over the core's control port; every access refused at once, `calib_fail` at the limit, `calib_done` never) and
sysreset (5000 accesses, about 230 resets) against `tb_litedram_top.v`: the bridge, the adapter
and LiteDRAM's controller, crossbar and native port, generated from the board's `arty.yml` with
`--sim` (LiteDRAM's DRAM model instead of the Artix-7 PHY, its memory zero at the start; section
10.2), whose native port, as on the board, stays blocked until `init_done` without
`init_error`.

The mutation check (`sim/ddr/ddr_mutants.py`) builds each deliberately broken copy and runs the
tests: wrong read lane, inverted or misplaced write mask, shifted `app_addr`, a lost address bit,
swapped read/write command, a new request sent while one is in flight, the in-flight flag reset
by `rst`, no answer for a request a controller reset interrupted (or that flag reset by `ui_rst`),
the error bit taken from the wrong place, the watchdog one cycle early, no fail-stop, no refusal before calibration,
calibration failure ignored, `calib_done` latched, no calibration limit, `app_wdf_rdy`/`app_rdy`
ignored, the ui side not refusing when uncalibrated, the request never taken, a response in
reset, the mailbox sending while full or taking while empty, and five adapter faults (mask
polarity, address shift, command, no write buffer, calibration). Nine more break the register
bridge (round ddr-3), run against `tb_ddr_csr.v` at two clock pairs: non-word accesses carried,
fetches carried, a request sent while one is in flight, a stale answer taken for a later
access, no watchdog, a request cut by a controller reset left unanswered, address bit 2 lost,
Wishbone errors ignored, no Wishbone limit; all nine are caught. 40 of 41 are caught. The one
that survives removes the first synchronizer stage of the request toggle: metastability is not
simulated, which is why the synchronizers rest on review and constraints.

Formal (`sim/ddr/formal/`, SymbiYosys with ABC PDR; unbounded proofs, synchronizers as plain
flip-flops):

* `mailbox.sby`: with both clocks and all inputs free, at most one message is ever in the box,
  `dst_valid` only while there is one and `dst_data` is then that message, `src_ready` only when
  the box is empty. Proved; a cover shows three messages getting through; the proof fails, as it
  must, on a mailbox that sends while full.
* `bridge.sby`: the whole bridge with a legal SurfBus master, a completely free controller (any
  `app_rdy`, `app_wdf_rdy`, read data at any time or never, `init_calib_complete` and `ui_rst` at
  any time), free clocks and a free `rst`, with `TIMEOUT_CYCLES` = 4 and a calibration limit of
  3. Proved:
  * a response only while an accepted request waits for one, never in reset (so exactly one
    response per request);
  * no request waits more than `TIMEOUT_CYCLES` + 1 cycles;
  * a successful response only after the controller accepted a command since its request was
    accepted: no stale answer (from a request cut short by `rst` or abandoned by the watchdog) is
    ever delivered for a later one, however many resets come in between;
  * `calib_done` and `calib_fail` never both 1; `timeout` sticky until `rst`.

  These are SURFBUS.md rules 4, 7 and 8 for any controller behaviour, which is what the watchdog
  and the in-flight flag are for. A cover reaches a successful answer. The proof fails, as it
  must, on bridges with a response in reset, no watchdog, calibration failure ignored, `timeout`
  cleared by a refused access, a request sent while another is in flight, or the in-flight flag
  reset by `rst`. A bridge that never answers an interrupted request still passes (it only
  stops recovering: every later access times out), which is why `uireset` and `watchdog` check
  recovery in simulation.

## 9. The MIG (Vivado flow)

### 9.1 Configuration

From Digilent's board files, `github.com/Digilent/vivado-boards` at commit
`d8d43ed9f3f7e367ee3de82782ed601d88305677`, `new/board_files/arty-a7-{100,35}/E.0/1.1/mig.prj`
(MIT licence, Copyright (c) 2021 Digilent, Inc.). They are kept unchanged in `ip/mig/digilent/`
with their SHA-256 (`SOURCE.txt`), fetched and re-verified by `ip/mig/fetch-digilent.sh`. They are
for board revision **E.0** (to confirm on the owner's board, G1).

| Setting | Value |
|---|---|
| MIG version | 7 Series 4.2 (Vivado 2018.3 and later) |
| Parts | `xc7a100t-csg324/-1` (A7-100), `xc7a35ti-csg324/-1L` (A7-35) |
| Memory | `MT41K128M16XX-15E` timing (the board's -125 part is faster and meets it), 1.35 V, 16 bits, 256 MiB, data mask, no ECC |
| Clock | 3077 ps: memory clock 325 MHz (650 MT/s), PHY 4:1, **ui_clk = 81.25 MHz** |
| Input clock | 99.997 MHz (the 100 MHz oscillator) |
| Address map | BANK_ROW_COLUMN; 3 bank, 14 row, 10 column bits; app_addr 28 bits |
| Mode registers | BL8 fixed, CL 5, CWL 5, RZQ/6 drive and ODT, dynamic ODT off |
| Other | internal Vref; XADC temperature monitoring enabled; 4 bank machines; one extra ui clock (MMCM CLKOUT0 / 3.25, about 200 MHz) |
| Pins | all 48 DDR3 pins with SSTL135 / DIFF_SSTL135 (the MIG writes their constraints) |

`ip/mig/make_prj.py` derives `ip/mig/<board>/mig_ddr3.prj` (`arty_a7_100t`, `arty_a7_35t`) with
four changes, each matched exactly once so a changed upstream file stops it:

1. `ModuleName` → `mig_ddr3`.
2. `SystemClock` Single-Ended → **No Buffer**, and the E3 pin entry goes: the board top owns the
   oscillator's input buffer and feeds the MIG from a global buffer, because the SoC's own clocks
   come from the same pin and must not depend on the MIG. (A Single-Ended MIG input on E3 is also
   reported to fail implementation when other bank-35 pins are used: viktor-nikolov's Arty
   MicroBlaze-DDR3 tutorial.)
3. `Ordering` Normal → **Strict** (section 4).
4. `PortInterface` AXI → **NATIVE**, and the AXI parameters go (the bridge talks to `app_*`).

`make_prj.py --check` (on `check`) also reads the result back and checks it against the bridge:
app_addr = 1 + 3 + 14 + 10 = 28 bits and app data = 2 x 4 x 16 = 128 bits equal the bridge's
defaults, and ui_clk is 81.25 MHz.

### 9.2 Clocks and reset

| Clock | From | To |
|---|---|---|
| 100 MHz | oscillator pin E3 → `IBUF` → `BUFG` (board top) | MIG `sys_clk_i` (No Buffer) and the board's PLL/MMCMs |
| 200 MHz reference | the board's PLL (VCO 800 MHz: /4 = 200 MHz, /32 = 25 MHz sys_clk) | MIG `clk_ref_i` (No Buffer): the IDELAYCTRL reference (200 MHz ± 10 MHz) |
| ui_clk, 81.25 MHz | the MIG's MMCM | the bridge's `ui_clk` |

Digilent's configuration also makes `ui_addn_clk_0` (about 200 MHz); looping it back into
`clk_ref_i` is the alternative its preset has in mind. Taking the reference from the board's PLL
keeps the IDELAYCTRL reference independent of the MIG's own clocking and is what this document
recommends; the board track decides (BOARD.md).

Reset: MIG `sys_rst` is asynchronous and **active low** (the MIG synchronizes it). Drive it with
`ck_rst_n && pll_locked`: the RESET button, held until the PLL that makes the reference clock is
locked. `ui_clk_sync_rst` goes to the bridge's `ui_rst`. The SoC's `rst` comes from
`surf_reset_sync` on sys_clk with the same button and lock. `init_calib_complete` goes to the
bridge only.

### 9.3 The board top

`ip/mig/mig_ddr3_wiring_example.v` is the wiring, lint-checked against the expected MIG port
list (`ip/mig/mig_ddr3_stub.v`, a black box) and the bridge:

* the MIG's DDR3 pins straight to the top-level `ddr3_*` ports. Their locations, I/O standards
  and the internal Vref come from the MIG's generated constraints: **do not** repeat them in the
  board XDC;
* `sys_clk_i` ← the BUFG'd 100 MHz, `clk_ref_i` ← 200 MHz, `sys_rst` ← `ck_rst_n && pll_locked`;
* the MIG's `app_*`, `ui_clk`, `ui_clk_sync_rst`, `init_calib_complete` ↔ the bridge;
* the bridge's SurfBus port ↔ `surfcpu_soc`'s `ddr_*` port, and `calib_done`/`calib_fail`/
  `timeout` → `ddr_calib_done`/`ddr_calib_fail`/`ddr_timeout`; `HAS_DDR` = 1;
* left open: `app_sr_active`, `app_ref_ack`, `app_zq_ack`, `ui_addn_clk_*`, `mmcm_locked`,
  `device_temp`. The MIG instantiates the XADC itself for temperature tracking, so the design must
  not use the XADC elsewhere (or must take the temperature from `device_temp`);
* the board XDC creates the 100 MHz clock on E3 (Vivado derives the rest) and the board's script
  reads `surf_ddr_bridge.xdc` scoped to the bridge (section 7).

### 9.4 Generating it

```sh
vivado -mode batch -nojournal -source ip/mig/mig_ddr3.tcl -tclargs arty_a7_100t build/mig
```

`ip/mig/mig_ddr3.tcl` (non-project): an in-memory project for the part, `create_ip mig_7series`
version 4.2 exactly, the project file copied into the IP directory with `XML_INPUT_FILE` and the
board parameters set to Custom, `generate_target` (instantiation template, synthesis, simulation),
`synth_ip` (out-of-context checkpoint), and a summary of every CONFIG property. The board's
build then does `read_ip build/mig/mig_ddr3/mig_ddr3.xci`.

**Pending: never run** (no Vivado host, BUILD-HOST.md). `make check-mig` checks what can be checked
without Vivado: the derived project files are up to date and fit the bridge, the Digilent files
match their SHA-256, the wiring example lints, and a dry run of the Tcl recipe and of the XDC
(`sim/ddr/vivado_dryrun.tcl`, in the Tcl interpreter inside Yosys, with stubbed Vivado commands)
calls the expected commands with the expected values. The first Vivado run must:

* compare the generated port list (`mig_ddr3.veo`) with `mig_ddr3_stub.v` (unsure: whether
  `ui_addn_clk_1..4` and `mmcm_locked` exist) and fix the stub;
* check that MIG accepts `<Ordering>Strict</Ordering>` and generates `ORDERING = "STRICT"`
  (if not, Normal is also safe, section 4), and `nCK_PER_CLK = 4`, `ADDR_WIDTH = 28`,
  `APP_DATA_WIDTH = 128`;
* close timing with the bridge (the ui side's 4:1 read mux and mask decode at 81.25 MHz) and
  review `report_cdc`;
* keep `mig_ddr3_summary.txt` and the logs in `docs/evidence/ddr/`.

## 10. The LiteDRAM alternative (open-source flow)

For BUILD-HOST.md option C (no Vivado, so no MIG) the DDR designs use a LiteDRAM core:
`surfcpu_arty_a7_ddr_bramdiag` and `surfcpu_arty_a7_ddr` (BOARD.md sections 1 and 7), the board
top with `ARTY_HAS_DDR`. **Round ddr-3 (2026-10-16)** changed two things, both forced by the open
flow's timing (10.3): the core has **no calibration CPU** (SurfCPU initializes it, 10.1), and
it runs at **80 MHz** (DDR3L at 640 MT/s), not 100.

### 10.1 Generating it, and who initializes it

* `make litedram-setup` (`ip/litedram/setup.sh`) builds a pinned, vendored generator in
  `build/third_party/litedram/`: Migen `e19524c9`, LiteX `b6ae9e0b` and LiteDRAM `c454a440`
  (both tag 2026.08), the pythondata repositories (`642ecfed`, `6a13ccce`, `6eb76609`,
  `a12c3f59`; the core no longer uses a CPU, but LiteX's liblitedram sources come with LiteX),
  and the PyPI wheels PyYAML 6.0.2, packaging 25.0, colorama 0.4.6, meson 1.9.1 and ninja 1.13.0
  with checked SHA-256, unpacked without pip. It needs the network once.
* `make litedram-gen` generates `ip/litedram/arty.yml` into `build/litedram/arty/` and the
  simulation core into `build/litedram/arty_sim/` (10.2), writes `build/litedram/summary.txt`
  (`ip/litedram/inspect_core.py`, `estimate.sh`), checks what the RTL and the firmware rely on
  (below) and checks the files against `ip/litedram/arty.sha256` (`ip/litedram/lock.sh check`;
  also `make litedram-check`, on `check-openflow`).
* **A build product, reproducible and checked.** The generated Verilog is never committed.
  `ip/litedram/generate.sh` makes it byte for byte the same on every run and in every checkout:
  SOURCE_DATE_EPOCH is the pinned LiteDRAM commit's time (1790239353), Python's clock is frozen
  at it for LiteX's "Date" lines (`ip/litedram/run_gen.py`), TZ is UTC, the generator starts in
  a fixed environment (`env -i`, PYTHONHASHSEED 0), and `run_gen.py` replaces the instance tree
  LiteX writes into the Verilog as a comment with a fixed note. LiteX lists each module's
  black-box instances in that tree sorted by their default string, which holds the object's
  address, so their order followed the Python process's memory layout: make's extra variables
  reordered it in one checkout, and a fresh checkout elsewhere (the release track's: 16
  generations, 5 files) still got other orders, while the Verilog outside the comment never
  varied. `make litedram-repro` (`ip/litedram/repro.sh`) generates both cores again in eight
  processes that differ in PYTHONHASHSEED, environment, output path and working directory, and
  passes only if they give one set of files, the locked one; with the comment kept, its control
  runs show the order moving (docs/evidence/ddr/litedram.txt). `ip/litedram/arty.sha256` records the five files the
  designs, the firmware and the tests rely on: the core, its constraints, its register map
  (`csr.csv`), its PHY settings and DRAM initialization (`software/include/generated/
  sdram_phy.h`) and the simulation core; a deliberate change (generator, `arty.yml`, the image)
  is recorded with `ip/litedram/lock.sh write`. The design lists name the list on a `generated`
  line, and `tools/fpga-build.sh` counts a build clean only if every generated file it reads
  has its recorded hash (`generated_files_match` in the manifest), and refuses to start without
  them.

**The configuration** (`ip/litedram/arty.yml`): LiteDRAM's Arty example with one native user
port, `cpu: None` and `sys_clk_freq: 80e6`. What came out (2026-10-16):

* `litedram_core.v` with the DDR3 pins, `clk` (100 MHz in) and `rst`, `pll_locked`,
  `init_done`/`init_error`, the native user port (`cmd_addr[23:0]` in 128-bit beats, `cmd_we`,
  `wdata_data[127:0]`, `wdata_we[15:0]` byte enables (1 = write), `rdata_data[127:0]`,
  valid/ready on each, `user_clk`/`user_rst` at 80 MHz) and, because there is no CPU, a
  **Wishbone control port** `wb_ctrl_*` (30-bit word address, 32-bit data, classic cycles) on
  the core's control and status registers: `ddrctrl` (init_done, init_error) at 0x000, `ddrphy`
  (the A7 PHY's reset, lane select, read/write phase and the read delay and bitslip strobes) at
  0x800, `sdram` (DFII: who drives the DRAM, and four phase injectors) at 0x1000.
* Clocking inside: one `PLLE2_ADV` (VCO 1600 MHz) fed from the board's BUFG, giving 200 MHz
  (IDELAYCTRL reference), 80 MHz (core, user port, control port), 320 MHz and 320 MHz at 90
  degrees (DDR3 at 640 MT/s; CL 6, CWL 5 as at 100 MHz; the generator's timings follow the
  clock).
* Vendor primitives: ISERDESE2 x16, OSERDESE2 x45, IDELAYE2 x16, IDELAYCTRL x1, IOBUF x16,
  IOBUFDS x2, OBUFDS x1, BUFG x4, PLLE2_ADV x1, FDCE x8, FDPE x8; the simulation core uses none.
* No ROM, SRAM, VexRiscv or console: round ddr-2's core carried LiteX's BIOS on a VexRiscv
  (lite) in the controller's own clock domain, whose bus paths were half of the paths that
  missed 100 MHz, and its console on ChipKit IO1 (`dram_uart_tx`, gone from the top, XDC and
  WIRING.md).
* Constraints shipped: two false paths and one max delay for LiteX's reset synchronizers
  (`litedram_core.xdc`), no pins, I/O standards or clocks (the board's XDC supplies them).
* Yosys estimate of the core alone (`estimate.sh`): 2,668 LUTs, 1,847 flip-flops, 32 RAM32M,
  no block RAM (round ddr-2's, CPU and BIOS included: 5,492 LUTs, 3,361 flip-flops, 19 block
  RAMs).

`inspect_core.py` checks on every `make litedram-gen` what the RTL and the firmware rely on: the
native port's widths, init_done/init_error, user_clk/user_rst and the wb_ctrl port's widths; no
CPU in either core; every register of spec/soc.toml's `ddrc` block at the offset and with the
access the generator's `csr.csv` gives it, and every register of `csr.csv` in the block;
`firmware/common/ddrinit_phy.h` (phases, modules, delays, bitslips, data bits, read and write
phase, CL, CWL, the mode register values and their order, ZQCL) equal to the generated
`sdram_phy.h`; the simulation core's DFII and init registers at the board core's offsets; and
the board top's lint stand-in with exactly the core's ports.

**Who initializes it: SurfCPU.** `rtl/soc/ddr/surf_ddr_csr.v` carries the SoC's `ddrc`
window (`0x1000_A000`, 8 KiB; MEMORY-MAP.md, spec/soc.toml) across to the controller's clock and
onto `wb_ctrl`: whole aligned words only (anything else, and a fetch, is refused at once), two
`surf_ddr_mailbox` crossings with the bridge's never-reset in-flight flags, a watchdog on each
side (1024 sys_clk cycles after acceptance on the SoC side, 256 controller cycles for the
Wishbone answer), no fail-stop: every access is answered, by the controller or with an error,
and an answer always belongs to its access. Its bounds are in `surf_ddr_csr.xdc` (six
`set_max_delay -datapath_only`, Vivado only). `sysinfo.DDR_STATUS.SWINIT` (the SoC's
`DDR_SWINIT`, 1 in the DDR designs) tells the firmware.

The firmware's sequence is `firmware/common/ddrinit.c`: LiteX 2026.08's liblitedram
(`sdram_init`, the generated `init_sequence()`, the exhaustive read leveling and its LFSR
patterns; BSD-2, its notice kept) ported to the `ddrc` registers, in SurfBoot 1.1.0 and the
BRAM diagnostics (FIRMWARE.md section 5): DFII to software, PHY reset pulse, RESET# released
with CKE low for 1 ms, CKE high, MR2, MR3, MR1, MR0 (0x200, 0x0, 0x6, 0x920), ZQ calibration
long; then for each byte lane every bitslip (8) and read delay (32 taps), each tap tried with
three pseudo-random bursts written to row 0 of bank 0 through the phase injectors and read
back, the bitslip with the best score chosen, its delay set to the middle of its widest
working window and checked; then DFII back to the controller and `init_done` (with
`init_error` first when a lane has no window). About 0.2 s at 25 MHz (0.245 s in the emulator
with the scans printed, 0.24 s in surfsim); the patterns are computed once rather than for
every probe (LiteX's 128 LFSR steps per probe cost as much as the probe on SurfCPU). The
bridge's 1.5 s calibration limit now counts from reset with the firmware's start and the
initialization in it (spec/soc.toml, CALIB_FAIL). A PHY whose phase registers do not read back
(the simulation core's model, 10.2) is initialized without leveling, as LiteX does for a PHY
that is not read-leveling capable.

`surf_ddr_litedram_adapter` (`rtl/soc/ddr/`) puts LiteDRAM's native port behind the bridge's
MIG-style port, on `user_clk`/`user_rst`: commands straight through (`cmd_addr = app_addr[26:3]`,
`cmd_we` for a write), a two-beat write FIFO (`rtl/lib/surf_fifo`) because the bridge gives write
data before the command and LiteDRAM only takes it after (the mutation check confirms that
without it the bridge deadlocks), `wdata_we = ~app_wdf_mask`, read data always taken
(`rdata_ready = 1`), `init_calib_complete = init_done && !init_error`, `ui_rst = user_rst`.

### 10.2 The simulation core and what it shares with the board's

`make test-ddr-litedram` and `board-ddr-sim`'s model runs use `litedram_sim`, generated from the
same `arty.yml`, unchanged, with `--sim`: LiteDRAM's DRAM model (SDRAMPHYModel) replaces the
Artix-7 PHY, the PLL and the chip. `inspect_core.py` compares the two cores on every
`make litedram-gen`:

* the same: the DRAM module (MT41K128M16) and its timings, geometry, the controller settings
  (command buffers 16 deep, bank machines, refresh), the native port and its blocking until
  `init_done` without `init_error`, CL 6 and CWL 5, the 4:1 phases and write phase, the DRAM
  initialization sequence itself, and the 27 DFII and init registers at the same offsets;
* different: the PHY (A7DDRPHY with its ISERDESE2/OSERDESE2/IDELAYE2, 32 delay taps and 8
  bitslips, its 13 registers; the model has no PHY registers, which read 0 there, and returns
  what was written with no delay to level), the read phase (3 on the board, 2 in the model) and
  the clock (a PLL on the board).

One quirk of the model matters to the firmware: its read data arrive on all four DFI phases but
it raises `rddata_valid` on phase 0 only, so a read injected through DFII updates phase 0's
register and leaves the other three, which makes read leveling impossible against it (every tap
fails). The firmware therefore levels only when the PHY's phase registers read back (on the
board they do; in the model they read 0), as LiteX's BIOS levels only a read-leveling-capable
PHY; `firmware/tests/ddrc_model.c` has the same quirk in its `no_phy` mode.

So the controller, crossbar, native port and DFII the bridge and the firmware talk to in
simulation are the bitstream's; the PHY, the pins and the read leveling against a real memory
are not.

### 10.3 Through the open flow (milestone 4 bitstreams)

`make build-fpga FLOW=openxc7 BOARD=<board> DESIGN=surfcpu_arty_a7_ddr_bramdiag` (and
`surfcpu_arty_a7_ddr`). **Since round ddr-3 both DDR designs pass every gate of the open flow on
both boards**: synthesis, bitstream, round trip, block RAM, LVS and clock-tile checks, the
board's netlist and boot ROM checks, routing complete and every clock met after routing. These
are the first DDR bitstreams. The builds' records are in docs/evidence/board/openxc7-ddr.txt,
the timing experiments in docs/evidence/ddr/openflow-timing.txt; BOARD.md section 8 has the
table, with every clock's figures and each bitstream's SHA-256.

**Timing.** Round ddr-2's builds missed nextpnr's estimate for LiteDRAM's 100 MHz clock
(63.5 to 81.0 MHz) on two kinds of path: through the calibration CPU's bus, and inside the
controller. With the CPU gone (approach (a) of this round) the controller's own path remains,
every time the worst: from the ZQ calibration timer (a 27-bit down-counter compared with 0)
through the refresher's state machine, the refresh request of the eight bank machines and the
command multiplexer's arbitration into a bank machine's state, 15 to 19 LUT levels, 2.4 to 3.4
ns of logic and 7.8 to 10.4 ns of nextpnr's routing. At 100 MHz, 20 placements (seeds, and
these changes) gave 74.3 to 90.2 MHz:

| tried at 100 MHz | dram_clk |
|---|---|
| no CPU, seeds 1 to 4 | 74.3, 87.4, 77.4, 78.8 MHz |
| LiteDRAM's settings (b): command buffers 8 or 4 deep, buffered; ZQ calibration at 128 Hz; no auto-precharge (cmd_latency is unused by the A7 PHY) | 79.8 to 87.5 MHz |
| nextpnr's options (c), on those cores: router rip-up, congestion-driven placement, both, a timing weight of 100, the annealing placer | 75.6 to 90.2 MHz (rip-up +1 to +3 MHz on some placements, worse on others); the annealing placer does not place this design |

None of them changes the path's 15 to 19 LUT levels, and the generated Verilog was not edited.
A slower clock (d): 87.5 MHz (PLL VCO 1400) missed on both seeds tried (86.6, 84.6 MHz); **80 MHz
(VCO 1600) closes in 9 of 12 placements** (74.3 to 94.0 MHz; the 100T 5 of 6, the 35T 4 of 6),
and that is the configuration now: DDR3L at 640 MT/s, above its DLL's floor (tCK at most 3.3 ns,
303 MHz: a controller clock of at least 76 MHz). Because nextpnr's result for one seed moves
with any change of the netlist, the build ID included, the DDR design lists give seeds 1 to 8,
tried in turn until a placement meets every clock (OPEN-FLOW.md section 3: the same gate for
each seed, every attempt in the manifest). Vivado routinely closes LiteDRAM at 100 MHz on the
Arty; with it the core could go back to 100 MHz (and the read leveling's window would shrink by
a fifth).

**The builds** (at 92a2b510, clean; `dram_clk` 81.72 to 93.35 MHz after routing, slack +0.263 to
+1.788 ns; sys_clk 56.5 to 74.4 MHz and pix_clk 135.0 to 153.7 MHz against their 25 and 25.17; each
on its first seed). LVS proves 10,878 points of each, with no difference; the clock-tile check now
compares all 85 fields of the three clock tiles, the 90-degree output's phase among them (below).
The designs use about 7,600 LUTs and 5,214 flip-flops (12 % and 4 % of the 100T, 37 % and 13 % of
the 35T), the SoC's 36 block RAMs (LiteDRAM has none now; round ddr-2's took 10.5 more) and 4
DSP48E1. Over this round's gated builds, the first seed met `dram_clk` in 8 of 10 builds (at
0964f875, 595ab382 and 92a2b510); the others needed seed 2 or 3, so over the whole round about
three placements in four meet it: the list's spread is what absorbs a netlist change such as the
build ID.

**The three checks that misread round ddr-2's designs** are fixed where they went wrong
(OPEN-FLOW.md 6.5, docs/evidence/flow/checkers.txt), each with a self-test on `make check` and
a regression through the real flow, and one gap is closed:

* LVS's `DIFFER ... WEBWE[3]` on the VexRiscv tag RAM was the LVS extractor's model, not
  nextpnr, the FASM or the netlist: the extractor read a RAMB18E1's byte write enables one to
  one from the site's first pins, where the silicon (Vivado's mapping, f4pga's fasm2bels) and
  nextpnr give byte i pins 2i and 2i+1 in the 36-bit simple dual port mode (and on an 18-bit
  port). `devenv/openxc7/bramwe.py` corrects the reading between extraction and proof and checks
  every pin it no longer reads; the tag RAM's shape now proves equal, and real differences
  still fail. The CPU and its RAM are gone from these designs; the fix stays.
* `cmtcheck.py` read Yosys's short bit strings (`5'd16` as "10000") as decimal numbers; LiteDRAM's
  PLL (16, 8, 20, 5) now checks as XAPP888, in these builds too.
* `synthcheck.py` counted register bits Yosys had already merged into a memory's read port as
  lost flip-flops; they are counted apart (VexRiscv lite alone: 22).
* `cmtcheck.py` reported any output with a phase as unchecked and compared no output's phase
  fields; it now computes them as XAPP888 does, so LiteDRAM's 320 MHz clock at 90 degrees (the
  DQS strobes: DELAY_TIME 1, PHASE_MUX 2) is checked in every build, and a wrong phase fails.

**On the board (round ddr-4):** of five placements of the DDR diagnostics, two failed although
they passed every gate (92a2b51 seed 1: a trap no correct core can raise at the first printed
character; 69ee491 seed 3: shifts through one ALU input wrong, so hexadecimal printed backwards,
the walking one at bit 20, FENCE.I's patch and lane 0's leveling compare failed), while seeds 1
and 2 at 69ee491 passed all 572 checks, DDR included. Both failing placements, and no working
one but seed 1 (whose one such route fed a flip-flop the tests exercised without fault), had
nextpnr route ordinary nets through unused global buffers onto the clock network: in the first
into the CPU's EX stage, in the second into that ALU input. The DDR PHY was not involved (its
199 cells sit at the same sites in every seed; lane 0's leveling compares one byte with a
right shift by 16, lane 1's by 8 and 24). The flow now uses a chip database without those
route-throughs and its clock check fails any trace of them; the two failing netlists rebuilt
that way keep their placement (OPEN-FLOW.md 6.8, docs/evidence/flow/bufg-routethrough.txt).

### 10.4 Simulation of the DDR top

`make board-ddr-sim BOARD=<board>` (`sim/board/ddr_top_sim.sh`, sim/board/tb_netlist.v from the
pins, Verilator; docs/evidence/ddr/ddr-top-sim.txt) runs the real board top with `ARTY_HAS_DDR`,
the real SoC, bridge, adapter and register bridge, the board's clocking and resets:

* with LiteDRAM's controller and DRAM model (`sim/board/litedram_simcore.v` around the simulation
  core of 10.2, on its own 80 MHz, its control port straight through): **the BRAM diagnostics
  initialize the controller through `surf_ddr_csr`** (the PHY reset, the mode registers and ZQ
  calibration through LiteDRAM's DFII; no leveling against the model, 10.2) **and pass all their
  DDR tests** (data and address lines, byte lanes, boundaries and aliases, 1 MiB of patterns, 4 x
  256 KiB copies at 720 KiB/s, code run from DDR: `BRAMDIAG: PASS (570 checks)` at 2.90 s);
  SurfBoot initializes it, calibrates, passes its quick DDR test and stops for want of a card
  (`ddr: initialized by SurfBoot, no read leveling (the PHY has no delay registers)`, `ddr:
  calibrated after 1 ms`, then `SurfBoot stopped: no card (NO_CARD)` at 59 ms of simulated time);
* with the generated board core itself (its PHY, its registers; `sim/board/prims` models of its
  primitives) and nothing on the DDR3L pins: the diagnostics' initialization reaches the real
  PHY's registers and strobes, scans every bitslip and delay of both lanes, finds no window
  (nothing answers), writes `init_error` and reports it (all 16 scan lines 0, `lane N: bitslip 0,
  no working delay`, then `BRAMDIAG: FAIL (3 of 206 checks failed)` at 0.42 s of simulated time).
  This is the failure path the board would print with no working memory.
* `make board-netlist-sim DESIGN=<a DDR design>` runs a DDR build's synthesized netlist the same
  way (the 100T diagnostics build's netlist at 92a2b510, the one LVS proves its bitstream equal
  to, with nothing on its memory pins, printed the same 2,166 characters at the same microseconds
  as the RTL run with the board core and the ROM of the same commit: the whole failed leveling
  included; 26 minutes in Verilator).
* `make board-uart-expect-ddr` boots SurfOS through SurfBoot with the C++ DDR model in place of
  the bridge, adapter and LiteDRAM's data path and LiteDRAM's register model
  (`firmware/tests/ddrc_model.c`, surfsim `--ddr-swinit`) for its registers: the soc track's
  surfsim with the SoC as the DDR designs configure it (`DDR_SWINIT` 1). SurfBoot levels the
  model's lanes in 0.24 s and SurfOS reaches its shell prompt at 4.2 s of simulated time.

Real calibration can happen only on the board: no DDR3 model is used anywhere here.

### 10.5 What the LiteDRAM path proves, and what only the board can show

Shown without a board:

* the bridge and adapter against LiteDRAM's own controller, crossbar and native port with the
  board's settings at 80 MHz (test-ddr-litedram), and in the whole SoC with the real board top
  (board-ddr-sim: 570 diagnostic checks over the DDR window);
* the register bridge against a Wishbone slave model at five clock pairs with resets, a stopped
  clock, slave errors and silence (test-ddr-csr; nine of nine mutants caught), and in the whole
  top against LiteDRAM's real register bank (board-ddr-sim, both cores);
* the firmware's initialization against a model of LiteDRAM's registers that fails a run on any
  protocol violation (mode registers, ZQ calibration, phases, open row) and gives each lane a
  read window, in the host tests and the emulator, SurfBoot and the diagnostics both; against
  LiteDRAM's DFII and DRAM model in RTL; against the real PHY's registers (no memory) in RTL;
* the board top's DDR wiring, clocking, resets and calibration-status path, from the pins;
* that the generated core is the pinned generator's output (hashes), that its registers and PHY
  settings are what the firmware was written for, and that the open flow builds the DDR designs
  with every check passing (10.3);
* the pins against Digilent's MIG projects, and that the pin options reach the bitstream.

Only the board can show (on the owner's A7-100T the first six have been shown working,
2026-10-16/17, as far as passing tests show them: windows of +-8 taps on both lanes at bitslip
2, the initialization in 229 ms, the DDR tests and `BRAMDIAG: PASS (572 checks)`, then SurfOS
loaded and run from DDR; no margins were measured, and the 35T has not been tried):

* that the DDR3L initializes and that read leveling finds a window on both byte lanes (the
  BRAM diagnostics print every lane's scan: a run of 1s in one bitslip line per lane; at 320 MHz
  a bit lasts 1.56 ns, about 20 of the 78 ps taps, so a window of about +-6 to +-9 taps is to be
  expected), and that the memory tests pass after it;
* how long the initialization takes on SurfCPU (about 0.2 s expected) against the 1.5 s limit;
* the write path: LiteDRAM's A7 PHY has no write leveling; its DQS strobes come from the PLL's
  320 MHz output at 90 degrees, whose encoding every build now checks against XAPP888, but whether
  they are centred on the data at the DRAM only the DDR tests' writes and read-backs on the board
  show;
* the I/O timing of the DDR3L pins (false paths for both flows; nextpnr has no path on the
  320 MHz clocks to time) and the electrical choices: 40 ohm split termination (Digilent's MIG
  uses 50), fast slew, the internal VREF, LiteDRAM's drive and ODT settings (34 ohm, 60 ohm), now
  at 640 MT/s, and the DLL at a DDR clock of 320 MHz (its floor is 303 MHz);
* timing: nextpnr's estimate passes at 80 MHz; Vivado's numbers and the hardware decide;
* milestone 4's tests on the real memory (the BRAM diagnostics' DDR sections, then SurfBoot
  loading SurfOS);
* the 35T placed on the whole xc7a50t die (OPEN-FLOW.md 6.3).

## 11. Pending, and what to check first on hardware

* **MIG generation, synthesis, implementation and timing closure**: blocked on a Vivado host.
* **Physical initialization and calibration** (LiteDRAM): done on the owner's A7-100T with
  `surfcpu_arty_a7_ddr_bramdiag` built by the fixed flow (BOARD.md section 12, steps 4 and 5):
  both lanes at bitslip 2, delay 16 (window +-8), `DDR calibration ok`, the DDR tests and
  `BRAMDIAG: PASS (572 checks)` ([evidence](evidence/physical/ddr-fixed-flow-2026-10-17.txt)).
* **Milestone 4's physical tests**: done on the board by the BRAM diagnostics' DDR sections
  (address and data lines, every byte lane, boundaries and aliases, sustained copies, code copied
  from BRAM into DDR and executed); the fw track's `DDRDIAG.BIN` from the card waits for
  milestone 5's test (PHYSICAL-TESTS.md m5).
* The DDR part and board revision (owner): Digilent's files are for revision E.0 and the
  MT41K128M16 family.
* The open flow at 100 MHz: LiteDRAM's controller does not close nextpnr's estimate there
  (10.3); the designs run the memory at 640 MT/s instead of 800 until a flow that closes 100 MHz
  (Vivado) is used.
* Not covered by any test here: metastability itself, and the real MIG's behaviour beyond the
  model (its latencies, reordering, refresh). The model follows UG586's rules; the MIG may differ
  in timing, which only changes latency.

## 12. Notes for the other tracks

* **soc**: `sim/models/ddr_model` should behave like this bridge: `s_req_ready` low while an
  access is outstanding; refused accesses answered with an error in the next cycle (before
  calibration, after a failure or a timeout); normal latency 5 cycles and up; the watchdog's
  error exactly `TIMEOUT_CYCLES` after acceptance, then `timeout` sticky and every access refused
  until reset; `calib_done` and `calib_fail` exclusive. `DDR_STATUS.PRESENT` comes from
  `HAS_DDR`, not from the bridge. surfsim's `--ddr-swinit` puts LiteDRAM's register model on the
  SoC's `ddrc` port (built with `DDR_SWINIT=1`).
* **board**: for the MIG (Vivado flow, not wired yet): own the oscillator's IBUF and BUFG and give
  the MIG the 100 MHz from the BUFG; make the 200 MHz reference; drive `sys_rst` low until the PLL
  is locked; read the MIG IP and the bridge's scoped XDC; no DDR3 pin constraints of your own;
  `DDR_SWINIT` 0 (the MIG calibrates by itself). `ip/mig/mig_ddr3_stub.v` lets the top lint
  without Vivado. The LiteDRAM top is done (BOARD.md section 7).
* **fw**: done in this round (SurfBoot 1.1.0): with `DDR_STATUS.SWINIT` initialize the
  controller (`firmware/common/ddrinit.c`), then wait for `CALIB_DONE` or `CALIB_FAIL` (never
  both); `TIMEOUT` means the DDR is out of service until reset (accesses fault at once). The
  kernel must leave the `ddrc` registers alone (BOOT-ABI.md section 6).
* **flow**: the seed lists, the three checker fixes and the clock check's phases of this round
  are in the flow's files (OPEN-FLOW.md 3 and 6.5); open is fasm2netlist's own RAMB18E1 port
  table (bramwe.py does its work until then) and nextpnr's wrong pairing of an 18-bit port B's
  byte enables in true dual port mode, which LVS refuses (OPEN-FLOW.md 6.5; no SurfCPU design has
  one).
* **release**: the DDR designs now build, and a fresh checkout's `make litedram-gen` matches its
  lock (10.1); `release/release.txt`'s and RELEASE.md's notes that the DDR designs do not build
  and that `run_gen.py` should drop the tree comment are the release track's to update.

## 13. Proposed contract changes

1. MODULES.md section 3, `surf_ddr_bridge`: add the parameters `CALIB_TIMEOUT_CYCLES`
   (37,500,000), `APP_ADDR_WIDTH` (28), `APP_DATA_WIDTH` (128), `DQ_WIDTH` (16), and note that the
   controller side is the MIG native UI of section 1 above.
2. SURFBUS.md section 3: "...a write with an error changed nothing, **except a DDR access
   abandoned by the bridge's watchdog after it was handed to the memory controller, which may
   still complete (DDR_STATUS.TIMEOUT; the DDR then refuses every access until reset), and a DDR
   access cut short by a reset of the memory controller, which may have completed**."
3. SURFBUS.md rule 8: "...no request survives a reset, **except that a DDR access already
   passed to the memory controller's clock domain completes after the SoC's reset; its response
   is discarded and it is ordered before every later DDR access**."
4. spec/soc.toml, `DDR_STATUS.CALIB_FAIL`: state that it is sticky until reset and that
   `CALIB_DONE` then stays 0 even if calibration completes late; give the time limit (now a bridge
   parameter, 1.5 s at 25 MHz) a place in the spec once measured, next to FIRMWARE.md's 2 s
   wait, which must stay longer.
