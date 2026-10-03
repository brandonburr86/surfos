# SurfOS Architecture (as restored, October 2026)

This is a map of the kernel as it exists on `master`, written for people and AI
sessions that are about to extend it. Everything marked **verified** was observed
by building the tree with GCC 13 / binutils 2.42 / NASM 2.16 and booting it under
QEMU 8.2 (`qemu-system-i386 -m 64 -kernel surfos.bin`). Everything else comes from
reading the source. Line numbers refer to the files on `master` at commit `cd518b5`; paragraphs marked
**ai-dev** describe what the development branch has changed since.

## 1. What SurfOS is

A 2004 hobby kernel for 32-bit x86, about 8,500 lines of C and NASM (plus a
5,800-line PCI vendor table that nothing includes yet):

| Property | Value |
|---|---|
| CPU mode | 32-bit protected mode, paging on, single CPU, no FPU init |
| Boot | Multiboot 1 (GRUB or `qemu -kernel`), ELF image linked at 1 MB |
| Kernel style | Monolithic, everything in ring 0 (shell included) |
| Segmentation | Flat: code 0x08, data 0x10 (ring-3 selectors exist but are broken, see audit) |
| Memory | Identity map of the low 16 MB, demand-paged kernel heaps above 2.5 GB |
| Scheduling | Pre-emptive, 100 Hz PIT, software stack switching, 4 run levels + sleeping + removal queues |
| Interrupts | Two 8259 PICs remapped to 0x20-0x2F, shared IRQ handler chains, exceptions kill the current task |
| Devices | VGA text console (3 virtual consoles) mirrored to a COM1 serial console, PS/2 keyboard plus serial input, PIT, CMOS RTC, parallel port, 8237 DMA (incomplete), floppy (incomplete), PCI bus 0 scan, 3Com 3c905B NIC (no protocol stack) |
| libc | `blibc`: printf, puts/gets, getch, strlen/strcmp/strcpy/strchr, memcpy/memset, ctype, CMOS time |
| User interface | Ring-0 debug shell with about 20 commands |

Required RAM is 32 MB (`mm/memory.c:21`). The kernel version string is 0.007 and
the shell calls itself v0.008.

## 2. Source tree

```
boot/        boot.S (Multiboot header + entry), multiboot.h
kernel/      main.c (kmain), gdt.c (GDT + TSS), interrupt.c (IDT, PICs, dispatch, IRQ chains),
             traps.asm (256 entry stubs), task.c (scheduler), timer.c (PIT), panic.c (exceptions, panic),
             ksym.c (symbol lookup, backtraces), klog.c (kernel log), console.c (VGA + kprintf),
             keyboard.c, sys.c (sleep/beep/reboot), turf.c (Martin McCormick's reader/writer "turf" locks)
mm/          memory.c (init + page stack push/pop), paging.c (memprobe, page tables, page-fault handler),
             kalloc.c (kernel heap, best-fit free list), palloc.c (1:1 "physical" heap for DMA)
lib/blibc/   chars.c printf.c strings.c string.c memory.c ctype.c time.c
shell/       main.c (the shell + demos), parport.c (lpstat)
driver/      drivers.c (init order), pci.c, parport.c, dma/, floppy/, net/3c905b/, PCIDATA.H (unused table)
include/     surfos/ (kernel headers), mm/, sys/ (driver headers), net/, asm/io.h, blibc headers
tools/       qemu-run.py (headless QEMU harness), gensyms.py (symbol table for backtraces)
tests/host/  unit tests for blibc, built with the host compiler (make unittest)
```

Thirteen files carry the comment `Rebuilt 9/28/2026 - not in the printout, see
REBUILD-NOTES.md`: they were reconstructed during the restoration rather than typed
in from the 2004 printout (`driver/drivers.c`, `driver/pci.c`, `lib/blibc/time.c`,
`lib/blibc/memory.c`, two Makefiles and seven headers). `REBUILD-NOTES.md` itself is
not in the repository. Treat those files as the least "original" code.

## 3. Boot sequence

