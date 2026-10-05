# Hardware record: Lenovo IdeaPad 3 15IIL05

The evidence file for the USB boot port ([../USB-BOOT.md](../USB-BOOT.md)). It keeps
three kinds of information apart: what was **observed** on the owner's laptop, what the
**family specification** says, and what is still **unknown**. Only an observation on the
laptop itself counts as native evidence; QEMU results are recorded separately.

## Identification

| Item | Value | Source |
|---|---|---|
| Model as reported by the owner | "IdeaPad 3 15IL05" | owner, 2026-10-05 |
| Matching Lenovo family | IdeaPad 3 15IIL05 (two capital I), machine type 81WE | Lenovo support site |
| Model on the underside label / in Windows System Information | **unknown: please record** | |
| Firmware (BIOS) version | **unknown: please record** (shown in firmware setup) | |

## Family specification versus this laptop

| Item | Family specification | This laptop |
|---|---|---|
| CPU | Core i3-1005G1 (Ice Lake, 2 cores / 4 threads, Intel 64) for the i3 tier | **unknown**: `cpuinfo` shows it |
| Graphics | Intel UHD Graphics (in the CPU) | **unknown**: `lspci` (class 03) |
| Panel | 15.6", 1366x768 or 1920x1080 | **unknown**: `bootinfo` shows the framebuffer mode |
| Memory | DDR4-2666, soldered plus one SO-DIMM | **unknown**: `memstat` |
| Storage | SATA 2.5" and/or PCIe NVMe M.2 | **unknown**: `lspci` (class 01); SurfOS never touches it |
| Networking | no Ethernet; Wi-Fi varies | not used by SurfOS |
| USB | one USB 2.0, two USB 3.2 Gen 1 | **record which port was used** |
| Keyboard | built-in, behind the embedded controller | **unknown**: `drivers` shows whether the 8042 answered |
| Touchpad | Precision TouchPad (I2C HID on this generation) | not used by SurfOS |
| Firmware | UEFI | **record**: Secure Boot setting, any CSM option, firmware version |

