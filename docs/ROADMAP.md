# SurfOS Roadmap: growing the core kernel

A plan for taking the 2004 kernel forward **inside its existing architecture**.
It is a menu of modules with dependencies and acceptance tests, plus a recommended
order. It builds on `docs/ARCHITECTURE.md` (what exists) and `docs/CODE-AUDIT.md`
(what is broken; audit IDs like T1 or I5 are cited below).

## 1. Ground rules

Keep, because they are the architecture:

* 32-bit x86, protected mode, single CPU, Multiboot 1 / GRUB boot, ELF kernel at 1 MB
  (and, since M7, Multiboot 2 for GRUB on UEFI).
* One monolithic kernel: drivers, scheduler, filesystems and (for now) the shell all in ring 0.
* Flat segmentation (code 0x08 / data 0x10), 8259 PICs, PIT timer, VGA text console (since
  M7, on a UEFI PC: the 8259s through the local APIC, its timer when the PIT does not tick,
  and a framebuffer text console).
* Software stack-switching scheduler with priority run queues, `yield` via `int 0x40`.
* Identity-mapped low memory, demand-paged kernel heaps, NASM stubs + C.
* `blibc` as the in-tree libc, the shell as the primary user interface.
* The 2004 code where it works; modernize interfaces, not the spirit.

Out of scope until this plan is done: x86-64, SMP/APIC, UEFI, a microkernel split,
a higher-half kernel, a GUI, porting an external libc.

Widened in October 2026 for milestone M7, the laptop port
(`docs/superpowers/plans/2026-10-05-lenovo-ideapad-3-15iil05-usb-boot.md`): booting from
UEFI through GRUB and Multiboot 2, a framebuffer text console, reading the ACPI tables
the platform needs, and the boot CPU's local APIC (virtual wire mode, and its timer as
the fallback tick) are in scope. Still out of scope: x86-64, SMP, I/O APIC interrupt
routing, a UEFI loader of our own, Secure Boot signing.

Working rules for every change on this branch:

1. It builds with `make` and boots to the prompt with `make run` under QEMU.
2. It passes `make test` (Phase 0 adds it).
3. Small commits, one module or fix per commit; update `docs/` when behaviour changes.
4. Do not rewrite `master`; this branch (`ai-dev`) is where the kernel grows.

## 2. Starting point

Verified under QEMU 8.2 with GCC 13: the restored source compiles after three
mechanical fixes, boots, runs the shell, pre-empts tasks and demand-pages its heaps.
It has no blocking I/O, no safe critical sections, a leaky allocator, exception
stubs that mis-handle error codes, no working block device, no filesystem, no user
mode, and a NIC driver for a card QEMU cannot emulate. Full detail in the audit.

## 3. Phase 0: toolchain and development loop (done on `ai-dev`)

Goal: `git clone && make && make run` works on a 2026 Linux box, and an AI session
can see the screen without a human.

**Status: done.** The top-level `Makefile`, `linker.ld`, `boot/grub.cfg`,
`tools/qemu-run.py` and the four source fixes are on `ai-dev`; `make test-all` passes
at -O0 and -O2, and the GRUB ISO boots to the shell. The serial console (K4) landed in
the same series because the smoke test is built on it. What follows is the recipe as
implemented, kept for reference.

### 3.1 Compiler and linker flags

```make
CC      = gcc
NASM    = nasm
CFLAGS  = -m32 -march=i486 -ffreestanding -fno-builtin -nostdlib -nostdinc \
          -fno-pie -fno-pic -fno-stack-protector -fno-asynchronous-unwind-tables \
          -fcf-protection=none -mno-sse -mno-sse2 -mno-mmx \
          -fgnu89-inline -fno-strict-aliasing -fno-omit-frame-pointer \
          -Wall -Wno-main -I$(TOP)/include -g -O0
ASFLAGS = -m32 -ffreestanding -nostdlib -fno-pie      # for boot.S via gcc
NASMFLAGS = -felf32
LDFLAGS = -m elf_i386 -nostdlib -T linker.ld
```

`-O2` also boots once A5 and A6 (below) are fixed; keep `-O0 -g` as the default
while the inline assembly is being cleaned up (K1), then switch.

### 3.2 Linker script (`linker.ld`)