```
GRUB / qemu -kernel
  -> start (boot/boot.S:9)        16 KB stack in .bss, pushes Multiboot magic + info pointer
  -> kmain (kernel/main.c:24)     both arguments are ignored
       init_serial()              programs COM1 (115200 8N1); from here on every kprintf also goes to the serial port
       init_gdt()                 writes a 256-entry GDT at physical 0x6000, far-jumps to reload CS, reloads the data segments
       init_mem()                 memprobe() sizes RAM by writing a magic value every 4 KB from 8 MB up;
                                  init_paging() builds the page tables and turns paging on
       init_console()             VGA text mode, 3 virtual consoles, console 0 active
       run_memcheck()             halts below 32 MB
       init_interrupt()           PIC remap, exception vectors 0-18, turf for IRQ masking, IRQ1-14 stubs, STI
       init_task()                6 run queues, int 0x40 = yield, idle task (pid 0), "Shell 0" task (pid 1)
       init_keyboard()            IRQ1 handler
       init_drivers()             serial IRQ 4 -> parport -> dma -> floppy -> pci -> 3c905b attach
       init_timer()               PIT 100 Hz, IRQ0 = timerISR
       for(;;) hlt                this loop IS the idle task; the first tick saves its ESP into task 0
```

The messages printed by `init_gdt()` and `init_mem()` only appear on the serial
console: the video console is initialised after them and `kputch()` has nowhere else
to put output while `conActive` is NULL. (On `master` they were lost entirely.)

CS reload: the Multiboot specification leaves the code selector undefined. QEMU's
built-in loader enters with CS=0x08, GRUB 2 with CS=0x10. The far jump in `init_gdt()`
is what makes the GRUB ISO boot (**verified**: without it the first `iret` after a page
fault raised a general protection fault).

## 4. Memory map

Physical and virtual addresses are the same for the first 16 MB. Everything is
defined in `include/mm/memory.h` and `include/sys/dma.h`.

| Range | Use | Notes |
|---|---|---|
| 0x00000000 - 0x000FFFFF | BIOS/real-mode area | **ai-dev** unmaps page 0 after driver init, so NULL dereferences fault |
| 0x00008000 - 0x00047FFF | ISA DMA bounce buffers (`dma-mm.c`) | 4 x 64 KB slots |
| 0x00100000 - ~0x00125000 | Kernel image | .text, .rodata, .data, .ksyms (symbol table), .bss (16 KB boot stack, GDT, IDT, page directory) |
| 0x00200000 - 0x005FFFFF | Page tables | 4 MB = 1024 tables, covers the whole 4 GB; every directory entry points here from boot |
| 0x00600000 - 0x009FFFFF | Free-frame stack | 4 MB of frame addresses, popped top-down (`mm/pmm.c`) |
| 0x00A00000 - 0x00EFFFFF | DMA heap | `kalloc_dma()`, 1:1 mapped, for anything a device reads or writes |
| 0x00F00000 - end of RAM | Free page frames | From the Multiboot memory map, minus loaded modules |
| 0xB0000000 - 0xBFFFFFFF | Kernel heap (`kalloc()`) | Pages mapped on first touch by the page fault handler |

**ai-dev**: `mm/pmm.c` builds the frame stack from the Multiboot map that
`kernel/multiboot.c` copies at boot (on `master`, `memprobe()` wrote a magic value to
every page from 8 MB up). `mm/paging.c` offers `vmm_map`/`vmm_unmap`/`vmm_get_phys`;
the page fault handler maps kernel-heap pages on demand and reports every other fault
with address and cause. `mm/kalloc.c` is a first-fit allocator with 16-byte headers,
splitting, merging in both directions, poisoning and bad-pointer panics, instantiated
twice (kernel heap, DMA heap). `memstat` shows frames and heaps; `heaptest` churns it.

## 5. Tasks and scheduling (`kernel/task.c`)

**ai-dev** rewrote the scheduler; the 2004 classes and pick order survive.

* A task is `surf_task` (`include/surfos/task.h`): name, pid, class, state, flags,
  console, saved frame, 16 KB kernel stack, parent, exit code, wake time, list links,
  slice and accounting. States: READY (on a run queue), RUNNING, BLOCKED (on a wait
  queue), SLEEPING (on the sleep list), DEAD (a zombie until collected).
* Four run queues, FIFO/HIGH/NORMAL/LOW, picked in the order `F H N L, F H N H`.
  Slices are 4/2/1 ticks for HIGH/NORMAL/LOW; FIFO tasks run until they block. A task
  made ready in a better class preempts at the next tick. The idle task (pid 0, the
  boot context) runs only when nothing else can and executes `hlt`.
