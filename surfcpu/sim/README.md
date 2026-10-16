# SurfCPU simulation

Testbenches, models and test programs. Everything runs in the `surfcpu-toolchain` container
(`make -C surfcpu <target>` from the Mac, or `surfcpu/devenv/run.sh <command>`); outputs go
to `build/`, which git ignores.

| Directory | What | Owner |
|---|---|---|
| `sim/lib/` | Icarus testbench of the shared blocks in `rtl/lib/` (`make test-lib`) | integrator |
| `sim/cpu/` | the core testbench `build/cpu/surfcpu_tb`, the directed tests, the test runners | cpu track |
| `sim/soc/`, `sim/models/`, `sim/io/`, `sim/ddr/`, `sim/board/`, `sim/ref/` | SoC, peripheral, model and reference-model work of the other tracks (docs/SOC.md, IO.md, DDR.md, BOARD.md, VERIFICATION.md) | their tracks |

## 1. The core testbench: `build/cpu/surfcpu_tb`

`surfcpu_core` alone (with `RESET_PC = 0x80000000` and RVFI), driven by a C++ harness under
Verilator: `sim/cpu/surfcpu_tb_top.v` brings the ports out, `sim/cpu/surfcpu_tb.cpp` is the
memory, the bus timing, the devices, the ELF loader and the trace writer. Build it with
`make build-cpu` (or `make -C surfcpu build/cpu/surfcpu_tb` inside the container).

```
surfcpu_tb --elf FILE [--trace FILE] [--seed N] [--max-cycles N]
           [--ilat MIN:MAX] [--dlat MIN:MAX] [--signature FILE --sig-begin SYM --sig-end SYM]
           [--vcd FILE] [--rand-reset] [--quiet]
```

| Option | Meaning |
|---|---|
| `--elf FILE` | a 32-bit little-endian RISC-V ELF; its `PT_LOAD` segments (by physical address) must lie in the RAM; it must define `tohost` |
| `--trace FILE` | write the retirement trace (section 2) |
| `--seed N` | seed of every random choice (bus timing, `IRQ_RANDOM`); default 1 |
| `--max-cycles N` | give up after N cycles (exit 2); default 10,000,000 |
| `--ilat MIN:MAX`, `--dlat MIN:MAX` | instruction / data port timing: each request waits 0..(MAX-MIN) cycles for `req_ready`, and its response comes MIN..MAX cycles after acceptance; half of the draws take the shortest value, the rest are uniform. Default 1:1 (no wait states). MIN >= 1 |
| `--signature FILE --sig-begin SYM --sig-end SYM` | at the end (whatever the result) write the words from SYM to SYM (end exclusive), one per line as 8 lowercase hex digits (the riscv-arch-test format) |
| `--vcd FILE` | a waveform of the whole run (large) |
| `--rand-reset` | start every register of the design with a random value (Verilator `randReset(2)`, from `--seed`), to show the reset is complete |
| `--quiet` | no summary line on success |
| `+verilator+...` | passed to Verilator |

The memory map (4 MiB RAM at `0x80000000`, a CLINT at `0x02000000`, the testbench registers
at `0x03000000`, bus errors everywhere else, fetches only from RAM) and the registers are
in [docs/CPU.md](../docs/CPU.md) section 9. Writes take effect, and reads sample, when the
slave answers. `req_ready` is random while `req_valid` is low.

**End of a run.** The program ends by storing to `tohost` (any nonzero value): 1 = pass,
anything else = fail with test number `value >> 1`. The run continues until the RVFI record
of that store, so the trace ends with it.

| Exit | Meaning |
|---|---|
| 0 | pass (`tohost` = 1) |
| 1 | fail (the summary gives the test number) |
| 2 | ran past `--max-cycles` |
| 3 | testbench error: bad arguments or ELF, no `tohost`, a bus protocol violation by the core, or a `tohost` store that never retired |

**Protocol checks.** On both ports, every cycle: no `req_valid` in reset; a request that was
not accepted is offered again unchanged (no withdrawal); no second request before the
previous response (one outstanding); `req_valid` unchanged when `req_ready` changes; fetches
with `write = 0`, `be = 1111`, `wdata = 0`, `fetch = 1` and aligned addresses; data requests
with `fetch = 0` and byte enables matching the address. A violation stops the run (exit 3).

The summary line (standard error) gives cycles, retired records, and per port the number of
requests, the cycles spent waiting for `req_ready` and the error responses.

## 2. The retirement trace

MODULES.md section 5 defines the file: one line per `rvfi_valid`, `#` starts a comment. The
testbench writes two comment lines first (the format, then the ELF, seed and timings).
Fields, space separated, all hexadecimal without `0x` except `order`:

