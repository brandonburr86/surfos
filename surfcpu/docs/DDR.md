# DDR3L: the bridge, the controller, and how they fit

The Arty A7 has 256 MB of DDR3L behind a 16-bit bus (Micron MT41K128M16JT-125 on the documented
board revisions; **the owner still has to confirm the part**, INVENTORY.md G1). SurfCPU reaches
it through the 256 MiB window at `0x8000_0000` (MEMORY-MAP.md). Three pieces sit in between:

```
 surfcpu_soc            surf_ddr_bridge                         memory controller
 ddr_* master  ---->  SurfBus slave | mailboxes |  MIG UI  ---->  MIG 7 Series (Vivado flow)
 port (sys_clk)       (sys_clk)     | (CDC)     | (ui_clk)   or   surf_ddr_litedram_adapter ->
                                                                LiteDRAM (open-source flow)
```

* `surf_ddr_bridge` (`rtl/soc/ddr/`): the SurfBus slave of MODULES.md, crossing from sys_clk
  (25 MHz) to the controller's clock, turning 32-bit accesses with byte enables into 128-bit MIG
  beats with byte masks, reporting calibration, and answering a stuck access with an error.
* The controller: AMD's MIG 7 Series for the Vivado flow (`ip/mig/`), or LiteDRAM for the open
  flow (`ip/litedram/`, with `surf_ddr_litedram_adapter` in front of its native port).
* The board top instantiates the controller and the bridge and owns the clocks: with LiteDRAM,
  `surfcpu_arty_a7_top` built with `ARTY_HAS_DDR` (BOARD.md section 7; the DDR designs
  `surfcpu_arty_a7_ddr_bramdiag` and `surfcpu_arty_a7_ddr`).

## Status (2026-10-16)

| Part | Simulation | Synthesis, place and route | On the board |
|---|---|---|---|
| `surf_ddr_bridge`, `surf_ddr_mailbox` | all tests pass (section 8): MIG model, LiteDRAM's controller, Icarus, 7 clock pairs, mutation check; formal proofs of the mailbox and of the bridge's SurfBus obligations | Yosys `synth_xilinx`: 247 LUTs, 387 flip-flops, no vendor primitives (an open-tool estimate); in the DDR designs, placed and routed by the open flow | **pending** |
| MIG recipe (`ip/mig/`) | the bridge is tested against a behavioural model of the MIG's user interface, not the MIG | **pending**: needs Vivado (no build host, BUILD-HOST.md); recipe dry-run only | **pending** |
| LiteDRAM core (`ip/litedram/`) | its real controller, generated from the board's configuration, passes the bridge tests through the adapter and runs the BRAM diagnostics' DDR tests in the whole board top; the exact generated core, BIOS included, runs its calibration and fails it cleanly with no memory on its pins, in RTL and, identically, as the synthesized netlist (sections 10.2, 10.4) | generated reproducibly, its hashes recorded; in the DDR designs (BOARD.md section 7) the open flow places and routes it, PHY in bank 34 included, but **misses nextpnr's timing for its 100 MHz clock: no bitstream**; routing converges only for some placements, and LVS finds one block RAM write enable of the calibration CPU different (section 10.3) | **pending** |
| `surf_ddr_litedram_adapter` | tested against LiteDRAM's controller; 5 of 5 mutants caught | in the DDR designs | **pending** |

Nothing here has run on an FPGA. Milestone 4's physical evidence (calibration, address and data
patterns, byte lanes, boundaries and aliases, sustained transfers, code running from DDR) is all
pending (sections 10.5 and 11).

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
| `ui_clk` | 1 | in | the controller's clock (MIG: 81.25 MHz; LiteDRAM: 100 MHz) |
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
| `CALIB_TIMEOUT_CYCLES` | 37,500,000 | sys_clk cycles after `rst` for calibration to succeed (1.5 s at 25 MHz; 0 = no limit): under SurfBoot's and the BRAM diagnostics' own 2 s wait, so they see `calib_fail` first; LiteDRAM's BIOS takes about 1 s in simulation (section 10.1). The real time must be measured on the board (the MIG's too) |
| `APP_ADDR_WIDTH` | 28 | MIG `app_addr` width |
| `APP_DATA_WIDTH` | 128 | MIG app data width; must be 8 x `DQ_WIDTH` (one BL8 burst per beat, 4:1), else elaboration stops |
| `DQ_WIDTH` | 16 | memory data width |

