# SurfCPU core: how the pipeline implements ISA.md

`surfcpu_core` is SurfCPU's processor: RV32IM + Zicsr + Zifencei in machine mode, exactly as
[ISA.md](ISA.md) specifies it (instructions, CSR table, traps, ports, RVFI), with two SurfBus
master ports ([SURFBUS.md](SURFBUS.md)). This document says how it does it: the five-stage
pipeline, its stall and flush rules, hazards, bus behaviour, trap timing, the CSR
implementation, the core testbench's memory map and registers, performance, a resource
estimate and the limits. Everything here is a simulation result; nothing has run on the
FPGA yet, and timing on the device has not been analysed (section 13).

## 1. Files

| File | Module | What it is |
|---|---|---|
| `rtl/cpu/surfcpu_core.v` | `surfcpu_core` | the pipeline: stage registers, forwarding, interlocks, the commit point, RVFI |
| `rtl/cpu/surfcpu_fetch.v` | `surfcpu_fetch` | IF: the instruction-port master and the 4-word fetch buffer |
| `rtl/cpu/surfcpu_decode.v` | `surfcpu_decode` | combinational decoder, illegal-instruction detection |
| `rtl/cpu/surfcpu_alu.v` | `surfcpu_alu` | the RV32I integer operations |
| `rtl/cpu/surfcpu_muldiv.v` | `surfcpu_muldiv` | single-cycle 33x33 multiplier, 32-cycle restoring divider |
| `rtl/cpu/surfcpu_csr.v` | `surfcpu_csr` | the CSRs, trap entry and MRET, counters, the interrupt decision |
| `rtl/cpu/surfcpu_regfile.v` | `surfcpu_regfile` | 32 x 32 registers, two asynchronous reads, one write (distributed RAM) |
| `rtl/cpu/surfcpu_defs.vh` | | ALU operation codes and exception cause numbers |
| `rtl/lib/surf_fifo.v` | `surf_fifo` | (shared) the fetch buffer's storage |
| `rtl/cpu/surfcpu_rvfi.vh` | | (contract, frozen) the RVFI port macros |

Parameters: `RESET_PC` (default `0x0000_0000`, ISA.md) and `MIMPID` (`0x0000_0100`). The
ports are ISA.md section 4. Verilog-2005, no vendor primitives; simulation-only code is
behind `` `ifndef SYNTHESIS `` (two protocol assertions in the fetch unit) and
`` `ifdef SURFCPU_RVFI `` (the retirement record). `make lint-cpu` runs `verilator
--lint-only -Wall` with and without `SURFCPU_RVFI`; the one waiver (two unused sign bits of
the 66-bit product) is commented where it is.

## 2. The pipeline

```
          instruction port                                        data port
                 |                                                    |
   +-------------v-----------+  +----------+  +-----------+  +--------v-------+  +-----------+
   | IF  surfcpu_fetch       |  | ID       |  | EX        |  | MEM            |  | WB        |
   |  pc, one request in     |  |  decode  |  |  operand  |  |  commit point: |  |  register |
   |  flight, 4-word buffer  |->|  the     |->|  forward  |->|  traps and     |->|  file     |
   |  {pc, fault, insn}      |  |  buffer  |  |  ALU, mul |  |  interrupts,   |  |  write    |
   |                         |  |  head;   |  |  div, br/ |  |  load/store,   |  |  RVFI     |
   |                         |  |  reg read|  |  jump, ad-|  |  CSR, MRET,    |  |  record   |
   |                         |  |  +bypass |  |  dress,   |  |  WFI, FENCE.I  |  |           |
   +-----------^-------------+  +----^-----+  |  checks   |  +--+-----+-------+  +-----+-----+
               |                     |        +--^-----^--+     |     |                |
               |                     |           |     |        |     |                |
               |                     |           |     +- MEM forward (results from EX)|
               |                     |           +------- WB forward ------------------+
               |                     +------------------- WB bypass of the reg read ---+
               +--- redirect: from EX (taken branch or jump, as it moves to MEM)
                              from MEM (trap, interrupt, MRET, FENCE.I; wins over EX)
```

| Stage | Holds | Pipeline register |
|---|---|---|
| IF | the next fetch address, a request being offered or awaited, the fetch buffer | the buffer is the IF/ID register |
| ID | the oldest buffered word (the buffer head) | ID/EX: `ex_*` |
| EX | one instruction; a divide stays here while it runs | EX/MEM: `mem_*` |
| MEM | one instruction until it completes or traps | MEM/WB: `wb_*` (and `rv_*` with RVFI) |
| WB | the register write of the instruction that completed last cycle | |

