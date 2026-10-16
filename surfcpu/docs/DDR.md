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
* The board top (board track) instantiates the controller and the bridge and owns the clocks.

## Status (2026-10-15)

| Part | Simulation | Synthesis | On the board |
|---|---|---|---|
| `surf_ddr_bridge`, `surf_ddr_mailbox` | all tests pass (section 8): MIG model, LiteDRAM's controller, Icarus, 7 clock pairs, mutation check; formal proofs of the mailbox and of the bridge's SurfBus obligations | Yosys `synth_xilinx`: 247 LUTs, 387 flip-flops, no vendor primitives (an open-tool estimate) | **pending** |
| MIG recipe (`ip/mig/`) | the bridge is tested against a behavioural model of the MIG's user interface, not the MIG | **pending**: needs Vivado (no build host, BUILD-HOST.md); recipe dry-run only | **pending** |
| LiteDRAM core (`ip/litedram/`) | its real controller (with LiteDRAM's DRAM model in place of the PHY) passes the bridge tests through the adapter | generated in the container; Yosys estimate 5,606 LUTs, 3,634 flip-flops; place and route in the open flow **pending** (flow track) | **pending** |
| `surf_ddr_litedram_adapter` | tested against LiteDRAM's controller; 5 of 5 mutants caught | lints and compiles; not synthesized on its own | **pending** |

Nothing here has run on an FPGA. Milestone 4's physical evidence (calibration, address and data
patterns, byte lanes, boundaries and aliases, sustained transfers, code running from DDR) is all
pending (section 9).

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
| `CALIB_TIMEOUT_CYCLES` | 25,000,000 | sys_clk cycles after `rst` for calibration to succeed (1 s at 25 MHz; 0 = no limit). **A guess**: the real MIG calibration time must be measured on the board |
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
* **Ordering**: our MIG configuration asks for strict ordering (section 6), so a read after a
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

