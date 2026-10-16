# SurfCPU commands

Run from `surfcpu/` (`make <target>`) or from the repository root (`make -C surfcpu <target>`).
On the Mac, toolchain targets re-run themselves inside the `surfcpu-toolchain` container;
board, card and build-host targets run on the Mac. Variables: `BOARD=arty_a7_100t` (default)
or `arty_a7_35t`; `DESIGN=arty_a7_baseline` (the milestone 1 design) or `surfcpu_arty_a7_top`
(default) for the bitstream targets; `BIT=file.bit` to program a particular file;
`CABLE_SERIAL=` the serial number of the board's USB cable when more than one FT2232H is
connected; `V=1` shows full commands.

| Command | Runs on | What it does | Status |
|---|---|---|---|
| `make image` | Mac | builds the toolchain container from `devenv/Dockerfile` | works |
| `make doctor` | Mac | probes the Mac, Docker and the container, the board's USB (FTDI, JTAG IDCODE), the serial port, mounted FAT volumes and the build-host settings; changes nothing | works |
| `make gen` / `make check-generated` | container | regenerate / verify the files made from `spec/soc.toml` | works |
| `make lint` | container | Verilator `-Wall` lint of every RTL module | works for the merged areas (lib, io, board); grows with the tracks |
| `make test-cpu` | container | CPU directed tests, random and reference-model (Spike) comparisons, architecture tests | track cpu, verif |
| `make test-soc` | container | building blocks, interconnect, peripherals, the SoC and the whole-system simulation | tracks soc, io, board, ddr |
| `make test-fw` | container | SurfBoot and the card tools on the host, against card images (and through the io track's SD card model), plus the ROM in a firmware emulator | works (host and emulator; not yet on the RTL SoC) |
| `make test-os` | container | the riscv32 SurfOS kernel under QEMU `virt` | track os |
| `make build-rom` | container | `build/fw/surfboot.{elf,bin,mem}`, the BRAM diagnostics `bramdiag.mem` and `DDRDIAG.BIN` | works |
| `make build-kernel` | container | `../build/riscv32/arty_a7/{surfos.elf,surfos.payload.bin,SURFOS.BIN}` | track os |
| `make build-fpga` | Mac → build host | the bitstream, reports and manifest of `DESIGN` in `build/fpga/BOARD/DESIGN/` (docs/BUILD-HOST.md, BOARD.md section 8); `DRY_RUN=1` stages it and prints every command without contacting the host | blocked: no build host |
| `tools/program.sh detect BOARD` | Mac | shows the IDCODE on the JTAG chain and whether it is `BOARD`'s FPGA; loads nothing | pending: board, openFPGALoader |
| `make program-volatile` | Mac | checks for one FT2232H cable (or the one `CABLE_SERIAL=` names), one device on its JTAG chain with `BOARD`'s IDCODE, a bitstream built for that FPGA and package (`DESIGN`'s last build, or `BIT=`) whose manifest passed and matches the file, then loads it into FPGA RAM over USB/JTAG (lost at power-off) | pending: board, openFPGALoader |
| `make program-flash` | Mac | writes the configuration flash (milestone 9): refused unless `SURFCPU_FLASH_CONFIRM=write-flash-BOARD` is given (or the board name typed at the prompt) and the bitstream's `tools/fpga-build.sh` manifest passed for this file, board and part | later |
| `make deploy-sd MOUNT=/Volumes/X` | Mac | installs `SURFOS.BIN` and system files on a mounted FAT32 card, verifies them, keeps user files; never formats; `DEPLOY_ARGS=--dry-run` shows what it would do | written and tested against a stand-in volume; never yet run on a real card |
| `make check` | container | every automated pre-release check | grows with the tracks |
