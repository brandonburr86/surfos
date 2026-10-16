# SurfCPU progress record

Brief section 12. Each milestone lists its evidence. Simulation, synthesis, programming and
physical results are separate columns, so nothing claims more than happened.

| Milestone | Simulation / host tests | Synthesis / bitstream | On the board | State |
|---|---|---|---|---|
| 0 Inventory and contracts | toolchain probes pass ([INVENTORY.md](INVENTORY.md)) | | | **done** (owner's hardware facts still to confirm: INVENTORY.md section 4) |
| 1 Toolchain and board baseline | | | | in progress |
| 2 CPU correctness in simulation | | | | in progress |
| 3 CPU on the board | | | | waiting for 1 and 2 |
| 4 DDR integration | | | | groundwork |
| 5 Card reads and SurfBoot | | | | in progress (software) |
| 6 Standalone I/O | | | | in progress (RTL) |
| 7 SurfOS kernel port | | | | in progress (software) |
| 8 Persistent files | | | | later |
| 9 Reproducible standalone release | | | | later |

## Log

### 2026-10-15: milestone 0

* Branch `surfcpu` from `ai-dev` (f3dbd57); worktree `06-restoration-surfcpu` next to
  `06-restoration`.
* Probed the MacBook (Apple M4 Max, macOS 15.1.1) and found no HDL or RISC-V tools on it;
  built the pinned toolchain container `surfcpu-toolchain:2026-10-15` (Verilator 5.032, Icarus
  12.0, Yosys 0.52, RISC-V GCC 14.2 with an `rv32im/ilp32` libgcc, Spike, QEMU 10.0.13). An RV32IM
  program built with it passed on Spike.
* The board, the adapters and the card are not connected: their checks are pending (gap list
  G1-G8 in INVENTORY.md). No bitstream build host exists yet (G2, BUILD-HOST.md).
* Contracts written and frozen at version 1: the memory map and registers (`spec/soc.toml`, with
  its generator and generated files), SurfBus, the ISA/CSR/trap contract with the core's ports
  and RVFI, the boot ABI (image header, CRC test vectors, boot info, handoff, card layout), the
  module interfaces.
* `make doctor` and the command interface; shared RTL blocks in `rtl/lib/` with their tests
  (`make test-lib`: FIFO against a model over 20,000 random steps, 3,000 word crossings without
  a torn value, 500 pulses across clocks).
* Parallel tracks started: [plans/2026-10-15-tracks.md](plans/2026-10-15-tracks.md).
