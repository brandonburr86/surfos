# SurfOS: notes for AI sessions

SurfOS is a 2004 hobby kernel for 32-bit x86 (Multiboot, ring-0 shell, pre-emptive
tasks, demand-paged kernel heaps) that was restored from printouts in 2026. Branch
`master` is the restoration; `ai-dev` is where the kernel grows. Read `docs/README.md`
first; it points to the architecture map, the code audit and the roadmap.

## Build, run, test

Requirements: `gcc` with 32-bit support (`gcc-multilib` on Debian/Ubuntu), `binutils`,
`nasm`, `qemu-system-x86`, `python3`. For `make iso`: `grub-pc-bin`, `xorriso`, `mtools`.
For `make usb-image`: `grub-common`, `grub-efi-amd64-bin`, `mtools`; for `make test-usb`:
`qemu-system-x86_64` and OVMF (`ovmf`). On macOS use `devenv/` (Docker builds, host QEMU runs).

```
make              # build/O0/surfos.bin (O=2 for an optimised build)
make run          # boot in QEMU on this terminal over the serial console; Ctrl-A x quits
make test         # headless boot + smoke test over the serial console; exit code 0 = pass
make test-all     # the smoke test at -O0 and -O2
make unittest     # host tests: blibc, the Multiboot 2 parser + free-frame ranges, the framebuffer renderer, USB/NCM parsing,
                  # partition tables (+ sfdisk/sgdisk cross-checks), mkfs (+ fsck.fat), the user C library,
                  # the prompt renderer, code page 437 and the UTF-8 decoder, the console's pager (test_pager),
                  # telnetd's protocol and its poll loop against a simulated kernel (test_telnet, test_telnetd),
                  # PCI configuration space as lspci -v reads it (test_pciinfo), the development loop's host side (test_devloop.py),
                  # Wi-Fi: crypto (test_crypto), the WPA2 supplicant against a simulated AP (test_wpa), 802.11 frames (test_ieee80211),
                  # Intel's firmware files (test_iwlfw: hand-made ones, and the real ones when firmware/ has them)
make usb-image    # build/usb/surfos-ideapad-15iil05.img: GPT + FAT32 + GRUB for UEFI (docs/USB-BOOT.md)
make test-usb     # boot that image under OVMF from emulated USB sticks: 13 scenarios (tools/qemu-usb.py)
make test-mkusb   # tests of the image builder (tools/mkusb.py); the image tests need mtools
make test-devloop # tools/devloop.py's logic, the data stick image (tools/mkimage.py devstick), the build id
make test-tcp     # the TCP stack against hand-made segments: tools/qemu-tcp.py (+ rawnet.py), QEMU 7.2 or later
make debug        # QEMU paused with the gdb stub; gdb build/O0/surfos.bin -ex 'target remote :1234'
make iso          # GRUB ISO for real hardware or other emulators (carries the initrd as a module)
make images       # build/images/test.img (MBR + FAT16 partition) and initrd.tar (rootfs/ + the user programs); run/test attach them;
                  # foreign-fat32.img (made by mkfs.fat + mcopy: long, accented, macOS-style names) for the storage scenario
make user         # just the user programs, build/O0/user/bin/
python3 tools/qemu-run.py --kernel build/O0/surfos.bin --shell ps memstat     # ad hoc commands
python3 tools/qemu-run.py --kernel build/O0/surfos.bin --disk build/images/test.img --initrd build/images/initrd.tar --shell lsblk
python3 tools/qemu-run.py --kernel build/O0/surfos.bin --initrd build/images/initrd.tar --scratch 64 --shell 'fdisk rd1'  # a blank ramdisk rd1 to partition and format
# the harness gives QEMU a user-mode network (gateway 10.0.2.2) with port forwards to guest ports 7 and 23 (telnet: q.ports['telnet']) and a host HTTP server (httpport= on the kernel command line)
python3 tools/qemu-run.py --kernel build/O0/surfos.bin --screen --keys 'help\n'  # VGA text dump
python3 tools/qemu-run.py --kernel build/O0/surfos.bin --initrd build/images/initrd.tar --append shell=kernel --shell ps   # the ring-0 shell (≋#>)
python3 tools/qemu-run.py --kernel build/O0/surfos.bin --initrd build/images/initrd.tar --append profile=laptop --shell drivers acpi
python3 tools/qemu-usb.py --image build/usb/surfos-ideapad-15iil05.img --shell drivers   # UEFI boot, serial
python3 tools/qemu-usb.py --image build/usb/surfos-ideapad-15iil05.img --screen --keys 'help\n'  # no serial: keyboard + framebuffer
python3 tools/qemu-usb.py --image build/usb/surfos-ideapad-15iil05.img --extra '-netdev user,id=n0 -device usb-net,bus=xhci.0,netdev=n0' --shell lsusb dhcp 'ping 10.0.2.2'  # a USB Ethernet adapter
python3 tools/qemu-usb.py --image build/usb/surfos-ideapad-15iil05.img --extra '-drive if=none,id=d1,format=raw,file=/tmp/stick.img -device usb-storage,bus=xhci.0,drive=d1,removable=on' --shell lsblk 'fdisk usb1'  # a data stick (--extra splits at spaces: no spaces in the path)
python3 tools/qemu-usb.py --image build/usb/surfos-ideapad-15iil05.img --test --scenario storage   # one scenario
```