* Context switch is pure stack swapping. Every trap builds a `struct trapframe`
  (`include/surfos/trap.h`); the timer tick and `yield()` (`int 0x40`) call
  `schedule(tf)`, which saves the frame address in the outgoing task and returns the
  incoming task's frame. `schedule()` never runs outside trap context.
* Blocking: `wait_prepare()`/`wait_on()`/`wake_up()` (`kernel/wait.c`), mutexes,
  semaphores and events (`kernel/sync.c`), `sleep_ms()`, kernel timers
  (`kernel/ktimer.c`). `getch()` blocks on the keyboard wait queue.
* Lifetime: `kthread_create()` makes a child of the caller; `task_exit()` or
  `kill_task()` turns a task into a zombie and `task_wait()` in the parent frees it;
  init (pid 1) collects detached and orphaned tasks and restarts a shell when one
  dies. Nothing frees a stack that may still be running, and every kernel stack is
  pre-faulted at creation (a stack page fault would double-fault).
* `KCRIT_ENTER`/`KCRIT_LEAVE` is a per-task counter that stops the tick from
  preempting; `irq_save()`/`irq_restore()` protect data an interrupt handler touches.
* `ps` lists pid, state, class, CPU ticks, switches and parent; `kill`, `sleep`,
  `uptime`, `selftest` exercise the machinery.

## 6. Interrupts and exceptions

**ai-dev** (`kernel/traps.asm`, `kernel/interrupt.c`, `kernel/panic.c`):

