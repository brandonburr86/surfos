# SurfCPU commands

Run from `surfcpu/` (`make <target>`) or from the repository root (`make -C surfcpu <target>`).
On the Mac, toolchain targets re-run themselves inside the `surfcpu-toolchain` container;
board, card and build-host targets run on the Mac. Variables: `BOARD=arty_a7_100t` (default)
or `arty_a7_35t`; `DESIGN=arty_a7_baseline` (the milestone 1 design), `surfcpu_arty_a7_bramdiag`
(the SoC with the BRAM diagnostics, milestone 3) or `surfcpu_arty_a7_top` (the SoC with SurfBoot;
default) for the bitstream targets; `BIT=file.bit` to program a particular file;
`CABLE_SERIAL=` the serial number of the board's USB cable when more than one FT2232H is
connected; `V=1` shows full commands.

| Command | Runs on | What it does | Status |
|---|---|---|---|
| `make image` | Mac | builds the toolchain container from `devenv/Dockerfile` | works |
| `make openxc7-image` | Mac | builds the open-source bitstream flow's image `surfcpu-openxc7` from `devenv/openxc7/Dockerfile` (about 6 minutes; `devenv/openxc7/build-image.sh --rm-builder` frees its build cache afterwards) | works |
| `make doctor` | Mac | probes the Mac, Docker and the container, the board's USB (FTDI, JTAG IDCODE), the serial port, mounted FAT volumes and the build-host settings; changes nothing | works |
| `make gen` / `make check-generated` | container | regenerate / verify the files made from `spec/soc.toml` | works |
| `make lint` | container | Verilator `-Wall` lint of every RTL module | works for the merged areas (lib, io, board); grows with the tracks |
| `make test-cpu` | container | CPU directed tests, riscv-tests, Icarus, a gate-level run of the synthesized core, a bounded formal check, and the real core compared with Spike on riscv-arch-test, random programs and CSR probes (`test-cpu-ref`) | works (simulation) |
| `make test-soc` | container | building blocks, interconnect, peripherals, the DDR bridge (with proofs), the board's clocks and tops, the SoC, and `surfsim` running the real core with the diagnostics and SurfBoot | works (simulation); SD, PS/2 and VGA models in `surfsim` pending |
| `make test-fw` | container | SurfBoot and the card tools on the host, against card images (and through the io track's SD card model), plus the ROM in a firmware emulator | works (host and emulator; not yet on the RTL SoC) |
| `make test-os` | container | the riscv32 SurfOS kernel: host tests, its SD driver against the io SD card model, QEMU `virt` at -O2 and -O0, and the Arty `SURFOS.BIN` booted by SurfBoot in the fw emulator | works (not yet on the RTL SoC or the board) |
| `make build-rom` | container | `build/fw/surfboot.{elf,bin,mem}`, the BRAM diagnostics `bramdiag.mem` and `DDRDIAG.BIN` | works |
| `make build-kernel` | container | `../build/riscv32/{arty_a7,qemu_virt}/...`; `SURFOS.BIN` packed by `tools/surfimg.py` | works (never booted on the board) |
| `make build-fpga` | Mac → build host | the bitstream, reports and manifest of `DESIGN` in `build/fpga/BOARD/DESIGN/` with Vivado on the host of `~/.config/surfcpu/build-host.conf` (docs/BUILD-HOST.md, BOARD.md section 8); `DRY_RUN=1` stages it and prints every command without contacting the host | blocked: no build host |
| `make build-fpga FLOW=openxc7` | Mac (Docker) | the same with the open-source flow on this Mac (docs/OPEN-FLOW.md), no build-host.conf needed: Yosys, nextpnr and Project X-Ray in the `surfcpu-openxc7` image, every check of the flow, the constraints nextpnr cannot check listed in the output and the manifest; `DRY_RUN=1` prints the commands | works for `arty_a7_baseline`, `surfcpu_arty_a7_bramdiag` and `surfcpu_arty_a7_top` (no DDR yet) on both boards; never yet on a board |
| `tools/fpga-build.sh --fetch [--design D] BOARD` | Mac → build host | brings back the results of the last Vivado run over ssh after a lost connection (Vivado keeps running there under nohup) and gates them as usual | blocked: no build host |
| `tools/program.sh detect BOARD` | Mac | shows the IDCODE on the JTAG chain and whether it is `BOARD`'s FPGA; loads nothing | pending: board, openFPGALoader |
| `make program-volatile` | Mac | checks for one FT2232H cable (or the one `CABLE_SERIAL=` names), one device on its JTAG chain with `BOARD`'s IDCODE, a bitstream built for that FPGA and package (`DESIGN`'s last build, or `BIT=`) whose manifest passed and matches the file, then loads it into FPGA RAM over USB/JTAG (lost at power-off) | pending: board, openFPGALoader |
| `make program-flash` | Mac | writes the configuration flash (milestone 9): refused unless `SURFCPU_FLASH_CONFIRM=write-flash-BOARD` is given (or the board name typed at the prompt) and the bitstream's `tools/fpga-build.sh` manifest passed for this file, board and part | written; tested with the fake openFPGALoader on a release's bitstream (`make test-release`); never run on a board (docs/RELEASE.md section 6) |
| `make deploy-sd MOUNT=/Volumes/X` | Mac | installs `SURFOS.BIN` and system files on a mounted FAT32 card, verifies them, keeps user files; never formats; `DEPLOY_ARGS=--dry-run` shows what it would do | written and tested against a stand-in volume; never yet run on a real card |
| `make board-uart-expect BOARD=...` | container | what the serial port should show for the bramdiag and SurfBoot bitstreams, from the RTL simulation (BOARD.md section 12) | works |
| `make board-netlist-sim` | container | the open flow's synthesized netlists of the SoC bitstreams run bramdiag and SurfBoot in Verilator | works (slow: minutes) |
| `make release [PRERELEASE=1] [SKIP_DDR=1] [DESIGNS=a,b] [BOARDS=a,b] [FORCE=1]` | Mac (Docker) | the release (milestone 9, docs/RELEASE.md): from a clean tree only, with `SOURCE_DATE_EPOCH` = the commit's time, the ROMs and the kernel from empty output directories, every design of `release/release.txt` for both boards through `tools/fpga-build.sh` (every gate as it is), the QSPI flash images, the card's `/BOOT` files as deploy-sd installs them, `SHA256SUMS` and `MANIFEST` in `build/release/<version>-<commit>/`; without the DDR designs only as a pre-release (`PRERELEASE=1`, `<version>-pre-<commit>`) | works: a pre-release (the DDR designs do not build yet) |
| `make release-verify RELEASE=build/release/<name> [DESIGNS=..] [BOARDS=..] [KEEP=1] [REPORT=file]` | Mac (Docker) | clones the release's commit into a new directory, makes the release there again and compares every artifact byte for byte, with the first differing byte and its likely cause | works |
| `make release-check RELEASE=build/release/<name>` | Mac | a release's files against its `SHA256SUMS` and `MANIFEST` | works |
| `make fresh-checkout [COMMIT=..] [REFERENCE=build/release/<name>] [ARGS="PRERELEASE=1 .."] [EVIDENCE=file] [KEEP=1]` | Mac (Docker) | `tools/fresh-checkout.sh`: clones the repository, follows docs/RELEASE.md (images, `make release`), compares the result with the reference | works |
| `make test-release` | container | the release tooling's tests (on `make check`, about 5 s): `tools/bitfile.py`, `tools/release.py` with stand-ins for the builds, `tools/program.sh flash` with the fake openFPGALoader | works |
| `make release-selftest` | Mac | the same tests with the Mac's own `python3`, where the release runs (its paths and Python differ from the container's) | works |
| `tools/bitfile.py info\|flash\|stamp FILE.bit` | Mac or container | a bitstream's configuration options and its configuration time from the flash; its QSPI flash image; the header's date set to an epoch (the open flow runs it with `SOURCE_DATE_EPOCH`) | works |
| `make check` | container | every automated pre-release check (about 20 minutes) | works |