```ld
OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)
ENTRY(start)
SECTIONS
{
    . = 0x100000;
    .text   ALIGN(4096) : { *(.multiboot) *(.text .text.*) }
    .rodata ALIGN(4096) : { *(.rodata .rodata.*) }
    .data   ALIGN(4096) : { *(.data .data.*) }
    .bss    ALIGN(4096) : { __bss_start = .; *(COMMON) *(.bss .bss.*) __bss_end = .; }
    __kernel_end = .;
    /DISCARD/ : { *(.note.*) *(.comment) *(.eh_frame) *(.rel.eh_frame) }
}
```

`boot/boot.o` must stay first in the link so the Multiboot header lands inside the
first 8 KB (it ends up at file offset 4100).

### 3.3 Source fixes required to compile and to survive -O2

| Audit | File | Change |
|---|---|---|
| A3 | `mm/paging.c:151` | `pTmp = (u_long*)((u_long)page_table + (iDir << 12)); *pd = (u_long)pTmp \| 3;` |
| A4 | `driver/floppy/floppy.c` | prototypes for `fd_rw`, `fd_seek`, `fd_recalibrate`, `dma_xfer`, `dma_alloc`, `dma_free`; `fd_rw(FD0, block, buf, ...)`, `fd_seek(drive, 1)` |
| A5 | `kernel/gdt.c:125`, `kernel/interrupt.c:126` | `asm volatile("lgdt %0" :: "m"(gdtr))`, `asm volatile("lidt %0" :: "m"(*IDTMaster))` |
| A6 | `mm/paging.c:41` | `volatile u_long *m;` in `memprobe()` |

### 3.4 Targets

* `make` builds `surfos.bin` with one top-level Makefile and real pattern rules (A8).
* `make run`: `qemu-system-i386 -m 64 -kernel surfos.bin` (add `-serial stdio` after K4).
* `make test`: boots headless and talks to the shell over the serial console
  (`tools/qemu-run.py --test`): waits for the prompt, runs `tick`, `memstat`, `ps`,
  `help`, `test`, checks their output and that no panic text appeared. The same tool
  dumps the VGA text screen through the monitor (`pmemsave 0xb8000 4000`) and types on
  the PS/2 keyboard (`sendkey`), so it still works when the serial path is broken.
* `make iso` (optional): `grub-mkrescue` image for real hardware and other emulators.
* `make debug`: `qemu -s -S` plus `gdb surfos.bin -ex 'target remote :1234'`.
* `.gitignore` for `*.o`, `surfos.bin`, `*.iso`, `*.log`.

Acceptance: fresh clone, `apt install build-essential nasm qemu-system-x86`,
`make test` prints PASS for "banner and prompt visible" at -O0 and at -O2.
Size: one session.

## 4. Module catalog

Each module: why, what exists, what to build, how to prove it, size
(S = part of a session, M = one session, L = two or three, XL = more) and dependencies.

### Kernel core

**K1. Trap and interrupt framework v2** (I1-I6, I10, T8) - size M - no deps - **done on `ai-dev`**

* Build: one macro-generated set of 256 entry stubs that push a vector number and a
  real or dummy error code, a single `struct trapframe` (vector, error code, all
  registers, EIP/CS/EFLAGS, and ESP/SS when coming from ring 3), a dispatch table
  `trap_handlers[256]`, a default handler that panics with the vector, IDT zeroed and
  fully populated, no EOI from exception stubs, IRQ 15 installed, spurious IRQ 7/15
  detection (read the ISR register), `irq_save()`/`irq_restore()` helpers
  (`pushf; cli` / `popf`) next to the existing preemption counter.
* Keep the shared-handler chains, the turf-based masking API and the idle-stack
  switch; they are architecture, just feed them from the new stubs.
* Prove: `demo` can raise vectors 8, 10, 11, 12 without a triple fault; `int 0x50`
  from the shell gives "unexpected trap 0x50" instead of a reboot; a forced spurious
  IRQ 7 is logged, not acted on.

**K2. Descriptor tables** (I8, I9, M9) - size S - no deps - **done on `ai-dev`** (TSS loaded; esp0 is set per task in P1)

