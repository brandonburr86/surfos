# SurfCPU: implemented ISA, CSRs, traps and core interface

This is the contract SurfCPU's RTL, its tests, SurfBoot and the SurfOS port share. It says
what is implemented and how; [CPU.md](CPU.md) (written with the RTL) says how the pipeline
does it. Frozen at version 1 on 2026-10-15.

## 1. ISA

| Item | Value |
|---|---|
| Base | RV32I, version 2.1 |
| Extensions | M 2.0, Zicsr 2.0, Zifencei 2.0 |
| GCC `-march` / `-mabi` | `rv32im_zicsr_zifencei` / `ilp32` (soft float) |
| Specifications | *The RISC-V Instruction Set Manual, Volume I: Unprivileged ISA*, version 20240411; *Volume II: Privileged Architecture*, version 20240411 (Machine ISA 1.13) |
| Harts | one, `mhartid` = 0 |
| XLEN / IALIGN | 32 / 32 (no C extension: a 2-byte-aligned target is misaligned) |
| Endianness | little (`mstatus.MBE` = 0) |
| Privilege modes | M only. No U, no S, no virtual memory, no PMP entries |
| Not implemented | A, F, D, C, Zba/Zbb/Zbs, Zicond, Zihpm counters (read as zero), debug mode and triggers, Smrnmi, vectored `mtvec` |

The core implements the unprivileged instructions of RV32I, M, Zicsr and Zifencei plus `ECALL`,
`EBREAK`, `MRET` and `WFI`. Everything else is an illegal instruction (cause 2), including
`0x00000000`, `0xFFFFFFFF`, every 16-bit (compressed) encoding, `SRET`, `URET`, `SFENCE.VMA`,
`SLLI/SRLI/SRAI` with `imm[5]` = 1, and any undefined `funct3`/`funct7` combination.
`FENCE` (any predecessor/successor set, including `FENCE.TSO` and `PAUSE`) completes as a no-op
because accesses already complete in order, one at a time. `FENCE.I` completes after every older
store and refetches the following instruction from memory.

Planned later, not now: a user mode and Sv32 for protected processes (brief section 5), caches.

## 2. CSRs

Any CSR address not in this table raises an illegal-instruction exception when accessed. A
write (a CSRRW/CSRRWI, or a CSRRS/CSRRC/CSRRSI/CSRRCI whose rs1/uimm is not 0) to a read-only
address (bits 11:10 = `11`) raises an illegal-instruction exception and changes nothing.
"Fixed" fields ignore writes.

