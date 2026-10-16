# SurfCPU commands

Run from `surfcpu/` (`make <target>`) or from the repository root (`make -C surfcpu <target>`).
On the Mac, toolchain targets re-run themselves inside the `surfcpu-toolchain` container;
board, card and build-host targets run on the Mac. Variables: `BOARD=arty_a7_100t` (default)
or `arty_a7_35t`; `V=1` shows full commands.

| Command | Runs on | What it does | Status |
|---|---|---|---|
| `make image` | Mac | builds the toolchain container from `devenv/Dockerfile` | works |
| `make doctor` | Mac | probes the Mac, Docker and the container, the board's USB (FTDI, JTAG IDCODE), the serial port, mounted FAT volumes and the build-host settings; changes nothing | works |
| `make gen` / `make check-generated` | container | regenerate / verify the files made from `spec/soc.toml` | works |
| `make lint` | container | Verilator `-Wall` lint of every RTL module | grows with the tracks |
| `make test-cpu` | container | CPU directed tests, random and reference-model (Spike) comparisons, architecture tests | track cpu, verif |
| `make test-soc` | container | building blocks, interconnect, peripherals, the SoC and the whole-system simulation | tracks soc, io, board, ddr |
| `make test-fw` | container | SurfBoot and the card tools on the host, against card images | track fw |
| `make test-os` | container | the riscv32 SurfOS kernel under QEMU `virt` | track os |
| `make build-rom` | container | `build/fw/surfboot.{elf,bin,mem}` (and the diagnostics) | track fw |
| `make build-kernel` | container | `../build/riscv32/arty_a7/{surfos.elf,surfos.payload.bin,SURFOS.BIN}` | track os |
| `make build-fpga` | Mac → build host | the bitstream, reports and manifest (docs/BUILD-HOST.md) | blocked: no build host |
| `make program-volatile` | Mac | checks the board's IDCODE against `BOARD`, then loads the bitstream into FPGA RAM over USB/JTAG (lost at power-off) | pending: board, openFPGALoader |
| `make program-flash` | Mac | writes the configuration flash; separate, asks for confirmation (milestone 9) | later |
| `make deploy-sd MOUNT=/Volumes/X` | Mac | installs `SURFOS.BIN` and system files on a mounted FAT32 card, verifies them, keeps user files; never formats | track fw |
| `make check` | container | every automated pre-release check | grows with the tracks |