**IF.** `surfcpu_fetch` requests 4-byte-aligned words sequentially from `pc`. SurfBus
allows one outstanding request, so a new request is offered only when none is waiting for
its response, or in the cycle that response arrives (with one-cycle memory that is one
fetch per cycle). A request is offered only if the buffer will have room for its response
next to everything it already holds and the response arriving now, so a response is always
taken in its cycle (SURFBUS.md rule 5) without looking at the rest of the pipeline. A request
offered and not accepted is offered again unchanged (rule 2, no withdrawal). A fetch error
becomes a buffer entry with its fault bit set (and instruction bits 0), so the fault is
raised in program order by the instruction that would have been there. `req_valid` depends
only on registers and on `rsp_valid`.

**ID** decodes the buffer head and reads `rs1`/`rs2` from the register file, with `x0`
reading 0 and a register being written by WB in the same cycle reading the new value. The
decoder flags illegal instructions, ECALL and EBREAK (with all controls zero, so they read
and write nothing); a fetch fault overrides the decode. Exceptions travel with the
instruction (`ex_exc`, cause, `mtval`); nothing is taken before MEM.

**EX** forwards operands (section 4), computes the ALU result, the branch decision, branch
and jump targets, the JAL/JALR link (`pc + 4`), load/store addresses and their alignment,
multiplies in one cycle and runs the divider. It adds two kinds of exception: a taken
branch or jump to a target with bit 1 set (instruction address misaligned, `mtval` = the
target; JALR clears bit 0 first; such a jump does not redirect fetch and writes no
register), and a misaligned load or store (`mtval` = the address). A taken branch or jump
with an aligned target redirects fetch in the cycle it moves to MEM.

**MEM** is the commit point: everything older has completed (it is in WB or retired) and
cannot trap any more. Here an instruction:

* is replaced by an interrupt if one is pending and enabled and the instruction has not
  started (section 6);
* otherwise traps if it carries an exception, or if it is a CSR access that `surfcpu_csr`
  declares illegal (no such CSR, or a write to a read-only one);
* otherwise does its work: a load or store goes to the data port and completes with the
  response (an error response is an access fault, `mtval` = the address); a CSR instruction
  reads and writes; MRET returns; WFI waits; FENCE.I refetches; the rest just complete.

A completing instruction moves to WB and is counted in `minstret`; a trapping one is
removed (with RVFI it leaves a trap record).

**WB** writes the register file at the end of its cycle (and presents the RVFI record).

## 3. Valid, stall and flush

Each stage has a valid bit. A stall keeps a stage's instruction where it is, a flush clears
valid bits, and a bubble is just a stage that is not valid: no `nop` is ever inserted.

| Signal | Meaning |
|---|---|
| `mem_done` | the MEM instruction completes (`commit`) or traps (`trap_take`) this cycle |
| `mem_redirect` | MEM traps, takes an interrupt, or completes an MRET or FENCE.I |
| `mem_free` | MEM is empty, or completes this cycle without redirecting |
| `ex_ready` | EX has its operands (no load-use wait) and, for a divide, the result |
| `ex_advance` | `ex_valid && ex_ready && mem_free` |
| `ex_redirect` | `ex_advance` of a taken branch or jump with an aligned target |
| `id_advance` | buffer not empty, EX empty or advancing, and no redirect this cycle |

Stalls: IF stops requesting when its buffer is full; ID waits for EX; EX waits for an
operand, the divider, or MEM; MEM waits for the data port or, for WFI, an interrupt. WB
never waits. Flushes: a redirect empties the fetch buffer (which is ID); `mem_redirect`
also clears EX and removes the MEM instruction. Requests already offered or accepted on the
instruction port cannot be withdrawn: they complete and their responses are dropped
(`hold_drop`, `out_drop` in the fetch unit). The data port never needs this, because only
MEM issues data requests and MEM never holds a wrong-path instruction.

Competing redirects: a redirect from MEM is older than one from EX and wins; EX redirects
only when its instruction actually moves to MEM, which cannot happen in a cycle where MEM
redirects (`mem_free` is then 0).

## 4. Hazards