| CSR | Address | Reset | Writable bits | Notes |
|---|---|---|---|---|
| `mvendorid` | `0xF11` | 0 | read only | not a commercial implementation |
| `marchid` | `0xF12` | 0 | read only | no registered architecture ID yet |
| `mimpid` | `0xF13` | `SURF_SOC_VERSION` | read only | 0x00000100 = SoC 0.1.0 |
| `mhartid` | `0xF14` | 0 | read only | |
| `mconfigptr` | `0xF15` | 0 | read only | no configuration structure |
| `mstatus` | `0x300` | `0x00001800` | `MIE` (bit 3), `MPIE` (bit 7) | `MPP` (12:11) is fixed at `11` (M); every other bit is 0 |
| `misa` | `0x301` | `0x40001100` | none (writes ignored) | MXL = 1 (32-bit), I and M |
| `mie` | `0x304` | 0 | `MSIE` (3), `MTIE` (7), `MEIE` (11) | |
| `mtvec` | `0x305` | 0 | `BASE` (31:2) | `MODE` (1:0) fixed at 0: direct mode only |
| `mstatush` | `0x310` | 0 | none | |
| `mcountinhibit` | `0x320` | 0 | `CY` (0), `IR` (2) | 1 stops `mcycle` / `minstret` |
| `mhpmevent3`..`31` | `0x323`..`0x33F` | 0 | none | |
| `mscratch` | `0x340` | 0 | all | |
| `mepc` | `0x341` | 0 | 31:2 | bits 1:0 read as 0 |
| `mcause` | `0x342` | 0 | bit 31 and 4:0 | WLRL; only legal values need to round-trip |
| `mtval` | `0x343` | 0 | all | see section 3 |
| `mip` | `0x344` | 0 | none | `MSIP` (3) = timer.MSIP, `MTIP` (7) = mtime >= mtimecmp, `MEIP` (11) = intc; all read only |
| `pmpcfg0`..`15` | `0x3A0`..`0x3AF` | 0 | none | zero PMP entries |
| `pmpaddr0`..`63` | `0x3B0`..`0x3EF` | 0 | none | |
| `mcycle` / `mcycleh` | `0xB00` / `0xB80` | 0 | all | counts `sys_clk` cycles |
| `minstret` / `minstreth` | `0xB02` / `0xB82` | 0 | all | counts retired instructions (a trapping instruction does not retire) |
| `mhpmcounter3`..`31` (`h`) | `0xB03`..`0xB1F`, `0xB83`..`0xB9F` | 0 | none | |
| `cycle` / `cycleh` | `0xC00` / `0xC80` | | read only | `mcycle` |
| `time` / `timeh` | `0xC01` / `0xC81` | | read only | the timer's `mtime` (`timebase_hz`) |
| `instret` / `instreth` | `0xC02` / `0xC82` | | read only | `minstret` |
| `hpmcounter3`..`31` (`h`) | `0xC03`..`0xC1F`, `0xC83`..`0xC9F` | | read only | 0 |

64-bit counters on RV32: read `xh`, then `x`, then `xh` again, and retry if the two high
reads differ. A write to `mcycle` (or `minstret`) replaces the low half; the counter's own
increment in that cycle is lost. A CSR instruction that writes `minstret` is not counted.

## 3. Traps

All traps go to M-mode at `mtvec.BASE` (direct mode). On a trap: `mepc` = the address of the
interrupted or trapping instruction; `mcause` as below; `mtval` as below; `mstatus.MPIE` =
`MIE`, `MIE` = 0, `MPP` = M. `MRET` sets `pc` = `mepc`, `MIE` = `MPIE`, `MPIE` = 1. Traps are
precise: everything older than the trapping instruction has completed and nothing younger
has changed architectural state, memory or a peripheral.

| Cause | `mcause` | `mtval` | Raised by |
|---|---|---|---|
| Instruction address misaligned | 0 | the target address | a taken branch, `JAL` or `JALR` whose target is not 4-byte aligned; reported on that instruction, which writes no register |
| Instruction access fault | 1 | the fetch address | a fetch error response (unmapped or non-fetch region) |
| Illegal instruction | 2 | the instruction bits | section 1 and section 2 |
| Breakpoint | 3 | the `EBREAK`'s address | `EBREAK` |
| Load address misaligned | 4 | the address | a halfword load at an odd address, a word load not 4-aligned |
| Load access fault | 5 | the address | an error response to a load |
| Store address misaligned | 6 | the address | as for loads |
| Store access fault | 7 | the address | an error response to a store |
| Environment call from M-mode | 11 | 0 | `ECALL` |
| Machine software interrupt | `0x80000003` | 0 | `mip.MSIP` and `mie.MSIE` and `mstatus.MIE` |
| Machine timer interrupt | `0x80000007` | 0 | `mip.MTIP` and `mie.MTIE` and `mstatus.MIE` |
| Machine external interrupt | `0x8000000B` | 0 | `mip.MEIP` and `mie.MEIE` and `mstatus.MIE` |

If several exceptions apply to one instruction, the standard priority is used: instruction
access fault, then illegal instruction / misaligned target / `ECALL` / `EBREAK`, then load/store
misaligned, then load/store access fault. Pending enabled interrupts are taken between
instructions, in the order MEI, MSI, MTI, before the next instruction executes; an interrupt
never splits a load or store that has gone to the bus.

