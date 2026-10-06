# SurfOS documentation (branch `ai-dev`)

`ai-dev` is the development fork of the restored 2004 SurfOS kernel. `master` stays
as the restoration; new work happens here.

| Document | What it is for |
|---|---|
| [ARCHITECTURE.md](ARCHITECTURE.md) | How the kernel works today: boot sequence, memory map, scheduler, interrupts, drivers, shell, build system, the UEFI laptop boot, USB networking, USB storage and FAT32 |
| [CODE-AUDIT.md](CODE-AUDIT.md) | Every bug and risk found in the source, with file:line references, and what was verified by booting under QEMU |
| [ROADMAP.md](ROADMAP.md) | The plan: ground rules, the Phase 0 toolchain recipe, a catalog of kernel modules with acceptance tests, and a recommended milestone order |
| [USB-BOOT.md](USB-BOOT.md) | Building the USB image, writing it to a drive, booting SurfOS on the Lenovo IdeaPad 3 15IIL05, networking it through a USB Ethernet adapter, and keeping files on a second USB stick |
| [WIFI-PLANNING.md](WIFI-PLANNING.md) | Wi-Fi through the laptop's internal card (tabled): what is needed before building it |
| [hardware/ideapad-3-15iil05.md](hardware/ideapad-3-15iil05.md) | The laptop: known facts, unknowns, how the kernel copes with each, test evidence and the form for the hardware test |
| [superpowers/plans/…usb-boot.md](superpowers/plans/2026-10-05-lenovo-ideapad-3-15iil05-usb-boot.md) | The brief the laptop port was built from |
| [superpowers/plans/…storage….md](superpowers/plans/2026-10-05-usb-storage-fat32-libc.md) | The brief for USB storage, FAT32, the C library and the editor |
| [superpowers/plans/…surfsh.md](superpowers/plans/2026-10-06-surfsh.md) | The brief for surfsh, the SurfOS shell: the prompt design, the pager, Tab completion, SurfOS 1.0 |

## Building and running

```
make              # build/O0/surfos.bin  (make O=2 for an optimised build)
make run          # boot in QEMU on this terminal (serial console; Ctrl-A x quits)
make test-all     # headless smoke test over the serial console at -O0 and -O2
make iso          # bootable GRUB ISO (needs grub-pc-bin, xorriso, mtools)
make usb-image    # USB disk image for UEFI PCs (needs grub-efi-amd64-bin, grub-common, mtools)
make test-usb     # boot that image under QEMU + OVMF from emulated USB sticks (12 scenarios)
make unittest     # host unit tests
```

Needs `gcc` with 32-bit support, `binutils`, `nasm`, `qemu-system-x86` and `python3`.
`CLAUDE.md` at the repository root has the longer version plus the conventions.
For macOS or a containerized build, use [devenv](../devenv/README.md):
`./devenv/build.sh` builds the kernel and boot images with modern GCC multilib;
`./devenv/run.sh` boots the exported files with host QEMU; `./devenv/build-usb.sh` builds
the USB image and `./devenv/test-usb.sh` / `./devenv/run-usb.sh` boot it with host QEMU.

Status in one paragraph: the kernel builds with a 2026 toolchain, boots to its shell
under QEMU (`-kernel` and GRUB ISO) and can be driven entirely over the serial console.
Multitasking, demand paging, the PIT, keyboard, VGA console, PCI scan and most shell
commands work, as do user programs, FAT and an IPv4 stack on QEMU's e1000. Since
milestone M7 it also boots from a USB drive on a UEFI PC: under OVMF from an emulated
USB stick it reaches the shell on a framebuffer console with the PS/2 keyboard and no
serial port, with or without a PIT, and the owner has booted it on the Lenovo IdeaPad.
Since M8 it drives the laptop's USB 3 controller and USB Ethernet adapters (CDC ECM/NCM,
such as RTL8153 ones); the owner reports that the network works on the laptop. Since
M9 (under emulation so far) a second USB stick holds files: `fdisk` (MBR or GPT), `mkfs`
(FAT32 that macOS checks clean), `mount`/`umount`/`eject` by hand, user programs on a
small C library, `sh` and a text editor, while the boot drive and internal disks are
never repartitioned or formatted. The console fills the screen (120x33 at 1920x1080).
Since M10 (under emulation so far) the shell is a program, `/initrd/bin/surfsh`, on every
console: the SurfOS prompt (`initrd ▸ bin ≋>`), Tab completion, history, a pager, quotes
and redirection, with the kernel's commands reached through two system calls; the
console draws UTF-8, and the banner says SurfOS 1.0.