**Forwarding into EX.** An operand whose producer is still in flight is taken from the
youngest older producer: the MEM instruction's result (from EX, in `mem_result`) or the WB
instruction's value (`wb_wdata`). `x0` is never forwarded (an instruction writing `x0` does
not set its write enable). Because EX can wait for many cycles (behind a slow load or
store, or the divider), its operand registers are refreshed from the forwarding network
every cycle it waits: a producer in WB now is retired next cycle, and its value must not be
lost.

**WB bypass in ID.** A register read in ID in the cycle WB writes it returns the new value.

**Late results and the load-use interlock.** A load and a CSR read produce their value only
as they leave MEM. An instruction in EX that reads the destination of a load or CSR
instruction in MEM waits until that instruction is in WB, then forwards from there: one
bubble after the load. Store data (`rs2`) and store addresses, branch operands, JALR
targets, CSR sources and divider operands all go through the same forwarding and
interlock.

**Divides** start in EX once their operands are final and take 32 more cycles; the result
waits in the divider until the instruction moves on. A divide flushed while it runs is
abandoned (`div_clear`).

**CSRs** need no serialisation: they are read and written only at the commit point, in
order, and nothing before MEM depends on CSR state (traps, interrupts, MRET and FENCE.I are
all decided in MEM). A write that enables an interrupt affects the next instruction to reach
MEM.

**Memory ordering.** Loads and stores complete in program order, one at a time, each before
the next instruction's access starts, so FENCE is a no-op. FENCE.I completes in MEM (every
older store has already received its response), then redirects fetch to the next
instruction and drops everything fetched before.

## 5. The bus ports

Instruction port: `req_write = 0`, `req_be = 1111`, `req_wdata = 0`, `req_fetch = 1`, aligned
addresses, one request in flight, no withdrawal, responses after a redirect dropped
(section 2). Fetches have no side effects (SURFBUS.md section 7), so wrong-path fetches,
including ones that fault, are harmless.

Data port, as SURFBUS.md section 7 requires:

* A load or store is offered only from MEM, where no older instruction can trap any more;
  a flushed (wrong-path) instruction never reaches MEM, so it never reaches the data port.
* It is not offered in a cycle where an interrupt is taken instead. Once offered it is held
  unchanged until accepted, and the instruction stays in MEM until the response; an
  interrupt waits for it. So every load and store is performed exactly once, however long
  `req_ready` or the response is delayed.
* `req_addr` is the byte address, `req_be` the lanes (`0001 << a[1:0]`, `0011 << a[1:0]`,
  `1111`), `req_wdata` the data replicated across the lanes. Misaligned accesses trap
  before the bus (ISA.md), so the enables always match the address.
* Load data is selected from the whole word by `a[1:0]` and sign- or zero-extended.

## 6. Traps, interrupts, WFI, MRET, FENCE.I

**Where exceptions are found.** IF: instruction access fault. ID: illegal instruction (the
encoding), ECALL, EBREAK. EX: misaligned target, misaligned load/store. MEM: illegal CSR
access, load/store access fault (at the response). An instruction keeps the first
exception found, which gives ISA.md's priority (fetch fault, then illegal / misaligned
target / ECALL / EBREAK, then misaligned access, then access fault).

**Taking a trap.** In MEM, in the instruction's first cycle there (or when its bus response
arrives): `mepc` = its pc, `mcause`, `mtval` as ISA.md section 3 (for an illegal instruction
the instruction bits; a 16-bit encoding, bits 1:0 not 11, reports just its 16 bits, as the
privileged specification and Spike do), `mstatus.MPIE` = `MIE`, `MIE` = 0; EX, ID and the
fetch buffer are flushed and fetch restarts at `mtvec`. With one-cycle memory the handler's
first instruction is in EX four cycles after the trap (four bubbles, one more than a taken
branch, because the redirect comes from MEM).

**Interrupts.** `mip` is the three inputs (MEIP, MTIP, MSIP); an interrupt is pending and
enabled when `mstatus.MIE` and `(mip & mie) != 0`, and is taken in the order MEI, MSI, MTI. It
is taken in place of the instruction in MEM, before that instruction has done anything
(`mepc` = its pc, `mtval` = 0, the instruction is re-executed after MRET): never once its
bus request has been offered, and never on a WFI. With MEM empty (after a branch, say) the
interrupt waits for the next instruction to arrive there, so interrupt latency is a few
cycles plus whatever the instruction in MEM is waiting for (a slow load, or a divide still
in EX). An instruction that would trap is still interrupted first (the interrupt is between
instructions); after MRET it traps.

