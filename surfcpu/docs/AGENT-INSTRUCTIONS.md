# SurfCPU on Arty A7 — Development Agent Instructions

**Prepared:** October 15, 2026  
**Workspace:** `/Users/brandonburr/Documents/SurfOS Restoration Project/06-restoration`  
**Purpose:** Project brief and ordered development outline for an agent building SurfCPU and porting SurfOS. This document does not assert that the CPU, FPGA bitstream, or RISC-V port already exists.

## 1. Your assignment

Build **SurfCPU**, our own synthesizable **32-bit RISC-V processor in Verilog**, and the surrounding system needed to run a RISC-V port of SurfOS on the **Digilent Arty A7**. Develop from a **MacBook Pro**. Use the owner's microSD card for both the kernel image and persistent files, the VGA adapter for a monitor, and the PS/2 adapter for keyboard input.

The first complete release must let the owner:

1. Build the hardware and software with documented, repeatable commands.
2. Program the Arty over its existing USB/JTAG connection.
3. Put the RISC-V kernel and supporting files on a FAT32 microSD card using the MacBook.
4. Power on the board and boot SurfOS from the card into DDR3 RAM.
5. Use a SurfOS shell on a VGA monitor with a PS/2 keyboard.
6. Create, read, change, and save files on that same card, then read them after a reboot and on macOS.

Treat this as three coordinated projects: **CPU/SoC hardware**, **SurfBoot firmware**, and **SurfOS architecture port**. Each needs independent tests before full-system integration. Prefer clear, correct hardware over speculative performance work.

## 2. Decisions carried forward from the owner's chats

The following discussions were reviewed:

