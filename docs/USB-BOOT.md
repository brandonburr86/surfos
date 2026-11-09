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
| The same image tested under emulation | done | `make test-usb`: 26 scenarios under OVMF, at -O0 and -O2 |
| Basic 2D graphics: pixels, rectangles, lines and RGB images | done under emulation; **pending on the laptop** | `gfxinfo`, `gfxtest`; `make test-usb`, scenario `graphics`; [graphics driver](GRAPHICS.md) |
| Boot on the physical IdeaPad | **reported working by the owner (2026-10-05)** | the details are still to be recorded in [hardware/ideapad-3-15iil05.md](hardware/ideapad-3-15iil05.md) |
| Wired network through a USB Ethernet adapter | done under emulation; **reported working on the laptop by the owner (2026-10-05)** | `make test-usb`, scenario `usbnet`; [section 8](#8-networking-a-usb-ethernet-adapter) |
| Files on a second USB stick: partitions, FAT32 that the Mac reads, a text editor, a C library | done under emulation; **pending on the laptop** | `make test-usb`, scenarios `usbdisk` and `storage`; [section 9](#9-files-on-a-usb-stick) |
| The SurfOS shell, surfsh: the breadcrumb prompt, Tab completion, a pager; SurfOS 1.0 | done under emulation; **pending on the laptop** | `make test` and `make test-usb`; [section 10](#10-the-shell) |
| Telnet into the laptop from another computer: `telnetd`, surfsh over the network | done under emulation; **pending on the laptop** | `make test`, `make test-tcp` and `make test-usb` (scenario `usbnet`); [section 11](#11-telnet-the-shell-from-another-computer) |
| Development builds over the network: a data stick, GRUB's development entry, `autonet`, `rx`, `tools/devloop.py` | done under emulation; **pending on the laptop** | `make test`, `make test-usb` (scenario `devloop`), `make test-devloop`; [section 13](#13-development-builds-over-the-network) |
| One stick for everything: the boot files copied onto a FAT32 stick that keeps your files (`tools/stick.py`), new builds copied into it | done under emulation; **pending on the laptop** | `make test-usb` (scenarios `onestick` and `twosticks`), `make unittest`, `make test-mkusb`; [section 14](#14-one-stick-for-everything) |
| Saved Wi-Fi networks joined by themselves at every start, and the settings on the stick (telnet at startup, Ethernet's DHCP, a host name) | done under emulation (a simulated Wi-Fi chip); **pending on the laptop** | `make test` (boots with a settings disk), `make test-usb` (scenario `settings`), `make unittest` (`test_netcfg`); [section 15](#15-wi-fi-saved-networks-and-the-settings-on-the-stick) |
| A web server: websites from a folder of a stick, opened in a browser on another computer (`httpd`) | done under emulation; **pending on the laptop** | `make test`, `make unittest` and `make test-usb` (scenario `usbnet`); [section 16](#16-a-web-server-hosting-a-site) |
| Files to and from other computers: an FTP server for the Mac's Finder, curl and FTP programs (`ftpd`), and `nc` both ways | done under emulation, and with the Mac's own curl, Finder and `nc` against SurfOS in QEMU; **pending on the laptop** | `make test`, `make unittest`, `make test-tcp` and `make test-usb` (scenario `usbnet`); [section 17](#17-files-to-and-from-other-computers-ftp-and-nc) |
| Applications on the desktop: Settings (Wi-Fi and the settings), Spreadsheet, Code Editor, Paint, Chat (IRC), Mines, Solitaire and Swell, and the Image Viewer as a program | done under emulation; **pending on the laptop** | `make unittest` and `make test-usb` (scenarios `guiapps`, `guichat` and `wsettings`); [GUI](GUI.md) |

## What is on the drive

The image `surfos-ideapad-15iil05.img` is a whole 256 MB disk, not a file to copy onto
an existing drive:

```text
GPT partition table (backup copy at the end of the disk)
└─ partition 1: EFI system partition, FAT32, label SURFOSUSB, 1 MB to 255 MB
   ├─ /EFI/BOOT/BOOTX64.EFI   GRUB for 64-bit UEFI (the path firmware tries on removable drives)
   ├─ /boot/grub/grub.cfg     the menu
   ├─ /boot/grub/x86_64-efi/fs.lst   empty: GRUB loads no file system modules (section 13)
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
devenv/test-usb.sh      # the automatic tests: 26 scenarios, several minutes
```

Both need `qemu-system-x86_64` with its EDK2 UEFI firmware (Homebrew's `qemu` has both).
The tests leave their serial transcripts and screenshots in `build/usb-test/`. In the
window the GRUB menu appears, then the SurfOS boot log, the banner and the `SURF ≋>` prompt.

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
   then the banner, *Brandon Burr's SurfOS* and *Version 1.0 on an x86, 2004-2026* in red
   and orange, and the shell's prompt, `SURF ≋>` (section 10).

The text fills the screen: 160x45 characters on the laptop's 1920x1080 panel, in Spleen
12x24 at its own size (since 10/25/26; 120x33 of the 8x16 font at twice its size before),
or 170x48 of 8x16 on a 1366x768 panel. To choose another font, add `font=8x16`, `font=12x24`
or `font=16x32` (120x33 on 1920x1080, the biggest letters) to the GRUB entry's `multiboot2`
line (press `e` in the menu); a font that 80x25 of its characters would not fit gives way to
8x16, and `bootinfo` says so.

The GRUB menu also has:

* **SurfOS (development build from the data stick)**, first and chosen after 5 seconds, but
  only when a second USB stick holds a development build (section 13); on one stick for
  everything, **SurfOS (development build from this stick)** (section 14).
* **SurfOS (troubleshooting: local APIC timer, polled keyboard)**: the same kernel with
  `timer=lapic kbd=poll`, for when the normal entry boots but the clock or the keyboard
  does not respond.
* **SurfOS (kernel shell)**: the kernel's own shell on every console instead of surfsh
  (section 10).
* **SurfOS (USB controller off: no USB network adapter, no USB sticks)**: section 8.
* **SurfOS (without the settings on the stick: no Wi-Fi joined, no telnetd)**: the build in
  `boot/` without reading the settings and the saved networks (section 15).
* **UEFI firmware setup**: restarts into the firmware's setup screens.
* **Restart** and **Power off**.

Nothing is written to the laptop. To go back to the normal operating system, take the
drive out and restart (`reboot` in SurfOS, or hold the power button). Turn Secure Boot
back on afterwards if you want it on.

## 7. What to try

Type at the prompt, `SURF ≋>`:

| Command | Expect |
|---|---|
| `help` | the commands, a screenful at a time: Space shows the next (section 10) |
| `bootinfo` | `multiboot2`, `profile=laptop`, `firmware UEFI`, the memory map, the framebuffer |
| `drivers` | which console, timer and keyboard paths were chosen |
| `cpuinfo` | the processor (an i3-1005G1 should say so) |
| `acpi` | the firmware's ACPI tables |
| `memstat`, `ps`, `tick`, `uptime` | memory, tasks, the clock moving |
| `lsblk`, `mount`, `ls /initrd` | the ramdisk rd0 mounted on `/initrd`, and the SurfOS drive as `usb0`, `boot drive: protected` |
| `cat /initrd/etc/version` | `SurfOS 1.0 (ai-dev)` |
| `cd /initrd/bin`, `ls`, `cd -` | the prompt shows where you are, `initrd ▸ bin ≋>`; the programs, surfsh among them |
| `run hello a b` | a ring-3 program, `exit status 42` |
| `count 3 10`, `spawn count 20 500` | user programs, one in the background |
| `heaptest`, `selftest` | `HEAPTEST PASS`, `SELFTEST PASS` (the disk tests are skipped, and the network tests without an adapter and a cable) |
| `lsusb` | the USB controller and what is plugged in: the SurfOS drive itself, the webcam, Bluetooth |
| `lspci -v`, `lspci 00:14.3` | every PCI device with its ids, subsystem, power state, interrupts and link; one in full, with its configuration space |
| `wifiinfo` | the Wi-Fi card: its ids, what it is, and its Bluetooth half |
| `wifi up`, `wifi scan` | the Wi-Fi card started; the networks in range (joining and saving one: section 15) |
| F1 to F4 | four consoles, each with its own shell |
| `edit /initrd/etc/version` | the text editor, full screen; Ctrl+Q leaves it (the ramdisk is read-only: files to keep go on a second stick, section 9) |

Please record the results, and photos of the screen, in
[hardware/ideapad-3-15iil05.md](hardware/ideapad-3-15iil05.md). The output of `cpuinfo`,
`acpi`, `drivers`, `bootinfo`, `lspci -v`, `lsusb` and `wifiinfo` answers most of its open
questions. A telnet session from the Mac (section 11) is the easiest way to copy them.

## 8. Networking: a USB Ethernet adapter

The IdeaPad 3 15IIL05 has no Ethernet socket (Lenovo's specification: "No onboard
Ethernet"), and its only network hardware is the Wi-Fi card (which SurfOS drives since
10/10/26: section 15). SurfOS reaches a wired network through a **USB-to-Ethernet adapter** in one of the
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
| `undertow https://example.com/` | read a page with verified HTTPS; `help` lists browser commands |
| `selftest` | `SELFTEST PASS`; on your network it checks the lease and that the gateway answers (the rest of its network checks need QEMU's network and are skipped) |

On a network without DHCP, set the address by hand:
`ifconfig eth0 192.168.1.50 255.255.255.0 192.168.1.1 192.168.1.1` (address, netmask,
gateway, DNS server).

When the adapter is unplugged, `eth0` stays (with `link down`). Plugged back in, it
is `eth0` again, and with the same adapter it keeps its address, so no new `dhcp` is
needed: SurfOS asks the network to confirm the address, and gives it up when the network
says it is not its own (section 15). A different adapter also becomes `eth0`, but needs
`dhcp` again.

The GRUB menu has an entry **SurfOS (USB controller off: no USB network adapter, no USB
sticks)**. It boots without touching the USB controller, in case the USB driver ever gets
in the way on the laptop.

## 9. Files on a USB stick

SurfOS forgets everything at a restart: the ramdisk is unpacked afresh at every boot, and
the flashed SurfOS drive is never written (it is mounted, read-only, only when asked, or
for the moment SurfOS reads its settings there at startup: section 15). Files that should last go on a **second USB stick**, or on the boot
stick itself when it is a FAT32 stick you copied the boot files onto (section 14), which
SurfOS writes but never partitions or formats. A second stick SurfOS can partition and
format as **FAT32**, the format macOS, Windows and Linux all read and write, and it has a text editor and the usual file commands, so the
same stick carries files between SurfOS and the Mac. Nothing stays mounted by itself (the
stick SurfOS was started from is mounted for a moment when its settings are read or saved,
section 15): you mount the stick, and you eject it before pulling it out.

**What to buy.** A plain USB-A flash drive from a known brand, 16 to 64 GB. Its size
hardly matters to SurfOS, and a USB 3 stick is faster in the laptop's USB 3 ports.
Avoid these:

* USB-C-only sticks: this laptop has USB-A ports.
* Hubs, docks and card-reader dongles: SurfOS has no hub support, so a stick behind
  one is not seen.
* Portable SSDs and USB hard drives: many use 4096-byte sectors, which SurfOS refuses
  (`+USB: port …: 4096-byte blocks are not supported (only 512)`).

Sticks of 32 GB or less are usually sold with FAT32 on them and work as they come.
Larger ones usually come with exFAT, which SurfOS cannot read. Either `mkfs` replaces
it with FAT32 (below; this erases the stick), or the Mac does: *Disk Utility* → *View* →
*Show All Devices* → the stick → *Erase*, format *MS-DOS (FAT)*, scheme *Master Boot
Record*.

**Find the stick.** Plug it in, before or after starting SurfOS (a line `+USB: usb1: …`
says it was found), and type `lsblk`. Under QEMU it looks like this; on the laptop the
models and sizes are those of your drives:

```text
SURF ≋> lsblk
NAME     SIZE BUS RM TYPE                MODEL/LABEL      STATUS
rd0        1M ram  0 disk                initrd.tar       mounted
usb0     256M usb  1 disk gpt            QEMU QEMU HARDDI boot drive: protected
  usb0p1 254M usb  1 part gpt EFI system SURFOSUSB        boot drive: protected
usb1      16M usb  1 disk mbr            QEMU QEMU HARDDI
  usb1p1  15M usb  1 part mbr FAT16
```

USB drives are named in the order SurfOS finds them (`usb0`, `usb1`, …) and their
partitions after them (`usb1p1`, `usb1p2`, …). The SurfOS drive is marked `boot drive:
protected` whatever its number. The examples below use `usb1`: check which name your
stick has.

**A stick that already holds FAT32 (or FAT16).** Mount it on a directory of your choice:

```text
SURF ≋> mount usb1p1 /data
    usb1p1 on /data: fat32, label SURFDATA, 1020 MB, 1019 MB free, read-write
```

`mount usb1p1` alone mounts it on `/usb1p1`, `mount -r usb1p1 /data` read-only, and
`mount` with nothing after it lists what is mounted where.

**A new file system, or a new partition table.** When `mount` answers `no file system
found (mkfs makes one)`, the stick holds exFAT, NTFS or nothing. `mkfs usb1p1 MYFILES`
replaces whatever is on the partition with a FAT32 file system labelled MYFILES (FAT16
when the partition is smaller than 512 MB; `mkfs -t fat16|fat32` chooses). A label has up
to 11 characters: letters (made upper case), digits, spaces (not first) and
``!#$%&'()-@^_`{}~``. To start from a blank stick, or to replace its
partitions, make a partition table first. On a 1 GB stick under QEMU (the kernel's own
`+PART:` and `+FAT32:` lines left out):

```text
SURF ≋> fdisk usb1 new mbr
    usb1: QEMU QEMU HARDDISK, 1.0G (2097152 blocks), USB, removable
    no partition table
    change: a new, empty MBR (disk signature 4fc8c3c8)
    Type usb1 to write the new partition table to usb1 (QEMU QEMU HARDDISK, 1.0G): usb1
    usb1: written, flushed and verified
    usb1 now has no partitions
SURF ≋> fdisk usb1 add
    usb1: QEMU QEMU HARDDISK, 1.0G (2097152 blocks), USB, removable
    MBR partition table, disk signature 4fc8c3c8
    change: add partition 1: blocks 2048-2097151 (0.9G), type 0c FAT32 LBA
    Type usb1 to write the new partition table to usb1 (QEMU QEMU HARDDISK, 1.0G): usb1
    usb1: written, flushed and verified
    usb1 now has usb1p1 (0.9G)
SURF ≋> mkfs usb1p1 SURFDATA
    usb1p1: QEMU QEMU HARDDISK, 1023 MB (2095104 blocks), a partition
    no file system found on it
    new: FAT32, 4096-byte clusters, 261120 of them, label SURFDATA
    Type usb1p1 to erase usb1p1 (1023 MB) and create a FAT32 file system: usb1p1
    usb1p1: FAT32, 4096-byte clusters: 261120 clusters, 2 FATs of 2044 sectors, 2056 reserved sectors, data at sector 6144 (1 MiB aligned), serial 4507-0D05
SURF ≋> mount usb1p1 /data
    usb1p1 on /data: fat32, label SURFDATA, 1020 MB, 1019 MB free, read-write
```

`fdisk` and `mkfs` say what they are about to change and write only after you type the
device's name; any other answer prints `Cancelled` and writes nothing. More of `fdisk`
(`fdisk` alone lists it):

| Command | What it does |
|---|---|
| `fdisk usb1` | the partition table, the free space, and whether SurfOS may change the stick |
| `fdisk usb1 new mbr` | a new, empty MBR table: what data sticks usually have, and what the Mac formats |
| `fdisk usb1 new gpt` | a new, empty GPT table instead |
| `fdisk usb1 add 2G` | a 2 GB partition (`500M`, `all`; without a size, the largest free space); a type may follow: `fat32` (the default), `fat16`, `linux`, `efi`, or an MBR type byte in hex |
| `fdisk usb1 delete 2` | delete partition 2 |
| `fdisk usb1 type 1 fat32` | change partition 1's type |

**Working with files.**

| Command | What it does |
|---|---|
| `ls /data`, `cat /data/notes.txt`, `stat /data/notes.txt` | list a directory, print a file, its size and time |
| `edit /data/notes.txt` | the text editor (below); the file is made when you first save |
| `cd /data`, `pwd` | change and show the current directory |
| `cp`, `mv`, `rm`, `mkdir`, `rmdir` | copy, rename or move, delete, make and remove directories |
| `write /data/todo.txt buy milk` | a file with one line of text |
| `df` | size, use and free space of everything mounted |
| `sync` | write the changes SurfOS still holds in memory to the drives (before pulling a stick out, `eject` it) |

Names with spaces go in quotes, and what a command prints can go into a file
(section 10):

```text
SURF ≋> echo "hello from SurfOS" > /data/hello.txt
SURF ≋> mkdir /data/Documents
SURF ≋> mv /data/hello.txt "/data/Documents/Hello World.txt"
SURF ≋> cd /data/Documents
data ▸ Documents ≋> ls -la
```

`ls`, `cat`, `cp`, `mv`, `rm`, `mkdir`, `rmdir`, `df` and `hexdump` are programs in
`/initrd/bin` with the usual options (`ls -la`, `cp -r`, `rm -rf`, `mkdir -p`), and they
read pipes and files (`cat < notes.txt`). The kernel has commands of the same names, which
`kernel ls` and so on run (section 10); `hexdump usb1 0` still shows the blocks of a drive.
The other programs: `touch`, `head [-n N]`, `wc [-lwc]`, `edit` and `sh` (an earlier,
smaller shell). They are written against SurfOS's small C library (`user/libsurf`), which
new programs can use too.

**The editor.** `edit <file>` opens a file, or starts a new one, on the whole screen: a
title line, the text, a status line and a line of keys.

| Keys | What they do |
|---|---|
| arrows, Home, End, PgUp, PgDn | move |
| Enter, Backspace, Delete, Tab | as usual (Tab puts in a tab character) |
| Ctrl+S | save |
| Ctrl+Q | quit; with unsaved changes it asks `Save changes? (y/n/^C)` |
| Ctrl+F | find; Enter alone finds the same text again |
| Ctrl+K, Ctrl+U | cut the line (several cuts in a row go together), paste them |
| Ctrl+G | go to a line number |
| Ctrl+C | say where the cursor is |
| Ctrl+L | draw the screen again |

It takes files up to 1 MB. Lines do not wrap: the line with the cursor scrolls sideways,
and a `>` in the last column says a line goes on. Saving writes a new copy and then
renames it over the old file, so a failed save leaves the old file as it was. The copy
is `<file>.tmp`, or `<file>.1.tmp` and so on when you have a file of that name, which
`edit` never overwrites. If the disk fails during the rename, the copy is kept with your
text in it, and the status line names it. The file and the copy may then share their
space on the disk, and deleting or saving over either can damage the other. For the rest
of that `edit`, each later version is saved as another copy beside the file (up to
`<file>.9.tmp`, then it says no name is left; none when nothing changed since the last),
the line of keys turns into a warning, and `edit` says where the last copy is when it
ends; Ctrl+Q ends it without asking when the text is all in that copy. It takes the same
care whenever it finds a `.tmp`, `.N.tmp` or `.part` beside the file of the same size and
time, when it opens the file and before each save: that is how such a pair usually
looks, though not always. Check the stick on another computer (First Aid on the Mac,
`fsck` elsewhere) before you change either file, also with `rm`, `mv` or `cp`, which take
no such care. Once it is checked, delete the copy you no longer want with `rm` (or on the
Mac), then start `edit` (or the desktop) again: while the two look alike, and for the rest
of a session that met them, `edit` and the desktop keep taking the care. Windows line
ends (CR LF) stay as they were.

**Taking the stick out.** Type `eject usb1` and wait for `usb1: safe to remove`. It
unmounts every partition of the stick and writes everything still held in memory. When
something still uses the stick, `eject` says what, for example `eject: usb1: /data is
busy: it holds the current directory, /data (cd / first)`. A stick pulled out without
`eject` may lose its last changes, and the next `mount` warns `… was not unmounted
cleanly`; let the Mac check it then (*Disk Utility* → *First Aid*). Since 10/24/26 (storage,
milestone 1) SurfOS writes in an order that leaves a pulled stick's files whole: a file
being saved holds the old text or the new one, never neither, and nothing names freed
space. It no longer clears that mark by itself either: it stays until the Mac (or
`fsck.fat`) has checked the stick. When the stick stops taking writes, `eject` writes
nothing off and says `eject: usb1 stays mounted: sync tries the writes again; umount gives
them up`; a save says it failed (the file keeps its old text), and `umount` says how many
changed blocks it had to give up.

**On the Mac.** The stick appears under its label, and the files SurfOS wrote are
ordinary files. macOS adds hidden files of its own (`.fseventsd`, `.Spotlight-V100`,
`.DS_Store`, and `._` files beside others); they do no harm, and `ls -a` in `sh` shows
them. Eject the stick in the Finder before pulling it out there too.

**What SurfOS will not change.** `fdisk` and `mkfs` work only on USB drives and
ramdisks, and refuse:

* the SurfOS boot drive, recognised by its volume label SURFOSUSB whatever its number. It
  can be mounted, but only read-only;
* the stick SurfOS was started from (GRUB names its volume, section 14), whatever its
  label. That volume can be mounted read-write, since it holds your files too; any other
  partition on that stick only read-only;
* internal disks. SurfOS has no driver for the laptop's SSD, so it cannot even see it;
  an internal disk it does see (an IDE disk under QEMU) is marked `internal: never
  formatted`;
* a drive that is mounted, or whose disk or partitions are: `umount` or `eject` first.

**Limits.**

* A file name with accents (`café.txt`) is stored and shown correctly, but letters beyond
  ASCII cannot be typed yet. Characters outside the console's set (section 10) show as `?`.
* FAT12, FAT16 and FAT32. exFAT and NTFS are not read.
* Files up to 4 GB, FAT32's limit, but a program can seek only within the first 2 GB.
  The editor takes files up to 1 MB.
* `fdisk add` makes primary partitions only (four at most in an MBR table). Logical
  partitions made elsewhere are read.
* `umount` and `eject` notice a current directory on the stick only in the shell where
  you type them. A shell on another console (F2 to F4) that is still in `/data` does not
  stop them; its commands then find nothing there until it changes directory.
* No USB hubs, so every drive needs a port of its own. The laptop's three ports take the
  SurfOS drive, a data stick and the Ethernet adapter; with one stick for both (section
  14), a port stays free.

## 10. The shell

Every console (F1 to F4) runs **surfsh**, the SurfOS shell. It is a program like the
others, `/initrd/bin/surfsh` (`ls /initrd/bin` lists it): `init` starts it on each
console at boot and starts it again when it ends. Typing `surfsh` starts a shell inside
the shell; `exit` (or Ctrl+D on an empty line) goes back.

**The prompt** shows where you are, as breadcrumbs, and the wave `≋`:

```text
SURF ≋>                         the root directory, /
initrd ▸ bin ≋>                 /initrd/bin
… ▸ 2026 ▸ October ≋>           a deep directory: the names on the left give way to …
data ▸ Documents ≋!>            the last command failed
SURF ≋@>                        a telnet session (section 11)
```

The `!` (in red) says the last command failed; it goes away after one that succeeds. In
a telnet session the `@` (magenta) is shown instead, whatever the last command did.
`cd` changes the directory (`cd` alone goes to `/`, `cd -` back to the previous one).
SurfOS has no users and no home directories, so a shell starts at `/`.

**Typing.**

| Keys | What they do |
|---|---|
| Left, Right, Home, End (Ctrl+A, Ctrl+E) | move in the line |
| Ctrl+Left, Ctrl+Right (Alt+B, Alt+F) | move by words |
| Backspace, Delete | delete before or under the cursor |
| Alt+Backspace, Alt+D | delete the word before the cursor, or the rest of the word after it |
| Ctrl+U, Ctrl+K, Ctrl+W | delete the whole line, to its end, back to the last blank |
| Up, Down | the lines typed before (`history` lists them; the last 1,000 are kept) |
| Ctrl+R | search them: type part of a line and the newest one that has it shows; Ctrl+R again goes further back, Enter runs it, Esc puts back what you had |
| Alt+. | put in the last word of the line before (press again for the line before that) |
| Tab | complete the command or file name; a second Tab lists the choices |
| Ctrl+L | clear the screen |
| Ctrl+C | drop the line; while a command runs, stop it |
| Ctrl+D | on an empty line: leave this shell |

**Lines from the history** (as in bash): `!!` is the line before (`!! | head` runs it again
into `head`), `!$` its last word (`mkdir /data/new` then `cd !$`), `!12` line 12 of
`history`, `!-2` the line before the last, and `!ls` the last line that started with `ls`.
The line is shown as it runs; a `!` that nothing follows, `!=`, and anything in quotes stay
as typed. A line that starts with a space is not kept in the history.

Tab completes the first word from the shell's commands, the kernel's and the programs, and
any other word (or one with a `/` in it) as a file or directory name: one match is filled
in, several as far as they agree, and a second Tab lists them (asking first when there
are more than the screen holds).

**Long output** stops when the screen is full, with `--More--` on the last row: Space
shows the next screenful, Enter one more line, and q (or Esc, or Ctrl+C) leaves the rest
unshown. `help` is long on purpose.

**Scrolling back.** Shift+PgUp shows what scrolled off the top of the screen, a screenful
at a time (each console keeps about 1,600 lines; `clear` and Ctrl+L keep the screen there
too); Shift+PgDn goes forward again, Shift+Home to the oldest line, Shift+End back to the
present. The top right corner says how many lines back you are (` -32 `). Anything printed
meanwhile waits below; any other key brings the live screen back and does what it does.
It works at `--More--` too, without answering it. (Not in the desktop, where the keys are
the desktop's, nor on a VGA text screen.)

**Which command runs.** First the shell's own commands (`cd`, `pwd`, `echo`, `exit`,
`help`, `history`, `set`, `type`, ...), then a program in the directories of `PATH`
(`/initrd/bin`), then the kernel's (everything in sections 7 to 9: `lsblk`, `fdisk`,
`mount`, `ifconfig`, `ps`, ...); a name with a `/` in it is a program's path. Since
10/25/26 the programs `ls`, `cat`, `cp`, `mv`, `rm`, `mkdir`, `rmdir`, `df` and `hexdump`
come before the kernel's commands of the same names: `kernel ls` runs the kernel's, and
`type ls` says which one runs and what it hides (`help` marks the kernel's with `*`).
`help` lists all three groups, `help <name>` one command.

**A line** is words separated by blanks. `"double"` or `'single'` quotes keep blanks
inside a word, and `\` keeps the next character as it is. `cmd > file` sends what the
command prints into the file (`>>` adds to its end; `2> file` sends its error messages
there, and `2>&1` where its output goes), `cmd < file` makes it read the file, `cmd &`
runs it in the background as a job (`[1] started (pid 42)`; its end, `[1] done  cmd`, is
reported before the next prompt), and `;` separates commands. The kernel's
commands can be redirected too, which is a quick way to keep what they say:
`lspci > /data/lspci.txt`. A question such as fdisk's still appears on the screen.

**Pipelines.** `a | b` sends what `a` prints to `b`, which reads it: `dmesg | head -20`,
`ls /initrd/bin | wc -l`, `lspci | head -3`, `help | head`, `dmesg | grep -i usb`. The
programs made for pipelines: `grep` (lines that match a pattern: `.`, `*`, `^`, `$`,
`[a-z]`; `-i` any case, `-v` those that do not, `-c` how many, `-n` with their numbers),
`sort` (`-n` by number, `-r` reversed, `-u` once each), `uniq` (`-c` with counts), `head`,
`tail` (`-n 5`), `wc`, `tee FILE` (a copy into a file), `tr a-z A-Z`, `seq 1 10`. A kernel command can begin a
pipeline but not follow a `|`, since it reads only the console; its output arrives when it
is done. A program that writes to a pipe whose reader has ended ends too (`hexdump
/dev/zero | head` stops). `a && b` runs `b` when `a` succeeded, `a || b` when it failed,
and `! a` turns the status round. `{ a; b; } > file` sends both into the file, and
`( cd /initrd; ls )` runs in a shell of its own, so its `cd` changes nothing here.
`cmd << END` makes `cmd` read the lines that follow, up to a line `END` (`$?` is filled in
there; with `<< 'END'` nothing is). A line that ends with `|`, `&&` or `||`, or inside
quotes or a `{ }`, goes on in the next line, after the prompt `… ≋>`; Ctrl+C there drops
the whole command.

**Variables and wildcards.** `name=value` sets a variable, and `$name` (or `${name}`) is
its value: `dir=/initrd/bin; ls $dir`. `export name` gives it to the programs started
afterwards, in their environment (`env` lists what a program gets), and `name=value cmd`
to that one command alone; `unset name` removes it, `readonly name` keeps it as it is.
`$(cmd)` is what `cmd` prints (`n=$(ls /initrd/bin | wc -l)`), `$((n * 2))` arithmetic,
`${name:-default}` a value for an empty variable, `${#name}` its length, `${f%.txt}` and
`${f##*/}` the value without its end or its beginning. A value with blanks in it becomes
several words unless it is in double quotes: `"$name"` is always one. In a word, `*`
matches any characters of a file's name, `?` one, `[ab]` one of those: `ls
/initrd/etc/*.conf`, `rm /data/*.tmp`; a word that matches nothing stays as it is, and in
quotes (`"*"`) a star is just a star. `$?` is the status of the last command, `$!` the pid
of the last job, `$$` the shell's own. `PATH` says where programs are looked for.

**Scripts.** A script is a file of commands: `surfsh /data/backup.sh` runs it, and one whose
first line is `#!/initrd/bin/surfsh` runs by its name too (`/data/backup.sh`; any file that is
not a program runs as a surfsh script). In it, and at the prompt, `$1`, `$2` ... and `"$@"` are
its arguments, and these work as in other shells:

```sh
if [ -f /data/notes.txt ]; then echo there; else echo missing; fi
for f in /data/*.txt; do echo "$f"; done
n=0; while [ $n -lt 3 ]; do n=$((n + 1)); done
case $1 in -h|--help) echo "usage: ..."; exit 2;; esac
copy() { local from=$1; cp "$from" /data/backup/; }
lsblk | while read name rest; do echo "device $name"; done
printf '%-10s %5d\n' files 12
```

`test` (or `[`) checks files (`-e -f -d -s`), strings (`= != -z -n`) and numbers (`-eq -lt
-gt` ...); `read` takes a line of input into variables, `printf` formats, `eval`, `.` (runs a
file's commands in this shell), `break`, `continue`, `return`, `local`, `exec`, `true`, `false`
and `:` are there too. `set -e` ends a script at the first command that fails, `set -x` shows
each command before it runs, `set -u` makes an unset variable an error. `exit` without a
number leaves with the status of the command before it (`cd /data || exit`). Descriptors 0
to 9 are yours in redirections (`exec 3< /data/list.txt`, then `read line <&3`); a program
gets 0 to 3 of them. `kernel test` runs the
kernel's command of a name that surfsh has too. Ctrl+C stops a loop or a script as a whole,
not just the command that was running: the prompt comes back with `$?` 130.

**Jobs.** Ctrl+Z stops the program that runs (`[1] stopped  count 100 100`) and gives the
prompt back; `jobs` lists the jobs, `fg` brings the last one (or `fg %2`) back to the screen,
`bg` lets it go on in the background, `kill %1` ends it, `wait` waits for the jobs in the
background (in a script, `wait $p` waits for the job whose pid `$!` gave, even when it has
ended already). A program in the background that wants to read the keyboard stops until `fg`
gives it the keys (`[1] stopped (reading the terminal)`). Ctrl+D on an empty line ends what a
program reads from the keyboard: `cat > /data/notes.txt`, a few lines, then Ctrl+D.

**Statuses.** 0 is success, 1 a failure, 2 a command used the wrong way, 126 a program
that could not be started, 127 no such command, 130 a command stopped with Ctrl+C (a
program, a kernel command, a loop), 137 a program that was killed (by `kill` or by a
fault), 148 a program stopped with Ctrl+Z. A
program that ends with another status says `[exit status N]`.

**Settings.** `set` lists them, `set <key> <value>` (or `<key>=<value>`) changes one for
this shell, and
`/initrd/etc/surfsh.conf` has the values every shell starts with:

| Key | Values | |
|---|---|---|
| `prompt.unicode` | `on`, `off` | `off` draws the prompt in ASCII: `initrd > bin ~>` |
| `prompt.color` | `on`, `off` | the colours (light blue prompt, teal typing, red `!`) |
| `prompt.max_depth` | 2 to 16 (5) | how many names the prompt shows at most |

**The characters on the screen.** The console draws accented letters, box drawing, some
Greek and symbols, and the prompt's `▸ ≋ …` (the characters of code page 437); anything
else shows as `?`. Letters beyond ASCII cannot be typed yet.

**When the shell cannot run.** If surfsh is missing or cannot be loaded, or ends
abnormally three times within 30 seconds (killed, or ended within 2 seconds of starting),
`init` gives that console the kernel's own shell as a **recovery console**: its
prompt is `SURF ≋?>`, and a line above it says why. Every kernel command works there, and
`surfsh` starts the shell by hand. The GRUB entry **SurfOS (kernel shell)** puts the
kernel's shell on every console on purpose; its prompt is `SURF ≋#>`, because it runs with
the kernel's privileges.

## 11. Telnet: the shell from another computer

`telnetd` is a telnet server. While it runs, another computer on the same network (the
Mac) can `telnet` to the laptop and get a SurfOS shell of its own, as on F1: the banner,
the prompt, every command.

> **There is no password and no encryption.** Anyone who can reach the laptop over the
> network gets a shell with every kernel command (`fdisk`, `mkfs` and `reboot`
> included), and everything typed crosses the network as plain text. Use it on your own
> network only, and turn it off when you are done. It starts by itself only with the
> setting `telnetd = on` (section 15) or in a development build (section 13), and then
> only once SurfOS has an address on Ethernet or on a WPA2 Wi-Fi network, never because
> of an open one. Once it runs it answers on **every** network the laptop is on, one you
> join by hand later included.

**Turn it on**, on one of the laptop's own consoles (F1 to F4):

1. Get the laptop onto the network: the USB Ethernet adapter, the cable and `dhcp`
   (section 8), or Wi-Fi (section 15).
2. `telnetd on` starts the server in the background and says where to connect:
   `telnetd: listening on 192.168.1.50 port 23`. The console is free again at once.
   `telnetd on 2323` uses another port. Without an address yet it says
   `... but eth0 has no address yet: run dhcp first`; the server listens all the same, so
   `dhcp` is enough.

**Connect from the Mac.** macOS has had no `telnet` command since 10.13 (High Sierra);
install it once with [Homebrew](https://brew.sh): `brew install telnet`. Then

```sh
telnet 192.168.1.50          # the address telnetd on printed
telnet 192.168.1.50 2323     # when it was started on another port
```

The banner appears, then the prompt `SURF ≋@>`: the `@` says this shell is a telnet
session (`help` explains it). From Linux use `telnet` (package `telnet` or
`inetutils-telnet`); from Windows, PuTTY with the connection type *Telnet*.

**In a session** surfsh works as on the laptop: editing the line, history, Tab, `help`
with `--More--` (sized to your telnet window), colours, and Ctrl+C to stop a command.
Characters beyond ASCII can be typed here (`echo café`), which the laptop's own keyboard
cannot do yet. A few differences:

* **Leaving:** `exit` (or Ctrl+D on an empty line) ends the session and closes the
  connection. Closing the telnet window, or Ctrl+] then `quit` in the Mac's telnet, ends
  it from the other side: the session's shell is stopped. A kernel command that is still
  running stops where it can stop cleanly, or finishes, before that: it is never killed
  in the middle of writing to a disk.
* **Pasting:** surfsh takes one command at a time, so of a pasted block of several lines
  only the first line runs and the rest of the block is dropped: paste one command at a
  time. A paste into a program that reads what is typed, such as `edit`, arrives whole.
  Keys typed while a command runs are given to that command; Ctrl+C stops the command and
  drops what was typed ahead, so it never runs afterwards.
* **Alt keys** (Alt+., Alt+B, Alt+D, ...) reach surfsh when the terminal sends the Option
  key as Alt, an ESC before the key: in the Mac's Terminal, Settings, Profiles, Keyboard,
  "Use Option as Meta key". Ctrl+R and the `!` references need nothing.
* **Messages on the laptop's screen:** a background task that a command starts (such as
  `tcpecho`) prints on the laptop's F1 console, not in the session, and keeps running after
  the session ends.
* **Up to 4 sessions** at once; a fifth connection is told
  `4 sessions are open already; try again later.` and closed.
* A session whose telnet program has taken no output for 60 seconds while output waits
  for it (one suspended with Ctrl+] and `z`, or a computer gone to sleep) keeps its place
  until that place is needed: when all 4 are in use, a new connection ends the session
  stalled longest instead of being refused.

**Check and turn off**, on the laptop or in a session:

| Command | What it says |
|---|---|
| `telnetd` (or `telnetd status`) | `telnetd: running (pid 12), 1 session shell:` and a line per session (`pty0 surfsh (pid 13)`), or `telnetd: not running` |
| `telnetd off` | `telnetd: stopped (1 session shell ended)`: the server is gone and every session has ended. Typed inside a session, it ends that session too |
| `dmesg` | who connected and when each session ended, and why: `telnetd: session 1 from 192.168.1.20:51234 (pty0, surfsh pid 13)` |

After a reboot `telnetd on` has to be typed again, unless the setting `telnetd = on`
starts it (section 15) or a development build does (section 13).

## 12. When something goes wrong

| What you see | What it means, what to do |
|---|---|
| The USB drive is not in the F12 menu | Write the image again with one of the writers above; try the other USB ports; check that Secure Boot is off and that USB boot is enabled. |
| "Secure Boot Violation", "Verification failed", or a red security warning | Secure Boot is still on (step 5). |
| GRUB says `file '/boot/…' not found`, or "SurfOS: /boot/surfos.bin … were not found" | The drive does not hold the image as written by a raw writer: write it again (step 4). On a stick you copied the files onto, one is missing: `tools/stick.py check` says which, `tools/stick.py install` puts them back (section 14). |
| A stick you copied the boot files onto is not offered at all | Check that `EFI/BOOT/BOOTX64.EFI` is on it (`tools/stick.py check`) and that it is FAT32, then choose it in the F12 menu; plug it straight into the laptop (section 14). |
| On that stick, `lsblk` says `boot drive: read-only` | Its write-protect switch is on: slide it back and plug the stick in again. |
| On that stick, `lsblk` shows none of `boot drive: writable`, `mounted (boot drive)` or `boot drive: read-only` | GRUB did not name the volume (the stick has an older boot loader or menu), or the build running is older than 10/17/26 (`tools/stick.py check` marks such builds). `tools/stick.py install` brings the boot loader and the menu up to date and replaces such a fallback build in `boot/`; `tools/stick.py update` puts a new development build in. Until then `fdisk` and `mkfs` do not know it is the boot drive: leave them alone. |
| The menu has no "development build from this stick" entry, though `surfos-dev` holds a build | `boot/ONE-STICK.txt` is missing (GRUB looks in its own stick's `surfos-dev` only beside it): `tools/stick.py install` puts it back. |
| GRUB's menu, then a blank screen | The kernel could not use the firmware's framebuffer. Photograph the GRUB screen and note the panel resolution; report it. |
| The boot log stops, or ends with `SYSTEM HALTED` or a line starting `BOOT PROBLEM` | Photograph the screen and report it: those lines name what failed. |
| The prompt appears but keys do nothing | Restart and choose the troubleshooting entry. USB keyboards are not supported (the USB stack drives network adapters and storage only), so use the built-in keyboard. |
| The boot stops at a line starting `+xHCI` or `+USB` | Restart and choose "SurfOS (USB controller off)". Please report the last lines on the screen. |
| `lsusb` does not list the adapter | Try another port, and plug the adapter straight into the laptop (not through a hub or a dock). A line `(connected, not set up)` or `+USB: port …: … failed` means the adapter did not answer: unplug it, wait two seconds, plug it in again. |
| `lsusb` lists the adapter with `no driver` | It speaks only its maker's protocol: use an RTL8153 or another CDC ECM/NCM adapter. |
| `ifconfig` says `link down` | Check the cable and that the other end (router, switch) is on: the adapter reports the link it sees. |
| `dhcp: eth0: no offer` | Nothing on that network hands out addresses: set one by hand (section 8). |
| `ping` by address works, by name does not | No DNS server is set: `dhcp` sets one, or give it as the last `ifconfig` value. |
| `lsblk` does not show a USB stick | Plug it straight into the laptop, not through a hub, and try another port. `dmesg` shows the `+USB:` lines: `4096-byte blocks are not supported` means a portable SSD or hard drive (section 9). |
| `mount: usb1p1: no file system found (mkfs makes one)` | The stick holds exFAT, NTFS or nothing: `mkfs` it (this erases it), or format it as MS-DOS (FAT) on the Mac. |
| `fdisk` or `mkfs` says it will not change a drive | The line after it says why: the boot drive, not a USB drive, or something on it is mounted (`umount` or `eject` first). |
| `… is busy: it holds the current directory` | `cd /`, then `umount` or `eject` again. |
| `+FAT32: usb1p1 was not unmounted cleanly` | The stick was pulled out without `eject` (or the laptop restarted with it mounted). If files look wrong, let the Mac check it (*Disk Utility* → *First Aid*); since 10/24/26 the warning comes back at every mount until it has been checked. |
| `bcache: write of usb1p1 blocks … failed (-3): kept for the next sync` | The stick refused a write. Nothing is lost yet: `sync` tries again. If it keeps failing, copy what matters off the stick; `eject` keeps it mounted until the writes work, `umount` gives them up. |
| A file name shows a `?` | It has a character the console cannot draw (section 10); the file itself is fine. |
| `--More--` on the last row | The output is longer than the screen: Space shows the next screenful, Enter one more line, q the rest is not shown. |
| The prompt ends in `≋!>` | The last command failed and said why; the mark goes away after one that succeeds. |
| `telnet: command not found` on the Mac | `brew install telnet` (section 11). |
| `telnet: connect to address …: Connection refused` | The server is not running (`telnetd on` on the laptop), or it was started on another port (`telnetd status` says which). |
| `telnet` says `Trying 192.168.…` and nothing more | That is not the laptop's address (`ifconfig` on the laptop shows it), or the Mac is on another network (a guest Wi-Fi, a VPN) that cannot reach the laptop. |
| `telnetd: port 23 is in use` | Something else listens there: `telnetd on 2323`, then `telnet <address> 2323`. |
| `4 sessions are open already; try again later.` | Four telnet sessions are open: `exit` one of them, or `telnetd off` and `telnetd on` (which ends them all). |
| The prompt is `SURF ≋?>`, with a line about a *recovery console* above it | surfsh could not run on that console (missing, not loadable, or it ended abnormally three times within 30 seconds), so the kernel's own shell took over. Every command still works and `surfsh` starts the shell by hand; please report the line. |
| `tick` does not change between runs | Choose the troubleshooting entry (it uses the local APIC timer). |
| Windows asks for a BitLocker recovery key | Enter the key you saved (step 5). It happens once after Secure Boot settings change. |
| The menu has no development entry, though the data stick holds a build | GRUB looks for a folder `surfos-dev` holding `surfos.bin` and `initrd.tar` at the top of the stick's first partition (FAT). Plug the stick straight into the laptop, or into another port: the firmware may not have shown it to GRUB (section 13). |
| The development build does not start, or misbehaves | Restart (hold the power button if need be) and choose **SurfOS**, the second entry, within the 5 seconds: the build on the boot stick, which nothing changes (section 13). |
| `+AUTONET: no network link within 90 s` | No Ethernet link came: the adapter or its cable is not plugged in, or the router is off. (The development entry starts telnetd for Ethernet only; over Wi-Fi only the setting `telnetd = on` starts it, section 15.) Type `dhcp` and `telnetd on` once the cable is in. |
| `+AUTONET: eth0: no DHCP lease after 3 tries` | The adapter's link came, but the router's DHCP did not answer: `dhcp`, then `telnetd on`. |
| A saved Wi-Fi network is not joined at startup | `wifi scan` (after `wifi up`, if the card is down), then `wifi saved`, says whether it is in range; `settings` whether `wifi` is on and where the settings came from; `dmesg` has the `+NET:` lines (the later tries are written only there). Networks saved on the one stick are not on the flashed stick: started from the SURFOSUSB stick, the first line of `settings` ends with `(the flashed SURFOSUSB stick: only read)`; with an older menu it says `none: SurfOS was not started from a stick GRUB named`. The **SurfOS** entries need a build from 10/17/26 on in `boot/` (section 15). An open network is not joined by itself while a telnet server runs or is to start (`... is open and a telnet server runs ...`): `wifi join` it. A changed password (`... refused its saved key 3 times in a row ...`): `wifi join` it, then `wifi save`. A hidden network needs `hidden = yes` in the file (section 15). |
| `+NET: networks.conf line 7: ...` or `+NET: settings.conf line 3: ...` at startup | That line was not understood (the message says why). In `settings.conf` it is skipped and stays in the file. In `networks.conf` the whole network it belongs to is left out (the message names it) and kept in the file as it was, after the others under a comment, until you correct it on the Mac: a save from SurfOS keeps it, it does not fix it. Two exceptions: a key SurfOS does not know, or a `hidden` value it cannot read, is only said: the network is used without it, and the next save drops that line. |
| `wifi save` or `wifi forget` says `... a password in a part SurfOS does not understand ...` | A network SurfOS left out has a password in it. SurfOS would have to keep that password in the file, and it never writes one: correct that network (the line is named) or delete it on the Mac, then save again. |
| `... it would bring back a network written by hand that SurfOS had left out (a name written twice, or a 17th network?)` | `networks.conf` names a network twice (or has more than 16); SurfOS used the first and kept the second (or the 17th) as it was, which forgetting one would make a network. Delete one of the two (or the extra ones) on the Mac. |
| `... in force until SurfOS restarts, but not saved: ...` | The setting changed for now, but the stick could not be written (the reason follows): a flashed SURFOSUSB stick, a write-protect switch, the stick mounted read-only. For `telnetd` and `telnetd.port`, which act at the next start, and for any setting under the entry without the settings, it says `nothing changes` instead. |
| Telnet is on at startup and should not be | Start with **SurfOS (without the settings on the stick)** in the GRUB menu, then `settings telnetd off` (saved for the next start). The development entry starts telnetd for an Ethernet link whatever the settings say: on a network you do not trust, choose the entry without the settings (renaming `surfos-dev` on the Mac takes the development entry away). |

`reboot` restarts the laptop. 10/25/26 (robustness, B4): it ends the programs and writes
out and unmounts what is mounted read-write first, so the sticks stay marked cleanly
unmounted (`reboot -f` restarts at once, as before), and `poweroff` does the same and then
switches the laptop off through ACPI (S5). `poweroff -n` says what it would do: the
programs it would end, the volumes it would unmount, and the values it would write. If the
laptop stays on, SurfOS says "It is now safe to turn off your computer.": hold the power
button then (and tell us, `docs/hardware/ideapad-3-15iil05.md`). Ctrl+Alt+Del restarts the
same way; pressed again it restarts at once.

## 13. Development builds over the network

Claude, working on the Mac, can put a new build of SurfOS onto the laptop and start it
without anyone at the laptop: over the network it copies the build onto a second USB
stick, the **data stick**, and restarts the laptop, whose GRUB menu then starts that
build. The boot stick itself never changes.

**Keep plugged in**, straight into the laptop's three USB ports (no hub): the SurfOS boot
stick, the data stick, and the USB Ethernet adapter with its cable (section 8). The data
stick is any USB stick whose first partition is FAT32. The 30 GB *USB Disk 2.0* (`lsblk`
calls it *VendorCo Product*) is FAT32 already: it is ready as it is. Another stick can be
made FAT32 on the Mac: *Disk Utility* → *View* → *Show All Devices* → the stick → *Erase*,
format *MS-DOS (FAT)*, scheme *Master Boot Record* (this erases it). Nothing here ever
formats the data stick: it is only mounted, written in its `surfos-dev` folder and
ejected. SurfOS may call the two sticks `usb0` and `usb1` in either order from one boot
to the next; the data stick is found by what it holds, not by its name.

**In the firmware setup (once):** besides Secure Boot off (step 5), set the boot order
so that **USB comes first**, before Windows Boot Manager, so that a restart comes back
to SurfOS by itself. If the laptop does not start SurfOS with both sticks plugged in,
swap their ports, or move the boot stick to the top of the USB entries.

**The first time**, with this image written to the boot stick (step 4), the data stick
holds no build yet and SurfOS starts as usual. Type `dhcp` and `telnetd on` (section 11)
and Claude does the rest. From then on:

* The GRUB menu starts with **SurfOS (development build from the data stick)** and
  chooses it after 5 seconds. That build brings up the network and the telnet server by
  itself: the screen shows `+AUTONET:` lines and `telnetd: listening on … port 23`.
  Everything said about telnet in section 11 holds: no password, so keep the data stick
  plugged in only on your own network. It starts telnetd for the Ethernet adapter only. With
  saved Wi-Fi networks (section 15) the build also joins the first one in range (no open one
  while telnetd is to start), and telnetd, once running, answers there too.
* **If a development build misbehaves**, restart (hold the power button if need be) and
  choose **SurfOS**, the second entry, with the arrow keys before the 5 seconds are up:
  the build on the boot stick, which nothing changes.
* **To stop using development builds**, take the data stick out (or delete its
  `surfos-dev` folder on the Mac). The menu is then exactly as before.

On the data stick there is one folder, `surfos-dev`, with `surfos.bin` and `initrd.tar`;
the rest of the stick is yours. `cat /initrd/etc/build-id` says which build runs: the
source revision (`-dirty` when it had changes not yet committed) and when it was built.

How it works, for Claude (`tools/devloop.py`, from the `06-restoration` folder; never
with `reboot`, `poweroff`, `crashstack`, `mkfs` or `fdisk`'s changes, which it refuses):

```sh
tools/devloop.py find                 # the laptop's address: 10.0.0.4, else it searches 10.0.0.0/24
tools/devloop.py run lsblk 'uptime'   # commands in a telnet session, with their output
tools/devloop.py log                  # dmesg
devenv/build-usb.sh                   # a build (O=0 for the debug build) ...
tools/devloop.py deploy               # ... onto the data stick: rx, eject, reboot, the build id checked
tools/devloop.py reboot               # a restart without a new build
```

`deploy` finds the data stick with `lsblk` (the USB disk that is not the boot drive and
whose first partition is FAT), mounts that partition, and pushes the newest build's `surfos.bin` and `initrd.tar`
with `rx`, a program that receives one file over the network and says its CRC-32, which
`deploy` compares with the file it sent. Both files are renamed into place only once both
have arrived whole. Then it ejects the stick, restarts the laptop, waits up to three
minutes for telnet, and checks that `/initrd/etc/build-id` is the pushed build's. A stick
without a FAT file system stops it with what to do.

With one stick for everything (section 14), the data stick *is* the boot stick: `deploy`
writes into the `surfos-dev` folder of the stick SurfOS was started from, and GRUB's first
entry is then called **SurfOS (development build from this stick)**.

## 14. One stick for everything

*Added 10/17/26.* The short version, step by step, is [ONE-STICK.md](ONE-STICK.md).

The boot stick and the data stick can be one and the same: any FAT32
USB stick, the 30 GB data stick for one, with SurfOS's boot files copied onto it as
ordinary files. Nothing is flashed or formatted, and the files already on the stick stay
where they are. SurfOS then starts from that stick and keeps your files on it, and each
new build is two files copied into its `surfos-dev` folder, from the Mac or over the
network. The flashed image of step 4 still works as before, and stays your rescue stick.

**What goes on the stick.** `devenv/build-usb.sh` writes the folder `build/usb/stick/`
next to the image, holding the image's files and one more:

```text
EFI/BOOT/BOOTX64.EFI          GRUB, the boot loader (the path firmware tries on removable drives)
boot/grub/grub.cfg            the menu
boot/grub/x86_64-efi/fs.lst   empty (section 13)
boot/surfos.bin               the fallback build, which the menu's "SurfOS" entry starts
boot/initrd.tar
boot/BUILD-INFO.txt           what that build is, from what, when
boot/ONE-STICK.txt            marks a stick made this way: keep it (below)
surfos-dev/surfos.bin         the development build, which the menu's first entry starts
surfos-dev/initrd.tar
```

Everything else on the stick is yours; SurfOS's files never go anywhere else. GRUB looks
for a development build in the `surfos-dev` folder of its own stick only when
`boot/ONE-STICK.txt` is there, which the flashed image never has.

**The first time, on the Mac.** Plug the stick into the Mac. Its name is the one Finder
shows (here `DATASTICK`):

```sh
devenv/build-usb.sh                                  # when build/usb/stick/ is not there yet
tools/stick.py check   /Volumes/DATASTICK            # what the stick holds now
tools/stick.py install /Volumes/DATASTICK --eject    # the files above onto it, then eject it
```

`install` checks each file of the folder against the build's manifest, writes it under a
temporary name, reads it back and only then renames it into place, the boot loader and then
the menu last: a stick pulled out halfway is never started with half a menu (a new stick
shows GRUB's message that its menu is missing, one being brought up to date starts its old
menu), and a build left half old, half new is noticed and written again by the next
`install`. It never formats, partitions or
deletes anything, and it refuses:

* anything that is not a FAT volume of an external drive;
* the flashed SURFOSUSB stick, which SurfOS only reads: keep it as it is, for rescue;
* a stick that holds another system's boot loader or GRUB menu where SurfOS's go (a
  Windows or Linux installer stick, say): it names those files, and writes nothing until
  you have moved them off the stick.

Without the tool, drag the folders `EFI`, `boot` and `surfos-dev` from `build/usb/stick/`
onto the stick in Finder, then eject it. Finder checks nothing, and its *Replace* replaces
whole folders: on a stick that already has an `EFI` or `boot` folder of its own, use the
tool, which looks at them first.

The stick must be FAT32 (or FAT16), with either partition table; the data stick already
is. An exFAT or NTFS stick has to be erased as *MS-DOS (FAT)* first (section 9), which
erases it. Do not name the stick SURFOSUSB: that name marks the flashed image. And never
copy the stick folder onto the flashed stick (its volume is called SURFOSUSB on the Mac):
GRUB would then start a `surfos-dev` there that SurfOS can never update.

**On the laptop.** Plug in that stick alone and start the laptop. With USB first in the
boot order (section 13) the firmware starts the stick's GRUB by itself. If it does not,
tap **F12** at power-on and choose the stick (its entry names its maker, such as
*USB Disk 2.0*). The menu is:

```text
SurfOS (development build from this stick)       the default, after 5 seconds
SurfOS                                           the fallback build in boot/
SurfOS (troubleshooting: local APIC timer, polled keyboard)
...
```

The first entry starts the build in `surfos-dev`, with the network and the telnet server
(section 13; no password, so on a network you do not trust choose **SurfOS (without the
settings on the stick)**, which starts neither and joins no network by itself). **SurfOS**
starts the build in `boot/`: choose it with the arrow keys when a development build
misbehaves. When that build has the network manager (from 10/17/26 on; `tools/stick.py
install --fallback` puts one there) it reads the settings (section 15), so it starts
telnetd too when they say `telnetd = on`.

GRUB tells SurfOS which volume it was started from, by the volume's serial number, so
SurfOS knows its own stick whatever it is called:

```text
NAME     SIZE BUS RM TYPE               MODEL/LABEL      STATUS
rd0        7M ram  0 disk               initrd.tar       mounted
usb0      29G usb  1 disk mbr           VendorCo Product boot drive: protected
  usb0p1  29G usb  1 part mbr FAT32 LBA                  boot drive: writable
```

`fdisk` and `mkfs` refuse the stick, as they refuse the flashed one. Behind a stick's
write-protect switch, if it has one, the volume says `boot drive: read-only` and
`tools/devloop.py` asks for the switch to be slid back. Its volume is mounted like any data
stick (section 9): `mount usb0p1 /data` says `the volume SurfOS was
started from: its files may be changed; fdisk and mkfs leave it alone`, `lsblk` then says
`mounted (boot drive)`, and `eject usb0` comes before pulling it out. Only builds from
10/17/26 on know their stick this way: an older build, in `surfos-dev` or in `boot/`, would
treat it as an ordinary data stick, which `fdisk` and `mkfs` may change. `tools/stick.py
check` marks such a build, `install` replaces an older fallback, and `update` and `deploy`
say so when the build they put there is older.

**Each new build** is two files that belong together: `surfos.bin`, the kernel, and
`initrd.tar`, the programs it runs (surfsh, the editor, the browser, the Wi-Fi firmware).
They go into `surfos-dev`, either way:

* **Over the network, with the stick left in the laptop:** `devenv/build-usb.sh &&
  tools/devloop.py deploy` (section 13). `deploy` finds the stick SurfOS was started
  from (`boot drive: writable`), writes the build into its `surfos-dev`, ejects it and
  restarts the laptop into that build. Nothing is plugged in or out. With the USB Ethernet
  adapter the laptop comes back by itself, and over Wi-Fi too once the network is saved
  (`wifi save`) and `telnetd = on` is set (section 15). It refuses `--stick` naming another
  stick (GRUB would not start a build there) and a stick without `boot/ONE-STICK.txt`.
* **On the Mac:** `devenv/build-usb.sh`, the stick into the Mac, `tools/stick.py update
  /Volumes/DATASTICK --eject` (only `surfos-dev` changes), the stick back into the laptop.
  Or drag `build/usb/stick/surfos-dev` onto the stick and choose *Replace*.

`tools/stick.py install` again brings the boot loader, the menu and `surfos-dev` up to date
and leaves a good fallback in `boot/` as it is (one from before 10/17/26, which would not
know the stick, is replaced). Once a development build has proved itself,
`tools/stick.py install /Volumes/DATASTICK --fallback` makes it the fallback too.

**Keep the flashed stick as a rescue stick.** Here SurfOS writes to the stick it boots
from. It never formats or repartitions it, but a bug in a development kernel, or a crash
while the stick is mounted and being written, could damage files on it, the boot files
among them. The stick then no longer starts SurfOS until `tools/stick.py install` puts the
files back, a minute on the Mac. The SURFOSUSB stick of step 4, which SurfOS only reads
and `tools/stick.py` never writes, still starts SurfOS meanwhile.

## 15. Wi-Fi, saved networks and the settings on the stick

*Added 10/17/26.* SurfOS drives the laptop's own Wi-Fi card, the Intel Wireless-AC 9560:
open and WPA2-Personal networks on 2.4 and 5 GHz (not networks that offer only WPA3). A
network you join can be **saved on the stick SurfOS starts from**, and SurfOS then joins it
by itself at every start and gets an address, with nobody typing. A **settings file** on
the same stick says what else to do at startup, such as starting the telnet server. Both
are plain text files the Mac can read and change.

This needs one stick for everything (section 14): the FAT32 stick you copied the boot files
onto is the one SurfOS knows as its own and may write. The flashed SURFOSUSB stick of step 4
is only read: settings put there on the Mac are used, but SurfOS saves nothing there. The
builds must be from 10/17/26 on, in both places: `tools/stick.py update` puts a new one into
`surfos-dev` (the development entry), and `tools/stick.py install /Volumes/DATASTICK
--fallback` also into `boot/`, which the **SurfOS** entries start, with the menu entry
without the settings. A stick set up before keeps its older build in `boot/`, which reads no
settings.

**Joining a network by hand:**

| Command | What it does |
|---|---|
| `wifi up` | starts the card (its firmware comes from the stick) |
| `wifi scan` | the networks in range, strongest first: name, channel, band, signal, security |
| `wifi join HouseOfBurr` | joins it; for WPA2 it asks `Password for HouseOfBurr:` and shows nothing of what you type |
| `dhcp wlan0` | an address on that network |
| `wifi status`, `wifi leave`, `wifi down` | what the card is doing; leave the network; turn the card off |

**Saving it.** While joined, type **`wifi save`**. SurfOS writes the network into
`surfos-config/networks.conf` on its stick, and from the next start on it brings the card
up, joins the network and asks for an address by itself. The screen shows `+NET:` lines:

```text
+NET: settings from usb0p1 (surfos-config/): telnetd off, wifi on, ethernet off, hostname surfos; 1 saved Wi-Fi network
+NET: wlan0: starting the Wi-Fi card to join a saved network
+NET: wlan0: HouseOfBurr is in range (-52 dBm): joining it
wlan0: link up: "HouseOfBurr" (42:75:c3:4b:21:09), channel 11, 2.4 GHz, -52 dBm, WPA2-Personal
+NET: wlan0: joined HouseOfBurr; asking for an address (DHCP)
dhcp: wlan0: lease 10.0.0.164 for 86400 s from 10.0.0.1
```

| Command | What it does |
|---|---|
| `wifi save` | saves the network wlan0 is joined to, first in the list |
| `wifi save <name>` | saves a network that is not joined now: it asks the password (nothing for an open network), checked the first time SurfOS joins it |
| `wifi saved` | the saved networks in the order they are tried, and which are in range or joined now |
| `wifi forget <name>` | SurfOS no longer joins it by itself (it stays joined now, if it is) |
| `wifi connect` | does now what a start does: the card up, the first saved network in range joined, its address |

Several saved networks are tried in their order, the one saved last first (16 at most).
When none is in range, SurfOS looks again every 30 seconds (every 2 minutes after 20 tries,
about 10 minutes), saying so once on the screen and the later tries only in the log
(`dmesg`). When
the network goes away while joined (the router restarts, the laptop is carried off), SurfOS
first joins it again by itself, five tries in about a minute, then looks for any saved
network. On a network whose DHCP server does not answer (yet), it asks for the address again
in 30 seconds, then less often, up to every 5 minutes.

What you do by hand wins: after `wifi leave` or `wifi down` SurfOS joins nothing by itself
until `wifi connect` (which then hands the looking back to it), the next start, or until a
network you joined by hand is lost for good; a network joined with `wifi join` is left as it
is. A saved key the network refuses three times in a row (its password changed, or it was
mistyped when saved) is set aside for 10 minutes, and `wifi saved` and the screen say so:
`wifi join` it with the right password, then `wifi save`.

**Open networks and telnet.** The telnet server has no password, so while one runs or is
to start by itself (`telnetd = on`, the development entry), SurfOS joins **no open network
by itself**, `wifi connect` included. It says so (`... is open and a telnet server runs or is
to start: not joined by itself`), and `wifi join <name>` joins it by hand (and says that the
server answers there too). Once telnetd runs, it answers on every network SurfOS is on: do
not join a network you do not trust while it runs; `telnetd off` first.

**Whose network?** SurfOS cannot tell: any Ethernet link and any WPA2 network count as
safe for telnetd. A café's, a hotel's or an office's WPA2 network, whose password others
know, is as open to all of them as an open network. So with `telnetd = on`, save only your
own networks (`wifi forget` the others, or `settings telnetd off` before you go out), and
while the development entry is the default (the one stick's, as long as `surfos-dev` holds a
build) do not plug the Ethernet adapter into other people's networks. Away from home, start
with the entry without the settings; renaming `surfos-dev` on the Mac takes the development
entry away.

**Hidden networks**, whose name is not broadcast: `wifi join <name>` finds one by asking for
it by name, and `wifi save` then saves it with `hidden = yes`, so SurfOS asks for it by name
at startup too. `wifi save <name>` saves a network that the last scan did not hear by its
name as hidden as well (it says so); if it was merely out of range, `wifi scan` near it, then
`wifi save <name>` again. A renamed network is a new one: `wifi forget` the old name, then join
and save the new one.

**What is saved.** The network's name, its security, whether it is hidden, and its **key**:
the 256-bit value WPA2 makes from the password and the network's name (what
`wpa_passphrase` prints on Linux). The password itself is not written anywhere, so it cannot
be read back from the stick, but the key is enough to join that network: **whoever has the
stick can join your Wi-Fi**, so keep it as safe as the password. While telnetd runs, whoever
has a session can read the files too. `wifi forget` removes a network from the file. A file
deleted in Finder goes to the stick's Trash until the Trash is emptied, and like any file
system FAT does not wipe the old bytes, so treat a stick that once held keys as one that
still may. (A password you type into the file on the Mac, below, stays readable there until
SurfOS next writes the file, which any `wifi save` or `wifi forget` does.)

**The settings.** `settings` lists them, `settings <key>` explains one, and
`settings <key> <value>` changes one and saves it on the stick at once: `settings telnetd
on` starts the telnet server at every start from then on, `settings telnetd off` no longer.

| Key | Values | Default | What it does |
|---|---|---|---|
| `telnetd` | `on`, `off` | `off` | `on` starts the telnet server (section 11) at every start, once there is an address on Ethernet or a WPA2 Wi-Fi network (never for an open one), and the screen says where to connect. It has **no password**: use it on your own networks only. Meanwhile SurfOS joins no open network by itself (above) |
| `telnetd.port` | 1 to 65535 | `23` | the telnet server's port |
| `wifi` | `on`, `off` | `on` | `on` joins a saved network at startup and when one comes back (above); `off` leaves the card alone |
| `ethernet` | `dhcp`, `off` | `off` | `dhcp` asks for an address whenever the USB Ethernet adapter's link comes up, also when it is plugged in later |
| `hostname` | letters, digits and `-` (not first or last), 1 to 32 | `surfos` | the name SurfOS gives in its DHCP requests: the router's list of devices shows it, which helps to find the laptop's address |

`telnetd` and `telnetd.port` act at the next start; `ethernet` and `hostname` at once;
`wifi off` at once, `wifi on` from when a network is next lost (`wifi connect` joins one
now). Whichever way an
address came (at startup, `dhcp` by hand, in every start of SurfOS), its lease is renewed
before it runs out; `ifconfig` shows how much of it is left, and `dhcp renew` renews it by
hand. A lease that cannot be renewed is given up when it ends: `dhcp` then gets a new one,
and so do `ethernet = dhcp` and, for a saved network it joined, SurfOS itself. When the USB
adapter's link comes back while `eth0` has a lease, SurfOS first asks the network to confirm
the address.

**The files** are at the top of the stick, beside `boot` and `surfos-dev`:

```text
surfos-config/settings.conf   the settings, "key = value", each explained in the file
surfos-config/networks.conf   the saved networks
surfos-config/surfshrc        10/25/26: your own commands, which surfsh runs at each prompt's start
surfos-config/history         10/25/26: what you typed, kept while settings history is on
```

`surfshrc` is a surfsh script of your own, made on the Mac (plain text, as below): every
surfsh with a prompt runs it before its first one (on F1 to F4, a telnet session, a surfsh
typed at a prompt), after `/initrd/etc/surfsh.conf`. `HOME=/data` there makes `~` stand for
`/data`; it may set variables, define functions and print a line. SurfOS only reads it, and
not under the menu's entry without the settings. `settings history on` makes surfsh keep the
last 1,000 lines typed at its prompts in `history` and recall them at its next start (Up,
Ctrl+R); it saves when surfsh ends, after every 25 lines and on `history -w`. The file is
plain text on the stick: a password typed on a command line would be in it (a line that
starts with a space is never kept). `settings history off` stops the saving; the file stays
until you delete it. At startup a surfsh waits for the stick's settings to be read (30 seconds
at most) before its first prompt; started without the settings, or not from a stick, it does not
wait at all.

On the Mac they open in TextEdit (*Open With* → *TextEdit*, as `.conf` files are not its
own): keep them plain text (*Format* → *Make Plain Text*), and before typing turn off *Edit*
→ *Substitutions* → *Smart Quotes* and *Smart Dashes*. SurfOS takes TextEdit's curly double
quotes for quotes, but a curly apostrophe or a long dash in a password or a name is another
character, which makes another key or names a network that is not there. A file made on the
Mac needs these exact names, with no `.txt` at the end. Easier still: `wifi join` and `wifi
save` on the laptop, which write the name exactly. In `settings.conf` a line like
`#telnetd = off` shows a setting's default: take away the `#` and change the value to set
it. SurfOS changes only the line of the setting it saves, so your comments stay. A network
can be added to `networks.conf` on the Mac with its password (a `"` in the password is
written `\"`, a `\` is written `\\`):

```text
network = "HouseOfBurr"
security = wpa2
password = "the password"
```

SurfOS makes the key from it at the next start, and the next time it writes the file (`wifi
save`, `wifi forget`) the key takes the password's place. A password with a `#` in it must be
in the double quotes. An open network is `security = open` with no key, a hidden one has
`hidden = yes`. SurfOS writes `networks.conf` anew each time, so comments added there are not
kept. A line SurfOS does not understand is shown at startup with its number; the network it
belongs to is left out (a network with no `security` line and a line SurfOS does not
understand is never taken for an open one) and kept in the file as it was, after the others
under a comment (lines before the first network stay before them), until you correct it on
the Mac. A key SurfOS does not know, or a `hidden` value it cannot read, in a network it can
use is only said: the next save drops that line. `wifi saved` says how many parts the file
keeps so. SurfOS refuses to change the file while such a part has a password in it, which it
would have to keep there, and while a change would make a part kept so a network (a name
written twice, the first forgotten): correct the file on the Mac first.

SurfOS mounts the stick only for the moment it reads or writes these files (at startup, and
when something is saved), and never leaves it mounted. If you have mounted it yourself
(`mount usb0p1 /data`), the files are written there, and for that moment `umount` and `eject`
say the mount is busy; if you mounted it read-only (`mount -r`), saving says so.

**If a setting gets in the way** (telnet on where you do not want it, say), choose **SurfOS
(without the settings on the stick: no Wi-Fi joined, no telnetd)** in the GRUB menu with the
arrow keys: it starts the build in `boot/` without reading the files, joins no network and
sends no host name. `settings` and `wifi save` still save on the stick then, for the next
start; nothing else changes. Or change the file on the Mac.

**The development build** (GRUB's first entry, section 13) reads the settings too and joins a
saved network. Its `autonet` asks for the USB adapter's address within 90 seconds and starts
telnetd on port 23 for it, whatever the settings say (meanwhile SurfOS joins no open network
by itself). For Wi-Fi it starts none: there, once `autonet` is done, the setting `telnetd = on`
starts one as in any start, for a WPA2 network. So with a saved WPA2 network and `telnetd =
on`, the laptop comes back over Wi-Fi after `tools/devloop.py deploy`, at the address the
router gives it (`tools/devloop.py find` looks for it), in about two minutes.

## 16. A web server: hosting a site

`httpd` is a web server. While it runs, a browser on any computer of the same network (the
Mac, a phone) opens the laptop's address and gets the pages of a folder, HTML, CSS,
pictures and scripts, as a website. It only sends files: nothing runs on the server,
nothing can be uploaded, and there are no logins. Like `telnetd` it never starts by itself.

**Try it**, on one of the laptop's own consoles (F1 to F4), with the network up (section 8,
or Wi-Fi):

1. `httpd on` serves the sample site that is part of SurfOS (`/initrd/www`) and says where:
   `httpd: serving /initrd/www at http://10.0.0.4/`. The console is free again at once.
   Without an address yet it says `... but eth0 has no address yet: run dhcp first`; the
   server listens all the same.
2. Open that address in a browser on the Mac.

**Your own site.** On the Mac, put the site in a folder of the data stick, say `www`, with
an `index.html` as its front page and whatever it uses beside it (`style.css`, pictures).
Then on SurfOS:

```
SURF ≋> mount usb1p1 /data
SURF ≋> httpd off
SURF ≋> httpd on /data/www
httpd: serving /data/www at http://10.0.0.4/
```

* **Several sites:** every folder inside is a site of its own: `/data/www/blog/` is at
  `http://10.0.0.4/blog/`. A folder without an `index.html` (or `index.htm`) is listed,
  folders first, so the sites can be found from the top.
* **Editing:** the files are read again at every request, so a page edited with `edit`,
  with Notepad in `gui`, or on the Mac shows at the next reload. The browser asks every
  time and is told "not modified" while the file stays the same.
* **What it sends:** pages (`.html`, `.htm`), style sheets, scripts (`.js`), `.json`,
  text, pictures (`.png`, `.jpg`, `.gif`, `.svg`, `.webp`, `.ico`), fonts (`.woff`,
  `.woff2`, `.ttf`, `.otf`) and PDFs, each with its type; anything else comes as a
  download.
* **Hidden files:** names starting with a dot are never listed or sent, such as the `._`
  and `.DS_Store` files the Mac leaves on a stick.
* **Another port:** `httpd on /data/www 8080` is at `http://10.0.0.4:8080/`. The folder and
  the port come in either order, and a folder without a leading `/` is taken from the
  current directory.

**Watch the requests.** `httpd serve [folder] [port]` runs the same server on this console
instead of in the background, and prints each request as it comes: the browser's address,
the method, the page, the status, and the bytes sent. Ctrl+C stops it.

```
SURF ≋> httpd serve
httpd: serving /initrd/www at http://10.0.0.4/
httpd: each request is printed here; Ctrl+C stops the server
10.0.0.12 GET / 200 2722
10.0.0.12 GET /style.css 200 2819
10.0.0.12 GET /wave.svg 200 434
10.0.0.12 GET /favicon.ico 404 567
```

**Check and turn off**, on the laptop or in a telnet session:

| Command | What it says |
|---|---|
| `httpd` (or `httpd status`) | `httpd: running (pid 14)`, or `httpd: not running` |
| `httpd off` | `httpd: stopped`: the background server is gone (`httpd serve` stops with Ctrl+C) |
| `dmesg` | `httpd: serving /data/www on port 80` when it started, and who stopped it |

Limits:

* One background server at a time; more sites are folders inside the folder it serves.
* Up to 6 connections at once (a browser opens up to 6). A connection that sends no request
  for 10 seconds is closed, or after a second when a new connection needs its place; when all
  6 are busy, a new one waits a moment.
* Started in a telnet session, the server belongs to that session and stops when it ends
  (`httpd on` says so); start it on the laptop's own console to keep it running.
* Only pages to read: no forms that post, no uploads, no server-side programs (PHP, CGI), no
  HTTPS. Clients on the network see everything in the folder, so serve only what may be
  seen.
* A stick ejected while it is served: its pages are "not found" until it is mounted again.

## 17. Files to and from other computers: FTP, nc and fetch

`ftpd` is a file transfer server. While it runs, the Mac (or any computer on the same
network) copies files from the laptop's sticks and onto them, with what it has already:
Finder, `curl` in Terminal, or an FTP program such as Cyberduck or FileZilla. `nc` works the
other way round as well: typed on SurfOS, it sends a file to another computer's `nc`, or takes
one from it.

> **There is no password and no encryption.** Anyone who can reach the laptop over the
> network can read, change and delete the files `ftpd` serves, and everything crosses the
> network as plain text. Use it on your own network only, and turn it off when you are done.
> It never starts by itself, and while it runs SurfOS joins no open Wi-Fi network by itself
> (one anyone nearby may join); `ftpd on -r` serves files read-only.

**Turn it on**, on one of the laptop's own consoles (F1 to F4), with the network up (section
8, or Wi-Fi in section 15):

1. Mount the stick whose files you want to reach: `mount usb1p1 /data` (section 9).
2. `ftpd on` serves every mounted volume and says where:
   `ftpd: serving / at ftp://10.0.0.4/ (read-write)`. The console is free again at once.
   `/data/notes.txt` on SurfOS is `ftp://10.0.0.4/data/notes.txt` on the Mac. Without an
   address yet it says `... has no address yet: run dhcp first`; the server listens all the
   same.

`ftpd on /data` serves that folder alone (`ftp://10.0.0.4/notes.txt` is then
`/data/notes.txt`), and `ftpd on -r /data` the same read-only. A folder without a leading `/`
is taken from the current directory.

**From the Mac:**

* **Finder** reads: Go > Connect to Server (Command-K), `ftp://10.0.0.4`, Connect, and
  Guest. The laptop's folders open in a Finder window; drag files out of it to copy them to
  the Mac. Finder's FTP never writes: to copy onto the laptop use one of the others.
* **Terminal**, with `curl`, which macOS has:

  ```sh
  curl ftp://10.0.0.4/data/                         # the folder's listing
  curl -O ftp://10.0.0.4/data/notes.txt             # a file, into the current folder
  curl -T photo.jpg ftp://10.0.0.4/data/            # a file onto the laptop
  curl -T photo.jpg ftp://10.0.0.4/data/pic.jpg     # ... under another name
  curl -Q 'DELE /data/old.txt' ftp://10.0.0.4/      # delete one
  curl -Q 'MKD /data/photos' ftp://10.0.0.4/        # a new folder
  curl -Q 'RNFR /data/a.txt' -Q 'RNTO /data/b.txt' ftp://10.0.0.4/    # rename
  curl -C - -O ftp://10.0.0.4/data/big.zip          # go on with a download that stopped
  ```

* **An FTP program**, for dragging files both ways and whole folders: Cyberduck (free),
  FileZilla or Transmit. Server `10.0.0.4`, port 21, protocol FTP (plain FTP, not FTPS or
  SFTP), anonymous login (any user name and password do). FileZilla asks once whether to go
  on without TLS: yes.
* **Python**, for scripts: `ftplib` in the standard library, for example
  `ftplib.FTP('10.0.0.4', 'anonymous').storbinary('STOR /data/a.txt', open('a.txt', 'rb'))`.

**What can be written:** mounted FAT sticks, and the stick SurfOS started from when it is a
one-stick (section 14). The initrd (`/initrd`) and a SURFOSUSB boot stick are read-only: an
upload there is refused (`550 ... Read-only file system`). `/dev` is never served.

**Watch it work.** `ftpd serve` runs the same server on this console instead of in the
background, and prints each command a client sends with its reply's code (a password shows as
`****`), and each transfer with its bytes. Ctrl+C stops it.

```
SURF ≋> ftpd serve /data
ftpd: serving /data at ftp://10.0.0.4/ (read-write)
ftpd: each command is printed here; Ctrl+C stops the server
10.0.0.12 session 1 from port 51234
10.0.0.12 USER anonymous -> 331
10.0.0.12 PASS **** -> 230
10.0.0.12 PASV -> 227
10.0.0.12 STOR /photo.jpg -> 226, 482113 bytes
```

**Check and turn off**, on the laptop or in a telnet session:

| Command | What it says |
|---|---|
| `ftpd` (or `ftpd status`) | `ftpd: running (pid 15)`, or `ftpd: not running` |
| `ftpd off` | `ftpd: stopped`: the server is gone, and every client's connection with it |
| `dmesg` | `ftpd: serving / on port 21 (read-write, passive ports 50000-50999)` when it started, each session (`ftpd: session 1 from 10.0.0.12:51234`, and when it ended, with the files it sent and received) |

**nc, started on SurfOS.** `nc` makes one connection by hand. The Mac's `nc` is the other end:

```sh
# the Mac to the laptop
SURF ≋> nc -l 9000 > /data/photo.jpg          # on SurfOS: wait for it
$ nc 10.0.0.4 9000 < photo.jpg                 # on the Mac: send it

# the laptop to the Mac
$ nc -l 9000 > notes.txt                       # on the Mac: wait for it
SURF ≋> nc 10.0.0.2 9000 < /data/notes.txt    # on SurfOS: send it (the Mac's address)
```

Either side ends when the file is through: the sender tells the other end its file has ended
(a half-close), the receiver closes, and both are done. The Mac's address is in System
Settings > Wi-Fi > Details, or `ipconfig getifaddr en0` in Terminal (`en1` for some Ethernet
adapters). More:

* `nc -z 10.0.0.2 22 80 443` says which of those ports are open (a range: `8000-8010`).
* `nc -c example.com 80`, then type `GET / HTTP/1.0` and an empty line: a web server answered
  by hand (`-c` sends each line with CR LF, as mail and web servers want).
* `nc -v ...` says both ends of the connection and how it ended; `-w 30` ends a connection
  silent for 30 seconds, or an `nc -l` that nobody connects to.
* A line typed is sent when its Enter comes, and what arrives meanwhile shows after it.
  Ctrl+C ends `nc`.

**The laptop by name.** `mdns on` makes the laptop `surfos.local` on its network (Bonjour, which
the Mac asks by itself), so its address need not be looked up: `ftp://surfos.local/` in Finder,
`curl ftp://surfos.local/data/`, `telnet surfos.local`, `ping surfos.local`. `mdns` says where it
answers, `mdns off` stops it (and tells the other computers to forget the name at once); `mdns
on lab-pc` answers for `lab-pc.local` instead (the name in the settings' `hostname` is the
default). When another computer has the name already, SurfOS takes `surfos-2.local` and says so
in `dmesg`.

**fetch, files from the web or from the Mac.** `fetch` downloads an `http://` or `https://`
address into a file in the current directory (on a mounted stick: the initrd is read-only):

```
SURF ≋> cd /data
SURF ≋> fetch https://www.example.com/
fetch: 1.2 KB in 0.4 s (3.1 KB/s), TLSv1.3 -> index.html
```

From the Mac, with nothing installed: in Terminal, in the folder whose files the laptop is to
take, start a web server with `python3 -m http.server 8000` (with the Mac's firewall on, macOS
may ask once whether Python may accept incoming connections: Allow), then on SurfOS:

```
SURF ≋> fetch http://10.0.0.2:8000/photo.jpg       (the Mac's address)
fetch: 470.8 KB in 0.9 s (523.1 KB/s) -> photo.jpg
```

* The file is named after the address (`index.html` for a folder); `-o name` names it, `-o
  /data/photos` puts it in that folder, `-o -` prints it.
* A file that is there already is kept: `-f` replaces it. A download cut short keeps what
  came and says so; `fetch -c` the same address goes on with it. A server that cannot send the
  rest of a file sends all of it again, and then the file is kept as it was: `fetch -f`
  downloads it anew (Python's `http.server` on the Mac is such a server).
* HTTPS checks the server's certificate as Undertow does, with no way around it, so the
  laptop's clock must be roughly right (Undertow's guide, The clock); `-t '2026-10-25
  14:05:00'` gives the time in UTC for one download. Redirects are followed, never from
  HTTPS to HTTP.

Limits:

* One background server; up to 6 clients at once (a seventh is told to try again later). A
  client silent for 5 minutes is disconnected (programs reconnect by themselves).
* Files up to 2 GB.
* No logins, no encryption (no FTPS or SFTP). Clients on the network can change everything in
  the folder served: serve only what may be changed, or use `-r`.
* File times show as SurfOS's clock has them (it has no time zone): `ftpd` does not give the
  Mac the UTC times some programs ask for, so they show no time, or the listing's.
* Started in a telnet session, the server ends with the session (`ftpd on` says so).
* Passive data connections use ports 50000-50999; behind a router that forwards them, tell
  `ftpd` the router's address: `ftpd on -a 203.0.113.7` (a range of your own:
  `ftpd on 60000-60099`).

## 18. The command-line tools and the manual

*11/02/26* (`docs/superpowers/plans/2026-11-01-cli-suite.md`). SurfOS has a manual: `man <name>`
shows the page of any program, kernel command or surfsh builtin, through `less` at a terminal,
and `man -k <word>` searches the pages' first lines. The tools the suite added:

| | |
|---|---|
| Text | `sed`, `awk`, `grep -E`, `diff`, `patch`, `cmp`, `comm`, `join`, `paste`, `fold`, `nl`, `rev` |
| Files | `tar`, `gzip`, `gunzip`, `zcat`, `unzip`, `od`, `base64`, `strings`, `file`, `tree`, `basename`, `dirname` |
| System | `top`, `watch`, `fsck`, `cal`, `bc` |
| Network | `telnet`, `ftp` |

* A tool that changes a file you have (`sed -i`, `patch`, `tar x`, `unzip -o`, `gzip`) writes a
  new file beside it and renames it over, as `edit` saves, so a failure leaves the old file as it
  was. Archives and patches that name absolute paths or `..` are refused.
* `fsck usb1p1` checks a FAT volume without writing anything: it says `clean`, or lists the
  problems. `fsck -r` is refused for now: two reviews found ways its repairs could lose data, so a
  damaged stick is still repaired on another computer (First Aid on the Mac, `fsck` elsewhere).
* `ftp` asks for a password only at a terminal, without echo, and never keeps it; `telnet` and
  `ftp` have no encryption, like the servers.
* None of this has been tried on the laptop yet.

## What works, what does not

| Works on the laptop (verified under emulation) | Not supported yet |
|---|---|
| UEFI boot from USB, GRUB, Multiboot 2 | Secure Boot (GRUB is unsigned) |
| Framebuffer console filling the screen (160x45 cells of Spleen 12x24 at 1920x1080, `font=` for another), VGA colours, 4 consoles, scrolling back | USB keyboards and mice; USB hubs and docks |
| Built-in keyboard through the 8042 (IRQ 1, and polled as a fallback) | Touchpad gestures such as two-finger scrolling (the touchpad works as a two-button mouse: [GUI](GUI.md)) |
| PIT timer, or the local APIC timer when the PIT does not tick | The internal SSD or hard disk (no AHCI/NVMe driver: they are never touched) |
| Delays calibrated on the PIT, the ACPI PM timer or the HPET | WPA3-only Wi-Fi networks, Bluetooth, sound, battery, suspend, ACPI power-off |
| RAM accounting above 4 GB (the kernel itself uses the RAM below 4 GB) | More than one CPU core |
| Ramdisk initrd, user programs and their C library, system calls, the CMOS clock, reboot | Network adapters that speak only a vendor protocol (ASIX, older Realtek modes) |
| The USB 3 controller; wired networking through a USB Ethernet adapter (CDC ECM/NCM, RTL8153): DHCP, ping, DNS, TCP; unplugging and plugging back in | exFAT and NTFS; drives with 4096-byte sectors |
| USB flash drives: MBR and GPT partitions (`fdisk`), FAT32 and FAT16 that macOS reads (`mkfs`, `mount`, `eject`, `df`), long file names, the editor and file programs | Typing letters beyond ASCII on the laptop's keyboard (a telnet session can) |
| The shell, surfsh: the SurfOS prompt, Tab completion, history (Ctrl+R, `!!`), scrolling back (Shift+PgUp), a pager, quotes and redirection, pipes, variables, wildcards, scripts and job control, the kernel's commands; UTF-8 on the screen | A startup file and saved history (in the works) |
| Telnet into the laptop (`telnetd`): surfsh in another computer's terminal, up to 4 sessions | Logins, passwords and encryption (no SSH): telnet is for a trusted network |
| Development builds over the network onto a data stick, started by GRUB's first entry | |
| One FAT32 stick for booting and for files, the boot files copied onto it (`tools/stick.py`) | |
| Wi-Fi on the Intel 9560 (open and WPA2-Personal, joined on the laptop 10/10/26); saved networks joined by themselves at startup and the settings on the stick (under emulation) | |
| A web server for static sites (`httpd`): HTML, CSS, pictures, folders listed | Forms, uploads, server-side programs, HTTPS |
| Files to and from other computers over FTP (`ftpd`: Finder, curl, Cyberduck and FileZilla on the Mac), and `nc` both ways | FTPS and SFTP, logins; UTC file times (MDTM) |
| Downloads over HTTP and HTTPS (`fetch`), resumed where they stopped | Uploads over HTTP; FTP as a client |
| The laptop by name on its network (`mdns`: `surfos.local` to the Mac's Bonjour) | Services in Finder's sidebar (DNS-SD), reverse lookups, IPv6 |

## How it works

* **Boot.** The kernel carries a Multiboot 2 header next to its Multiboot 1 header
  (`boot/boot.S`). GRUB loads it on UEFI, exits the firmware's boot services and starts it
  in 32-bit protected mode with a Multiboot 2 information block; `kernel/multiboot2.c`
  checks every tag against the block's size before using it.
* **Memory.** The memory map decides where the page tables, the free-frame stack and the
  DMA heap go (`mm/pmm.c`); they used to sit at fixed addresses where UEFI firmware keeps
  data. RAM above 4 GB is counted with 64-bit arithmetic and left unused.
* **Screen.** `driver/framebuffer.c` draws the console's cells into the firmware's
  framebuffer with an 8x16 font, scaled by the largest whole number at which 640x400
  fits; the grid then takes as many cells as the screen holds (120x33 at 1920x1080), and
  the console follows its size (10/25/26: under profile=laptop at 1920x1080 or more in
  Spleen 12x24, 160x45 cells; `font=` chooses 8x16, 12x24 or 16x32). A cell holds a code page 437 character, into which the
  console decodes UTF-8; the font has those 256 characters, from Spleen, with SurfOS's
  `▸` and `≋` drawn for it. A framebuffer above 4 GB is reached with a
  4 MB page (PSE-36), write-combining through PAT.
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
  into `eth0` for the existing network stack, and the mass-storage driver
  (`driver/usb/msc.c`: the Bulk-Only Transport and SCSI commands) turns a flash drive into
  the block device `usb0`, `usb1`, … .
* **Files** (`driver/part.c`, `shell/fdisk.c`, `fs/fat.c`, `fs/fat_mkfs.c`). Partition
  tables are read (MBR with logical partitions, GPT) and written through plans that are
  checked before and verified after writing. FAT is written the way Microsoft's
  specification lays it out, which macOS and Linux check without complaint. One rule,
  `bdev_may_format()` in `driver/bdev.c`, decides what `fdisk` and `mkfs` may change.
* **Programs** (`user/`). User programs run in ring 3 and link `libsurf`, a small C
  library (stdio, stdlib, string, dirent, time, errno) over 44 system calls; `edit` uses
  the console's raw mode and a subset of ANSI escape sequences.
* **The shell** (`user/surfsh/`, `shell/prompt.c`). `init` starts `/initrd/bin/surfsh` on
  every console and starts it again when it ends. It edits the line in raw mode, draws
  the prompt with the same renderer as the kernel's own shell, and runs the kernel's
  commands inside its own task through two system calls, which can also send their output
  to a file. A pager in the terminal layer stops each command's output after a
  screenful.
* **Development builds** (`boot/grub-usb.cfg`, `kernel/autonet.c`, `user/bin/rx.c`,
  `tools/devloop.py`). GRUB looks at each disk other than the boot drive (`test -f` on
  `(hdN,msdos1)`, `(hdN,gpt1)` and `(hdN)`, which says nothing when a disk is not there)
  for `/surfos-dev/surfos.bin` and `initrd.tar`, and when it finds them makes the first
  entry boot them with the kernel word `autonet`. The empty
  `/boot/grub/x86_64-efi/fs.lst` keeps GRUB from trying to load file system modules,
  which are not on the drive, for a disk that is not FAT. `autonet` starts a kernel
  thread that waits up to 90 seconds for a link, gets a DHCP lease (three tries) and runs
  `telnetd on` on F1. `rx` writes what one connection sends to `<path>.part`, then renames
  it; after a rename that failed with an I/O error the `.part` file is kept (the rename may
  have half happened, the two names sharing their clusters), and rx says to have the disk
  checked before either is deleted or rx runs for them again. So a later rx does not start
  over a `<path>.part` of `<path>`'s size and time, which is how that pair looks, and says
  so; delete it once the disk is known to be sound. A `.part` that a killed rx left is
  written again as before. (Since 10/24/26, storage milestone 1, a failed rename changes
  nothing, a rename over a file cut short never leaves two names on one chain, and SurfOS
  keeps the chain for the other name of such a pair; rx's care stays for the pairs older
  builds and other systems left.) The Makefile writes
  `etc/build-id` into the initrd at every build.
* **One stick** (`boot/grub-usb.cfg`, `driver/part.c`, `driver/bdev.c`, `tools/stick.py`).
  The boot loader finds the partition it was started from through GRUB's `$cmdpath`, so it
  runs from any FAT partition, the image's or a stick's. The menu asks GRUB for that
  volume's serial number (`probe --fs-uuid`) and passes it to the kernel as
  `bootvol=5D0F-0901`; SurfOS compares it with the boot sector of every partition it finds
  (and of every disk without a partition table) and makes the disk that holds it the boot
  drive, which `fdisk` and `mkfs` refuse, while that one volume may be mounted read-write.
  On a stick that has `boot/ONE-STICK.txt` the menu looks for `surfos-dev` on its own
  partition before the other disks, and `tools/devloop.py` writes there first too. `tools/mkusb.py
  --stick-dir` writes the image's files as a folder, and `tools/stick.py` copies it onto
  a mounted FAT volume of an external drive, checked against the manifest, each file
  written under a temporary name and read back before it is renamed into place.
* **Telnet** (`user/telnetd/`, `net/sock.c`, `kernel/pty.c`, `fs/poll.c`). Programs reach
  TCP through descriptors (listen, accept, connect) and wait on several at once with
  `poll()`. A pseudo-terminal is a terminal whose screen is a byte stream read by the
  program holding its other side. `telnetd` is one background process: it accepts
  connections, starts surfsh on a pseudo-terminal for each, and passes the bytes between
  the two in the telnet protocol (the window size, Ctrl+C, who echoes).
* **Settings and saved networks** (`kernel/netman.c`, `net/netcfg.c`, `shell/settings.c`,
  `shell/wifi.c`). A kernel thread reads `surfos-config/` on the volume GRUB named
  (`bootvol=`) at startup, mounted read-only on `/.netman` for that moment; a keeper thread
  joins the first saved network in range with its stored key (never the password) and asks
  for its address; every DHCP lease is renewed at half its time, as RFC 2131 has it. A change
  is written to a `.new` file, synced, renamed into place and read back.

The design is in [ARCHITECTURE.md](ARCHITECTURE.md) sections 14, 16, 17, 18 and 19, and the original briefs in
[superpowers/plans/2026-10-05-lenovo-ideapad-3-15iil05-usb-boot.md](superpowers/plans/2026-10-05-lenovo-ideapad-3-15iil05-usb-boot.md),
[superpowers/plans/2026-10-05-usb-storage-fat32-libc.md](superpowers/plans/2026-10-05-usb-storage-fat32-libc.md),
[superpowers/plans/2026-10-06-surfsh.md](superpowers/plans/2026-10-06-surfsh.md),
[superpowers/plans/2026-10-06-telnetd.md](superpowers/plans/2026-10-06-telnetd.md)
for development builds over the network, [superpowers/plans/2026-10-09-wifi.md](superpowers/plans/2026-10-09-wifi.md) (track D),
and, for the settings and saved networks, [superpowers/plans/2026-10-17-network-manager.md](superpowers/plans/2026-10-17-network-manager.md).

## Make targets

```sh
make usb-image [O=2]     # build/usb/surfos-ideapad-15iil05.img + .sha256 + build-manifest.json, and build/usb/stick/
make test-usb  [O=2]     # boot that image under OVMF from emulated USB sticks: 26 scenarios (tools/qemu-usb.py --test)
make test-mkusb          # the image builder's own tests (tools/mkusb.py)
make test-devloop        # the development loop's host side: tools/devloop.py, the data stick image, the build id
tools/mkusb.py --inspect build/usb/surfos-ideapad-15iil05.img   # list and check an image
```

`make test-usb` finds OVMF in the usual places (Homebrew, Debian/Ubuntu `ovmf`) or takes
`OVMF_CODE=… OVMF_VARS=…`.

## Undertow on the boot image

The normal USB build includes `/initrd/bin/undertow`, its pinned certificate roots,
`/initrd/etc/undertow.conf` and the Mbed TLS/root-store licenses. Build with
`devenv/build-usb.sh` so the pinned Intel Wi-Fi firmware and its licenses are fetched
and included too. The image is a file; building it does not write a physical disk.

After establishing a network connection, run `undertow` and check its UTC clock and
HTTPS readiness, then `open https://example.com/`. `clock YYYY-MM-DD HH:MM:SS` sets
UTC for that browser session if the hardware clock is wrong. HTTP remains available
when strong randomness is absent; HTTPS cannot bypass that requirement or a failed
certificate check. See [the browser guide](UNDERTOW.md) for commands and limitations.

QEMU exercises the browser through USB Ethernet and simulated Wi-Fi. Physical laptop
browsing, clock correctness and RDRAND availability still need an owner-run check;
no emulator result substitutes for that check.
