# Porting SurfOS to riscv32 (machine mode)

The x86 kernel of this repository (Multiboot, ring 0 and ring 3, paging, PC devices) runs on
SurfCPU, our RV32IM core, as a **machine-mode** kernel: one privilege level, flat physical
memory, the SurfOS kernel shell on the console, kernel tasks scheduled pre-emptively. This is
the portability inventory the brief asks for (AGENT-INSTRUCTIONS.md section 7): every x86
dependency, what replaces it, what is left out, and the plan.

| | |
|---|---|
| Build | `make -C surfcpu build-kernel` → `build/riscv32/arty_a7/{surfos.elf,surfos.payload.bin,SURFOS.BIN}` (and `qemu_virt`); `arch/riscv32/kernel.mk`; `SURFOS.BIN` packed by the fw track's `tools/surfimg.py` where the tree has it |
| Test | `make -C surfcpu test-os`: host tests of the Arty drivers' logic, then the qemu_virt kernel at -O2 and -O0 in `qemu-system-riscv32` (restricted to SurfCPU's ISA), driven over its serial console, booted as an ELF and through a SurfBoot stand-in (the handoff, FAT over the SD driver with injected write faults and the card taken out, and `SURFOS.BIN` and a `mkcard.py` card where the fw track's tools are present) (`boards/qemu_virt/test_os.py`); `make -C surfcpu test-os-persist`: files written on the RTL system in surfsim, ejected, and read back after a second boot; `make -C surfcpu test-os-faults`: write faults, the card taken out and put back, and a full card on the RTL system (section 6) |
| ISA, ABI | `rv32im_zicsr_zifencei`, `ilp32`, machine mode only (ISA.md); every linked instruction checked (`arch/riscv32/isacheck.py`) |
| Boards | `boards/qemu_virt/` (QEMU's virt machine, for tests now) and `boards/arty_a7/` (SurfCPU's SoC, MEMORY-MAP.md) |
| State | boots to the kernel shell under QEMU, in the fw emulator and on the RTL system in simulation, and passes its tests; files written on the card persist across a reboot in the RTL simulation; **on the owner's Arty A7-100T** SurfBoot boots it from the microSD card into DDR: its shell over the UART, `uptime`, `ps`, `memstat`, the two-task test, the card mounted read-only and read ([evidence](evidence/physical/surfos-boot-2026-10-16.txt)); its VGA console, the PS/2 keyboard and writing to a real card are not yet shown on the board |

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
  `time.c`, and `memory.c`, which `arch/riscv32/memory.c` replaces with word moves),
  `fs/{vfs,bcache,fat,fat_mkfs,devfs,poll,tarfs}.c`,
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
  other packer: on a tree without surfimg.py kernel.mk builds `surfos.elf` and
  `surfos.payload.bin` and says that `SURFOS.BIN` was skipped. `SURFIMG=` (or `OS_SURFIMG=` for
  `make -C surfcpu`) names another copy. Card images come from the fw track's `tools/mkcard.py`
  the same way (`OS_MKCARD=`).

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

**Shared kernel files changed.** First, for the port, each change guarded and the x86 build
unchanged:

| File | Change |
|---|---|
| `kernel/task.c` | four `#ifdef SURFOS_RISCV32` blocks: `build_initial_frame()` becomes `arch_task_frame()`; `yield()` is `ecall`; no `tss_set_kernel_stack()`; init starts the kernel shell on every console |
| `lib/blibc/vsnprintf.c` | one: the 64/32 division in C instead of `divl` |
| `mm/kalloc.c` | one (10/16/26): on riscv32 the kernel heap's bounds are set by `heap_init()`, since its end comes from the boot information at run time |

Then a fix for both kernels (10/16/26), audit **C10** in `docs/CODE-AUDIT.md`: the console race
that the RTL system boot and the fw emulator found (the screen showed blank, half-written and
doubled rows during `tasks`; on x86 the same race was there, but a scroll takes microseconds):

| File | Change |
|---|---|
| `kernel/console.c` | the console lock: one writer at a time on the grids, the screen and the serial mirror. A task that may wait does so on a wait queue, with interrupts on (a scroll is long on SurfCPU, and the UART must be heard); it counts as holding a mutex meanwhile, and a paged task gives the lock back while it waits at the question. A whole `kprintf()` call is drawn as one (`console_print_begin()`). Whoever may not wait (an interrupt handler, a trap, a task with interrupts off or in KCRIT_ENTER) draws at once when nobody draws; otherwise its character goes to the serial line at once and to the grid when the holder lets go. A task killed holding the lock gives it back; a panic draws over everything. `sscroll()` and the screen copy move words instead of bytes |
| `lib/blibc/printf.c` | a `printf()` call drawn as one, as `kprintf()` |
| `include/surfos/console.h` | `console_print_begin()`, `console_print_end()` |
| `tests/host/test_pager.c`, `pager_bridge.c` | three scenarios with a hook that stops a task inside its line (a preemption) or prints as an interrupt handler: the other task's line waits, the handler's line comes after, a paged line gives the lock back at the question (the old console fails the first two) |

Then fixes for both kernels (10/16/26) that milestone 8's fault tests found (section 6), audit
**B1, B2, B3** in `docs/CODE-AUDIT.md`:

