# SurfOS USB Boot on Lenovo IdeaPad 3 15IIL05 Implementation Plan

> **For the development agent:** Use `superpowers:executing-plans`, if available, to implement this brief task by task. Read the entire document before changing code. Checkboxes track future implementation; this document does not claim the port or image already exists.

**Goal:** Produce a raw USB disk image that the owner can write to a USB flash drive and boot on their Lenovo IdeaPad 3 15IIL05 into an interactive SurfOS shell, with the existing user programs available from an initrd loaded into RAM.

**Architecture:** Keep the monolithic ELF32 x86 kernel and single-CPU scheduler. Add an x86-64 UEFI GRUB loader, a Multiboot2 handoff into the 32-bit kernel, a framebuffer text console, and the platform support needed for input and timer interrupts. Preserve the current Multiboot1/QEMU development path.

**Tech stack:** Freestanding C, GNU assembly, NASM, GCC multilib, binutils, GRUB, GPT/FAT32 image tooling, Python, QEMU and OVMF. The EFI loader is 64-bit; the SurfOS kernel remains 32-bit.

**Spec:** This document contains both the requested product specification and implementation plan. It is self-contained and can be handed to an agent without the preceding conversation.

**Prepared:** 2026-10-05. **Inspected source baseline:** `ai-dev`, commit `3f476d2` (`build: add modern Docker environment and fix QEMU launch paths`). Recheck the checkout and toolchain before implementation.

## 1. Owner request and completion criteria

The owner has restored their 2004 operating system from printed source. They now want to build a flashable USB image and boot SurfOS directly on their Lenovo laptop.

The required result is:

1. A documented build command creates `build/usb/surfos-ideapad-15iil05.img`.
2. The image contains its own bootloader, kernel, configuration and initrd; it does not depend on files installed on the laptop.
3. The owner writes the entire image to a USB flash drive using a raw-image writer.
4. The laptop's one-time boot menu can select the USB drive in UEFI mode.
5. SurfOS displays boot progress and reaches its shell on the laptop screen.
6. A keyboard accepts commands; the target is the built-in keyboard. A USB keyboard may be an explicitly documented intermediate milestone.
7. Timer ticks, scheduling, RAM-backed filesystem access and an existing ring-3 program work.
8. The laptop's internal operating system and disks are not modified by this boot workflow.

An image that builds, a kernel that boots through QEMU `-kernel`, or a shell visible only over an emulated COM1 port is not sufficient evidence of native Lenovo support.

## 2. Target hardware: facts and remaining unknowns