`WFI` waits until `(mip & mie) != 0` (whatever `mstatus.MIE` is), then completes; if the
interrupt is enabled it is taken with `mepc` = the instruction after the `WFI`. After reset
the core starts at `SURF_RESET_PC` (0) with every CSR at its reset value and the register file
undefined except `x0` (tests must not rely on register reset values).

## 4. Core interface

```verilog
module surfcpu_core #(
    parameter [31:0] RESET_PC = 32'h0000_0000,
    parameter [31:0] MIMPID   = 32'h0000_0100
) (
    input  wire        clk,
    input  wire        rst,             // synchronous, active high
    // instruction port (SurfBus master, SURFBUS.md)
    output wire        ibus_req_valid,
    input  wire        ibus_req_ready,
    output wire [31:0] ibus_req_addr,
    output wire        ibus_req_write,  // 0
    output wire [3:0]  ibus_req_be,     // 4'b1111
    output wire [31:0] ibus_req_wdata,  // 0
    output wire        ibus_req_fetch,  // 1
    input  wire        ibus_rsp_valid,
    input  wire [31:0] ibus_rsp_rdata,
    input  wire        ibus_rsp_err,
    // data port (SurfBus master)
    output wire        dbus_req_valid,
    input  wire        dbus_req_ready,
    output wire [31:0] dbus_req_addr,
    output wire        dbus_req_write,
    output wire [3:0]  dbus_req_be,
    output wire [31:0] dbus_req_wdata,
    output wire        dbus_req_fetch,  // 0
    input  wire        dbus_rsp_valid,
    input  wire [31:0] dbus_rsp_rdata,
    input  wire        dbus_rsp_err,
    // interrupts: levels, synchronous to clk
    input  wire        irq_external,    // mip.MEIP
    input  wire        irq_timer,       // mip.MTIP
    input  wire        irq_software,    // mip.MSIP
    input  wire [63:0] mtime            // for the time/timeh CSRs
`ifdef SURFCPU_RVFI
    , `SURFCPU_RVFI_OUTPUTS             // section 5
`endif
);
```

## 5. Retirement trace (RVFI)

With `SURFCPU_RVFI` defined (simulation and formal only), the core reports every retired
instruction and every trap on the RISC-V Formal Interface, one retirement per cycle at most
(NRET = 1, XLEN = 32, ILEN = 32), with its standard meaning
([riscv-formal docs](https://github.com/YosysHQ/riscv-formal/blob/main/docs/rvfi.md)):

| Signal | Width | | Signal | Width |
|---|---|---|---|---|
| `rvfi_valid` | 1 | | `rvfi_rd_addr` | 5 |
| `rvfi_order` | 64 | | `rvfi_rd_wdata` | 32 (0 when `rd` is `x0`) |
| `rvfi_insn` | 32 | | `rvfi_pc_rdata` | 32 |
| `rvfi_trap` | 1 | | `rvfi_pc_wdata` | 32 |
| `rvfi_halt` | 1 | | `rvfi_mem_addr` | 32 |
| `rvfi_intr` | 1 | | `rvfi_mem_rmask` | 4 |
| `rvfi_mode` | 2 (always 3) | | `rvfi_mem_wmask` | 4 |
| `rvfi_ixl` | 2 (always 1) | | `rvfi_mem_rdata` | 32 |
| `rvfi_rs1_addr`, `rvfi_rs2_addr` | 5 | | `rvfi_mem_wdata` | 32 |
| `rvfi_rs1_rdata`, `rvfi_rs2_rdata` | 32 | | | |

`rtl/cpu/surfcpu_rvfi.vh` defines the `SURFCPU_RVFI_OUTPUTS` port list. The simulation
testbenches write the trace as text, one line per `rvfi_valid` (`sim/README.md` gives the
format), and `tools/` compares it with a pinned reference model (Spike) instruction by
instruction: pc, instruction, register write, memory access, trap.