sys_clk (the board's PLL) and ui_clk (the MIG's MMCM) both come from the 100 MHz oscillator, so
Vivado sees them as related clocks and would time the bridge's crossings as synchronous paths
against the closest edges of 25 MHz and 81.25 MHz, a meaningless requirement.
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
and it finds the clocks through the module's `clk` and `ui_clk` ports. Any other path between
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
| `make test-ddr-defaults` | the bridge with its default parameters: calibration limit of 25,000,000 cycles (ui clock stopped), the 4096-cycle watchdog (18 cases), 5000 random accesses | pass; `calib_fail` after 25,000,001 cycles |
| `make test-ddr-icarus` | the mailbox in Icarus (5 clock pairs x 5000 messages, clock stops, misuse) and a four-state smoke test of the bridge with a Verilog MIG stand-in (3 seeds x 1094 accesses) | pass |
| `make test-ddr-litedram` | `tb_ddr_litedram --test all`, seeds 1, 2, plus a jittered run: the bridge and adapter against LiteDRAM's controller | 2 x 8 tests + 1 pass |
| `make synth-ddr` | Yosys 0.52 `synth_xilinx` of the bridge | 247 LUTs, 387 flip-flops, 11 CARRY4 (estimate) |
| `make check-mig` | the MIG recipe without Vivado (section 6.4) | pass |
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
`init_done`) and sysreset (5000 accesses, about 230 resets) against `tb_litedram_top.v`: the
bridge, the adapter and LiteDRAM's controller, crossbar and native port, generated with
`--sim` (LiteDRAM's DRAM model instead of the Artix-7 PHY, its memory zero at the start).

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

For BUILD-HOST.md option C (no Vivado, so no MIG), `ip/litedram/` generates a LiteDRAM core:

* `make litedram-setup` (`ip/litedram/setup.sh`) builds a pinned, vendored generator in
  `build/third_party/litedram/`: Migen `e19524c9`, LiteX `b6ae9e0b` and LiteDRAM `c454a440`
  (both tag 2026.08), pythondata-cpu-vexriscv `642ecfed`, -software-picolibc `6a13ccce`,
  -software-compiler_rt `6eb76609`, -misc-tapcfg `a12c3f59`, and the PyPI wheels PyYAML 6.0.2,
  packaging 25.0, colorama 0.4.6, meson 1.9.1 and ninja 1.13.0 with checked SHA-256, unpacked
  without pip. It needs the network once.
* `make litedram-gen` generates `ip/litedram/arty.yml` (LiteDRAM's Arty example with one
  native port) into `build/litedram/arty/` and `arty_sim.yml` into `build/litedram/arty_sim/`,
  and writes `build/litedram/summary.txt` (`ip/litedram/inspect_core.py`, `estimate.sh`).

What came out, for the Arty (2026-10-15, in the container):

* `litedram_core.v` with the DDR3 pins, `clk` (100 MHz in) and `rst`, `pll_locked`,
  `init_done`/`init_error`, a serial console `uart_tx`/`uart_rx`, and the native user port:
  `cmd_addr[23:0]` (128-bit beats), `cmd_we`, `wdata_data[127:0]`, `wdata_we[15:0]` (byte enables,
  1 = write), `rdata_data[127:0]`, valid/ready on each, and `user_clk`/`user_rst` (100 MHz).
* Clocking inside: one `PLLE2_ADV`, 100 MHz in, VCO 1600 MHz, giving 200 MHz (IDELAYCTRL
  reference), 100 MHz (core, user port), 400 MHz and 400 MHz at 90° (DDR3 at 800 MT/s).
* Vendor primitives: ISERDESE2 x16, OSERDESE2 x45, IDELAYE2 x16, IDELAYCTRL x1, IOBUF x16,
  IOBUFDS x2, OBUFDS x1, BUFG x4, PLLE2_ADV x1, FDCE x8, FDPE x8; the simulation core uses none.
  The openXC7 nextpnr-xilinx source (openXC7/nextpnr-xilinx at `bc9b2346`, 2026-09-25, archived
  with development moved to openXC7/nextpnr) has packing rules for every one of them
  (`pack_io_xc7.cc`: ISERDESE2, OSERDESE2, IDELAYE2, IDELAYCTRL, IOBUF/IOBUFDS, OBUFDS;
  `pack_clocking_xc7.cc`: PLLE2_ADV, BUFG), and its CI builds LiteX DDR3 demos for the Arty S7
  (Spartan-7, the same I/O logic) and a Kintex-7 board, as non-gating jobs, but none for the Arty
  A7. That is read from the source, not run: whether the open flow places, routes and times this
  core correctly on the A7 (the IDELAYE2/ISERDESE2 paths especially) is for the flow track to
  prove.
* Calibration: the core has its own small VexRiscv running the LiteX BIOS's `sdram_init` from an
  internal ROM (27.4 KiB, 57% of 48 KiB), which raises `init_done` (or `init_error`). It needs
  `VexRiscv.v` from pythondata-cpu-vexriscv next to the core. This is a second CPU inside the
  memory controller, a calibration sequencer, not SurfCPU's replacement; the owner should know
  it is there (brief section 2). The alternative (`cpu: None`) leaves calibration to SurfBoot
  over a Wishbone control port that the memory map does not have.
* Constraints shipped: two false paths and one max delay for LiteX's reset synchronizers, but no
  pins, no I/O standards and no clocks. The board XDC must give the DDR3 pins (the same as the
  MIG's: Digilent's `mig.prj` lists them), SSTL135/DIFF_SSTL135, the bank-34 internal Vref and the
  100 MHz clock.
* Yosys `synth_xilinx` estimate (core with VexRiscv, the user port as top-level pins): 5,606
  LUTs, 3,634 flip-flops, 4 DSP48E1, 56 RAM32M, 21 RAMB18E1, 3 RAMB36E1. That is about a quarter
  of the A7-35T's 20,800 LUTs.
* The generated files carry a timestamp, so they are not byte-identical between runs; the
  generator and its inputs are pinned.

`surf_ddr_litedram_adapter` (`rtl/soc/ddr/`) puts LiteDRAM's native port behind the bridge's
MIG-style port, on `user_clk`/`user_rst`: commands straight through (`cmd_addr = app_addr[26:3]`,
`cmd_we` for a write), a two-beat write FIFO (`rtl/lib/surf_fifo`) because the bridge gives write
data before the command and LiteDRAM only takes it after (the mutation check confirms that
without it the bridge deadlocks), `wdata_we = ~app_wdf_mask`, read data always taken
(`rdata_ready = 1`), `init_calib_complete = init_done && !init_error`, `ui_rst = user_rst`. In the
board top: oscillator → IBUF → BUFG → `litedram_core.clk` and the board's own PLL; the button to
`rst`; `user_clk` is the bridge's `ui_clk` (100 MHz); the serial console can be left idle
(`uart_rx` = 1).

## 11. Pending, and what to check first on hardware

* **MIG generation, synthesis, implementation and timing closure**: blocked on a Vivado host.
* **Physical calibration**: `init_calib_complete` on the real board; measure how long it takes
  and set `CALIB_TIMEOUT_CYCLES` with margin (1 s is a guess).
* **Milestone 4's physical tests** (fw track's DDR diagnostic): address and data lines, every byte
  lane, boundaries and aliases, sustained copies, code copied from BRAM into DDR and executed.
* The DDR part and board revision (owner): Digilent's files are for revision E.0 and the
  MT41K128M16 family.
* The open flow: LiteDRAM through openXC7 (flow track), and its calibration on the board.
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
* **board**: own the oscillator's IBUF and BUFG and give the MIG the 100 MHz from the BUFG; make
  the 200 MHz reference; drive `sys_rst` low until the PLL is locked; read the MIG IP and the
  bridge's scoped XDC; no DDR3 pin constraints of your own. `ip/mig/mig_ddr3_stub.v` lets the top
  lint without Vivado.
* **fw**: wait for `CALIB_DONE` or `CALIB_FAIL` (never both); `TIMEOUT` means the DDR is out of
  service until reset (accesses fault at once); after `TIMEOUT`, report and do not retry.
* **flow**: the primitives list above is what LiteDRAM needs from openXC7.

## 13. Proposed contract changes

1. MODULES.md section 3, `surf_ddr_bridge`: add the parameters `CALIB_TIMEOUT_CYCLES`
   (25,000,000), `APP_ADDR_WIDTH` (28), `APP_DATA_WIDTH` (128), `DQ_WIDTH` (16), and note that the
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
   parameter, 1 s at 25 MHz) a place in the spec once measured.
