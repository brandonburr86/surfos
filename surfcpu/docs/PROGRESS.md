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
| 6 Standalone I/O | SPI + SD card model, PS/2 + keyboard model, VGA 640x480 text decoded from the pixels ([evidence](evidence/io/)) | in the full-SoC bitstreams (open flow, both boards) | pending | done in simulation (review findings fixed) |
| 7 SurfOS kernel port | host tests, QEMU `virt`, the fw emulator, and on the RTL SoC: SurfBoot boots `SURFOS.BIN` from a card image to the shell, UART and PS/2 input, console decoded from the VGA pixels ([evidence](evidence/soc/test-soc-system-boot.txt)) | | pending | done in simulation (the console race it found is fixed: audit C10) |
| 8 Persistent files | on the RTL system: files written, ejected, read back after a second boot by name, size and CRC, then checked on the host by `fsck.fat` and mtools (`test-os-persist`); write errors, the card taken out (after a sync and in the middle of a write) and a full card on the RTL system too (`test-os-faults`), both on `make check`; under QEMU with the same driver: the card taken out after each block, a full volume, with what `fsck.fat` finds ([evidence](evidence/os/test-os-persist.txt), [faults](evidence/os/test-os-faults.txt), [CARD.md](CARD.md) section 8) | | pending: needs the DDR design | done in simulation |
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

### 2026-10-16: files on the card (milestone 8 in simulation)

* os-3 (cd36aa0): SurfOS writes to the card. Every shell command that changes files syncs
  before the prompt returns and fails when the card refused; `append`; `eject` keeps a volume
  it could not write; the card-detect watch says when the card was taken out or put back.
* Three shared kernel fixes (both kernels; x86 `test-all`, `unittest` and `test-usb` pass at
  O0 and O2): a failed block write is kept for the next sync instead of dropped (audit B1),
  `eject` no longer says "safe to remove" after losing writes (B2), and blocks were to reach the
  card in the order data, FAT, directory entries, free count (B3). A later review found holes in
  that order, and two regressions from B1 and B3. They are fixed in the entry "the block cache
  and FAT review" below.
* `test-os-persist` on `make check`: on the RTL system, mkdir, write, a 160 KiB copy, write
  over, append, mv and rm, then eject; a second boot reads every file back with its CRC, and
  `fsck.fat` finds the image clean. The port's own code got faster for it (CRC tables, word
  copies, a tight SD data loop): 402 s of wall time became 145 s.
* What FAT32 cannot make safe is written down for the owner in CARD.md section 8.
* Integrator's runs on the merged tree: `make check` passes (23 minutes). x86 `test-all`:
  one TCP timing check failed once while the Mac was loaded by other builds (a burst spread over
  411 ms against a limit of 100 ms; the change does not touch the network stack), and the full
  run passed on the rerun (smoke 311 and files 51 at O0 and O2, that burst 1 ms).

### 2026-10-16: card faults and a full card on the RTL system

* os-4 (88ed9bd): `surfsim` can make the card model fail on cue (`--sd-fault`, `--sd-remove`,
  `--sd-insert`, also "after the nth block"), so `test-os-faults` (on `make check`, about 90 s)
  runs on the RTL system:
  * a refused write is retried;
  * a card that refuses everything makes the command fail at once and keeps `eject` from
    unmounting;
  * a card taken out is seen through surf_spi's card detect within 0.3 s;
  * a card pulled in the middle of a write leaves only what `fsck.fat` repairs;
  * a full card ends a copy with "No space left on device", and `rm` gives the space back.
* Shared fix B4 (`fs/fat.c`, both kernels; x86 suites pass at O0 and O2): a write that runs out
  of space keeps what fitted and gives back the clusters it had linked past the file's end.

### 2026-10-16: the block cache and FAT review (audit B5 to B11)

* An independent review of B1 to B4 ran the real `fs/bcache.c` and `fs/fat.c` over RAM disks
  that refuse writes. It found two regressions:
  * A failed sync in the middle of a rename left the file with two names on one cluster chain.
    The same `mv` again, or an `rm` of either name, then freed the chain under the other name.
  * Blocks that failed to write, on one device, could fill the cache that every device shares.

  It also found that B3's order held less than claimed.
* fs-fix (merged as d7452f4) fixes them, in code both kernels share:
  * A failed sync in the middle of an operation puts back what the operation changed, and the
    operation fails with nothing changed (B5).
  * A device that refuses writes keeps at most 256 changed blocks (128 KiB); past that its
    writes fail at once with EIO (B6).
  * The cache itself enforces the write order, and FAT orders its links, new directories and
    cuts on the disk (B7).
  * Smaller fixes: B8, B9 and B11 (both FAT copies change, or neither).
  * `close`, `fsync`, `umount` and `eject` say when changes did not reach the card, including
    `> file` in the shell (B10).
* New host test `tests/host/test_fatfs.c`, on `make unittest`: 91 checks. Before the fixes 37
  of them fail: 34 on real defects and 3 on behaviour the round changed on purpose
  ([evidence](evidence/os/bcache-review.txt)).
* Still not promised, in CARD.md section 8:
  * The guarantee is a write order, not power-fail atomicity.
  * Two narrow cases can still leave a chain running into free space: a write that stopped
    because the card began refusing partway through, and FAT12 entries that straddle two
    sectors.
* Cost: a growing file now forces a sync each time its chain crosses into another FAT
  sector. Writing 4 MiB to a 1 GiB FAT32 volume takes 24% more requests. A 64 MiB FAT32 test
  volume with 512-byte clusters takes two to three times as many.
* The track's runs:
  * x86: `test-all` at O0 and O2 (smoke 311, files 51), `unittest` and `test-usb` (315 of 315)
    at O0 and O2.
  * surfcpu: QEMU 132 of 132 at both levels, the emulator 18 of 18, `test-os-persist` 17 of 17
    and `test-os-faults` 25 of 25.
  * One riscv32 -O0 timing check failed once under load and passed on the rerun.
* A second independent review of this round is running.
* The integrator's `make check` after the merge stopped on that same check at -O0 ("mtime moved
  as much as the ticks did"). It runs right after boot, before any file is written. The tick
  count was 10 to 15, but mtime had moved more than 16 periods: QEMU was held up between the two
  reads on a loaded Mac. The test now reads both clocks together and gets three tries (6c89831).

### 2026-10-16: the board test kit

* os-5 (merged as c6ff037) gives one command per board milestone,
  `make -C surfcpu board-test M=m1` (also m3, m5, m7, m8 and m9). It uses `tools/uart.py` and
  `tools/board-test.py`, which need only the Mac's own python3. Each run:
  * leads the owner through the steps and reads the board's serial port the whole time;
  * writes `docs/evidence/physical/<m>-<date>.txt` with the transcript.

  Runs against the RTL system say SIMULATION and never go to the physical folder
  ([PHYSICAL-TESTS.md](PHYSICAL-TESTS.md)).
* Rehearsed before there is a board:
  * Against the RTL system through `surfsim --uart-pty`: m3, m5, m7, m8, and m9 with two cold
    boots and a one-minute soak.
  * Against fake boards on pseudo-terminals: m1, and the failure cases (the port going away,
    garbage, silence, a wrong build manifest, a lost second). Each failure case ends in a FAIL
    within seconds.

  `test-os-kit` is on `make check` (about a minute; [evidence](evidence/os/test-os-kit.txt)).
* Nothing has run on the board yet.
