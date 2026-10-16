# SurfCPU progress record

Brief section 12. Each milestone lists its evidence. Simulation, synthesis, programming and
physical results are separate columns, so nothing claims more than happened.

| Milestone | Simulation / host tests | Synthesis / bitstream | On the board | State |
|---|---|---|---|---|
| 0 Inventory and contracts | toolchain probes pass ([INVENTORY.md](INVENTORY.md)) | | | **done** (owner's hardware facts still to confirm: INVENTORY.md section 4) |
| 1 Toolchain and board baseline | baseline design (LEDs, buttons, 1 Hz UART line, both clocks) passes in Verilator and Icarus ([evidence](evidence/board/)) | bitstreams for both boards from the open flow on the Mac, every flow check passed ([evidence](evidence/board/openxc7.txt)); no Vivado run | pending: board not connected (G1, G3) | bitstream ready; waiting for the board |
| 2 CPU correctness in simulation | directed tests, riscv-tests, riscv-arch-test, random programs and CSR probes compared with a contract-patched Spike instruction by instruction (8.2 M retirements on every `make check`, 19 M in the verif track's long runs), a bounded formal RVFI check on `make check`, riscv-formal 43 of 44 checks, the synthesized netlist at gate level ([cpu](evidence/cpu/), [verif](evidence/verif/)) | Yosys estimate (about 3,100 LUTs, 4 DSP48E1) | | **done in simulation** (independent review: no blocker; its findings fixed) |
| 3 CPU on the board | the real core in `surfsim` runs `bramdiag` to PASS and starts SurfBoot ([evidence](evidence/soc/)); the bitstreams' own synthesized netlists do the same ([evidence](evidence/board/openxc7-soc.txt)) | open flow, both boards: `surfcpu_arty_a7_bramdiag` and `surfcpu_arty_a7_top` (SurfBoot), HAS_DDR=0, every flow check passed | pending | bitstreams ready; waiting for the board |
| 4 DDR integration | bridge tests against a MIG model and LiteDRAM's controller, unbounded proofs ([evidence](evidence/ddr/)) | MIG recipe pending a Vivado host; LiteDRAM generated | pending | bridge done; board top wiring pending |
| 5 Card reads and SurfBoot | SurfBoot: 137 card-image scenarios, 120 through the io SD card model, 29 runs of the ROM in a firmware emulator ([evidence](evidence/fw/)) | | pending | software done on the host; RTL SoC and board pending |
| 6 Standalone I/O | SPI + SD card model, PS/2 + keyboard model, VGA 640x480 text decoded from the pixels ([evidence](evidence/io/)) | Yosys estimates | pending | RTL in simulation; review fixes in progress |
| 7 SurfOS kernel port | host tests, QEMU `virt`, the fw emulator, and on the RTL SoC: SurfBoot boots `SURFOS.BIN` from a card image to the shell, UART and PS/2 input, console decoded from the VGA pixels ([evidence](evidence/soc/test-soc-system-boot.txt)) | | pending | done in simulation; console scroll race being fixed |
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

### 2026-10-16: review rounds, open-flow bitstreams, the whole system in simulation

* fw-2 (c198306): deploy-sd stays on the card (no symlinks followed, bound to the device node)
  and verifies after a remount; isa-audit sees words hidden in code; check 2 requires I.
* cpu-2 (fbb250e): an interrupt enabled just before a WFI is taken before it; a bounded formal
  check of the core's RVFI on `make check`; no interrupt input depends combinationally on a bus
  request (Yosys cone check on the full SoC).
* board-2 (1acdc69): `program.sh` reads openFPGALoader's real output and checks the full part,
  the manifest and the cable; WIRING.md's orientation notes corrected (an upside-down Pmod VGA
  shorts FPGA outputs); `make build-fpga FLOW=openxc7 DESIGN=arty_a7_baseline` builds both boards'
  bitstreams on the Mac; the pixel clock is 25,173,611 Hz.
* soc-2 (5c11ea4): `surfsim` has the SD, PS/2 and VGA models; `test-soc-system-boot` boots SurfOS
  from a card image on the RTL SoC with the real core (about 95 s of wall time, 314 M cycles).
* Found: the kernel console's scroll is not protected against preemption, so concurrent
  printing can leave partial rows on the screen (UART is unaffected); and code fetched from DDR
  one word at a time is slow (no instruction cache yet).

### 2026-10-16: verif merged; every track in

* verif (0992632): Spike's commit log converted to the retirement trace, a strict comparator,
  random programs, CSR probes, riscv-arch-test 4.1.0 and riscv-formal. `make check` now runs
  the real core against Spike (6b71d73): 705 programs, 8.2 million retirements, no divergence.
* All nine tracks are merged; `make check` passes in about 22 minutes on the Mac.
* The owner's guide: [OWNER-GUIDE.md](OWNER-GUIDE.md).

### 2026-10-16: the console lock; the whole SoC as bitstreams

* os-2 (4e06018): one writer at a time on the console (audit C10, shared with x86; x86
  `test-all` passes, run again by the integrator on the merged tree); `scrolltest` on
  `make check`; the kernel heap ends 16 MiB below the RAM the boot information reports.
* board-3 (96677fb): the whole SoC (HAS_DDR=0) through the open flow for both boards, with
  bramdiag or SurfBoot in the boot ROM: every flow check plus the board's netlist and ROM checks;
  the synthesized netlists reach `BRAMDIAG: PASS (203 checks)` and SurfBoot's DDR-absent stop in
  simulation. Expected UART transcripts for the owner are in BOARD.md section 12.
