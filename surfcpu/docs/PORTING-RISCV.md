# Porting SurfOS to riscv32 (machine mode)

The x86 kernel of this repository (Multiboot, ring 0 and ring 3, paging, PC devices) runs on
SurfCPU, our RV32IM core, as a **machine-mode** kernel: one privilege level, flat physical
memory, the SurfOS kernel shell on the console, kernel tasks scheduled pre-emptively. This is
the portability inventory the brief asks for (AGENT-INSTRUCTIONS.md section 7): every x86
dependency, what replaces it, what is left out, and the plan.

| | |
|---|---|
| Build | `make -C surfcpu build-kernel` → `build/riscv32/arty_a7/{surfos.elf,surfos.payload.bin,SURFOS.BIN}` (and `qemu_virt`); `arch/riscv32/kernel.mk` |
| Test | `make -C surfcpu test-os`: the qemu_virt kernel at -O2 and -O0 in `qemu-system-riscv32`, driven over its serial console (`boards/qemu_virt/test_os.py`), plus host tests |
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
  BOOT-ABI.md section 2. Until the fw track's `surfcpu/tools/surfimg.py` is merged, a temporary
  packer written to BOOT-ABI.md does it (`arch/riscv32/surfimg_tmp.py`: pack, verify with
  SurfBoot's checks in order, the CRC test vectors); every constant comes from the generated
  `soc.h`.

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
| PS/2 keyboard via the 8042, scan code set 1 | `kernel/keyboard.c` | the Arty's `surf_ps2` delivers set 2: `boards/arty_a7/` translates it (planned in this document's section 6) |
| CMOS clock | `lib/blibc/time.c` | `arch/riscv32/time.c`: QEMU's Goldfish RTC; the Arty has no clock (kernel build time + uptime) |
| Reboot (ACPI, 0xCF9, 8042, triple fault) | `kernel/sys.c` | `board_reset()`: QEMU's reset register; on the Arty a jump to SurfBoot at the reset vector (a warm start) |
| PC speaker | `kernel/sys.c`, `shell/demos.c` | nothing (`sysbeep()` does nothing; the demos are not built) |
| `divl` 64/32 division | `lib/blibc/vsnprintf.c`, `include/surfos/math64.h` | C division through libgcc (guarded in vsnprintf.c; math64.h shadowed) |
| Driver table (PCI, ATA, USB, NICs, ...) | `driver/drivers.c` | `arch/riscv32/drivers.c`: the serial input, then the board's table |
| RAM disks from Multiboot modules | `driver/ramdisk.c` | QEMU: `boards/qemu_virt/board.c`'s rd0, the image `-device loader` puts at 0x88000000. Arty: the microSD card |
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

How "unchanged" was checked: the x86 kernel built before and after (`devenv/build.sh`, O0 and
O2) has byte-identical allocated sections (code, data, rodata, the symbol table, BSS) in
`kernel/task.o`, `lib/blibc/vsnprintf.o` and `surfos.bin`; only debug line numbers moved. Then
`make test-all`, `make unittest` and `make test-usb` (O0 and O2) passed (evidence in
`surfcpu/docs/evidence/os/`).

## 4. The port checks (AGENT-INSTRUCTIONS.md section 10)

`selftest` (arch/riscv32/rvtest.c), run by `make test-os` at -O2 and -O0:

| Check | How |
|---|---|
| Traps preserve every register | `rvregs.S` fills ra, gp, tp, x5-x31 with a pattern, spins while timer interrupts arrive (at least three), or yields with ECALL every round, then dumps them all; two tasks do it at once with different patterns while the scheduler switches between them |
| Stack alignment (16 bytes) | sampled in a task, in nested calls and inside the timer interrupt; the trap frame is a multiple of 16 and is aligned whatever `sp` was |
| `sp`, `gp`, `tp` | `sp` inside its task's stack at every trap (trap.c) and in the test; `gp` the kernel's global pointer in tasks and interrupts; `tp` zero |
| ABI and structure sizes | ilp32 sizes, 8-byte alignment of `long long`, signed `char`, the boot ABI's 64- and 256-byte structures, the trap frame against traps.S |
| Runtime helper ISA | `isacheck.py` on every link (libgcc included) |
| BSS and data initialization | start.S clears the BSS; the kernel runs on initialized data (any wrong value shows) |
| Allocator limits | heap churn with patterns and a heap walk (`heaptest`, `heap`); `run_memcheck()` |
| Timer rollover | the tick difference across the 32-bit wrap; mtime against the tick count; 64-bit mtime read hi/lo/hi |
| Task switching under load | 32 tasks created and collected, exit codes, two sleepers, a mutex between two tasks, a semaphore ring, an event with a timeout, the two-task register test |
| 64-bit arithmetic | division, remainder, 32x32 multiply, the formatter's 64-bit numbers |

## 5. The boards

**qemu_virt** (`boards/qemu_virt/`): QEMU's generic virt machine, entered with `-bios none` at
0x80000000: its 16550 UART (PLIC source 10), the CLINT (10 MHz), the PLIC (context 0), the
Goldfish RTC, the sifive_test device (`poweroff`, `reboot`) and a RAM disk: the harness loads a
FAT32 image (tools/mkimage.py's "foreign" stick) at 0x88000000 with `-device loader`; its size is
read from its partition table. It is a separate HAL so the portable kernel can be tested now; it
models none of SurfCPU's peripherals and says nothing about the FPGA.

**arty_a7** (`boards/arty_a7/`): SurfCPU's SoC, MEMORY-MAP.md, every address and bit from the
generated `soc.h`: `surf_uart`, `surf_timer` (timebase from the register), `surf_intc`,
`surf_gpio` (LD4 blinks with the heartbeat task), `surf_sysinfo`. The microSD card, the PS/2
keyboard and the text display: section 6.

## 6. Plan and what is still to come

Done (tested under QEMU): the build, the arch core, the shell, tasks, the timer, interrupts,
the self test, FAT read/write on a RAM disk with the host checking the result.

Next, in this order (each host-tested where its logic allows):

1. The Arty's 80x30 text display as the console backend (32-bit cells, palette, hardware scroll).
2. The PS/2 keyboard: scan code set 2 translated for the keyboard code, F1-F4, LEDs.
3. The microSD card over `surf_spi` as a block device (`sd0`, partitions `sd0p1`): SD commands,
   CRC7/CRC16, SDHC/SDSC addressing, read, then write with flush; FAT through the VFS, mounted
   read-only first and read-write once tested, with `eject` (flush and unmount).

Later (brief section 11), each a design of its own: a user mode (U mode with PMP, or S mode
with Sv32) and with it protected processes, the ELF loader for `EM_RISCV`, system calls by
ECALL from U mode, `libsurf` and `surfsh` rebuilt for RV32; the Ethernet MAC and the network
stack; a graphics mode.

Pending on hardware: everything on the Arty (no board, no bitstream build host yet).