| Field | Format | Meaning |
|---|---|---|
| `order` | decimal | `rvfi_order`, counting records from 0 |
| `pc` | 8 hex digits | `rvfi_pc_rdata` |
| `insn` | 8 hex digits | `rvfi_insn` (a 16-bit encoding has its upper half 0; a failed fetch reads 0) |
| `trap` | 1 digit | `rvfi_trap`: 1 for an instruction that raised an exception |
| `intr` | 1 digit | `rvfi_intr`: 1 for the first record of a trap handler |
| `rd` | 1 or 2 hex digits | `rvfi_rd_addr`, 0 when no register is written |
| `rd_wdata` | 8 hex digits | `rvfi_rd_wdata`, 0 when `rd` is 0 |
| `mem_addr` | 8 hex digits | `rvfi_mem_addr`: the word-aligned address of a load or store, else 0 |
| `rmask` | 1 hex digit | `rvfi_mem_rmask`: lanes read |
| `wmask` | 1 hex digit | `rvfi_mem_wmask`: lanes written |
| `mem_rdata` | 8 hex digits | `rvfi_mem_rdata`: the read lanes in place, other lanes 0 |
| `mem_wdata` | 8 hex digits | `rvfi_mem_wdata`: the written lanes in place, other lanes 0 |
| `pc_next` | 8 hex digits | `rvfi_pc_wdata`: the next pc; for a trap record the handler address |

Conventions (docs/CPU.md section 8): memory fields follow `RISCV_FORMAL_ALIGNED_MEM`
(word-aligned address plus lane masks, as SurfBus works), so a byte store of `0xAB` to
`0x80001001` reads `80001000 0 2 00000000 0000ab00`. An exception gives a record with
`trap` = 1, no register write and no memory access (an access fault had no effect), and
`pc_next` = `mtvec`. An interrupt gives no record; the next record has `intr` = 1, as does the
first record after an exception.

```
# surfcpu-trace v1: order pc insn trap intr rd rd_wdata mem_addr rmask wmask mem_rdata mem_wdata pc_next
0 80000000 00001297 0 0 5 80001000 00000000 0 0 00000000 00000000 80000004
```

## 3. The directed tests

`sim/cpu/tests/*.S` are self-checking programs in the style of riscv-tests, built for
`rv32im_zicsr_zifencei` with `sim/cpu/tests/link.ld` (everything in the RAM from
`0x80000000`). `gen_muldiv.py` and `gen_hazards.py` generate two more into `build/cpu/gen/`.
[docs/CPU.md](../docs/CPU.md) section 12 lists them.

The framework is `sim/cpu/tests/surf_test.h`: `TEST_BEGIN`, a `test_main:` body ending in
`j pass`, checks that branch to `fail` with the check number in `TESTNUM` (`gp`), and
`TEST_DATA` at the end. Its trap handler records `mcause`/`mepc`/`mtval` in `t5`/`t6`/`t4`,
counts traps in `t3` and resumes at `s11` (or after the trapping instruction);
`TRAP_EXPECT(num, cause, instructions)` checks that the instructions trap exactly once with
that cause at their first instruction. Interrupt tests define `TEST_IRQ_HANDLER` and an
`irq_handler`. Registers `x25`..`x31` belong to the framework.

A comment line `// surf-test: spike=no max-cycles=N` in a test sets runner options:
`spike=no` marks tests that need the testbench's devices or check where ISA.md and Spike
differ (reset values, the CSR map, fetching from MMIO).

## 4. Commands

| Command | What it does |
|---|---|
| `make lint-cpu` | Verilator `-Wall` lint of `surfcpu_core` with and without `SURFCPU_RVFI` (on `make lint`) |
| `make test-cpu-tb` | the testbench's own contract: exit codes 0/1/2/3, `--signature` and `--trace` formats (`tb_selftest.py`; on `make test-cpu`) |
| `make test-cpu-directed` | every directed test under 14 bus timings and seeds (`run_tests.py`; on `make test-cpu`) |
| `make test-cpu-riscv-tests` | riscv-tests rv32ui/rv32um/rv32mi "p" tests under three timings (`riscv_tests.py`; on `make test-cpu`). The first run fetches the pinned tarballs (SHA-256 checked) into `build/third_party/riscv-tests/`, so it needs the network once |
| `make test-cpu-icarus` | the twelve directed tests that need only RAM and bus errors on the core under Icarus Verilog (`run_icarus.py`, `tb_core_icarus.v`; on `make test-cpu`) |
| `make test-cpu-spike` | the directed tests on Spike (`--isa=rv32im_zicntr_zicsr_zifencei_zihpm --priv=m --pmpregions=0`), which checks the tests' own expectations |
| `make synth-cpu` | the Yosys `synth_xilinx` resource estimate of the core (on `make check`) |
| `python3 sim/cpu/spike_compare.py ELF -- TB-ARGS` | run one ELF on the testbench and on Spike and compare the trace with Spike's commit log instruction by instruction |
| `python3 sim/cpu/fuzz_spike.py --count N --seed S` | random RV32IM programs compared with Spike under three timings |

`spike_compare.py` and `fuzz_spike.py` are the cpu track's own quick cross-checks; the
official Spike trace converter, comparator and random generator belong to the verif track
(`tools/`, docs/VERIFICATION.md).

To debug a failure: the runner keeps a trace of each failing run in `build/cpu/runs/`; rerun
it with `--vcd FILE` for a waveform, and read the disassembly next to the ELF
(`build/cpu/tests/<test>.dis`).
