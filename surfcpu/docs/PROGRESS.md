# SurfCPU progress record

Brief section 12. Each milestone lists its evidence. Simulation, synthesis, programming and
physical results are separate columns, so nothing claims more than happened.

| Milestone | Simulation / host tests | Synthesis / bitstream | On the board | State |
|---|---|---|---|---|
| 0 Inventory and contracts | toolchain probes pass ([INVENTORY.md](INVENTORY.md)) | | | **done** (the board answered most of the owner's hardware facts: INVENTORY.md sections 3 and 4; still open: the PCB revision, the adapters' revisions, a PS/2 keyboard) |
| 1 Toolchain and board baseline | baseline design (LEDs, buttons, 1 Hz UART line, both clocks) passes in Verilator and Icarus ([evidence](evidence/board/)) | bitstreams for both boards from the open flow on the Mac, every flow check passed ([evidence](evidence/board/openxc7.txt)); no Vivado run | **A7-100T** (IDCODE 0x03631093), loaded over JTAG from the Mac: **m1 PASS** on 2026-10-16, 11 checks and the LEDs confirmed by the owner ([evidence](evidence/physical/m1-2026-10-16-2.txt); the first run's FAIL was a test-kit bug: [evidence](evidence/physical/m1-2026-10-16.txt)) | **passed on the board** with the open flow; open: the brief's Vivado post-route timing and DRC reports and the Mac → Vivado host loop (no Vivado host) |
| 2 CPU correctness in simulation | directed tests, riscv-tests, riscv-arch-test, random programs and CSR probes compared with a contract-patched Spike instruction by instruction (8.2 M retirements on every `make check`, 19 M in the verif track's long runs), a bounded formal RVFI check on `make check`, riscv-formal 43 of 44 checks, the synthesized netlist at gate level ([cpu](evidence/cpu/), [verif](evidence/verif/)) | Yosys estimate (about 3,100 LUTs, 4 DSP48E1) | | **done in simulation** (independent review: no blocker; its findings fixed) |
| 3 CPU on the board | the real core in `surfsim` runs `bramdiag` to PASS and starts SurfBoot ([evidence](evidence/soc/)); the bitstreams' own synthesized netlists do the same ([evidence](evidence/board/openxc7-soc.txt)) | open flow, both boards: `surfcpu_arty_a7_bramdiag` and `surfcpu_arty_a7_top` (SurfBoot), HAS_DDR=0, every flow check passed | **m3 PASS** on the A7-100T on 2026-10-16: the BRAM diagnostics' 203 checks (identity, ALU, M extension, branches, loads and stores, CSRs, 56 KiB March C-, 34 exceptions each recovered, timer interrupts and WFI at 1.02 ms a tick, FENCE.I), the UART both ways, the LEDs confirmed by the owner; bitstream d174d0f ([evidence](evidence/physical/m3-2026-10-16.txt)) | **done on the board** (open-flow bitstream; nextpnr's timing is an estimate, no Vivado sign-off) |
| 4 DDR integration | bridge tests against a MIG model and LiteDRAM's controller, unbounded proofs; the DDR top with LiteDRAM's generated core and its DRAM model: the BRAM diagnostics' DDR checks pass (569), and SurfBoot calibrates and boots SurfOS to the shell; the 100T DDR diagnostics' synthesized netlist prints the same lines at the same times as the RTL ([evidence](evidence/ddr/), [the DDR top](evidence/ddr/ddr-top-sim.txt)) | open flow: both DDR designs place and route on both boards, but nextpnr's estimate for LiteDRAM's 100 MHz clock is 63.5 to 77.3 MHz, and one build did not finish routing: no DDR bitstream ([evidence](evidence/board/openxc7-ddr.txt)); MIG recipe pending a Vivado host | SurfBoot 1.1.0 initializes and read-levels the DDR3L on the A7-100T (both lanes bitslip 2, delay 16, window +-8), calibrates in 187 ms and passes its quick test of 256 MiB, then loads SurfOS into it ([evidence](evidence/physical/surfos-boot-2026-10-16.txt)); the full DDR diagnostics PASS on the board (572 checks: calibration, data lines, byte lanes, address lines and aliases, boundaries, patterns, sustained copies, code run from DDR) in two placements, two other placements failed on the silicon until the flow stopped routing through unused BUFGCTRLs; with the fix both pass, and so do the standard builds ([evidence](evidence/physical/ddr-routethrough-fix-2026-10-16.txt), [fixed flow](evidence/physical/ddr-fixed-flow-2026-10-17.txt)) | **physical tests passed** (open-flow bitstreams at 80 MHz, the route-through fix in); open: Vivado sign-off |
| 5 Card reads and SurfBoot | SurfBoot: 137 card-image scenarios, 120 through the io SD card model, 29 runs of the ROM in a firmware emulator ([evidence](evidence/fw/)) | | SurfBoot runs on the A7-100T and stops in its monitor with the reason (DDR_ABSENT) ([evidence](evidence/physical/surfboot-no-ddr-2026-10-16.txt)); with the Pmod MicroSD it reads the owner's 32 GB SDHC card over SPI: identification, CSD/CID, the MBR, the FAT32 volume and its root ([evidence](evidence/physical/surfboot-card-2026-10-16.txt)); after `deploy-sd` it lists `/BOOT` and `/SYSTEM` and reads the whole 388,724-byte `SURFOS.BIN` through its 8 fragments, every byte position matching the Mac's copy ([evidence](evidence/physical/surfboot-card-files-2026-10-16.txt)); SurfBoot 1.1.0 then loads SURFOS.BIN from the card into DDR (CRC checked in both) and starts it ([evidence](evidence/physical/surfos-boot-2026-10-16.txt)); the formal m5 test (DDRDIAG.BIN from the card, the card taken out) not run yet | software done on the host; RTL SoC done; on the board: card reads and the DDR boot shown, m5 pending |
| 6 Standalone I/O | SPI + SD card model, PS/2 + keyboard model, VGA 640x480 text decoded from the pixels ([evidence](evidence/io/)) | in the full-SoC bitstreams (open flow, both boards) | VGA: the video block runs on the board (text mode, 80x30, 60 frames a second, pixel clock 25,173,611 Hz) the owner confirmed SurfBoot's text readable with nothing cut off, and the colour-bar test pattern exact (border, eight bars, every level of each 4-bit channel) ([evidence](evidence/physical/surfboot-card-files-2026-10-16.txt)). PS/2: the owner's keyboard (a USB keyboard on a passive USB-to-PS/2 adapter) never answers a reset (TX_NAK, no clock pulses): it does not speak PS/2; a real PS/2 keyboard is needed | done in simulation; on the board: VGA done, the PS/2 keyboard pending (needs a real PS/2 keyboard) |
| 7 SurfOS kernel port | host tests, QEMU `virt`, the fw emulator, and on the RTL SoC: SurfBoot boots `SURFOS.BIN` from a card image to the shell, UART and PS/2 input, console decoded from the VGA pixels ([evidence](evidence/soc/test-soc-system-boot.txt)) | | SurfOS boots on the A7-100T from the microSD card into DDR: its shell over the UART, uptime, ps, memstat (256 MiB), the two-task test, and (after 22cbdc9) the card mounted and read ([evidence](evidence/physical/surfos-boot-2026-10-16.txt)); VGA console and PS/2 keyboard not yet shown (a real PS/2 keyboard is needed) | done in simulation; on the board: UART shell and tasks done, VGA and keyboard pending |
| 8 Persistent files | on the RTL system: files written, ejected, read back after a second boot by name, size and CRC, then checked on the host by `fsck.fat` and mtools (`test-os-persist`); write errors, the card taken out (after a sync and in the middle of a write) and a full card on the RTL system too (`test-os-faults`), both on `make check`; under QEMU with the same driver: the card taken out after each block, a full volume, with what `fsck.fat` finds ([evidence](evidence/os/test-os-persist.txt), [faults](evidence/os/test-os-faults.txt), [CARD.md](CARD.md) section 8) | | SurfOS mounts the card read-only on the board; writes (sd=rw) and the m8 test not run yet | done in simulation (open: the fifth storage review's findings, being fixed in fs-fix-5) |
| 9 Reproducible standalone release | the release tooling's tests (16 in the container; the self-test with both of the Mac's Pythons) ([evidence](evidence/release/tests.txt)) | a fresh checkout's pre-release 1.0.0-pre-bb066932: the six non-DDR bitstreams pass every gate, and `release-verify` rebuilt all 18 artifacts byte for byte ([evidence](evidence/release/)); the DDR designs now build reproducibly and pass on the board; no release made with them yet | pending: the flash not written (needs the owner's go-ahead), no DDR boot design | tooling done and reviewed (its findings fixed in rel-2); the pre-release must be made again |

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
* A second independent review of this round tested it on the host with scenarios, a fuzzer
  (274,000 random operations with refused writes and read errors) and real threads.
  * It found no deadlock, no order violation on the disk, and no drift in the cache's counters.
  * It found three major defects:
    * A sync can report success while blocks are still unwritten, when other tasks keep
      writing to the same volume. A regression: `close` can say a file is saved when it is not.
    * A failed write can be forgotten when its retries are held back. Also a regression.
    * After one refused write, writes can keep failing without the card being asked again.

  There are also six minor findings. The same author is fixing them in fs-fix-2, with the
  review's tests carried into the repository.
* The integrator's `make check` after the merge stopped on that same check at -O0 ("mtime moved
  as much as the ticks did"). It runs right after boot, before any file is written. The tick
  count was 10 to 15, but mtime had moved more than 16 periods: QEMU was held up between the two
  reads on a loaded Mac. The test now reads both clocks together and gets three tries (6c89831).
* After that fix, on the merged tree (d174d0f):
  * `make check` passes (26 minutes);
  * x86 `test-all` passes at O0 and O2 (smoke 311, files 51);
  * `unittest` passes (37 suites, including the storage test's 91 checks).

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

### 2026-10-16: DDR through the open-source flow (milestone 4)

* ddr-2 (merged as 08081cb):
  * LiteDRAM is generated reproducibly, its files checked against recorded SHA-256 hashes.
  * The board top gains the DDR3L pins and two DDR designs: the BRAM diagnostics with DDR
    checks, and SurfBoot.
  * A small VexRiscv CPU inside LiteDRAM calibrates the memory in about 1 s.
* In simulation, LiteDRAM's controller with its DRAM model:
  * passes the diagnostics' DDR checks (569 in all);
  * reports a failed calibration correctly;
  * lets SurfBoot load SurfOS and reach the shell, at 5.4 s.

  The 100T DDR diagnostics' synthesized netlist prints the same 78 lines, each at the same
  microsecond, as the RTL.
* The open flow places and routes both designs on both boards, but misses the 100 MHz timing of
  LiteDRAM's clock:
  * 100T SurfBoot 63.5 MHz;
  * 35T SurfBoot 73.8 MHz;
  * 35T diagnostics 77.3 MHz;
  * the 100T diagnostics did not finish routing.

  The worst paths are the calibration CPU's bus into its own instruction fetch, and LiteDRAM's
  refresh timer into a bank machine. The gate holds, so no DDR bitstream was written.
  `program.sh` also refuses any bitstream whose flow shows a clock missing timing
  ([evidence](evidence/board/openxc7-ddr.txt), [timing](evidence/ddr/openflow-timing.txt)).
* Found on the way, all in the flow's own checks:
  * a one-pin difference between netlist and bitstream on a block RAM of the calibration CPU;
  * a parsing bug in the clock-tile check;
  * a false synthesis-check finding on block RAM read registers.
* The HAS_DDR=0 designs still pass every check on both boards.
* `make check` passes with the round merged (26c2bd2, 25 minutes).
* Next, ddr-3 (running):
  * take the calibration CPU out of the 100 MHz domain, so that SurfCPU calibrates over
    LiteDRAM's control bus;
  * shorten the refresh path;
  * fix the three findings in the flow's checks.

  Timing is not relaxed to get there. If the open flow cannot close it, the choices go to the
  owner, Vivado among them.

### 2026-10-16: the board, milestone 1

* The Arty enumerates on the Mac as Digilent's FT2232 (serial 210319C129C6):
  `/dev/cu.usbserial-210319C129C60` is JTAG and `...C61` the UART. macOS's own FTDI driver
  holds both channels, yet openFPGALoader 1.1.1 (installed by the owner) claims the JTAG
  channel without sudo.
* Programming:
  * `tools/program.sh detect` read IDCODE 0x03631093: the board is an **A7-100T**
    (INVENTORY.md G1).
  * The baseline was rebuilt at 2d1f32b, and every flow check passed (nextpnr's estimates:
    sys 115 MHz and pix 183 MHz, both constrained to 25 MHz).
  * `make program-volatile` loaded it into configuration RAM (DONE high). The flash was not
    touched.
* The Arty's JP2 was fitted (WIRING.md U7). Raising DTR pulses RESET, and the system raises
  DTR whenever a program opens the port, so every opening restarted the design: the count
  began at 1 each time. Dropping DTR, and RTS either way, did nothing. The owner took JP2 off,
  and after that the count ran on through every DTR and RTS change. The owner's guide and the
  checklists now say the board can come with JP2 fitted.
* `board-test M=m1`, first run: FAIL ([evidence](evidence/physical/m1-2026-10-16.txt)).
  * 10 checks passed and the owner confirmed the LEDs. Only "a line every second by the Mac's
    clock" failed, with gaps of 0.00 s, though the transcript shows the lines a second apart.
  * The cause was a test-kit bug. The owner took a few seconds over the first step, and the
    lines that came meanwhile waited in the buffer; each was stamped with the time the script
    read it.
  * Fixed in f85079e: lines keep the time they arrived (`uart.Link.arrival`), and the evidence
    says when each step was answered. A new self-test case, a stand-in owner who takes 3.5 s
    over the first step, failed the same way before the fix (0.00, 0.00, 0.37, 1.03, 0.98 s).
  * A second kit bug was found on the way (309fd37). The self-test split the stand-in owner's
    command at the spaces in this checkout's path, so `make board-test-selftest` failed on the
    Mac. `make check` runs in a container whose path has no spaces, so it never saw this.
  * The kit's tests pass: 17 of 17 on the Mac, and 21 of 21 in the container
    (`test-os-kit`).
* `board-test M=m1`, second run: **PASS** ([evidence](evidence/physical/m1-2026-10-16-2.txt)).
  The owner confirmed the LEDs, and 11 checks passed:
  * the line's form and its count;
  * a line every 0.99 to 1.01 s;
  * the pixel clock at 25,173,611 Hz (25,173,612 on some lines);
  * the board, id, build and build time against the manifest;
  * the receive pin (`rx 53`), the switches at 0101, BTN0, and RESET restarting the count.

  The tools it ran are those of f85079e (board-test.py sha256 cf3a99d2fb49d5a6, uart.py
  dd6dda3bf7893ce1). The uncommitted changes it notes were these documentation edits.
* Milestone 1 is not closed, because the brief also asks for Vivado's post-route timing and
  DRC reports and the Mac → Vivado host → JTAG loop. With no Vivado host, the open flow's
  nextpnr timing is only an estimate.
* `board-test M=m3`: **PASS** ([evidence](evidence/physical/m3-2026-10-16.txt)).
  * The bitstream is `surfcpu_arty_a7_bramdiag` built at d174d0f: clean, and every flow check
    passed. It is not rebuilt at this commit. The top file has changed since, but only to add
    the DDR design's pins and blocks, which this HAS_DDR=0 design leaves out.
  * The diagnostics' first run came before the port was open, so the script typed Ctrl-R, and
    the second run printed every section ok: 203 checks, `BRAMDIAG: PASS`. The checks were:
    * identity, the ALU, the M extension, branches, loads and stores, CSRs and counters;
    * a March C- test of the 56 KiB scratch RAM, and the spare text RAM;
    * 34 exceptions, each one recovered;
    * timer, software and external interrupts with WFI (10 ticks in 10,186 µs);
    * FENCE.I with code written to RAM.

    DDR was skipped: this bitstream has no DDR controller.
  * The identity lines match the manifest: build d174d0f8, clean, board id 0x100, 25,000,000 Hz.
  * `Arty7` typed came back character by character.
  * The owner saw LD4..LD6 lit, LD7 blinking and LD0 green. LD7 blinking is the firmware's
    heartbeat: at the summary all four green LEDs light, then the echo loop toggles LD7 every
    half second. The test's question said "LD4..LD7 lit"; it now says so.
* That is milestone 3's exit evidence (brief section 11): a physical UART transcript with the
  build identity, the arithmetic and memory results, the timer's progress and deliberate fault
  recovery.
* SurfBoot on the board (`surfcpu_arty_a7_top`, d174d0f, nothing in the Pmod headers): the
  integrator's own capture, not a board-test run
  ([evidence](evidence/physical/surfboot-no-ddr-2026-10-16.txt)).
  * It prints its banner 0.02 s into the capture's first second and says the bitstream has no
    DDR controller, so there is nowhere to load a kernel.
  * It stops in its monitor with DDR_ABSENT (code 1 on LD4-LD7) instead of hanging, which
    matches the owner's guide section 6.
  * `help`, `info` and `sd` answer. `sd` says there is no card, from the open card-detect
    switch. `info` reports 25 MHz sys, a 1 MHz timebase and a 25,173,611 Hz pixel clock.
  * Cosmetic, for the next firmware round: `info` prints `features 7f (uart timer irq gpio spi
    ps2 video ddr)`. The parentheses are a fixed legend of bits 0 to 7, not the bits that are
    set, so it reads as if DDR were there.

### 2026-10-16: the release tooling (milestone 9)

* rel was merged as a05b879 and is described in [RELEASE.md](RELEASE.md).
  * `make release` refuses a dirty tree and builds from empty output directories, with
    `SOURCE_DATE_EPOCH` set to the commit's time. It builds:
    * the ROMs and the kernel;
    * every design for both boards, through every gate;
    * the QSPI flash images, and the card's `/BOOT` files as `deploy-sd` installs them;
    * `SHA256SUMS` and a `MANIFEST`: commit, contract versions, each design's gates, timing
      and resources.
  * A missing DDR design makes it a `-pre-<commit>` pre-release, and only when asked
    (`PRERELEASE=1`). Any other failure stops it.
  * `make release-verify` clones the commit, rebuilds and compares byte for byte, and names the
    first difference and its likely cause. `make release-check` and `make fresh-checkout` are
    also new, and `tools/bitfile.py` reads a bitstream's header and configuration options.
* Results ([evidence](evidence/release/)):
  * The pre-release 1.0.0-pre-bb066932 was made from a fresh checkout in 53 minutes; the six
    HAS_DDR=0 bitstreams pass every gate.
  * `release-verify` rebuilt it from a new clone in 59 minutes: all 18 artifacts and the
    `MANIFEST` match byte for byte.
  * The toolchain and open-flow images, rebuilt with no cache, have the same version records
    and build the same software and baseline bitstreams.
* Nondeterminism found and fixed:
  * the kernel took its build time, because `SOURCE_DATE_EPOCH` never reached the container;
  * `xc7frames2bit` writes the current time into the `.bit` header.

  A SoC built twice gave the same netlist, placement and frames.
* Found for the DDR track, and sent to ddr-3: LiteX writes a hierarchy comment into the
  generated LiteDRAM core in an order that follows the Python interpreter's memory layout (5
  different files from 16 generations; the Verilog is identical in all of them). A fresh
  checkout's `make litedram-gen` therefore fails its own SHA-256 lock, which is why the DDR
  designs are missing from the release.
* The configuration flash ([evidence](evidence/release/flash.txt)):
  * The image is the bitstream without its header, written at address 0: 3,825,788 bytes on
    the 100T, 2,192,012 on the 35T. These are the bytes openFPGALoader `-f` writes.
  * The open flow's bitstreams cannot set configuration options, so the FPGA reads its flash
    over one data line at about 3 MHz: 10.2 s to start on a 100T, 5.8 s on a 35T. Vivado's x4
    at 33 MHz would take a quarter of a second.
  * There is no golden and update (MultiBoot) layout: these streams carry no CRC check, so a
    damaged update would configure instead of falling back. Recovery is JTAG with JP1 off
    (RELEASE.md section 7, and the owner's guide section 9).
  * Nothing was programmed: the flash path was tested against a fake openFPGALoader only.
* For the board (RELEASE.md section 10), all with the owner's go-ahead:
  * writing the flash, and its JEDEC id;
  * starting from it with JP1 on, and the real start-up time;
  * the recovery procedure;
  * m9's ten cold boots and one-hour soak, once a DDR boot design exists.
* `make check` passes with the round merged (a05b879, 25 minutes).
* The independent review found two major defects, both reproduced:
  * **M1.** A release checks each artifact when it is built, but copies them about 50 minutes
    later from shared build directories without checking again. A commit or a `make check` in
    the same worktree during `make release` produced a release named for one commit that
    shipped another commit's kernel and mismatched ROM hashes, and `release-check` passed it.
    Fresh-checkout releases are immune, because they build in their own clone.
  * **M2.** `release-verify` does not check the DESIGNS and BOARDS it is given: a typo rebuilt
    nothing and reported PASS.
* It also found smaller defects:
  * `release-check` and `compare` pass a damaged release: empty sums, unlisted or truncated
    files, a renamed pre-release.
  * The builds inherit the whole environment, so `DRY_RUN=1` shipped an earlier attempt's
    bitstreams.
  * The clean-tree check follows the user's git configuration.
  * Two command-line options delete the directory they are given.
  * `mk/host.mk` does not quote `BIT`, so `program-volatile` or `program-flash` with an absolute
    path under this checkout (it has spaces) is refused. RELEASE.md's recovery needs that path.
  * Small `bitfile.py` and documentation issues.
* What held:
  * `program.sh`'s flash path: the exact arguments, the confirmation, every refusal.
  * The flash image's bytes, against openFPGALoader 1.1.1's parser.
  * The header stamp's place before the checks.
  * A reduced real release and `release-verify`: 11 of 11 artifacts match.
  * Checkout paths with spaces and quotes, the PRERELEASE logic, and the recovery procedure.
  * The MultiBoot reasoning and the boot time.
* Being fixed in rel-2, with the reviewer's scenarios turned into regression tests.

### 2026-10-16: the storage round fs-fix-2, its review; the release fixes

* fs-fix-2 was merged as fd37406 and closes the second storage review's findings as audit
  B12-B19 (docs/CODE-AUDIT.md):
  * B12: a sync's 0 now means everything that was dirty at its start reached the card, even
    while others write. Each change is numbered, and each device keeps per-order lists of its
    dirty blocks. A block that keeps changing is written as a copy.
  * B13: a failed run counts as unwritten.
  * B14: a refusing card is asked again.
  * B15: the dirty share holds for file data, and a refused write changes nothing.
  * B16: a refusing block no longer blocks rm of its own file.
  * B17: mv and create undo their steps.
  * B18: a sync gives up on a stuck holder after 10 s.

  15 new host scenarios fail on d7452f4 and pass now; randomized and threaded campaigns ran
  clean ([evidence](evidence/os/bcache-review.txt)).
* The integrator's runs at fd37406:
  * `make check` passes in 24 minutes, and x86 test-all passes.
  * x86 unittest stopped at an Undertow fixture test, `test_trickle_client_leaves`, which this
    round does not touch. That test closes a client after 0.3 s and expects the server to have
    seen it; the Mac had three other agents' containers busy. Because make stops at the first
    failure, the FAT tests did not run in that pass. Run again at cb6e108 (with rel-2 merged), x86
    unittest passes (FATFS 154 checks, FATFUZZ 4 runs, CONC ok three times), and so does `make
    check` (24 minutes).
* A third independent review found B12's core sound: copies are never torn, the lists stay
  consistent, and there is no lock inversion. It found two major defects, both reproduced:
  * **M1**, a regression from B16: `mv a b` sets b's refused blocks aside before the first order
    point while b's entry is still live. That point can write b's directory block over data
    that never reached the card, and after a second failure the card keeps two names on one
    chain.
  * **M2**: a stack buffer overflow in `aside_failed()` on a damaged volume whose chain loops,
    found with AddressSanitizer.

  It also found five minor ones:
  * the aside list is capped at 16 failed blocks;
  * a partial multi-block write on the Arty's card driver leaves the cache disagreeing with
    the card;
  * `grow_zeros` grows the file on a refused zero-fill;
  * mv's undo is best effort under a double fault;
  * the documented `trim_chain` window shows in the threaded test, 2 of 9 runs at one setting.

  fs-fix-3 is fixing them on a branch from b81ccc9.
* rel-2 was merged as b81ccc9 and closes the release review's findings:
  * M1: everything is built in `build/release-work` under a lock. Each artifact is checked
    when it is read and staged at once, and HEAD, the tree, the images and every staged file
    are checked again before writing.
  * M2: `release-verify` refuses unknown or unbuilt designs and never says PASS for a partial
    check.
  * m1: `release-check` is strict, and `compare` hashes both sides.
  * m2: the builds get an allow-listed environment.
  * m3: the clean-tree check ignores the user's git configuration.
  * m4: only directories the tools marked are ever emptied.
  * m5: `BOARD` and `BIT` are quoted.
  * c1-c3: strict `.bit` headers, a truthful locale record, and the version tied to
    `SURFOS.BIN`.

  37 new tests fail before and pass after, and `test-release` gives 52 OK
  ([evidence](evidence/release/review-fixes.txt)). The round-1 pre-release fails today's
  stricter check, because its MANIFEST names a boot design it lacks: the release has to be
  made again. `make test-release` fails in the container at the intermediate commits
  ebbcebe..8835b31 (fixed in 147fe81).

### 2026-10-16: the adapters on the board

The owner wired the board as in the owner's guide section 3:
* JP1 and JP2 off;
* the Pmod MicroSD in JA, the Pmod VGA in JB+JC with J1 in JB, the Pmod PS2 in JD (its JP1
  corrected to VB later);
* a VGA monitor, and a keyboard;
* the owner's 32 GB card.

The integrator's own captures, not board-test runs
([evidence](evidence/physical/surfboot-card-2026-10-16.txt)):
* The baseline, which drives no Pmod pin, runs normally with the adapters attached.
* SurfBoot (d174d0f) starts as before. With the card in, its monitor reads the card over SPI:
  * a SanDisk SD32G SDHC card, ready in 173 ms, data at 6.25 MHz with CRC checked;
  * its MBR, with one FAT32 partition of 30,432 MiB, 32 KiB clusters, label "NO NAME";
  * an empty root directory. The card was as it came from the shop; SurfOS is not installed
    on it yet.
* VGA:
  * the video block's registers show text mode, 80x30, the frame counter advancing at 60 Hz
    and a 25,173,611 Hz pixel clock;
  * the owner's monitor showed nothing until its input was switched to VGA. After that the owner
    confirmed that SurfBoot's text is readable with nothing cut off.
  * The colour-bar test pattern (video CTRL MODE 0, set with `mw`) is exactly as designed, the
    owner confirmed: the white border all round, the eight bars, and the four 16-step bands of
    grey, red, green and blue.
* PS/2: the keyboard is an Amazon Basics USB keyboard on a passive USB-to-PS/2 adapter.
  * A reset command (0xFF, through SurfBoot's `mw` into the port's TXDATA) gets no
    acknowledge: STATUS shows TX_NAK, there are no bytes and no error counts. This held both
    before and after the owner corrected the Pmod PS2's JP1 to VB.
  * A USB-only keyboard does not speak PS/2 through a passive adapter, so a real PS/2 keyboard
    is needed. Whether that one runs at 3.3 V is the next question (WIRING.md section 4).
* SurfOS on the card: the owner ran `deploy-sd` (package SurfOS 1.0.0 build fd374065, 388,724
  bytes, verified on the Mac). SurfBoot then:
  * lists `/BOOT/SURFOS.BIN` and `/SYSTEM/SURFCARD.TXT`;
  * shows the kernel's chain: macOS wrote it in 8 fragments, 12 clusters, and the chain covers
    the size;
  * prints the whole file with `cat`. Every byte position agrees with the Mac's copy; the
    92,146 printable bytes are compared exactly
    ([evidence](evidence/physical/surfboot-card-files-2026-10-16.txt)).
* What the board can show next needs the DDR design (m5: boot a DDR diagnostic image from this
  card; m7, m8, m9) or a real PS/2 keyboard (milestone 6's keyboard part).

### 2026-10-16: DDR on the board, SurfOS boots on the board

* ddr-3 was merged as cf107dc: the first DDR bitstreams, at 80 MHz, and SoC 0.2.0 (map 2).
  `make check` passes there (25 minutes).
* On the A7-100T, the integrator's own captures:
  * **SurfBoot 1.1.0** (`surfcpu_arty_a7_ddr`, 92a2b51) initializes the DDR3L itself.
    * Read leveling finds bitslip 2, delay 16 and a window of ±8 on both lanes.
    * It calibrates in 187 ms, and its quick test of all 256 MiB passes.
    * It loads `SURFOS.BIN` from the card into DDR (the CRC matches on the card and in DDR) and
      starts it.
  * **SurfOS boots.** It reports a 237 MB heap, interrupts, the UART, GPIO and PS/2, and its
    banner, then the shell `SURF ≋#>` over the UART.
    * `uptime` keeps time with mtime.
    * `ps` shows 8 tasks, and `memstat` 256 MiB with the heap check ok.
    * `tasks` (two kernel tasks that sleep and wake) passes.
  * **The SoC 0.2.0 diagnostics without DDR** (`surfcpu_arty_a7_bramdiag`, e396177) pass:
    203 checks.
  * **The DDR diagnostics** (`surfcpu_arty_a7_ddr_bramdiag`, 92a2b51) fault deterministically
    after printing "S".
    * The trap is reported at `lw s0,8(sp)` in `__riscv_restore_0` (after `video_putc`), with
      an odd target `0x5451` that no jump can make.
    * This happens before any DDR access, and only in this placement. ddr-3 is investigating
      and building candidates ([evidence](evidence/physical/ddr-bramdiag-fault-2026-10-16.txt)).
* **The kernel's SD driver failed on the real card:** `+SD: out of range (ACMD41 failed)`.
  * Cause: it sent ACMD41 in the byte right after CMD55's response. A real card needs 8 clocks
    in between (N_RC), and the owner's SanDisk answered with a parameter error. SurfBoot, which
    deselects between commands, read the card fine.
  * Both card models now count such a command as a host mistake. The driver's tests failed with
    them, and pass with one 0xFF byte after CMD55. ACMD41's limit is now 2 s, as SurfBoot's
    (22cbdc9). `make check` passes at 22cbdc9 (24 minutes).
  * On the board, SurfOS 1.0.0 build 22cbdc9a then identifies the card (SDHC 29 GB, CRC
    checked), mounts the FAT32 volume read-only (`sd=rw` on the command line would allow
    writes), lists `/BOOT`, reads `SURFCARD.TXT`, and `df` reports 29.7 GB.

### 2026-10-16: the DDR diagnostics on the board

ddr-3 built five candidates, every gate passing on each
([evidence](evidence/physical/ddr-candidates-2026-10-16.txt)):
* **The failing 92a2b51 placement with two altered ROMs** (`remu` removed from `video_putc`; the
  video console off): both fault exactly like the original. So the divider and `video_putc` are
  not the trigger.
* **The design at 69ee491 with placer seeds 1 and 2: BRAMDIAG PASS, 572 checks.** Read leveling
  works on both lanes (bitslip 2, delay 16, ±8), and every DDR test passes:
  * initialization and calibration;
  * data lines (192 checks), byte lanes, address lines and aliases (54), boundaries (98);
  * 1 MiB of patterns;
  * sustained copies at 720 KiB/s;
  * code copied from BRAM into DDR and run there.

  These are milestone 4's physical exit tests.
* **Seed 3 at 69ee491 fails** in another way, with all multiplier DSPs in DSP_R tiles:
  * the identity values print scrambled;
  * scratch RAM bit 20 reads 0 in the walking-ones test;
  * FENCE.I's patched code returns 0x51;
  * DDR lane 0 finds no working delay.
* So two of five placements of the same netlist fail on the silicon, each differently, though
  LVS, round trip and nextpnr's timing all pass. The original's "dropped bits" and seed 3's stuck
  scratch-RAM bit both point at a block RAM (or LUT RAM) pin or constant tie-off that the open
  flow's database gets wrong in some placements.
* ddr-3 is comparing the failing and passing placements for a cause the flow can remove, and a
  check that flags it. Until then a DDR bitstream is trusted only after it passes on the board;
  the release's reproducible bytes keep such a build.

### 2026-10-16: storage round fs-fix-3 and the fourth review

* fs-fix-3 was merged as 15f0f3c. It closes the third review's findings as audit B20-B26:
  * B20: mv sets the replaced file's refused blocks aside only after its entry is deleted.
  * B21: every failed block is considered and chain walks are bounded, which removes the stack
    overflow on a looping chain.
  * B22: a refused straight write drops the clean copies of its range, so the cache agrees with
    the card after a partial CMD25.
  * B23: the zero gap of a write past the end is all or nothing.
  * B24: a failed undo makes the volume read-only until it is unmounted.
  * B25: trim_chain frees nothing when its order point fails.
  * B26: each sync keeps its own refusal count, and set-aside blocks are other syncs' debt.

  The host tests have 213 checks, each new one failing on fd37406, and are clean under ASan and
  UBSan.
* The integrator's runs at 15f0f3c all pass:
  * x86 test-all, twice at O0 and O2: smoke 311 and files 51 each, with no telnet failure;
  * unittest: FATFS 213/0, FATFUZZ 4/0, CONC ok ×4;
  * `make check` in 26 minutes.
* A fourth independent review confirmed B20-B26 and ran 1.4 M operations clean under
  ASan/UBSan. It found:
  * **Critical, from before this round:** a shrinking truncate whose directory read fails undoes
    with 32 uninitialized stack bytes. The next sync writes them over the file's entry: the
    file is renamed to junk and its clusters are lost, or the directory ends early and its other
    files vanish. Both outcomes were reproduced.
  * **Major, a regression of B23 with B25:** a refusing block in a cut-off chain stays owed by
    every sync while the chain is pending, so every order point and sync fails. rm of the file
    cannot clear it, which breaks B16's guarantee.
  * **Minor:**
    * B24 also turns the volume read-only when nothing had been changed;
    * a failed mkdir frees its cluster with no order point (from before this round);
    * a 9th pending chain is lost without a word;
    * a long set-aside window can make other syncs time out.

  fs-fix-4 is fixing them, the reviewer's scenarios becoming tests.

### 2026-10-17: the cause of the board faults, fixed in the flow

* ddr-3 found it by comparing every FASM feature of seven builds. nextpnr routed ordinary nets
  through unused BUFGCTRL route-through pips (I0 → O), onto a global clock line and back into
  logic.
  * The chip database lists the path; Project X-Ray marks it "always".
  * The FASM configures no buffer for it, so the round trip and LVS see a wire. On the silicon
    the nets never arrived.
* How that explains each failing build:
  * **92a2b51 seed 1** had seven route-throughs into the EX stage: the branch-target adder, ALU
    decode, and the `ex_insn`, `mem_tval` and `trap_epc` registers. Hence a trap no correct core
    can raise.
  * **69ee491 seed 3** had one, into the shifter's bits 20/21, and every symptom is a shift
    through it: `%x` printing, the walking-one test, FENCE.I's mask, and lane 0's leveling
    compare.
* The fix: the flow now routes with a chip database without the 32 BUFGCTRL route-through pips.
  * It is generated from the image's own generator. Run unchanged, the generator reproduces the
    image's databases byte for byte, and the fixed databases' SHA-256 hashes are checked.
  * `gclkcheck` fails any build that drives a global line from an unconfigured BUFGCTRL, or a
    clock line into a data input. Its self-test is on `make test-flow`.
  * Merged as the commit after 98d6460.
* **On the board, both failing placements**, rebuilt with only the database changed (same
  netlists, placements, ROMs and seeds), **pass all 572 checks**, DDR included
  ([evidence](evidence/physical/ddr-routethrough-fix-2026-10-16.txt)).
* Every open-flow bitstream built before this fix is suspect until rebuilt or checked by
  gclkcheck. The board's SurfBoot DDR build (92a2b51) and the e396177 builds are clean. The
  passing seed 1 at 69ee491 had one route-through, into the timer's MSIP flip-flop, which
  happened to carry its signal.
* This is a defect in the open toolchain's chip database that any openXC7 user can hit, and it
  is worth reporting upstream (the owner's call).
* `make check` passes with the fix merged (0bcadbc, 26 minutes).
* The standard 100T DDR builds, made in the main worktree with the fixed flow (0bcadbc), pass
  every gate. gclkcheck reports 7 global buffers configured and 0 route-throughs in each.
  * On the board, `surfcpu_arty_a7_ddr_bramdiag` passes all 572 checks.
  * `surfcpu_arty_a7_ddr` (SurfBoot 1.1.0) calibrates the DDR3L and boots SurfOS from the
    owner's card to its shell.
  * Evidence: [evidence](evidence/physical/ddr-fixed-flow-2026-10-17.txt).
* From now on the board runs these builds, loaded with the standard commands, and no longer
  from a track's worktree.


### 2026-10-17: storage round fs-fix-4 and the fifth review

* fs-fix-4 was merged as 42a45d4. It closes the fourth review's findings as audit B27-B33:
  * B27 (the critical one): a shrinking truncate's undo starts from a zeroed entry and puts it
    back only when it changed it. Every other undo records only what it changed.
  * B28: the blocks of a chain cut off or given back are dropped, so later syncs do not owe
    them. A chain about to lose its last reference has its dirty blocks set aside for that
    order point.
  * B29: an undo can make the volume read-only only for a change that was made.
  * B30: a failed mkdir whose name reached the card frees its cluster only after a later sync
    works.
  * B31: 64 chains can wait to be freed. One more is reported lost on the console, and the
    unmount says how many clusters may be left allocated.
  * B32: blocks are set aside right before their order point, then put back or dropped right
    after it.
  * B33: a copy that needs a block another task's operation set aside lets go of its own block
    and looks again, so the two tasks never wait for each other.

  The host tests have 254 checks. Each new one fails on 15f0f3c, except two: B22b (only
  B22's wording changed) and B30b (that case behaves as before). They are clean under ASan
  and UBSan.
* The integrator's runs at 42a45d4 all pass:
  * x86 test-all at O0 and O2: smoke 311 and files 51 each, with no telnet failure;
  * unittest: FATFS 254/0, FATFUZZ 4/0, CONC ok ×4;
  * `make check` in 24 minutes.
* A fifth independent review confirmed B27, B29-B32, and B33 where the copy waits for the
  block that gets set aside. It ran 1.57 M threaded and 133 k randomized operations clean,
  with no lost clusters. It found:
  * **Major, B33 incomplete:** a copy whose dependency is set aside while it writes another
    block still reaches the card. With B28, any recently written file can be that dependency.
    An rm, truncate or mv at that moment can leave the card naming a file over another file's
    old bytes until the next sync that works. Reproduced at the cache level.
  * **Major, a gap in B28:** a directory that cannot grow frees its new cluster, but its
    refused zero block stays owed. Every sync, rm and mkdir then fails for as long as that
    sector refuses writes. The code predates this round, but B28 claimed to cover it.
  * **Minor:**
    * the unmount marks the volume clean right after reporting lost clusters, so no checker
      runs on it;
    * by reading, a table point (a file crossing into another FAT sector) does not drop a
      waiting chain's blocks.
  * fs-fix-4's one watchdog: the review found no cycle by reading and did not see it again
    in 90 runs. The likeliest cause is a host stall, since the host was swapping.

  fs-fix-5 is fixing them, the reviewer's scenarios becoming tests. The file test on the
  owner's card (m8) waits for it.