* GDT and IDT become arrays in kernel `.data`, limits 0xFFFFF, far jump to reload
  CS, the user descriptors stop being blanked, a TSS slot is reserved (filled by P1),
  the page directory moves out of 0x9C000.
* Prove: `die` fails with "no TSS" semantics only after P1 (it must still not take
  the kernel down); boot still works under QEMU and `grub-mkrescue` on real hardware.

**K3. Panic and diagnostics** (I7, T7) - size M - deps K1, K4 - **done on `ai-dev`** (the shell is respawned by the reaper; the init task proper comes with S1)

* Register dump with CR2, EFLAGS, error code and vector; symbolized EIP and a
  frame-pointer backtrace from a symbol table generated at link time
  (`nm` -> C array, linked in a second pass); `ASSERT`/`BUG_ON`; a kernel log ring
  buffer with levels and `dmesg`; a kernel-mode fault in the shell respawns the
  shell (an `init` supervisor task) instead of idling forever.
* Prove: `demo` -> 1 shows a backtrace through `demoException` and the shell comes back.

**K4. Serial console and early printk** - size S - no deps

* **Status: done on `ai-dev`** (`driver/serial.c`): COM1 at 115200 8N1, polled TX,
  IRQ 4 RX into the keyboard queue, `kputch()` mirror including the early boot log.
  Still open from the list below: the Multiboot command line and a real `term` command.

* 16550 UART driver on COM1 (8N1, 115200), polled TX, IRQ 4 RX; `kprintf` mirrors
  to serial from the first line (before paging is even enabled, serial needs no
  memory); the `term` command becomes a real serial terminal; Multiboot command
  line (`console=serial`) selects where the shell lives.
* This is the single highest-leverage item for AI-driven development: QEMU
  `-serial stdio` turns the kernel into a text program.
* Prove: `make run` shows the boot log in the terminal; `make test` asserts on it.

**K5. printf and libc** (A11, C2, C3, C7) - size M - no deps - **done on `ai-dev`**

* `vsnprintf` on `__builtin_va_list` with `%c %s %d %i %u %x %X %p %lu %ld %%`,
  width, zero padding, left-justify; `kprintf`, `printf`, `kcprintf`, `snprintf`
  all use it; one character output path (`kputch`) so `puts` stops writing video
  memory itself; standard `strcmp`/`strncmp`/`strncpy`; add `strcat`, `strlcpy`,
  `memcmp`, `memmove`, `strtol`, `atoi`, `strtok_r`; fixed-width types
  (`u8 u16 u32 i32`) in `types.h` while keeping the `u_long` family for old code.
* Prove: a host-side unit test (compile `lib/blibc` with the host compiler) covering
  the formatter and string functions; `floppy.c`'s `%02x` prints correctly.

### Memory

**M1. Physical memory manager v2** (M7, M8, A6) - size M - no deps - **done on `ai-dev`**

* Use the Multiboot `mem_upper` and `mmap` passed to `kmain`; reserve the kernel
  image (`__kernel_end`), page tables, page stack, the 1:1 DMA region and anything
  the map marks reserved; keep the free-page stack (it is simple and O(1)) but
  size it from the map; frame counters for `memstat`; a clean out-of-memory return
  instead of `BUG()`.
* Prove: `memstat` matches QEMU's `-m` within a few hundred KB at `-m 32`, `64`,
  `512`; boot with `-m 3072` no longer claims 3 GB of 1:1 heap.

**M2. Virtual memory API** (M6, M13) - size M - deps M1 - **done on `ai-dev`**

* `vmm_map(virt, phys, flags)`, `vmm_unmap`, `vmm_get_phys` (replaces
  `mm_lookup_linear`), `invlpg`; page 0 unmapped so NULL dereferences fault; the
  page-fault handler checks the error code and the address range: kernel-heap
  ranges are demand-mapped, everything else is a bug (K3 backtrace); guard pages
  below task stacks.
* Prove: `*(int*)0 = 1` in the shell produces a page-fault panic with a backtrace;
  a recursion test hits the guard page instead of corrupting the heap.

**M3. Kernel heap v2** (M1-M5, M11, M12, T5) - size M - deps M2 (soft) - **done on `ai-dev`**