**WFI** waits in MEM until `(mip & mie) != 0`, whatever `MIE` is, then completes. If the
interrupt is also enabled by `MIE`, the next instruction to reach MEM takes it, so `mepc` =
the instruction after the WFI. An interrupt already pending and enabled when the WFI reaches
MEM makes it complete at once, and the same holds (the WFI itself is never interrupted).

**MRET** completes in MEM: `pc` = `mepc`, `MIE` = `MPIE`, `MPIE` = 1 (`MPP` stays M);
younger instructions are flushed.

**FENCE.I** completes in MEM and refetches from the next instruction (section 4).
**FENCE** (any fields) completes as a no-op. **ECALL** and **EBREAK** are exceptions; they do
not retire.

## 7. CSRs and counters

`surfcpu_csr` holds exactly ISA.md section 2: the identification registers (`mimpid` =
`MIMPID`), `mstatus` (only MIE and MPIE are state; MPP reads 11), `misa` = `0x40001100`
(writes ignored), `mie`, `mtvec` (BASE only), `mstatush` = 0, `mcountinhibit` (CY, IR),
`mscratch`, `mepc` (bits 1:0 read 0), `mcause` (bit 31 and 4:0), `mtval`, `mip` (read-only
inputs), `mcycle`/`minstret` and their high halves, the read-only shadows `cycle`,
`time` (the `mtime` input), `instret`, and the read-as-zero ranges (PMP, `mhpmevent`,
`mhpmcounter`, `hpmcounter`). Every other address, and a write to an address with bits 11:10
= 11, is an illegal instruction; "write" is decided by the encoding (CSRRW[I], or a nonzero
`rs1`/`uimm` field), not the value.

* The CSR read happens at the commit point and returns the value before the instruction's
  own write. The write applies the instruction's operation (write, set, clear) under the
  register's writable mask.
* `mcycle` counts every clock cycle unless `mcountinhibit.CY`. `minstret` counts every
  instruction that completes in MEM (not traps, not interrupted instructions) unless
  `mcountinhibit.IR`. A CSR read of `minstret` sees every older instruction counted.
* A write to `mcycle`, `mcycleh`, `minstret` or `minstreth` replaces that half and that
  cycle's increment is lost; so the CSR instruction that writes `minstret` is not counted.

## 8. RVFI

With `SURFCPU_RVFI`, WB presents one record per completed instruction and one per exception
(NRET = 1, `rvfi_mode` = 3, `rvfi_ixl` = 1, `rvfi_halt` = 0, `rvfi_order` counting records
from 0):

* a completed instruction: its pc and instruction, the registers it read (address 0 and
  value 0 for an operand it does not use), the register it wrote (0 / 0 for none or `x0`),
  `rvfi_pc_wdata` = the next pc (the target of a taken branch or jump, `mepc` for MRET);
  for a load or store `rvfi_mem_addr` = the word-aligned address, the lane mask in
  `rmask`/`wmask`, and `mem_rdata`/`mem_wdata` with only those lanes (others 0): the
  `RISCV_FORMAL_ALIGNED_MEM` convention;
* an exception: `rvfi_trap` = 1, no register write, no memory access (masks 0, even for an
  access fault, which had no effect), `rvfi_pc_wdata` = the trap handler address (`mtvec`);