Every change must build with `make` and pass `make test-all` before it is committed;
a change to the boot path, memory setup, console, timer, keyboard, the USB stack or anything
`profile=laptop` touches must also pass `make test-usb` (at -O0 and -O2) and `make unittest`.
Add a smoke-test step in `tools/qemu-run.py` (the `SMOKE` table) when you add a
shell command or a kernel feature that the shell can exercise, and a check in
`tools/qemu-usb.py` when it matters on the laptop.

## Layout

```
boot/      Multiboot 1 + 2 entry (boot.S), grub.cfg for the ISO, grub-usb*.cfg for the USB image
kernel/    gdt, interrupts + ISR stubs (traps.asm), scheduler (task.c), process.c (spawn, ELF), syscall.c, console, tty, keyboard, timer, panic,
           pty.c (pseudo-terminals: a tty with a stream console), proc.c (procinfo, detach),
           multiboot.c + multiboot2.c (boot information), cpu.c (CPUID, CR0/CR4, PAT), acpi.c, apic.c (laptop profile),
           autonet.c (the kernel word autonet: a link, a lease and telnetd at boot, for development builds)
mm/        physical page stack + reservations (pmm.c, memrange.c), paging (+ large/temporary mappings), kernel heap (kalloc), 1:1 DMA heap, uvm.c
driver/    serial console, PCI (+ id tables; pciinfo.c: configuration space for lspci -v and wifiinfo), driver table, block layer (bdev.c: buses, the format rule; ramdisk, ATA), partition tables
           (parttab.c: parsing and change plans, host-tested; part.c: registering, the boot drive, writing), e1000 NIC, parallel port, DMA, floppy, 3c905B NIC,
           framebuffer.c + fbcore.c + font8x16.c (the UEFI console: 256 code page 437 glyphs made by tools/bdf2c.py from
           fonts/spleen-8x16.bdf and fonts/surfos-symbols.bdf, with their licences), cp437.c (the character set, the UTF-8 decoder)
           usb/: xhci.c (USB 3 host controller, polled), usb.c (enumeration, the usb thread, lsusb),
           usbnet.c (CDC ECM/NCM Ethernet adapters as eth0), msc.c (USB sticks: Bulk-Only Transport + SCSI, as usb0, usb1, ...),
           usbdesc.c (descriptor and NCM parsing, host-tested)
fs/        VFS (mounts, paths, files, descriptors, rename, statfs), block cache, tarfs (initrd), FAT12/16/32 (fat.c), mkfs (fat_mkfs.c), devfs
           (/dev/kmsg too), poll.c (poll(): vnode_ops->poll, poll_wake())
net/       the net thread and buffers, Ethernet, ARP, IPv4, ICMP, UDP, TCP, DHCP client, DNS resolver; sock.c (TCP connections as
           programs' descriptors); routing over several interfaces (by source address, then subnet, Ethernet before Wi-Fi);
           wlan/: the 802.11 station layer (wlan.c: scan list, joining, wlan0; ieee80211.c: frames, host-tested) and the
           WPA2 supplicant (wpa.c); interfaces in include/net/wlan.h and include/net/wpa.h
lib/crypto/ SHA-1/256, HMAC, PBKDF2, the 802.11 PRF, AES-128, key wrap, an RDRAND-fed HMAC-DRBG (include/crypto/crypto.h)
user/      user programs: user.ld, libsurf (crt0, a small C library: stdio, stdlib, string, ctype, dirent, time, errno), include/ (the
           standard headers and surf.h), bin/*.c (built into the initrd's /bin: sh, edit, ls, cp, mv, rm, mkdir, df, fstest,
           pagetest, kcmdtest, nettest, ptytest, rx (one file from the network), ...), surfsh/ (the SurfOS shell: main,
           editor, complete, parse, run, builtins),
           telnetd/ (the telnet server: main the commands, server the poll loop, telnet the protocol)
lib/blibc/ the in-tree libc (printf, strings, getch, ctype, CMOS time)
driver/wifi/ wlansim.c: a simulated Wi-Fi chip and access points, only with the kernel word wlansim (tests);
           iwl_fw.c: Intel's firmware files (the TLV container, include/sys/iwl_fw.h, host-tested); iwl.c: the
           laptop's Intel 9560 (profile=laptop only; registers in include/sys/iwl_reg.h): PCI, power, wlan ops;
           iwl_trans.c: its firmware transport (context info, receive and command queues, polled); iwl_priv.h
           what the two share; growing step by step (docs/superpowers/plans/2026-10-09-wifi.md)
firmware/  Intel's firmware for the laptop's card, fetched by tools/fetch-firmware.py (pinned, SHA-256 checked;
           devenv/build-usb.sh runs it); never committed; the USB image's initrd carries it in /lib/firmware/
shell/     the ring-0 shell and the kernel's commands (main.c: the table, shell_run_command() for SYS_kcmd), prompt.c (the SurfOS
           prompt, also built into libsurf), fdisk.c, fscmd.c (mkfs, mount, umount, eject, df, mv, fsbench), selftest.c, demos
include/   surfos/ kernel headers, mm/, sys/ driver headers, fs/ (vfs.h, bcache.h), net/, libc headers
tools/     qemu-run.py test harness (with a telnet client), mkimage.py (disk image, initrd with its build id, the data stick),
           mkusb.py (the USB image), qemu-usb.py (its UEFI boot tests), bdf2c.py, qemu-tcp.py + rawnet.py (TCP against
           hand-made segments), devloop.py (development builds onto the laptop over the network)
tests/     host/ unit tests (blibc, boot information, framebuffer, USB parsing, partitions, mkfs, the C library, the telnet
           protocol, telnetd's loop), test_part_tools.py,
           test_mkusb.py, test_devenv.py, test_devloop.py
rootfs/    contents of the initrd (build/images/initrd.tar), block device rd0
devenv/    Docker toolchain and host QEMU wrappers: build.sh, run.sh, build-usb.sh, test-usb.sh, run-usb.sh
docs/      ARCHITECTURE.md, CODE-AUDIT.md, ROADMAP.md, USB-BOOT.md, hardware/ (the IdeaPad record), superpowers/plans/ (the brief)
```

