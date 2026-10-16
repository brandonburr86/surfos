# SurfCPU verification: checking the core against a reference model

The independent checks that judge SurfCPU (brief section 10, "Reference testing"): every
program runs on the core's testbench and on Spike, the RISC-V reference simulator, and the
two retirement traces are compared instruction by instruction: pc, instruction, trap,
register write, memory access. Directed programs, seeded random programs and the
riscv-arch-test suite go through the same runner. Everything here was proven on Spike
against itself and against deliberately broken traces, because the core is being written at
the same time; [section 9](#9-for-the-integrator) says how to point it at the real one.

A pass is not a certification: it means the core and Spike, configured to our contract,
agree on these programs. Simulation only; nothing here touches the board.

| Command (from `surfcpu/`) | What it does | Needs |
|---|---|---|
| `make test-ref` | the machinery's own tests (on `make test-cpu`): unit tests, the CSR probe on both Spikes, the runner against a Spike stand-in for the testbench (directed, random, riscv-arch-test), and the runner's injected-fault self-test | the container; network once (Spike and riscv-arch-test sources, SHA-256 checked) |
| `make test-cpu-ref` | the comparison that matters: `build/cpu/surfcpu_tb` against Spike, all suites, 50 random seeds, testbench seeds 1,2,3 | the core (cpu track) |
| `make ref-spike` / `make ref-arch` | build the contract-patched Spike / the riscv-arch-test programs and their references | |
| `python3 tools/rvrun.py --help` | the runner, with every option | |

Variables for `make test-cpu-ref`: `REF_SEEDS` (random programs, default 50),
`REF_DUT_SEEDS` (default `1,2,3`), `REF_ILAT`/`REF_DLAT` (`MIN:MAX` bus latencies),
`REF_DUT` (another testbench command), `REF_JOBS` (default 4).

## 1. The pieces

| File | Role |
|---|---|
| [`tools/tracefmt.py`](../tools/tracefmt.py) | the trace format of [MODULES.md](MODULES.md) section 5: read, write, annotations, memory fields as byte maps |
| [`tools/rvisa.py`](../tools/rvisa.py) | [ISA.md](ISA.md) as code: which words are legal on SurfCPU, operands, disassembly (checked against objdump on 30,000 random words, both ways), the CSR table of ISA.md section 2 |
| [`tools/rvelf.py`](../tools/rvelf.py) | ELF reader (entry, segments, symbols) |
| [`tools/trace_spike.py`](../tools/trace_spike.py) | runs an ELF on Spike and converts its log into the retirement trace ([section 3](#3-reference-traces-from-spike)) |
| [`tools/trace_compare.py`](../tools/trace_compare.py) | the comparator: first divergence, its kind, context from both traces ([section 4](#4-the-comparison)) |
| [`tools/trace_inject.py`](../tools/trace_inject.py) | damages a trace in 21 ways, for the self-tests |
| [`tools/rvgen.py`](../tools/rvgen.py) | the constrained-random program generator ([section 6](#6-random-programs)) |
| [`tools/rvcsrmap.py`](../tools/rvcsrmap.py) | the CSR probe: all 4096 CSR addresses and the write masks, checked against ISA.md section 2 ([section 5](#5-spike-against-our-contract)) |
| [`tools/rvarch.py`](../tools/rvarch.py) | riscv-arch-test 4.1.0: fetch, scope, build, Spike references ([section 7](#7-riscv-arch-test)) |
| [`tools/rvrun.py`](../tools/rvrun.py) | the runner: a DUT command against Spike over all suites, and its self-test ([section 8](#8-the-runner)) |
| [`tools/rvspike_tb.py`](../tools/rvspike_tb.py) | a stand-in for `build/cpu/surfcpu_tb` made of Spike, with the same command line, that can inject faults |
| [`tools/rvformal.py`](../tools/rvformal.py) | riscv-formal set up for `surfcpu_core`, ready to run ([section 10](#10-riscv-formal)) |
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
out. The trace is the text format of MODULES.md section 5, one line per `rvfi_valid`
(note that `rd` is hex, so x10 is `a`). What the programs assume about the testbench:

* 4 MiB of RAM at `0x80000000`, the core starting there; RAM outside the ELF reads as zero at
  the start (as Spike's does), or programs read only what they wrote (all of ours do).
* Every other address answers with a bus error, except the CLINT-compatible timer at
  `0x02000000` and the testbench's own registers. The random programs and `traps.S` fault on
  `0x00000000`, `0x40000000`, `0x7FFFFFFC`, `0x80400000`, `0xF0000000` and `0xFFFFFFFC`: if
  docs/CPU.md puts a register at one of them, change `FAULT_ADDRS` in `tools/rvgen.py` (and
  `traps.S`). These programs are for the CPU testbench, not the SoC (where 0 is the boot ROM).

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

How the log becomes a trace:

* A commit line is a retired instruction. An `exception` line (with its `tval` line) is a
  trap: a record with `trap = 1`, no register write, no store, and an annotation
  `#@ trap cause=.. tval=..` (comment lines the comparator uses; other readers ignore them).
  The record after a trap or an interrupt has `intr = 1`, RVFI's first instruction of a
  handler.
* Spike's boot ROM (five instructions at `0x1000`) is dropped: the trace starts at the ELF's
  entry, `0x80000000`, as the testbench's does. (The boot ROM still runs: `--disable-dtb`
  would skip it but also removes the CLINT. It leaves t0, a0 and a1 set and counts in
  `minstret`, so programs initialize every register and write `minstret` before reading it.)
* The trace ends with the first store that makes `tohost` non-zero; Spike notices `tohost`
  only between time slices and runs on for a while, which is dropped.
* `insn` is the 32-bit word in memory at `pc`, as a core without C fetches it, also for a
  trapping 16-bit encoding (Spike's log shows only its 16 bits). A fetch fault reports 0
  (riscv-formal's rule: nothing was fetched).
* A shadow memory (the ELF image and every store to RAM) gives each load's `mem_rdata`, also
  for a load into x0, whose value Spike does not print; every other RAM load is checked
  against the value Spike wrote to `rd` (a mismatch stops the conversion). Device loads (the
  CLINT) take their value from `rd`.
* `pc_next` is the pc of the next instruction: the handler's first for a trap, an
  interrupt's `epc` for the record before it.
* Memory fields use riscv-formal's default layout (`mem_addr` is the first byte; masks and
  data start at bit 0).

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
(which bytes; an access missing or extra), `mem_data`, `pc_next`, `missing` (the core
retired fewer: its trace ends early, or it skipped one: a look-ahead recognizes the
reference's next record), `extra` (it retired more: a longer trace, or an inserted or
repeated record).

The rules where RVFI leaves room:

| Case | Rule | Why |
|---|---|---|
| memory layout | compared as maps of byte address to value, so riscv-formal's default layout and `RISCV_FORMAL_ALIGNED_MEM` both work | MODULES.md does not fix it; SurfBus is word-aligned with byte lanes |
| reads | a load may report more bytes than it needs, with the needed ones right | riscv-formal allows wider reads (`rmask` a superset); SurfBus memories return whole words |
| writes | exactly the bytes stored | a wider write would be a real bug |
| trap records | no register write and no store; reads, `mem_addr` and `pc_next` not compared | riscv-formal: a trapping instruction has no rd or memory write; its `pc_wdata` is open (the handler's address is checked as the next record's pc) |
| a trapping 16-bit encoding | `insn` may be the 32-bit word or its low 16 bits | RVFI zero-extends instructions shorter than ILEN; a core without C fetches 32 bits |
| fetch fault | `insn` must be 0 | riscv-formal's `bus_imem_fault` rule |
| `rvfi_order` | consecutive; any start value (noted) | RVFI: no gaps, no repeats |
| end of run | both traces end at the first store that writes `tohost`; later records are ignored; the core's trace may stop just before that store (a testbench that ends on the bus write never logs it) unless `--strict-final-store` | the testbench's exit status reports the value anyway |
| `#@ mem rdata=unknown` | that load's data is not compared | a device load into x0 (never in our programs) |

The self-tests (`sim/ref/tests/test_trace_compare.py`) inject every kind of damage
(`tools/trace_inject.py`: 21 mutations: wrong pc, flipped instruction bit, trap added or
removed, intr set, wrong rd value or number, a value with rd = x0, a trapping instruction
writing a register or storing, store address/mask/data, load address/data/unreported, wrong
pc_next, a retirement dropped or repeated, an order gap, a truncated trace) into a real Spike
trace at several places, and check the comparator names the right kind at the right record;
and that the allowed variations above (aligned layout, wider reads, trap pc_next, low-half
short encodings, missing final store, records after tohost, an order offset) pass.

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
| `mcause` | bit 31 and 4:0 writable (WLRL) | every bit | patched (legal values agree anyway) |

`sim/ref/spike/surfcpu-contract.patch` makes Spike take SurfCPU's choice in each of these
cases and changes nothing else (instruction semantics, traps and every other CSR stay
Spike's); `sim/ref/spike/build-spike.sh` downloads the container's pinned commit
(`5a760667`, SHA-256 checked), applies it and builds `build/third_party/spike-surfcpu`
(about four minutes, once). On the patched Spike all 5212 probes agree with ISA.md.

Other places where the reference and the contract part ways, and how each is handled:

| Case | ISA.md / testbench | Spike | Handling |
|---|---|---|---|
| mtval of an illegal 16-bit encoding | "the instruction bits" (ambiguous: 16 or 32?) | the 16-bit parcel, zero-extended | random programs use 16-bit encodings only with a zero upper half, where both readings agree; **contract question 1** ([section 11](#11-proposed-contract-changes)) |
| 48/64-bit encodings (`0xFFFFFFFF`) | illegal, mtval = the word | fetches 8 bytes; mtval = low 32 bits | agree, except at the end of mapped memory (Spike would take a fetch fault): programs never put one there |
| misaligned loads/stores | trap (cause 4/6, mtval = address) | trap without `--misaligned` | agree |
| misaligned jump targets | trap on the jump, rd not written | same (IALIGN 32 without C) | agree |
| access-fault priority | misaligned before access fault | same | agree |
| boot ROM | the core starts at `0x80000000` | five ROM instructions first | dropped from the trace; minstret written before use (see section 3) |
| memory map | RAM + CLINT + testbench registers | also a debug ROM at 0, boot ROM at `0x1000`, PLIC, a UART at `0x10000000` | programs touch only RAM and the fault addresses of section 2 |
| `mcycle`, `cycle`, `time`, `mip` | count clock cycles / follow the timer | count instructions, tick per time slice | never read by random or directed programs; arch tests that read them are judged by their self-check, not by trace |
| `minstret` counting | a trapping instruction does not retire; a CSR write to it is not counted | the same (checked by `directed/csr.S`) | agree; the start value differs by the boot ROM |
| `minstreth` written by a CSR instruction | ISA.md says only that a write to `minstret` is not counted | Spike also drops the increment on a `minstreth` write | random programs never write `minstreth`; **contract question 2** |
| WFI | waits for `mip & mie` | waits; interrupts only between time slices | never used (timing) |
| interrupts | taken between instructions | only at time-slice boundaries | never enabled (`mie` stays 0) |
| reset values | listed in ISA.md section 2 | `mstatus` 0x1800 with `--priv=m`, others 0 | programs write what they read (the probe checks reset values on the core) |
| FENCE/FENCE.I fields | any pred/succ/fm, ignored rs1/rd | the same | agree (`fence.S`, random FENCE words) |
| self-modifying code | FENCE.I makes stores visible | Spike flushes its decode cache on FENCE.I | programs always use FENCE.I |

## 6. Random programs

`tools/rvgen.py --seed N` writes a self-contained program (its own SplitMix64 random numbers:
the same seed gives the same program on any Python). Body items, by weight: ALU register and
immediate operations with corner values and immediates at both ends; LUI/AUIPC; M
operations, also with special operands (0, -1, the extremes); loads and stores of every
width and sign in a 4 KiB data area (base x3), also through pointers made just before;
forward branches over blocks (taken and not); bounded loops (1-4 iterations); JAL/JALR
forwards (links, JALR bit 0); calls to leaf functions; CSR operations where Spike and the
contract agree; FENCE (any fields), FENCE.I, hints; MRET to a label; and deliberate traps:
ECALL, EBREAK, illegal words (contract cases, 16-bit encodings with a zero upper half, random
words with undefined fields), illegal CSR accesses, misaligned accesses and jump targets,
load/store/fetch access faults. Sources come mostly from the last four registers written
(dependences at distance 1-4); x0 appears as source and destination. The handler (x30 and
x31 only) reads mtval, mepc and mcause (compared through the trace) and resumes at mepc + 4,
or at ra after a fetch fault. The end stores all 31 registers into the signature (the data
area plus the register dump), then 1 to `tohost`.

Never: WFI, interrupts, `mcycle`/`cycle`/`time`/`mip`, reads of `marchid`/`mimpid` (only
trapping writes), `pmp*`, trigger CSRs, writes to `misa` or `minstreth`, `mtvec` MODE bits,
stores outside the data area (`test_rvgen.py` checks the list in generated programs). On
Spike, the tests check that every illegal word traps with mtval equal to the word, that every
fault address faults for loads, stores and fetches, and that programs pass with traps of all
nine causes. A 3000-instruction program retires about 5,000 instructions with about 250
traps.

## 7. riscv-arch-test

**Version: riscv-arch-test 4.1.0** (ACT4, the current Architectural Certification Tests;
tag 4.1.0 = commit `6e8a45123f14`, tarball SHA-256 `aa00df82…58bb`). The 3.x line is the
old riscof framework; 4.x has more thorough unprivileged tests (every register as source and
destination, corner values), Zicsr tests, and generated machine-mode privileged tests. ACT4
supports Spike as the reference model. Its own framework needs Sail, Ruby and the UDB gem,
which the container does not have, so `tools/rvarch.py` does what the framework does for our
one configuration (`sim/ref/arch/act4/`: `rvtest_config.h` written as UDB would generate it
for ISA.md, `rvmodel_macros.h`, `link.ld`):

1. the unprivileged tests ship as assembly; the machine-mode privileged ones are generated
   with the suite's own generator (`generators/testgen`, standard library only);
2. each test in scope is compiled in signature mode and run on Spike: the reference
   signature (its SHA-256 is recorded in `sim/ref/arch/reference.sha256`; `rvarch.py check`
   fails on any change of suite, Spike, toolchain or configuration);
3. the signature is compiled back into the test (ACT4's self-check mode): an ELF that stores
   1 to `tohost` only if every result matches Spike's;
4. that ELF must pass on Spike; the runner runs it on the core and compares exit status and,
   unless the test reads `cycle`, `time`, `mip` or `instret`, the whole retirement trace.

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
| `SsstrictSm_CompressedInstr` (17) | 17 | a suite bug for cores without C: its handler resumes a 16-bit encoding at mepc + 2, which IALIGN = 32 cannot hold, so the test loops (on Spike too); `-04` also fails to link |
| `rv32i/Misalign` | 5 | need misaligned loads and stores in hardware; SurfCPU traps (ExceptionsSm and our own programs test the traps) |
| `InterruptsSm` | 1 | raises external interrupts through the testbench's register (docs/CPU.md, not known yet); its timer interrupts depend on mtime's rate |
| `rv64i`, `rv64e`, `rv32e`; privileged generators for S, U, H, F, V, Sdtrig, Smstateen, Zawrs, Zkr, Zicbo | not unpacked | other base ISAs and extensions |

On Spike all 177 build, pass their self-check and pass through the runner against the
stand-in: 2,451,871 retirements, the longest tests (`Sm_csr_ro-00` and `-01`) 83,362 each. 167 are also
compared by trace; 10 read `cycle`, `mcycle`, `mcycleh`, `cycleh` or `mip` and are judged by
their self-check (listed in `build/ref/arch/manifest.json`). 32 of the 177 reference
signatures (`Sm_mcsr_access-00` and the 31 `SsstrictSm_CSR` tests) come out different on the
stock Spike: without the contract patch they would expect Spike's choices, not ISA.md's.

## 8. The runner

`tools/rvrun.py` runs each program on the DUT command and on Spike (references cached next
to the ELFs) and gives one verdict per (program, DUT seed): PASS only if the exit status
matches Spike's (0), the trace matches (unless the program is marked timing-dependent), the
signature matches (directed and random programs), and for `csrmap` the core's CSR behaviour
matches ISA.md. Suites: `directed`, `random` (`--seeds N` or `--seed-list`), `arch`,
`csrmap`, and any `--elf FILE`. Each failure leaves `build/ref/<run>/<suite>/<prog>.s<seed>.report.txt`
(the comparator's report with context, the testbench's last output lines); `--json` writes
every outcome.

`python3 tools/rvrun.py selftest` runs the runner itself against the Spike stand-in in 28
behaviours on three programs: clean, the aligned memory layout, no final tohost store (all
must pass), each of the 21 trace faults, a wrong signature, exit 1, exit 2 and a crash
(each must fail with the right kind).

## 9. For the integrator

* Build the core's testbench, then `make test-cpu-ref` (or `python3 tools/rvrun.py --dut
  build/cpu/surfcpu_tb --dut-seeds 1,2,3 --seeds 50 --ilat 1:4 --dlat 1:6`).
* Check the fault addresses of section 2 against docs/CPU.md's testbench registers.
* Things the comparator accepts both ways (memory layout, trap pc_next, short-encoding insn)
  need no decision; things it requires (intr on a handler's first instruction, insn 0 on a
  fetch fault, rd_wdata 0 with rd = x0, consecutive rvfi_order) are RVFI's rules.
* A failure report shows the first divergence: start there; `trace_compare.py REF DUT
  --context 40` gives more context.
* InterruptsSm (riscv-arch-test) needs `RVMODEL_SET_MEXT_INT`: once docs/CPU.md fixes the
  testbench's interrupt register, fill in `sim/ref/arch/act4/rvmodel_macros.h`, remove the
  exclusion in `tools/rvarch.py` and judge it by exit status (its timer interrupts make the
  trace timing-dependent).

## 10. riscv-formal

`tools/rvformal.py` sets up riscv-formal (commit `c992aa61`, SHA-256 checked) for
`surfcpu_core`: `sim/ref/formal/wrapper.sv` puts the core's SurfBus ports on
`surfbus_formal_slave.sv` (keeps every slave rule of SURFBUS.md section 2, answers with
anything the solver likes; asserts the master rules: no withdrawn or changed request, one
outstanding), and `checks.cfg` asks for the RV32IM instruction checks and the pc, register,
uniqueness, causality, liveness and illegal-instruction checks (52 checks). `rvformal.py
smoke` (run by `make test-ref`) elaborates the wrapper with a stand-in that has exactly
ISA.md's port list, generates the checks and runs one through yosys, sby and z3 (vacuous on
the stand-in, which retires nothing). Once the core exists: `rvformal.py gen [--aligned-mem]
[--no-altops]` and `rvformal.py run -j 4`. Two decisions for the cpu track: whether its RVFI
memory fields use the aligned layout (`--aligned-mem`), and whether it implements
riscv-formal's alternative M operations under `RISCV_FORMAL_ALTOPS` (otherwise
`--no-altops` leaves the eight M instruction checks out). The container's boolector crashes
under yosys-smtbmc's `--unroll`, so the solver is z3.

## 11. Proposed contract changes

1. **mtval for an illegal 16-bit encoding** (ISA.md section 3 says "the instruction bits").
   The privileged specification's rule (the shortest of the instruction, ILEN bits, MXLEN
   bits) gives the 16-bit parcel, zero-extended, because bits 1:0 != 11 make it a 16-bit
   instruction; Spike does that. A core that fetches 32 bits may report the whole word.
   Proposal: "for an encoding with bits 1:0 != 11, only its 16 bits, zero-extended", which is
   one mux in the core. Until decided, our programs avoid the case; riscv-arch-test's
   SsstrictSm CompressedInstr tests would depend on it but are out of scope for another
   reason (section 7).
2. **Writing `minstreth`.** ISA.md says a CSR instruction that writes `minstret` is not
   counted, but not what happens on a `minstreth` write. Proposal: the same (Spike and the
   specification's "the write takes effect after the instruction" reading agree).
3. **`mcause` WLRL.** The table says bits 31 and 4:0 are writable and the note says only
   legal values need to round-trip; the patched reference keeps exactly bits 31 and 4:0.
   Proposal: say "illegal values read back masked to bits 31 and 4:0", or leave the note and
   accept that a test writing an illegal value (only riscv-arch-test's `Sm_mcause` does)
   may differ.
4. **The trace format** (MODULES.md section 5) should say which RVFI memory layout the
   testbench writes and that `rd` is hex; the comparator accepts both layouts meanwhile.

## 12. Results

Evidence with commands, seeds and counts: [`docs/evidence/verif/`](evidence/verif/).
Pending: the comparison against the real core (`make test-cpu-ref`), riscv-formal on the
real core, InterruptsSm.
