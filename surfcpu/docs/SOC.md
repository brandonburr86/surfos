# SurfCPU SoC: interconnect, memories, base peripherals, simulation

The system SurfCPU lives in: the SurfBus interconnect of [SURFBUS.md](SURFBUS.md) section 6,
the three block RAMs, the five base peripherals of [MEMORY-MAP.md](MEMORY-MAP.md),
`surfcpu_soc` as [MODULES.md](MODULES.md) section 4, the stand-ins that let it build before the
core and the io blocks existed, the C++ UART and DDR models and the `surfsim` harness. Owned by
the soc track ([plans/2026-10-15-tracks.md](plans/2026-10-15-tracks.md)).

**Status (2026-10-15).** Simulation: everything below passes (section 8). Synthesis: Yosys
estimates only, and the synthesized netlists simulate like the RTL (section 10); no Vivado run
has happened (no build host). Board: nothing has run on the Arty; all physical results are
**pending**.

## 1. The system

```
                       clk (sys_hz)                                             pix_clk
 surfcpu_core.ibus --> I decoder --> boot ROM port A | scratch port A | DDR arbiter (a) | error
 surfcpu_core.dbus --> D decoder --> boot ROM port B (reads) | scratch port B | uart | timer |
                                     intc | gpio | spi | ps2 | video | sysinfo |
                                     text RAM port A | DDR arbiter (b) | error
 DDR arbiter (data first, one at a time) --> calibration gate --> ddr_* master port
 text RAM port B (pix_clk) <------------------------------------------- surf_video scan-out
 timer: mtime -> core, MTIP, MSIP     uart, ps2, gpio, video, spi -> intc -> MEIP
```

Files: `rtl/soc/` (`surf_bus_err`, `surf_bus_demux`, `surf_ibus_decoder`, `surf_dbus_decoder`,
`surf_bus_arbiter`, `surf_ddr_gate`, `surf_interconnect`, `surf_bootrom`, `surf_scratch`,
`surf_textram`, `surfcpu_soc`, `surf_bus_targets.vh`), `rtl/peripherals/surf_{uart,timer,intc,
gpio,sysinfo}.v`. Every address, register offset and field position comes from
`rtl/generated/surfcpu_soc.vh`. All of it is Verilog-2005 without vendor primitives,
`verilator --lint-only -Wall` clean and compiled by Icarus (`make lint-soc`).

## 2. The interconnect

**Decoding.** Each decoder compares the address with every region of the memory map (base and
mask from the generated header) and gives the request to exactly one target; anything else goes
to that decoder's own error slave (`surf_bus_err`), which answers with `rsp_err` in the next
cycle and has no other state. Targets are numbered in `surf_bus_targets.vh`.

| Request | Goes to |
|---|---|
| instruction port, boot ROM / scratch / DDR (HAS_DDR = 1) | that memory's port A, the arbiter's fetch side |
| instruction port, anything else (MMIO, text RAM, eth, holes), or a write | error |
| data port, boot ROM read | boot ROM port B |
| data port, boot ROM write | error (the ROM never sees it; port B refuses writes too) |
| data port, scratch, an MMIO window, text RAM | that target |
| data port marked as a fetch (the core never does it) to MMIO or text RAM | error |
| data port, DDR | the arbiter's data side (error with HAS_DDR = 0) |
| either port, eth window or unmapped | error |

**Response routing** (`surf_bus_demux`). The request fields go to every target; only the
selected one sees `req_valid`, and the master sees that target's `req_ready`. A register
remembers which target accepted the master's request, and its response is the one returned,
whatever the latency; a new request accepted in the same cycle as the previous response is
handled (that is what gives one access per cycle with single-cycle slaves). Decoding depends
only on the request fields, which the master holds until acceptance, so the choice never
changes during an offer.

**DDR arbiter** (`surf_bus_arbiter`). When the DDR side is free, a data request wins over a
fetch. A request offered to the DDR side but not yet accepted stays the offered one until
accepted, even if the other side asks meanwhile (no withdrawal, SURFBUS.md rule 2). After
acceptance nothing new is offered until the response has come back, and the response goes to the
side that issued the request. A new grant is made in the cycle after a response, not in the same
cycle: one idle cycle per DDR access, a deliberate simplification at DDR latencies of 10 or more
cycles.

