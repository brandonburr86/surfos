# SurfOS documentation (branch `ai-dev`)

`ai-dev` is the development fork of the restored 2004 SurfOS kernel. `master` stays
as the restoration; new work happens here.

| Document | What it is for |
|---|---|
| [ARCHITECTURE.md](ARCHITECTURE.md) | How the kernel works today: boot sequence, memory map, scheduler, interrupts, drivers, shell, build system, the UEFI laptop boot, USB networking, USB storage and FAT32, surfsh, the telnet server with the sockets, pseudo-terminals and `poll()` it stands on, the network manager with the settings on the stick, the web server `httpd`, surfsh's pipelines and scripting with the kernel's pipes, and files to and from other computers with `ftpd` and `nc` (section 22) |
| [CODE-AUDIT.md](CODE-AUDIT.md) | Every bug and risk found in the source, with file:line references, and what was verified by booting under QEMU |
| [ROADMAP.md](ROADMAP.md) | The plan: ground rules, the Phase 0 toolchain recipe, a catalog of kernel modules with acceptance tests, and a recommended milestone order |
| [USB-BOOT.md](USB-BOOT.md) | Building the USB image, writing it to a drive, booting SurfOS on the Lenovo IdeaPad 3 15IIL05, networking it through a USB Ethernet adapter, keeping files on a second USB stick, reaching its shell from another computer over telnet, one stick for everything, Wi-Fi with saved networks and the settings on the stick (section 15), hosting websites with `httpd` (section 16), and files to and from the Mac over FTP and `nc` (section 17) |
| [WIFI-PLANNING.md](WIFI-PLANNING.md) | Wi-Fi through the laptop's internal card: the plan it was built from (the driver works on the laptop since 10/10/26; saved networks: USB-BOOT.md section 15) |
| [hardware/ideapad-3-15iil05.md](hardware/ideapad-3-15iil05.md) | The laptop: known facts, unknowns, how the kernel copes with each, test evidence and the form for the hardware test |
| [GUI.md](GUI.md) | SurfOS desktop: Notepad, Files, Calculator, Sketch, the Terminal, System Monitor, Disks, Log, Clock, Character Map, Image Viewer and Hex, the menus and windows, mouse input and current touchpad limits; 11/09/26 the desktop applications: Settings, Spreadsheet, Code Editor, Paint, Chat, Mines, Solitaire and Swell, and what the desktop does for programs (Apps > Programs, Open with, screenshots) |
| [UNDERTOW.md](UNDERTOW.md) | The HTTP/HTTPS text browser: commands, certificate checks, limits, testing and interoperability |
| [GRAPHICS.md](GRAPHICS.md) | Basic 2D framebuffer drawing, `gfxinfo`, `gfxtest`, the kernel interface and text-console recovery |
| [../surfcpu/README.md](../surfcpu/README.md) | SurfCPU, our RV32IM RISC-V CPU for the Arty A7 FPGA, its SoC, the SurfBoot firmware and the RISC-V port of SurfOS |
| [superpowers/plans/…usb-boot.md](superpowers/plans/2026-10-05-lenovo-ideapad-3-15iil05-usb-boot.md) | The brief the laptop port was built from |
| [superpowers/plans/…storage….md](superpowers/plans/2026-10-05-usb-storage-fat32-libc.md) | The brief for USB storage, FAT32, the C library and the editor |
| [superpowers/plans/…surfsh.md](superpowers/plans/2026-10-06-surfsh.md) | The brief for surfsh, the SurfOS shell: the prompt design, the pager, Tab completion, SurfOS 1.0 |
| [superpowers/plans/…telnetd.md](superpowers/plans/2026-10-06-telnetd.md) | The brief for telnetd, the telnet server: network connections for programs, pseudo-terminals, `poll()` |
| [superpowers/plans/…network-manager.md](superpowers/plans/2026-10-17-network-manager.md) | The brief for the network manager: saved Wi-Fi networks joined at every start, the settings on the stick |
| [superpowers/plans/…httpd.md](superpowers/plans/2026-10-17-httpd.md) | The brief for httpd, the web server: static websites from a folder, keep-alive, folder listings |
| [superpowers/plans/…surfsh-scripting.md](superpowers/plans/2026-10-24-surfsh-scripting.md) | The brief for pipelines and scripting in surfsh: kernel pipes, pipelines, variables and the environment, script files, job control |
| [superpowers/plans/…nettools.md](superpowers/plans/2026-10-24-nettools.md) | The brief for the network tools: `ftpd`, an FTP server for files both ways with the Mac, `nc`, and the sockets' half-close, both ends and connections not waited for |
| [superpowers/plans/…daily-console.md](superpowers/plans/2026-10-25-daily-console.md) | The brief for track A, the everyday console and shell: TAB, programs before kernel commands, a fast screen, modifier keys, Ctrl+R, scrollback, Spleen 12x24, escapes, `less` and tools, a startup file and saved history |
| [superpowers/plans/…robustness.md](superpowers/plans/2026-10-25-robustness.md) | The brief for track B, robustness: kernel threads that can't be killed, floating point per task, out of memory that fails the program and not the machine, kernel stack guards, a clean reboot and `poweroff` |
| [superpowers/plans/…cli-suite.md](superpowers/plans/2026-11-01-cli-suite.md) | The brief for the command-line suite (USB-BOOT.md section 18): sed, awk, diff and patch, tar, gzip and unzip, fsck (checking only), top, watch, the manual and `man`, telnet and ftp clients, bc and the small utilities, with an account per track beside it |
| [superpowers/plans/…desktop-apps.md](superpowers/plans/2026-11-05-desktop-apps.md) | The brief for the desktop applications (milestone M14): the desktop's side (menus, choosers, paste, keys, the theme, the registry, screenshots), a window toolkit, libsurf's pictures, and Settings, Spreadsheet, Code Editor, Paint, Chat and three games, with an account per track beside it (`…desktop-apps-desk.md`, `-kit`, `-img`, `-settings`, `-sheet`, `-code`, `-paint`, `-chat`, `-games`) and the integration's (`…desktop-apps-integration.md`) |
| [superpowers/plans/…app-windows.md](superpowers/plans/2026-10-25-app-windows.md) | The brief for track C, windows for user programs (milestone M12): a window descriptor, `libgui`, the Calculator as a program, a launcher; then the desktop's polish |