| File | Change |
|---|---|
| `fs/bcache.c`, `include/fs/bcache.h` | B1: a block whose write failed stays dirty (`wfail`) while its device is there, instead of being dropped when it had to make room (the command that wrote never heard of it, and a dropped FAT or directory block let FAT read its old copy back); the buffer search passes over it, one failed run is tried again when nothing else is free, and else the caller gets no buffer (an I/O error); `bsync()` tries each run once per call and fails when one did; the console hears of a device's first failure and of its first good write after. B3: a write order (`bdirty_order()`, `BORDER_DATA` < `TABLE` < `DIR` < `SUMMARY`): a run is of one order, `bsync()` writes a device order by order and stops it at the order that fails, and a block written to make room has every dirty block of a lower order of its device written first |
| `fs/fat.c` | B3: the FAT copies are written as tables, directory entries as `DIR`, FSInfo last, file data and a new directory's cluster first; unlink, truncate to zero and rename put an order point (a sync of the volume) between the entry and the clusters, so an entry leaves the disk before its clusters are freed and a new name is on it before the old one goes |
| `shell/fscmd.c` | B2: `eject` writes each mount out first and keeps them all when that fails ("stays mounted: sync tries the writes again; umount gives them up") instead of unmounting, which dropped the blocks it could not write, and saying "safe to remove" |

And (10/16/26, the full volume of section 6), audit **B4**:

| File | Change |
|---|---|
| `fs/fat.c` | B4: a run of blocks ends where the volume does (a write that does not fit puts in what fits and returns the bytes it wrote, the next one gets ENOSPC), and after a write that stopped early (no space, a block refused) the chain is cut back to the file's size; before, the clusters allocated ahead of the data stayed in the chain past it (fsck.fat: "cluster chain length is > size", the free count low) |
| `shell/main.c` (and the riscv32 `kshell.c`) | `cp` says why a write came up short ("No space left on device") instead of "short write" |

How the first set was shown unchanged (`surfcpu/docs/evidence/os/x86.txt`): the x86 kernel
built before and after (`devenv/build.sh`, O0 and O2) has byte-identical allocated sections
(code, data, rodata, the symbol table, BSS) in `kernel/task.o`, `lib/blibc/vsnprintf.o` and
`surfos.bin` (`arch/riscv32/elfcmp.py`); only the debug information moved with the line
numbers. With the changed files, `make unittest` passed, `make test-usb` passed 315 of 315 at
O0 and at O2, and `make test-all` passed at O0 (smoke test 311 checks); its O2 smoke run failed
once (32 of 306, all following one step on the loaded host) and passed 311 when run again, with
a kernel image identical to the one built before the change. The console fix changes the x86
kernel on purpose; it passed the same suites (`x86.txt`, round 2), and so did B1-B3 (round 3).

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
at once. The two-task register test runs until both tasks were switched out twice, for at most
300 ticks (it was 400 rounds, which an idle host sometimes finished within two time slices).
`test_memory` checks `arch/riscv32/memory.c` (memcpy, memmove both ways, memset) at every
alignment, offset and length up to 70 bytes against byte loops.

`scrolltest` (arch/riscv32/kshell.c, audit C10): two kernel tasks print 60 numbered lines each
at once, as fast as they can, while the console scrolls; then every row of the console's cells
(and of the screen's copy) must be one whole line, each task's lines numbered on by one to its
last. `make test-os` runs it on QEMU slowed with `-icount shift=7` (about SurfCPU's pace, so a
tick often lands inside a scroll) and in the fw emulator, where the text screen is checked too.
The console before the lock fails it in both (rows half written, doubled or blank); with the
lock it passes, and the scroll is faster (892 ticks instead of 1430 for the 120 lines in the
emulator).

The rest of `make test-os` (`boards/qemu_virt/test_os.py`; 118 checks per build on a tree without
the fw track's tools, 132 with them): the banner and identity, help, ps (idle, init, heartbeat, a
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
reported as skipped. Then milestone 8's faults on the simulated card (37 checks) and a full
volume (9), section 6.

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
read past the end, a data token that never comes, no card; and (10/16/26) a write error on the
first of eight blocks, a write command without an answer, and the card taken out after the third
of sixteen blocks and put back. The model behaves as the io
track's card model (`surfcpu/sim/models/sdcard_model`, merged into `surfcpu`, documented in
IO.md): data responses 0xE5/0xEB/0xED followed by busy, R1 0x40 past the end, commands sent
while the card is busy ignored; it counts the host's mistakes (a command while busy, chip select
released inside a block) and the tests require none; and a card that refuses CMD18 and CMD25
(the driver goes on one block per command). 204 checks over the three seeds.

`make test-os-iomodel` (`boards/arty_a7/tests/iomodel/`): the same `sdcard.c` against the io
track's own bit-level card model (`sim/models/sdcard_model.{h,cpp}`), through a software SPI
master with surf_spi's timing (390.625 kHz for identification, 12.5 MHz after; a millisecond is
25,000 clock cycles): three kinds of card per seed, 200 random reads and writes checked against a
shadow copy and the card's backdoor, every fault the model can inject (a bad read CRC, a write
rejected for its CRC, a read error token, no response, a write error, a slow card, an
initialization that never ends, no card), and the model's count of protocol mistakes, which must
stay zero; and (10/16/26) a write error on the first block of a multiple write, a write command
without a response, and the card taken out in the middle of a 16-block write (by `set_present()`
after a number of bytes), then put back and initialized again: 164 checks over seeds 1, 7 and
123. The first of these found a driver mistake the C model had let pass: after a block the card
refused, the stop token went into the card's busy time (fixed: the driver waits for it, as after
a block it took). It runs where the model is in the tree (on
`surfcpu` since the io merge; `OS_IOMODEL_DIR` names another copy), and says it is skipped
otherwise.