**Calibration gate** (`surf_ddr_gate`). While `ddr_calib_done` is 0 it accepts every request
itself and answers it with an error in the next cycle; nothing reaches the bridge. The decision
is taken when a request is first offered: a request already being offered to the bridge stays
offered until taken, even if calibration is lost meanwhile (the bridge's watchdog bounds it).
With `HAS_DDR = 0` there is no arbiter or gate: the DDR window is unmapped for both decoders and
the `ddr_*` outputs stay 0.

The request path is combinational from the core's address through the decoder to the selected
slave's `req_ready`; responses are registered in every slave and multiplexed by a registered
selection. Timing at 25 MHz has not been checked by a place-and-route tool (pending a Vivado
host).

## 3. The memories

| Memory | Ports | Contents | Block RAM (Yosys) |
|---|---|---|---|
| `surf_bootrom`, 64 KiB | A: fetches; B: data reads. Both answer next cycle; a write on either gets an error and changes nothing; reads return the whole word | `BOOTROM_INIT` ($readmemh, MODULES.md section 5); all zero without one; in simulation `+bootrom=<file>` loads another image at time 0 | 15 RAMB36 (4Kx9, parity bits used as data, LUT multiplexers) |
| `surf_scratch`, 64 KiB | A: fetches (reads only; a write gets an error); B: data, byte enables, read-first | zero | 16 RAMB36 (16Kx2) |
| `surf_textram`, 16 KiB | A (clk): SurfBus, byte enables, reads only when not writing; B (pix_clk): `pix_data` = cell `pix_addr` one `pix_clk` edge later | zero; cells 0..2399 the screen, 2400..4095 ordinary RAM (the scan-out may read them) | 4 RAMB36 (4Kx9) |

Port A of the scratch RAM reading the word port B writes in the same cycle gets the old word
(READ_FIRST, safe on 7-series with one clock). The text RAM is the only thing shared by the two
clock domains, so it needs no other CDC logic. Its port A uses NO_CHANGE mode on purpose: UG473
warns that a true dual-port block RAM in READ_FIRST mode with asynchronous clocks can corrupt
the stored word on a same-address collision, and Yosys refuses READ_FIRST there for that reason
(it fell back to 4096 LUTs of distributed RAM). With NO_CHANGE a collision can at worst give the
video one wrong read, which is harmless, and memory is never corrupted. A SurfBus write's read
data are don't-care, so nothing visible changes. Vivado's inference has not been run; the code
follows its dual-clock NO_CHANGE byte-write template.

## 4. The peripherals

Common to all five (and to the text RAM's rules): `s_req_ready` is always 1 and every request is
answered in the next cycle. An offset with no register, a non-word access (`req_be` other than
1111, or a misaligned word) and a fetch get `rsp_err` and have no effect at all (no FIFO push or
pop, no register change). Writes to read-only registers are accepted and ignored. Read data are
0 for writes and errors. Unused bits read 0. Interrupt outputs are combinational from registers,
so a write that clears a cause has taken effect by the time its response arrives.

| Block | Choices where MEMORY-MAP.md leaves room |
|---|---|
| `surf_uart` | DIV: a write below the minimum of 16 stores 16. RX: `rx` is synchronized; a falling edge starts a frame, the start bit is checked half a bit later (shorter glitches are ignored), each bit is sampled in its middle. A stop bit of 0 drops the byte and sets RX_FRAME, and also RX_BREAK when all eight data bits were 0; the receiver then waits for the line to return to 1, so a long break counts once. RX_OVF is set when a byte arrives with the FIFO full in that cycle (a pop in the same cycle does not save it). TX: start, 8 data bits LSB first and one stop bit, exactly DIV cycles each, back to back while bytes wait. A sticky event in the same cycle as its w1c clear wins. |
| `surf_timer` | The rate comes from a fractional accumulator (exactly TIMEBASE_HZ ticks per SYS_HZ cycles for any ratio; every 25 cycles at 25 MHz). A write to MTIME_LO or MTIME_HI replaces that half; a tick in the same cycle is lost (the ISA.md rule for mcycle). MTIP = mtime >= mtimecmp straight from the registers. |
| `surf_intc` | ENABLE stores all 32 bits; PENDING shows all 32 source levels. |
| `surf_gpio` | Two-flip-flop synchronizers on every input. Buttons: a shared 1 ms tick; the debounced state follows the pin after it has differed for 10 consecutive ticks (9 to 10 ms of stable level; a bounce restarts the count). A debounced press sets BTN_EDGE (a press in the same cycle as the clear wins). RGB at 1/8 duty: on for 256 of every 2048 cycles (about 12 kHz at 25 MHz). LED and RGB outputs are registered. |
| `surf_sysinfo` | MAGIC is typed as 0x46525553 (the map gives it only as text). BUILD_FLAGS and FEATURES show only their defined bits. PRESENT = FEATURES.DDR. TIMEOUT latches the bridge's `ddr_timeout` (pulse or level) until reset and reads 1 already in the cycle of the pulse, so a load right behind the abandoned access sees it. |

## 5. `surfcpu_soc`

Ports and parameters exactly as MODULES.md section 4 (the board track's top instantiates it
with `HAS_DDR = 0`). FEATURES = all eight blocks, DDR only with `HAS_DDR = 1`; DDR_SIZE is 0
without DDR. sysinfo sees the calibration inputs only with `HAS_DDR = 1`. `rst` and `pix_rst`
are expected from a `surf_reset_sync` per domain; `ddr_calib_*` and `ddr_timeout` already in
the clk domain. Interrupt sources by spec/soc.toml [irqs]: uart 0, ps2 1, gpio 2, video 3, spi 4.
`surf_ps2`'s `*_oe` outputs go straight to the ports (the board drives the lines only low).
With `SURFCPU_RVFI` the core's RVFI outputs pass through.

## 6. Stand-ins

`sim/soc/stubs/` holds modules with the exact contract names, parameters and ports, never
synthesized:

* `surfcpu_core`: a scripted SurfBus master (two `sim/soc/bfm/surf_script_port.v` engines, one
  per port) that runs a compiled bus script (section 7) and prints `SCRIPT PASS` or
  `SCRIPT FAIL`. Under `SYNTHESIS` it is only a port list (a black box for synthesis).
* `surf_spi`, `surf_ps2`, `surf_video`: tie-offs that keep their register maps' SurfBus rules
  (registers answer, other offsets, non-word accesses and fetches get errors), read their reset
  values where the map defines them (SPI DIV, PS/2 CTRL and DATA's EMPTY, video CTRL, INFO,
  PIXCLK), ignore writes, and hold their pins idle.

`mk/area-soc.mk` uses a stand-in only while the real file is missing (`rtl/cpu/surfcpu_core.v`
brings in all of `rtl/cpu/*.v`; `rtl/peripherals/surf_spi.v` brings `surf_spi*.v`, and so on).
The SoC's interconnect tests always use the scripted core; lint, `build/sim/surfsim` and the
synthesis estimate use the real core as soon as it exists.

## 7. Bus scripts

`sim/soc/tools/surfscript.py` compiles `sim/soc/scripts/*.sbs` for the scripted core (its
docstring is the language reference). Operations go to the data port (D) or the fetch port (I),
which run at the same time and meet at `sync`: reads and writes of every width with expected
data or errors, polls, idle cycles, waits for and checks of the core's interrupt inputs, a check
that the core's `mtime` input follows the timer, and generators for random memory traffic,
unmapped addresses and every MMIO register's error rules. Every address, field and constant
comes from spec/soc.toml by name (`uart.STATUS.RX_AVAIL`, `ddr.last`, `clocks.sys_hz`); `auto`
and `expect mem` take outcomes and values from the compiler's model of the memory map rules and
the memories; it refuses scripts in which the two ports race on a word. The engine idles
0..`+gap` cycles (from `+seed`) before each request and, with no gap, offers the next request in
the same cycle as the previous response.

## 8. Tests

`make lint test-soc check` (container). Seeds 1, 7, 123 unless noted; results in
[evidence/soc/](evidence/soc/).

| Target | What |
|---|---|
| `lint-soc` | Verilator -Wall of every module, the SoC with both HAS_DDR and with RVFI, the stand-ins; Icarus -g2005 |
| `test-soc-units` | Icarus testbenches through a SurfBus BFM (random gaps, back-to-back requests, junk on the fields after acceptance) with a protocol monitor on every link: `tb_intc`, `tb_sysinfo` (with and without DDR), `tb_timer` (25 MHz and a fractional 33.33 MHz), `tb_uart` (real divisor, bit timing to the cycle, both FIFOs' overflow, framing errors, break, glitch, +-3 % baud, interrupts, error rules), `tb_gpio` (bouncing buttons at 100 kHz and at the real 25 MHz, the debounce delay measured), `tb_mem` (three memories, `+bootrom=`, empty ROM, pixel port), `tb_interconnect` (every target a random-stall slave, a reference decoder written as address ranges, 17 monitored links, both ports alone and together, DDR before, after and losing calibration, DDR errors, arbitration and the offer lock; with and without DDR) |
| `test-soc-verilator` | the same ten unit testbench configurations under Verilator --timing |
| `test-soc-system` | `tb_soc` (Icarus): the whole SoC with the scripted core, UART and GPIO looped back, a Verilog DDR model with calibration delay, random ready and latency and a watchdog case; scripts `soc_regions`, `soc_errors`, `soc_random` (also back to back), `soc_periph`, with and without DDR; a negative script must fail with exactly its three wrong expectations |
| `test-soc-models` | `test_models`: the UART model's waveform, exchanges, errors and baud tolerance; the DDR model against a reference over the whole window |
| `test-soc-surfsim` | the same four scripts in `surfsim` (Verilator, C++ models), a UART exchange with the model matched by `--expect`, the exit codes, a smoke run of the deliverable build with a trace |
| `test-soc-boot` | once the real core and firmware exist: the BRAM diagnostics to `BRAMDIAG: PASS` and SurfBoot to its monitor in `surfsim` (skipped until then) |
| `synth-soc` (check) | Yosys: the logic modules synthesize (check -assert), the memories infer as memories, the SoC through synth_xilinx (section 10) |
| `test-soc-gate` (check) | the synthesized memories under `tb_mem` and the synthesized SoC under the four scripts |

`make test-soc-mutants` (not in `test-soc`, about a minute) checks the tests themselves: 37
one-line bugs planted in copies of the RTL (`sim/soc/tools/mutants.py`: intc 2, uart 9, gpio 6,
timer 3, sysinfo 2, memories 5, interconnect 10) must each make the covering unit testbench
fail. All are caught. Surviving mutants found two gaps on the way, both closed: `tb_uart` did not
look at TX_IE while the shifter is busy, and `tb_interconnect`'s calibration-lost case could let
the fetch through before calibration dropped.

Large outputs are cleaned up: the Verilator object directories (about 170 MB each) are deleted
after use; no target writes waveforms, and traces are bounded by `--max-cycles`.

## 9. `surfsim`

`build/sim/surfsim` (`make surfsim`): the whole SoC in Verilator with the UART model on the
serial pins and the DDR model on the DDR port, at the spec's clock rates. Options of MODULES.md
section 7 (`--rom --sd --uart-in --ps2-keys --vga-dir --trace --max-cycles --expect`) plus
`--seed`, `--quiet`, `--uart-loopback`, `--gpio-loopback`, `--sw`, `--btn`, `--ddr-calib N`,
`--ddr-fail`, `--ddr-latency MIN:MAX`, `--ddr-ready PCT`, `--ddr-stall ADDR`; tb_soc's `+ddr_*`
plusargs work too, and other plusargs go to the RTL (`+script=` for the scripted core). The SoC
output is echoed on stdout. `--expect` is an ECMAScript regular expression tried after every
received byte (end it with what must follow, such as `\n`; `.` does not match the `\r` of CRLF).
Exit: 0 when the output matched, or a clean finish (without `--expect`, also when
`--max-cycles` ran out); 1 on `$fatal`, a DDR protocol violation seen by the model, or an
unmatched `--expect` at `$finish`; 2 when `--max-cycles` ran out before `--expect` matched.
`--trace` writes the RVFI trace of MODULES.md section 5. `--sd`, `--ps2-keys` and `--vga-dir`
are accepted but their models (io track) are wired in at integration: until then the card is
absent (`sd_cd_n` = 1) and the PS/2 lines idle; the hook points are marked in `surfsim.cpp`.

## 10. Synthesis estimate and post-synthesis simulation

`make synth-soc` runs Yosys 0.52 `synth_xilinx` (out of context, `-noiopad -noclkbuf`). With
the stand-ins as black boxes the soc part comes to about 1,200 LUTs, 620 flip-flops, 85 CARRY4,
35 RAMB36 and 5 LUTRAM primitives (the UART FIFOs). With the cpu and io tracks' blocks (tried in
a scratch export, section 11) the whole SoC without DDR bridge and board top is about 5,400 LUTs,
2,670 flip-flops, 36 RAMB36, 4 DSP48E1 and 30 LUTRAM primitives. These are Yosys estimates, not
Vivado results; BRAM fits the 35T's 50 RAMB36.

Yosys can drop logic silently, so `make test-soc-gate` simulates what it produced: `tb_mem` on
the three memories synthesized alone, and the four SoC scripts on the whole synthesized SoC with
the stand-ins as RTL for the black boxes. All pass, with exactly the RTL's cycle counts. Yosys
0.52's `cells_sim.v` has only a timing shell for RAMB36E1, so
`sim/soc/synth/surf_ramb36e1_model.v` models it from UG473 (true dual port, every width, the
three write modes, the INIT/INITP layout) and stops on anything it does not model. The core's
multiplier (the 4 DSP48E1, where the io track saw Yosys delete logic) is outside this check.

## 11. Integration preview (scratch export, simulation only)

Run in an export of `surfcpu` at 9b8a93b (with the cpu, io and fw merges) with this branch's
soc files laid over it, under the ignored `build/integ/`; nothing was committed from it:

* `lint-soc` clean with the real core and io blocks; `test-soc-system`, `test-soc-surfsim` and
  `test-soc-gate` pass with the real io blocks (their reset values and error rules match).
* `build/sim/surfsim` with the real core runs the fw track's BRAM diagnostics to
  `BRAMDIAG: PASS (569 checks)` after 66.5 million cycles (exceptions from the interconnect's
  error responses, timer and UART interrupts, FENCE.I, the DDR tests through the DDR model and
  code run from DDR), and SurfBoot 1.0.0 to "no card" and its monitor, which answers `help` typed
  through the UART model. `--trace` gives the documented trace.

## 12. Limits and pending

* Physical: nothing has run on the board. Timing closure, Vivado's BRAM inference and the
  bitstream are pending a build host.
* The DDR arbiter loses one cycle per DDR access (section 2).
* `surfsim` lacks the SD, PS/2 and VGA models until integration.
* Not tested: a reset in the middle of traffic (the whole SoC resets together, SURFBUS.md rule
  8, so no request survives).
* A stray container from an early `synth-soc` version (it mapped the 64 KiB memories to
  flip-flops) may still be running in this session's Docker; see the track report.

## 13. Contract notes

Built exactly to the frozen contracts; points where they leave room or could be clearer:

* `sysinfo.MAGIC` is only text in spec/soc.toml; a generated constant (`SURF_SYSINFO_MAGIC_VALUE`)
  would avoid typing it.
* `uart.DIV` "minimum 16" does not say what a smaller write does (here: stores 16).
* `uart.STATUS`: whether a break sets RX_FRAME as well as RX_BREAK (here: both, a break's stop bit
  is 0).
* `surf_ddr_bridge`'s `timeout` output: pulse or level is not specified (sysinfo handles both).
* `timer`: a tick in the cycle of a write to MTIME_LO/HI (here: lost, like mcycle).
* MODULES.md section 3 gives no parameter types for `surf_sysinfo` (here: as `surfcpu_soc`'s).
