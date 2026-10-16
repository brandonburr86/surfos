# Where bitstreams are built

Everything except bitstream generation runs on the MacBook (natively or in the
`surfcpu-toolchain` container). Synthesis, placement, routing and bitstream generation for
the Artix-7 need a tool that runs somewhere else, because AMD supports Vivado only on
x86-64 Linux and Windows ([AMD installer information](https://www.amd.com/en/support/adaptive-socs-and-fpgas/installer-info-general.html)),
and this MacBook is an Apple M4 Max (arm64).

The decision is the owner's. Until it is made, bitstream generation is **blocked** and every
FPGA result is reported as pending. Everything else (RTL, simulation, firmware, the kernel
port, the card tools) does not depend on it.

## Options

| | Option | Supported by AMD | What it needs | Notes |
|---|---|---|---|---|
| A | **Vivado on an x86-64 Linux machine** reached over SSH (a PC at home, or a cloud VM) | yes | the machine; Vivado ML Standard (free, covers the xc7a35t and xc7a100t) installed there; an SSH key on the Mac | The brief's recommended arrangement. `make build-fpga` copies the sources there, runs the non-project TCL flow, and brings back the bitstream, reports and manifest. No passwords in the repository. |
| B | **Vivado in an x86-64 Linux container on this Mac**, run through Docker Desktop's Rosetta emulation | no (works for many people, unsupported) | the owner's AMD account to download the installer; tens of GB of disk (about 84 GiB is free); a slow first install | Same scripts as A, run locally. Must be proven on this Mac before it is relied on. |
| C | **Open-source flow** in an arm64 Linux container: Yosys + nextpnr-xilinx + Project X-Ray (the openXC7 toolchain) | no | nothing from AMD; a toolchain container built from pinned sources | Runs natively and fast on the Mac. Experimental: its timing analysis is approximate and the DDR3 controller would be LiteDRAM, not AMD's MIG. Must be proven on the board. |

Status, 2026-10-15: none of A, B or C is set up. The board track writes the Vivado
non-project scripts and the `make build-fpga` wrapper (used by A and B); the flow track
tries C for the milestone 1 LED/UART design and records what works.

## How the wrapper is configured

`make build-fpga` reads `~/.config/surfcpu/build-host.conf` on the Mac (never committed):

```sh
FLOW=vivado-ssh          # A: vivado-ssh | B: vivado-docker | C: openxc7
HOST=builder.example     # A only: an SSH host alias from ~/.ssh/config (key authentication)
REMOTE_DIR=surfcpu-build # A only: working directory on that host
VIVADO=/tools/Xilinx/Vivado/2025.1/bin/vivado   # A and B: the pinned Vivado binary
IMAGE=surfcpu-vivado:2025.1  # B only: an x86-64 Linux image with that Vivado installed
DOCKER=docker                # B only, optional: the docker binary (default docker)
```

`make build-fpga DESIGN=arty_a7_baseline` builds the milestone 1 design instead of the whole
SoC, and `DRY_RUN=1` stages the build and prints every command without contacting the host or
starting Docker (`tools/fpga-build.sh`, docs/BOARD.md section 8).

For option A, connect once by hand (`ssh builder.example`) and check the host key's
fingerprint: the wrapper's ssh accepts only hosts already in `~/.ssh/known_hosts`
(`StrictHostKeyChecking=yes`) and only key authentication. Vivado runs on the host under
`nohup`, so a dropped connection does not stop it; `tools/fpga-build.sh --fetch [--design D]
BOARD` brings back the results of the last run later.

Every build writes a manifest next to the bitstream: source revision and dirty state, FPGA
part, constraint-file hash, ROM and kernel hashes, ISA/ABI versions, tool versions, build
host, the timing summary and utilization. The DDR controller and clocking IP are generated
from checked-in recipes (TCL and the MIG project file), never from a hand-made GUI project.

## Programming stays on the Mac

Whichever host builds the bitstream, the board is programmed from the Mac over its USB
cable with openFPGALoader (`make program-volatile`), which supports both Arty A7 variants.
That loads FPGA configuration RAM only; it is lost at power-off. Writing the configuration
flash is a separate, explicit command (`make program-flash`, milestone 9) with a recovery
procedure.