* One allocator: block headers with size and magic, free list with splitting and
  coalescing (or a simple slab for common sizes plus a first-fit large pool),
  8-byte alignment, `kalloc`/`kfree`/`kcalloc`/`krealloc`, poisoning on free,
  double-free detection; `kalloc_dma()` served from the 1:1 region replaces
  `palloc` and `dma_alloc`; accurate `memstat`.
* Prove: a `heaptest` command runs 100,000 random alloc/free cycles with
  checksums and finishes with the same free-byte count it started with; the 2004
  "60,000 kills" scenario (`die` in a loop) no longer exhausts memory.

### Scheduling and time

**S1. Scheduler v2** (T1, T4, T6, T7, T13) - size L - deps K1 - **done on `ai-dev`**

* Explicit task states (READY, RUNNING, BLOCKED, SLEEPING, ZOMBIE), a time slice
  per priority instead of a switch on every tick, the idle task `hlt`s only when
  nothing is runnable, `task_exit(code)` and `task_wait(pid)`, a pid table with
  reuse, ZOMBIE reaping by the supervisor task, a safe `kill_task` (defer freeing
  the running stack to the reaper), per-task CPU time and switch counts for `ps`,
  a `kthread_create(name, fn, arg)` API, and a correct sleeping-queue walk.
* Keep the FIFO/HIGH/NORMAL/LOW levels and the `F H N L / F H N H` pattern as the
  default policy; it is the SurfOS flavour.
* Prove: `ps` shows state and CPU time; a tight loop at LOW cannot starve a HIGH
  shell; 10,000 `kthread_create`/exit cycles leak nothing.

**S2. Blocking and synchronization** (T2, T3, T9, T12) - size M - deps S1 - **done on `ai-dev`**

* Wait queues (`wait_on(queue)`, `wake_up(queue)`), `sleep_ms` that blocks,
  mutexes with owner tracking, counting semaphores, a one-shot event, all built on
  wait queues plus `irq_save`; the keyboard ISR wakes a wait queue so `getch()`
  blocks; the turf API is either reimplemented on the mutex/rwlock or retired with
  its callers (the IRQ-mask turf stays, it is small and correct).
* Prove: idle CPU use of the booted system under QEMU drops to near zero
  (`top` on the host); a producer/consumer test with two kthreads passes.

**S3. Timers and time** (I11, C8) - size S - deps S2 - **done on `ai-dev`**

* Monotonic milliseconds from the tick, `uptime`, kernel timers (one-shot and
  periodic callbacks run from the tick or a timer kthread), RTC date and time with
  a `date` command (the 2004 `time` stub), a TSC- or PIT-calibrated `udelay` to
  replace the nop loops.
* Prove: `date` matches the host clock; a periodic timer fires 100 times in 1 s
  within tolerance.

### Console and input

**C1. Console/TTY v2** (C1, C9, T10) - size M - deps S2

* A `tty` object per console: output buffer, input queue, line discipline (echo,
  backspace, line editing, history), wait queue for readers; the active console is
  a display property, the task's console is an I/O property, and the scheduler stops
  touching `conActive`; scrollback; `NUM_CONSOLES` configurable; the serial port is
  tty 4.
* Prove: F2 switches to a second shell on console 2 and back, with both keeping
  their own history; typing while a `funky` task prints does not interleave garbage.

**C2. Keyboard v2** (C5, C6, T11) - size S - deps C1

* Full scan-code set 1 including E0 prefixes, caps/num lock state, key events
  (make/break with modifiers) delivered to the tty layer, no task creation from the
  ISR, LED updates from a kthread.
* Prove: arrow keys recall history; shift/caps combinations produce the right ASCII.

### Devices

**D1. Driver model and PCI v2** (D3, D5, D6, H3) - size M - deps K1, M3

* `struct driver { name, init, probe, ... }` with a registration table and ordered
  init replacing `drivers.c`; `struct device` with bus, resources and IRQ; full PCI
  enumeration (all buses via bridges, all functions), `pci_find_device(vendor, dev)`,
  BAR mapping helpers (`vmm_map` for MMIO), the vendor/device/class names from
  `PCIDATA.H` behind an `lspci` command; parallel-port register fixes.
* Prove: `lspci` on QEMU lists "Intel 82540EM Gigabit Ethernet" and friends with
  IRQs and BARs; adding `-device rtl8139` shows up without code changes.

