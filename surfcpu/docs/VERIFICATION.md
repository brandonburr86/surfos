# SurfCPU verification: checking the core against a reference model

The independent checks that judge SurfCPU (brief section 10, "Reference testing"): every
program runs on the core's testbench and on Spike, the RISC-V reference simulator, and the
two retirement traces are compared instruction by instruction: pc, instruction, trap,
register write, memory access. Directed programs, seeded random programs, a CSR probe and
the riscv-arch-test suite go through the same runner. The machinery was proven first on
Spike against itself and against deliberately broken traces (a Spike stand-in for the
testbench), then run against the real core: the cpu track's `surfcpu_core` and
`build/cpu/surfcpu_tb` as of `surfcpu` `231ad56` (after the cpu review round), built by the
cpu track's own make rule in the tree a merge of this branch would give, and before that the
core of the first cpu merge (`e792960`). Every simulation run passes on both, and 43 of the
44 riscv-formal checks on the current one; [section 12](#12-results) has the numbers.

A pass is not a certification: it means the core and Spike, configured to our contract,
agree on these programs. Simulation only; nothing here touches the board.

| Command (from `surfcpu/`) | What it does | Needs |
|---|---|---|
| `make test-ref` | the machinery's own tests (on `make test-cpu`): unit tests, the CSR probe on both Spikes, the runner against the Spike stand-in (directed, random, riscv-arch-test, CSR probe), the runner's injected-fault self-test, the riscv-formal smoke check | the container; network once (Spike, riscv-arch-test and riscv-formal sources, SHA-256 checked) |
| `make test-cpu-ref` | the comparison that matters: `build/cpu/surfcpu_tb` against Spike, all suites, 50 random seeds, testbench seeds 1,2,3 with `--ilat 1:4 --dlat 1:6` | the core (cpu track) |
| `make ref-spike` / `make ref-arch` | build the contract-patched Spike / the riscv-arch-test programs and their references | |
| `make ref-formal` | riscv-formal on the core (slow: [section 10](#10-riscv-formal)) | the core |
| `python3 tools/rvrun.py --help` | the runner, with every option | |

Variables for `make test-cpu-ref`: `REF_SEEDS` (random programs, default 50),
`REF_DUT_SEEDS` (default `1,2,3`), `REF_ILAT`/`REF_DLAT` (`MIN:MAX` bus latencies, default
`1:4` and `1:6`; empty for the testbench's 1:1),
`REF_DUT` (another testbench command), `REF_JOBS` (default 4). For `make ref-formal`:
`REF_FORMAL_CHECKS`, `REF_FORMAL_TIMEOUT` (seconds), `REF_FORMAL_OPTS`.

## 1. The pieces

| File | Role |
|---|---|
| [`tools/tracefmt.py`](../tools/tracefmt.py) | the trace format of [MODULES.md](MODULES.md) section 5: read, write, annotations, memory fields as byte maps |
| [`tools/rvisa.py`](../tools/rvisa.py) | [ISA.md](ISA.md) as code: which words are legal on SurfCPU, operands, disassembly (checked against objdump on 30,000 random words, both ways), the 251 CSRs of ISA.md section 2 |
| [`tools/rvelf.py`](../tools/rvelf.py) | ELF reader (entry, segments, symbols) |
| [`tools/trace_spike.py`](../tools/trace_spike.py) | runs an ELF on Spike and converts its log into the retirement trace ([section 3](#3-reference-traces-from-spike)) |
| [`tools/trace_compare.py`](../tools/trace_compare.py) | the comparator: first divergence, its kind, context from both traces ([section 4](#4-the-comparison)) |
| [`tools/trace_inject.py`](../tools/trace_inject.py) | damages a trace in 25 ways, for the self-tests |
| [`tools/rvgen.py`](../tools/rvgen.py) | the constrained-random program generator ([section 6](#6-random-programs)) |
| [`tools/rvcsrmap.py`](../tools/rvcsrmap.py) | the CSR probe: all 4096 CSR addresses and the write masks, checked against ISA.md section 2 ([section 5](#5-spike-against-our-contract)) |
| [`tools/rvarch.py`](../tools/rvarch.py) | riscv-arch-test 4.1.0: fetch, scope, build, Spike references ([section 7](#7-riscv-arch-test)) |
| [`tools/rvrun.py`](../tools/rvrun.py) | the runner: a DUT command against Spike over all suites, and its self-test ([section 8](#8-the-runner)) |
| [`tools/rvspike_tb.py`](../tools/rvspike_tb.py) | a stand-in for `build/cpu/surfcpu_tb` made of Spike, with the same command line, that can inject faults |
| [`tools/rvformal.py`](../tools/rvformal.py) | riscv-formal set up for `surfcpu_core` ([section 10](#10-riscv-formal)) |
| [`tools/rvfetch.py`](../tools/rvfetch.py) | pinned source archives (the cpu track's `sim/cpu/riscv_tests.py` pattern): download, SHA-256 check, unpack under `build/third_party/<name>/` |
| [`sim/ref/spike/`](../sim/ref/spike/) | `surfcpu-contract.patch` and `build-spike.sh`: Spike made to take SurfCPU's implementation-defined choices |
| [`sim/ref/directed/`](../sim/ref/directed/) | directed reference programs: `alu`, `muldiv`, `mem`, `branch`, `csr`, `fence`, `traps` |
| [`sim/ref/common/`](../sim/ref/common/) | their environment (`ref_env.h`) and the linker script for the testbench's memory |
| [`sim/ref/arch/`](../sim/ref/arch/) | SurfCPU's riscv-arch-test configuration and the recorded reference signature hashes |
| [`sim/ref/formal/`](../sim/ref/formal/) | the riscv-formal wrapper, SurfBus slave model, configuration and a port-list stand-in |
| [`sim/ref/tests/`](../sim/ref/tests/) | unit tests (`python3 -m unittest discover -s sim/ref/tests`) |
| [`mk/area-verif.mk`](../mk/area-verif.mk) | the make targets |

## 2. The interface with the testbench

The runner drives the testbench exactly as [MODULES.md](MODULES.md) section 6 defines it:

```
surfcpu_tb --elf F --trace T --seed S --max-cycles N [--ilat A:B --dlat A:B]
           [--signature F --sig-begin begin_signature --sig-end end_signature]
```

Exit status 0 means the program stored 1 to `tohost`; 1 another value; 2 `--max-cycles` ran
out; 3 a testbench error (bad arguments or ELF, no `tohost`, a bus protocol violation by the
core, a `tohost` store that never retired). `--ilat`/`--dlat` are passed as given (without
them the testbench uses 1:1), except to the one program marked latency-sensitive
([section 7](#7-riscv-arch-test)); `--max-cycles` is set per program
([section 8](#8-the-runner)). The trace is the text format of MODULES.md section 5, one line
per `rvfi_valid` (`rd` is hex, so x10 is `a`). What the programs assume about the testbench
(docs/CPU.md section 9):

* 4 MiB of RAM at `0x80000000` loaded from the ELF, the core starting there; programs read
  only what the ELF holds or what they wrote.
* The CLINT-compatible timer at `0x02000000` (64 KiB) and the testbench registers at
  `0x03000000` (4 KiB); every other address answers with a bus error. The random programs and
  `traps.S` fault on `0x00000000`, `0x40000000`, `0x7FFFFFFC`, `0x80400000`, `0xF0000000`
  and `0xFFFFFFFC`, none of them in either device (`FAULT_ADDRS` in `tools/rvgen.py`). These
  programs are for the CPU testbench, not the SoC (where 0 is the boot ROM).

## 3. Reference traces from Spike

`tools/trace_spike.py` runs Spike with this configuration (each flag stands for a line of
[ISA.md](ISA.md)):

```
spike --isa=rv32im_zicsr_zifencei_zicntr_zihpm --priv=m --pmpregions=0 --triggers=0
      -m0x80000000:0x400000 -l --log-commits --log=/dev/stdout prog.elf
```

`zicntr`/`zihpm` because the contract has `cycle`, `time`, `instret` and the hpmcounters
(this Spike makes them illegal otherwise); `--priv=m` for M-mode only (no S/U CSRs, `MPP`
fixed at M); no PMP entries; no triggers; the testbench's RAM. Misaligned loads and stores
trap because `--misaligned` is not given. The binary is the contract-patched build of the
container's Spike commit ([section 5](#5-spike-against-our-contract)); every trace says which
Spike made it.

How the log becomes a trace, following the conventions of MODULES.md section 5:

* A commit line is a retired instruction. An `exception` line (with its `tval` line) is a
  trap: a record with `trap = 1`, no register write, no memory access, `pc_next` = the
  handler (`mtvec`), and an annotation `#@ trap cause=.. tval=..` (a comment line the
  comparator uses; other readers ignore it). An interrupt has no record of its own. The
  first record after a trap or an interrupt has `intr = 1`.
* Spike's boot ROM (five instructions at `0x1000`) is dropped: the trace starts at the ELF's
  entry, `0x80000000`, as the testbench's does. (The boot ROM still runs: `--disable-dtb`
  would skip it but also removes the CLINT. It leaves t0, a0 and a1 set and counts in
  `minstret`, so programs initialize every register and write `minstret` before reading it.)
* The trace ends with the first store that makes `tohost` non-zero; Spike notices `tohost`
  only between time slices and runs on for a while, which is dropped.
* `insn` is the 32-bit word in memory at `pc`, as a core without C fetches it; for a
  trapping 16-bit encoding (bits 1:0 not `11`) only its 16 bits, zero-extended, as its
  `mtval` (ISA.md clarification of 2026-10-15). A fetch fault reports 0 (nothing was
  fetched, riscv-formal's rule).
* A shadow memory (the ELF image and every store to RAM) gives each load's `mem_rdata`, also
  for a load into x0, whose value Spike does not print; every other RAM load is checked
  against the value Spike wrote to `rd` (a mismatch stops the conversion). Device loads (the
  CLINT) take their value from `rd`.
* `pc_next` is the pc of the next instruction: the handler's first for a trap, an
  interrupt's `epc` for the record before it.
* Memory fields follow `RISCV_FORMAL_ALIGNED_MEM`: `mem_addr` word aligned, the masks select
  byte lanes, data sits in its lanes and unused lanes are 0.

The log is read from a pipe while Spike runs: Spike's own `--instructions` limit counts
5000-instruction time slices that every trap ends early, so it cuts trap-heavy programs
short; instead a run stops after 4 million records or a time limit.

## 4. The comparison

`tools/trace_compare.py REF DUT --elf PROG.elf` compares record by record and prints the
first divergence with context from both traces (and `--json` for tools):

```
MISMATCH rd at retirement 16: x17 = 00000000 expected, x17 = 00010000 retired
  context (index, pc, insn, disassembly, effects):
         13  80000034 00000613 addi x12,x0,0      x12=00000000 -> 80000038
     ...
  >>     16  ref: 80000040 00000893 addi x17,x0,0 x17=00000000 -> 80000044
             dut: 80000040 00000893 addi x17,x0,0 x17=00010000 -> 80000044
```

Kinds: `order` (rvfi_order skips or repeats), `pc`, `insn`, `trap`, `intr`, `rd` (number or
value; a write by a trapping instruction; a value with rd = x0), `mem_addr`, `mem_mask`
(which bytes; an access missing or extra, also any access by a trapping instruction),
`mem_data`, `mem_layout` (the right bytes, but fields that break the aligned convention),
`pc_next`, `missing` (the core retired fewer: its trace ends early, or it skipped one: a
look-ahead recognizes the reference's next record), `extra` (it retired more: a longer
trace, or an inserted or repeated record).

The rules, all from the trace conventions of MODULES.md section 5 and RVFI:

| Case | Rule |
|---|---|
| memory fields | compared first as maps of byte address to value (an architectural verdict: which bytes, which values), then field by field: `mem_addr` word aligned, the masks exactly the lanes accessed, unused lanes 0 |
| reads and writes | exactly the bytes the instruction loads or stores |
| trap records | no register write, no memory access (read or write), `pc_next` = the handler |
| `intr` | 1 on the first record after any trap and on an interrupt handler's first, 0 elsewhere |
| a trapping 16-bit encoding | `insn` is its 16 bits, zero-extended |
| fetch fault | `insn` is 0 |
| `rd` = x0 | `rd_wdata` is 0 |
| `rvfi_order` | consecutive; any start value (noted) |
| end of run | both traces end at the first store of non-zero data to `tohost`, later records are ignored, and the core's trace must contain that store |
| `#@ mem rdata=unknown` | that load's data is not compared (a device load into x0; never in our programs) |

Two options relax a rule for other RVFI producers, never for the CPU testbench:
`--lenient-layout` compares memory as bytes only and lets a load report more bytes than it
needs (riscv-formal's default layout and superset rule); `--lenient-final-store` accepts a
trace that stops just before the final `tohost` store.

The self-tests (`sim/ref/tests/test_trace_compare.py`) inject every kind of damage
(`tools/trace_inject.py`, 25 mutations: wrong pc, flipped instruction bit, trap added or
removed, intr set, wrong rd value or number, a value with rd = x0, a trapping instruction
writing a register, storing or reporting a read, store address/mask/data, load
address/data/unreported, the default memory layout, a byte load reported as a word read,
wrong pc_next, a trap's pc_next not the handler, a retirement dropped or repeated, an order
gap, a truncated trace) into a real Spike trace at several places, and check the comparator
names the right kind at the right record; and that the reference itself follows every
convention, unused lanes must be 0, records after `tohost` are ignored and an order offset
passes.

## 5. Spike against our contract

Where the RISC-V specification lets an implementation choose, ISA.md has made SurfCPU's
choice; Spike sometimes chose differently. `tools/rvcsrmap.py` probes all 4096 CSR addresses
(read), writes four patterns to every CSR of the contract and to 20 it does not have (and
reads them back), and checks which CSR instructions count as writes: 5212 probes, compared
with ISA.md section 2 (the same check runs on the core's trace: `rvrun.py --suites csrmap`).
On the container's stock Spike, configured as above, 423 probes differ, all in these known
ways (`make test-ref` fails if that list changes):

| Case | ISA.md (SurfCPU) | Stock Spike | Handling |
|---|---|---|---|
| `marchid` | 0 | 5 (Spike's registered id) | patched |
| `mimpid` | `0x00000100` (SoC 0.1.0) | 0 | patched |
| `pmpcfg0-15`, `pmpaddr0-63` | exist, read as zero, writes ignored | illegal with `--pmpregions=0` | patched |
| `tselect`, `tdata1-3`, `tinfo`, `mcontext` | illegal (no triggers) | present even with `--triggers=0` | patched |
| `misa` writes | ignored | can clear M, A, F, D, C | patched |
| `mtvec.MODE` | fixed at 0 (direct) | vectored mode sticks | patched |
| `mcountinhibit` | only CY and IR writable | HPM bits writable (with Zihpm) | patched |
| `mcause` | bit 31 and 4:0 writable (WLRL) | every bit | patched; the core keeps the same bits |

`sim/ref/spike/surfcpu-contract.patch` makes Spike take SurfCPU's choice in each of these
cases and changes nothing else (instruction semantics, traps and every other CSR stay
Spike's); `sim/ref/spike/build-spike.sh` downloads the container's pinned commit
(`5a760667`, SHA-256 checked), applies it and builds `build/third_party/spike-surfcpu`
(about four minutes, once). On the patched Spike all 5212 probes agree with ISA.md; so do
they on the core.

Other places where the reference and the contract part ways, and how each is handled:

| Case | ISA.md / testbench | Spike | Handling |
|---|---|---|---|
| mtval of an illegal 16-bit encoding | its 16 bits, zero-extended (clarification of 2026-10-15) | the same | agree; random programs use 16-bit encodings with any upper half |
| 48/64-bit encodings (`0xFFFFFFFF`) | illegal, mtval = the word | fetches 8 bytes; mtval = low 32 bits | agree, except at the end of mapped memory (Spike would take a fetch fault): programs never put one there |
| misaligned loads/stores | trap (cause 4/6, mtval = address) | trap without `--misaligned` | agree |
| misaligned jump targets | trap on the jump, rd not written | same (IALIGN 32 without C) | agree |
| access-fault priority | misaligned before access fault | same | agree |
| boot ROM | the core starts at `0x80000000` | five ROM instructions first | dropped from the trace; minstret written before use (see section 3) |
| memory map | RAM + CLINT + testbench registers | also a debug ROM at 0, boot ROM at `0x1000`, PLIC, a UART at `0x10000000` | programs touch only RAM and the fault addresses of section 2 |
| `mcycle`, `cycle`, `time`, `mip` | count clock cycles / follow the timer | count instructions, tick per time slice; MTIP set at reset | never read by random or directed programs; arch tests that read them are judged by their self-check, not by trace |
| `minstret` counting | a trapping instruction does not retire; a CSR write to it is not counted | the same (checked by `directed/csr.S`) | agree; the start value differs by the boot ROM |
| `minstreth` written by a CSR instruction | ISA.md says only that a write to `minstret` is not counted | Spike also drops the increment on a `minstreth` write | random programs never write `minstreth`; [contract question 1](#11-proposed-contract-changes) |
| WFI | waits for `mip & mie` | waits; interrupts only between time slices | never used (timing) |
| interrupts | taken between instructions | only at time-slice boundaries | never enabled (`mie` stays 0) |
| reset values | listed in ISA.md section 2 | `mstatus` 0x1800 with `--priv=m`, others 0 | programs write what they read (the probe checks reset values on the core) |
| FENCE/FENCE.I fields | any pred/succ/fm, ignored rs1/rd | the same | agree (`fence.S`, random FENCE words) |
| self-modifying code | FENCE.I makes stores visible | Spike flushes its decode cache on FENCE.I | programs always use FENCE.I |

## 6. Random programs

`tools/rvgen.py --seed N` writes a self-contained program (its own SplitMix64 random numbers
with the seed mixed in: the same seed gives the same program on any Python, neighbouring
seeds unrelated ones). Body items, by weight: ALU register and immediate operations with
corner values and immediates at both ends; LUI/AUIPC; M operations, also with special
operands (0, -1, the extremes); loads and stores of every width and sign in a 4 KiB data area
(base x3), also through pointers made just before; forward branches over blocks (taken and
not); bounded loops (1-4 iterations); JAL/JALR forwards (links, JALR bit 0); calls to leaf
functions; CSR operations where Spike and the contract agree; FENCE (any fields), FENCE.I,
hints; MRET to a label; and deliberate traps: ECALL, EBREAK, illegal words (contract cases,
16-bit encodings with any upper half, random words with undefined fields), illegal CSR
accesses, misaligned accesses and jump targets, load/store/fetch access faults. Sources come
mostly from the last four registers written (dependences at distance 1-4); x0 appears as
source and destination. The handler (x30 and x31 only) reads mtval, mepc and mcause
(compared through the trace) and resumes at mepc + 4, or at ra after a fetch fault. The end
stores all 31 registers into the signature (the data area plus the register dump), then 1 to
`tohost`.

Never: WFI, interrupts, `mcycle`/`cycle`/`time`/`mip`, reads of `marchid`/`mimpid` (only
trapping writes), `pmp*`, trigger CSRs, writes to `misa` or `minstreth`, `mtvec` MODE bits,
stores outside the data area (`test_rvgen.py` checks the list in generated programs). On
Spike, the tests check that every illegal word traps with mtval equal to the word (16 bits
for a 16-bit encoding), that every fault address faults for loads, stores and fetches, and
that programs pass with traps of all nine causes. A 3000-instruction program retires about
5,000 instructions with about 250 traps; the 1000 programs of seeds 1001-2000 retired
5,120,980 on the core, with 252,025 traps of all nine causes and 55 distinct instructions
([`random.txt`](evidence/verif/random.txt)).

## 7. riscv-arch-test

**Version: riscv-arch-test 4.1.0** (ACT4, the current Architectural Certification Tests;
tag 4.1.0 = commit `6e8a45123f14`, tarball SHA-256 `aa00df82…58bb`). The 3.x line is the
old riscof framework; 4.x has more thorough unprivileged tests (every register as source and
destination, corner values), Zicsr tests, and generated machine-mode privileged tests. ACT4
supports Spike as the reference model. Its own framework needs Sail, Ruby and the UDB gem,
which the container does not have, so `tools/rvarch.py` does what the framework does for our
one configuration (`sim/ref/arch/act4/`: `rvtest_config.h` written as UDB would generate it
for ISA.md, `rvmodel_macros.h`, `link.ld`; `surfcpu.yaml` is the same configuration in UDB's
language, not validated):

1. the unprivileged tests ship as assembly; the machine-mode privileged ones are generated
   with the suite's own generator (`generators/testgen`, standard library only);
2. each test in scope is compiled in signature mode and run on Spike: the reference
   signature (its SHA-256 is recorded in `sim/ref/arch/reference.sha256`; `rvarch.py check`
   fails on any change of suite, Spike, toolchain or configuration);
3. the signature is compiled back into the test (ACT4's self-check mode): an ELF that stores
   1 to `tohost` only if every result matches Spike's;
4. that ELF must pass on Spike; the runner runs it on the core and compares exit status and,
   unless Spike's run of it reads `cycle`, `time`, `mip` or `instret` into a register, the
   whole retirement trace.

Scope (`python3 tools/rvarch.py list` prints the full list with reasons):

| In scope: 177 tests | Count | What they check |
|---|---|---|
| `rv32i/I` | 39 | every RV32I instruction (with FENCE and NOP), every register as source and destination, corner values |
| `rv32i/M`, `rv32i/Zmmul` | 8 + 4 | MUL to REMU (M includes Zmmul, whose 4 tests repeat the multiplies) |
| `rv32i/Zicsr` | 6 | the six CSR instructions |
| `rv32i/Zifencei` | 1 | FENCE.I with self-modifying code |
| `rv32i/Zicntr` | 2 | cycle, time and instret readable and counting (self-check only: timing) |
| `rv32i/Zihpm` | 2 | the hpmcounters (hardwired to zero on SurfCPU, which Zihpm allows) |
| `rv32i/Zihintpause` | 1 | PAUSE (a FENCE encoding) |
| `priv/ExceptionsSm` (generated) | 4 | every M-mode exception with mcause, mepc and mtval, their priorities, access faults at `RVMODEL_ACCESS_FAULT_ADDRESS` (0); the `medeleg` variants run what applies without S/U |
| `priv/Sm` (generated) | 24 | M-mode CSRs: read-only ones, write masks (walking ones and zeros), counters, `misa`, `mcause`, `mstatus.SD`, xRET |
| `priv/SsstrictSm` (generated) | 86 | every unimplemented CSR (31 tests), illegal opcode (26) and vector instruction (29) must trap |

| Out of scope: 957 tests looked at | Count | Why |
|---|---|---|
| F, D, Zfh, Zfa, A (Zaamo, Zalrsc, Zabha, Zacas), B (Zba, Zbb, Zbc, Zbs), Zbk*, Zk*, C (Zca, Zcb, Zcd, Zcf, Zcmop), Zicond, Zicbo*, Zihintntl, Zimop | 573 | SurfCPU does not implement them (ISA.md section 1) |
| Sv*, Svade, Svadu, Svbare, Svinval, Svnapot, Svpbmt, ExceptionsSv*, and the S- and U-mode PMP tests | 279 | need S-mode, U-mode or virtual memory: SurfCPU is M-mode only |
| PMPSm, PMP with extensions | 82 | need PMP entries (`NUM_PMP_ENTRIES > 0`) or RV64; SurfCPU has none |
| `SsstrictSm_CompressedInstr` | 17 | a suite bug for cores without C: its handler resumes a 16-bit encoding at mepc + 2, which IALIGN = 32 cannot hold, so the test loops (on Spike too); `-04` also fails to link |
| `rv32i/Misalign` | 5 | need misaligned loads and stores in hardware; SurfCPU traps (ExceptionsSm and our own programs test the traps) |
| `InterruptsSm` | 1 | raises external interrupts through `RVMODEL_SET_MEXT_INT`: the testbench has a register for it (`IRQ_EXT`, docs/CPU.md section 9) but Spike has not, so Spike cannot make its reference signature; its timer interrupts also depend on mtime's rate |
| `rv64i`, `rv64e`, `rv32e`; privileged generators for S, U, H, F, V, Sdtrig, Smstateen, Zawrs, Zkr, Zicbo | not unpacked | other base ISAs and extensions |

On Spike all 177 build and pass their self-check: 2,451,871 retirements, the longest tests
(`Sm_csr_ro-00` and `-01`) 83,362 each. 167 are also compared by trace; 10 read `cycle`,
`mcycle`, `mcycleh`, `cycleh` or `mip` and are judged by their self-check (listed in
`build/ref/arch/manifest.json`). 32 of the 177 reference signatures (`Sm_mcsr_access-00`
and the 31 `SsstrictSm_CSR` tests) come out different on the stock Spike: without the
contract patch they would expect Spike's choices, not ISA.md's.

One test assumes a fast core: `Sm_mcsr_cntr-00` sets `mcycle` to all ones, runs a
300-instruction loop and requires `mcycle` < 1000 afterwards. The core needs about 1200
cycles for that loop with `--ilat 1:4 --dlat 1:6`, so the runner gives this test the
testbench's default 1:1 latencies whatever `--ilat`/`--dlat` say (the report notes it).

## 8. The runner

`tools/rvrun.py` gives one verdict per (program, DUT seed): PASS only if the exit status
matches Spike's (0), the trace matches (unless the program is marked timing-dependent), the
signature matches (directed and random programs), and for `csrmap` the core's CSR behaviour
matches ISA.md. Suites: `directed`, `random` (`--seeds N` or `--seed-list`), `arch`,
`csrmap`, and any `--elf FILE`. `--json` writes every outcome; `--coverage` adds what
Spike's traces exercised (traps by cause, mnemonics, branches) per suite.

It is built to run thousands of programs on a small disk:

* Each program runs once on Spike; its trace stays in memory and is never written. All DUT
  seeds of the program run against it.
* A passing run leaves nothing: the DUT's trace, signature and output are deleted, and so is
  a passing random program's generated source and ELF (`--keep-programs` keeps them).
* A failing run leaves `<out>/<suite>/<prog>.s<seed>.report.txt` (the verdicts, the
  comparator's report with context, the testbench's last output lines), the DUT's signature,
  and both traces (`.dut.trace`, `.ref.trace`) cut to 20,000 records before the divergence
  and 2,000 after it.
* Every DUT run is bounded. `--max-cycles` defaults to (64 + twice the two maximum
  latencies) cycles per Spike retirement plus 200,000: 68 at 1:1, 184 at 6:30, where the core
  needed at most 5.4 and 22.7 (measured on eight programs from every suite). A DUT whose
  trace grows past 200 bytes per Spike record (the testbench writes about 85) is stopped and
  reported as `runaway`; at most Spike's records + 2,016 of its trace are read. `--timeout`
  stops a run after 900 s.

`python3 tools/rvrun.py selftest` runs the runner itself against the Spike stand-in in 36
behaviours on three programs (two directed, one random): clean, the default memory layout
with `--lenient-layout`, no final tohost store with `--lenient-final-store` (these must
pass); the default layout and the missing store without those options, each of the 25 trace
faults, a wrong signature, exit 1, 2, 3, a crash, and a runaway (each must fail with the
right kind). For each it also checks what the run left on disk (nothing after a pass, a
report and bounded traces after a failure); then three DUT seeds of one program, failing
(each keeps its own files) and passing (none left), and a passing random program kept with
`--keep-programs` and removed without: 112 checks.

## 9. For the integrator

* Build the core's testbench, then `make test-cpu-ref` (or `python3 tools/rvrun.py --dut
  build/cpu/surfcpu_tb --dut-seeds 1,2,3 --seeds 50 --ilat 1:4 --dlat 1:6`).
* The conventions this machinery relies on, all in MODULES.md sections 5 and 6 and
  docs/CPU.md section 9: the aligned memory fields with zero unused lanes; a trap record
  with no register write, no memory access and `pc_next` = `mtvec`; no record for an
  interrupt and `intr` = 1 on the first record after any trap or interrupt; the trace ending
  with the `tohost` store; `insn` 0 on a fetch fault and the 16 bits of a 16-bit encoding;
  exit codes 0-3; the memory map (RAM, CLINT, testbench registers, bus errors elsewhere).
  A change to any of them shows as a failure of a known kind.
* A failure report shows the first divergence: start there; `trace_compare.py REF DUT
  --context 40` on the kept traces gives more context.
* The random suite takes any number of seeds (`--seeds 1000`); disk use stays flat.

## 10. riscv-formal

`tools/rvformal.py` sets up riscv-formal (commit `c992aa61`, SHA-256 checked) for
`surfcpu_core`: `sim/ref/formal/wrapper.sv` puts the core's SurfBus ports on
`surfbus_formal_slave.sv`, which keeps every slave rule of SURFBUS.md section 2 and leaves
acceptance, response time and read data to the solver, and asserts the master rules (no
withdrawn or changed request, one outstanding). Bus errors come exactly for addresses from
`0xF0000000` up, which `checks.cfg` declares invalid (`RISCV_FORMAL_VALIDADDR`), so the
instruction checks expect an access fault there and none elsewhere: the core's RVFI has no
`rvfi_mem_fault` signals, without which riscv-formal cannot accept a bus error at an
arbitrary address. `checks.cfg` asks for the RV32I instruction checks with the aligned
memory layout, and the pc, register, uniqueness, causality, liveness and
illegal-instruction checks: 44 checks (the eight M instruction checks need riscv-formal's
alternative operations, `RISCV_FORMAL_ALTOPS`, which the core does not implement; `--altops`
puts them back).

The solver is ABC's `bmc3`: it proves each instruction check on the core in 5 to 21 seconds,
where z3 did not finish `insn_lw` in 55 minutes; the container's boolector crashes in
yosys-smtbmc with and without unrolling. `rvformal.py` removes sby's `skip` option for it
(the abc engine does not take it; riscv-formal asserts only in the last step anyway) and
replays a failure's trace with z3 (sby's default, yices, is not in the container).

On the current core (`surfcpu` `231ad56`, [`formal.txt`](evidence/verif/formal.txt)) 43 of
the 44 checks pass at their depths in 17 minutes with 4 jobs: 21 frames for the instruction
and illegal-instruction checks (5 to 22 s each), 26 for causality, uniqueness and pc_bwd, 31
for liveness, and 21 for pc_fwd (17 minutes; at 26 frames it did not finish in 40). The
44th, `reg_ch0`, did not finish on the `e792960` core (40 minutes at 26 frames, 25 at 21) and
was not run again. These are bounded checks: no counterexample within that many cycles after
reset, for any bus timing, read data and interrupt input the model allows. The first run, with a slave
that raised errors anywhere, failed `insn_lw`: the core took a load access fault on an
error, as it must, where riscv-formal expected the load to complete. That is how the
address rule came about, and putting random errors back makes `insn_lw` and `insn_sw` fail
again within a second, so these checks do see the bus.

`rvformal.py smoke` (run by `make test-ref`) elaborates the wrapper with a stand-in that has
exactly ISA.md's port list, generates the checks into a directory of its own
(`cores/surfcpu_stub`) and runs one through yosys, sby and z3 (vacuous on the stand-in,
which retires nothing). On the core: `rvformal.py gen [--root TREE] [--solver S] [--depth
NAME=N]` and `rvformal.py run -j 4 [--timeout S] [CHECK...]`, or `make ref-formal
REF_FORMAL_TIMEOUT=3600` (`reg_ch0` will run until the limit).

## 11. Proposed contract changes

1. **Writing `minstreth`.** ISA.md says a CSR instruction that writes `minstret` is not
   counted, but not what happens on a `minstreth` write. Proposal: the same (Spike and the
   specification's "the write takes effect after the instruction" reading agree). Random
   programs never write `minstreth` until this is settled.
2. **`mcause` WLRL.** The table says bits 31 and 4:0 are writable and the note says only
   legal values need to round-trip; the core and the patched reference keep exactly bits 31
   and 4:0, so they agree. Proposal: say "other values read back masked to bits 31 and 4:0",
   which is what both do.
3. **Formal coverage of bus errors.** With riscv-formal's `rvfi_mem_fault`,
   `rvfi_mem_fault_rmask` and `rvfi_mem_fault_wmask` in `surfcpu_rvfi.vh`
   (`RISCV_FORMAL_MEM_FAULT`), the formal slave could answer any request with an error. Not
   needed for simulation, which covers access faults at the six fault addresses.
4. **A store of 0 to `tohost`.** MODULES.md section 6 says "1 = pass; any other value =
   fail", which reads as if 0 failed; the testbench, Spike's HTIF and the comparator all
   ignore a store of 0 and end at the first non-zero one. Proposal: "the program ends with
   its first non-zero store to `tohost`".

Settled by the 2026-10-15 clarifications (no longer open): mtval and `insn` of an illegal
16-bit encoding (ISA.md section 6), the trace's memory, trap and interrupt conventions and
`rd` in hex (MODULES.md section 5), the testbench's exit code 3 and latency ranges
(section 6).

Found in riscv-arch-test 4.1.0 (not our contract): `SsstrictSm_CompressedInstr` cannot pass
on a core without C (section 7), and `Sm_mcsr_cntr-00` assumes fewer than 3.3 cycles per
instruction.

## 12. Results

Evidence with commands, seeds and counts: [`docs/evidence/verif/`](evidence/verif/).

| Run | DUT | Result |
|---|---|---|
| `make test-ref` | Spike stand-in | 47 unit tests; CSR probe: patched Spike 0 of 5212 probes differ, stock Spike 423, all known; 177 arch tests built, 177/177 reference hashes as recorded; arch 177, csrmap 1, directed 7, random 12 pass (2,549,696 retirements); self-test 112/112; formal smoke |
| `make test-cpu-ref`: all suites, 50 random seeds, DUT seeds 1,2,3, `--ilat 1:4 --dlat 1:6` | the core | arch 531, csrmap 3, directed 21, random 150 pass; 8,196,852 retirements |
| all suites, 50 random seeds, DUT seed 1, default 1:1 | the core | arch 177, csrmap 1, directed 7, random 50 pass; 2,732,284 retirements |
| all suites, random seeds 51-150, DUT seed 7, `--ilat 6:30 --dlat 6:30` | the core | arch 177, csrmap 1, directed 7, random 100 pass; 2,984,569 retirements |
| random seeds 1001-2000, DUT seed 1, `--ilat 1:4 --dlat 1:6` | the core | 1000 pass; 5,120,980 retirements |
| riscv-formal, ABC bmc3 (`checks.cfg` as committed) | the core | 43 of 44 checks pass in 17 minutes (4 jobs); `reg_ch0` did not finish in 40 minutes (on the `e792960` core) |

"The core" is the cpu track's `surfcpu_core` in `build/cpu/surfcpu_tb` at `surfcpu`
`231ad56`, built by `make test-cpu-ref` itself in the tree a merge of this branch would give
(`git merge-tree`, exported; nothing was merged); the same simulation runs passed before on
the `e792960` core. [`core-results.txt`](evidence/verif/core-results.txt) has the builds and
the cycle measurements. The four simulation runs together: 2,225 runs, 19,034,685
retirements, no failure, in under 4 minutes with 3 jobs, leaving under 2 MB on disk.

Pending: InterruptsSm (needs a reference that models the testbench's interrupt register),
riscv-formal's `reg_ch0` (a longer run or another solver) and its M instruction checks (need
`RISCV_FORMAL_ALTOPS` in the core), and anything on the board.
