# Porting SurfOS to riscv32 (machine mode)

The x86 kernel of this repository (Multiboot, ring 0 and ring 3, paging, PC devices) runs on
SurfCPU, our RV32IM core, as a **machine-mode** kernel: one privilege level, flat physical
memory, the SurfOS kernel shell on the console, kernel tasks scheduled pre-emptively. This is
the portability inventory the brief asks for (AGENT-INSTRUCTIONS.md section 7): every x86
dependency, what replaces it, what is left out, and the plan.

| | |
|---|---|
| Build | `make -C surfcpu build-kernel` → `build/riscv32/arty_a7/{surfos.elf,surfos.payload.bin,SURFOS.BIN}` (and `qemu_virt`); `arch/riscv32/kernel.mk`; `SURFOS.BIN` packed by the fw track's `tools/surfimg.py` where the tree has it |
| Test | `make -C surfcpu test-os`: host tests of the Arty drivers' logic, then the qemu_virt kernel at -O2 and -O0 in `qemu-system-riscv32` (restricted to SurfCPU's ISA), driven over its serial console, booted as an ELF and through a SurfBoot stand-in (the handoff, FAT over the SD driver, and `SURFOS.BIN` and a `mkcard.py` card where the fw track's tools are present) (`boards/qemu_virt/test_os.py`) |
| ISA, ABI | `rv32im_zicsr_zifencei`, `ilp32`, machine mode only (ISA.md); every linked instruction checked (`arch/riscv32/isacheck.py`) |
| Boards | `boards/qemu_virt/` (QEMU's virt machine, for tests now) and `boards/arty_a7/` (SurfCPU's SoC, MEMORY-MAP.md) |
| State | boots to the kernel shell under QEMU and passes its tests; **nothing has run on the FPGA** (no board connected): every Arty result is pending |

## 1. The machine-mode model, and what it does not have

SurfOS on SurfCPU runs entirely in machine mode, the only privilege mode the core has
(ISA.md section 1). That decides what the port can and cannot be:

* **No process protection.** There is no user mode, no PMP entry and no MMU: every task,
  every driver and every line of the shell can read and write all of memory and every device
  register. The x86 kernel's ring-3 processes (separate page directories, `uvm_check()` on
  every user pointer, a faulting program killed alone) do not exist here. A stray store is
  caught only when it hits an unmapped address or the read-only boot ROM.
* **No paging.** Addresses are physical. There is no demand paging, no page fault handler, no
  `ioremap()`, no guard page under a stack. The kernel heap (`mm/kalloc.c`, unchanged) has RAM
  behind it from the start. Instead of guard pages every kernel stack has a canary word at its
  bottom, checked at every trap, and every trap checks that the interrupted `sp` is inside the
  current task's stack (`arch/riscv32/trap.c`).
* **No ring-3 programs, no surfsh.** No ELF loader, no system calls, no `libsurf`, no
  `/initrd/bin`: `init` (kernel/task.c) starts the ring-0 kernel shell (`SURF ≋#>`) on every
  console, as `shell=kernel` does on x86. `run`, `spawn` and the user programs are absent.
* **No network** (no NIC driver for SurfCPU yet; the Arty's Ethernet needs a MAC first:
  brief section 11) and therefore no telnetd and no pseudo-terminals.
* **No PC hardware**: PCI, ATA, USB (xHCI, sticks, Ethernet adapters), ACPI, the APIC, the
  8259s, the PIT, the 8042, the CMOS clock, the PC speaker, ISA DMA, the floppy, the parallel
  port, Wi-Fi, the I2C touchpad and the UEFI framebuffer. Their drivers are not built.

What machine mode keeps is everything else: the scheduler with its four classes and slices,
kernel threads, waits, sleeps, kernel timers, mutexes, semaphores, events, the kill that waits
for the end of a call, Ctrl+C, the four virtual consoles with their ttys, line editor,
history, escape sequences, UTF-8 and pager, the kernel log, the heap, the block layer with
MBR/GPT partitions and the `bdev_may_format()` rule, the block cache, the VFS, FAT12/16/32
with long names (read and write), `mkfs`, devfs and tarfs, and the kernel shell with every
command that does not need PC hardware or processes.

## 2. How it is built

`arch/riscv32/kernel.mk` builds the kernel from the repository root, separately from the x86
`Makefile`, whose wildcards (`kernel/*.c`, `driver/*.c`, ...) never reach `arch/riscv32/`,
`include/arch/riscv32/` or `boards/`. Outputs go to `build/riscv32/<board>/` only.