Sources: [Lenovo PSREF](https://psref.lenovo.com/syspool/Sys/PDF/IdeaPad/IdeaPad_3_15IIL05/IdeaPad_3_15IIL05_Spec.pdf),
[Intel i3-1005G1](https://www.intel.com/content/www/us/en/products/sku/196588/intel-core-i31005g1-processor-4m-cache-up-to-3-40-ghz/specifications.html).
They describe the family, not every installed component.

## Unknowns and how the kernel copes with each

None of these was guessed: each is either probed at boot or has a fallback that is
chosen automatically, and each fallback was exercised under QEMU.

| Unknown | What the kernel does | Tested under emulation by |
|---|---|---|
| Where the firmware keeps its own data in RAM | arenas placed from the memory map; the kernel image and modules checked against it (`BOOT PROBLEM` otherwise) | OVMF's map has ACPI NVS at 8-9 MB, inside the old fixed arenas: every UEFI boot |
| Framebuffer address (Ice Lake often puts it above 4 GB) | 4 KB pages below 4 GB, a PSE-36 4 MB page above (a reason is shown if neither works) | `highmem`: RAM at 0x100000000 written through a PSE-36 page, read back by the QEMU monitor |
| Framebuffer format and pitch | any 15/16/24/32-bit RGB or BGR mode from 640x400 up, pitch honoured | host unit tests; OVMF 1280x800x32, an earlier run at 800x600x24 |
| Whether the 8254 PIT runs (chipsets can gate its clock) | delays calibrated on the first clock seen counting (PIT, ACPI PM timer, HPET); if no PIT tick arrives within 100 ms the local APIC timer takes over | `no-pit`: QEMU without a PIT |
| Whether 8259 interrupts reach the CPU | local APIC in virtual wire mode (LINT0 ExtINT), I/O APIC inputs masked, stale in-service interrupts acknowledged | every laptop-profile boot; `fallback` |
| Whether the 8042 exists and has its IRQ on | probed with bounded waits, command byte rewritten, IRQ 1 plus polling on every tick | `keyboard` (IRQ), `fallback` (polling only) |
| The firmware's CR0/CR4 (OVMF hands over CR0.WP set) | CR0 and CR4 set explicitly; the originals are shown by `cpuinfo` | every UEFI boot |
| COM1 | probed (scratch register); absent is the normal case | `keyboard` (no serial port at all) |
| The FADT's 8042 flag (QEMU's says no 8042 although there is one) | recorded only; the probe decides | laptop profile under `-kernel` |

## Evidence

### Baseline before the port (2026-10-05, branch `ai-dev`, commit `3f476d2`)

Docker toolchain `surfos-modern-toolchain` (Debian bookworm, linux/amd64 under emulation
on Apple Silicon): GCC 12.2.0, binutils 2.40, NASM 2.16.01, QEMU 7.2 in the container;
host QEMU 11.1.1 (Homebrew).

| Step | Command | Result |
|---|---|---|
| Kernel build | `devenv/build.sh` after `devenv/build.sh clean` | exit 0, 726,412-byte kernel; warnings only in untouched 2004 files |
| Host unit tests | `devenv/build.sh unittest` | `BLIBC UNIT TESTS PASSED: 45 checks` |
| QEMU suite | `devenv/build.sh test-all` | `SMOKE TEST PASSED: 42 checks` at -O0 and at -O2 |
| Host QEMU | `tools/qemu-run.py --test` | 41 checks pass (no mtools on the Mac); needed a path without spaces until the harness copied the initrd |

### Emulator results for the port (2026-10-05)

Firmware: Homebrew QEMU 11.1.1 `edk2-x86_64-code.fd` with a fresh copy of
`edk2-i386-vars.fd` per boot, machine q35, 2 CPUs, xHCI + usb-storage (removable), no
IDE disk, no network card. GRUB 2.06-13+deb12u2 (Debian), mtools 4.0.32.

| Check | Result |
|---|---|
| `make unittest` | blibc 45, boot information 50, framebuffer 39 checks: all pass |
| `make test-mkusb` | 8 tests pass (GPT CRCs, layout, files, manifest, refusals, failure keeps the old image) |
| `make test-all` | 42/42 at -O0 and -O2 (the Multiboot 1 path is unchanged) |
| `make test-usb` (`tools/qemu-usb.py --test`) | 91/91 checks at -O0 and 91/91 at -O2, 7 cold boots each: serial (512 MB), keyboard-only without a serial port (4 GB), high memory (6 GB), troubleshooting entry, no PIT, image without initrd, image without kernel |
| `tools/qemu-run.py --test` with host QEMU | 46/46 at -O0 and -O2 (the five new commands included) |

The tests keep their transcripts and screenshots in `build/usb-test/` (not in git).

### Physical laptop: pending

**Native Lenovo validation has not happened yet.** Nothing above was run on the laptop.
Please fill in a copy of this table for each boot (photos of the screen help most):

| Field | Value |
|---|---|
| Date | |
| Image file and its sha256 (from `.sha256` or `BUILD-INFO.txt`) | |
| Source revision (from `BUILD-INFO.txt`) | |
| Firmware version, Secure Boot setting, other settings changed | |
| USB port used (USB 2.0 or 3.2) | |
| Reached the GRUB menu? | |
| Reached the `SurfOS*>` prompt? | |
| Panel mode (`bootinfo`: framebuffer line) and text scale | |
| `cpuinfo` output | |
| `drivers` output (console, timer, keyboard, apic lines) | |
| `acpi` output | |
| `memstat` first lines | |
| Built-in keyboard: typing, Shift, Backspace, Up for history, F1-F4 | |
| `tick` twice a few seconds apart; `sleep 1000` | |
| `run hello a b`, `spawn count 20 500` while typing other commands | |
| `selftest` result | |
| Ten minutes of use: still responsive? | |
| Three cold boots: all the same? | |
| Normal operating system boots afterwards without the stick? | |
| Anything else on screen (`BOOT PROBLEM`, `Unexpected trap`, warnings) | |

Ways to read the laptop's details outside SurfOS, if needed: on Windows, *System
Information* (`msinfo32`) and *Device Manager*; from a Linux live USB, `lspci -nn`,
`lsusb`, `cat /proc/cpuinfo` and `sudo dmidecode -t bios`.