`make test-os-surfemu` (`boards/arty_a7/tests/surfemu_test.py`): the Arty kernel itself, in the
fw track's firmware emulator (`firmware/tests/surfemu.c`, an instruction-level model of the SoC:
the core of ISA.md, the memory map, uart at its baud rate, timer, intc, gpio, spi with a simulated
card, ps2, video, sysinfo), booted by the real SurfBoot ROM from a `mkcard.py` card holding
`/BOOT/SURFOS.BIN`, `/BOOT/CMDLINE.TXT` and `/USERS/DOCUMENTS/HELLO.TXT`: SurfBoot's checks and
handoff, the kernel's console on surf_uart, SurfBoot's boot information taken, the card through
surf_spi with FAT32 on /sd0p1, `/USERS/DOCUMENTS` by its long name, the package read back from
the card by CRC, two tasks, the drivers, the self test, `scrolltest`, the heap's end from the
boot information, and surf_video's text RAM showing the console with hardware scroll and
`scrolltest`'s rows whole: 18 checks, about 120 s of simulated time in 2 s. It needs the
emulator, surfimg.py and mkcard.py, which `surfcpu` has since the fw merge. The emulator is
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
  frame, as IO.md describes). Text mode replaces the reset test pattern at start. The console
  lock calls it one caller at a time, so interrupts stay on while it compares and writes a
  screen (milliseconds on SurfCPU).
* `ps2kbd.c` + `ps2set2.c` + `kbdset1.c`: the PS/2 keyboard (interrupt source 1): set 2
  translated to set 1, decoded as kernel/keyboard.c does, to the active tty, F1-F4 switching
  consoles, Ctrl+Alt+Del rebooting; line errors forget half-received sequences; the lock keys'
  LEDs set through surf_ps2's host-to-device path (0xED, the value, each ACKed, a state machine
  driven by the replies, 100 ms timeout); a keyboard plugged in gets its LEDs again (`kbdinfo`).