## Conventions

* Stay inside the architecture described in `docs/ROADMAP.md` section 1: 32-bit,
  monolithic, Multiboot 1, 8259 PIC + PIT, software task switching. The laptop port
  (milestone M7) widened it: Multiboot 2 from GRUB on UEFI, a framebuffer console, the
  ACPI tables and the boot CPU's local APIC (virtual wire, its timer as a fallback) are
  in scope; x86-64, SMP and I/O APIC routing are not. The USB stack (milestone M8) polls the
  xHCI event ring from a kernel timer rather than relying on the controller's interrupt.
* Storage safety (milestone M9): `bdev_may_format()` in `driver/bdev.c` is the only rule
  for what may be repartitioned or formatted: USB drives and ramdisks, never an ATA
  (internal) disk, never the SurfOS boot drive (`protect`, set by `driver/part.c` from the
  volume label SURFOSUSB), never anything read-only, gone or mounted. Anything new that
  writes a partition table or a file system goes through it, shows what it will change and
  asks for the device's name. Nothing mounts a USB drive by itself.
* Network connections and terminals (milestone M11): a TCP connection or listening socket
  is a descriptor (`include/surfos/sock.h`, `net/sock.c`); a pseudo-terminal
  (`include/surfos/pty.h`, `kernel/pty.c`) is a tty whose console is a byte stream that
  the holder of its master reads, and a master's write never waits. Whatever changes a
  descriptor's readiness calls `poll_wake()`, and a vnode's poll op never does. A kernel
  thread never belongs to a pty session (`task_alloc()` puts it on F1), and a pty's hangup
  kills its processes with the kill that waits for the end of the call, without the
  2-second fallback (a kernel command is never killed holding a lock). A line-mode Ctrl+C
  always gets through (it ends an escape sequence and drops type-ahead); after type-ahead
  is dropped, the serial line and ptys skip input until it pauses for 200 ms (`TTY_SKIP`),
  so the rest of a paste never runs in pieces. `telnetd` has no login or encryption by the
  owner's choice: never start it by itself. The one exception is the kernel word `autonet`
  (kernel/autonet.c), which only GRUB's development entry passes (docs/USB-BOOT.md section 13).
