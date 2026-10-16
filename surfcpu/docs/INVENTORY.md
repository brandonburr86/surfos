# Milestone 0: hardware and toolchain inventory

Brief section 3. Only what was observed is recorded as fact; everything else is marked
**to confirm**, with who or what can confirm it. `make doctor` repeats the probes.

## 1. Development computer (probed 2026-10-15)

| Item | Observed |
|---|---|
| Model | MacBook Pro, Mac16,5 |
| Processor | Apple M4 Max, 14 cores (10 performance, 4 efficiency), arm64 |
| Memory | 36 GB |
| macOS | 15.1.1 (24B2091), Darwin 24.1.0 |
| Free disk | about 84 GiB on the data volume (90% used): enough for the toolchain container; a Vivado install (tens of GB) would be tight |
| Docker | Docker Desktop, engine 29.8.1 arm64, 14 CPUs and 7.7 GiB for the Linux VM |
| Homebrew | `/opt/homebrew`; QEMU 11.1.1 installed (`qemu-system-riscv32`) |
| Native HDL / RISC-V tools | none: no Verilator, Icarus, Yosys, openFPGALoader or RISC-V GCC on the Mac itself |
| Serial tools | `screen` only (no picocom, tio or minicom) |

Because the Mac is Apple Silicon, AMD Vivado cannot run on it natively, and an arm64 Linux
VM cannot run it either (docs/BUILD-HOST.md).

## 2. Toolchain container `surfcpu-toolchain:2026-10-15` (built and probed 2026-10-15)

Built from [`devenv/Dockerfile`](../devenv/Dockerfile): Debian trixie at the snapshot of
2026-10-05, Linux arm64, so it runs at native speed.

| Tool | Version |
|---|---|
| Verilator | 5.032 |
| Icarus Verilog | 12.0 |
| Yosys | 0.52 (`synth_xilinx` for resource estimates) |
| SymbiYosys | v0.52, with z3 4.13.3 and boolector 1.5.118 |
| RISC-V GCC | riscv64-unknown-elf-gcc 14.2.0, binutils 2.44 |
| rv32 runtime | `-march=rv32im_zicsr_zifencei -mabi=ilp32` selects the `rv32im/ilp32` multilib: `libgcc.a` exists for exactly this ISA and ABI (brief section 5) |
| picolibc | 1.8.10 (available; SurfBoot does not use a C library) |
| Spike | riscv-isa-sim at commit `5a760667bf0d` (the reference model) |
| QEMU | 10.0.13 (`qemu-system-riscv32`, for `virt`-machine kernel tests) |
| mtools / dosfstools | 4.0.48 / 4.2 (FAT32 card images for tests) |
| Python | 3.13.5 |

Probe evidence: an `rv32im` test program compiled and linked with this GCC, its `mul` and
`csrr` instructions were checked in the disassembly, and it passed on Spike with
`--isa=rv32im_zicsr_zifencei`. A module that includes the generated register definitions
linted clean in Verilator (`-Wall`) and compiled in Icarus.

## 3. The board and adapters (all to confirm: nothing is connected yet)

On 2026-10-15 no FTDI FT2232 (USB `0403:6010`, the Arty's JTAG/UART bridge) was attached
to the Mac, and there was no `/dev/cu.usbserial-*` port and no FAT volume mounted. The
owner's earlier chats mention the parts below; they are **references, not an inspection**.

| Item | From the chats / Digilent documentation | To confirm |
|---|---|---|
| Board | Arty A7-100T (the chats); the A7-35T is the other variant | **Owner:** the label on the board. **Doctor:** the JTAG IDCODE once `openFPGALoader` is installed and the board is plugged in (`0x0362D093` = 35T, `0x13631093` = 100T) |
| FPGA part / speed grade | 100T: XC7A100TCSG324-1; 35T: XC7A35TICSG324-1L | the IDCODE gives the device; speed grade from the chip marking |
| PCB revision | | **Owner:** the revision printed near the board name (the master XDC lists the revisions it covers) |
| DDR | 256 MB DDR3L, 16-bit (Micron MT41K128M16JT-125 on the documented revisions) | **Owner:** the part number on the memory chip |
| Configuration flash | 16 MB quad-SPI; the vendor changed between revisions | **Owner:** the part number on the flash chip (needed before QSPI programming, milestone 9) |
| USB | FTDI FT2232HQ: channel A JTAG, channel B UART | doctor, once plugged in |
| Clock | 100 MHz oscillator | from the master XDC (board track) |
| Pmod VGA (410-345) | RGB444 + HS/VS on two Pmod connectors | **Owner:** that it is a genuine Pmod VGA, and its revision |
| Pmod MicroSD (410-380?) | SPI-mode microSD socket with card detect | **Owner:** the model and revision. This is the FPGA-side socket; the Mac's card reader alone does not connect a card to the Arty |
| Pmod PS2 (410-094?) | 6-pin mini-DIN with clock and data; supply and level-shifting options | **Owner:** the model, revision and jumper settings; the keyboard's supply voltage must not put 5 V on FPGA pins |
| Keyboard | | **Owner:** a real PS/2 keyboard, or a USB keyboard documented to speak PS/2 (a passive USB-to-PS/2 plug does not translate arbitrary USB keyboards) |
| Monitor and cable | | **Owner:** a VGA monitor (or a VGA input) that accepts 640x480 at about 60 Hz |
| microSD card | 32 GB SanDisk microSDHC (the chats) | **Owner:** capacity and type (SDHC/SDXC). A card larger than 32 GB comes formatted exFAT and must be reformatted FAT32 (only after explicit approval) |
| Card reader | | **Owner:** how the card reaches the Mac (built-in SD slot with an adapter, or a USB reader) |
| Power | USB from the Mac (the board can also take an external supply) | **Owner:** whether a separate supply is available; Pmods and a keyboard add load |

Provisional Pmod allocation (brief section 4; a proposal, not permission to connect
anything): microSD on **JA**, VGA on **JB + JC**, PS/2 on **JD**. The checked wiring table
(adapter pin → connector pin → FPGA pin → direction → I/O standard) is
[WIRING.md](WIRING.md), made from Digilent's master XDC by the board track.

## 4. Gap list

Work continues in simulation and software while these are resolved; each blocks only the
physical step named.

| # | Gap | Blocks | Who resolves |
|---|---|---|---|
| G1 | Exact Arty variant and PCB revision | final pin/part choice for the bitstream (the 35T and 100T pinouts are the same; the part differs) | owner (label) or doctor (IDCODE) |
| G2 | A build host for bitstreams (Vivado on x86-64 Linux/Windows, or a proven alternative) | milestone 1 bitstream and every later FPGA step | owner's choice from docs/BUILD-HOST.md |
| G3 | `openFPGALoader` on the Mac | programming the board (milestone 1) | owner: `brew install openfpgaloader` (the doctor checks it) |
| G4 | Pmod models/revisions and the PS/2 power and level arrangement | connecting adapters; the PS/2 pins especially | owner (labels, jumpers); board track (schematics) |
| G5 | Keyboard type (true PS/2) | milestone 6 keyboard tests | owner |
| G6 | Card capacity and format | milestone 5 boot from the card | owner, with the card in the Mac's reader |
| G7 | Configuration-flash part number | milestone 9 (persistent configuration) | owner (chip marking) |
| G8 | A VGA monitor that shows 640x480 | milestone 6 picture tests | owner |
