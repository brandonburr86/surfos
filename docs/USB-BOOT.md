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
| The same image tested under emulation | done | `make test-usb`: 12 cold boots under OVMF, at -O0 and -O2 |
| Boot on the physical IdeaPad | **reported working by the owner (2026-10-05)** | the details are still to be recorded in [hardware/ideapad-3-15iil05.md](hardware/ideapad-3-15iil05.md) |
| Wired network through a USB Ethernet adapter | done under emulation; **reported working on the laptop by the owner (2026-10-05)** | `make test-usb`, scenario `usbnet`; [section 8](#8-networking-a-usb-ethernet-adapter) |
| Files on a second USB stick: partitions, FAT32 that the Mac reads, a text editor, a C library | done under emulation; **pending on the laptop** | `make test-usb`, scenarios `usbdisk` and `storage`; [section 9](#9-files-on-a-usb-stick) |
| The SurfOS shell, surfsh: the breadcrumb prompt, Tab completion, a pager; SurfOS 1.0 | done under emulation; **pending on the laptop** | `make test` and `make test-usb`; [section 10](#10-the-shell) |

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
devenv/test-usb.sh      # the automatic tests: 12 cold boots, about three minutes
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

The text fills the screen: 120x33 characters on a 1920x1080 panel, with the letters
drawn at twice the font's size, or 170x48 on a 1366x768 panel.

The GRUB menu also has:

* **SurfOS (troubleshooting: local APIC timer, polled keyboard)**: the same kernel with
  `timer=lapic kbd=poll`, for when the normal entry boots but the clock or the keyboard
  does not respond.
* **SurfOS (kernel shell)**: the kernel's own shell on every console instead of surfsh
  (section 10).
* **SurfOS (USB controller off: no USB network adapter, no USB sticks)**: section 8.
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
| F1 to F4 | four consoles, each with its own shell |
| `edit /initrd/etc/version` | the text editor, full screen; Ctrl+Q leaves it (the ramdisk is read-only: files to keep go on a second stick, section 9) |

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

The GRUB menu has an entry **SurfOS (USB controller off: no USB network adapter, no USB
sticks)**. It boots without touching the USB controller, in case the USB driver ever gets
in the way on the laptop.

## 9. Files on a USB stick

SurfOS forgets everything at a restart: it never writes to its own drive (which it does
not even mount unless asked, and then only read-only), and the ramdisk is unpacked
afresh at every boot. Files that should last go on a **second USB
stick**. SurfOS partitions it and formats it as **FAT32**, the format macOS, Windows and
Linux all read and write, and it has a text editor and the usual file commands, so the
same stick carries files between SurfOS and the Mac. Nothing is mounted by itself: you
mount the stick, and you eject it before pulling it out.

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
data ▸ Documents ≋> /initrd/bin/ls -la
```

The kernel's `ls`, `cat`, `cp`, `mv`, `rm`, `mkdir` and `rmdir` come first; the programs
of the same names know more options (`ls -la`, `cp -r`, `rm -rf`, `mkdir -p`) and run by
their path, as above. The other programs: `touch`, `head [-n N]`, `wc [-lwc]`,
`hexdump`, `df`, `edit` and `sh` (an earlier, smaller shell). They are written against
SurfOS's small C library (`user/libsurf`), which new programs can use too.

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
renames it over the old file, so a failed save leaves the old file as it was. Windows
line ends (CR LF) stay as they were.

**Taking the stick out.** Type `eject usb1` and wait for `usb1: safe to remove`. It
unmounts every partition of the stick and writes everything still held in memory. When
something still uses the stick, `eject` says what, for example `eject: usb1: /data is
busy: it holds the current directory, /data (cd / first)`. A stick pulled out without
`eject` may lose its last changes, and the next `mount` warns `… was not unmounted
cleanly`; let the Mac check it then (*Disk Utility* → *First Aid*).

**On the Mac.** The stick appears under its label, and the files SurfOS wrote are
ordinary files. macOS adds hidden files of its own (`.fseventsd`, `.Spotlight-V100`,
`.DS_Store`, and `._` files beside others); they do no harm, and `ls -a` in `sh` shows
them. Eject the stick in the Finder before pulling it out there too.

**What SurfOS will not change.** `fdisk` and `mkfs` work only on USB drives and
ramdisks, and refuse:

* the SurfOS boot drive, recognised by its volume label SURFOSUSB whatever its number. It
  can be mounted, but only read-only;
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
  SurfOS drive, a data stick and the Ethernet adapter.

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
```

The `!` (in red) says the last command failed; it goes away after one that succeeds.
`cd` changes the directory (`cd` alone goes to `/`, `cd -` back to the previous one).
SurfOS has no users and no home directories, so a shell starts at `/`.

**Typing.**

| Keys | What they do |
|---|---|
| Left, Right, Home, End (Ctrl+A, Ctrl+E) | move in the line |
| Backspace, Delete | delete before or under the cursor |
| Ctrl+U, Ctrl+K, Ctrl+W | delete the whole line, to its end, the word before the cursor |
| Up, Down | the lines typed before (`history` lists them) |
| Tab | complete the command or file name; a second Tab lists the choices |
| Ctrl+L | clear the screen |
| Ctrl+C | drop the line |
| Ctrl+D | on an empty line: leave this shell |

Tab completes the first word from the shell's commands, the kernel's and the programs, and
any other word (or one with a `/` in it) as a file or directory name: one match is filled
in, several as far as they agree, and a second Tab lists them (asking first when there
are more than the screen holds).

**Long output** stops when the screen is full, with `--More--` on the last row: Space
shows the next screenful, Enter one more line, and q (or Esc, or Ctrl+C) leaves the rest
unshown. `help` is long on purpose.

**Which command runs.** First the shell's own commands (`cd`, `pwd`, `echo`, `exit`,
`help`, `history`, `set`), then the kernel's (everything in sections 7 to 9: `lsblk`,
`fdisk`, `mount`, `ifconfig`, `ps`, ...), then a program in `/initrd/bin`; a name with a
`/` in it is a program's path. The kernel's `ls`, `cat`, `cp`, `mv`, `rm`, `mkdir` and
`rmdir` come before the programs of the same names, which know more options: run those by
their path, for example `/initrd/bin/ls -la` (`help` marks them with `*`). `help` lists
all three groups, `help <name>` one command.

**A line** is words separated by blanks. `"double"` or `'single'` quotes keep blanks
inside a word, and `\` keeps the next character as it is. `cmd > file` sends what the
command prints into the file (`>>` adds to its end), `cmd < file` makes a program read
the file, `cmd &` runs a program in the background (its end is reported before the next
prompt), and `;` separates commands. `$?` is
the status of the last command (`echo $?`); there are no other variables, no wildcards
and no pipes. The kernel's commands can be redirected too, which is a quick way to keep
what they say: `lspci > /data/lspci.txt`. A question such as fdisk's still appears on the
screen.

**Statuses.** 0 is success, 1 a failure, 2 a command used the wrong way, 126 a program
that could not be started, 127 no such command, 137 a program the kernel killed. A
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

## 11. When something goes wrong

| What you see | What it means, what to do |
|---|---|
| The USB drive is not in the F12 menu | Write the image again with one of the writers above; try the other USB ports; check that Secure Boot is off and that USB boot is enabled. |
| "Secure Boot Violation", "Verification failed", or a red security warning | Secure Boot is still on (step 5). |
| GRUB says `file '/boot/…' not found`, or "SurfOS: /boot/surfos.bin … were not found" | The drive does not hold the image as written by a raw writer: write it again (step 4). |
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
| `+FAT32: usb1p1 was not unmounted cleanly` | The stick was pulled out without `eject`. If files look wrong, let the Mac check it (*Disk Utility* → *First Aid*). |
| A file name shows a `?` | It has a character the console cannot draw (section 10); the file itself is fine. |
| `--More--` on the last row | The output is longer than the screen: Space shows the next screenful, Enter one more line, q the rest is not shown. |
| The prompt ends in `≋!>` | The last command failed and said why; the mark goes away after one that succeeds. |
| The prompt is `SURF ≋?>`, with a line about a *recovery console* above it | surfsh could not run on that console (missing, not loadable, or it ended abnormally three times within 30 seconds), so the kernel's own shell took over. Every command still works and `surfsh` starts the shell by hand; please report the line. |
| `tick` does not change between runs | Choose the troubleshooting entry (it uses the local APIC timer). |
| Windows asks for a BitLocker recovery key | Enter the key you saved (step 5). It happens once after Secure Boot settings change. |

SurfOS has no ACPI power-off: `reboot` restarts the laptop; to switch it off, hold the
power button.

## What works, what does not

| Works on the laptop (verified under emulation) | Not supported yet |
|---|---|
| UEFI boot from USB, GRUB, Multiboot 2 | Secure Boot (GRUB is unsigned) |
| Framebuffer console filling the screen (120x33 cells at 1920x1080), VGA colours, 4 consoles | USB keyboards and mice; USB hubs and docks |
| Built-in keyboard through the 8042 (IRQ 1, and polled as a fallback) | The touchpad |
| PIT timer, or the local APIC timer when the PIT does not tick | The internal SSD or hard disk (no AHCI/NVMe driver: they are never touched) |
| Delays calibrated on the PIT, the ACPI PM timer or the HPET | Wi-Fi, Bluetooth, sound, battery, suspend, ACPI power-off |
| RAM accounting above 4 GB (the kernel itself uses the RAM below 4 GB) | More than one CPU core |
| Ramdisk initrd, user programs and their C library, system calls, the CMOS clock, reboot | Network adapters that speak only a vendor protocol (ASIX, older Realtek modes) |
| The USB 3 controller; wired networking through a USB Ethernet adapter (CDC ECM/NCM, RTL8153): DHCP, ping, DNS, TCP; unplugging and plugging back in | exFAT and NTFS; drives with 4096-byte sectors |
| USB flash drives: MBR and GPT partitions (`fdisk`), FAT32 and FAT16 that macOS reads (`mkfs`, `mount`, `eject`, `df`), long file names, the editor and file programs | Typing letters beyond ASCII |
| The shell, surfsh: the SurfOS prompt, Tab completion, history, a pager, quotes and redirection, the kernel's commands; UTF-8 on the screen | Pipes, variables, wildcards, job control |

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
  the console follows its size. A cell holds a code page 437 character, into which the
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
  library (stdio, stdlib, string, dirent, time, errno) over 33 system calls; `edit` uses
  the console's raw mode and a subset of ANSI escape sequences.
* **The shell** (`user/surfsh/`, `shell/prompt.c`). `init` starts `/initrd/bin/surfsh` on
  every console and starts it again when it ends. It edits the line in raw mode, draws
  the prompt with the same renderer as the kernel's own shell, and runs the kernel's
  commands inside its own task through two system calls, which can also send their output
  to a file. A pager in the terminal layer stops each command's output after a
  screenful.

The design is in [ARCHITECTURE.md](ARCHITECTURE.md) sections 14, 16 and 17, and the original briefs in
[superpowers/plans/2026-10-05-lenovo-ideapad-3-15iil05-usb-boot.md](superpowers/plans/2026-10-05-lenovo-ideapad-3-15iil05-usb-boot.md),
[superpowers/plans/2026-10-05-usb-storage-fat32-libc.md](superpowers/plans/2026-10-05-usb-storage-fat32-libc.md)
and [superpowers/plans/2026-10-06-surfsh.md](superpowers/plans/2026-10-06-surfsh.md).

## Make targets

```sh
make usb-image [O=2]     # build/usb/surfos-ideapad-15iil05.img + .sha256 + build-manifest.json
make test-usb  [O=2]     # boot that image under OVMF from emulated USB sticks: 12 scenarios (tools/qemu-usb.py --test)
make test-mkusb          # the image builder's own tests (tools/mkusb.py)
tools/mkusb.py --inspect build/usb/surfos-ideapad-15iil05.img   # list and check an image
```

`make test-usb` finds OVMF in the usual places (Homebrew, Debian/Ubuntu `ovmf`) or takes
`OVMF_CODE=… OVMF_VARS=…`.