* The shell (milestone M10): `init` starts `/initrd/bin/surfsh -l` on every console; the
  ring-0 shell is the recovery console (`≋?>`) and `shell=kernel` (`≋#>`). Kernel commands
  return CMD_OK, CMD_FAIL or CMD_USAGE (0, 1, 2) and also run inside a user process's
  system call (SYS_kcmd), so a new command prints on `curTask->con` (printf), never on
  `conActive`; keeps no state in statics that two consoles could use at once (or claims
  it for one run at a time, as heaptest and selftest do); asks its questions through `shell_ask()` (so a
  redirection does not swallow them); and returns rather than looping when its input
  ends. The prompt is drawn only by `shell/prompt.c`.
* Two profiles: without `profile=laptop` on the kernel command line (QEMU `-kernel`, the
  GRUB ISO) the boot must stay exactly as it was; `profile=laptop` (the USB image) is real
  UEFI hardware. Every hardware wait is bounded and a missing legacy device is a normal
  configuration, never a hang. The USB image is only ever built as a file: nothing in the
  build or the tests may write a disk device, a boot entry or firmware settings.
* Fix things the way the audit describes them and cite the audit ID (for example
  "A6", "T1") in the commit message; update `docs/CODE-AUDIT.md` when an item is closed.
* Console output goes through `kputch()` (so the serial mirror sees it); use
  `kprintf()` in the kernel and `printf()` in task code.
* Keep the 2004 file headers and style where you touch old code; new files use the
  same header block with the current date.
* Small commits, one topic each. Do not rewrite `master`.

## Debugging tips

* `tools/qemu-run.py --int-log file.log ...` records every interrupt and exception
  QEMU delivers (`-d int`); `grep "v=0d"` finds general protection faults.
* A killed task or a panic prints a symbolized backtrace; `build/O<level>/surfos.sym`
  is the sorted symbol table for anything else. `dmesg` in the shell shows the kernel log.
* Shell commands `crashdiv`, `crashgp`, `crashint` raise real exceptions on purpose.
* The screen dump works even when the serial console does not.
* `panic()`, `BUG_ON()`, `ASSERT()` are in `include/surfos/panic.h`; `irq_save()` /
  `irq_restore()` in `include/surfos/irq.h` for anything an ISR touches.
* Kernel APIs: `kthread_create()`/`task_exit()`/`task_wait()`/`sleep_ms()` (task.h),
  `wait_on()`/`wake_up()` (wait.h), `mutex_t`/`semaphore_t`/`event_t` (sync.h),
  `ktimer_start()` (ktimer.h), `kalloc()`/`kcalloc()`/`kfree()`/`kalloc_dma()`
  (mm/kalloc.h), `vmm_map()`/`pmm_alloc()` (mm/paging.h, mm/pmm.h). The scheduler
  runs only in trap context; never call `schedule()` directly.
* Every kernel stack must be fully mapped (task_alloc pre-faults it): a page fault on
  the stack itself is a double fault.
* `selftest` in the shell (shell/selftest.c) is where kernel-level tests go; add a
  check there and a line to `SMOKE` in tools/qemu-run.py for new features.
* On the laptop (or under OVMF): `bootinfo`, `drivers`, `cpuinfo`, `acpi`, `memstat` say
  what the firmware handed over and which console, timer and keyboard path was chosen.
  `lspci -v` gives every PCI function's ids, subsystem, power state, MSI/MSI-X and PCI
  Express link, `lspci bb:dd.f` one with its 256 bytes, and `wifiinfo` the Wi-Fi card
  for a future driver (docs/WIFI-PLANNING.md).
  Kernel command-line words for experiments: `timer=lapic`, `kbd=poll`, `kbd=irq`,
  `clock=pmtmr`, `clock=hpet`, `usb=off`. `tools/qemu-usb.py --extra '-machine pit=off'` removes the PIT.
* USB: `lsusb` lists the controller and every device with what drives it; `+USB:` lines in
  `dmesg` record plugging and errors. Endpoint 0 transfers are synchronous and belong to
  the usb thread (or boot); bulk/interrupt `done` callbacks run in the timer interrupt.
  QEMU's `device_del`/`device_add` on the monitor unplug and plug a USB device.