* **Shared files are compiled from where they are** (the list is `SHARED_C` in kernel.mk):
  `kernel/{task,wait,sync,ktimer,klog,console,tty}.c`, `mm/kalloc.c`, `lib/blibc/*` (but
  `time.c`), `fs/{vfs,bcache,fat,fat_mkfs,devfs,poll,tarfs}.c`,
  `driver/{bdev,part,parttab,cp437}.c`, `shell/{prompt,fscmd,fdisk}.c`.
* **Header shadowing.** `-Iinclude/arch/riscv32` comes before `-Iinclude`. A header there
  replaces the x86 header of the same name for the riscv32 build only: `surfos/irq.h`
  (mstatus.MIE instead of EFLAGS.IF), `surfos/trap.h` (the RV32 trap frame), `surfos/interrupt.h`
  (the handler chains without the IDT and PIC), `surfos/cpu.h` (misa, and `rdtsc()` as the cycle
  counter), `surfos/math64.h` (C division through libgcc instead of `divl`), `asm/io.h` (MMIO
  accessors; the port I/O names stop the kernel with a panic), `mm/memory.h` (the layout from the
  linker script) and `stdint.h` (for the SurfCPU contract headers). Architecture-only headers
  live in `include/arch/riscv32/riscv/` (`csr.h`, `board.h`, `arch.h`).
* **Flags**: `-march=rv32im_zicsr_zifencei -mabi=ilp32 -mcmodel=medlow`, freestanding with the
  kernel's own libc headers (`-nostdinc`, as x86), frame pointers (backtraces), `-fgnu89-inline`
  (as x86), and two riscv32 decisions: `-fsigned-char` (the shared code was written where `char`
  is signed; the RISC-V psABI's unsigned `char` matters only at interfaces with foreign code, and
  there is none) and `-mstrict-align` (SurfCPU traps misaligned loads and stores, so the compiler
  must never create one). `-DSURFOS_RISCV32` selects the riscv32 lines of the shared files.
* **libgcc**: the rv32im/ilp32 multilib is linked for 64-bit division and multiplication (the
  x86 kernel avoids libgcc with `divl`). `isacheck.py` audits the linked disassembly, libgcc's
  routines included: nothing outside rv32im_zicsr_zifencei + MRET/WFI, no compressed encoding.
* **Linking**: `arch/riscv32/linker.ld` with the board's `board.ld` (the Arty's takes every address
  from the generated `surfcpu/firmware/ld/surfcpu_soc.ld`). The entry is the first byte at
  `0x80000000`; `.text .rodata .ksyms .data` are the payload, `.bss` follows. Linked twice, as on
  x86, for the backtrace symbol table (`tools/gensyms.py`, shared); its room is fixed so linker
  relaxation cannot move code between the passes, and kernel.mk checks that it did not.
* **Packaging**: `surfos.payload.bin` (objcopy) and `SURFOS.BIN`, the 64-byte header of
  BOOT-ABI.md section 2, packed by the fw track's `surfcpu/tools/surfimg.py` (`pack --elf`),
  which puts the ELF through its `tools/isa-audit.py` and refuses to write a package SurfBoot
  would refuse; kernel.mk also requires the payload it packed to be objcopy's bytes. There is no
  other packer: on a tree without surfimg.py (`sc/os` before integration) kernel.mk builds
  `surfos.elf` and `surfos.payload.bin` and says that `SURFOS.BIN` was skipped. `SURFIMG=` (or
  `OS_SURFIMG=` for `make -C surfcpu`) names another copy, which is how the package was tested
  here, with surfimg.py taken from `surfcpu` (b555bb6) by `git show`. Card images come from the
  fw track's `tools/mkcard.py` the same way (`OS_MKCARD=`).

## 3. The inventory: every x86 dependency and what replaces it