**D2. Block layer and storage** (D1, D2) - size L - deps D1, S2

* `struct bdev { read_blocks, write_blocks, block_size, count }`; a ramdisk backed
  by a Multiboot module (`qemu -initrd`), the fastest route to "files"; an ATA PIO
  driver (LBA28, IDENTIFY, primary and secondary, IRQ 14/15, QEMU `-hda`); MBR
  partition parsing; optional retro item: finish the floppy driver (correct ISA DMA
  page math, actually start the transfer, interrupt-driven waits).
* Prove: `hexdump /dev/hda 0` shows the MBR of a QEMU disk image; the ramdisk
  checksum matches the host file.

### Files

**F1. VFS and filesystems** - size L - deps D2, M3

* A small VFS: mount table, `vfs_open/read/write/close/readdir/stat`, path lookup,
  a `file` table per task; an initrd filesystem (tar or cpio, read-only) first,
  then FAT12/16/32 (read, then write) because QEMU, `mtools` and every other OS can
  produce the images; shell commands `ls cd pwd cat hexdump cp mkdir rm`.
* Prove: `cat /initrd/motd` and `ls /hda1` work; a file written by SurfOS is
  readable by `mdir`/`mcopy` on the host.

**F2. Files on a USB stick** (done on `ai-dev`, emulation) - size XL - deps F1, P2, N3

* Persistent storage on the laptop, whose internal disk SurfOS never touches: a USB
  mass-storage driver (Bulk-Only Transport, SCSI, error recovery, unplugging while
  mounted) on the xHCI stack of N3; a block layer that knows each device's bus and
  refuses to repartition or format anything but USB drives and ramdisks, and never the
  SurfOS boot drive; MBR and GPT tables (`lsblk`, `fdisk`, MBR by default); FAT32 that
  macOS accepts (`mkfs`, rename, FSInfo, the clean-shutdown bit, UTF-8 long names) and
  `mount [-r]`, `umount`, `eject`, `df`, all by hand; system calls 22-31, `libsurf` (a
  small C library with POSIX `errno`), file programs, `sh` with redirection and `edit`, a
  nano-like editor; a console grid that fills the screen, with raw mode and ANSI
  sequences for the editor.
* Prove: under OVMF, a blank 1 GB stick is partitioned and formatted by SurfOS, written
  through the C library, `sh` and the editor, ejected, and then passes macOS `fsck_msdos`
  and Linux `fsck.fat` with the files reading back on the host; a stick made by
  `mkfs.fat` with long, accented and macOS-style names reads in SurfOS; the boot drive is
  refused and mounts read-only (`make test-usb`, scenarios `usbdisk` and `storage`). On
  the laptop: the same with a real stick, read afterwards on the Mac.

### Processes

**P1. User mode and syscalls** (I8, I9) - size L - deps K1, K2, M2, S1

* TSS with `esp0` per task, correct ring-3 descriptors, an `int 0x80` gate with
  DPL 3, a syscall table (`write read open close exit getpid sleep yield spawn
  wait`), per-process page directory sharing the kernel's upper mappings,
  separate kernel and user stacks, exceptions in ring 3 kill only that process.
  The kernel stays monolithic; this is the boundary the 2004 code already named
  (`KERNEL`/`USER` in `task.h`).
* Prove: a user task that writes to address 0 is killed and the shell survives;
  a user task that does `int 0x80` write prints to its console.

**P2. Loader and processes** - size M - deps P1, F1

* ELF32 executable loader from the filesystem, `argv`, exit codes, `wait`, a tiny
  `crt0` + user libc built from `blibc`, `kill`; eventually the shell itself as a
  user program while the ring-0 debug shell remains available on another console.
* Prove: `/bin/hello` built with the cross flags runs from FAT and returns its
  exit code to the shell.

### Networking

**N1. NIC driver for QEMU hardware** (D4) - size M - deps D1, M3, S2

* An e1000 driver (QEMU's default, found by the PCI scan today) or rtl8139 (the
  simplest PCI NIC), interrupt-driven RX/TX rings in DMA-safe memory, implemented
  behind Martin McCormick's existing `iface` interface (`SnagPackets`,
  `SendPackets`, `Setting`) so the 3c905B code remains a valid second driver for
  real hardware.
