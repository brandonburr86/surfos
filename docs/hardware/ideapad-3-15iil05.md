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
| Graphics | Intel UHD Graphics (in the CPU) | observed 2026-10-10: Intel `8086:8a56`, subsystem `17aa:3fb5`, revision 07, at `00:02.0` |
| Panel | 15.6", 1366x768 or 1920x1080 | observed 2026-10-10: 1920x1080, 32 bpp RGB, pitch 7680, framebuffer `0x70000000` |
| Memory | DDR4-2666, soldered plus one SO-DIMM | **unknown**: `memstat` |
| Storage | SATA 2.5" and/or PCIe NVMe M.2 | **unknown**: `lspci` (class 01); SurfOS has no driver for it and never touches it. Files go on a USB stick (**record its make, model and size**) |
| Networking | "No onboard Ethernet"; Wi-Fi + Bluetooth on an M.2 card (802.11ac or ax, varies) | observed 2026-10-12: Intel 9560, firmware `77.8dbafb52.0`, associated over 2.4 GHz WPA2 with DHCP address `10.0.0.164`; no USB Ethernet adapter attached |
| USB | one USB 2.0, two USB 3.2 Gen 1, on the chipset's xHCI controller | **record which port was used, and `lsusb`** |
| Keyboard | built-in, behind the embedded controller | observed 2026-10-10: 8042, IRQ 1 plus polling, command byte `0x65` in the pre-GUI kernel |
| Touchpad | Precision TouchPad (I2C HID on this generation) | works as a two-button mouse since 2026-10-15: Synaptics 06cb:ce2d at I2C1 0x2c, mouse mode set directly (it refuses GET_REPORT for InputMode); movement, both buttons, dragging. No multi-touch gestures ([GUI](../GUI.md)) |
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
| Which USB port the firmware numbers first, so which drive is usb0 | the SurfOS drive is recognised by its volume label SURFOSUSB, never by its number; `fdisk` and `mkfs` refuse it and it mounts read-only. 10/17/26: also by the serial number of the FAT volume GRUB was started from (`bootvol=`), whatever its label; that volume alone mounts read-write | every UEFI test; `storage` (the refusals, the read-only mount); `onestick`, `twosticks` |
| Whether the firmware boots a FAT32 stick the boot files were only copied onto (MBR, partition type 0c, not labelled SURFOSUSB), the owner's data stick for one (USB-BOOT.md section 14) | the removable-media path `\EFI\BOOT\BOOTX64.EFI` on any FAT partition; GRUB finds its own partition from `$cmdpath`; with the stick not offered by itself, F12 picks it once | `onestick`: OVMF boots an MBR FAT32 stick with 16 KB clusters. **Pending on the laptop**: record whether it starts by itself with USB first in the boot order, or needed F12 |
| Whether SurfOS can reach an internal disk | no AHCI or NVMe driver, so not on this laptop; an ATA disk, where one is found, is `internal: never formatted` | `make test-all`: QEMU's IDE disk is refused by `fdisk` and `mkfs` |
| A stick pulled out while mounted | the device is marked gone and its blocks are dropped; its mounts stay until `umount`; the next mount warns that it was not unmounted cleanly | `usbdisk`: unplugged while mounted, plugged in again |

## Evidence

### Native desktop test (2026-10-12)

The owner approved installing and rebooting the validated O0 desktop build.
The data stick received the kernel (1,752,536 bytes, CRC32 `34e40c18`) and
initrd (4,874,240 bytes, CRC32 `72bd1d95`). The previous development files were
saved in `/surfos-dev-backup-20261013-032913` on that stick. After reconnecting
Wi-Fi, telnet verified build ID `4ac143bfb43a-dirty 2026-10-13T03:21:34Z`.
The owner reported that the desktop works, but the built-in touchpad does not.
`pointerinfo` reports `none (no PS/2 mouse answered)` and zero pointing devices.
The native touchpad path therefore remains incomplete; no PS/2 compatibility
was observed. The original boot stick was not changed.