## 2. What the SoC sees

* **One access at a time** (SURFBUS.md rule 6, and the SoC's DDR arbiter): `s_req_ready` is
  `!rst && !busy`; the next request can be offered in the cycle of the previous response.
* **Accepted while usable** (`calib_done` and no timeout): the access goes to the controller and
  is answered when the controller is done. Latency (sys cycles from the accepting edge to the
  response cycle, MIG model at 81.25 MHz with 12-32 cycles of read latency): 5 to 20, average 9;
  back to back, 6.8 cycles per write and 13.4 per read. Against LiteDRAM's controller at 100 MHz:
  6.0 per write and 9.3 per read. These are simulation figures with modelled controller latency.
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

sys_clk (the board's PLL) and ui_clk (the MIG's MMCM, or LiteDRAM's PLL: 100 MHz) both come from
the 100 MHz oscillator, so Vivado sees them as related clocks and would time the bridge's
crossings as synchronous paths against the closest edges of 25 MHz and 81.25 (or 100) MHz, a
meaningless requirement.
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

| Command | What runs | Result (2026-10-15) |
|---|---|---|
| `make lint-ddr`, `lint-ddr-adapter` | Verilator 5.032 `--lint-only -Wall` (default and four other parameter sets for the bridge), Icarus 12 `-g2005 -Wall` | clean |
| `make test-ddr-bridge` | `tb_ddr_bridge --test all`, seeds 1, 2, 3, at 25 MHz / 81.25 MHz | 3 x 13 tests pass |
| `make test-ddr-ratios` | random, sysreset, uireset and lanes at 7 more clock pairs (below) | 28 runs pass |
| `make test-ddr-defaults` | the bridge with its default parameters, as the board top instantiates it: calibration limit of 37,500,000 cycles (ui clock stopped), the 4096-cycle watchdog (18 cases), 5000 random accesses | pass; `calib_fail` after 37,500,001 cycles |
| `make test-ddr-icarus` | the mailbox in Icarus (5 clock pairs x 5000 messages, clock stops, misuse) and a four-state smoke test of the bridge with a Verilog MIG stand-in (3 seeds x 1094 accesses) | pass |
| `make test-ddr-litedram` | `tb_ddr_litedram --test all`, seeds 1, 2, plus a jittered run: the bridge and adapter against LiteDRAM's controller generated from the board's `arty.yml` (section 10.2) | 2 x 9 tests + 1 pass |
| `make board-ddr-sim` (board track's target) | the DDR top in RTL from its pins: LiteDRAM's controller and model, and the generated core with its BIOS and no memory (section 10.4) | pass: 569 diagnostic checks; CALIB_FAIL at 1.500 s in both failure runs |
| `make synth-ddr` | Yosys 0.52 `synth_xilinx` of the bridge | 247 LUTs, 387 flip-flops, 11 CARRY4 (estimate) |
| `make check-mig` | the MIG recipe without Vivado (section 9.4) | pass |
| `make prove-ddr` | SymbiYosys 0.52: the mailbox and bridge proofs (ABC PDR, no depth bound) with covers | proved, covers reached (about 20 s) |
| `make test-ddr-mutants` | 32 deliberately broken copies of the RTL (slow, not on check) | 31 caught, 1 survives as expected |

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
(LiteDRAM), 40 : 40 : 0, 40 : 37.3 : 0, 40 : 97 : 0 (ui slower than sys), 10 : 12.3077 : 0 (sys
faster than ui), 40 : 12.3077 : 0.3 and 13.1 : 41.7 : 0.3 (every half period jittered by up to
30%).

`tb_ddr_litedram.cpp` runs basic, lanes, bounds, alias, random (10,000), stream, precal (before
`init_done`), calerror (the BIOS's failure report: `init_error` then `init_done` over the core's
control port; every access refused at once, `calib_fail` at the limit, `calib_done` never) and
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
polarity, address shift, command, no write buffer, calibration). 31 of 32 are caught. The one that
survives removes the first synchronizer stage of the request toggle: metastability is not
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
top with `ARTY_HAS_DDR`.

### 10.1 Generating it

* `make litedram-setup` (`ip/litedram/setup.sh`) builds a pinned, vendored generator in
  `build/third_party/litedram/`: Migen `e19524c9`, LiteX `b6ae9e0b` and LiteDRAM `c454a440`
  (both tag 2026.08), pythondata-cpu-vexriscv `642ecfed`, -software-picolibc `6a13ccce`,
  -software-compiler_rt `6eb76609`, -misc-tapcfg `a12c3f59`, and the PyPI wheels PyYAML 6.0.2,
  packaging 25.0, colorama 0.4.6, meson 1.9.1 and ninja 1.13.0 with checked SHA-256, unpacked
  without pip. It needs the network once.
* `make litedram-gen` generates `ip/litedram/arty.yml` (LiteDRAM's Arty example with one native
  port and the lite calibration CPU, below) into `build/litedram/arty/` and the simulation core
  into `build/litedram/arty_sim/` (10.2), writes `build/litedram/summary.txt`
  (`ip/litedram/inspect_core.py`, `estimate.sh`) and checks the files against
  `ip/litedram/arty.sha256` (`ip/litedram/lock.sh check`; also `make litedram-check`, on
  `check-openflow`).
* **A build product, reproducible and checked.** The generated Verilog is never committed.
  `ip/litedram/generate.sh` makes it byte for byte the same on every run: SOURCE_DATE_EPOCH is
  the pinned LiteDRAM commit's time (1790239353), which GCC uses for the BIOS's `__DATE__` and
  `__TIME__`, Python's clock is frozen at it for LiteX's "Date" lines (`ip/litedram/run_gen.py`),
  and TZ is UTC. Two runs, and a run into another directory, gave identical files.
  `ip/litedram/arty.sha256` records the six files the designs and the tests read (the core, its
  BIOS ROM and SRAM images and constraints, `VexRiscv_Lite.v`, the simulation core); a
  deliberate change (generator, `arty.yml`, the toolchain image whose RISC-V GCC compiles the
  BIOS) is recorded with `ip/litedram/lock.sh write`. The design lists name the list on a
  `generated` line, and `tools/fpga-build.sh` counts a build clean only if every generated file
  it reads has its recorded hash (`generated_files_match` in the manifest), and refuses to start
  without them (`make litedram-gen`).

What came out, for the Arty (2026-10-16, in the container):

* `litedram_core.v` with the DDR3 pins, `clk` (100 MHz in) and `rst`, `pll_locked`,
  `init_done`/`init_error`, a serial console `uart_tx`/`uart_rx`, and the native user port:
  `cmd_addr[23:0]` (128-bit beats), `cmd_we`, `wdata_data[127:0]`, `wdata_we[15:0]` (byte enables,
  1 = write), `rdata_data[127:0]`, valid/ready on each, and `user_clk`/`user_rst` (100 MHz).
* Clocking inside: one `PLLE2_ADV`, its `clk` straight into CLKIN1 with no buffer of its own
  (the generator has no CRG option; the board top gives it the oscillator through a BUFG, BOARD.md
  section 7), VCO 1600 MHz, giving 200 MHz (IDELAYCTRL reference), 100 MHz (core, user port),
  400 MHz and 400 MHz at 90 degrees (DDR3 at 800 MT/s, CL 6, CWL 5).
* Vendor primitives: ISERDESE2 x16, OSERDESE2 x45, IDELAYE2 x16, IDELAYCTRL x1, IOBUF x16,
  IOBUFDS x2, OBUFDS x1, BUFG x4, PLLE2_ADV x1, FDCE x8, FDPE x8; the simulation core uses none.
* **Calibration**: the core has its own small CPU running the LiteX BIOS's `sdram_init` from an
  internal ROM (27.3 KiB of 48 KiB): reset and mode registers, read leveling (bitslip and delay
  scan per byte), then a 2 MiB memory test and, if it passed, a speed test; then it writes
  `init_done`, or `init_error` and `init_done`. It needs `VexRiscv_Lite.v` from
  pythondata-cpu-vexriscv next to the core. This is a second CPU inside the memory controller, a
  calibration sequencer, not SurfCPU's replacement; the owner should know it is there (brief
  section 2). Its console (`uart_tx`) is the board's `dram_uart_tx` on ChipKit IO1; its input is
  held idle.
* **Which CPU, and why it matters.** The BIOS's run time is set by the CPU, and it must end
  before SurfBoot and the BRAM diagnostics stop waiting for `CALIB_DONE` (2 s, FIRMWARE.md
  section on DDR) and before the bridge's limit (1.5 s). Simulated with the board core and no
  memory (`make board-ddr-sim`, 10.4; the BIOS's code and the controller's cycle timing are
  exact, only the memory's answers are missing), its milestones were:

  | VexRiscv variant | leveling done | memory test done (the failed one here) | estimated `init_done` on a working memory (+ speed test) |
  |---|---|---|---|
  | minimal (RV32I, no caches) | 0.383 s | about 1.75 s (writes 0.39 to 1.07 s, 1.3 of the 2 MiB read back by 1.51 s) | about 2 s: **too late** (past the bridge's 1.5 s, at SurfBoot's 2 s) |
  | lite (RV32IM, instruction cache) | 0.189 s | 0.823 s | about 0.95 s |
  | standard (both caches) | 0.164 s | 0.617 s | about 0.75 s |

  The speed test that follows a passing memory test reads and writes the same 2 MiB with
  unrolled loops, a fraction of the memory test's time (an estimate from its code, not
  simulated: no DDR3 model here). **The core uses lite**: about 1 s, well inside both limits. The
  minimal CPU was tried first (it is smaller, and the open flow's synthesis check passes on it)
  and would make every DDR design fail its calibration wait on the board (or need the waits
  raised: the bridge's limit and FIRMWARE.md's 2 s). Lite and standard
  both trip the open flow's synthesis check on a false finding (10.3).
* Constraints shipped: two false paths and one max delay for LiteX's reset synchronizers
  (`litedram_core.xdc`, read by the DDR designs for Vivado), but no pins, no I/O standards and no
  clocks: the board's `arty_a7_ddr3_pins.xdc` and `arty_a7_ddr3_timing.xdc` supply them (BOARD.md
  section 7, WIRING.md section 2.5).
* Yosys `synth_xilinx` estimate of the core with its CPU and the user port as top-level pins
  (`estimate.sh`): 5,492 LUTs, 3,361 flip-flops, 56 RAM32M, 17 RAMB18E1, 2 RAMB36E1, no DSP48E1
  (lite's multiplier is iterative). In the whole DDR design the open flow counts 38 RAMB36E1 and
  17 RAMB18E1 (10.3).

`surf_ddr_litedram_adapter` (`rtl/soc/ddr/`) puts LiteDRAM's native port behind the bridge's
MIG-style port, on `user_clk`/`user_rst`: commands straight through (`cmd_addr = app_addr[26:3]`,
`cmd_we` for a write), a two-beat write FIFO (`rtl/lib/surf_fifo`) because the bridge gives write
data before the command and LiteDRAM only takes it after (the mutation check confirms that
without it the bridge deadlocks), `wdata_we = ~app_wdf_mask`, read data always taken
(`rdata_ready = 1`), `init_calib_complete = init_done && !init_error`, `ui_rst = user_rst`.

### 10.2 The simulation core and what it shares with the board's

`make test-ddr-litedram` (and `board-ddr-sim`'s model runs) use `litedram_sim`, generated from
**the board's own `arty.yml`** with `--sim`: `ip/litedram/sim_config.py` changes exactly two keys
(`cpu` becomes None, `uart` goes) and `build/litedram/arty_sim/config.yml` keeps the result.
LiteDRAM's DRAM model (SDRAMPHYModel) then replaces the Artix-7 PHY, the PLL and the chip.
LiteX 2026.08 cannot build the calibration BIOS for that model (its liblitedram calls the A7
PHY's delay registers, which the model lacks), so the simulation core has no CPU and the
testbench writes `init_done` (and `init_error`) over the core's Wishbone control port, the BIOS's
last step on the board. `inspect_core.py` compares the two generated cores on every
`make litedram-gen`:

* the same: the DRAM module (MT41K128M16) and its timings, geometry, the controller settings
  (command buffers 16 deep, bank machines, refresh), the native port and its blocking until
  `init_done` without `init_error`, CL 6 and CWL 5, the 4:1 phases and write phase, and the
  DRAM initialization sequence itself (its four mode-register loads and waits, compared line by
  line);
* different: the PHY (A7DDRPHY with its ISERDESE2/OSERDESE2/IDELAYE2, 32 delay taps and 8
  bitslips, read leveling; the model has none and reads back what was written), the read phase
  (3 on the board, 2 in the model), the clock (a PLL on the board), and the calibration CPU with
  its port on the controller's crossbar (idle once calibration is done).

So the controller, crossbar and native port the bridge talks to in simulation are the
bitstream's; the PHY, the pins and the calibration are not, and are simulated only as far as
10.4 says.

### 10.3 Through the open flow (milestone 4 bitstreams)

`make build-fpga FLOW=openxc7 BOARD=<board> DESIGN=surfcpu_arty_a7_ddr_bramdiag` (and
`surfcpu_arty_a7_ddr`). Results in docs/evidence/board/openxc7-ddr.txt (2026-10-16: the gated
builds at source a338e7a3, clean; the diagnostic builds at 383dba74, the same design but for its
build ID):

* **Placement and routing of the PHY in bank 34 work**: nextpnr places and routes all 16
  ISERDESE2, 45 OSERDESE2, 16 IDELAYE2, the IDELAYCTRL, the IOBUF/IOBUFDS/OBUFDS and the third
  clock tile on both parts (OPEN-FLOW.md 6.7 had left this open), with the pins' SSTL135
  standards, fast slew and UNTUNED_SPLIT_40 termination in the FASM (a probe build showed
  nextpnr applies SLEW and IN_TERM; docs/evidence/ddr/openflow-pins.txt) and bank 34's VREF at
  675 mV.
* **Timing fails, and with it every build.** nextpnr's estimate after routing for LiteDRAM's
  100 MHz clock (`dram_clk`: the controller, the calibration CPU and its bus, the adapter and the
  bridge's controller side) is 63.5 to 81.0 MHz over the builds that routed: at a338e7a3 77.3 MHz
  for ddr_bramdiag on the 35T and 63.5 and 73.8 MHz for ddr on the 100T and the 35T; at 383dba74,
  where all four routed, 73.0, 81.0, 77.0 and 78.4 MHz; the diagnostic builds 73.7 and 79.0 MHz.
  That is 2.3 to 5.8 ns short of the 10 ns, and it moves by several MHz with any change to the
  netlist, even the build ID. The late paths, a few hundred endpoints each time, are of two kinds:
  through the calibration CPU's bus (VexRiscv lite's instruction or data bus address through
  LiteX's bus decoder back into its fetch stage, 12.7 to 15.8 ns, 2.5 to 2.8 ns of it logic and
  the rest routing) and inside LiteDRAM's controller (from the ZQ-calibration timer through the
  refresher and the command multiplexer to a bank machine: 12.1 to 13.6 ns, 13 to 17 LUT levels,
  about 3 ns of logic). Everything else meets its clock with a wide margin (sys_clk 52.7 to 75.7
  MHz for its 25, pix_clk 116 to 162 for 25.17, LiteDRAM's 200 MHz 341 to 394). The gate is
  kept: `tools/fpga-build.sh` writes no bitstream, so there is no DDR bitstream from the open
  flow. What was tried, with the minimal CPU unless said (docs/evidence/ddr/openflow-timing.txt):
  other placer seeds (80.3 to 80.6 MHz), the 35T (77.6), LiteDRAM's options that touch the
  controller (command buffers 4 deep and buffered: 82.7; no auto-precharge: 80.9), and a slower
  controller clock: at 80 MHz (DDR3 at 640 MT/s) nextpnr reaches only 75.4 MHz, and the DDR3L
  cannot go much slower (its DLL needs at least 303 MHz, a controller clock of 76 MHz). Vivado
  routinely closes this controller at 100 MHz on the Arty; nextpnr's delays are an estimate
  (OPEN-FLOW.md 6.4), but the flow's rule is that a missed estimate fails the build.
* **Routing does not always converge either.** With the lite CPU the 35T did not route at the
  placer's seed 1 (router2 stayed at 300 to 500 overused wires past 300 iterations), so the
  design lists set seed 2, with which all four builds routed at 383dba74. At a338e7a3, whose
  netlist differs only by the build ID, the 100T ddr_bramdiag build did not route at seed 2
  (router2 between 750 and 980 overused wires through 100 iterations, where it was stopped; the
  other three routed in 18 to 45 iterations); its record shows the placer's estimates instead
  (dram_clk 84.1 MHz before routing), and its manifest, from that commit's
  `tools/fpga-build.sh`, gives them as the reason (the script now records that routing did not
  finish). So whether a DDR build routes is, for now, a matter of the
  placement's luck, and no seed fixes it for the next commit; a congestion-aware placement
  (nextpnr's `--placer-heap-congestion-spread`) is a flow setting, the flow track's to try.
* **Diagnostic builds** (not the designs' results): the same flow with nextpnr allowed to finish
  (a copy of `tools/openxc7-build.sh` with `--timing-allow-fail`, made and recorded by
  `sim/board/openxc7_ddr_evidence.sh`), so that the checks after it run, on
  surfcpu_arty_a7_ddr_bramdiag for both boards (at 383dba74; 35 and 38 minutes, 30 of them LVS):
  the bitstream checks pass (IDCODE and part, frames, commands), the round trip through the FASM
  passes, all 55 block RAMs (38 RAMB36E1 and 17 RAMB18E1, LiteDRAM's BIOS ROM among them) hold
  their initial contents in the bitstream, and the board's netlist and boot ROM checks pass. **LVS
  fails on one pin** on both boards, 14,148 points proved equal and 1 different: write enable
  WEBWE[3] of the RAMB18E1 that holds the calibration CPU's instruction-cache tags
  (`u_dram.VexRiscv.IBusCachedPlugin_cache.ways_0_tags`, 512 x 36 in simple dual port mode, 23
  of its 36 bits used), which the netlist ties to 0 and the routed bitstream drives from the
  write enable of the other three byte lanes (on the 100T through BYP_L3 of INT_L_X38Y147, fed by
  the same NW2END3 wire as WEBWE[2:0]). It is the only block RAM in these designs with a
  constant byte enable beside used ones. The extra writes put zeros (its data inputs for that
  lane are 0) into a byte lane nothing reads, so the design should behave the same, but the
  bitstream is not the netlist there and LVS is right to fail it; whether nextpnr's handling of
  the pin or the LVS tile model is at fault is the flow track's to find. The diagnostic
  bitstreams are refused by `tools/program.sh` (the build's manifest says fail, and the flow's
  own manifest lists the failing checks).
* Checks that are the flow's to fix, found here: `cmtcheck.py` reads a parameter that Yosys's JSON
  writes as a short bit string (LiteDRAM's `5'd16` becomes `"10000"`) as a decimal number, so it
  reports LiteDRAM's PLL dividers as mismatches although the FASM holds exactly the XAPP888
  values (read as binary, every divider matches: docs/evidence/ddr/openflow-cmtcheck.txt); and
  `synthcheck.py` reports the lite CPU's 22 instruction-cache tag-read register bits as lost:
  synthesis puts them in the tag memory's RAMB18E1, whose read port is synchronous, and the check
  credits a block RAM's output register only when its optional DOA_REG/DOB_REG is on. Neither
  is waived here; both are reported for the flow track.

### 10.4 Simulation of the DDR top

`make board-ddr-sim BOARD=<board>` (`sim/board/ddr_top_sim.sh`, sim/board/tb_netlist.v from the
pins, Verilator; docs/evidence/ddr/ddr-top-sim.txt) runs the real board top with `ARTY_HAS_DDR`,
the real SoC, bridge and adapter, the board's clocking and resets:

* with LiteDRAM's controller and DRAM model (`sim/board/litedram_simcore.v` around the
  simulation core of 10.2, `init_done` 2 ms after start): **the BRAM diagnostics pass all their
  DDR tests** (data and address lines, byte lanes, boundaries and aliases, 1 MiB of patterns,
  4 x 256 KiB copies at 738 KiB/s, code run from DDR: `BRAMDIAG: PASS (569 checks)` at 2.81 s);
  with the BIOS's failure report instead (`+dram_calib_fail`), the DDR window stays closed and
  the bridge sets CALIB_FAIL at its limit, 1.500 s, which the diagnostics report (`DDR_STATUS
  0x6`, `BRAMDIAG: FAIL (1 of 204 checks failed)`); SurfBoot calibrates, passes its quick DDR
  test and stops for want of a card;
* with the generated board core itself (its PHY, its CPU and its BIOS from the ROM image the
  bitstream holds; `sim/board/prims` models of its primitives) and nothing on the DDR3L pins:
  the BIOS prints its banner, levels every bitslip of both byte lanes and finds no window, its
  memory test fails (`Memtest KO`, 0.823 s), it writes `init_error`; the bridge never sees
  calibration and sets CALIB_FAIL at 1.500 s; the diagnostics report it. This is the only
  simulation of the exact generated core and of the calibration path, and it shows the failure
  path end to end: what the board prints with no working memory.
* `make board-netlist-sim DESIGN=<a DDR design>` runs a DDR build's synthesized netlist the same
  way (the second bullet's case): the core's CPU in gates runs its BIOS from the synthesized ROM,
  with `sim/board/prims/RAMB18E1.v` modelling the simple dual port block RAMs of its register
  file and instruction cache. For surfcpu_arty_a7_ddr_bramdiag on the 100T (the netlist of the
  gated build at a338e7a3, 17,680 cells, its boot ROM inside) **it printed the same 78 lines on
  the two consoles, each at the same microsecond, as the RTL run of the second bullet**; the
  two consoles' characters are identical but for the build ID: the diagnostics' sections, the
  BIOS's banner, every leveling line, `Memtest KO` at 0.823 s and `BRAMDIAG: FAIL` at 1.514 s
  after the bridge's CALIB_FAIL, and the same LEDs and VGA timing. So the synthesized DDR top,
  LiteDRAM's controller, PHY logic and calibration CPU with its block RAM caches and register
  file included (synthcheck's finding, 10.3), behaves as its RTL on this path. It is slow, 0.17
  ms of simulated time a second, 146 minutes for the 1.5 s (`sh sim/board/ddr_sim_evidence.sh
  --netlist-only` runs it alone; the record is in docs/evidence/ddr/ddr-top-sim.txt).
* `make board-uart-expect-ddr` boots SurfOS through SurfBoot with the C++ DDR model in place of
  the bridge, adapter and LiteDRAM (the soc track's surfsim with the SoC as the DDR designs
  configure it): the only model fast enough for a kernel boot (5.4 s of simulated time to the
  shell prompt).

Real calibration can happen only on the board: no DDR3 model is used anywhere here.

### 10.5 What the LiteDRAM path proves, and what only the board can show

Shown without a board:

* the bridge and adapter against LiteDRAM's own controller, crossbar and native port with the
  board's settings (test-ddr-litedram, 2 seeds x 9 tests and a jittered run, including the
  calibration-error report), and in the whole SoC with the real board top (board-ddr-sim:
  569 diagnostic checks over the DDR window);
* the board top's DDR wiring, clocking, resets and calibration-status path, from the pins, in
  RTL with the model and with the exact generated core (failure path), and the synthesized
  netlist of the DDR bramdiag design doing exactly what that RTL does (10.4);
* that the generated core is the pinned generator's output (hashes) and that the open flow
  places and routes its PHY in bank 34 and assembles a bitstream from it (diagnostic: round
  trip, block RAM contents including the BIOS ROM, the clock tiles' dividers, and LVS but for
  one write enable of the calibration CPU's tag memory, 10.3);
* the pins against Digilent's MIG projects, and that the pin options reach the bitstream.

Only the board can show:

* that the DDR3L calibrates: read leveling finds a window on both byte lanes (the controller's
  console on IO1 prints the scan), and the BIOS's memory test passes;
* how long calibration takes (about 1 s expected from simulation) against the 1.5 s limit;
* the electrical choices: 40 ohm split termination (Digilent's MIG uses 50), fast slew, the
  internal VREF, LiteDRAM's drive and ODT settings (34 ohm, 60 ohm);
* timing: nextpnr says LiteDRAM's clock misses 100 MHz by 19 to 37 %; Vivado's numbers and the
  hardware decide;
* milestone 4's tests on the real memory (the BRAM diagnostics' DDR sections, then SurfBoot
  loading SurfOS);
* the 35T placed on the whole xc7a50t die (OPEN-FLOW.md 6.3), now with 93 % of its block RAM.

## 11. Pending, and what to check first on hardware

* **MIG generation, synthesis, implementation and timing closure**: blocked on a Vivado host.
* **Physical calibration**: `init_calib_complete` on the real board; measure how long it takes
  (LiteDRAM: about 1 s expected, section 10.1) against `CALIB_TIMEOUT_CYCLES` (1.5 s).
* **Milestone 4's physical tests** (fw track's DDR diagnostic): address and data lines, every byte
  lane, boundaries and aliases, sustained copies, code copied from BRAM into DDR and executed.
* The DDR part and board revision (owner): Digilent's files are for revision E.0 and the
  MT41K128M16 family.
* The open flow: LiteDRAM's 100 MHz clock misses nextpnr's timing (section 10.3), so there is no
  DDR bitstream yet; routing converges only for some placements, LVS differs on one block RAM
  write enable, and two of the flow's checks misread these designs (all for the flow track,
  10.3); then the calibration on the board (section 10.5).
* Not covered by any test here: metastability itself, and the real MIG's behaviour beyond the
  model (its latencies, reordering, refresh). The model follows UG586's rules; the MIG may differ
  in timing, which only changes latency.

## 12. Notes for the other tracks

* **soc**: `sim/models/ddr_model` should behave like this bridge: `s_req_ready` low while an
  access is outstanding; refused accesses answered with an error in the next cycle (before
  calibration, after a failure or a timeout); normal latency 5 cycles and up; the watchdog's
  error exactly `TIMEOUT_CYCLES` after acceptance, then `timeout` sticky and every access refused
  until reset; `calib_done` and `calib_fail` exclusive. `DDR_STATUS.PRESENT` comes from
  `HAS_DDR`, not from the bridge.
* **board**: for the MIG (Vivado flow, not wired yet): own the oscillator's IBUF and BUFG and give
  the MIG the 100 MHz from the BUFG; make the 200 MHz reference; drive `sys_rst` low until the PLL
  is locked; read the MIG IP and the bridge's scoped XDC; no DDR3 pin constraints of your own.
  `ip/mig/mig_ddr3_stub.v` lets the top lint without Vivado. The LiteDRAM top is done (BOARD.md
  section 7).
* **fw**: wait for `CALIB_DONE` or `CALIB_FAIL` (never both); `TIMEOUT` means the DDR is out of
  service until reset (accesses fault at once); after `TIMEOUT`, report and do not retry. The 2 s
  wait suits LiteDRAM with the lite CPU (about 1 s); it would not suit the minimal CPU (about
  2 s), so keep it above the bridge's limit if either changes. The message "did not succeed
  within 2 s" appears when the bridge's `CALIB_FAIL` came first (at 1.5 s): consider naming the
  status instead.
* **flow**: what LiteDRAM needs from openXC7 places and routes (section 10.3); open are its
  timing (LiteDRAM's controller at 100 MHz in nextpnr's model), `cmtcheck.py` reading
  bit-string parameters, and `synthcheck.py` crediting a register absorbed into a block RAM's
  synchronous read port (VexRiscv's tag memories).

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