The owner reported `IdeaPad 3 15IL05`. The matching official Lenovo family is **IdeaPad 3 15IIL05** (two consecutive capital `I` characters), machine type **81WE**. Confirm the underside label or Windows System Model before recording actual-hardware results. [Lenovo support](https://pcsupport.lenovo.com/us/en/products/laptops-and-netbooks/3-series/ideapad-3-15iil05/81we/document-userguide)

| Item | Known information | Consequence for SurfOS |
|---|---|---|
| Owner's processor tier | 10th-generation Intel Core i3 | This family's published i3 option is i3-1005G1; verify the installed CPU. |
| CPU | i3-1005G1, Ice Lake, Intel 64/x86-64, 2 cores and 4 threads | It supports the existing 32-bit protected-mode kernel. Initially run only the bootstrap processor. |
| Graphics | The i3 includes Intel UHD Graphics | Use the bootloader-provided framebuffer for text; defer an accelerated Intel graphics driver. |
| Memory | Platform offers DDR4-2666, soldered RAM plus one SO-DIMM | Actual installed capacity is unknown; map and reserve memory correctly, including when total RAM exceeds 4 GiB. |
| Storage options | SATA 2.5-inch HDD and/or PCIe NVMe M.2 SSD, depending on configuration | Actual installed drive is unknown. Internal storage is deferred for the first boot milestone. |
| Networking | No onboard Ethernet; wireless options vary | The current e1000/3Com drivers do not supply native networking on this laptop. Defer Wi-Fi. |
| USB | One USB 2.0 port and two USB 3.2 Gen 1 ports | Firmware can be the boot-media reader. Kernel USB support is a separate requirement for continued access or USB input. |
| Input | Built-in keyboard and Precision TouchPad | PS/2 compatibility, I2C/HID details and controller IDs require inspection; do not invent them. |
| Firmware | UEFI is the implementation target | Legacy/CSM availability and installed Secure Boot settings have not been established. |

Hardware facts come from [Lenovo's platform specification](https://psref.lenovo.com/syspool/Sys/PDF/IdeaPad/IdeaPad_3_15IIL05/IdeaPad_3_15IIL05_Spec.pdf) and [Intel's i3-1005G1 specification](https://www.intel.com/content/www/us/en/products/sku/196588/intel-core-i31005g1-processor-4m-cache-up-to-3-40-ghz/specifications.html). They describe the family, not every component installed in the owner's unit. The CPU launched in Q3 2019; that does not establish the laptop's purchase or manufacture date.

CPU 32-bit support and firmware legacy boot support are independent. A 64-bit UEFI application can load a 32-bit kernel through a suitable handoff. See [Intel's system programming manual](https://cdrdv2-public.intel.com/874249/253668-090-sdm-vol-3a.pdf) and the [Multiboot2 machine-state specification](https://www.gnu.org/software/grub/manual/multiboot2/html_node/Machine-state.html).

## 3. Global constraints

- Work in `06-restoration/` on the development branch or a feature branch based on it. Keep `master` as the restoration branch.
- Preserve `01-scans-original/`, `03-transcriptions/`, `04-source-recovered/` and `05-source-rebuilt/`; do not modernize the archival copies.
- Keep the kernel 32-bit, monolithic and single-CPU for this milestone. Do not make an x86-64 rewrite or multicore scheduler a prerequisite.
- Preserve existing shell commands, task behavior, user programs and the Multiboot1 development path.
- This brief extends the older restrictions in `CLAUDE.md` and `docs/ROADMAP.md`: UEFI/Multiboot2, framebuffer output and necessary platform discovery/interrupt support are explicitly in scope for the laptop port. Update those development conventions to reflect the new scope.
- Default to UEFI boot; optional BIOS compatibility must not substitute for the required UEFI image.
- First release assumes Secure Boot is disabled by the owner where firmware permits it. Signed Secure Boot deployment is a later feature; report if the owner cannot disable it.
- Image generation operates on regular files only. It must not flash physical drives, register host/laptop EFI boot entries, or install anything to the internal disk.
- Native boot defaults to an initrd-only configuration with no internal disk writes. Do not automatically mount internal FAT volumes through `init_fs()` in this profile.
- Use the modern development environment. Do not return to the Debian etch/GCC 3.3 restoration toolchain.
- Retain bounded hardware waits and visible boot-stage diagnostics. Missing legacy devices must not cause an infinite loop.
- Required checks before committing implementation changes remain `make test-all` and the relevant new tests. Use selective staging and preserve unrelated changes.

## 4. Current source and why it needs changes

All code paths below are relative to the **`06-restoration/` repository root**, not to this document's directory.

| Existing files | Current behavior | Required direction |
|---|---|---|
| `boot/boot.S`, `linker.ld` | Multiboot1 header and 32-bit startup; kernel at 1 MiB | Add a valid Multiboot2 header and keep both boot protocols discoverable. |
| `include/surfos/multiboot.h`, `kernel/multiboot.c` | Copies memory map/modules/command line; no framebuffer or ACPI handoff | Parse both protocols into one owned boot-information representation. |
| `mm/pmm.c`, `mm/paging.c`, `mm/kalloc.c`, `include/mm/memory.h` | Page tables, frame stack and DMA heap occupy fixed physical ranges between 2 and 15 MiB | Validate/reserve those ranges or relocate them before writing; exclude all boot-owned and firmware-reserved memory. |
| `kernel/console.c`, `include/surfos/console.h` | Text cells at `0xB8000`; VGA cursor ports | Keep virtual text buffers, add framebuffer rendering, avoid VGA accesses on framebuffer-only boots. |
| `kernel/main.c` | Serial first; unconditional PS/2 and driver initialization | Select a boot profile, establish screen diagnostics, and initialize only supported devices. |
| `driver/serial.c` | COM1 port I/O and IRQ4; receive loop assumes a UART | Probe presence, bound receive work, and disable the UART path when absent. This laptop is not assumed to expose COM1. |
| `driver/drivers.c`, `driver/parport.c`, `driver/floppy/floppy.c`, `driver/dma/` | Legacy devices probed during startup; parallel port reads BIOS data-area addresses | Skip unsupported legacy devices on native UEFI boot; do not trust stale BIOS data-area contents. |
| `kernel/keyboard.c`, `kernel/tty.c` | PS/2 scan-code input into tty queues | Probe/initialize the actual keyboard interface and preserve tty editing. |
| `kernel/interrupt.c`, `kernel/timer.c`, `include/surfos/irq.h` | 8259 PIC/PIT and calibrated PIT channel-2 delay | Discover routing, verify tick delivery and calibration; add an APIC/timer backend if necessary. |
| `driver/ramdisk.c`, `fs/tarfs.c`, `fs/vfs.c` | Boot modules become ramdisks; tar initrd mounts at `/initrd` | Reuse the initrd. Guard its memory against reclamation and skip internal-device automounts in laptop profile. |
| `Makefile`, `tools/mkimage.py`, `devenv/Dockerfile`, `devenv/build.sh` | Kernel, user binaries, synthetic FAT test disk and GRUB ISO | Add a complete GPT/FAT32 USB image builder and export its artifacts. |
| `tools/qemu-run.py`, `shell/selftest.c` | Existing smoke suite expects IDE/e1000/VGA/serial emulation | Keep that suite; add a separate USB/UEFI/ramdisk-only profile and framebuffer capture. |

The synthetic `build/images/test.img` is not a complete bootable USB image. Likewise, `make iso` presently describes an ISO workflow and is not proof of USB/UEFI boot. The current Dockerfile lacks several image/firmware tools required below.

Some existing documentation describes historical defects that newer code has fixed. Inspect implementation and fresh test results rather than treating that status paragraph as current truth. Previous analysis ran syntax/script checks but did not complete a fresh full build or native boot. Establish a new baseline.

## 5. Boot and artifact contract

### Boot sequence

```text
Lenovo x86-64 UEFI firmware
  -> USB GPT disk / FAT32 EFI System Partition
  -> /EFI/BOOT/BOOTX64.EFI (standalone x86-64 GRUB)
  -> GRUB loads SurfOS ELF32 kernel + initrd.tar
  -> standard Multiboot2 i386 handoff; firmware boot services exited
  -> validated boot info, memory setup, framebuffer, timer and keyboard
  -> SurfOS shell + existing programs from /initrd/bin
```

Use existing GRUB functionality rather than writing a custom UEFI loader. Request the normal Multiboot2 i386 handoff. Do not request the EFI boot-services-preservation or EFI amd64-entry tags and then jump into the existing 32-bit entry code: those paths have different machine-state contracts.

UEFI console/input/block protocols are boot services. After the handoff, SurfOS needs its own display/input implementation. Firmware reading the USB at boot does not give SurfOS a USB storage driver. [UEFI boot services](https://uefi.org/specs/UEFI/2.10/07_Services_Boot_Services.html)

### USB image layout

Create a **256 MiB raw disk image**, with a valid primary/backup GPT and a FAT32 EFI System Partition from 1 MiB to 255 MiB, labeled `SURFOSUSB`. Increase the image size explicitly if artifacts ever outgrow it; fail rather than truncate files.

Required files inside the ESP:

```text
/EFI/BOOT/BOOTX64.EFI
/boot/grub/grub.cfg
/boot/surfos.bin
/boot/initrd.tar
/boot/BUILD-INFO.txt
```

Include a GRUB font if the selected GRUB graphics configuration requires one. Build a standalone EFI executable with required modules and a bootstrap configuration that locates this ESP and loads its configuration. Eliminate references to build-host paths. Its configuration must load the kernel with `multiboot2`, load the initrd with `module2`, and pass `profile=laptop`.

Use the removable-media EFI fallback path, with no dependency on an NVRAM boot entry. [UEFI removable-media boot](https://uefi.org/specs/UEFI/2.10/03_Boot_Manager.html), [GRUB removable installation](https://www.gnu.org/software/grub/manual/grub/html_node/Installing-GRUB-using-grub_002dinstall.html)

The published build interface should be:

```sh
# Run from 06-restoration/. These targets are TO BE IMPLEMENTED.
make usb-image
make test-usb
```

`make usb-image O=2` must package the matching optimized kernel and user programs. Export:

- `build/usb/surfos-ideapad-15iil05.img`
- `build/usb/surfos-ideapad-15iil05.img.sha256`
- `build/usb/build-manifest.json` with source revision/dirty status, optimization, tool versions and component hashes

The checksum must identify the actual final image. Do not promise byte-for-byte reproducibility unless timestamps, identifiers and filesystem metadata are normalized and repeat builds prove it.

## 6. Review focus

1. **Above-4-GiB addresses:** preserve 64-bit physical addresses and accounting; map only representable ranges for this non-PAE milestone. Never silently truncate a framebuffer, BAR or memory-map address.
2. **Boot-data lifetime and low-memory collisions:** no pool write may overwrite an initrd, boot-info block, reserved firmware region or the kernel.
3. **Absent legacy devices:** no UART, floppy, parallel port, IDE disk or Ethernet card must be a valid boot configuration.
4. **Real display/input:** serial output and a QEMU VGA text dump cannot prove framebuffer rendering or keyboard operation after UEFI boot services end.
5. **Artifact correctness:** the exact image must boot from an emulated USB device with fresh firmware variables; booting a separately supplied kernel or cached firmware entry does not validate it.

## 7. Implementation tasks

### Task 1: Establish the baseline and hardware record

**Files:** Read `CLAUDE.md`, `docs/ARCHITECTURE.md`, `docs/CODE-AUDIT.md`, `docs/ROADMAP.md`, `devenv/README.md`. Create `docs/hardware/ideapad-3-15iil05.md` as the evolving hardware/evidence record.

- [ ] Recheck branch, HEAD and working-tree changes. Record the actual tool versions and build commands.
- [ ] Run `./devenv/build.sh`, `./devenv/build.sh unittest` and `./devenv/build.sh test-all` in the documented x86 container; capture exit statuses and logs. On a provisioned x86 Linux host, the equivalent `make`, `make unittest`, `make test-all` commands are acceptable.
- [ ] Resolve actual baseline failures before building the port. A Docker access failure is an environment blocker, not evidence the kernel compiles or boots.
- [ ] Record missing laptop facts without guessing: CPU string, installed RAM, firmware version, boot options, Secure Boot setting, storage/controller and keyboard/touchpad interfaces. On Windows, use `msinfo32` and Device Manager; a Linux live environment may provide `lspci -nn`, `lsusb` and input/ACPI information.
- [ ] Continue emulator-side implementation while hardware details are unavailable. Mark native support as pending until the owner supplies observations or the agent has access to the laptop.

**Acceptance:** Baseline logs clearly distinguish host tests, kernel build and QEMU boot; hardware record separates observations, family specifications and unknowns.

### Task 2: Add the Multiboot2 handoff and laptop profile

**Modify:** `boot/boot.S`, `linker.ld`, `include/surfos/multiboot.h`, `kernel/multiboot.c`, `kernel/main.c`, `driver/drivers.c`, `fs/vfs.c`.

**Create:** `kernel/multiboot2.c`, `include/surfos/multiboot2.h`, `tests/host/test_bootinfo.c`.

**Interface:** Preserve `mb_init(u_long magic, u_long addr)` and `bootinfo`. Add `int mb2_parse(const void *data, u32 bytes, struct boot_info *out)`, returning zero on valid input and a negative result on malformed/unsupported input. Extend `bootinfo` with framebuffer metadata, ACPI RSDP data and a parsed laptop-profile flag. Preserve 64-bit physical bases/lengths.

- [ ] Add tests for valid command-line/module/map/framebuffer/ACPI tags, truncated tags, invalid tag size/alignment, missing end tag, overflow and unknown optional tags. Pass a separately bounded byte count to the parser; validate the envelope before using it in the real entry path.
- [ ] Add both headers with required alignment and placement; verify with `grub-file --is-x86-multiboot` and `grub-file --is-x86-multiboot2` on the final linked ELF.
- [ ] Dispatch by loader magic and normalize both protocols. Copy needed data before memory initialization; do not allocate from the uninitialized heap. Increase the fixed map capacity to 256 entries and fail visibly on overflow rather than silently losing regions. Preserve the existing eight-module bound and reject excess modules explicitly.
- [ ] Require valid memory-map and framebuffer information for the UEFI laptop profile. Keep a clear serial/VGA diagnostic path for development boots that lack a framebuffer.
- [ ] Make `profile=laptop` select ramdisk-first initialization, suppress unsupported legacy probes and internal-disk automounting, and leave the existing emulator profile intact.

**Acceptance:** Existing Multiboot1 smoke tests pass; a GRUB/OVMF Multiboot2 boot reaches a visible, identified boot stage with initrd metadata. Invalid inputs fail predictably.

### Task 3: Make memory initialization safe for firmware-loaded images

**Modify:** `mm/pmm.c`, `mm/paging.c`, `mm/kalloc.c`, `include/mm/memory.h`, `include/mm/pmm.h`, `driver/ramdisk.c`; add focused reservation tests in `tests/host/test_bootinfo.c` or `tests/host/test_boot_memory.c`.

- [ ] Test memory maps with holes, reserved low-memory ranges, modules overlapping the old pools, available RAM above 4 GiB, no usable low RAM and arithmetic overflow.
- [ ] Audit all writes to the fixed 2–15 MiB arenas. Retain an arena only if the normalized map proves the complete range is available and unoccupied. Otherwise select/reserve a suitable low-memory arena and update its consumers before writing it.
- [ ] Exclude the kernel, boot structures retained in place, initrd/modules, allocator arenas, framebuffer and firmware/ACPI reserved ranges from allocation. Make the reserved intervals explicit and testable.
- [ ] Keep total-RAM accounting in 64-bit values; report installed versus usable low RAM separately. Clip allocations to below 4 GiB for this non-PAE implementation without wraparound.
- [ ] Reject an unaddressable framebuffer or required boot resource with a diagnostic. Request a usable placement/mode from the loader; do not blindly cast a `u64` physical address into `u_long`.

**Acceptance:** Reservation tests pass, initrd remains intact under allocation churn, low-memory collisions fail or relocate safely, and OVMF tests at 64 MiB, 4 GiB and 8 GiB boot without count wraparound or reserved-memory corruption.

### Task 4: Add a framebuffer text backend

**Modify:** `kernel/console.c`, `include/surfos/console.h`, `kernel/main.c`.

**Create:** `driver/framebuffer.c`, `include/sys/framebuffer.h`, `tests/host/test_framebuffer.c`; include a suitably licensed bitmap font with attribution.

**Interface:** `int framebuffer_init(void)` consumes validated `bootinfo` and returns zero when ready. The text backend exposes `framebuffer_draw_cell(u32 col, u32 row, u8 ch, u8 color)` and `framebuffer_present(void)` to console code. Preserve `kputch()` and existing tty APIs.

- [ ] Test RGB/BGR channel masks, padded pitch, glyph drawing, clipping and invalid/overflowing dimensions against an in-memory framebuffer with boundary guards.
- [ ] Map the framebuffer through the VMM using physical-address checks and suitable cache attributes. Honor reported pitch, depth and color masks rather than assuming packed RGB or width-times-four stride.
- [ ] Preserve the existing 80x25 logical console initially; render it with an 8x16 bitmap font into a supported framebuffer of at least 640x400. Request a normal mode such as 1024x768 and accept another valid usable mode.
- [ ] Route cell updates, clear, scroll, color and console switching to the selected backend. Never touch `0xB8000` or VGA cursor ports on framebuffer-only boots. Use a software cursor.
- [ ] Establish framebuffer output early enough to display subsequent initialization failures. Avoid heap-dependent early rendering and ensure memory validation failures have a usable diagnostic path.
- [ ] Replace VGA-memory-only evidence for the UEFI test with QEMU framebuffer screenshots.

**Acceptance:** OVMF screenshots show the banner, shell, colored text, scrolling and console switching; serial and framebuffer agree on boot stages.

### Task 5: Establish bounded device probing, timer interrupts and input

**Modify:** `driver/serial.c`, `driver/drivers.c`, `kernel/keyboard.c`, `kernel/interrupt.c`, `kernel/timer.c`, `kernel/tty.c`, `kernel/main.c`.

**Create as needed:** `kernel/acpi.c`, `include/surfos/acpi.h`; `kernel/apic.c`, `include/surfos/apic.h` if routing requires it; `driver/usb/xhci.c`, `driver/usb/hid.c` and their headers only if USB keyboard support is necessary.

- [ ] Add tests/boot scenarios with no COM1, IDE disk, floppy, parallel port or Ethernet interface. Bound UART receive work and every controller probe. Preserve QEMU serial diagnostics when COM1 is present.
- [ ] Validate ACPI RSDP/table signatures, lengths and checksums before consuming them. Copy/map necessary tables and keep their memory reserved. Use firmware flags and hardware observations to decide whether i8042/PIC/PIT interfaces exist.
- [ ] Verify delay calibration terminates and timer ticks advance; kernel startup currently calibrates before scheduling starts, so missing PIT hardware cannot be handled by scheduler-based timeouts alone.
- [ ] Retain PIC/PIT if they function on the target. If they do not, implement the smallest single-CPU local-APIC/I/O-APIC and timer backend needed, including interrupt overrides and correct EOIs. Keep scheduler policy and saved-frame handling unchanged.
- [ ] Probe and initialize i8042 input when supported, including controller translation/scan-code expectations and bounded LED operations. Feed decoded input into the existing `tty_input()` path.
- [ ] If built-in keyboard access cannot work initially, implement xHCI enumeration and USB HID keyboard input as a documented intermediate route. A USB 2.0 port does not guarantee an EHCI controller. Scope USB support to input; storage remains deferred.
- [ ] Expose `bootinfo`/`drivers` diagnostics identifying the selected console, timer and input backends. Test ordinary typing, Shift, Enter, Backspace, arrows, history and console switching after handoff.

**Acceptance:** The shell responds without COM1; `tick` advances, sleeping tasks wake, and concurrent tasks run. Record which keyboard was tested. Do not call the native milestone complete with only serial input or firmware-side menu input.

### Task 6: Build the complete USB image

**Modify:** `Makefile`, `devenv/Dockerfile`, `devenv/build.sh`; keep the existing `tools/mkimage.py` test-disk behavior.

**Create:** `tools/mkusb.py`, `boot/grub-usb.cfg`, `tests/test_mkusb.py`.

**Build contract:** `make usb-image O=<level>` packages that level's kernel and user-program initrd into the artifact/layout defined in section 5. `tools/mkusb.py --kernel <file> --initrd <file> --output <regular-file>` is the underlying script interface.

- [ ] Add tests that reject missing/empty inputs and block-device outputs; inspect GPT/ESP geometry, required files and hashes in the generated image.
- [ ] Provision GRUB EFI tooling (`grub-efi-amd64-bin` and common tooling), FAT utilities, GPT tooling and OVMF in the modern environment. Record exact installed versions; use image-file/mtools operations rather than requiring privileged host mounts.
- [ ] Build the standalone EFI loader and required files; embed a self-contained bootstrap configuration and verify module/font availability.
- [ ] Write the raw image atomically through a staging file. Any failed command returns nonzero and preserves the last successful image. Generate checksums and manifests only after successful assembly.
- [ ] Ensure Docker exports the new image/manifest and reports the correct artifact paths. Preserve existing default build and explicit-target behavior.
- [ ] Verify a second image build does not embed stale components or point to host files. Verify both O0 and O2 images contain the matching kernel and initrd user binaries.

**Acceptance:** Partition/filesystem inspection finds every required file; the manifest matches extracted bytes; failure cases preserve the previous artifact. Image creation performs no physical disk writes.

### Task 7: Boot the exact image through UEFI and emulated USB

**Modify:** `tools/qemu-run.py`, `shell/selftest.c`, `Makefile`, `.github/workflows/ci.yml`.

**Create:** `tools/qemu-usb.py`, with `--image`, `--ovmf-code`, `--ovmf-vars`, `--output-dir` and `--test` options.

- [ ] Boot `qemu-system-x86_64` with OVMF, read-only firmware code and a fresh per-run writable copy of its variable store. Use a matched firmware pair with Secure Boot disabled.
- [ ] Attach a copy of the final raw image through an emulated xHCI controller and `usb-storage`. Do not supply `-kernel`, `-initrd`, a substitute IDE boot disk or a preconfigured EFI boot entry.
- [ ] Test with no IDE data disk or NIC. Capture command line, firmware versions, serial transcript when present and framebuffer screenshots. For the input test, inject keyboard events into an emulated keyboard device rather than sending shell commands over serial.
- [ ] Add a portable smoke profile: `help`, `bootinfo`, `drivers`, `tick`, `ps`, `memstat`, `lsblk` (rd0 required), `mount` (`/initrd` required), `cat /initrd/etc/version`, `run hello a b` (exit 42), `count 3 10`, `heaptest` and relevant `selftest` groups.
- [ ] Keep the existing IDE/FAT/e1000 smoke suite as a separate regression suite. Mark absent hardware tests as skipped with reasons; never claim that a skipped device path passed.
- [ ] Add failure checks for missing kernel/initrd, invalid framebuffer information and malformed boot data. Required stages must not produce a silent blank screen or an unbounded wait.
- [ ] Repeat cold boots with fresh firmware variables at O0 and O2 and different RAM sizes; set finite process/boot/command timeouts and retain failure evidence.

**Acceptance:** `make test-usb` exits zero only when the exact image boots through UEFI/USB and the portable checks pass. Existing `make test-all` continues to pass.

### Task 8: Document flashing and prove the physical Lenovo boot

**Create:** `docs/USB-BOOT.md`; update `docs/hardware/ideapad-3-15iil05.md`, `docs/README.md`, `CLAUDE.md` and `docs/ROADMAP.md`.

- [ ] Document the tested build environment, command, artifact location and checksum verification. State that the `.img` represents a whole USB disk, not a kernel file to copy onto an ordinary filesystem.
- [ ] Give platform-specific raw-image writing instructions using a GUI writer or an explicitly selected device. Explain that writing the image replaces the USB contents. Never hardcode a disk number, auto-select a drive, or execute physical flashing as part of a build/test target.
- [ ] Document selecting the USB from Lenovo's one-time boot menu/Novo menu. Record the actual firmware settings and Secure Boot state used; do not prescribe unverified menu labels or require a firmware update without a concrete reason.
- [ ] On the physical laptop, run the portable checks from Task 7, type through the built-in keyboard, and keep two tasks running while interacting with the shell. Observe at least 10 minutes of responsiveness and perform three cold boots.
- [ ] Record the exact source revision, image hash, firmware version, CPU/RAM, tested USB port, input backend, screenshots/photos and test outcomes. Record skips and failures individually.
- [ ] Verify booting SurfOS did not write internal disks or alter the normal OS boot configuration. Document normal OS boot afterward.
- [ ] If the agent lacks physical access, deliver the tested emulator image and precise owner test instructions, then mark **native Lenovo validation pending**. Do not substitute QEMU results for this step.

**Acceptance:** Hardware evidence demonstrates the identified IdeaPad boots the published image into an interactive shell. If hardware validation is unavailable, the report states that limitation clearly.

## 8. Deferred features

The USB boot milestone does not require internal SSD access, post-boot USB mass-storage access, Wi-Fi/Bluetooth, touchpad/touchscreen, audio, accelerated graphics, multicore execution, a 64-bit kernel, battery reporting or suspend/resume.

For later persistent storage: identify the installed controller first, add AHCI or NVMe as appropriate, add GPT partition parsing, and address PCI BARs/interrupt routing/DMA constraints. For continued USB access: extend the USB stack with mass-storage transports. For networking: identify the actual wireless module and its firmware requirements; a USB Ethernet adapter also requires USB and a driver for that adapter.

These are separate milestones. Their absence must be reported without blocking a correctly scoped USB-booted shell.

## 9. Final delivery checklist

- [ ] Source changes and a reproducible documented image-build command.
- [ ] Raw `.img`, SHA-256 file and build manifest.
- [ ] Image-layout inspection and matching component hashes.
- [ ] Existing regression-suite results and O0/O2 USB/UEFI results.
- [ ] Framebuffer screenshots and keyboard-input evidence.
- [ ] Flashing and Lenovo boot instructions.
- [ ] Actual-hardware test report, or an explicit pending-hardware status.
- [ ] A concise list of supported and deferred devices.

Report completion in separate terms: **image built**, **UEFI/USB emulation verified**, **physical Lenovo boot verified**. Each requires its own evidence.