### Desktop inspection resumed (2026-10-12)

Telnet at `10.0.0.164` answered with build ID
`4ac143bfb43a 2026-10-11T14:15:41Z`. The framebuffer is still 1920x1080 at
`0x70000000`; the keyboard command byte is still `0x65`. The two I2C PCI
functions remain in D3hot with BAR0 zero. USB lists two storage devices, the
integrated camera and Bluetooth, with no USB pointing device. Wi-Fi provides
the active network connection. The current development boot starts DHCP and
telnet after a link appears, but does not save or rejoin a WPA network; after
reboot the owner must reconnect Wi-Fi locally before remote inspection resumes.

### Desktop input inventory (2026-10-10)

The owner enabled telnetd at `10.0.0.164`. Read-only `drivers`, `lspci -v`,
`acpi` and `bootinfo` queries verified the graphics/panel/keyboard fields above.
The firmware reports an 8042 and legacy devices. The old kernel's command byte
`0x65` has the auxiliary clock disabled. This cannot establish whether the
built-in touchpad exposes PS/2 compatibility; the new mouse probe must be booted.

PCI functions `00:15.0` (`8086:34e8`, subsystem `17aa:3815`) and `00:15.1`
(`8086:34e9`, subsystem `17aa:3818`) are present, with BAR0 zero, memory decoding
off, and power state D3hot. Device identity alone does not supply the touchpad's
ACPI resource, I2C slave address or HID descriptor address. Native I2C HID support
is still pending. No firmware setting, driver state or disk was changed by this
inventory. Emulator GUI evidence and this inventory are separate from a native
GUI/touchpad test.

### Basic 2D graphics (2026-10-10)