| x86 | Where | riscv32 |
|---|---|---|
| Multiboot 1/2 entry, boot stack | `boot/boot.S` | `arch/riscv32/start.S`: BOOT-ABI.md section 4 (a0 hart, a1 boot info, a2 magic); interrupts off, `mtvec` = our own trap entry, its own `gp`, `tp` = 0, its own 16-byte aligned stack, BSS cleared, the frame-pointer chain ended |
| Multiboot information | `kernel/multiboot*.c` | `arch/riscv32/bootinfo.c`: SurfBoot's `surf_boot_info` taken only after a2, alignment, magic, version, size and CRC agree, copied before anything else runs; the kernel's own `struct boot_info` (command line, RAM) filled from it. QEMU has no SurfBoot: the board's defaults |
| Linker script at 1 MB | `linker.ld` | `arch/riscv32/linker.ld` + `boards/*/board.ld` at 0x80000000 |
| GDT, TSS, segment registers | `kernel/gdt.c` | nothing: no segmentation, no privilege change. `tss_set_kernel_stack()` is not called (kernel/task.c, guarded) |
| IDT, 256 stubs, `trap_common` | `kernel/traps.asm`, `kernel/interrupt.c` | `arch/riscv32/traps.S`: one entry (`mtvec` direct mode), a 144-byte frame aligned to 16 bytes below the interrupted `sp` with all 31 registers, mepc, mstatus, mcause, mtval; `gp` reloaded for C; `trap_dispatch()` returns the frame to resume; the exit restores mepc and mstatus, every register, `sp` last, MRET |
| Exception policy, register dump | `kernel/panic.c` | `arch/riscv32/trap.c`: the same policy (kill the faulting task with a report and a backtrace; a wipeout in the idle task, in an interrupt handler or before tasks), RISC-V causes and register names; EBREAK reported and stepped over |
| 8259 PICs, IRQ chains | `kernel/interrupt.c` | `arch/riscv32/irq.c`: the same chains (`add_irq_handler()` ...) over the board's controller: the PLIC on QEMU, `surf_intc` on the Arty (levels, no claim/complete); a source nobody handles is switched off before it storms |
| `int $0x40` = `yield()` | `kernel/task.c` | `ecall` (guarded in task.c); `trap_dispatch()` steps mepc past it, then the yield handler task.c installs (`T_YIELD` = ECALL from M) calls `schedule()` |
| First frame of a task (segments, EFLAGS) | `kernel/task.c` `build_initial_frame()` | `arch_task_frame()` (guarded in task.c): mepc = the task's start, mstatus MPP = M and MPIE = 1 (MRET enables interrupts), `sp` = the stack top, `gp` the kernel's, `ra` = `s0` = 0, the stack canary |
| Context switch by stack swapping | `kernel/task.c` `schedule()` | unchanged: the scheduler returns another task's frame, `traps.S` resumes it |
| PIT at 100 Hz, delay calibration | `kernel/timer.c` | `arch/riscv32/timer.c`: the machine timer (64-bit `mtime`/`mtimecmp`, read hi/lo/hi, written lo=~0/hi/lo), HZ 100; the tick count follows mtime so a long irq-off stretch loses no time; `udelay()` spins on mtime, no calibration |
| Local APIC, ACPI | `kernel/apic.c`, `kernel/acpi.c` | nothing |
| CPUID, CR0/CR4, PAT, MSRs | `kernel/cpu.c` | `arch/riscv32/cpu.c`: misa, mvendorid, marchid, mimpid, the counters (`cpuinfo`) |
| Paging, frames, ioremap, user address spaces | `mm/paging.c`, `mm/pmm.c`, `mm/memrange.c`, `mm/uvm.c` | nothing: flat physical memory. `mm/kalloc.c` unchanged, its arenas from the linker (`include/arch/riscv32/mm/memory.h`); `arch/riscv32/mem.c` checks the heap ends inside RAM SurfBoot verified |
| Kernel heap demand paging | `mm/paging.c` | not needed (RAM behind every heap page) |
| Port I/O (`inb`/`outb`) | `include/asm/io.h` | MMIO accessors (`mmio_read32()`, ...); the port names remain only so a dead x86 path compiles (console.c's VGA cursor) and panic if reached |
| Backtraces (EBP chain, page-table checks) | `kernel/ksym.c` | `arch/riscv32/panic.c`: the RISC-V frame (`ra` at s0-4, the caller's s0 at s0-8), frames checked against RAM; the same symbol table |
| `panic()`, `BUG()` | `kernel/panic.c` | `arch/riscv32/panic.c` |
| Console: VGA text at 0xB8000, CRTC cursor, framebuffer | `kernel/console.c`, `driver/framebuffer.c` | console.c unchanged, always on its framebuffer path (a RAM grid): `arch/riscv32/fbcon.c` hands the cells to the board's text display (the Arty's 80x30 text RAM) or to none (QEMU), so 0xB8000 and the VGA ports are never touched |
| Serial console on COM1 | `driver/serial.c` | `arch/riscv32/serial.c`: the same mirroring and input rules (`tty_term_input()`), on the board's UART (16550 on QEMU, `surf_uart` on the Arty) |
| ttys, line editor, pager | `kernel/tty.c` | unchanged |
| PS/2 keyboard via the 8042, scan code set 1 | `kernel/keyboard.c` | the Arty's `surf_ps2` delivers set 2: `boards/arty_a7/ps2set2.c` does the 8042's translation to set 1, `kbdset1.c` is keyboard.c's set-1 decoding (its tables, unchanged) without the 8042, `ps2kbd.c` the driver (section 5) |
| CMOS clock | `lib/blibc/time.c` | `arch/riscv32/time.c`: QEMU's Goldfish RTC; the Arty has no clock (kernel build time + uptime) |
| Reboot (ACPI, 0xCF9, 8042, triple fault) | `kernel/sys.c` | `board_reset()`: QEMU's reset register; on the Arty a jump to SurfBoot at the reset vector (a warm start) |
| PC speaker | `kernel/sys.c`, `shell/demos.c` | nothing (`sysbeep()` does nothing; the demos are not built) |
| `divl` 64/32 division | `lib/blibc/vsnprintf.c`, `include/surfos/math64.h` | C division through libgcc (guarded in vsnprintf.c; math64.h shadowed) |
| Driver table (PCI, ATA, USB, NICs, ...) | `driver/drivers.c` | `arch/riscv32/drivers.c`: the serial input, then the board's table |
| RAM disks from Multiboot modules | `driver/ramdisk.c` | QEMU: `boards/qemu_virt/board.c`'s rd0, the image `-device loader` puts at 0x88000000 |
| ATA and USB disks | `driver/ata.c`, `driver/usb/msc.c` | the Arty's microSD card over `surf_spi`: `boards/arty_a7/sdcard.c` (SD protocol) and `sd.c` (the block device `sd0`) |
| ELF loader, processes, system calls, kernel commands for programs | `kernel/process.c`, `kernel/syscall.c`, `mm/uvm.c` | absent (`arch/riscv32/absent.c`: `process_spawn()` says ENOSYS, a user pointer is refused) |
| Pseudo-terminals, telnetd, sockets | `kernel/pty.c`, `net/` | absent (`pty_out()` drops, nothing ever opens a pty) |
| The ring-0 shell | `shell/main.c` | `arch/riscv32/kshell.c`: the same line handling, prompt, paging, help and output for every shared command; RISC-V crash commands; the board's commands |
| Self test | `shell/selftest.c` | `arch/riscv32/rvtest.c`: the portable checks, plus the port checks below |

**Inline assembly**, every file of the x86 tree that has some, and what happens to it:
`boot/boot.S`, `kernel/traps.asm`, `kernel/gdt.c`, `kernel/interrupt.c`, `kernel/cpu.c`,
`kernel/apic.c`, `kernel/acpi.c`, `kernel/panic.c`, `kernel/ksym.c`, `kernel/sys.c`,
`kernel/timer.c`, `kernel/keyboard.c`, `kernel/process.c`, `kernel/syscall.c`,
`kernel/main.c`, `mm/paging.c`, `mm/uvm.c`, `lib/blibc/time.c`, `shell/main.c`,
`shell/demos.c`, the PC drivers and `include/asm/io.h`, `include/surfos/irq.h`,
`include/surfos/cpu.h`, `include/surfos/math64.h` are not built or are shadowed;
`kernel/task.c` (`int $0x40`) and `lib/blibc/vsnprintf.c` (`divl`) have riscv32 lines under
`#ifdef SURFOS_RISCV32`; `fs/poll.c` has only a compiler barrier (`asm volatile("" ::: "memory")`),
which is portable. The riscv32 assembly is `start.S`, `traps.S`, `rvregs.S` and the CSR
accessors of `riscv/csr.h`.

**Atomics**: rv32im has no A extension and the kernel uses none (`isacheck.py` would refuse
`lr`/`sc`/`amo*`). One hart: a critical section is interrupts off. `irq_save()` is a single
`csrrc mstatus, MIE`, so reading the old state and disabling cannot be split; `KCRIT_ENTER`
(no pre-emption) is a per-task counter as on x86.

**Integer widths and layout**: ILP32 matches i386 (int, long and pointers are 32 bits). Two
differences: RV32 aligns `long long` and `double` to 8 bytes inside structures (i386 to 4),
which changes in-memory layouts only, compiled consistently; and `char` is unsigned in the
RISC-V psABI (built with `-fsigned-char`, above). On-disk and on-the-wire structures are read
with byte accessors (`fs/fat.c`'s `le16()`/`le32()`, `driver/parttab.c`) or packed structures,
never by casting a byte pointer, so endianness (both little) and alignment are safe; the
self test checks the sizes of the boot ABI's structures.

**Floating point**: none in the compiled code (the soft-float routines of libgcc would work
under ilp32, but the speaker tune that used floats is not built).

**Shared kernel files changed** (each guarded, the x86 build unchanged):

| File | Change |
|---|---|
| `kernel/task.c` | four `#ifdef SURFOS_RISCV32` blocks: `build_initial_frame()` becomes `arch_task_frame()`; `yield()` is `ecall`; no `tss_set_kernel_stack()`; init starts the kernel shell on every console |
| `lib/blibc/vsnprintf.c` | one: the 64/32 division in C instead of `divl` |

How "unchanged" was checked (`surfcpu/docs/evidence/os/x86-unchanged.txt`): the x86 kernel
built before and after (`devenv/build.sh`, O0 and O2) has byte-identical allocated sections
(code, data, rodata, the symbol table, BSS) in `kernel/task.o`, `lib/blibc/vsnprintf.o` and
`surfos.bin` (`arch/riscv32/elfcmp.py`); only the debug information moved with the line
numbers. With the changed files, `make unittest` passed, `make test-usb` passed 315 of 315 at
O0 and at O2, and `make test-all` passed at O0 (smoke test 311 checks); its O2 smoke run failed
once (32 of 306, all following one step on the loaded host) and passed 311 when run again, with
a kernel image identical to the one built before the change.

## 4. The port checks (AGENT-INSTRUCTIONS.md section 10)

`selftest` (arch/riscv32/rvtest.c), run by `make test-os` at -O2 and -O0:

| Check | How |
|---|---|
| Traps preserve every register | `rvregs.S` fills ra, gp, tp, x5-x31 with a pattern, spins while timer interrupts arrive (at least three), or yields with ECALL every round, then dumps them all; two tasks do it at once with different patterns while the scheduler switches between them |
| Stack alignment (16 bytes) | sampled in a task, in nested calls and inside the timer interrupt; the trap frame is a multiple of 16 and is aligned whatever `sp` was |
| `sp`, `gp`, `tp` | `sp` inside its task's stack at every trap (trap.c) and in the test; `gp` the kernel's global pointer in tasks and interrupts; `tp` zero |
| ABI and structure sizes | ilp32 sizes, 8-byte alignment of `long long`, signed `char`, the boot ABI's 64- and 256-byte structures, the trap frame against traps.S |
| Runtime helper ISA | `isacheck.py` on every link (libgcc included); QEMU runs a CPU restricted to SurfCPU's ISA (`misa` 0x40001100), where anything else would trap |
| BSS and data initialization | start.S clears the BSS; the kernel runs on initialized data (any wrong value shows) |
| Allocator limits | heap churn with patterns and a heap walk (`heaptest`, `heap`); `run_memcheck()` |
| Timer rollover | the tick difference across the 32-bit wrap; mtime against the tick count; 64-bit mtime read hi/lo/hi |
| Task switching under load | 32 tasks created and collected, exit codes, two sleepers, a mutex between two tasks, a semaphore ring, an event with a timeout, the two-task register test |
| 64-bit arithmetic | division, remainder, 32x32 multiply, the formatter's 64-bit numbers |

The timing windows of the self test (sleeps, kernel timers, timeouts) allow a few late ticks,
since an emulator on a busy host can stall for a while and the tick then catches up with mtime
at once.

The rest of `make test-os` (`boards/qemu_virt/test_os.py`; 68 checks per build on a tree without
the fw track's tools, 82 with them): the banner and identity, help, ps (idle, init, heartbeat, a
kernel shell on each of F1-F4), the tick against a 1000 ms sleep, the heartbeat task, two kernel
tasks that count, sleep and wake (`tasks`), interrupt counts, the CPU, the self test and
heaptest; FAT32 on the RAM disk: long, accented and 8.3 names, three folders deep, a 1 MB file
read back by CRC, `/USERS/DOCUMENTS/` (below), write, mkdir, a 1 MB copy under a long name, mv,
rm, sync, umount, then the image saved through QEMU's monitor and checked on the host (fsck.fat
clean, mtools reads back what SurfOS wrote); faults that kill the shell with a backtrace and
init restarting it, EBREAK stepped over, RISC-V's division by zero; the boot ABI's handoff: a
stand-in for SurfBoot's last step (`boards/qemu_virt/stub/`) enters the kernel with a0/a1/a2 as
BOOT-ABI.md says, its boot information's command line taking effect, and a corrupted boot
information block refused; FAT32 over the SD driver: with the kernel word `sdsim`
(`boards/qemu_virt/sdsim.c`) the RAM disk becomes `sd0` through the Arty's own `sdcard.c` and
the simulated card of its host test, so the partition code, the block cache (runs of up to 128
blocks: CMD18, CMD25), the VFS and fs/fat.c run over the real SD code path under the kernel; a
1 MB file read and copied, files written, `/USERS/DOCUMENTS` made, the image checked by fsck.fat
and mtools. The handoff and sdsim boots put `surfos.elf` in place with QEMU's loader, so they
need no packer. Where the fw track's tools are present: `SURFOS.BIN` checked with `surfimg.py
verify` and booted by the stand-in, which copies its payload to the load address; and a card
image made by `mkcard.py` (MBR, FAT32, the standard directories of BOOT-ABI.md section 5, the
package in `/BOOT`) mounted through the SD driver, `/USERS/DOCUMENTS` read and written, the
image read back by `mkcard.py --inspect`, mtools and fsck.fat. Without them those two parts are
reported as skipped.

**Long names and `/USERS/DOCUMENTS/`.** "DOCUMENTS" has nine letters, so on FAT it exists only as
a long-name (VFAT) entry with a short alias such as `DOCUME~1`, as the Mac makes it. fs/fat.c
(unchanged) reads and writes long names: a lookup compares the long name when an entry has a
valid one (its checksum matching the short entry), folding ASCII case, and a name that is not
8.3 is created with its long-name entries and a `~N` alias. The tests show both directions: the
directory made by mtools (and by mkcard.py) is found as `DOCUMENTS` (also as `documents`), read
and written; `mkdir /sd0p1/USERS/DOCUMENTS` through the SD driver leaves entries that mtools
lists as `DOCUME~1` with the long name `DOCUMENTS`, and fsck.fat finds the volume clean. A
lookup by the alias itself (`DOCUME~1`) is not supported; SurfBoot, which reads 8.3 names only,
never needs this directory.

`make test-os-host` (`boards/arty_a7/tests/`, seeds 1, 7, 123): the Arty drivers' logic on the
host, from the same sources: the text screen (present, hardware scroll across the wrap, random
changes), the keyboard (every character typed through set 2, modifiers, locks, the E0 keys,
F1-F4, Ctrl+Alt+Del, Pause, Print Screen, the keyboard's replies, line errors, raw mode), the SD
helpers against the specification's examples (CRC7 of CMD0/CMD8/..., CRC16 0x7FA1, CSD 1.0 and
2.0, CID), and the SD driver against a simulated card (`sdmodel.c`, its own CRCs) of three kinds
(SDHC, SDSC version 2, SDSC version 1): 300 random reads and writes of 1-64 blocks, a corrupted
block read again, a block that stays corrupted, a write rejected for its CRC and sent again, a
read past the end, a data token that never comes, no card. The model behaves as the io
track's card model (`surfcpu/sim/models/sdcard_model`, merged into `surfcpu`, documented in
IO.md): data responses 0xE5/0xEB/0xED followed by busy, R1 0x40 past the end, commands sent
while the card is busy ignored; it counts the host's mistakes (a command while busy, chip select
released inside a block) and the tests require none; and a card that refuses CMD18 and CMD25
(the driver goes on one block per command). 177 checks over the three seeds.

`make test-os-iomodel` (`boards/arty_a7/tests/iomodel/`): the same `sdcard.c` against the io
track's own bit-level card model (`sim/models/sdcard_model.{h,cpp}`), through a software SPI
master with surf_spi's timing (390.625 kHz for identification, 12.5 MHz after; a millisecond is
25,000 clock cycles): three kinds of card per seed, 200 random reads and writes checked against a
shadow copy and the card's backdoor, every fault the model can inject (a bad read CRC, a write
rejected for its CRC, a read error token, no response, a write error, a slow card, an
initialization that never ends, no card), and the model's count of protocol mistakes, which must
stay zero: 101 checks over seeds 1, 7 and 123. It runs where the model is in the tree (on
`surfcpu` since the io merge; on `sc/os` from a copy, `OS_IOMODEL_DIR`), and says it is skipped
otherwise.

`make test-os-surfemu` (`boards/arty_a7/tests/surfemu_test.py`): the Arty kernel itself, in the
fw track's firmware emulator (`firmware/tests/surfemu.c`, an instruction-level model of the SoC:
the core of ISA.md, the memory map, uart at its baud rate, timer, intc, gpio, spi with a simulated
card, ps2, video, sysinfo), booted by the real SurfBoot ROM from a `mkcard.py` card holding
`/BOOT/SURFOS.BIN`, `/BOOT/CMDLINE.TXT` and `/USERS/DOCUMENTS/HELLO.TXT`: SurfBoot's checks and
handoff, the kernel's console on surf_uart, SurfBoot's boot information taken, the card through
surf_spi with FAT32 on /sd0p1, `/USERS/DOCUMENTS` by its long name, the package read back from
the card by CRC, two tasks, the drivers, the self test, and surf_video's text RAM showing the
console with hardware scroll: 15 checks, about 120 s of simulated time in 2 s. It needs the
emulator, surfimg.py and mkcard.py, so on `sc/os` it says it is skipped; it was run on a trial
of the branch merged with `surfcpu` (2dd26c9, after the fw, ddr and flow merges), a tree made by `git merge-tree` and unpacked
under `build/` with nothing merged or committed (evidence in `merged-trial/`). The emulator is
a test aid, not the RTL and not the board. Its card has no CMD18 or CMD25 (SurfBoot reads with
CMD17 only), which is why the SD driver learned to fall back to one block per command.

## 5. The boards

**qemu_virt** (`boards/qemu_virt/`): QEMU's generic virt machine, entered with `-bios none` at
0x80000000: its 16550 UART (PLIC source 10), the CLINT (10 MHz), the PLIC (context 0), the
Goldfish RTC, the sifive_test device (`poweroff`, `reboot`) and a RAM disk: the harness loads a
FAT32 image (tools/mkimage.py's "foreign" stick) at 0x88000000 with `-device loader`; its size is
read from its partition table. It is a separate HAL so the portable kernel can be tested now; it
models none of SurfCPU's peripherals and says nothing about the FPGA.

**arty_a7** (`boards/arty_a7/`): SurfCPU's SoC, MEMORY-MAP.md, every address and bit from the
generated `soc.h`:

* `board.c`: `surf_uart` (the console; receive interrupt), `surf_timer` (timebase from its
  register), `surf_intc` (levels, no claim/complete), `surf_gpio` (LD4 blinks with the heartbeat
  task; `leds`), `surf_sysinfo` (`bootinfo`), the reset as a jump to SurfBoot at the reset vector.
* `textcon.c` + `textscreen.c`: the 80x30 text display as the console's screen: VGA cells to
  32-bit cells (glyph, foreground, background), the VGA palette as RGB444, the hardware cursor,
  and hardware scroll (a present that finds the picture moved up one row moves SCROLL on and
  rewrites one row: the new bottom row first, then SCROLL, which surf_video takes once per
  frame, as IO.md describes). Text mode replaces the reset test pattern at start.
* `ps2kbd.c` + `ps2set2.c` + `kbdset1.c`: the PS/2 keyboard (interrupt source 1): set 2
  translated to set 1, decoded as kernel/keyboard.c does, to the active tty, F1-F4 switching
  consoles, Ctrl+Alt+Del rebooting; line errors forget half-received sequences; the lock keys'
  LEDs set through surf_ps2's host-to-device path (0xED, the value, each ACKed, a state machine
  driven by the replies, 100 ms timeout); a keyboard plugged in gets its LEDs again (`kbdinfo`).
* `sd.c` + `sdcard.c` + `sdproto.c`: the microSD card through `surf_spi` as the block device
  `sd0`, its partitions `sd0p1`...: initialization at 390 kHz (CMD0, CMD8, CMD59 CRC on, ACMD41,
  CMD58, CMD16 for SDSC, CSD, CID), then 12.5 MHz; SDHC/SDXC block and SDSC byte addressing;
  single and multiple block reads with every CRC16 checked and a bad block read again; single
  and multiple block writes with data responses (the low five bits), busy waits, the stop
  token also after an error, and CMD13; a wait for 0xFF before every command (a card ignores
  commands while busy); every wait bounded; one block per command from then on if a card
  refuses CMD18 or CMD25 as illegal (every SD card has them; the fw emulator's does not). FAT
  through the existing block layer, cache, VFS and fs/fat.c: init_fs() mounts the card's FAT32
  partition on /sd0p1, long names included (`/sd0p1/USERS/DOCUMENTS`). Read-only until writing has been tried on the real board (the brief's
  "read-only first"): `sd=rw` in /BOOT/CMDLINE.TXT, or `sdmode rw` while unmounted, enables it;
  `eject sd0` flushes and unmounts. Never partitioned or formatted (bdev_may_format() refuses
  its bus). `sdinfo` shows the card.

## 6. What is done, pending and later

Done and tested under QEMU (qemu_virt, emulation only): the build, the arch core, the
machine-mode kernel with its shell, tasks, timer and interrupts, the self test with the port
checks, FAT read/write on a RAM disk checked from the host, the boot ABI's handoff, FAT over the
SD protocol driver and a simulated card, `/USERS/DOCUMENTS` by its long name both ways, and,
with copies of the fw track's surfimg.py and mkcard.py, the kernel package and a card image
laid out as SurfBoot expects. Done and tested in the fw track's firmware emulator, on a trial
merge with `surfcpu`: the Arty kernel booted by SurfBoot from a card, to the shell and through
its self test. Done and tested on the host: the
Arty drivers' logic (display, keyboard, SD protocol and driver against a simulated card), and
the SD driver against the io track's card model with surf_spi's timing.

**Pending (needs the board, a bitstream build host, or integration):** everything on the Arty
itself. The Arty HAL (`board.c`, `textcon.c`, `ps2kbd.c`, `sd.c`) compiles and links into
`build/riscv32/arty_a7/surfos.elf` (and `SURFOS.BIN` with the fw track's packer, tested with a
copy). Its register accesses have run only in the fw track's instruction-level emulator, in a
trial merge (`make test-os-surfemu`: surf_uart, surf_timer, surf_intc, surf_gpio, surf_spi with
the emulator's read-only card, surf_video's text RAM; surf_ps2 only idle, with no keyboard): not
in RTL simulation (the SoC, its peripherals and SurfBoot are other tracks' work, joined at
integration: brief milestone 7's "real board boots SurfOS from microSD" and milestone 8's
persistent files are open), not on the FPGA (no board connected). The SD write path in
particular stays off (read-only) until it is tried on a real card. On `sc/os` itself
`SURFOS.BIN` is not built (no surfimg.py in this branch); it is, and the package and card parts
of test-os run, once the branch meets the fw track's tools at integration.

Later (brief section 11), each a design of its own: a user mode (U mode with PMP, or S mode
with Sv32) and with it protected processes, the ELF loader for `EM_RISCV`, system calls by
ECALL from U mode, `libsurf` and `surfsh` rebuilt for RV32; the Ethernet MAC and the network
stack; a graphics mode.

## 7. Notes on the contracts

Found while building to them; nothing was changed in the frozen files.

* `firmware/ld/surfcpu_soc.ld` holds both the symbols and a `MEMORY` block. A kernel script that
  INCLUDEs it for the symbols gets the regions too, and GNU ld then puts sections without an
  explicit region into the first one that fits (`.text` into `bootrom`). The kernel names its
  region (`REGION_ALIAS("kernel_ram", ddr)` in boards/arty_a7/board.ld). Proposal: the generator
  writes the symbols and the `MEMORY` block to two files.
* `firmware/include/surfcpu/bootabi.h` includes `<stdint.h>`, which a freestanding kernel built
  with `-nostdinc` must provide (include/arch/riscv32/stdint.h does).
* There is no way for software to reset the SoC (no reset register): `reboot` jumps to the reset
  vector, a warm start in which SurfBoot runs again but the peripherals keep their state until
  SurfBoot sets them up. Proposal for a later map version: a reset register in sysinfo.
* The Arty has no real-time clock: FAT time stamps on the card are the kernel's build time plus
  the uptime. A clock (or a time set over the UART or the network) is for later.
* BOOT-ABI.md's clarifications (on `surfcpu`, version 1 unchanged) make `ddr_top` the top 16 MiB
  of the DDR that calibrated. The kernel's heap ends at the map's `ddr_top` (`0x8F000000`), which
  is right for the Arty's 256 MiB; with less DDR, `run_memcheck()` stops the boot with a report
  (the heap would not lie inside `ram_size`) instead of using memory that is not there. Taking
  the heap's end from the boot information is a small change for when a smaller part matters.
* The fw track's simulated SD card (its emulator's) has no CMD18 or CMD25; real cards all do.
  The kernel copes (one block per command), but a run in that emulator says nothing about the
  driver's multiple-block paths, which the io track's card model and the host model test.