* One stub per vector pushes an error code (the CPU's for #DF #TS #NP #SS #GP #PF
  #AC #CP, otherwise 0) and the vector number; `trap_common` saves the registers,
  loads the kernel data segments, calls `trap_dispatch(tf)` and resumes whatever
  frame it returns. Handlers are registered with `trap_set_handler(vector, fn)`.
* Vectors 0-31 go to `trap_exception()`: a kernel wipeout (register dump,
  backtrace, halt) if there is no current task, the idle task is current, or we are
  inside an interrupt handler; otherwise the task is killed with a one-line reason
  and a backtrace, and the scheduler moves on. The page fault (14) is the demand
  pager in `mm/paging.c`. Vectors nobody installed are reported and ignored.
* IRQ 0-15 (0x20-0x2F) run the shared handler chains (`add_irq_handler()`,
  `del_irq_handler()`, enable/disable), count into `irq_count[]`, send a specific
  EOI, and for IRQ 0 hand the frame to `timer_tick()` and the scheduler. IRQ 7 and
  15 are checked against the in-service register first; spurious ones are counted
  and not acknowledged. IRQ masking is under `irq_save()`.
* `init_interrupt()` populates all 256 IDT entries (int 0x80 with DPL 3 for the
  future syscall gate), remaps the PICs to 0x20/0x28 and enables the cascade.
* Backtraces: `tools/gensyms.py` turns the link map into a `.ksyms` table (the
  kernel is linked twice; the table sits after the code so nothing moves) and
  `kernel/ksym.c` walks the frame-pointer chain, checking each frame against the
  page tables first. `panic(fmt, ...)`, `BUG_ON()`, `ASSERT()` print a backtrace
  and halt. Everything `kprintf()` prints is also kept in a 16 KB ring (`dmesg`).
* Shell commands `crashdiv`, `crashgp`, `crashint` exercise the three paths.

On `master` the same area was 18 hand-written stubs with error-code mismatches,
an uninitialised IDT at 0x6800, and an IRQ 7 handler that rebooted the machine.

## 7. Console and input

* `console.c` keeps three `surf_console` buffers plus `conVideo` for 0xB8000.
  Output goes to the task's console and, if that console is active, to video memory.
  `kprintf()` prints in yellow to the active console, `printf()` (blibc) to the
  current task's console in its colour. Every character funnels through `kputch()`,
  including `puts()`/`cputs()`.
* Serial console (`driver/serial.c`): `kputch()` mirrors everything written to the
  active console to COM1 (newline becomes CR LF, backspace becomes "\b \b", a console
  clear becomes an ANSI clear), and IRQ 4 pushes received bytes into the keyboard
  queue (CR to newline, DEL to backspace, escape sequences dropped). `qemu -nographic`
  therefore gives a complete terminal session; `tools/qemu-run.py` drives it.
* F1-F3 switch consoles from inside the keyboard ISR, but `schedule()` sets
  `conActive = curTask->con` on every switch, so the display is left showing the
  new console while input and output continue on console 0 (**verified**: F2 blanks
  the screen and it stays blank).
* Keyboard: scan-code set 1 table for 0x00-0x58, shift handled, caps lock only
  toggles the LED, extended (E0) keys not decoded. The ISR also spawns tasks on
  F5/F6/F9/F12 and toggles the floppy motor on F7/F8 (debug hooks).
* Input is a 255-byte queue shared by the keyboard and the serial port; `getch()`
  blocks on a wait queue that both interrupts wake (**ai-dev**; `master` spun and
  emptied the queue on entry, losing type-ahead).

## 8. Drivers

| Driver | Files | State |
|---|---|---|
| PIT timer | `kernel/timer.c` | Works, 100 Hz, `getticks()` |
| PS/2 keyboard | `kernel/keyboard.c` | Works for ASCII input |
| Serial console | `driver/serial.c` | COM1, 115200 8N1, polled TX, IRQ 4 RX. Mirrors the active console and feeds input to the key queue. Added on `ai-dev` |
| VGA text | `kernel/console.c` | Works, console switching half broken |
| CMOS RTC | `lib/blibc/time.c` | Reads BCD time; the shell `time` command is commented out |
| Parallel port | `driver/parport.c`, `shell/parport.c` | Status readout works in QEMU. Its IRQ 7 handler is `reboot()` |
| ISA DMA | `driver/dma/` | Register helpers only. `DMAComplete()`, `dma_alloc()`, `dma_xfer()` are broken |
| Floppy | `driver/floppy/floppy.c` | Detects drive type, resets controller, takes IRQ 6. Read/write path never worked (no DMA start, wrong arg counts, inverted timeout). Needs 3 prototype fixes to compile today |
| PCI | `driver/pci.c` (reconstructed) | Config mechanism 1, bus 0, function 0 only, BAR sizing, enables IO/MEM/bus-master. **Verified** in QEMU: finds i440FX, PIIX3, VGA and the default e1000 NIC |
| 3c905B NIC | `driver/net/3c905b/` | Martin's "alpha" driver: EEPROM MAC read, MII, TX/RX descriptor rings in `palloc` memory, interrupt handler with debug prints, `iface` abstraction (`SnagPackets`/`SendPackets`/`Setting`). Registers the parallel-port ISR as its IRQ handler (debug leftover). QEMU does not emulate this card, so it is untestable there |

Driver init order is fixed in `driver/drivers.c` (reconstructed file).

## 9. The shell (`shell/main.c`)

**ai-dev**: a command table with argument splitting; `help` is generated from it.

| Command | Result |
|---|---|
| `help`, `clear`, `echo`, `tick`, `uptime`, `date`, `ps`, `kill`, `sleep`, `memstat`, `dmesg`, `irqstat`, `bootinfo`, `lpstat`, `test` | Work |
| `heaptest`, `selftest` | Heap churn; the kernel self test (tasks, sleep, timers, mutex, semaphore, event, formatter): prints `SELFTEST PASS` |
| `crashdiv`, `crashgp`, `crashnull`, `crashint` | Real exceptions in the shell task; the first three kill it and init restarts it, the last is reported and ignored |
| `funky`, `beep`, `hanoi`, `demo` | The 2004 demos (`shell/demos.c`) |
| `die` | Starts a ring-3 task; it faults at once until P1 adds user mode |
| `reboot` | Pulses the keyboard controller reset line |

## 10. Build system

One non-recursive `Makefile` at the top level (the 2004 per-directory Makefiles with
their GCC 3 flags were removed on `ai-dev`; `master` still has them). Objects and the
kernel go to `build/O<level>/`; `make O=2` builds an optimised kernel next to the
default `-O0 -g` one. `linker.ld` places the Multiboot header first, the `.ksyms`
symbol table after the data, and discards the sections a 2026 toolchain adds. The
kernel is linked twice so the symbol table can hold the final addresses. Targets:
`all`, `run` (QEMU, serial console on the terminal), `run-vga`, `test` (headless
smoke test through `tools/qemu-run.py`), `test-all` (-O0 and -O2), `unittest`
(blibc on the host), `debug` (gdb stub), `iso` (GRUB image), `run-iso`, `clean`.
`.github/workflows/ci.yml` runs the unit tests, the smoke tests and the ISO build.
See `CLAUDE.md` for the requirements and `docs/ROADMAP.md` Phase 0 for the flags.
