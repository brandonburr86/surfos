# SurfCPU progress record

Brief section 12. Each milestone lists its evidence. Simulation, synthesis, programming and
physical results are separate columns, so nothing claims more than happened.

| Milestone | Simulation / host tests | Synthesis / bitstream | On the board | State |
|---|---|---|---|---|
| 0 Inventory and contracts | toolchain probes pass ([INVENTORY.md](INVENTORY.md)) | | | **done** (owner's hardware facts still to confirm: INVENTORY.md section 4) |
| 1 Toolchain and board baseline | baseline design (LEDs, buttons, 1 Hz UART line, both clocks) passes in Verilator and Icarus ([evidence](evidence/board/)) | Yosys estimate only; no bitstream: no build host (G2) | pending: board not connected (G1, G3) | design ready; bitstream blocked |
| 2 CPU correctness in simulation | 308 directed runs, 63 riscv-tests, Icarus, 200 random programs against Spike, the synthesized netlist at gate level ([evidence](evidence/cpu/)) | Yosys estimate (3,094 LUTs, 4 DSP48E1) | | core done in simulation; independent review and the verif track pending |
| 3 CPU on the board | the real core in `surfsim` runs `bramdiag` to PASS (569 checks) and starts SurfBoot ([evidence](evidence/soc/)) | | pending | simulation done; bitstream and board pending |
| 4 DDR integration | bridge tests against a MIG model and LiteDRAM's controller, unbounded proofs ([evidence](evidence/ddr/)) | MIG recipe pending a Vivado host; LiteDRAM generated | pending | bridge done; board top wiring pending |
| 5 Card reads and SurfBoot | SurfBoot: 137 card-image scenarios, 120 through the io SD card model, 29 runs of the ROM in a firmware emulator ([evidence](evidence/fw/)) | | pending | software done on the host; RTL SoC and board pending |
| 6 Standalone I/O | SPI + SD card model, PS/2 + keyboard model, VGA 640x480 text decoded from the pixels ([evidence](evidence/io/)) | Yosys estimates | pending | RTL in simulation; review fixes in progress |
| 7 SurfOS kernel port | host tests, QEMU `virt`, and `SURFOS.BIN` booted by SurfBoot in the fw emulator to the shell ([evidence](evidence/os/)) | | pending | kernel done in emulation; RTL SoC boot pending |
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

### 2026-10-15: first tracks merged

* io (fde641f): `surf_spi`, `surf_ps2`, `surf_video` with the SD card, PS/2 keyboard and VGA
  capture models. An independent review found no hardware-safety problem and one PS/2 defect
  (a send could drop a byte arriving at that moment) plus test gaps; fixes are in progress.
* board (1eed8c4): wiring table and pin checker, clock plan, timing and CDC constraints, the
  milestone 1 baseline, the Vivado non-project flow and the build and programming wrappers
  (dry runs and fake tools only). Digilent's manuals are a checked local cache
  (`boards/arty_a7/digilent/fetch.sh fetch`), not in git.
* fw (b555bb6): SurfBoot, the firmware runtime, `bramdiag`, `DDRDIAG.BIN`, `surfimg.py`,
  `mkcard.py`, `bin2mem.py`, `isa-audit.py`, `deploy-sd.py`. SurfBoot's storage code passes 120
  scenarios through the io track's SD card model.
* Contract clarifications, version 1 unchanged (4ea8903): SURFBUS.md section 8, BOOT-ABI.md
  section 6, three register descriptions.
* `make check` passes after each merge.

### 2026-10-15: cpu, ddr, flow, os and soc merged

* cpu (160aa0c): `surfcpu_core`, five stages, with its testbench, directed tests, riscv-tests
  (fetched once, pinned) and a gate-level check of the synthesized core.
* ddr (e792960): `surf_ddr_bridge` and its crossings with unbounded proofs, the MIG recipe
  (pending Vivado) and LiteDRAM for the open flow; the LiteDRAM test is on `make check-openflow`.
* flow (2dd26c9): the open-source bitstream flow (openXC7) builds and checks bitstreams on the
  Mac for both parts; it found two tool defects that give wrong bitstreams silently (a Yosys DSP
  packing bug, and nextpnr's encoding of fractional MMCM dividers with an odd integer part) and
  its checks catch both. The pixel clock moves to settings both flows encode correctly.
* os (c3fb0e0): the machine-mode riscv32 kernel with the qemu_virt and Arty boards; x86 output
  unchanged.
* soc (8b99bb0): the interconnect, memories, UART/timer/intc/GPIO/sysinfo, `surfcpu_soc`,
  `surfsim`. With everything merged, `test-soc-boot` runs the real core in the SoC: `bramdiag`
  passes 569 checks and SurfBoot starts and answers its monitor.
* Reviews: io (one PS/2 defect, fixed), board (wrapper defects and one unsafe wiring note, being
  fixed), fw (deploy-sd symlink and verification defects, being fixed), cpu (running).