* Storage: `lsblk` (STATUS says why a device may not be changed), `fdisk <disk>` alone
  (only reads), `mount`, `df`, `lsblk -s` (blocks moved), `hexdump <dev> <block>`,
  `readbench`, `fsbench`; `+PART:` and `+FAT` lines in `dmesg`. A device that was unplugged
  stays in the list as gone (never freed while referenced); its mounts fail cleanly until
  `umount`. The storage scenario checks the sticks afterwards with macOS `fsck_msdos`
  (`hdiutil attach -nomount -readonly`) and `fsck.fat` (host or the devenv container).
* The shell: `shell=kernel` on the kernel command line gives the ring-0 shell on every
  console; a recovery console (`≋?>`) has a line from init saying why surfsh could not run.
  `ascii` takes UTF-8 out of TTY_CAPS (the ASCII prompt). `pagetest N|utf8|caps` exercises
  the pager and the character set, `kcmdtest` the kernel-command calls. The harnesses
  answer `--More--` with Space (`auto_more`), and `PROMPT` is a compiled pattern that
  matches every prompt state; a smoke step `@boot <words>` boots QEMU again with other
  kernel words (or `no-initrd`).
* Network connections and telnet: `nettest` (echo server and client, limits) and `ptytest`
  (pseudo-terminals, poll, detach, hangups) are in the initrd; `tcpka=<idle>,<interval>` on
  the kernel command line shortens keepalive; `tools/qemu-tcp.py --scenario <name>` runs
  one TCP scenario (`--verbose` prints every segment); the harness's `Telnet` client
  (tools/qemu-run.py) reaches guest port 23 through `q.ports['telnet']`; `telnetd status`
  and `dmesg` show the server's sessions and why each ended.
* The development loop (docs/USB-BOOT.md section 13): `tools/devloop.py` drives the laptop from the Mac over
  telnet. `find`; `run <cmd>...` (output with escapes stripped, `--More--` answered; it refuses `reboot`, `mkfs`,
  `fdisk` changes and `telnetd off`); `log` (dmesg); `deploy` (the newest `surfos.bin` and `initrd.tar` under
  build/docker/build/O2 or O0, from `devenv/build-usb.sh`, pushed with `rx` onto the data stick's
  `/surfos-dev/`, then eject, reboot through GRUB's development entry, telnet again, `/initrd/etc/build-id`
  checked); `reboot`. The laptop (10.0.0.4) belongs to one track at a time, as the integrator assigns it; test
  against QEMU instead: `tools/qemu-usb.py --test --scenario devloop`, or devloop.py with `--host 127.0.0.1
  --port <forwarded telnet> --rx-port <a port forwarded to itself> --net none` against a guest you started
  (`tools/mkimage.py devstick` makes a data stick image). Never run `find` or a default `--net` from a test:
  it searches 10.0.0.0/24. On the laptop the boot stick is the 118 GB General USB Flash Disk (USB 346d:3281,
  GPT, SURFOSUSB, "boot drive: protected") and the data stick the 30 GB USB Disk 2.0 (346d:5678, lsblk model
  "VendorCo Product", MBR, one FAT32 LBA partition): only mounted, written in `/surfos-dev/` and ejected, never
  formatted. usb0 and usb1 swap between boots, so devloop.py takes the USB disk that lsblk does not call
  protected and whose first partition is FAT, never a name or a size. The laptop boots from USB first, so a
  `reboot` comes back to the boot stick's GRUB by itself.
* Wi-Fi (docs/superpowers/plans/2026-10-09-wifi.md): `wifi up | down | scan | join <ssid> [bssid] | status |
  leave` (shell/wifi.c); a WPA2 password is asked with `shell_ask_secret()` (never echoed or kept) and turned into
  the PMK at once. The kernel word `wlansim` gives a simulated card with six access points (SurfSim, SurfSimWPA
  with the test passphrase in tools/qemu-run.py, a hidden one, WEP, WPA3-only, a flaky one) bridged onto eth0, so
  `dhcp wlan0` works in QEMU; the smoke test's `@boot wlansim net` and `@boot wlansim usbnet-later` blocks drive
  it. Never put a real Wi-Fi password in a file, a log or a test. The laptop's Intel card says each stage of a
  start in `+IWL:` lines (dmesg; `tools/devloop.py log`) and its state in `wifi status`'s driver line; a firmware
  error prints the firmware's error log. QEMU has no Intel card: that code runs only on the laptop.
* User programs: system call numbers are in `include/surfos/syscall.h` (libsurf includes
  it through `user/libsurf/sysnum.h`); errors come back as negative POSIX `errno` values.
  The editor uses only `ttyctl(TTY_RAW)`, `ttyctl(TTY_GETSIZE)` and the ANSI subset that
  `include/surfos/tty.h` lists; `rawkeys` and `ansi` in the shell show both sides.
