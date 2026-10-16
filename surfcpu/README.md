# SurfCPU

SurfCPU is our own 32-bit RISC-V processor (RV32IM + Zicsr + Zifencei, written in Verilog)
and the small system around it, built to run a RISC-V port of SurfOS on a Digilent Arty A7
FPGA board. It boots from a microSD card, shows a SurfOS shell on a VGA monitor and takes a
PS/2 keyboard. Everything is developed from the MacBook Pro. The owner's brief is
[docs/AGENT-INSTRUCTIONS.md](docs/AGENT-INSTRUCTIONS.md).

This lives on the `surfcpu` branch of the SurfOS repository, apart from `ai-dev` (the x86
kernel), which stays as it is. The hardware, the boot firmware and the SurfOS port are three
projects held together by one set of contracts.

## Status

[docs/PROGRESS.md](docs/PROGRESS.md) is the running record. In short: the CPU, the SoC,
SurfBoot and the SurfOS kernel pass in simulation, up to SurfBoot booting SurfOS from a card
image on the RTL with the real CPU; bitstreams build on the Mac with the open-source flow
([docs/OPEN-FLOW.md](docs/OPEN-FLOW.md)); nothing has run on the board yet (it is not
connected). **The owner's step-by-step guide is [docs/OWNER-GUIDE.md](docs/OWNER-GUIDE.md).**

## Getting started on the Mac

```sh
open -a Docker                 # Docker Desktop must be running
cd surfcpu
make image                     # build the toolchain container (once, about 2 minutes)
make doctor                    # what the Mac, the container, the board and the card look like
make check                     # every automated test
```

All tools (Verilator, Icarus Verilog, Yosys, RISC-V GCC, Spike, QEMU) live in the
`surfcpu-toolchain` container, pinned to a Debian snapshot: nothing is installed on the Mac.
Programming the board needs `openFPGALoader` on the Mac (`brew install openfpgaloader`).

## Commands

[docs/COMMANDS.md](docs/COMMANDS.md): `doctor`, `lint`, `test-cpu`, `test-soc`, `test-fw`,
`build-rom`, `build-kernel`, `build-fpga`, `program-volatile`, `deploy-sd`, `check`.

## Contracts

| Document | What it fixes |
|---|---|
| [spec/soc.toml](spec/soc.toml) → [docs/MEMORY-MAP.md](docs/MEMORY-MAP.md) | addresses, registers, clocks, board IDs; generated into Verilog, C and linker files |
| [docs/ISA.md](docs/ISA.md) | the implemented ISA, CSRs, traps, the core's ports, the retirement trace |
| [docs/SURFBUS.md](docs/SURFBUS.md) | the bus between the CPU, memories and peripherals |
| [docs/BOOT-ABI.md](docs/BOOT-ABI.md) | `SURFOS.BIN`, the handoff to the kernel, the card layout |
| [docs/MODULES.md](docs/MODULES.md) | module ports, simulation models, file formats |

## Layout

```
spec/              soc.toml: the one source for the memory map and registers
rtl/cpu/           the SurfCPU core
rtl/soc/           SurfBus interconnect, memories, the SoC
rtl/peripherals/   UART, timer, interrupt controller, GPIO, SPI, PS/2, VGA text
rtl/lib/           shared building blocks (synchronizers, FIFO, clock crossings)
rtl/generated/     generated from spec/ (do not edit)
boards/arty_a7/    the board's top level, constraints, clocks, Vivado scripts
ip/                vendor IP recipes (DDR controller, clocking)
sim/               testbenches and C++ models (SD card, PS/2 keyboard, VGA capture, UART, DDR)
firmware/          SurfBoot, diagnostics, shared headers and linker files
tools/             generators, packers, the card installer, build-host and programming wrappers
devenv/            the toolchain container
docs/              contracts, guides, plans, evidence
build/             generated output (ignored)
```

The SurfOS port itself is outside this directory, next to the x86 kernel it shares code with:
`arch/riscv32/`, `include/arch/riscv32/` and `boards/` at the repository root
([docs/PORTING-RISCV.md](docs/PORTING-RISCV.md)).