## Building and running

```
make              # build/O0/surfos.bin  (make O=2 for an optimised build)
make run          # boot in QEMU on this terminal (serial console; Ctrl-A x quits)
make test-all     # headless smoke test over the serial console at -O0 and -O2
make iso          # bootable GRUB ISO (needs grub-pc-bin, xorriso, mtools)
make usb-image    # USB disk image for UEFI PCs (needs grub-efi-amd64-bin, grub-common, mtools)
make test-usb     # boot that image under QEMU + OVMF from emulated USB sticks (20 scenarios)
make unittest     # host unit tests
make test-web     # deterministic HTTP/HTTPS browser scenarios in QEMU
make test-tcp     # TCP against hand-made segments (tools/qemu-tcp.py)
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
console draws UTF-8, and the banner says SurfOS 1.0. Since M11 (under emulation so far)
programs have TCP connections, `poll()` and pseudo-terminals, and `telnetd on` lets
another computer on the network telnet in to a surfsh of its own (`SURF ≋@>`). Since
10/10/26 the laptop's own Wi-Fi card works (open and WPA2 networks, joined on the laptop),
and since 10/17/26 (under emulation so far) a network saved with `wifi save` is joined by
itself at every start, with the settings (`settings`: telnet at startup, Ethernet's DHCP,
a host name) kept on the stick SurfOS starts from.
`httpd on` (10/17/26, under emulation so far too) serves a folder, such as a site on a
USB stick, as a website that browsers on the network open at the laptop's address.
Since 10/24/26 (under emulation so far) surfsh has pipelines (`dmesg | head`), `&&`,
`||`, groups and subshells, here-documents and lines that continue, on pipes in the
kernel (`SYS_pipe`).
Since 10/24/26 (under emulation, and with the Mac's own curl, Finder and `nc` against it in
QEMU) `ftpd on` lets the Mac copy files from the laptop's sticks and onto them over FTP
(Finder reads, curl and FTP programs such as Cyberduck both read and write), and `nc` sends
a file to another computer's `nc` or takes one from it. Since 10/25/26 `fetch` downloads
an `http://` or `https://` address into a file (from the web, or from a Mac running
`python3 -m http.server`), on Undertow's HTTPS, and `mdns on` makes the laptop
`surfos.local` to the Mac (multicast DNS).
