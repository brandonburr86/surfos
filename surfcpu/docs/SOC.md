# SurfCPU SoC: interconnect, memories, base peripherals, simulation

The system SurfCPU lives in: the SurfBus interconnect of [SURFBUS.md](SURFBUS.md) section 6,
the three block RAMs, the five base peripherals of [MEMORY-MAP.md](MEMORY-MAP.md),
`surfcpu_soc` as [MODULES.md](MODULES.md) section 4, the stand-ins that let it build before the
core and the io blocks existed, the C++ UART and DDR models and the `surfsim` harness with the io
track's SD card, PS/2 keyboard and VGA capture models. Owned by the soc track
([plans/2026-10-15-tracks.md](plans/2026-10-15-tracks.md)).

**Status (2026-10-17).** Simulation: everything below passes (section 8), and the whole system
runs in `surfsim`: SurfBoot loads the Arty kernel from a card image, the kernel boots to its
shell, answers commands on the UART and on the PS/2 keyboard and draws its console on the VGA
output, and SurfBoot falls back to `BACKUP.BIN` (section 9.2). Synthesis: Yosys estimates here,
and the synthesized netlists simulate like the RTL (section 10); the open flow builds the whole
SoC into bitstreams (BOARD.md section 8); no Vivado run has happened (no build host). Board: on
the owner's Arty A7-100T, with those bitstreams loaded over JTAG, the SoC runs the BRAM
diagnostics to PASS (203 checks; 572 with the DDR3L), and SurfBoot reads the card, calibrates
the DDR3L and boots SurfOS to its shell over the UART (BOARD.md section 12,
[evidence/physical/](evidence/physical/)); the PS/2 keyboard and the kernel's VGA console are
not yet shown there.

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
| data port, ddrc window (the DDR controller's registers), not a fetch | the `ddrc_*` master port with HAS_DDR = 1 and DDR_SWINIT = 1 (HAS_DDRC in the interconnect), else error |
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
| `surf_sysinfo` | MAGIC is typed as 0x46525553 (the map gives it only as text). BUILD_FLAGS and FEATURES show only their defined bits. PRESENT = FEATURES.DDR; SWINIT = the DDR_SWINIT parameter, with PRESENT only. TIMEOUT latches the bridge's `ddr_timeout` (pulse or level) until reset and reads 1 already in the cycle of the pulse, so a load right behind the abandoned access sees it. |

## 5. `surfcpu_soc`

Ports and parameters exactly as MODULES.md section 4 (the board track's top instantiates it
with `HAS_DDR = 0`, and its DDR designs with `HAS_DDR = 1` and `DDR_SWINIT = 1`: LiteDRAM
without a CPU, whose registers the `ddrc_*` port reaches through `surf_ddr_csr`). FEATURES = all
eight blocks, DDR only with `HAS_DDR = 1`; DDR_SIZE is 0 without DDR. sysinfo sees the
calibration inputs only with `HAS_DDR = 1`; DDR_STATUS.SWINIT is `DDR_SWINIT` with DDR. `rst` and `pix_rst`
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
| `lint-soc` | Verilator -Wall of every module, the interconnect with HAS_DDR 0 and HAS_DDRC 1, the SoC with both HAS_DDR, with DDR_SWINIT and with RVFI, the stand-ins; Icarus -g2005 |
| `test-soc-units` | Icarus testbenches through a SurfBus BFM (random gaps, back-to-back requests, junk on the fields after acceptance) with a protocol monitor on every link: `tb_intc`, `tb_sysinfo` (with and without DDR, each with and without SWINIT), `tb_timer` (25 MHz and a fractional 33.33 MHz), `tb_uart` (real divisor, bit timing to the cycle, both FIFOs' overflow, framing errors, break, glitch, +-3 % baud, interrupts, error rules), `tb_gpio` (bouncing buttons at 100 kHz and at the real 25 MHz, the debounce delay measured), `tb_mem` (three memories, `+bootrom=`, empty ROM, pixel port), `tb_interconnect` (every target a random-stall slave, a reference decoder written as address ranges, 17 monitored links, both ports alone and together, DDR before, after and losing calibration, DDR errors, arbitration and the offer lock; with and without DDR, and with the ddrc window mapped) |
| `test-soc-verilator` | the same ten unit testbench configurations under Verilator --timing |
| `test-soc-system` | `tb_soc` (Icarus): the whole SoC with the scripted core, UART and GPIO looped back, a Verilog DDR model with calibration delay, random ready and latency and a watchdog case; scripts `soc_regions`, `soc_errors`, `soc_random` (also back to back), `soc_periph`, with and without DDR; a negative script must fail with exactly its three wrong expectations |
| `test-soc-models` | `sim/soc/test_models.cpp`: the UART model's waveform, exchanges, errors and baud tolerance; the DDR model against a reference over the whole window |
| `test-soc-surfsim` | the same four scripts in `surfsim` (Verilator, C++ models), a UART exchange with the model matched by `--expect`, the exit codes, a smoke run of the deliverable build with a trace |
| `test-soc-boot` | the real core in `surfsim`: the fw track's BRAM diagnostics to `BRAMDIAG: PASS`, and SurfBoot without a card to its monitor, which answers `help` |
| `test-soc-system-boot` | the whole system in `surfsim` (section 9.2): SurfBoot, the Arty kernel from a card image, a UART session, a PS/2 command checked on the decoded VGA screen, the BACKUP.BIN fallback; about 100 s |
| `synth-soc` (check) | Yosys: the logic modules synthesize (check -assert), the memories infer as memories, the SoC through synth_xilinx (section 10) |
| `test-soc-gate` (check) | the synthesized memories under `tb_mem` and the synthesized SoC under the four scripts |

`make test-soc-system-selftest` (not in `test-soc`: 1.7 billion cycles, about 8.5 minutes) boots
the same way and runs the kernel's `selftest` (section 9.2).

`make test-soc-mutants` (not in `test-soc`, about a minute) checks the tests themselves: 41
one-line bugs planted in copies of the RTL (`sim/soc/tools/mutants.py`: intc 2, uart 9, gpio 6,
timer 3, sysinfo 3, memories 5, interconnect 13, of which two run against the variant with the
DDR controller's register window, `HAS_DDRC = 1`) must each make the covering unit testbench
fail. All are caught. Surviving mutants found two gaps on the way, both closed: `tb_uart` did not
look at TX_IE while the shifter is busy, and `tb_interconnect`'s calibration-lost case could let
the fetch through before calibration dropped.

Large outputs are cleaned up: the Verilator object directories (about 170 MB each) are deleted
after use; no target writes waveforms, and traces are bounded by `--max-cycles`.

## 9. `surfsim` and the whole system

### 9.1 The harness

`build/sim/surfsim` (`make surfsim`, `sim/soc/surfsim/surfsim.cpp`): the whole `surfcpu_soc`
in Verilator with C++ models at its pins, wired as [IO.md](IO.md) section 6 says:

| Pins | Model | Options |
|---|---|---|
| `uart_tx`, `uart_rx` | UART model (115200 8N1), sampled before each clk edge | `--uart-in FILE`, `--send`, `--uart-loopback` |
| `ddr_*` | DDR model: calibration after `--ddr-calib` cycles, random ready and latency | `--ddr-calib N`, `--ddr-fail`, `--ddr-latency MIN:MAX`, `--ddr-ready PCT`, `--ddr-stall ADDR`, `+ddr_*` |
| `sd_sclk`, `sd_mosi`, `sd_cs_n`, `sd_miso`, `sd_cd_n` | io's SD card model, ticked after each clk edge with the SoC's registered outputs, MISO and card detect fed back before the next edge | `--sd IMAGE` (absent without it), `--sd-flush` |
| `ps2_clk_oe/_i`, `ps2_data_oe/_i` | io's PS/2 keyboard model with the open-drain resolution (a line is low when the SoC's `_oe` or the keyboard pulls it) | `--ps2-keys TEXT`, `--ps2-send` |
| `vga_r/g/b`, `vga_hs`, `vga_vs` | io's VGA capture, ticked on every pix_clk rising edge | `--vga-dir DIR`, `--vga-font`, `--vga-frames N`, `--vga-after REGEX` |
| `led`, `sw`, `btn` | fixed levels or looped back | `--sw`, `--btn`, `--gpio-loopback` |

`clk` runs at `SURF_SYS_HZ` and `pix_clk` at `SURF_PIX_HZ`, both taken from the generated
`soc.h` and scheduled in femtoseconds on two independent clocks (the Makefile passes the same
values to the RTL as `SYS_HZ`/`PIX_HZ`, as the board top does), each held in reset for 8 of its
cycles. Nothing in the SoC uses a falling edge, so the inputs set after a rising edge are
settled by the falling edge's evaluation. `--rom` loads the boot ROM image (`+bootrom=`).

UART output is echoed on stdout (unless `--quiet`) and kept by `--transcript FILE`. Patterns are
ECMAScript regular expressions tried after every received byte over at most the last 16 KiB
(`--fail-on`: 4 KiB); end one with what must follow it, such as `\n` (`.` does not match the `\r`
of CRLF):

* `--expect REGEX`, repeatable: all must match, in order, each after the previous match; then the
  run ends (exit 0), or runs `--settle N` cycles more (to let the screen catch up).
* `--send REGEX=TEXT` sends TEXT to the UART once REGEX has matched; `--ps2-send REGEX=TEXT`
  types it on the keyboard. Each kind goes in order, each REGEX searched after the previous one's
  match, so a session answers a prompt with a command and the next prompt with the next. TEXT
  takes `\r \n \t \e \b \\ \xHH`; the first `=` ends REGEX (write `\x3d` for one in it).
* `--fail-on REGEX` ends the run with exit 1.
* Every match and send is logged on stderr with its cycle; `--progress N` prints a line every N
  cycles.

`--vga-dir DIR` writes, at the end, `DIR/screen.ppm` (the last whole frame) and `DIR/screen.txt`:
the measured timing and the 80x30 text decoded from the pixels with the font ROM
(`rtl/peripherals/surf_video_font.v`), the cursor's cell and the count of cells that match no
glyph. The cursor blinks (16 frames dark of 32), so while `--settle` runs every whole frame is
decoded and, when the last frame is a dark one, the cell where the cursor was last seen is
given. `--vga-frames N` also writes every Nth frame. `--trace` writes the RVFI trace of MODULES.md
section 5; other plusargs go to the RTL (`+script=` for the scripted core).

Exit: 0 when every `--expect` matched, or a clean finish (without `--expect`, also when
`--max-cycles` ran out); 1 on `$fatal`/`$stop`, a `--fail-on` match, a DDR handshake error, an SD
or PS/2 protocol violation seen by the card or keyboard model (the end of its log is printed), or
an unmatched `--expect` at `$finish`; 2 when `--max-cycles` ran out before the `--expect`
patterns matched. The summary on stderr gives cycles, simulated and wall time, and the models'
counters.

Speed (container on the owner's Mac, Verilator 5.032 `-O3 --x-assign fast`, one thread): about
3.3 million cycles a second with the real core, 7.6 times slower than real time. The pixel clock
domain and the VGA capture cost about 1 %, so `pix_clk` always runs.

### 9.2 The whole system: `make test-soc-system-boot`

`sim/soc/tools/system_boot_test.py` builds two cards with `tools/mkcard.py` (64 MiB, the standard
directories, `/BOOT/CMDLINE.TXT` = `ascii`, `/USERS/DOCUMENTS/HELLO.TXT` and an 8 KiB `DATA.BIN`)
and runs `surfsim` with `HAS_DDR=1`, the DDR model and SurfBoot (`build/fw/surfboot.mem`) twice,
side by side; expectations follow `boards/arty_a7/tests/surfemu_test.py` where they fit:

* **boot** (card with `/BOOT/SURFOS.BIN` from `build-kernel`): in order on the UART, SurfBoot
  finds the package, loads it over `surf_spi` and checks its payload CRC (as read from the card
  and from DDR); the handoff (entry 0x80000000, a1 = the boot information, a2 = the magic); the
  kernel's boot messages (its card driver, the FAT32 partition mounted) and banner; the prompt.
  Typed on the UART: `bootinfo`, `ls` and `cat` in `/sd0p1/USERS/DOCUMENTS`, `ps` (7 kernel
  tasks: idle, init, heartbeat, four shells), `tasks` (two more tasks count, sleep and wake on the
  machine timer), `crc32` of both files (zlib's values). Then `drivers` typed on the PS/2 keyboard
  model: on the UART its table (the keyboard answered, its keys counted), and on the screen decoded
  from the VGA pixels the command line and its output, the cursor on the new prompt (the run
  settles 8 million cycles, about 19 frames, to see it through its blink), 800x525 timing at
  59.95 Hz, every cell a glyph of the font ROM.
* **recovery** (card with a `SURFOS.BIN` whose payload has one byte flipped and the good package as
  `/BOOT/BACKUP.BIN`): SurfBoot reports the CRC mismatch, tries `BACKUP.BIN`, starts it ("boot:
  starting BACKUP.BIN at 0x80000000 ..."); the kernel boots to the prompt and `bootinfo` says
  "from BACKUP.BIN".

Results (2026-10-16, in `make check` at b4717a6, SURFOS.BIN of 361,800 bytes): 21 of 21
checks; boot 314.3 million cycles (12.57 s simulated) in 95 s, recovery 195.8 million cycles
(7.83 s) in 58 s, so 95 s for the target; with `--seed 7` (other DDR and card timing) 21 of 21
as well. Where the simulated time goes (cycles after reset, at 25 MHz, in that run):

| Milestone | boot run | recovery run |
|---|---|---|
| SurfBoot has loaded the package (SPI at 6.25 MHz) | 45.7 M (1.83 s) | 80.8 M (3.23 s), after reading the damaged file once (36.3 M) |
| handoff | 46.3 M (1.85 s) | 81.4 M (3.26 s) |
| the kernel's first line | 56.3 M (2.25 s) | 91.4 M (3.66 s) |
| card mounted | 106.9 M (4.28 s) | 141.9 M (5.68 s) |
| shell prompt | 128.5 M (5.14 s) | 163.8 M (6.55 s) |
| bootinfo, ls, cat, ps, tasks, crc32 done | 262.5 M (10.50 s) | (bootinfo: 180.1 M) |
| `drivers` typed on PS/2, output complete | 299.8 M (11.99 s) | |

The kernel runs from DDR, fetched one word at a time (the core keeps one fetch outstanding and
has no cache) with the DDR model's latency of 2 to 12 cycles: 24.3 million DDR fetches in the
260 million cycles after the handoff, idle time included. So it is slow in simulated time, as on
the fw track's emulator, and two steps of `surfemu_test.py` are left out for wall time: `crc32`
of the 353 KiB package (23 s simulated, 584 million cycles, measured here) and the self test,
which `make test-soc-system-selftest` runs on its own: `SELFTEST PASS` (35 checks ok, the FAT
write check skipped on the read-only card) after 1.73 billion cycles (69.4 s simulated, 64 s of
them the self test) in 505 s ([record](evidence/soc/test-soc-system-selftest.txt)).
The decoded screen and the transcripts are kept in `build/soc/system_boot/`, with each run's
summary (`summary.txt`); the record of the b4717a6 run these figures come from (SurfBoot 1.0.0,
SoC 0.1.0) is [evidence/soc/test-soc-system-boot.txt](evidence/soc/test-soc-system-boot.txt).

## 10. Synthesis estimate and post-synthesis simulation

`make synth-soc` runs Yosys 0.52 `synth_xilinx` (out of context, `-noiopad -noclkbuf`). Since
the merges it synthesizes the whole SoC with the real core and io blocks (no black boxes) but
without the DDR bridge and board top: 5,412 LUTs, 2,673 flip-flops, 306 CARRY4, 36 RAMB36,
4 DSP48E1 and 30 LUTRAM primitives (2026-10-16). The soc part alone, with the stand-ins as black
boxes (round 1): 1,144 LUTs, 618 flip-flops, 85 CARRY4, 35 RAMB36 and 5 LUTRAM primitives (the
UART FIFOs). These are Yosys estimates, not Vivado results; BRAM fits the 35T's 50 RAMB36.

Yosys can drop logic silently, so `make test-soc-gate` simulates what it produced: `tb_mem` on
the three memories synthesized alone, and the four SoC scripts on the whole synthesized SoC with
the stand-ins as RTL for the black boxes. All pass, with exactly the RTL's cycle counts. Yosys
0.52's `cells_sim.v` has only a timing shell for RAMB36E1, so
`sim/soc/synth/surf_ramb36e1_model.v` models it from UG473 (true dual port, every width, the
three write modes, the INIT/INITP layout) and stops on anything it does not model. The core's
multiplier (the 4 DSP48E1, where the io track saw Yosys delete logic) is outside this check.

## 11. Integration preview (scratch export, simulation only)

Superseded since the merges: `make check` runs all of this on the merged tree (`synth-soc` with
no black boxes, `test-soc-boot`, `test-soc-system-boot`). Kept as the round 1 record.

With the cpu and io tracks' RTL from `surfcpu` at 9b8a93b, put into this worktree only for the
run (untracked, removed afterwards), the makefile picked the real files by itself and:

* `lint-soc` is clean with the real core and io blocks; `test-soc-system`, `test-soc-surfsim` and
  `test-soc-gate` pass with the real io blocks (their reset values and error rules match the
  stand-ins'); the synthesis estimate of section 10 includes them.

In an export of `surfcpu` at 9b8a93b (with the cpu, io and fw merges) with this branch's soc
files laid over it, under the ignored `build/`, removed afterwards (`make test-soc-boot`):

* `build/sim/surfsim` with the real core runs the fw track's BRAM diagnostics to
  `BRAMDIAG: PASS (569 checks)` after 66.5 million cycles (exceptions from the interconnect's
  error responses, timer and UART interrupts, FENCE.I, the DDR tests through the DDR model and
  code run from DDR), and SurfBoot 1.0.0 to "no card" and its monitor, which answers `help` typed
  through the UART model. `--trace` gives the documented trace.

## 12. Limits and pending

* Physical: the open flow's bitstreams of the whole SoC run on the owner's A7-100T (BOARD.md
  section 12, `docs/evidence/physical/`). Vivado's timing closure and BRAM inference are pending a
  build host.
* The DDR arbiter loses one cycle per DDR access (section 2).
* The system runs are simulation with models, not the board: the card, keyboard and DDR models
  answer as the io and soc tracks wrote them, and the DDR model is not the MIG.
* The screen during `tasks` (two kernel tasks printing at once) shows blank and partly written
  rows where the UART has whole lines; the fw track's emulator shows the same kind of damage. It
  is the kernel's console, not the RTL: `kputch()` mirrors each character to the serial line and
  then updates the console's grid without excluding other tasks, and `sscroll()` copies the
  4,800-byte grid one byte at a time (by my estimate about 10 ms on SurfCPU running from DDR,
  as long as a timer tick), so a task preempted inside a scroll lets the other task write its
  line into rows that the resumed scroll then moves or clears (reported to the os track).
* Not tested: a reset in the middle of traffic (the whole SoC resets together, SURFBUS.md rule
  8, so no request survives).

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
* MODULES.md section 4 gives `PIX_HZ = 25177305` (and `SYS_HZ = 25000000`) as literal defaults,
  kept so in `surfcpu_soc`; the board top and `surfsim` pass the generated `SURF_PIX_HZ` and
  `SURF_SYS_HZ`, so a change of `clocks.pix_hz` reaches them, but the defaults (here, in
  `surf_sysinfo` and `surf_video`) would then disagree with the spec until MODULES.md changes them.
* MODULES.md section 7 lists the `surfsim` options of version 1; the ones added for the system
  runs (repeatable `--expect` in order, `--send`, `--ps2-send`, `--fail-on`, `--settle`,
  `--transcript`, `--sd-flush`, the `--vga-dir` outputs, exit 2) are proposed for it in the soc
  round 2 report.
