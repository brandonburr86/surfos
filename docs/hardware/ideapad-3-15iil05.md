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
| Storage | SATA 2.5" and/or PCIe NVMe M.2 | **unknown**: `lspci` (class 01); SurfOS has no driver for it and never touches it. Files go on a USB stick (**record its make, model and size**) |
| Networking | "No onboard Ethernet"; Wi-Fi + Bluetooth on an M.2 card (802.11ac or ax, varies) | Wi-Fi not used by SurfOS; wired network through a **USB Ethernet adapter** (CDC ECM/NCM, e.g. RTL8153): **record the adapter and its `lsusb` line** |
| USB | one USB 2.0, two USB 3.2 Gen 1, on the chipset's xHCI controller | **record which port was used, and `lsusb`** |
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
| Where the USB controller's registers are (a 64-bit BAR; Ice Lake firmware often puts it above 4 GB) | mapped uncached, through a PSE-36 4 MB page above 4 GB (`ioremap64()`) | OVMF puts QEMU's xHCI at 0xc000000000: every UEFI boot |
| Whether the firmware still owns the USB controller (legacy USB support) | the xHCI ownership handshake (up to 1 s), USB SMIs off; on a timeout the controller is taken anyway, with a message | not exercised: QEMU's xHCI has no legacy-support capability |
| Whether the controller's interrupt reaches the CPU (the I/O APIC is masked) | not needed: the event ring is polled on every tick | every test |
| Context size, scratchpad pages, port power switches, USB 2/3 port numbering | read from the capability registers and the supported-protocol capability | QEMU: 32-byte contexts, no scratchpad, USB 3 ports first; Intel's 64-byte contexts and scratchpad only on the laptop |
| How long the adapter's USB 3 link takes to train | up to 1 s, then a warm reset; a device that appears later is set up by the `usb` thread | QEMU's USB 3 ports come up at once (not exercised) |
| What else is on the USB ports (webcam, Bluetooth, the SurfOS drive) | each is given an address and named; none gets a driver; hubs are reported as not supported | the SurfOS drive itself in every UEFI test |
| Which network adapter, and which configuration it offers | the first configuration with a CDC ECM function, else NCM (an RTL8153's second configuration) | QEMU's usb-net (RNDIS first, ECM second) in `usbnet`; RTL8153- and NCM-shaped descriptors in `tests/host/test_usb.c` |
| Which USB stick, and how well it follows the SCSI commands | the common subset (INQUIRY, TEST UNIT READY, READ CAPACITY 10 then 16, READ/WRITE 10, MODE SENSE for the write-protect switch, SYNCHRONIZE CACHE); a failed command is followed by REQUEST SENSE, a stuck stick gets a Bulk-Only reset and endpoint resets; a stick that is not ready yet is asked ten times over a second, then every two seconds by the `usb` thread | QEMU's usb-storage (`usbdisk`, `storage`); the error paths by code review only |
| The stick's sector size | 512-byte sectors only; any other size is refused with a `+USB:` message, never misread | not exercised: QEMU's sticks have 512-byte sectors |
| Which USB port the firmware numbers first, so which drive is usb0 | the SurfOS drive is recognised by its volume label SURFOSUSB, never by its number; `fdisk` and `mkfs` refuse it and it mounts read-only | every UEFI test; `storage` (the refusals, the read-only mount) |
| Whether SurfOS can reach an internal disk | no AHCI or NVMe driver, so not on this laptop; an ATA disk, where one is found, is `internal: never formatted` | `make test-all`: QEMU's IDE disk is refused by `fdisk` and `mkfs` |
| A stick pulled out while mounted | the device is marked gone and its blocks are dropped; its mounts stay until `umount`; the next mount warns that it was not unmounted cleanly | `usbdisk`: unplugged while mounted, plugged in again |

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

### Emulator results for USB networking (2026-10-05)

The same firmware and machine, plus QEMU's `usb-net` (CDC ECM and RNDIS, full speed) on
the xHCI controller next to the stick, on QEMU's user network.

| Check | Result |
|---|---|
| `make unittest` | blibc 45, boot information 50, framebuffer 39, USB 57 checks: all pass |
| `make test-all` | 48/48 at -O0 and -O2 (`lsusb` added; no USB controller there) |
| `make test-usb` | 117/117 at -O0 and -O2, 8 cold boots each: the seven above, plus `usbnet` (adapter at boot, DHCP, ping, HTTP, 1 MB at 2-3 MB/s, self test, unplug, plug back in with the address kept, a second adapter on 192.168.76.0/24) |

The xHCI driver met QEMU's controller (1b36:000d) with its registers above 4 GB, a
SuperSpeed storage device and a full-speed network device. Intel's controller differs
(64-byte contexts, scratchpad pages, the firmware handshake, slower USB 3 links): those
paths are written to the specification but only the laptop can exercise them.

### Emulator results for USB storage (2026-10-06)

The same firmware and machine with two more sticks on the xHCI controller: a blank 1 GB
one and a 64 MB FAT32 one made by `mkfs.fat` and `mcopy` (long, accented and
macOS-style names, three folders deep, a 1 MB file). Host checks: macOS 15.1.1
`fsck_msdos -n` through `hdiutil attach -nomount -readonly`, `fsck.fat -n` from dosfstools
4.2 (Debian, in the devenv container), and the FAT32 reader of `tools/mkusb.py`.

| Check | Result |
|---|---|
| `make unittest` | blibc 45, boot information 50, framebuffer 51, USB 57, partitions 143, partition tools 33, C library 195, mkfs 115 checks: all pass |
| `make test-all` | 140/140 at -O0 and -O2 (lsblk, fdisk, mkfs, mount, eject and the user programs on QEMU's IDE disk and a scratch ramdisk) |
| `make test-usb` | 202/202 at -O0 and -O2, 11 cold boots each: the eight above, plus `fullhd` (1920x1080: a 120x33 grid, lines past 80 columns, the ANSI sequences the editor draws with, F1/F2), `usbdisk` (a FAT16 stick at boot and plugged in later, unplugged while mounted) and `storage` (39 checks: the boot drive refused and mounted read-only; the foreign stick read; the blank stick partitioned, formatted, written through `fstest`, `sh`, `mv` and the editor, ejected; then `fsck_msdos` and `fsck.fat` find no errors on either stick and the files read back on the host) |

QEMU's sticks answer every command at once and never stall. Real flash drives are slower,
some answer "not ready" for a while after being plugged in, and a few get commands wrong:
only the laptop can show how a given stick behaves.

### Physical laptop

**2026-10-05: the owner reports that the USB image boots SurfOS on the laptop.** No
details were recorded yet.

**2026-10-05: the owner reports that the network interface works on the laptop** through a
USB Ethernet adapter, with the image built from `5dfb279` (sha256 `df6510f2…04d6`). The
adapter's make and chip and the command output were not recorded; the network rows of the
table below are still open.
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
| `lsusb` (the controller line, and each device) | |
| Network adapter: make, model, chip (box or product page) | |
| `lsusb` line of the adapter, the `+USB: port …` boot line | |
| `ifconfig` with the cable in; `dhcp`; `ping` the router; `ping example.com` | |
| `httpget example.com 80 /`; `selftest` | |
| Adapter unplugged and plugged in again: messages, `ifconfig`, `ping` | |
| Text on the screen: the `bootinfo` framebuffer line (`… text at scale …`); does it fill the screen? | |
| USB stick: make, model, size; its `+USB: usbN: …` line; `lsblk` | |
| `fdisk usbN new mbr`, `fdisk usbN add`, `mkfs usbNp1 LABEL`: output | |
| `mount usbNp1 /data`, `run fstest /data`, `df` | |
| `edit /data/notes.txt`: typing, arrows, Ctrl+S, Ctrl+Q; `cat /data/notes.txt` | |
| `readbench usbN 16` and `fsbench /data 16`: the speeds | |
| `eject usbN`; then on the Mac: the files there? *Disk Utility* → *First Aid* result | |
| A file written on the Mac, read in SurfOS after `mount` | |

Ways to read the laptop's details outside SurfOS, if needed: on Windows, *System
Information* (`msinfo32`) and *Device Manager*; from a Linux live USB, `lspci -nn`,
`lsusb`, `cat /proc/cpuinfo` and `sudo dmidecode -t bios`.