* an interrupt retires nothing; the next record (the handler's first instruction) has
  `rvfi_intr` = 1. So does the first record after any trap.

The testbench writes these records as the trace of MODULES.md section 5;
[sim/README.md](../sim/README.md) gives the field formats.

## 9. The core testbench (`build/cpu/surfcpu_tb`)

MODULES.md section 6 fixes the command line; [sim/README.md](../sim/README.md) describes the
harness. Its memory map:

| Address | Size | What | Fetch |
|---|---|---|---|
| `0x8000_0000` | 4 MiB | RAM, loaded from the ELF (the core starts at `0x8000_0000`) | yes |
| `0x0200_0000` | 64 KiB | CLINT-compatible timer, as Spike's: `msip` +0x0000 (bit 0 = `irq_software`), `mtimecmp` +0x4000/+0x4004 (reset all ones), `mtime` +0xBFF8/+0xBFFC (counts clock cycles from the end of reset; writable); `irq_timer` = `mtime >= mtimecmp` | no |
| `0x0300_0000` | 4 KiB | testbench registers (below) | no |
| everything else | | bus error | |

Registers (CLINT and testbench) take aligned word accesses only; any other width, or an
offset with no register, gets a bus error, as the SoC's MMIO does.

| Offset | Register | Access | Meaning |
|---|---|---|---|
| `0x000` | `CONSOLE` | wo | bits 7:0 are written to the testbench's standard output |
| `0x004` | `IRQ_EXT` | rw | bit 0 is the `irq_external` level |
| `0x008` | `IRQ_DELAY` | rw | writing N > 0 sets `IRQ_EXT` N cycles later (0 cancels); reads the cycles left |
| `0x00C` | `IRQ_RANDOM` | rw | N > 0: each cycle, with probability 1/N (random from `--seed`), set `IRQ_EXT`; 0 = off |
| `0x010` | `PROBE_RD` | rc | a read returns how many times it was read before (and counts this one); a write sets the count |
| `0x014` | `PROBE_WR` | rw | a write counts one (and adds its data into `PROBE_SUM`); a read returns the count |
| `0x018` | `PROBE_SUM` | rw | the sum of the values written to `PROBE_WR`; a write sets it |
| `0x01C` | `CYCLE` | ro | the testbench's cycle counter (low 32 bits) |

The probes are MMIO registers with side effects: the tests use them to show that wrong-path
code never reaches the data port and that each access happens exactly once.

## 10. Performance (simulation, one-cycle memory, no wait states)

Measured with `mcycle` on loops (zero-latency bus):

| Case | Cost |
|---|---|
| independent ALU instructions | 1 cycle each |
| taken branch or jump | 3 extra cycles (resolved in EX; fetch restarts the next cycle) |
| not-taken branch | 1 cycle (fetch continues sequentially; no prediction) |
| load or store | 2 cycles in MEM (request, then response) |
| instruction using a load or CSR result at once | 1 extra cycle |
| MUL, MULH, MULHSU, MULHU | 1 cycle |
| DIV, DIVU, REM, REMU | 34 cycles in EX |
| trap, interrupt, MRET or FENCE.I | 4 extra cycles (redirect from MEM) |

Examples: a loop of 8 independent ALU instructions plus `addi`/`bnez` runs at 13 cycles per
10 instructions; a two-instruction loop takes 5 cycles per iteration. With slower memory
every fetch and access costs its latency plus any `req_ready` wait: there is no cache and
only one request in flight per port (SURFBUS.md version 1).

## 11. Resources (Yosys estimate)

`make synth-cpu` runs Yosys 0.52 `synth_xilinx -family xc7 -flatten -noiopad` on
`surfcpu_core` alone (without RVFI). Estimate (2026-10-15, the commit of this document):

| Resource | Count |
|---|---|
| LUTs (logic) | 3094 |
| LUTs as distributed RAM | 92 (23 RAM32M: register file 12, fetch buffer 11) |
| Flip-flops | 937 |
| CARRY4 | 151 |
| MUXF7 / MUXF8 | 646 / 278 |
| DSP48E1 | 4 (the 33 x 33 multiplier) |
| Block RAM | 0 |

That is about 15 % of the LUTs of an xc7a35t (20,800) and 5 % of an xc7a100t (63,400).
These are Yosys's numbers, not Vivado's post-route utilisation, which is pending. They move
by 10-15 % with small RTL changes (the flattened ABC mapping is sensitive): an earlier
revision gave 2694 LUTs, and the same RTL synthesised per module adds up to about 2830
(core 1160, CSRs 632, ALU 487, multiplier/divider 330, decoder 106, fetch 111). The
largest single item is the ALU's three barrel shifters; one shared shifter would save a
couple of hundred LUTs if space ever matters.

**Did synthesis keep everything?** Yosys 0.52 can silently delete logic when it packs a
multiply into DSP48s (another track hit this). Three checks show it did not delete any here:

* Synthesised per module (`make synth-cpu-modules`, `docs/evidence/cpu/synth-modules.txt`),
  every register keeps its full width, listed bit by bit by the net on each flip-flop's
  output (`sim/cpu/synth_regs.py`): divider 139, CSRs 265, fetch unit 98, fetch-buffer
  pointers 6, pipeline registers 433 (436 declared, less 3 bits of the exception-cause
  fields that can never be 1: ID only produces causes 1, 2, 3 and 11, EX adds 0, 4 and 6),
  941 in all; the multiplier is 4 DSP48E1, the register file 12 RAM32M, the fetch buffer 11.
  The flattened run above has 4 fewer, the always-zero low bits of `ex_pc` and `mem_pc`,
  which only show up as constants across the fetch unit's boundary.
* `make synth-cpu-gate` writes the synthesised netlist (the same flow, with `RESET_PC` set to
  the testbench's `0x80000000`) and simulates it in Icarus with Yosys's models of the Xilinx
  cells (LUTs, CARRY4, FDRE, RAM32M, DSP48E1) on the 13 directed tests that need only RAM,
  including `muldiv` and the exhaustive `muldiv_edges`: they all pass on the netlist.
* `sim/cpu/gate_mutant.py` gives one DSP48E1 of a copy of that netlist an output register it
  does not have; the multiply test then fails, so the gate-level check really exercises the
  DSP mapping and would notice a multiplier that synthesis broke or dropped.

The deepest combinational path is WB/MEM forwarding into the multiplier into the EX/MEM
result register: 26 levels in a generic 6-LUT mapping where the multiplier is built from
LUTs (Yosys `ltp`); on the FPGA the multiplier maps to DSP48E1s. The next ones are the
forwarding mux into the branch target adder and the fetch redirect, and the data-port
response through `mem_done` into the stage enables. Whether the core meets 25 MHz is not
known until a real place and route: **pending** (no Vivado host yet; docs/BUILD-HOST.md).
If it is ever short, the multiplier is the first candidate for a pipeline register.

## 12. Verification (simulation)

`make test-cpu` builds the testbench and runs:

* **Directed tests** `sim/cpu/tests/*.S` (self-checking, riscv-tests style; two generated
  from `gen_*.py`) and one C program (`sim/cpu/tests/c/cprogram.c`, GCC -O2 with libgcc),
  each under 14 bus timings and seeds (`sim/cpu/run_tests.py`): latencies from 1:1 up to
  20:100 cycles on both ports, `req_ready` waits, one configuration with random reset values.
  22 tests: `alu`, `branch`, `ldst`, `muldiv`, `muldiv_edges`, `csr`, `csr_impl` (all 4096 CSR
  addresses), `counters`, `traps`, `illegal`, `misaligned`, `access_fault`, `hazards_matrix`
  (420 RAW cases), `hazards`, `flush`, `fencei`, `mmio`, `wrongpath`, `interrupts`, `wfi`,
  `irq_stress`, `c_cprogram`. The testbench also checks the bus protocol on both ports in
  every run.
* **riscv-tests** (`sim/cpu/riscv_tests.py`, pinned commit, fetched once): the rv32ui,
  rv32um and rv32mi "p" tests under three timings; three need features ISA.md leaves out
  (debug triggers, PMP entries, hardware misaligned accesses) and must fail as noted.
* **The testbench itself** (`sim/cpu/tb_selftest.py`): its exit codes, signature and trace
  formats against MODULES.md section 6 and sim/README.md.
* **Icarus Verilog** (`sim/cpu/run_icarus.py`, `sim/cpu/tb_core_icarus.v`): the core built
  without RVFI runs the thirteen tests that need only RAM and bus errors (the C program
  among them), with random `req_ready` and latencies, three seeds each: the RTL simulates in
  both simulators. The same testbench runs the post-synthesis netlist (section 11).

Cross-checks of the tests themselves: `make test-cpu-spike` runs every directed test that
does not need the testbench's devices on Spike (they all pass there too), and
`sim/cpu/spike_compare.py` / `fuzz_spike.py` compare traces with Spike's commit log
(throwaway tools of this track; the verif track owns the official comparator). Results and
seeds are in [evidence/cpu/](evidence/cpu/).

## 13. Limits and known gaps

* **No hardware results.** Simulation only; synthesis is a Yosys estimate; timing,
  utilisation after place and route, and behaviour on the board are pending.
* No caches, no branch prediction (taken branches cost 3 cycles), one outstanding request
  per port: performance is modest by design (brief section 5).
* Misaligned loads and stores trap (no hardware or emulated support).
* Interrupt latency depends on the instruction in MEM: a slow MMIO access delays it; with
  MEM empty it waits for the next instruction (at most the time to fetch it).
* Not implemented, as ISA.md says: U and S modes, PMP entries, debug mode and triggers
  (accessing `tselect` is an illegal instruction), vectored `mtvec`, HPM events.
* The register file is not reset (ISA.md): software must not read a register before writing
  it. Distributed RAM on the FPGA powers up as 0, simulation in Verilator as 0 (random with
  `--rand-reset`).
