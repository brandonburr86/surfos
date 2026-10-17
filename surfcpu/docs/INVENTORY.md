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

## 3. The board and adapters (connected since 2026-10-16)

On 2026-10-15 no FTDI FT2232 (USB `0403:6010`, the Arty's JTAG/UART bridge) was attached
to the Mac, and there was no `/dev/cu.usbserial-*` port and no FAT volume mounted. The
owner's earlier chats mention the parts below; they were **references, not an inspection**.
Since 2026-10-16 the board and the three adapters are connected and in use; the last column
says what the board work showed, with its source (PROGRESS.md, `docs/evidence/physical/`), and
what nothing has shown yet is still **to confirm**.

| Item | From the chats / Digilent documentation | Observed, or to confirm |
|---|---|---|
| Board | Arty A7-100T (the chats); the A7-35T is the other variant | **Observed: an A7-100T**: JTAG IDCODE 0x03631093 (`tools/program.sh detect`, 2026-10-16, PROGRESS.md). The label on the board was not read |
| FPGA part / speed grade | 100T: XC7A100TCSG324-1; 35T: XC7A35TICSG324-1L | the IDCODE gives the device (XC7A100T), and bitstreams built for xc7a100tcsg324-1 load and run; the speed grade from the chip marking is **to confirm** |
| PCB revision | | **Rev. E** (read on the board by the owner, 2026-10-17) |
| DDR | 256 MB DDR3L, 16-bit (Micron MT41K128M16JT-125 on the documented revisions) | LiteDRAM set up for the MT41K128M16 calibrates it, and 256 MiB pass the 572-check diagnostics (`ddr-fixed-flow-2026-10-17.txt`); **owner, to confirm:** the part number on the memory chip |
| Configuration flash | 16 MB quad-SPI; the vendor changed between revisions | **Observed:** a Spansion/Infineon part, JEDEC id 0x012018 (openFPGALoader reads it over JTAG and names it S25FL128S; the S25FL127S shares the id), no block protection ([evidence](evidence/physical/flash-id-2026-10-17.txt), G7) |
| USB | FTDI FT2232HQ: channel A JTAG, channel B UART | **Observed:** FT2232 serial 210319C129C6: `/dev/cu.usbserial-210319C129C60` is channel A (JTAG) and `...C61` channel B (the UART); macOS's own FTDI driver holds both, and openFPGALoader claims the JTAG channel without sudo. JP2 came fitted, which made every opening of the port reset the board; the owner took it off (PROGRESS.md 2026-10-16, WIRING.md U7) |
| Clock | 100 MHz oscillator | from the master XDC (board track); the baseline's line came every 0.99 to 1.01 s by the Mac's clock (m1, `m1-2026-10-16-2.txt`) |
| Pmod VGA (410-345) | RGB444 + HS/VS on two Pmod connectors | **Observed:** in JB+JC (J1 in JB), it shows SurfBoot's text, readable with nothing cut off, and the colour-bar test pattern exactly (`surfboot-card-files-2026-10-16.txt`); **owner, to confirm:** that it is a genuine Pmod VGA, and its revision |
| Pmod MicroSD (410-380?) | SPI-mode microSD socket with card detect | **Observed:** in JA, SurfBoot and SurfOS read the owner's card through it over SPI at 6.25 MHz with CRC checked (`surfboot-card-2026-10-16.txt`, `surfos-boot-2026-10-16.txt`); **owner, to confirm:** the model and revision. This is the FPGA-side socket; the Mac's card reader alone does not connect a card to the Arty |
| Pmod PS2 (410-094?) | 6-pin mini-DIN with clock and data; supply and level-shifting options | **Observed:** in the top row of JD, its JP1 moved to VB during the bring-up (PROGRESS.md 2026-10-16); not yet shown working, since the owner's keyboard does not speak PS/2. **Owner, to confirm:** the model and revision |
| Keyboard | | **Observed:** an Amazon Basics USB keyboard on a passive USB-to-PS/2 adapter; it never acknowledges the port's reset command (STATUS shows TX_NAK, no bytes come back), before and after the JP1 correction: it does not speak PS/2 (PROGRESS.md 2026-10-16). **Still needed:** a real PS/2 keyboard, tried at 3.3 V first (WIRING.md section 4) |
| Monitor and cable | | **Observed:** the owner's VGA monitor shows the 640x480 picture once its input is set to VGA (`surfboot-card-files-2026-10-16.txt`, PROGRESS.md 2026-10-16) |
| microSD card | 32 GB SanDisk microSDHC (the chats) | **Observed:** SanDisk SD32G rev 8.5 (CID manufacturer 03, OEM SD, made 2026-06), SDHC, 62,333,952 sectors (29.7 GiB), MBR with one FAT32 partition (type 0x0c) from sector 8192, 32 KiB clusters; label "NO NAME" as bought, "SURFOS" since the owner set it up (`surfboot-card-2026-10-16.txt`, `surfos-boot-2026-10-16.txt`) |
| Card reader | | the card mounts on the Mac at `/Volumes/SURFOS` (`make deploy-sd`, 2026-10-16); which reader was used was not recorded |
| Power | USB from the Mac (the board can also take an external supply) | **Owner, to confirm:** whether a separate supply is available; Pmods and a keyboard add load |

Provisional Pmod allocation (brief section 4; a proposal, not permission to connect
anything): microSD on **JA**, VGA on **JB + JC**, PS/2 on **JD**. The checked wiring table
(adapter pin → connector pin → FPGA pin → direction → I/O standard) is
[WIRING.md](WIRING.md), made from Digilent's master XDC by the board track.

## 4. Gap list

Work continues in simulation and software while these are resolved; each blocks only the
physical step named. The last column says which the board work has answered (2026-10-16), and
from where.

| # | Gap | Blocks | Who resolves | State |
|---|---|---|---|---|
| G1 | Exact Arty variant and PCB revision | final pin/part choice for the bitstream (the 35T and 100T pinouts are the same; the part differs) | owner (label) or doctor (IDCODE) | **variant answered:** an A7-100T, IDCODE 0x03631093 (`tools/program.sh detect`), its USB cable the FT2232 with serial 210319C129C6 (PROGRESS.md 2026-10-16). **Revision answered:** Rev. E (the owner, 2026-10-17) |
| G2 | A build host for bitstreams (Vivado on x86-64 Linux/Windows, or a proven alternative). Since 2026-10-16 the open flow (BUILD-HOST.md option C, `make build-fpga FLOW=openxc7`) builds checked bitstreams on this Mac, so bitstreams no longer wait for a build host; but its timing is only nextpnr's estimate, it cannot check the clock-crossing and I/O constraints, and the DDR3L needs LiteDRAM (MIG is Vivado's). Its bitstreams run on the owner's board (BOARD.md section 12) | Vivado timing signoff and MIG; whether C is enough is the owner's decision | owner's choice from docs/BUILD-HOST.md | **open** (optional: only the Vivado sign-off of milestones 1 and 4 waits for it) |
| G3 | `openFPGALoader` on the Mac | programming the board (milestone 1) | owner: `brew install openfpgaloader` (the doctor checks it) | **answered:** openFPGALoader 1.1.1, installed by the owner, has loaded every design into the board's configuration RAM (PROGRESS.md 2026-10-16) |
| G4 | Pmod models/revisions and the PS/2 power and level arrangement: where is the Pmod PS2's supply jumper JP1? It must be on VB (the Arty's 3.3 V), not VE (an external 5 V supply would pull the FPGA's PS/2 pins above 3.3 V); and does the keyboard work at 3.3 V (WIRING.md section 4)? | connecting adapters; the PS/2 pins especially | owner (labels, jumpers); board track (schematics) | **partly answered:** the adapters are wired as WIRING.md says, the Pmod PS2's JP1 is on VB, and the MicroSD and VGA adapters work (PROGRESS.md 2026-10-16; `surfboot-card-2026-10-16.txt`, `surfboot-card-files-2026-10-16.txt`). **Open:** the adapters' models and revisions, and a real PS/2 keyboard at 3.3 V |
| G5 | Keyboard type (true PS/2) | milestone 6 keyboard tests | owner | **answered twice:** the first was not a PS/2 keyboard (a USB keyboard on a passive USB-to-PS/2 adapter, which never acknowledges a reset: TX_NAK; PROGRESS.md 2026-10-16); the second, an MCSAITE PS/2 keyboard specified for 5 V, does not start on the Pmod PS2's 3.3 V (no self-test flash, no answer, nothing on the lines: [evidence](evidence/physical/ps2-2026-10-17.txt)). **Open as a need:** a level shifter with 5 V for that keyboard (WIRING.md section 4, question 3), or a keyboard that runs at 3.3 V |
| G6 | Card capacity and format | milestone 5 boot from the card | owner, with the card in the Mac's reader | **answered:** a SanDisk SD32G 32 GB SDHC card, MBR with one FAT32 partition (`surfboot-card-2026-10-16.txt`); SurfBoot boots SurfOS from it (`surfos-boot-2026-10-16.txt`) |
| G7 | Configuration-flash part number. By PCB revision (Arty A7 reference manual section 5.2): Micron N25Q128A13ESF40 on Rev. C and earlier without a sticker; Spansion/Infineon S25FL128SAG[M\|N]FI00 on later revisions up to E without a sticker; Rev. E and later with a sticker, the part the sticker names (S25FL128SAG[M\|N]FI00 or S25FL127SABMFx00) | milestone 9 (persistent configuration) | owner (chip marking, revision, sticker) | **answered:** on the owner's Rev. E board, JEDEC id 0x012018 (Spansion/Infineon, 128 Mbit), read over JTAG by openFPGALoader (`--detect -f`, which writes nothing): it names the part S25FL128S, and the S25FL127S shares the id; either is one RELEASE.md section 6 allows for. Status register 0x00 (no block protection, so no `--unprotect-flash`), configuration register 0x02 (QUAD set) ([evidence](evidence/physical/flash-id-2026-10-17.txt)) |
| G8 | A VGA monitor that shows 640x480 | milestone 6 picture tests | owner | **answered:** the owner's monitor shows the picture once its input is set to VGA (`surfboot-card-files-2026-10-16.txt`) |