[The graphics driver](../GRAPHICS.md) extends the boot framebuffer with pixels,
filled rectangles, lines and RGB image copies. Use `gfxinfo` to record the actual
mode and `gfxtest 15` on the active local console to check the color bars. Check
Ctrl+C and F1/F2 text recovery too. It uses the existing firmware-selected mode
and CPU drawing; native Intel modesetting and acceleration remain future work.
Physical drawing validation of this extension is pending. Later telnet
inspection verified the PCI ID and panel mode recorded above, but did not
establish the on-screen drawing result.

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
| The banner (Brandon Burr's SurfOS, version 1.0, 2004-2026) and the prompt `SURF ≋>`: are ▸ and ≋ drawn cleanly? | |
| `cd /initrd/bin`: the prompt `initrd ▸ bin ≋>`; a failing command: `≋!>` with a red `!` | |
| Tab: `lsb` + Tab completes `lsblk`; `cat /initrd/e` + Tab completes `etc/`; a second Tab lists choices | |
| Arrows, Home, End, Backspace, Delete in the line; Up and Down for the history | |
| `help`: `--More--` after a screenful; Space, Enter and q (or Esc) on the built-in keyboard | |
| F1 to F4: each console has its own surfsh; `ps` lists four | |
| GRUB entry "SurfOS (kernel shell)": the prompt `SURF ≋#>` | |
| `wifiinfo`: the whole output (for the Wi-Fi driver; easiest copied from a telnet session) | |
| `telnetd on` (after `dhcp`): the address it prints | |
| From the Mac (`brew install telnet`), `telnet <address>`: the banner and `SURF ≋@>`; are ▸, ≋ and the colours right in the Mac's terminal? | |
| In the session: typing (does the echo lag?), Tab, `help` with `--More--`, Ctrl+C on `ping <router> 100` | |
| A second telnet window at the same time; `telnetd status` on the laptop lists both | |
| `exit` in one window, close the other; `dmesg` on the laptop: the session lines | |
| `telnetd off`, then `telnet <address>` again: "Connection refused" | |
| 10/17/26, on one stick: `wifi join HouseOfBurr` (the password), then `wifi save`: what it prints; `wifi saved` | |
| A restart: the `+NET:` lines; joined and leased with nobody typing? How many seconds after the GRUB menu? `ifconfig` | |
| `settings telnetd on`, a restart: `telnetd: listening on …` by itself; `telnet` from the Mac | |
| `tools/devloop.py deploy` with Wi-Fi only (no adapter): back by itself? | |
| The router's device list (the Xfinity app): is SurfOS listed as `surfos`? | |
| On the Mac: `surfos-config/` on the stick with both files; do they open in TextEdit? | |
| GRUB entry "SurfOS (without the settings on the stick)": no Wi-Fi joined, no telnetd | |

Ways to read the laptop's details outside SurfOS, if needed: on Windows, *System
Information* (`msinfo32`) and *Device Manager*; from a Linux live USB, `lspci -nn`,
`lsusb`, `cat /proc/cpuinfo` and `sudo dmidecode -t bios`.

### Touchpad firmware diagnostics (2026-10-12)

The desktop's PS/2 probe finds no pointing device on this laptop. The two visible
Intel I2C functions (8086:34e8 and 8086:34e9 at 00:15.0/1) have BAR0 unassigned
and report D3hot. Their presence alone does not identify the touchpad.

`acpidump DSDT` and `acpidump SSDT <instance>` export checksummed AML tables as
hex, without evaluating AML or changing hardware state. `acpi` lists SSDT instance
numbers. The host helper collects and validates their binary form:

```sh
python3 tools/acpi-fetch.py --host 10.0.0.164 --output-dir build/laptop-acpi
```

Use an empty output directory. The helper saves an inventory, binary tables,
and a SHA-256 manifest tied to the running build ID. ACPICA's `iasl -e ssdt-*.aml -d dsdt.aml`
can disassemble the DSDT with SSDT externals on the host. Tables up to 1 MiB are copied in chunks through the bounded temporary mapping
window; inaccessible,
malformed or bad-checksum tables are refused. Only DSDT and SSDT are exportable.

`acpinvs <hex-address> <bytes>` can read 1–256 bytes of firmware variables only
when the entire range is marked ACPI NVS in the boot memory map. It rejects
ordinary RAM, MMIO, crossed ranges and addresses above 4 GiB. Use it only for
specific fields identified in the decoded AML; it does not evaluate methods or
modify variables.


The revised diagnostic build exported the full 171,067-byte DSDT and nine SSDTs
with valid checksums. Native evidence is under
`build/validation/laptop-acpi-r2/` (ignored build output). Important observations:

- DSDT OEM `LENOVO`, table `CB-01`; host bridge 8086:8a02 / 17aa:3808;
  I2C1 at 00:15.1 is 8086:34e9 / 17aa:3818.
- Firmware's `SOD3` explicitly enters D3 and clears BAR0. `_PS0` is empty.
  SANV supplies the PCI producer window 0x65400000–0xbfffffff. The binding
  checks existing BARs, bridge apertures, RAM, modules and framebuffer before
  assigning a page within that window. Bus mastering remains disabled.
- PNVA gives PCHS=2, SBRG=0xfd000000, IM01=1 and IC01=0xa9000 (PCI config
  offset, not a BAR). The TPD0 interrupt GPIO resolves to 0xfd6e0780.
- GNVS I2C1 fast-mode timing fields give high=110, low=207, SDA hold=30.
- TPD0's LFCN.TPTY selects the device: 1 means ELAN at 0x15/descriptor 1;
  2 or 4 means Synaptics/Cirque at 0x2c/descriptor 0x20; 5 means FocalTech
  at 0x38/descriptor 1. The actual selector is read at runtime. LFCN is in
  reserved DRAM rather than NVS, so the public `acpinvs` command refuses it.
  Only the matching platform binding may read that bounded AML-named region.

The native driver followed; it works on the laptop since 2026-10-15 (below).
The diagnostic R2 image passed all 311 USB/UEFI checks at O0 and O2; those results
do not establish support by the newer native driver.

### Native touchpad: first boot (2026-10-13) and the InputMode fix (2026-10-15)

Build `7a269b053261-dirty 2026-10-13T03:50:06Z` (O0, development entry) on the
laptop, `touchpad info` (`build/validation/laptop-touchpad-n2/after.txt`):

- I2C1 powered to D0, BAR0 assigned at 0x65400000; LFCN.TPTY = 2, so address
  0x2c and HID descriptor register 0x20. Fast-mode timings 110/207, hold 30.
- HID descriptor read: **Synaptics 06cb:ce2d**, version 0x0102, report
  descriptor 665 bytes, maximum input 64 bytes. Power on, reset (its reset
  response read through the GPIO), power on again, report descriptor read.
- The report descriptor (now `tests/host/touchpad_06cb_ce2d.h`): report 2 is a
  mouse (two buttons, 8-bit relative X/Y), report 3 Precision Touchpad
  contacts, report 4 the InputMode feature (one 8-bit field, 0..10), report 6
  the surface/button switches, 7 the certification blob, 8 and 13 capabilities
  and latency, 9-12, 14 and 15 vendor reports.
- Start-up stopped at "HID read input mode": GET_REPORT(Feature, ID 4) aborted
  with IC_TX_ABRT_SOURCE 0x03000008 (TXDATA_NOACK, 6 commands flushed). Ten
  commands were queued (6 write bytes, 4 reads), so the device NACKed the 4th
  byte: the GET_REPORT opcode. The interrupt pad (DW0 at 0xfd6e0780) then read
  0x80100100, line low, where it read 0x80100102 before start-up.
- PS/2: no mouse; `pointerinfo`: 0 devices, no reports accepted.

Microsoft's configuration collection has the host set InputMode (0 is the mouse
collection), nothing reads it back, and a host-initiated reset clears it.
Linux's hid-multitouch only sets it. The 2026-10-15 build builds
the whole one-field report and sends SET_REPORT directly. GET-and-patch is kept
only for mode reports that share other data. A refused SET is recorded in
`touchpad info`'s mode line and is not fatal. The host suite now replays this
device: a simulated Synaptics that NACKs that GET runs the real start-up and
input code. `pointerinfo`'s device count also now includes the I2C touchpad.

