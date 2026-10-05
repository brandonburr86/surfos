# Booting SurfOS from a USB drive on the Lenovo IdeaPad 3 15IIL05

SurfOS can boot on a real UEFI PC from a USB flash drive. The target is the Lenovo
IdeaPad 3 15IIL05 (machine type 81WE, 10th-generation Core i3). The drive holds its
own boot loader, kernel and programs, and SurfOS runs directly on the laptop's
processor, screen and keyboard: there is no emulator on the drive. QEMU appears only on
the build machine, where it tests the image before it reaches real hardware (step 2).
Nothing is installed on the laptop, its internal disk is never touched, and pulling the
drive out and restarting brings the laptop back to its normal operating system.

| Stage | Status | Evidence |
|---|---|---|
| Image built | done | `make usb-image`, checked by `make test-mkusb` |
| The same image tested under emulation | done | `make test-usb`: 8 cold boots under OVMF, 117 checks, at -O0 and -O2 |
| Boot on the physical IdeaPad | **reported working by the owner (2026-10-05)** | the details are still to be recorded in [hardware/ideapad-3-15iil05.md](hardware/ideapad-3-15iil05.md) |
| Wired network through a USB Ethernet adapter | done under emulation; **pending on the laptop** (needs an adapter) | `make test-usb`, scenario `usbnet`; [section 8](#8-networking-a-usb-ethernet-adapter) |

## What is on the drive

The image `surfos-ideapad-15iil05.img` is a whole 256 MB disk, not a file to copy onto
an existing drive:

```text
GPT partition table (backup copy at the end of the disk)
└─ partition 1: EFI system partition, FAT32, label SURFOSUSB, 1 MB to 255 MB
   ├─ /EFI/BOOT/BOOTX64.EFI   GRUB for 64-bit UEFI (the path firmware tries on removable drives)
   ├─ /boot/grub/grub.cfg     the menu
   ├─ /boot/surfos.bin        the SurfOS kernel (32-bit, Multiboot 2)
   ├─ /boot/initrd.tar        the user programs and rootfs/, loaded into RAM as ramdisk rd0
   └─ /boot/BUILD-INFO.txt    source revision, build date, checksums
```

The boot path: laptop firmware → `BOOTX64.EFI` (GRUB) → `multiboot2 /boot/surfos.bin
profile=laptop` + `module2 /boot/initrd.tar` → SurfOS draws on the firmware's
framebuffer and takes input from the built-in keyboard.

## 1. Build the image

On the Mac, with Docker Desktop running:

```sh
cd 06-restoration
devenv/build-usb.sh          # optimized build (O=2): build/usb/surfos-ideapad-15iil05.img
O=0 devenv/build-usb.sh      # or the debug build
```

The script builds in the Docker toolchain (GCC 12, GRUB 2.06, mtools) and copies three
files to `build/usb/`, checking the checksum as it does:

```text
build/usb/surfos-ideapad-15iil05.img          the disk image
build/usb/surfos-ideapad-15iil05.img.sha256   its SHA-256
build/usb/build-manifest.json                 source revision, tools, layout, file hashes
```

On Linux with the toolchain installed (`gcc-multilib nasm grub-common grub-efi-amd64-bin
mtools`), `make usb-image O=2` does the same without Docker. The image is only ever
written as a regular file: no build or test step writes to a disk device.

## 2. Try it in QEMU first (recommended)

```sh
devenv/run-usb.sh       # a QEMU window with UEFI firmware, booting a copy of the image from a virtual USB stick
devenv/test-usb.sh      # the automatic tests: 7 cold boots, about a minute and a half
```

Both need `qemu-system-x86_64` with its EDK2 UEFI firmware (Homebrew's `qemu` has both).
The tests leave their serial transcripts and screenshots in `build/usb-test/`. In the
window the GRUB menu appears, then the SurfOS boot log and the `SurfOS*>` prompt.

## 3. Check the image

In `build/usb/`:

```sh
shasum -a 256 -c surfos-ideapad-15iil05.img.sha256      # macOS
sha256sum -c surfos-ideapad-15iil05.img.sha256          # Linux
certutil -hashfile surfos-ideapad-15iil05.img SHA256    # Windows: compare with the .sha256 file
```

`OK` (or matching hashes) means the file is exactly what was built.

## 4. Write it to a USB drive

Use any USB flash drive of 512 MB or more. **Writing the image erases everything on the
drive.** Pick the drive carefully: the wrong choice can erase a disk you care about.

**With a graphical writer (easiest).** These write the image raw, which is what it needs:

* balenaEtcher (macOS, Windows, Linux): *Flash from file* → the `.img` → *Select target*
  → your USB drive → *Flash*.
* Raspberry Pi Imager: *Choose OS* → *Use custom* → the `.img` → *Choose storage* → the
  drive → *Write* (skip any OS-customisation offer).
* Rufus (Windows): choose the drive and the `.img`. If it asks how to write, choose DD
  image mode.

**From the macOS command line**, if you prefer:

```sh
diskutil list                         # note the disks, insert the stick, run it again:
diskutil list                         # the new "external, physical" disk is the stick, e.g. /dev/disk4
diskutil unmountDisk /dev/diskN       # N from the line above, never disk0 or disk1
sudo dd if=build/usb/surfos-ideapad-15iil05.img of=/dev/rdiskN bs=4m
diskutil eject /dev/diskN
```

On Linux, find the stick with `lsblk` and use `sudo dd if=… of=/dev/sdX bs=4M
conv=fsync status=progress` with the whole-disk device (`/dev/sdX`, not `/dev/sdX1`).

Afterwards macOS or Windows may show a small drive called SURFOSUSB. That is the
system partition; leave its files alone.

## 5. Prepare the laptop (once)

SurfOS's GRUB is not signed for Secure Boot, so **Secure Boot must be turned off** in
the laptop's firmware setup:

1. **If the laptop runs Windows with BitLocker or "Device encryption" on, find your
   recovery key first** (at https://account.microsoft.com/devices/recoverykey, or
   *Manage BitLocker* in Windows), or suspend BitLocker. Changing Secure Boot can make
   Windows ask for that key on its next start.
2. Shut the laptop down completely, then open the firmware setup: power it on and tap
   **F2** (on some keyboards **Fn+F2**), or use the Novo button menu (a small button or
   pinhole beside the power connector on many IdeaPads) and choose *BIOS Setup*.
3. Find **Secure Boot** (usually on the *Security* tab) and set it to **Disabled**. Save
   and exit (usually **F10**).

Menu names differ between firmware versions; Lenovo's user guide for your model shows
the current ones. If there is a *USB Boot* setting, it must be *Enabled*. No *Legacy*
or *CSM* setting is needed: the drive boots in UEFI mode. Note what you changed and the
firmware version setup shows, for the hardware record.

## 6. Boot SurfOS

1. Plug the drive into one of the laptop's USB ports.
2. Power on and tap **F12** (or **Fn+F12**) for the one-time boot menu, or use the Novo
   button menu → *Boot Menu*.
3. Pick the USB drive (shown as *EFI USB Device* or by the drive's name).
4. GRUB's menu waits 5 seconds and starts **SurfOS**. The screen fills with the boot log,
   then the red *Phrite's SurfOS* banner and the `SurfOS*>` prompt.

On a 1366x768 panel the 80x25 text screen is drawn 1:1 in the middle of the display; on
a 1920x1080 panel it is drawn at twice the size.

The GRUB menu also has:

* **SurfOS (troubleshooting: local APIC timer, polled keyboard)**: the same kernel with
  `timer=lapic kbd=poll`, for when the normal entry boots but the clock or the keyboard
  does not respond.
* **UEFI firmware setup**: restarts into the firmware's setup screens.
* **Restart** and **Power off**.

Nothing is written to the laptop. To go back to the normal operating system, take the
drive out and restart (`reboot` in SurfOS, or hold the power button). Turn Secure Boot
back on afterwards if you want it on.

## 7. What to try

Type at the `SurfOS*>` prompt:

| Command | Expect |
|---|---|
| `help` | the command list |
| `bootinfo` | `multiboot2`, `profile=laptop`, `firmware UEFI`, the memory map, the framebuffer |
| `drivers` | which console, timer and keyboard paths were chosen |
| `cpuinfo` | the processor (an i3-1005G1 should say so) |
| `acpi` | the firmware's ACPI tables |
| `memstat`, `ps`, `tick`, `uptime` | memory, tasks, the clock moving |
| `lsblk`, `mount`, `ls /initrd` | the ramdisk rd0 mounted on `/initrd` |
| `cat /initrd/etc/version` | `SurfOS 0.007 (ai-dev)` |
| `run hello a b` | a ring-3 program, `exit status 42` |
| `count 3 10`, `spawn count 20 500` | user programs, one in the background |
| `heaptest`, `selftest` | `HEAPTEST PASS`, `SELFTEST PASS` (the disk tests are skipped, and the network tests without an adapter and a cable) |
| `lsusb` | the USB controller and what is plugged in: the SurfOS drive itself, the webcam, Bluetooth |
| F1 to F4 | four consoles, each with its own shell |

Please record the results, and photos of the screen, in
[hardware/ideapad-3-15iil05.md](hardware/ideapad-3-15iil05.md). The output of `cpuinfo`,
`acpi`, `drivers`, `bootinfo`, `lspci` and `lsusb` answers most of its open questions.

## 8. Networking: a USB Ethernet adapter

The IdeaPad 3 15IIL05 has no Ethernet socket (Lenovo's specification: "No onboard
Ethernet"), and its only network hardware is the Wi-Fi card, which SurfOS has no driver
for. SurfOS reaches a wired network through a **USB-to-Ethernet adapter** in one of the
laptop's USB ports. Its USB stack drives the laptop's USB 3 controller (xHCI) and any
adapter that speaks one of the two standard USB networking classes, **CDC ECM** or **CDC
NCM**.

**What to buy.** The safest choice is an adapter built on the **Realtek RTL8153** chip
(USB 3.0, gigabit). These are the most common USB-A gigabit adapters, and besides
Realtek's own protocol they offer a standard CDC Ethernet (ECM) mode, which SurfOS uses.
The TP-Link UE300 is one. Look for "RTL8153" on the product page or the box. Also fine,
but not yet tried on hardware: adapters that list CDC ECM or NCM support (for example the
2.5 gigabit Realtek RTL8156, which uses NCM). Avoid these:

* Adapters that speak only a vendor protocol. Many ASIX adapters (AX88179, AX88772) do.
  `lsusb` then shows the adapter with `no driver`.
* Docks and multi-port dongles (USB hub + Ethernet + card reader + …). SurfOS has no
  hub support yet, so the adapter inside such a dongle is not seen. Use a single-purpose
  adapter, plugged straight into the laptop.
* A USB-C-only adapter. This laptop has USB-A ports, so it would need a converter.

Any of the three USB ports works. The USB 3 ports run the adapter at 5 Gbit/s, the USB 2
port at 480 Mbit/s; SurfOS itself manages a few megabytes per second either way.

**Using it.** Plug in the adapter and an Ethernet cable from your router or switch,
before or after starting SurfOS, then:

| Command | Expect |
|---|---|
| `lsusb` | the adapter, ending in `-> eth0, cdc-ecm, MAC …` |
| `ifconfig` | `eth0: …, link up` (`link down` until the cable is plugged in) |
| `dhcp` | `dhcp: eth0: lease 192.168.… from …`: an address, netmask, gateway and DNS server from your router |
| `ping 192.168.1.1` (your router) | replies |
| `ping example.com` | the name looked up through the DNS server, then replies |
| `httpget example.com 80 /` | a web page over TCP |
| `selftest` | `SELFTEST PASS`; on your network it checks the lease and that the gateway answers (the rest of its network checks need QEMU's network and are skipped) |

On a network without DHCP, set the address by hand:
`ifconfig eth0 192.168.1.50 255.255.255.0 192.168.1.1 192.168.1.1` (address, netmask,
gateway, DNS server).

When the adapter is unplugged, `eth0` stays (with `link down`). Plugged back in, it
is `eth0` again, and with the same adapter it keeps its address, so no new `dhcp` is
needed. A different adapter also becomes `eth0`, but needs `dhcp` again.

The GRUB menu has an entry **SurfOS (USB controller off: no USB network adapter)**. It
boots without touching the USB controller, in case the USB driver ever gets in the way
on the laptop.

## 9. When something goes wrong

| What you see | What it means, what to do |
|---|---|
| The USB drive is not in the F12 menu | Write the image again with one of the writers above; try the other USB ports; check that Secure Boot is off and that USB boot is enabled. |
| "Secure Boot Violation", "Verification failed", or a red security warning | Secure Boot is still on (step 5). |
| GRUB says `file '/boot/…' not found`, or "SurfOS: /boot/surfos.bin … were not found" | The drive does not hold the image as written by a raw writer: write it again (step 4). |
| GRUB's menu, then a blank screen | The kernel could not use the firmware's framebuffer. Photograph the GRUB screen and note the panel resolution; report it. |
| The boot log stops, or ends with `SYSTEM HALTED` or a line starting `BOOT PROBLEM` | Photograph the screen and report it: those lines name what failed. |
| The prompt appears but keys do nothing | Restart and choose the troubleshooting entry. USB keyboards are not supported (the USB stack drives network adapters only), so use the built-in keyboard. |
| The boot stops at a line starting `+xHCI` or `+USB` | Restart and choose "SurfOS (USB controller off)". Please report the last lines on the screen. |
| `lsusb` does not list the adapter | Try another port, and plug the adapter straight into the laptop (not through a hub or a dock). A line `(connected, not set up)` or `+USB: port …: … failed` means the adapter did not answer: unplug it, wait two seconds, plug it in again. |
| `lsusb` lists the adapter with `no driver` | It speaks only its maker's protocol: use an RTL8153 or another CDC ECM/NCM adapter. |
| `ifconfig` says `link down` | Check the cable and that the other end (router, switch) is on: the adapter reports the link it sees. |
| `dhcp: eth0: no offer` | Nothing on that network hands out addresses: set one by hand (section 8). |
| `ping` by address works, by name does not | No DNS server is set: `dhcp` sets one, or give it as the last `ifconfig` value. |
| `tick` does not change between runs | Choose the troubleshooting entry (it uses the local APIC timer). |
| Windows asks for a BitLocker recovery key | Enter the key you saved (step 5). It happens once after Secure Boot settings change. |

SurfOS has no ACPI power-off: `reboot` restarts the laptop; to switch it off, hold the
power button.

## What works, what does not

| Works on the laptop (verified under emulation) | Not supported yet |
|---|---|
| UEFI boot from USB, GRUB, Multiboot 2 | Secure Boot (GRUB is unsigned) |
| Framebuffer console, 80x25 cells, VGA colours, 4 consoles | USB keyboards, mice and storage; USB hubs and docks |
| Built-in keyboard through the 8042 (IRQ 1, and polled as a fallback) | The touchpad |
| PIT timer, or the local APIC timer when the PIT does not tick | The internal SSD or hard disk (no AHCI/NVMe driver: they are never touched) |
| Delays calibrated on the PIT, the ACPI PM timer or the HPET | Wi-Fi, Bluetooth, sound, battery, suspend, ACPI power-off |
| RAM accounting above 4 GB (the kernel itself uses the RAM below 4 GB) | More than one CPU core |
| Ramdisk initrd, user programs, system calls, the CMOS clock, reboot | Network adapters that speak only a vendor protocol (ASIX, older Realtek modes) |
| The USB 3 controller; wired networking through a USB Ethernet adapter (CDC ECM/NCM, RTL8153): DHCP, ping, DNS, TCP; unplugging and plugging back in | |

## How it works

* **Boot.** The kernel carries a Multiboot 2 header next to its Multiboot 1 header
  (`boot/boot.S`). GRUB loads it on UEFI, exits the firmware's boot services and starts it
  in 32-bit protected mode with a Multiboot 2 information block; `kernel/multiboot2.c`
  checks every tag against the block's size before using it.
* **Memory.** The memory map decides where the page tables, the free-frame stack and the
  DMA heap go (`mm/pmm.c`); they used to sit at fixed addresses where UEFI firmware keeps
  data. RAM above 4 GB is counted with 64-bit arithmetic and left unused.
* **Screen.** `driver/framebuffer.c` draws the console's 80x25 cells into the firmware's
  framebuffer with an 8x16 font (Spleen), scaled by a whole number and centred; a
  framebuffer above 4 GB is reached with a 4 MB page (PSE-36), write-combining through PAT.
* **profile=laptop** (`kernel/main.c`): ACPI tables, the local APIC in virtual wire mode
  with the I/O APICs masked, the 8042 set up again and polled, ISA devices skipped, only
  the initrd mounted, and a timer that must prove it ticks. Without the word on the command
  line (QEMU's `-kernel`, the GRUB ISO) the kernel boots as before.
* **USB** (`driver/usb/`). The xHCI driver takes the controller over from the firmware and
  resets it. It polls the controller's event ring on every timer tick instead of waiting
  for its interrupt, which on this laptop could not reach the processor anyway. The device
  layer sets up whatever is plugged into the laptop's ports, at boot and whenever a port
  changes, and picks the first configuration a driver wants. For an RTL8153 that is its
  CDC Ethernet configuration, not Realtek's own. The CDC ECM/NCM driver turns the adapter
  into `eth0` for the existing network stack.

The design is in [ARCHITECTURE.md](ARCHITECTURE.md) section 14 and the original brief in
[superpowers/plans/2026-10-05-lenovo-ideapad-3-15iil05-usb-boot.md](superpowers/plans/2026-10-05-lenovo-ideapad-3-15iil05-usb-boot.md).

## Make targets

```sh
make usb-image [O=2]     # build/usb/surfos-ideapad-15iil05.img + .sha256 + build-manifest.json
make test-usb  [O=2]     # boot that image under OVMF from an emulated USB stick (tools/qemu-usb.py --test)
make test-mkusb          # the image builder's own tests (tools/mkusb.py)
tools/mkusb.py --inspect build/usb/surfos-ideapad-15iil05.img   # list and check an image
```

`make test-usb` finds OVMF in the usual places (Homebrew, Debian/Ubuntu `ovmf`) or takes
`OVMF_CODE=… OVMF_VARS=…`.