* Prove: `ifconfig` shows the MAC from the EEPROM; a raw frame sent from the host
  (QEMU `-netdev user` + `-object filter-dump`) arrives in `SnagPackets`.

**N2. Network stack** - size XL - deps N1, S2, S3

* Ethernet, ARP (table + resolution), IPv4 (no fragmentation at first), ICMP echo
  both ways, UDP with a socket-like kernel API, a DHCP client, DNS resolver; TCP
  last. Commands: `ifconfig`, `arp`, `ping`, `dhcp`, `nslookup`, `udpecho`.
* Prove: `ping 10.0.2.2` (QEMU's gateway) gets replies; the host can ping the
  guest through a port forward; `dhcp` obtains 10.0.2.15.

**N3. USB Ethernet for the laptop** (done on `ai-dev`, emulation) - size L - deps N2, M7

* The IdeaPad has no Ethernet socket: an xHCI host controller driver (polled, root
  ports only), USB enumeration with configuration choice, and a CDC ECM/NCM class
  driver presenting a `struct netdev`; Realtek RTL8153 adapters through their CDC
  Ethernet configuration. Unplug and replug keep the interface name (and the address
  with the same adapter). `lsusb`, `usb=off`.
* Prove: under OVMF, QEMU's usb-net on qemu-xhci is eth0 at boot; `dhcp`, `ping`,
  `httpget` (1 MB) and `selftest` pass over it; unplugging and plugging it in through the
  QEMU monitor works (`make test-usb`, scenario `usbnet`); NCM's transfer blocks are
  host-tested. On the laptop: the same with an RTL8153 adapter.

**N4. Network connections for programs and a telnet server** (done on `ai-dev`, emulation) - size L - deps N2, P2, U2

* TCP connections and listening sockets as file descriptors (`listen`, `accept`,
  `connect`, `netinfo`), `poll()`, pseudo-terminals whose console is a byte stream,
  `procinfo` and `detach`, `/dev/kmsg`; `telnetd on|off|status`, one background process
  serving up to 4 surfsh sessions over telnet. TCP hardened for a server that must outlive
  bad clients: partial writes, the persist timer, keepalive, silly-window avoidance, no
  Nagle for program sockets, resets where RFC 793 wants them.
* Prove: smoke steps telnet into the guest through a forwarded port (a session, two, a
  fifth refused, Ctrl+C, the pager, a client that stops reading and one that floods keys,
  the protocol end to end); `make test-tcp` drives TCP with hand-made segments; `make
  test-usb` scenario `usbnet` runs a session over the USB adapter. On the laptop: `telnet`
  from the Mac.

### Shell, tests, hygiene

**U1. Shell v2** - size S-M - deps K5 (then grows with every module) - **done on `ai-dev`**

* A command table (`{name, help, fn(argc, argv)}`) with argument parsing,
  generated `help`, history, a `selftest` command that runs every module's
  self-test and prints `SELFTEST PASS` or the failures; new commands arrive with
  their modules (`lspci dmesg uptime date free kill heaptest ls cat ping`).

**U2. surfsh, the SurfOS shell** (done on `ai-dev`, emulation) - size L - deps U1, P2, F2, C1

* The shell becomes a program, as `bash` or `sh` is elsewhere: `/initrd/bin/surfsh`,
  which `init` starts on every console and restarts, with the ring-0 shell kept as a
  recovery console (when surfsh cannot run) and on purpose (`shell=kernel`). The owner's
  prompt design (2026-10-06): breadcrumbs with `▸`, the wave `≋`, a state character
  (`≋!>` failed, `≋#>` elevated, `≋@>` remote, `≋?>` diagnostic), drawn by one renderer
  (`shell/prompt.c`) that both shells use. A line editor with history and Tab
  completion, quotes and redirection, `$?`; the kernel's commands through two system
  calls (`SYS_kcmd`, `SYS_kcmdinfo`), with a status each and redirectable output; a pager
  in the terminal layer (`--More--`); a console that draws UTF-8 as code page 437 cells
  (accents, box drawing, `▸ ≋ …`); the banner of SurfOS 1.0.
* Prove: the smoke test and the USB scenarios drive surfsh on every console: the prompt
  and its states, Tab, history, redirection of programs and kernel commands, the pager
  (Space, Enter, q) from the serial line and the keyboard, UTF-8 on the VGA and
  framebuffer screens, the recovery console (no initrd, a surfsh that keeps failing) and
  the kernel shell entry. On the laptop: the same on its screen and keyboard.

**T1. Test and CI harness** - size S - deps Phase 0, K4 - **done on `ai-dev`** except the `selftest` command (comes with U1)

* `make test` boots QEMU headless, drives the serial console, runs `selftest` and
  greps for PASS; host-side unit tests for pure code (lists, allocator, printf,
  string functions) compiled with the host compiler; a GitHub Actions workflow
  (`apt-get install nasm qemu-system-x86`, `make test`) so every push to this
  branch is boot-tested.

**H1. Code hygiene** (H1-H6, A9, A10) - size S, ongoing - **first pass done on `ai-dev`**

* Delete dead code, fix the copy-pasted file headers, split `assem.asm` into
  `io.asm`/`traps.asm`/`switch.asm`, remove `static` prototypes from headers, add
  `REBUILD-NOTES.md` describing which files were reconstructed and from what, add
  a `CLAUDE.md` with the build, run, test and style rules, get the tree clean under
  `-Wall -Wextra`.

## 5. Recommended order

| Milestone | Modules | Done when |
|---|---|---|
| **M0 Builds and boots** (done) | Phase 0, K4 | `make test` passes at -O0 and -O2 on a fresh clone |
| **M1 Solid ground** (done) | K4, K1, K2, K5, K3, T1, H1 (first pass) | Every CPU exception produces a register dump and backtrace; no vector reboots or triple-faults the machine; the shell survives its own crash; boot log on serial; CI green |
| **M2 Kernel services** (done) | M1, M2, M3, S1, S2, S3, U1 | NULL dereferences fault; `heaptest` and the task churn test pass; `getch` and `sleep` block instead of spinning; `date`, `uptime`, `dmesg`, `ps` with states |
| **M3 Devices** (done) | C1, C2, D1, D2 | Virtual consoles work; `lspci` with names; ramdisk and ATA sectors readable |
| **M4 Files** (done) | F1 | `ls`/`cat` on an initrd and on a FAT disk image; FAT write verified from the host |
| **M5 Processes** (done) | P1, P2 | A user-mode ELF runs, makes syscalls, and cannot crash the kernel |
| **M6 Network** (done) | N1, N2 | `ping` works in both directions on QEMU user networking; DHCP lease obtained |
| **M7 Laptop USB boot** (done: the owner booted it on the laptop, 2026-10-05; the hardware record is still to be filled in) | the USB boot plan | `make usb-image` builds a GPT/FAT32 image with GRUB; `make test-usb` boots that exact file under OVMF from an emulated USB stick; the IdeaPad 3 15IIL05 boots it to an interactive shell (`docs/USB-BOOT.md`, `docs/hardware/`) |
| **M8 Laptop network** (done: the owner reports the network works on the laptop, 2026-10-05; details still to be recorded) | N3 | `make test-usb` scenario `usbnet` passes at -O0 and -O2; on the laptop an RTL8153 adapter gets a DHCP lease and `ping`/`httpget` work (`docs/USB-BOOT.md` section 8) |
| **M9 Files on a stick** (done under emulation, 2026-10-06; pending on the laptop) | F2 | `make test-usb` scenarios `usbdisk` and `storage` pass at -O0 and -O2; on the laptop a USB stick is partitioned, formatted, written with `edit`, ejected, and its files read on the Mac (`docs/USB-BOOT.md` section 9) |
| **M10 The SurfOS shell** (done under emulation, 2026-10-06; pending on the laptop) | U2 | `make test`, `make test-all` and `make test-usb` (at -O0 and -O2) drive surfsh on every console; on the laptop the prompt, Tab, the pager and the banner work on its screen and keyboard (`docs/USB-BOOT.md` section 10) |
| **M11 Telnet** (done under emulation, 2026-10-06; pending on the laptop) | N4 | `make test-all`, `make test-tcp` and `make test-usb` (at -O0 and -O2) telnet into the guest through a forwarded port and over the USB adapter; on the laptop a Mac on the same network telnets in, gets the banner and `SURF ≋@>`, and `telnetd off` ends the sessions (`docs/USB-BOOT.md` section 11) |

Rough effort: M0 one session; M1 three to four; M2 four to five; M3 three;
M4 three; M5 four; M6 five or more. M1 and M2 are the foundation and should not be
skipped to reach the showier milestones; most later bugs will otherwise land in the
allocator, the trap stubs and the sleeping-queue walk.

## 6. Module map for planning

For slicing the work into owned modules, this is the kernel by directory after the
plan, with the catalog IDs that build each one:

| Directory | Module | Catalog |
|---|---|---|
| `boot/` | Multiboot entry, early serial | Phase 0, K4 |
| `kernel/trap/` | stubs, IDT, IRQ chains, spurious handling | K1 |
| `kernel/` | GDT/TSS, panic, log, time, timers | K2, K3, S3 |
| `kernel/sched/` | tasks, run queues, wait queues, locks | S1, S2 |
| `mm/` | frames, VMM, heap | M1, M2, M3 |
| `dev/` (was `driver/`) | driver model, PCI, serial, keyboard, console/tty, ATA, ramdisk, NIC | D1, D2, K4, C1, C2, N1 |
| `fs/` | VFS, initrd, FAT, mkfs | F1, F2 |
| `net/` | ARP, IP, ICMP, UDP, DHCP, TCP | N2 |
| `proc/` | syscalls, ELF loader, user memory | P1, P2 |
| `user/` | `libsurf` (crt0, C library), programs, `sh`, `edit` | P2, F2 |
| `lib/blibc/` | formatter, strings, user crt0 | K5, P2 |
| `shell/` | command table, selftest | U1 |
| `tools/`, `tests/` | QEMU runner, host unit tests, CI | T1 |
| `docs/` | this set, kept current | all |

## 7. Immediate next step

M7 and M8 are reported working on the IdeaPad; their tables in
`docs/hardware/ideapad-3-15iil05.md` (adapter model, `lsusb`, `dhcp`, `ping` output) are
still to be filled in. M9, M10 and M11 are done under emulation and wait for the laptop
(`docs/USB-BOOT.md` sections 9 to 11, the storage, shell and telnet rows of the hardware
record).

The shell work left these open: typing letters beyond ASCII (the keyboard and the line
editor are byte-wise; the console already draws them); job control (bringing a background
program back); pipes, variables and wildcards in surfsh; and users and home
directories (a shell starts at `/`).

The telnet work left these open: logins and encryption (by the owner's choice; an SSH
server would need both); type-ahead in surfsh, so that a pasted block of several lines
runs whole; a terminal type (TTYPE) for programs that draw; a host name for the prompt's
remote state (`≋@>`); and in TCP, random initial sequence numbers, challenge ACKs (RFC
5961) and Nagle as an option for programs.

The storage work left these open, smallest first: the busy check of `umount` and `eject`
sees only the asking task's current directory (it should look at every task's);
`fdisk add` of logical partitions; one cache identity for a disk and its
partitions (today a block reached both ways is cached twice); FAT12 in `mkfs`; exFAT,
which larger sticks are sold with (deferred by the owner, October 2026); floating point in
`printf`; and a 64-bit `time_t` before 2038.

After that, in the order the laptop needs them (each its own milestone): USB hub
support (docks and multi-port dongles), the controller's interrupt through MSI (lower
latency than the 10 ms poll), a USB HID keyboard on the same stack, AHCI or NVMe for the
internal disk (identify the controller first; the block layer refuses to format internal
disks, and that rule stays), and the actual Wi-Fi chip (tabled by the owner on
2026-10-06, started 2026-10-09: `docs/superpowers/plans/2026-10-09-wifi.md`; the Intel 9560
loads its firmware and comes up as wlan0 on the laptop since 2026-10-10, milestone W1, and
`wifi scan` lists the networks in range, W2;
`docs/WIFI-PLANNING.md` and `wifiinfo` gathered what it needed).

The six earlier milestones are done. What remains there is polish and the items the catalog left open:
DHCP renewal, TCP congestion control
and out-of-order queueing, the floppy transfer path (D1/D2), the parallel-port register
fix (D5), wrapping the 3c905B driver behind `struct netdev`, retiring turf (T12), and
the user shell as the default shell on one console.