### Native touchpad works (2026-10-15)

Build `784b294ab7d9 2026-10-15T22:47:22Z` (O0, development entry, pushed with
`tools/devloop.py deploy`; `build/validation/laptop-touchpad-n3/`):

- Before: the 2026-10-13 build still stopped at "HID read input mode" (abort
  0x03000008, tx 6, rx 0), the same failure as above.
- After: `+TOUCHPAD: ready (I2C HID mouse reports); variant=2 address=2c
  device=06cb:ce2d`. The device accepted SET_REPORT(Feature 4, mode 0): mode line
  "mouse (InputMode 0 set)", interrupt line high before and after (0x80100102).
  No transfer error, abort or controller re-init.
- The owner tested the touchpad in `gui`. A telnet sampler (`live.txt`) read
  `touchpad info` about three times a second meanwhile (349 samples): the left
  button in 21 samples (16 presses, some held while reports kept coming:
  drags), the right button in 6 (three presses), no other value.
- At the end: 2,374 reports, all decoded as report 2 (the mouse collection);
  0 ignored, 0 errors, 0 zero-length reads, 0 pointer queue drops;
  `pointerinfo`: 1 pointing device. While a finger moved, reports came at
  roughly 50-80 a second; the driver reads up to 300.

Not covered: multi-touch gestures (two-finger scrolling) and tap-to-click
behaviour in this mode, both decided by the device's firmware unless SurfOS
switches it to the Precision Touchpad collection (report 3).
