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
| C | **Open-source flow** in an arm64 Linux container: Yosys + nextpnr-xilinx + Project X-Ray (the openXC7 toolchain) | no | nothing from AMD; a toolchain container built from pinned sources | Runs natively and fast on the Mac. Experimental: its timing analysis is approximate and the DDR3 controller would be LiteDRAM, not AMD's MIG. Its bitstreams run on the owner's A7-100T (2026-10-16/17). |

Status, 2026-10-17: **C works for bitstreams, and they run on the owner's A7-100T** (the
baseline, the SoC and both DDR designs, BOARD.md section 12).
`make openxc7-image` builds its image once (about 6 minutes), and `make build-fpga
FLOW=openxc7 BOARD=... DESIGN=arty_a7_baseline` builds the milestone 1 design for either
board with every check of the flow passing (docs/OPEN-FLOW.md, docs/BOARD.md section 8). What
it does not give: Vivado's timing signoff (nextpnr's timing is an estimate), the constraints
nextpnr cannot read (clock-crossing bounds, I/O delays, configuration options; each build
lists them), and AMD's MIG for the DDR3L (LiteDRAM instead).
A and B are not set up: the board track's Vivado scripts behind `make build-fpga` are tested
only against stand-ins.

## How the wrapper is configured

`make build-fpga` reads `~/.config/surfcpu/build-host.conf` on the Mac (never committed):

```sh
FLOW=vivado-ssh          # A: vivado-ssh | B: vivado-docker | C: openxc7 (C needs no file at all)
HOST=builder.example     # A only: an SSH host alias from ~/.ssh/config (key authentication)
REMOTE_DIR=surfcpu-build # A only: working directory on that host
VIVADO=/tools/Xilinx/Vivado/2025.1/bin/vivado   # A and B: the pinned Vivado binary
IMAGE=surfcpu-vivado:2025.1  # B only: an x86-64 Linux image with that Vivado installed
DOCKER=docker                # B only, optional: the docker binary (default docker)
```

`make build-fpga DESIGN=arty_a7_baseline` builds the milestone 1 design instead of the whole
SoC, and `DRY_RUN=1` stages the build and prints every command without contacting the host or
starting Docker (`tools/fpga-build.sh`, docs/BOARD.md section 8). `FLOW=` on the command line
(or in the environment) overrides the file: `make build-fpga FLOW=openxc7` uses option C with
no file at all.

For option A, connect once by hand (`ssh builder.example`) and check the host key's
fingerprint: the wrapper's ssh accepts only hosts already in `~/.ssh/known_hosts`
(`StrictHostKeyChecking=yes`) and only key authentication. Vivado runs on the host under
`nohup`, so a dropped connection does not stop it; `tools/fpga-build.sh --fetch [--design D]
BOARD` brings back the results of the last run later.

Every build writes a manifest next to the bitstream: source revision and dirty state, FPGA
part, constraint-file hash, ROM and kernel hashes, ISA/ABI versions, tool versions, build
host, the timing summary and utilization. The DDR controller and clocking IP are generated
from checked-in recipes (TCL and the MIG project file), never from a hand-made GUI project.

**The manifests' format.** `build/fpga/BOARD/DESIGN/manifest.txt` (TOML, written by
`tools/fpga-build.sh`) carries `manifest_format = 2` since 2026-10-27; one without it is format 1,
the same keys before format 2's. The open flow's own `openxc7/manifest.txt` says `manifest format
2` on its second line (OPEN-FLOW.md section 3). Format 2 adds: `[openxc7] lvs_solver` and
`lvs_timeout`, the LVS proof's route and limit; in the flow's manifest the synthesized netlist,
the routed FASM and the placement by SHA-256 and an `lvs solver` line; and the records of a
recheck (below). Every reader takes formats 1 and 2 and refuses another, which it could misread:
`tools/program.sh`, `tools/board-test.py`, `tools/release.py` (a release's build records). A
change to what a manifest says, or how, bumps the format and changes every reader with it.

**A recheck's manifests.** `make recheck-fpga BOARD=... DESIGN=...` (`tools/fpga-build.sh
--recheck`, BOARD.md section 8) checks a routed open-flow build again without synthesis,
placement or routing, and writes both manifests anew; nobody edits a manifest by hand. The new
`manifest.txt` keeps the build's own keys as they were (run id, revision, build id and time, the
hashes of the sources, constraints and ROM, the placer seeds: the bitstream carries the id and the
time), says `result` and, on a pass, the bitstream's SHA-256 as a build's does, takes the kept
checks (`check_srl`, `check_synthesis`, `check_netlist`, `check_boot_rom`) from the build and
the others from the recheck, and adds a `[recheck]` table: `rechecked_at`, `tools_revision` and
`tools_dirty` (the commit whose tools ran it), `image`, `exit_status`, `lvs_solver`, `lvs_timeout`,
`checks_run_again`, `checks_kept`, `build_result` and `build_reason` (what the build's own
manifest said), and the build's own manifests, kept beside the new ones as `manifest.build.txt`
and `openxc7/manifest.build.txt`, by path and SHA-256. `tools/program.sh` and `tools/board-test.py`
take a recheck's passing manifest as a build's and say it is a recheck's.

## Programming stays on the Mac

Whichever host builds the bitstream, the board is programmed from the Mac over its USB
cable with openFPGALoader (`make program-volatile`), which supports both Arty A7 variants.
That loads FPGA configuration RAM only; it is lost at power-off. Writing the configuration
flash is a separate, explicit command (`make program-flash`, milestone 9) with a recovery
procedure.