* `sd.c` + `sdblk.c` + `sdcard.c` + `sdproto.c`: the microSD card through `surf_spi` as the block
  device `sd0`, its partitions `sd0p1`... (`sdblk.c`, shared with QEMU's sdsim: the block device
  and a task that looks at card detect five times a second, surf_spi having no interrupt for
  it; section 6): initialization at 390 kHz (CMD0, CMD8, CMD59 CRC on, ACMD41,
  CMD58, CMD16 for SDSC, CSD, CID), then 12.5 MHz; SDHC/SDXC block and SDSC byte addressing;
  single and multiple block reads with every CRC16 checked and a bad block read again; single
  and multiple block writes with data responses (the low five bits), busy waits, the stop
  token also after an error (after the card's busy time), and CMD13; card detect looked at
  before every transfer; a data block moved in a tight loop (`spi_rx`, `spi_tx`); a wait for
  0xFF before every command (a card ignores
  commands while busy); every wait bounded; one block per command from then on if a card
  refuses CMD18 or CMD25 as illegal (every SD card has them; the fw emulator's does not). FAT
  through the existing block layer, cache, VFS and fs/fat.c: init_fs() mounts the card's FAT32
  partition on /sd0p1, long names included (`/sd0p1/USERS/DOCUMENTS`). Read-only unless
  allowed (the brief's "read-only first"; writing is tested in simulation, not yet on a real
  card): `sd=rw` in /BOOT/CMDLINE.TXT, or `sdmode rw` while unmounted, enables it; `eject sd0`
  flushes and unmounts. Never partitioned or formatted (bdev_may_format() refuses its bus).
  `sdinfo` shows the card and how often it was taken out and put in.

## 6. Writing to the card (milestone 8)

The brief's milestone 8 is on-device writes with flush, unmount and eject, useful errors and
persistence across a reboot. All of it is tested in simulation; **none of it on a real card or
the board** (pending).

**What a command does.** The kernel shell follows every command that changes a file system
(`write`, `append`, `cp`, `mv`, `rm`, `mkdir`, `rmdir`, `fsbench`) with a sync of each mounted
volume that can be written, one at a time (`vfs_fsync()` on its root), so at the prompt its work
is on the card, and a write the card refused is that command's failure: `cp: not all of it
reached /sd0p1: I/O error`, status 1. A volume whose disk was taken out before the command
began is left out (that was said when it went); one taken out during the command is a failure.
`append <file> <text>...` exists because the shell has no redirection. A program (there are none
on riscv32 yet) learns of a failed write from `vfs_fsync()` and, since audit B10, from the last
`vfs_close()` of a file it wrote (it returned 0 before, whatever its sync found); `umount` and
`eject` say how many changed blocks they gave up because they could not be written.

**Errors** (block cache B1, eject B2): a block the card refuses stays dirty and is tried again by
the next sync; the console says `bcache: write of sd0p1 blocks ... failed (-3); kept for the
next sync` once, and `bcache: sd0p1 takes writes again` when none of its blocks waits after a
failure any more. Since audit B6 a card that refuses keeps at most a quarter of the cache (256
blocks) of such blocks: its dirty blocks are held back (making room sends it no write bound to
fail, and a block held back because one below it failed is not marked failed), past that share
its new writes fail at once with EIO (`bcache_room()`, `bget()`), and one refusing card leaves
the rest of the cache to the other devices (before, a card that refused everything filled the
cache, and then every read of every device failed; there is no share across devices). Since
B14 the card is asked again at each such call, with one write of its oldest block: once it
takes writes, they work at once. What the share counts (B15): file data (each block through the
cache asks first, and a write straight to the card changes no cached copy before the card has
the bytes; B22: a refused request may still have stored its first blocks on the card, as
`sd_write()`'s CMD25 does, so the cache drops its clean copies of it and a file reads those blocks
as the card holds them, but for dirty copies, which keep their bytes for the next sync to write
over the card's); an operation's own FAT, directory and FSInfo blocks come on top. A sync tries every
block it may, in every sync (B13: blocks that never failed first, a refused run again block by
block; a card that refused 4 requests in one sync and took none is asked nothing more in it,
B26: counted by each sync for itself), so one bad block does not hold back its neighbours. A
sync returns 0 only when every block that was dirty when it began reached the card (B12; before,
after 8 rounds of another task's writing it returned 0 with a directory block still dirty), and
gives up on such a block once syncs have waited 10 s for its hold (B18: a task killed with
buffers in hand left every later sync hanging; B39: a kill or Ctrl+C cuts a sync's long waits short,
and a task inside a FAT call, holding the cache's lock, or with blocks of its own marked in the cache
is not killed at once (`fs_hold`, `bcache_holds()`): the kill that waited 2 s comes once it is out,
never in the middle, where it left a copy's record on the dead task's stack and blocks marked as
copied, written or read for good; B47: nor inside a device request, also one outside the cache, where
a killed `mkfs` left `sdblk.c`'s lock held and the card hung, and `mkfs` stops before its next request
and says how far it got; B48: a kernel thread killed in there that enters a call gets the call's 2 s;
B49: a long read or write stops before its next piece, a block through the cache, up to 64 KiB
straight; B46: but `umount`, and eject's unmount, are not cut short at all: once begun they finish,
and give nothing up that the card takes). Nothing hangs (a 1 MB `cp` returns at once with
its error). `eject` keeps a volume it could not write. A `mv`, `rm` or truncate to zero whose
order point (below) the card refuses puts back what it changed in the cache and fails with EIO
(audit B5): nothing changed, and the same command later is safe (before, a refused `mv` left
both names on one chain, and the next `mv` or `rm` freed the chain under the other name). Since
B16 the refused blocks of the clusters such a command frees do not hold its order point back
(`rm` of the file whose block refuses works, B21: however many refuse, B28: all its dirty blocks,
refused or never tried, set aside for that order point only, B32; other refused blocks still do:
an operation that needs an order point fails while they refuse, and B20: so does a `mv` onto
that file, whose first order point must not write the replaced entry ahead of them; B26: another
task's sync waits for them while they are set aside, B33: but not one writing a block from a copy that
needs one: it lets that block go and looks again; B34: and a copy is written only when nothing
it needed was set aside or dropped since it was taken, else it is taken again; B41: spoiled only by a
block not set aside before it was taken, and only when its own block changed since, taken again as
long as it takes and ended when another task's write paid the sync's debt, so rm traffic alone no
longer fails a sync; B40: also when umount gives a block up, `binvalidate()`). Since B28 the
bytes nobody will read (a part cut off past the end of a write that stopped, a gap of zeros
given up, B36: a folder's new cluster given back) are dropped, never written, and hold no order
point, table point (B37) or sync back while their clusters wait to be freed (B35: one another
task holds or is writing then is dropped when let go or when that write ends; only a block of an
rm's own file that cannot be set aside then is written as any other, and an `rm` it fails in can
say EIO once). Since B17 a `mv` that fails at any step changes nothing, none failing once the
replaced file is freed. Since B24 an undo that cannot be done (a block it changed cannot be read
back) makes the volume refuse every change (EROFS) until it is unmounted, says so, and leaves it
marked as not clean (B27, B29: an undo puts back only what it saved, and only a change that was
made). A write that fails leaves the file's size as it was (B23: a gap of zeros past the end
included). Clusters given back while the card refused (B25: a cut-back's; B30: a failed mkdir's
new folder, once something of its name was written) wait to be freed until a sync that began
after them works; 64 such chains can wait (B31), one more is said lost on the console and again
at unmount (fsck.fat frees it), and the volume stays marked as not cleanly unmounted (B38; B42: also
through SurfOS's own next mount and unmount, until a checker clears it; B43: clusters a free could
not give back, the FAT unreadable in the middle of the chain, are counted and said likewise, and the
rm whose name is off the card says 0; B44: FAT12 has no such mark, and its unmount says so).

**The card taken out** (`sdblk.c`): `sdcard.c` looks at card detect before every transfer and
fails at once (`SD_ENOCARD`, no timeout); the watch task (two looks 200 ms apart agreeing, so a
bouncing switch is not a removal) unregisters the disk and its partitions (`bdev_unregister()`:
every I/O fails, the cache drops their blocks, a mount stays until umount), marks the driver not
ready, and says `+SD: sd0: the card was taken out` and, per mount, `+SD: /sd0p1 was mounted
read-write: what was not yet written to the card is lost; umount /sd0p1`. A card put in is
initialized and registered as `sd0` again (as `sd1` while a mount of the old `sd0` is left), its
partitions read, not mounted: `+SD: sd0: a card was put in: mount sd0p1 to use it`. A transfer
begun for the old disk never reaches the new card (it must be for the disk the card has now). A
card taken out and put back between two looks is not noticed: it lost its power, answers
nothing, and every transfer fails until it is out for a moment.

**The write order** (B3) is what keeps an interrupted write repairable: the block cache writes a
device's dirty blocks file data first, then the FAT copies, then directory entries, then FSInfo,
and a block written to make room has the dirty blocks of a lower order it may point at written
first (B12: those with a change older than its own last change); FAT puts
an order point (a sync) in unlink, truncate to zero and rename so that taking away goes the
other way round (entry first). Before B3, a card pulled after the first block of a new file's
sync left its directory entry pointing at a free cluster (`fsck.fat`: "Contains a free
cluster"), and after the third the FAT and entry written without the file's bytes (a file
reading whatever its cluster held before, which no check can see). Since audit B7 the order is
checked where each write is issued (`write_run()`: no block of order k while its device has a
dirty block below k; since B12, below k with a change older than the block's last one: each
change is numbered, and each device lists its dirty blocks of each order by their first
unwritten change), so it also holds when another task writes during a sync or while room is
made; a block is not written while someone holds it, nor handed out while it is written (B12's
copies, for a block other tasks keep changing, write what was checked). Within
the FAT, a link into a new cluster in another FAT sector (and every link that grows a folder)
waits for the cluster's own entry to be on the card (`fat_table_point()`, a sync of data and
tables), a new folder is on the card before anything names it, and a cut (truncate to a smaller
size, the end of a write that stopped early) is on the card before the clusters past it are
freed. `tests/host/test_fatfs.c` checks the image after every write the disk stores.

**Tests, QEMU** (`boards/qemu_virt/test_os.py`, 37 checks per build; the kernel word `sdsim`
makes the RAM disk a simulated card behind `sdcard.c` and `sdblk.c`, and the `sdsim` command
arms the card model's faults, which follow the io track's `SdFaults`): one block answered "write
error" (retried at the file's close, the command succeeds); a card that refuses every block
(`write` and a 1 MB `cp` say "not all of it reached /sd0p1", return at once, `eject` says "stays
mounted"; after `sdsim fault clear`, `sync` writes everything, and on the host every file is
whole and the copy holds the first 57344 bytes of its source: since B6 the refusing card gets
its share of the cache and then EIO, where before it took 514560 bytes, nearly the whole cache);
a card that answers no command (the write fails at once, and since B10 its close says so: `write:
close: I/O error`); the card taken out after a sync (the messages, writes refused, the
mount marked unplugged; `fsck.fat -n` finds only the dirty bit; put back, mounted with "not
unmounted cleanly", the files there); and taken out right after the nth block a new file's write
sends, n = 1 to 4, and after the first block of an `rm`, an `mv` into a folder and a write over a
file. What `fsck.fat -n` found on those cards: the dirty bit; "Reclaimed 1 unused cluster"; the
FATs differing; a stale free count; for the `mv`, the file under both names ("share clusters",
fsck keeps the first). Each new-file card was booted again (the reboot): mounted with a warning,
the earlier file whole, the new one whole (n = 4) or absent.

**A full volume, QEMU** (`test_os.py`, 9 checks): the stick made nearly full on the host (mtools
adds `/FILLER.BIN`, 3003 clusters of 512 bytes stay free; mtools keeps FSInfo's count). A 1 MB
`cp` fits; the next says `cp: write: No space left on device` when the space runs out (0.1-0.2
s, no hang) and keeps what fitted: the source's first 488960 bytes, every free cluster. `df`
then says 0 KB free; `write` and `mkdir` on the full volume fail the same way, at once, leaving
no directory behind; `rm` gives the space back and a write works. After `eject`, `fsck.fat -n`
finds the volume clean and mtools reads every file back. Before B4, the copy that did not fit
left the clusters of its last run in its chain past its size, and fsck.fat said so.

**Tests, RTL** (`make -C surfcpu test-os-persist`, `sim/soc/tools/system_persist_test.py`, on
`make check`): the soc track's whole SoC in surfsim with the io track's card model, booted by
SurfBoot from a `mkcard.py` card with `/BOOT/CMDLINE.TXT` "ascii sd=rw". Over the UART, in
`/sd0p1/USERS/DOCUMENTS`: `mkdir Saved-notes`, a small file in it, a 160 KiB `cp` into it (320
clusters of 512 bytes: entries in three FAT sectors and more), a file written over with shorter
text, a line appended to another, one moved into the folder under a long name, one deleted, then
`sync`, `cd /`, `eject sd0` ("/sd0p1 unmounted", "sd0: safe to remove"); `--sd-flush` writes the
model's blocks into the image. A second boot from that image (the reboot) mounts it clean and
reads every name, size and CRC back; then `fsck.fat -n` finds the image clean and mtools lists
the folder (long names and their aliases) and returns all seven files byte for byte. 17 checks,
about 2.5 minutes (two runs of about 11 and 9 s simulated, at about 3.5 million cycles a second
of wall time); it is on
`make check`, because it is the one test of the write path through the RTL and stays within a
few minutes.

**Faults and a full card, RTL** (`make -C surfcpu test-os-faults`,
`sim/soc/tools/system_fault_test.py`, 25 checks, on `make check`): the same SoC with surfsim's
card actions (`--sd-fault`, `--sd-remove [N@]`, `--sd-insert`, MODULES.md section 7), which set
the io track's card model's faults and take the card out of its socket and put it back, so card
detect reaches the kernel through surf_spi's STATUS.CARD. Two runs at once on 36 MiB `mkcard.py`
cards with sd=rw, about 1.5 minutes of wall time (12 and 8 s simulated). The faults run: one
block answered "write error": the console says so and "takes writes again" at the file's close,
the write succeeds; every block refused: `write: not all of it reached /sd0p1: I/O error` 348 ms
after the prompt it was typed at, `eject sd0 stays mounted`; the fault cleared, `sync` writes
what the cache kept; the card taken out after that sync: the watch says "the card was taken out"
286 ms later (its two looks are 200 ms apart), `sdinfo` reads `card absent` from STATUS.CARD, a
write is refused (`I/O error`); `umount`, the card put back: sd0 again, `mount sd0p1` says "not
unmounted cleanly", and the files read back by CRC; then the card out right after the 2nd block
of a new file's write: the write fails and the watch says so. On the host the card as it came
out: `fsck.fat -n` finds the two FATs differing and the dirty bit, nothing else; mtools reads
every earlier file whole, and the new file is not there. The full run: a card made nearly full
on the host (mtools adds a filler, 83 clusters stay free): a 64 KiB `cp` says `No space left on
device` and keeps the source's first 42496 bytes (every free cluster), `df` says 0 KB, a write
on the full card fails the same way, `rm` gives the space back, a write works, `eject`; on the
host `fsck.fat -n` finds it clean and mtools reads every file back.

**Speed.** On SurfCPU every instruction is fetched from DDR without a cache, so the instruction
count is the time. Four changes in the port's own code took the test above from 402 s to 158 s
of wall time (and leaving the 160 KiB source file's read-back to the host, to about 145 s):
CRC-16 (`sdproto.c`) and CRC-32 (`crc32.c`) from 256-entry tables instead of bit by bit and by
nibbles, a data block moved in a tight loop on surf_spi's DATA register (`spi_rx`, `spi_tx`)
instead of a function call per byte, and `memcpy`/`memmove`/`memset` moving words
(`arch/riscv32/memory.c`; never a misaligned access: SurfCPU traps them). The 160 KiB `cp` now
takes about 113 million cycles (4.5 s at 25 MHz), some 700 cycles a byte read and written.

**The cost of the order** (audit B6, B7). The order points B7 adds cost requests, not data: a
file that grows has its chain's FAT sector written each time the chain crosses into the next
one (every 128 clusters on FAT32: every 4 MiB with 32 KiB clusters, every 64 KiB with a test
card's 512-byte clusters). Counted on the host with the real code (`tests/host` stand-ins, a
RAM disk): 4 MiB written in 4 KiB pieces on a 1 GiB FAT32 volume takes 119 device requests
instead of 96 before (84 instead of 68 in 64 KiB pieces), 0.2-0.4 % more blocks; on a 64 MiB
volume with 512-byte clusters, 197 instead of 99. `rm` (3 requests a file), `mv` (2) and a
write over a file (5) take the same as before. A device keeps at most 256 dirty blocks: a long
write through the cache writes half of them out when it reaches that (much as making room did
when the cache was full). `trim_chain()` walks a short write's chain from where the write was,
not from the file's first cluster.

Since audit B12-B19 (counted the same way, before = d7452f4): every workload above sends the
same requests and blocks, but 4 MiB in 4 KiB pieces on a 1 GiB FAT32 volume, 105 requests
instead of 119 (a FAT sector no longer waits for every dirty data block, only for the older
ones, so the run of data still being filled is no longer cut in pieces); the host's CPU time
is within 5 % on each (bcache-review.txt section 8). The bookkeeping costs memory: each of the
cache's 1024 buffers grows from 580 to 628 bytes on i386 (the change numbers, the list links,
three flags and when a sync began to wait for its hold: 48 KiB), each struct bdev by 44 bytes.
On SurfCPU (the RTL system, the same simulator and ROM, d7452f4's kernel against this one):
test-os-persist's write run 0.9 % fewer cycles, its reboot run 1.0 % more, test-os-faults' run
on the card that refuses 3.7 % more and its full card 0.3 % fewer; the commands that sync take
4-20 % fewer cycles, the 160 KiB `cp` 1.7 % more, and a boot about 2 million more (a package
6.4 KB larger to load, a larger cache to clear; bcache-review.txt section 10).

Since audit B20-B26 (counted the same way, before = fd37406): every workload above sends the
same requests and blocks (nothing refuses there, so no list of failed blocks is made), the host's
CPU time the same on the long writes and under 1.5 us more an operation on the short ones
(bcache-review.txt section 15). struct bdev is 8 bytes smaller, each mounted FAT volume 76 bytes
larger, a sync keeps a 68-byte count on its stack, and while an rm, mv, truncate or cut-back runs
on a device with failed blocks a list of them is allocated (5 bytes each, 1024 at most). On
SurfCPU (b81ccc9's kernel against this one, the same simulator and ROM): test-os-persist's write
run 0.3 % more cycles, its reboot run 0.3 %, test-os-faults' run on the card that refuses 0.1 %
and its full card 0.4 %, most of it the package, 3.7 KB larger, that SurfBoot loads (+0.44
million cycles). B26 had first made the check of what a sync owes too large for gcc to inline,
and each command that syncs cost 0.78 million cycles more (3.4 % on the write run): it is inline
again (bcache-review.txt section 17).

Since audit B27-B33 (counted the same way, before = 15f0f3c): every workload above sends the same
requests and blocks but an rm of a file right after it was written, which no longer writes the file's
last dirty blocks (B28: a 4 MiB file, 5 requests and 39 blocks instead of 6 and 47; its chain is walked
to find them); the host's CPU time is within the noise of a loaded Mac (bcache-review.txt section 22).
Each mounted FAT volume is 452 bytes larger (64 chains can wait to be freed, B31), and an rm, truncate
or mv over a file lists the device's dirty blocks (from the device's lists of them, a few blocks) into a
small kalloc'd list. On SurfCPU (15f0f3c's kernel against this one, the same simulator and ROM):
test-os-persist's write run 0.15 % more cycles, its reboot run 0.17 %, test-os-faults' full card
0.24 % and its refusing card 1.96 %, all but 0.12 % of that the card-detect task meeting the card's
removal one look (200 ms) later; SurfBoot loads a package 3.2 KB larger (+0.38 million cycles), and each
command is within 0.4 % either way. Two drafts cost more and were taken back: each rm, truncate and mv
over a file looked over the cache's 1024 buffers twice, and an rm walked its file's whole chain for its
own directory block (in the data area on FAT32: +0.5 million cycles, 9.7 %, on the full card's rm of a
few thousand clusters) (bcache-review.txt section 24).

Since audit B34-B38 (counted the same way, before = 42a45d4): every workload above sends the same
requests and blocks, and the host's CPU time is the same within the noise of a loaded Mac
(bcache-review.txt section 29). A copy under way (B12) keeps a record on its writer's stack, and
setting aside or dropping a block looks at those records (none most of the time); struct buf's new
flag fits in its padding. On SurfCPU (42a45d4's kernel against this one, the same simulator and ROM):
test-os-persist's write run 0.21 % more cycles, its reboot run 0.24 %, test-os-faults' full card
0.20 % and its refusing card 0.37 % fewer (the card-detect task meeting the removal one 200 ms look
sooner; 0.11 % more without those steps); SurfBoot loads a package 1.1 KB larger (+0.13 million
cycles), and each command is within 0.2 % (bcache-review.txt section 31).

Since audit B39-B45 (counted the same way, before = 47fb4ec): every workload above sends the same
requests and blocks, and the host's CPU time is the same within the noise (bcache-review.txt section
37). A FAT call counts its task inside the file system (two stores and a critical section's two
calls), and the cache counts it only around a device request or a copy: the calls made for each block
(bread, brelse, bpeek) count nothing, the cache lock's owner standing for their short sections. On
SurfCPU (47fb4ec's kernel against this one, the same simulator and ROM): test-os-persist's write run
0.25 % more cycles, its reboot run 0.19 %, test-os-faults' full card 0.32 % and its refusing card
0.51 % fewer (the card-detect task meeting the removal sooner); against 47fb4ec padded with 4 KB of
code that never runs, +0.08 %, +0.04 %, +0.04 % and -0.28 %: the rest is the package, 3.8 KB larger,
that SurfBoot loads (+0.48 million cycles). Each command is within 0.3 %. Two drafts cost more and were
taken back: a count in each of the cache's calls (+1.05 % on the write run against the padded base, cp
+1.9 %), and a wake-up of the cache's waiters for each block an eject's binvalidate() dropped (+5 % on
the eject) (bcache-review.txt section 39).

Since audit B46-B49 (counted the same way, before = 0a9a5f2): every workload above sends the same
requests and blocks. A device request counts its task inside (two stores around the driver's call),
a cache wait looks at one more flag where it begins, and mkfs at its call before each request. On
SurfCPU (0a9a5f2's kernel against this one, the same simulator and ROM): test-os-persist's write run
0.05 % more cycles, its reboot run 0.06 %, test-os-faults' full card 0.06 %, and its refusing card
1.46 %, all but 0.04 % of that the card-detect task meeting the card's removal one look (200 ms)
later; SurfBoot loads a package 880 bytes larger (+0.08 million cycles), and each command is within
0.5 % (bcache-review.txt section 47).

**Not safe, and not claimed** (CARD.md section 8 says it for the owner): FAT32 has no journal,
so a card that comes out in the middle of a write leaves the volume half-changed, repairable by
`fsck.fat` but not repaired by SurfOS (it mounts such a volume and warns). A file being
overwritten (O_TRUNC) can be left empty, and an append can lose its new bytes. A rename can
leave a name too many, a truncate cut short a size that runs past the chain. One narrow case
can still leave a chain running into a free cluster: a FAT12 entry that straddles two sectors
(B25 closed the other: a write cut back while the card refused now frees the clusters past
its cut only once the cut is on the card). A write that fails or comes up short may have
changed, on the card, the file's bytes in the request it stopped in (B22: up to 64 KiB past
what it returned). A card taken out and put back within half a second is not seen. What the
card itself does with a block it was programming when its power
went is beyond the kernel.

## 7. What is done, pending and later

Done and tested under QEMU (qemu_virt, emulation only): the build, the arch core, the
machine-mode kernel with its shell, tasks, timer and interrupts, the self test with the port
checks, FAT read/write on a RAM disk checked from the host, the boot ABI's handoff, FAT over the
SD protocol driver and a simulated card, `/USERS/DOCUMENTS` by its long name both ways, and,
with copies of the fw track's surfimg.py and mkcard.py, the kernel package and a card image
laid out as SurfBoot expects. Done and tested in the fw track's firmware emulator: the Arty
kernel booted by SurfBoot from a card, to the shell, through its self test and `scrolltest`.
In the RTL system simulation (the soc track's `test-soc-system-boot`): SurfBoot loads the Arty
`SURFOS.BIN` from a card image on the real core, and the kernel reaches its shell over the UART
and the PS/2 keyboard model, its console decoded from the VGA pixels; that run found the console
race fixed as C10. Milestone 8 in simulation (section 6): files written, ejected and read back
after a second boot on the RTL system; write errors, the card taken out (card detect through
surf_spi) and put back, and a full card, on the RTL system too, with what fsck.fat finds
afterwards; under QEMU also a card that stops answering, pulls after each block of a write, an
rm, an mv and an overwrite, and a full volume. Done and tested on the host: the
Arty drivers' logic (display, keyboard, SD protocol and driver against a simulated card), and
the SD driver against the io track's card model with surf_spi's timing.

**On the board (the owner's Arty A7-100T, 2026-10-16/17):** the Arty HAL (`board.c`,
`textcon.c`, `ps2kbd.c`, `sd.c`) runs on the FPGA and its drivers start (`Drivers: serial gpio
ps2kbd sdcard`). SurfBoot 1.1.0 loads `SURFOS.BIN` from the owner's 32 GB card into DDR, and
the kernel reaches its shell over the UART; `uptime`, `ps`,
`memstat` (256 MiB) and the two-task test work. The SD driver identifies the card (SDHC 29 GB,
CRC checked), mounts its FAT32 volume read-only, and `ls`, `cat` and `df` read it, since 22cbdc9
sends the 8 clocks a real card needs between CMD55's response and ACMD41 (the first board run
failed there, `+SD: out of range (ACMD41 failed)`)
([evidence](evidence/physical/surfos-boot-2026-10-16.txt),
[fixed flow](evidence/physical/ddr-fixed-flow-2026-10-17.txt)). So brief milestone 7's "real
board boots SurfOS from microSD" is shown over the UART.

**Pending:** the console on VGA and the PS/2 keyboard on the board (a real PS/2 keyboard is
needed, INVENTORY.md G5), and milestone 8 on a real card: the SD write path stays off by default
(read-only, `sd=rw` to allow it) until it is tried there. The board's acceptance tests are one
command each (`make -C surfcpu board-test M=m1|m3|m5|m7|m8|m9`, docs/PHYSICAL-TESTS.md), each
writing its own evidence file: m1 and m3 passed on the board; m5, m7, m8 and m9 have run against
the RTL system through `surfsim --uart-pty` and against fake boards (`make test-os-kit`, `make
test-os-kit-rtl`), not yet against the board.

Later (brief section 11), each a design of its own: a user mode (U mode with PMP, or S mode
with Sv32) and with it protected processes, the ELF loader for `EM_RISCV`, system calls by
ECALL from U mode, `libsurf` and `surfsh` rebuilt for RV32; the Ethernet MAC and the network
stack; a graphics mode.

## 8. Notes on the contracts

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
  of the DDR that calibrated. Since 10/16/26 the kernel heap ends at `ram_base + ram_size - 16
  MiB` from the boot information (`arch/riscv32/mem.c`), never above the board's `HEAP_END`
  (the Arty: the map's `ddr_top`; QEMU virt: its RAM disk window). The Arty's 256 MiB give
  `0x8F000000` as before; `test_os.py` hands over 128 MiB and sees the heap end at `0x87000000`.
* The fw track's simulated SD card (its emulator's) has no CMD18 or CMD25; real cards all do.
  The kernel copes (one block per command), but a run in that emulator says nothing about the
  driver's multiple-block paths, which the io track's card model and the host model test.
* Done (10/16/26, round 4; it was a proposal here): surfsim can inject the card model's faults
  and take the card out during a run: `--sd-fault KIND=N@REGEX`, `--sd-remove [N@]REGEX` and
  `--sd-insert REGEX` in `sim/soc/surfsim/surfsim.cpp` (the soc track's file, changed by the os
  track with the coordinator's leave; a test-harness change, no RTL), documented in MODULES.md
  section 7. `test-os-faults` uses them (section 6).
* Done (10/16/26, round 5): `surfsim --uart-pty` puts the UART on a pseudo-terminal, so the
  physical test kit (`tools/uart.py`, `tools/board-test.py`) talks to the RTL system as to the
  board's USB serial port; with it SIGTERM ends the run as a power cut (`--sd-flush` keeps the
  card) and SIGUSR1/SIGUSR2 take the card out and put it back (MODULES.md section 7; a
  test-harness change in the soc track's file, no RTL).
