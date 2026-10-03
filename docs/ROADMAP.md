# SurfOS Roadmap: growing the core kernel

A plan for taking the 2004 kernel forward **inside its existing architecture**.
It is a menu of modules with dependencies and acceptance tests, plus a recommended
order. It builds on `docs/ARCHITECTURE.md` (what exists) and `docs/CODE-AUDIT.md`
(what is broken; audit IDs like T1 or I5 are cited below).

## 1. Ground rules

Keep, because they are the architecture:

* 32-bit x86, protected mode, single CPU, Multiboot 1 / GRUB boot, ELF kernel at 1 MB.
* One monolithic kernel: drivers, scheduler, filesystems and (for now) the shell all in ring 0.
* Flat segmentation (code 0x08 / data 0x10), 8259 PICs, PIT timer, VGA text console.
* Software stack-switching scheduler with priority run queues, `yield` via `int 0x40`.
* Identity-mapped low memory, demand-paged kernel heaps, NASM stubs + C.
* `blibc` as the in-tree libc, the shell as the primary user interface.
* The 2004 code where it works; modernize interfaces, not the spirit.

Out of scope until this plan is done: x86-64, SMP/APIC, UEFI, a microkernel split,
a higher-half kernel, a GUI, porting an external libc.

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

### Shell, tests, hygiene

**U1. Shell v2** - size S-M - deps K5 (then grows with every module) - **done on `ai-dev`**

* A command table (`{name, help, fn(argc, argv)}`) with argument parsing,
  generated `help`, history, a `selftest` command that runs every module's
  self-test and prints `SELFTEST PASS` or the failures; new commands arrive with
  their modules (`lspci dmesg uptime date free kill heaptest ls cat ping`).

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
| **M3 Devices** | C1, C2, D1, D2 | Virtual consoles work; `lspci` with names; ramdisk and ATA sectors readable |
| **M4 Files** | F1 | `ls`/`cat` on an initrd and on a FAT disk image; FAT write verified from the host |
| **M5 Processes** | P1, P2 | A user-mode ELF runs, makes syscalls, and cannot crash the kernel |
| **M6 Network** | N1, N2 | `ping` works in both directions on QEMU user networking; DHCP lease obtained |

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
| `fs/` | VFS, initrd, FAT | F1 |
| `net/` | ARP, IP, ICMP, UDP, DHCP, TCP | N2 |
| `proc/` | syscalls, ELF loader, user memory | P1, P2 |
| `lib/blibc/` | formatter, strings, user crt0 | K5, P2 |
| `shell/` | command table, selftest | U1 |
| `tools/`, `tests/` | QEMU runner, host unit tests, CI | T1 |
| `docs/` | this set, kept current | all |

## 7. Immediate next step

M0, M1 and M2 are done. Next is M3: C1 (per-console ttys with a line editor), C2
(full keyboard decoding), D1 (driver model, full PCI enumeration, `lspci`), D2 (block
layer with a ramdisk from a Multiboot module, ATA PIO and MBR partitions).