| Conversation | Relevant context |
|---|---|
| [FPGA vs ASIC Comparison](https://chatgpt.com/c/6ac5b23c-64a0-832b-8a8f-e838d7da0f93) | A real FPGA implementation of a custom CPU; convergence on RISC-V32; RV32IM discussed; Arty A7-100T suggested; VGA and PS/2 peripherals; existing x86 SurfOS needs a port. |
| [A7 FPGA complexity overview](https://chatgpt.com/c/6aced026-5df0-8329-bc63-90945adb3244) | Five-stage pipeline discussed; microSD/FAT32 for kernel and files; boot ROM loads RAM; separate hardware/software updates; USB/JTAG and UART; Vivado's host requirements. |
| Owner's instruction for this brief | Verilog, RISC-V32, Arty A7, acquired microSD/VGA/PS2 hardware, MacBook Pro development, and direct Mac access to the card. |

**Resolve earlier alternatives in favor of this brief:**

- Implement our own CPU RTL. Existing cores may be references or verification aids; substituting PicoRV32, VexRiscv, NEORV32, MicroBlaze, or a generated SoC is not completion of the custom CPU assignment.
- Use standard RISC-V encodings and **32 integer registers**, including hardwired-zero `x0`. The earlier illustrative 16-register proprietary ISA is superseded.
- Preserve the five-stage pipeline as the initial release's hardware goal. A simple multicycle RV32I core is an acceptable intermediate milestone, not a silent replacement for that goal.
- Use the **MacBook Pro**, superseding the earlier Mac Studio examples. Inspect its actual processor and macOS version; do not assume Intel or Apple Silicon.
- Use removable microSD as the first software deployment method. Serial upload and Ethernet updates are later improvements.
- Earlier chat memory maps were examples. Adopt one documented map and generate matching hardware/software definitions.

The chats discussed a **100T board**, **Pmod MicroSD 410-380**, **Pmod VGA 410-345**, **Pmod PS2 410-094**, and a **32 GB SanDisk microSDHC**. These are historical references, not an inspection of the hardware now present. Confirm labels and revisions before assigning pins or generating a bitstream.

## 3. First actions: inventory and preserve the existing project

- [ ] Read `CLAUDE.md`, `docs/README.md`, `docs/ARCHITECTURE.md`, `docs/ROADMAP.md`, and `REBUILD-NOTES.md`.
- [ ] Inspect current branch, worktree, uncommitted changes, and any newer SurfCPU work. Do not overwrite unrelated work or the original restoration.
- [ ] Record the exact Arty variant, FPGA part/speed grade, PCB revision, DDR part, configuration-flash part, adapter models, keyboard, display, cables, card capacity, and available power supply.
- [ ] Confirm that there is an **FPGA-side microSD socket/adapter**. A microSD card and Mac USB card reader alone do not connect storage to the Arty.
- [ ] Confirm a real PS/2 keyboard or a keyboard known to support PS/2 signaling. A passive USB-to-PS/2 plug does not translate arbitrary USB keyboards.
- [ ] Inspect installed simulators, cross-compilers, programmer, and available Vivado host. Record versions and actual probe results.
- [ ] Produce a short gap list. Continue simulation and software work while missing hardware details are resolved; block only the dependent physical action.

Keep the restored x86 build working. Existing repository guidance describes x86/Multiboot conventions; apply it to that target, while isolating the explicitly requested RISC-V target behind architecture and board boundaries. Preserve original file headers and restoration provenance. A document-only handoff does not require changing the current branch or committing unrelated files.

## 4. Hardware and MacBook development workflow

### Board resources and wiring

Digilent documents Arty A7 variants with 256 MB DDR3L, 16 MB QSPI configuration flash, USB/JTAG, USB/UART, four Pmod connectors, and 10/100 Ethernet. These resources still require our RTL and software interfaces. Confirm the particular board revision against the [Digilent board information](https://digilent.com/shop/arty-a7-100t-artix-7-fpga-development-board/).

Make a checked wiring table containing **adapter pin → board connector/pin → FPGA package pin → direction → voltage/I/O standard**. Start from the matching [Digilent master constraints](https://github.com/Digilent/digilent-xdc), not a hand-invented pin list. The [A7-100 master XDC](https://github.com/Digilent/digilent-xdc/blob/master/Arty-A7-100-Master.xdc) explicitly identifies its supported PCB revisions.

For genuine Digilent adapters, plan provisionally for VGA on **JB + JC**, microSD on **JA**, and PS/2 on **JD**. VGA uses two Pmod connectors, not one. Confirm mechanical fit, orientation, timing, and every pin before adopting this allocation. The Arty reference manual identifies JB/JC as high-speed ports and JA/JD as standard ports. This allocation is a proposal, not permission to connect unknown hardware. See the [Digilent-authored Arty reference manual, sections 7 and 10](https://media.digikey.com/pdf/Data%20Sheets/Digilent%20PDFs/Arty_A7_RM_Web.pdf).

For PS/2, verify the actual adapter schematic, pull-ups, keyboard supply, and level shifting. FPGA-side signals must remain within the board's 3.3 V I/O limits; do not assume an external keyboard supply also makes its signal levels safe. Use open-drain behavior where the host drives PS/2 lines. See [Pmod PS2](https://digilent.com/shop/pmod-ps2-keyboard-mouse-connector/).

### Tools and where they run

| Task | Intended environment |
|---|---|
| Edit Verilog, C, assembly, tests, constraints, documentation | MacBook Pro |
| Lint/simulate RTL | Mac-compatible Verilator and/or Icarus Verilog; testbench code may use SystemVerilog |
| Compile SurfBoot and SurfOS | Bare-metal RISC-V GCC/binutils, or validated LLVM equivalent, producing ELF32 RISC-V |
| Synthesize, place/route, generate bitstream and DDR IP | Version-pinned AMD Vivado on a supported **x86-64 Linux or Windows** host |
| Program local Arty and capture UART | MacBook USB connection; validated openFPGALoader and serial tools |
| Copy kernel/files to card | MacBook card reader and mounted FAT32 volume |

Vivado has no supported native macOS route in the [AMD support matrix](https://www.amd.com/en/support/adaptive-socs-and-fpgas/installer-info-general.html). Select and verify a supported build host early. A remote x86-64 Linux machine controlled from the Mac is the recommended arrangement. On an Intel Mac, a suitable x86-64 VM may be an option. An ordinary ARM Linux VM on Apple Silicon does not provide an x86-64 Vivado environment. Treat emulation and experimental open-source Artix-7 flows as alternatives requiring proof, not as an assumed working toolchain.

Keep build scripts callable from the Mac; transfer source to the selected build host and return the bitstream, timing/utilization reports, logs, and build manifest. Do not embed credentials. Check the chosen Vivado version's device/IP support and licensing. Use vendor IP for DDR PHY/controller and clocking when appropriate, while keeping our CPU in readable Verilog.

[openFPGALoader documents macOS installation](https://trabucayre.github.io/openFPGALoader/guide/install.html) and [both Arty A7 variants](https://trabucayre.github.io/openFPGALoader/compatibility/board.html). Verify the installed tool and detected device before programming. For a confirmed 100T, a **volatile** programming example is:

```sh
openFPGALoader -b arty_a7_100t build/surfcpu.bit
```

That command is an example to validate, not evidence of a completed build. Use the 35T board identifier if that is the actual board. FPGA configuration RAM loses its contents when power is removed. Standalone power-on requires a separately verified QSPI programming procedure and correct boot-mode selection. Keep persistent-flash programming a distinct, explicit operation with a recovery procedure.

## 5. CPU and SoC architecture

### CPU contract

| Area | Requirement |
|---|---|
| RTL | Synthesizable Verilog for our CPU and SoC logic; clearly isolate vendor primitives/IP and simulation-only code. |
| ISA | Start with RV32I + Zicsr and machine-mode traps. Initial complete release adds M and Zifencei: `rv32im_zicsr_zifencei`, ILP32 soft-float ABI. |
| Registers/data | One hart, 32-bit addresses/data, little-endian, `x0` always zero, 32 integer registers, standard instruction encodings. |
| Pipeline | In-order IF → ID → EX → MEM → WB, explicit valid/stall/flush control, forwarding, load-use interlocks, memory backpressure, precise exceptions. |
| Control flow | Correct signed/unsigned branches, JAL/JALR link values, JALR bit-zero clearing, wrong-path cancellation. No branch prediction required initially. |
| Loads/stores | Byte/halfword/word access, signed/unsigned loads, byte enables, defined alignment checks. Misaligned halfword/word accesses trap initially. |
| Traps | Implement documented machine CSRs, direct-mode `mtvec`, ECALL/EBREAK, illegal instruction, instruction/data alignment and access faults, MRET, timer and external interrupts. |
| Memory ordering | Initially uncached and single outstanding data transaction; implement FENCE and FENCE.I correctly for the actual design. Do not advertise unsupported extensions. |
| Multiplication/division | M extension includes signed/unsigned multiply-high variants and specified division-by-zero/overflow results. A multicycle unit is acceptable. |
| Initial privilege model | Machine mode only, flat physical memory. No protected user processes or virtual memory claim. |
| Performance | First close timing at a conservative clock such as 25 MHz. Raise it only with post-route evidence; 50–100 MHz is a later target, not a guarantee. |

Pin the implemented ISA and privileged-spec versions. Derive instruction and trap behavior from the [RISC-V specifications](https://docs.riscv.org/reference/isa/), including [RV32I](https://docs.riscv.org/reference/isa/unpriv/rv32), [Zicsr](https://docs.riscv.org/reference/isa/unpriv/zicsr.html), and the privileged architecture. Publish implemented CSRs, reset values, writable masks, interrupt priorities, and unsupported features. Include RV32-safe handling of 64-bit timer/counter halves.

Start without caches, an FPU, atomics, compressed instructions, SMP, or an MMU. Compile every object and runtime library for the actual ISA; audit linked disassembly. The release compiler settings must include `-march=rv32im_zicsr_zifencei -mabi=ilp32` and a validated freestanding link/code model for the chosen addresses; earlier RV32I milestones need their own matching flags and libraries. Verify that compiler helper libraries actually have an RV32/ILP32 variant. If floating-point helpers are needed, use compatible soft-float support. Single-hart kernel synchronization must use the appropriate interrupt/critical-section design rather than silently emitting unsupported atomic instructions.

Later protected applications require a separately designed privilege model, memory protection, and possibly S-mode/Sv32. Do not describe a machine-mode prototype as preserving the existing x86 process isolation or demand paging.

### SoC components

Implement a small documented **SurfBus** connecting the core, memories, and MMIO. Use explicit request/accept/response semantics, byte-write strobes, error responses, and bounded failure behavior. Freeze this interface before splitting work across components. A load/store must commit once despite stalls; flushed instructions must never write memory or peripherals. Adapt SurfBus to the selected DDR controller interface.

Required components:

- Boot ROM and scratch RAM in FPGA block RAM, initialized from build artifacts.
- UART RX/TX with buffering and status/error reporting; begin at 115200 8N1.
- GPIO for LEDs, buttons, switches, and early diagnostics.
- Timer with 64-bit time/compare and a defined clock frequency, plus a simple interrupt controller.
- SPI controller for microSD, with programmable speed and chip select; SD protocol and FAT handling may live in firmware.
- VGA timing, text memory, font ROM, cursor, and palette.
- PS/2 receiver with input synchronization, frame/parity checks, timeout, FIFO, and interrupt/status reporting.
- DDR3L controller/PHY integration, calibration status, and tested bus-width conversion.

Use the board oscillator and proper clock resources. Synchronize asynchronous reset deassertion separately in each clock domain; handle UART, PS/2, video, and DDR clock crossings deliberately. Do not use arbitrary fabric clock division as a substitute for clock design. Do not make boot ROM depend on DDR calibration just to print an error.

### Proposed memory map to finalize in milestone 0

These are design defaults, not existing FPGA addresses. Generate RTL constants, C headers, linker symbols, and emulator definitions from one versioned source.

| Range/base | Proposed function |
|---|---|
| `0x00000000–0x0000FFFF` | 64 KiB boot ROM; reset vector `0x00000000` |
| `0x00010000–0x0001FFFF` | 64 KiB scratch RAM for boot stack, buffers, and BRAM-only tests |
| `0x10000000` | UART, 4 KiB register window |
| `0x10001000` | Timer, 4 KiB window |
| `0x10002000` | Interrupt controller, 4 KiB window |
| `0x10003000` | GPIO, 4 KiB window |
| `0x10004000` | microSD SPI controller, 4 KiB window |
| `0x10005000` | PS/2 keyboard, 4 KiB window |
| `0x10006000` | Video control, 4 KiB window |
| `0x10007000` | Board/system information and DDR status, 4 KiB window |
| `0x10008000` | Reserved for later Ethernet registers |
| `0x20000000–0x20003FFF` | 16 KiB text-memory window; 2,400 32-bit cells occupy 9,600 bytes |
| `0x80000000–0x8FFFFFFF` | 256 MiB DDR address window, subject to verified board configuration |

Reserve bootloader/kernel stacks, image staging, heaps, and later display buffers explicitly. Size ROM from the real firmware link report; adjust the provisional BRAM split before freezing it if needed. Invalid/unimplemented addresses must fault rather than hang. Do not expose DDR as usable before calibration succeeds.

## 6. Boot and persistent storage

### Two separate kinds of image

| Artifact | Role |
|---|---|
| `surfcpu.bit` | Configures FPGA hardware, including initialized boot ROM. Used with JTAG during development. |
| Configuration-flash image | Board/programmer-specific persistent FPGA configuration; generated separately when ready. |
| `surfboot.elf` and ROM initialization file | RISC-V boot firmware incorporated into the FPGA build. |
| `surfos.elf` | RISC-V kernel with symbols, sections, and debug information. |
| `surfos.payload.bin` | Raw linked kernel payload, an intermediate artifact. |
| `SURFOS.BIN` | Versioned boot package containing a checked header and payload; installed at `/BOOT/SURFOS.BIN`. |

Never copy the existing x86 `surfos.bin` or GRUB/UEFI USB disk image onto the card and call it a RISC-V kernel. Give target-specific build outputs different directories even when basenames coincide.

### Required boot sequence

```text
Power on
  → FPGA loads configuration from onboard QSPI flash
  → SurfCPU starts SurfBoot in BRAM (stack also in BRAM)
  → UART diagnostics; wait for DDR calibration with a timeout
  → SD initialization in SPI mode; read partition table and FAT32
  → locate /BOOT/SURFOS.BIN; validate header, ranges, and checksum
  → copy payload into DDR; verify loaded bytes; synchronize instruction fetch
  → jump to kernel entry with versioned board/boot information
  → kernel initializes its own runtime, drivers, scheduler, shell, and filesystem
```

During early development, JTAG loads the FPGA and boot ROM directly. Add a BRAM diagnostic program before requiring DDR or microSD. A serial RAM loader may help debugging but is not a prerequisite for manual card deployment.

Implement SDHC/SDXC block addressing correctly, starting with slow initialization clocks and increasing speed only after card initialization. Handle command responses, data tokens, busy states, capacity, CRC requirements, card absence, and timeouts. Begin with 512-byte sector reads; add writes only after reads are reliable. The adapter provides electrical access, not a filesystem implementation: see the [Digilent-authored Pmod MicroSD manual](https://media.digikey.com/pdf/Data%20Sheets/Digilent%20PDFs/Pmod_MircoSD_RM_Web.pdf).

Default card layout: **MBR with one primary FAT32 partition**, with 512-byte logical sectors. Explicitly reject exFAT or unsupported layouts. Use short boot-path names so the bootloader need not implement VFAT long names. Read fragmented files and multi-cluster directories; do not assume the kernel file is contiguous. Bound cluster-chain walks and all arithmetic using validated volume limits.

Define a compact fixed-size little-endian image header before writing the loader and packer: magic/version/header size, required ISA and platform ABI version, payload length, load address, entry point, and CRC32 over the payload. Check that all ranges fit supported RAM and avoid reserved regions; entry must be aligned and inside executable payload. The linker determines BSS bounds and kernel startup clears BSS. Document the checksum variant and test vectors. CRC detects accidental corruption; it is not authentication.

Define a versioned `surf_boot_info` structure and handoff convention: hart ID in `a0`, boot-info pointer in `a1`, machine mode, interrupts disabled, documented register/clock/memory state. Kernel startup must establish its own aligned stack and global pointer. Specify boot-info lifetime and reserve its storage until the kernel copies it. Keep this board-specific ABI distinct from an SBI/OpenSBI boot protocol.

### Card layout and updates

```text
/BOOT/SURFOS.BIN     active checked kernel package
/BOOT/BACKUP.BIN     optional last known-good package
/SYSTEM/            configuration and supporting data
/PROGRAMS/          later RISC-V applications
/USERS/DOCUMENTS/   persistent user files
```

Use the same card for boot and ordinary files. Initially mount it read-only in SurfBoot; the kernel takes sole ownership after handoff and enables read/write when that driver is validated. Preserve the existing `fs/fat.c` implementation where practical, with a new SD block driver and architecture support beneath it. Its presence does not mean it already runs on the FPGA.

Provide a Mac deployment command that accepts an explicitly selected mount point, verifies FAT32/expected card identity, installs only project files, checks copied bytes/checksums, and tells the user to eject before moving the card. A normal update must preserve user directories and must not repartition or erase the card. Build/test scripts should operate on regular disk-image files by default. Physical formatting requires explicit selection and approval of the actual device.

Do not mount the card on the Mac and Arty simultaneously. The Arty USB port does not automatically expose the card as a USB drive. On-device writes need flush/unmount/eject behavior, useful error handling, and reboot persistence tests. FAT32 is not journaled: interrupted updates can damage metadata. Keep backup/recovery selection available, but do not claim that a rename or backup file makes updates power-fail atomic.

## 7. Port SurfOS deliberately

The inspected checkout is an x86 kernel. `Makefile` targets `-m32 -march=i486`/`elf_i386`; `boot/boot.S` uses Multiboot and x86 startup; `kernel/traps.asm`, `include/surfos/irq.h`, `mm/paging.c`, and `user/libsurf/crt0.S` contain architecture-specific behavior. **A new compiler flag is not the whole port.**

Build a portability inventory and replace these boundaries:

| Existing area | RISC-V work |
|---|---|
| Boot/linker/Multiboot | RISC-V reset/startup and linker scripts; board boot info; initialized data/BSS/stack/global pointer. |
| GDT/IDT/PIC/PIT/x86 traps | Machine CSR/trap assembly, timer, interrupt dispatch, panic diagnostics. |
| Task state and context switching | RV32 register frame, saved PC/status, task-stack setup, MRET-based return and preemption rules. |
| Paging/heaps/user address spaces | First use flat physical RAM and an allocator; isolate x86 paging. Track protection/virtual-memory work separately. |
| Port I/O and PC devices | MMIO HAL for our UART, GPIO, SPI/SD, keyboard, video, and timer. No PCI/ATA/xHCI/i8042 assumptions. |
| Console/rendering | Retain console/shell behavior over UART and our VGA text device. Audit existing framebuffer assumptions. |
| Filesystem/VFS/block cache | Reuse portable logic; provide SD block I/O, timing, locking, allocation, and error propagation. |
| C library and compiler helpers | Audit inline assembly, alignment, packed structures, integer widths, floating point, division, and atomic operations. |
| User programs/syscalls/ELF | Later RV32 startup, calling convention and syscall stubs, ELF `EM_RISCV`, loader, link addresses, and ABI-compatible libraries. |

For the first release, boot a **machine-mode SurfOS kernel with kernel shell and at least two scheduled kernel tasks**. Preserve usable SurfOS commands and identity while clearly documenting absent process protection. Save/restore all required integer context during traps; verify `sp`, `gp`, `tp`, and ABI alignment. For C function calls follow the [RISC-V psABI](https://github.com/riscv-non-isa/riscv-elf-psabi-doc). Do not conflate a kernel context switch with just saving C callee-saved registers.

Stage full `surfsh`, separately loaded applications, protected user mode, and existing GUI applications after the kernel shell is working. The existing user library/syscall interface can guide that work, but all executable objects must be rebuilt and the privilege/memory model must support them. Existing PC network and USB drivers do not drive Arty peripherals.

## 8. VGA and PS/2 requirements

**VGA v1:** 640 × 480 at approximately 60 Hz, with a documented actual pixel clock and monitor-tested timing. Use 800 total pixels/line and 525 lines/frame; horizontal visible/front porch/sync/back porch = 640/16/96/48, vertical = 480/10/2/33, negative syncs. Target 25.175 MHz, or document a validated approximation. Use the actual clock to report refresh rate.

Provide an 80 × 30 console with 8 × 16 glyphs, foreground/background colors, cursor, newline, scrolling, clear-screen, and keyboard echo. Use dual-port text RAM or a properly synchronized bridge so display refresh remains independent of CPU stalls. Define each 32-bit cell as glyph in bits 7:0, foreground in 11:8, background in 15:12, remaining bits reserved. Use an appropriately licensed font. Produce a stable color-bar/test-pattern mode before involving SurfOS. The genuine [Pmod VGA](https://digilent.com/shop/pmod-vga-video-graphics-array/) exposes RGB444 and needs FPGA-generated video signals.

**PS/2 v1:** keyboard receive path and scan-code-set-2 decoding, make/break sequences, extended keys, Shift/Ctrl/Alt, Enter, Backspace, arrows, repeat, and FIFO overflow handling. Synchronize inputs and reject bad parity/framing with recovery after partial frames. Keyboard-generated receive clocks and any host command sequence must follow PS/2 signaling; do not blindly drive a clock continuously. One PS/2 socket supports the selected device, not a keyboard and mouse simultaneously. Mouse input is later unless another verified port/device arrangement is available.

Graphics/framebuffer modes and the SurfOS GUI are extensions after a dependable text console. Avoid allocating a full-resolution framebuffer from scarce BRAM without a resource budget; later DDR scanout also needs bandwidth, arbitration, buffering, and underrun tests.

## 9. Suggested source organization and build interface

Keep FPGA work separate from the restored source. Adjust names only after inspecting any existing equivalent structure.

```text
surfcpu/
  README.md
  rtl/cpu/                 core, pipeline, ALU, registers, CSR/trap, mul/div
  rtl/soc/                 SurfBus, address decoder, memories, SoC integration
  rtl/peripherals/         UART, timer, IRQ, GPIO, SPI, PS2, text video
  boards/arty_a7/          top-level Verilog, checked XDC, board configuration
  ip/                     versioned IP configuration and generation scripts
  sim/                    unit/system testbenches, memory/card models, tracing
  firmware/surfboot/       ROM startup, linker, SD/FAT reader, image loader
  firmware/diagnostics/    BRAM/DDR/SD/video/keyboard diagnostic programs
  tools/                  image packer, ROM converter, Mac deploy, build-host wrapper
  docs/                   hardware inventory, ISA, bus/MMIO, boot ABI, test evidence
  build/                  ignored generated artifacts
arch/riscv32/             kernel entry, trap/context, IRQ, linker, architecture support
include/arch/riscv32/      architecture interfaces
boards/arty_a7/            SurfOS board HAL/drivers and configuration
build/riscv32/arty_a7/     separate generated software outputs
```

Implement and document commands equivalent to `doctor`, `lint`, `test-cpu`, `test-soc`, `build-rom`, `build-kernel`, `build-fpga`, `program-volatile`, and `deploy-sd`. These names are requirements for a usable workflow, not claims that commands currently exist. A separate explicit command handles configuration flash. One command should run the automated pre-release checks.

Pin tools, third-party IP, test suites, and font licenses. Record source revision/dirty state, FPGA part, constraints hash, ROM/kernel hashes, ISA/ABI versions, tool versions, build host, timing summary, and utilization in a manifest. Retain IP-generation recipes rather than depending on an unrecorded GUI project. Never reuse the x86 build directory or require the Mac's card to build/test software.

## 10. Ordered milestones and acceptance evidence

Within each milestone, write meaningful tests, implement the smallest working increment, run them, and save evidence. A simulation result, successful synthesis, successful programming, and physical board behavior are different results; report each separately.

| Milestone | Work | Required exit evidence |
|---|---|---|
| **0 — Inventory and contracts** | Confirm hardware/toolchain; finalize ISA, pipeline, bus, memory map, boot ABI, source boundaries, and build host. | Checked hardware/wiring inventory, architecture documents, tool probes, feasible BRAM/resource budget, and explicit unresolved dependencies. |
| **1 — Toolchain and board baseline** | Reproducible LED/clock/reset and UART diagnostic design. Establish Mac → Vivado host → Mac → JTAG loop. | Bitstream for the exact part; post-route timing/DRC reports; physical LED behavior and readable UART output. Simulation alone does not close this milestone. |
| **2 — CPU correctness in simulation** | RV32I/CSR/traps first, then five-stage hazards and M/Zifencei. BRAM program execution and trace interface. | Directed ISA, trap, memory-stall and wrong-path tests; differential retirement comparison; suitable upstream architecture tests with their scope recorded. |
| **3 — CPU on the board** | Run BRAM-resident SurfCPU diagnostic firmware; GPIO/UART, timer interrupts, exception reports. | Physical UART transcript with build identity, arithmetic/memory results, timer progress, and deliberate fault recovery. |
| **4 — DDR integration** | Generate/configure vendor DDR controller, bridge SurfBus, reset/calibration behavior, memory diagnostics. | Calibration and physical address/data-pattern tests, byte lanes, boundary/alias tests and sustained transfers; CPU executes a test copied from BRAM into DDR. |
| **5 — Card reads and SurfBoot** | SPI/SD initialization, sector reads, FAT32 reader, image packaging and validated boot handoff. | Image-file tests for fragmentation/corruption; real card boots a DDR diagnostic image; missing/invalid image returns to useful diagnostics instead of hanging. |
| **6 — Standalone I/O** | VGA timing/text, PS/2 receiver/decoder, console integration. | Test pattern, readable 80 × 30 console, keyboard modifiers/editing, FIFO/error recovery; UART remains available for diagnosis. |
| **7 — SurfOS kernel port** | Architecture startup/traps/allocator/timer/context switching; kernel shell and minimum HAL. | Real board boots SurfOS from microSD; shell via VGA/PS2 and UART; two kernel tasks make progress, sleep/wake, and survive repeated scheduling. |
| **8 — Persistent files** | SD writes, VFS/FAT integration, flush/unmount, controlled errors, Mac install workflow. | Create/edit/read files; reboot and power-cycle after clean flush; Mac reads matching bytes; Mac-created files read on board; unrelated card files survive a kernel update. |
| **9 — Reproducible standalone release** | Persistent QSPI configuration, recovery image selection, final documentation and clean rebuild. | Board boots without JTAG programming after power-on; checksum-matched release artifacts; at least 10 consecutive cold boots plus a one-hour shell/task/storage soak; fresh checkout reproduces the procedure. |

DDR work is a major integration risk; begin its vendor-IP feasibility work early without coupling CPU unit tests to DDR. VGA/PS2 can be developed independently once pin assignments are checked. A milestone may report partial evidence, but must not be marked complete while its physical acceptance test is outstanding.

### Minimum verification coverage

- CPU: every implemented instruction class; `x0`; immediate sign extension; signed/unsigned comparisons and shifts; JALR/alignment; load extension/store strobes; divide corner cases; CSR masks; interrupt entry/return; reset and WFI behavior; stalls during branches, traps, loads and stores.
- Pipeline: RAW dependencies, load-use, store-data forwarding, branch flushes, competing redirects, and **no wrong-path MMIO/store effects**. Randomized memory latency must not duplicate a UART write or lose a response.
- Reference testing: expose retired PC/instruction/register writes/memory effects, preferably RVFI-compatible; compare to a pinned reference model. Check eligibility and setup for current [RISC-V architecture tests](https://github.com/riscv/riscv-arch-test); a test-suite pass is not a blanket certification claim.
- Boot/storage: absent card, failed initialization, bad MBR/BPB/header, truncated package, bad checksum, out-of-range load/entry, fragmented chains, cycles/out-of-range FAT entries, full volume, write failure, and controlled removal. Failed boots must remain diagnosable without a screen.
- Hardware: relevant clocks constrained, setup/hold timing met, no unexplained critical warnings, no unconstrained external pins, reviewed CDC/reset paths, resource headroom documented. Never silence timing or pin violations to obtain a bitstream.
- Port: ABI/structure sizes, runtime helper ISA, stack alignment, BSS/data initialization, trap register preservation, allocator limits, timer rollover, and task switching under I/O load.

QEMU's generic RISC-V `virt` machine is useful for a **separate matching HAL** or portable kernel tests. It does not emulate our custom Arty memory map/peripherals by default and is not proof that the FPGA works. Use our RTL system simulation and the physical board for that evidence.

## 11. Later work, after the first complete release

1. USB-UART kernel upload to RAM, then a carefully designed card updater.
2. Ethernet MAC integration for the onboard PHY, actual board driver, network tests, then remote file/kernel updates. An Ethernet jack/PHY is not a complete MAC or SurfOS driver.
3. RISC-V user applications and `surfsh`, with an explicit protection model.
4. Graphics/framebuffer and a small SurfOS GUI; mouse support with adequate hardware.
5. Caches and measured performance improvements, including FENCE.I/coherency behavior for loaders and any DMA.
6. Optional custom instructions/accelerators, after baseline ISA compatibility is proven.

USB host support, HDMI, audio, Wi-Fi, custom PCBs, and ASIC fabrication are outside v1. The existing Arty programming USB connection is not a USB-host port for a flash drive or USB keyboard.

## 12. Handoff and reporting requirements

Maintain a concise progress record inside `surfcpu/docs/` with completed milestones, exact reproducible commands, artifacts, test logs, physical observations, and remaining gaps. Provide the owner a wiring guide and a MacBook setup/build/program/card-update/boot/recovery guide, with expected output at each step.

If hardware is unavailable, continue and deliver simulation-tested RTL, firmware, the port scaffolding, and build instructions; label FPGA and physical tests **pending**. If the Vivado host is unavailable, say specifically that bitstream generation is blocked while continuing independent work. Do not invent successful builds, clock rates, hardware access, or card contents.

**Start by completing milestone 0 and preparing the first reproducible LED/UART build. Work forward in tested increments. Keep the custom RISC-V hardware, the boot firmware, and the SurfOS port aligned around the same published contracts.**
