# SurfOS Code Audit (October 2026)

Findings from reading every source file on `master` (commit `cd518b5`) and from
building and booting the tree under QEMU with a current toolchain. Items marked
**verified** were reproduced; the rest are from reading. IDs (A, M, T, I, C, D, H)
are referenced by `docs/ROADMAP.md`.

## Status on `ai-dev`

Closed by Phase 0 and the serial console: **A1, A2, A3, A4, A5, A6, A8, A12**
(build system, flags, linker script, the compile errors, the -O2 memprobe miscompile),
**A7** (worked around with `-fgnu89-inline`), **C5** (`getch()` no longer discards
pending input), and the console backspace that erased the wrong cell.

Closed by milestone M1 (traps, GDT, libc, panic, hygiene): **A9, A10, A11, C3, C7**
(vsnprintf, standard strings), **I1, I2, I3, I4, I5, I7, I8, I9** (one stub per vector,
no EOI from exceptions, IRQ 15, full IDT, no reboot on IRQ 7, dumps with backtraces,
user descriptors kept, TSS loaded, CS reloaded), **M9** (GDT, IDT and page directory
in kernel memory), **T5, T6, T7, T8** (name overflow, deferred stack free through the
reaper, shell respawn, no `sti` with a null task), **H2, H4, H5, H6** (dead code,
file headers, REBUILD-NOTES.md, CI). **I10** is moot (all gates are interrupt gates).
Closed by milestone M2 (memory, scheduler, shell): **M1, M2, M3, M4, M5, M6, M7, M8,
M11, M12, M13** (new allocator, Multiboot map, VMM API, fault policy, page 0 unmapped),
**T1, T2, T3, T4, T6, T9, T10, T11, T13** (sleep list, blocking sleep and getch,
time slices, deferred stack free, per-task critical sections, console no longer forced
on switch, debug task hooks removed, idle only when nothing is runnable), **I11**
(calibrated delays), **C8** (RTC modes and century), **D2** in part (`dma_alloc`).
Closed by milestone M3 (devices): **C1, C2, C4, C6, C9** (ttys with a line editor and
history, console switching that sticks, full keyboard decoding, bounded `gets()`, four
consoles), **T9** (the input queue is filled under `irq_save()` and read the same way),
**T11** (no task creation or floppy hooks in the keyboard ISR), **D3** (full PCI
enumeration, BAR sizing with decoding off, device matching), **D6** (driver table with
status), **D4** in part (the 3c905B probes through `pci_find_device()`; the rest of the
card code is untestable without hardware). Roadmap D2 added the block layer, ramdisk,
ATA and MBR drivers on top. Still open: **T12** (turf is unused but still in the tree),
**D1** (floppy transfers), **D2** (the ISA DMA page math; the floppy is the only user),
**D5** (parallel port register addressing), **H1, H3**.
Milestone M4 added roadmap F1 (VFS, block cache, tarfs, FAT) and milestone M5 roadmap P1 and
P2 (user address spaces, system calls, ELF loader, user programs); the ring-3 descriptors
and TSS from **I8, I9** now carry real processes. Milestone M6 added roadmap N1 and N2 (e1000,
the IPv4 stack with TCP, DHCP and DNS); **D4** stays open for the 3c905B code itself, which
QEMU cannot exercise.
Found and closed by the riscv32 port (SurfCPU, October 2026): **C10** (the console lock in
`kernel/console.c`: one writer at a time on the grids, the screen and the serial mirror; a
task waits its turn with interrupts on, a whole `kprintf()`/`printf()` call is drawn as one,
an interrupt handler that finds the lock held leaves its characters for the holder; the
scroll moves words, not bytes). Checked by `scrolltest` (riscv32 kernel shell, under QEMU
slowed to SurfCPU's pace and in the fw emulator) and `tests/host/test_pager.c`.

Found and closed by milestone 8 of the riscv32 port (SurfCPU, October 2026: files on a card
that can be pulled out at any moment), **verified** with the card model's injected faults under
QEMU (`boards/qemu_virt/test_os.py`: refused writes, a card that stops answering, the card taken
out after the nth block); the files are shared, so both kernels have the fixes:

| ID | Where | Problem | Fix |
|---|---|---|---|
| B1 | `fs/bcache.c` (`reuse()`, `bsync()`) | A dirty block that had to make room and could not be written was dropped ("lost, and said so"): the next sync found nothing wrong, so the command that wrote never heard of it, and a dropped FAT or directory block let FAT read its old copy back and hand out clusters a file already had. `bsync()` sent a failed run again for each of its blocks (up to 128 tries and console lines in one sync) and returned a failure even when a later try had written everything | the block stays dirty while its device is there and is passed over when room is needed; when nothing else is free one refused run is tried again, else the caller gets an I/O error; `bsync()` tries each run once and fails when one did; the console hears of a device's first failure and of its first good write after |
| B2 | `shell/fscmd.c` (`cmd_eject()`) | `eject` unmounted a disk whose writes failed, which dropped the blocks it could not write, and said "safe to remove" | each mount is written out first; a disk that refuses keeps its mounts, and eject says so ("sync tries the writes again; umount gives them up") |
| B3 | `fs/bcache.c`, `fs/fat.c` | Dirty blocks reached the disk in the order of the buffer table: a card pulled during a sync could keep a new file's directory entry while its cluster was still free in the FAT, or the entry and the FAT without the file's bytes (a file reading another's old data); unlink freed the clusters before the entry was gone | a write order (file data, FAT, directory entries, FSInfo: `bdirty_order()`), kept by `bsync()` and when room is made; FAT's order points (a sync) take an entry away before its clusters are freed and put a new name on the disk before the old one goes. What FAT32 can still leave behind is in `surfcpu/docs/CARD.md` section 8 |
| B4 | `fs/fat.c` (`run_blocks()`, `fat_rw()`) | A write allocates a run of clusters ahead of its data; when the volume ran out in the middle of a run (or a block could not be written) the clusters already linked stayed in the file's chain past its size, and the write returned ENOSPC without the bytes that fitted: fsck.fat found the chain longer than the file and the free count low (**verified** under QEMU with a nearly full stick, `boards/qemu_virt/test_os.py`) | a run ends where the volume does, so a write puts in what fits and returns its bytes (ENOSPC on the next); after a write that stopped early the chain is cut back to the file's size. `cp` (both shells) says why a write came up short |

Found by a review of B1-B4 (October 16, 2026) and fixed; every one is **verified** by
`tests/host/test_fatfs.c` (`make unittest`: the real `fs/bcache.c`, `fs/fat.c` and `fs/fat_mkfs.c`
over RAM disks that refuse writes on request, with a check of the image after every write the disk
stores), which fails on the code before the fixes (`surfcpu/docs/evidence/os/bcache-review.txt`):

| ID | Where | Problem | Fix |
|---|---|---|---|
| B6 | `fs/bcache.c`, `fs/fat.c` | The failed blocks B1 keeps were never reused and the cache is shared: a device that refused every write filled it (about 512 KB of writes), and then every read of every device failed ("every buffer is busy"), the initrd's included, until a sync or umount; each such miss also sent one more write bound to fail. A block held back because something below it failed was marked failed without being tried; a failed run was tried again only whole, so one bad block held back its 127 neighbours for good; read-ahead stopped at the first dirty buffer of the old end of the list, so one failed block turned it off for every device | a device that refuses (one of its blocks failed) has its dirty blocks held back: making room tries none of them, and no block is marked failed that was not tried; a device keeps at most a quarter of the cache dirty (one that works writes some out first; one that refuses gets EIO for new writes: `bcache_room()`, `bget()`); a sync tries a failed run again block by block, once; read-ahead looks past dirty buffers |
| B7 | `fs/bcache.c` | B3's order was kept by the callers that sorted blocks, not where writes are issued: blocks dirtied by another task after `bsync()` had passed their order (a close's sync while another task wrote) went out behind the higher orders that pointed at them, and so did those of a block made dirty while making room wrote below it; a block could also change while it was being written | `write_run()` sends a block of order k only when its device has no dirty block below k (the device's account, under the lock, just before the request), for every caller; `bsync()` goes round again when blocks below are dirtied meanwhile; a held block is not written and a block being written is not handed out |
| B8 | `fs/bcache.c` (`evict_write()`) | Making room wrote a block's lower orders first, which let the lock go, then wrote the block without looking at it again: written or dropped meanwhile, it went out as a zero-length request (and "takes writes again" was said of a device that took nothing); taken for another device's block, that block was written without its own device's order | the block is written only if it is still the same dirty block; `write_run()` sends no empty request |
| B9 | `fs/bcache.c`, `include/sys/bdev.h` | The console's "write of ... failed" was said once per device through a table of eight devices that only a write cleared: a device that failed and was then unplugged kept its slot for ever, and after eight such devices every failure was printed and "takes writes again" never was | each device carries the cache's account of it (`bc_dirty[]` by order, `bc_failed`, `bc_said`), kept at every change of a block's state and gone with the device; `bcache_audit()` checks the account against the blocks |

Found and fixed by the USB boot work (milestone M7, October 2026), all of them harmless
under QEMU's loader and dangerous on a UEFI PC:

| ID | Where | Problem | Fix |
|---|---|---|---|
| U1 | `include/mm/memory.h` | page tables, frame stack and DMA heap fixed at 2-15 MB without asking the memory map; OVMF keeps ACPI NVS at 8-9 MB, GRUB may put modules there | `pmm_init()` places them in available RAM, reserved explicitly (`mm/memrange.c`, host-tested) |
| U2 | `mm/pmm.c` | RAM total summed into a 32-bit `u_long` (a region above 4 GB wrapped) | 64-bit totals, RAM above 4 GB reported separately |
| U3 | `driver/parport.c` | LPT1 address read as 32 bits from the BIOS data area and trusted; on OVMF it read 0x1f055913, and init then wrote to I/O port 0x5913 | 16-bit read, only 0x378/0x278/0x3BC accepted; skipped with `profile=laptop` |
| U4 | `driver/serial.c` | the receive loop ran while the line status had bit 0 set: an absent UART reads 0xFF, an endless loop in the ISR | COM1 probed (scratch register), loop bounded |
| U5 | `kernel/sys.c`, `lib/blibc/time.c` | `while(inb(0x64) & 2)` and `while(cmos_busy())` never end on a machine without the device | bounded; reset also tries the ACPI reset register, 0xCF9 and a triple fault |
| U6 | `kernel/cpu.c` (new) | CR0/CR4 inherited from the boot loader: GRUB on OVMF hands over CR0.WP set, and firmware may set SMAP or PVI | both registers set explicitly at boot, the originals shown by `cpuinfo` |
| U7 | `kernel/keyboard.c` | the ISR read port 0x60 whatever the status said | reads only when a keyboard byte is waiting, so polling and the IRQ never take one twice |

## What was verified under QEMU

Built with GCC 13.3 (`-m32`), binutils 2.42 and NASM 2.16, run with
`qemu-system-i386 -m 64 -kernel surfos.bin`, screen read back through the monitor
(`pmemsave 0xb8000 4000`):

| Check | Result |
|---|---|
| Build with the original Makefiles | Fails immediately: `-fwritable-strings` unknown, `pushl` rejected by the 64-bit assembler |
| Build with modern flags + linker script | 3 hard compile errors (A3, A4), then links to a 69 KB Multiboot ELF |
| Boot | Reaches the shell prompt in well under a second. PCI scan finds i440FX, PIIX3, VGA, e1000 |
| `ps`, `memstat`, `tick`, `test`, `kalloc`, `pl`, `lpstat`, `help` | Work. 50172 KB detected on a 64 MB guest; the tick count is consistent with 100 Hz |
| `funky` then `ps` | Second task runs; three tasks listed |
| `demo` -> `1` -> `1` (`int 1` in the shell) | "Process ('Shell 0':1) killed by "Debug Exception"", then only the idle task remains |
| `die` (ring-3 task) | Killed at once by a General Protection Fault |
| F2 | Screen goes blank and stays blank; shell still answers on console 0 |
| 30 s idle, then `tick` | Still responsive, no reset |
| Same source at -O2 | Halts during floppy init: "switch_int_task: null current task". Cause: A6. Boots after A5 + A6 fixes |

## A. Toolchain and build

| ID | Where | Problem | Fix |
|---|---|---|---|
| A1 | every Makefile | `-fwritable-strings` was removed in GCC 4.0 (2005) | drop it; nothing in the tree writes to string literals |
| A2 | every Makefile, `kernel/Makefile:22` | 32-bit kernel built on a 64-bit host with no linker script. Modern GCC also defaults to PIE, stack protector, CET and SSE | `-m32 -fno-pie -fno-stack-protector -fcf-protection=none -fno-asynchronous-unwind-tables -mno-sse -mno-mmx`, `ld -m elf_i386 -T linker.ld`, `nasm -felf32`, discard `.note*`/`.eh_frame` |
| A3 | `mm/paging.c:151` | `*pd = (u_long)pTmp = ...` assigns through a cast (GCC 3 extension) | `pTmp = (u_long*)(...); *pd = (u_long)pTmp \| 3;` |
| A4 | `driver/floppy/floppy.c:46,50,79,86,119` | `fd_rw`, `fd_seek`, `fd_recalibrate`, `dma_xfer`, `dma_alloc`, `memcpy` are used before any declaration, and `fd_rw`/`fd_seek` are called with the wrong number of arguments | add prototypes to `include/sys/floppy.h` and `include/sys/dma.h`, pass the drive number |
| A5 | `kernel/gdt.c:125`, `kernel/interrupt.c:126` | `asm("lgdt (%0)" :: "p"(&gdtr))` is rejected at -O2 ("invalid expression as operand") | `asm volatile("lgdt %0" :: "m"(gdtr))` |
| A6 | `mm/paging.c:41-53` | `memprobe()` writes a magic value through a non-volatile pointer and reads it back; at -O2 the read-back is folded away, the loop runs to the 3 GB limit, and the page allocator hands out frames that do not exist, so the first heap write is lost and `kalloc()` returns NULL (**verified**) | `volatile`, and really: use the Multiboot memory map (M7) |
| A7 | 11 functions in `mm/*.c`, `kernel/task.c`, `kernel/timer.c`, `kernel/console.c` | `inline` without `static` has C99 semantics in modern GCC: no external symbol is emitted, so every caller in another file gets an undefined reference | `-fgnu89-inline` for now; `static inline` in headers or plain functions later |
| A8 | every Makefile | `.c.o: $(CC) $(CFLAGS) -c $< -o $@` on one line is a prerequisite list, not a recipe; GNU make silently uses its built-in rule | one top-level Makefile with real pattern rules |
| A9 | `kernel/assem.asm:13` | `GLOBAL defISR` names a symbol that is never defined (NASM 2.16 tolerates it) | delete |
| A10 | `kernel/setjmp.asm` | Not linked. Pre-Multiboot entry code with its own page tables and VGA routines | delete, or keep under `attic/` |
| A11 | `lib/blibc/printf.c:10`, `kernel/console.c:244,280` | Varargs are read by walking the stack from `&format`. Undefined behaviour that happens to work on i386 cdecl at -O0 and -O2. Formats support `%d %i %u %x %s` only; `%02x` (used in `floppy.c:131`) prints garbage, `%c` works by accident | one `vsnprintf` on `__builtin_va_list`, used by `kprintf`, `printf`, `kcprintf` |
| A12 | `kernel/Makefile:24`, `Makefile:40-45` | `-Ttext 0x100000` with no linker script; `copy`/`bochs` targets mount a floppy and need a missing `bochsrc` | linker script, `run`/`test`/`iso` targets |

## M. Memory management

| ID | Where | Problem | Consequence |
|---|---|---|---|
| M1 | `mm/kalloc.c:120`, `mm/palloc.c:39` | Best-fit loop reads `best->size` while `best` is still NULL | Reads physical address 4 (the real-mode IVT). Works only because page 0 is identity mapped; -O2 may legally miscompile it |
| M2 | `mm/kalloc.c` | No splitting, no coalescing, free blocks reused only when at least as large as the request, `ksbrk` never shrinks | Heap fragments and leaks under churn; the 2004 TODO's "60,000 process kills hang" is this |
| M3 | `mm/kalloc.c:181` | Allocation descriptors come from a separate bump heap (`asbrk`) and are never freed | Every allocation of a new size costs 12 bytes forever |
| M4 | `mm/kalloc.c:146`, `delAllocItem` | `kfree` is an O(n) walk of the allocated list for every free | Slow with many objects |
| M5 | `mm/memory.c:68` | `kByteFree+=kByteFree` | "Freed kernel memory" is always 0 (**verified**) |
| M6 | `mm/paging.c:163` | Page fault handler maps any address, ignores the error code, has no owner/permission concept, and `BUG()`s on frame exhaustion | A wild pointer write anywhere in 4 GB silently gets a page; out-of-memory halts the machine |
| M7 | `mm/paging.c:37`, `kernel/main.c:24` | RAM is sized by probing instead of using the Multiboot info that `boot.S` already pushes to `kmain` (ignored) | Unsafe on real hardware (MMIO holes), wrong at -O2 (A6), caps at 3 GB |
| M8 | `mm/memory.c:21` | The 32 MB check compares `PMEM_SIZE` (which starts at 15 MB) against 16 MB | Effectively requires 31 MB; fine, but misleading |
| M9 | `include/surfos/gdt.h:14`, `include/surfos/interrupt.h:16`, `include/mm/memory.h:63` | GDT, IDT and the page directory live at fixed low addresses (0x6000, 0x6800, 0x9C000) | 0x9C000 is inside the EBDA on many real machines; the IDT is never cleared (I4) |
| M10 | `kernel/task.c:229` | Task stacks are 4 KB heap blocks with no guard page | `shell/main.c:243` declares an 8 KB local array (unreachable today); `gets()` has no bound (C5) |
| M11 | `mm/palloc.c`, `driver/dma/dma-mm.c` | Three copies of the same allocator (`kalloc`, `palloc`, `dma_alloc`) | Bugs fixed in one are not fixed in the others |
| M12 | `driver/dma/dma-mm.c:29` | `while(!cur) cur = cur->next;` | NULL dereference loop the first time `dma_alloc()` is called |
| M13 | `mm/paging.c:144` | `map_page` never invalidates the TLB | Fine today (only not-present to present), required once pages are unmapped or remapped |

## T. Tasks and scheduling

| ID | Where | Problem | Consequence |
|---|---|---|---|
| T1 | `kernel/task.c:444-448` | The SLEEPING walk calls `wake_task()`, which moves the task to another queue, then reads `slpTmp->next` from the new queue. `timeleft` is unsigned so `<= 0` means `== 0` | At most one task wakes per tick; a sleep that is not a multiple of 10 ms underflows to about 49 days |
| T2 | `kernel/sys.c:22` | `sleep()` busy-waits on `getticks()`; its argument is milliseconds despite the name; `sleep_task()` is never called by anyone | Sleeping tasks burn their whole quantum; the speaker tune in `funky` spins a core |
| T3 | `lib/blibc/chars.c:20` | `getch()` busy-waits on the key queue | The idle shell consumes CPU; no blocking I/O primitive exists |
| T4 | `kernel/task.c:440` | No time slice: every tick switches tasks regardless of priority; FIFO tasks are never preempted | Priority only changes the pattern of 10 ms turns; a FIFO task can lock the machine |
| T5 | `kernel/task.c:258,271` | `memset(nTask,0,sizeof(nTask))` clears 4 bytes; `*(name + strlen + 1) = 0` writes one byte past the allocation | Latent corruption of the next heap block |
| T6 | `kernel/task.c:81,294,306` | `kill_task(curTask)` frees the stack that is still in use, then `yield()` pushes a frame onto it | Works only because freed memory stays mapped and nothing reallocates in between |
| T7 | `kernel/task.c:433` | Nothing supervises the shell | Any exception in the shell leaves a machine with only the idle task (**verified**) |
| T8 | `kernel/task.c:315,348`, `kernel/panic.c:138` | `delete_task(curTask)` sets `curTask = NULL`; `panic()` then does `sti` and `yield()`. An IRQ in that window hits `switch_int_task`'s `if(!curTask) BUG()` | Rare halt when an exception kills a task while IRQs are busy |
| T9 | `kernel/task.c:474`, everywhere | `KCRIT_ENTER` is a counter that blocks preemption; it does not disable interrupts. `pop_key_queue` (IF=1) and the keyboard ISR's `push_key_queue` both rewrite `keyQueue` | Lost or duplicated keystrokes under load (the 2004 TODO mentions this) |
| T10 | `kernel/task.c:463` | `schedule()` forces `conActive = curTask->con` | Defeats console switching (C1) |
| T11 | `kernel/keyboard.c:261-267` | The keyboard ISR calls `new_task()` (heap allocation), `reboot()` and the floppy motor control on function keys | Debug hooks in an interrupt handler |
| T12 | `kernel/turf.c:46-49` | `turfTread()` waits by `yield()`ing in a loop and prints "TURF: HAD TO WAIT!" | Spin-yield lock with console output in the slow path; no owner tracking |
| T13 | `kernel/task.c:322` | `getNextTask()` returns NULL for empty levels and the caller spins `while(... == NULL)` | Correct only because the idle task is always runnable at LOW |

## I. Interrupts, exceptions, descriptor tables

| ID | Where | Problem | Consequence |
|---|---|---|---|
| I1 | `kernel/assem.asm:397,431,447,463,543` | Vectors 8, 10, 11, 12 and 17 push an error code; only `ex13` drops it (`add esp,4`) and `ex14` after `POPREGS` | The C handler sees a shifted `surf_regs`; `iret` returns into the error code; a double fault becomes a triple fault |
| I2 | `kernel/assem.asm` every `exN` | Exception stubs send an EOI to the master PIC | Can acknowledge an unrelated in-service IRQ |
| I3 | `kernel/interrupt.c:88` | `for(i=1;i<NR_IRQS;i++)` with `NR_IRQS = 15` installs IRQ 1..14 | IRQ 15 (secondary ATA) has no IDT entry |
| I4 | `kernel/interrupt.c:53` | The IDT at 0x6800 is never zeroed; vectors 19-31, 0x2F-0x3F, 0x41-0xFF are whatever was in memory | Any stray interrupt or `int n` jumps into garbage |
| I5 | `driver/parport.c:129,139`, `driver/net/3c905b/wnet.c:389` | `parPortISR()` is `reboot()`, installed on IRQ 7 and (by the NIC driver) on the NIC's IRQ | A spurious IRQ 7, which the 8259 generates on glitches, reboots the machine |
| I6 | `kernel/panic.c:119-142` | `panic()` prints with interrupts off, kills the task, then `sti` + `yield()` with `curTask == NULL` (T8) | Window for a BUG halt |
| I7 | `kernel/panic.c:153` | Register dump labels `swapCount` as the pid and omits EIP's symbol, CR2, EFLAGS, the error code and any backtrace | Hard to debug faults |
| I8 | `kernel/gdt.c:84` | `for(lv0 = 3; lv0 < 256; lv0++)` blanks entries 3 and 4 right after writing the ring-3 descriptors | User selectors 0x1B/0x23 are null; `die` faults (**verified**). `getNewRegs()` also uses DS=0x43 (index 8) for ring 3 |
| I9 | `kernel/gdt.c` | No TSS and no `ltr`; CS was never reloaded with a far jump after `lgdt` (QEMU's loader enters with CS=0x08, GRUB 2 with CS=0x10, so the GRUB boot faulted on the first `iret`; fixed on `ai-dev`); code limit is 0xF0FFF pages, not 4 GB | Ring 3 cannot work |
| I10 | `kernel/timer.c:29` | The timer uses a trap gate (IF stays set) and relies on the stub's `cli` | Inconsistent with the interrupt gates used elsewhere |
| I11 | `kernel/interrupt.c:215`, `kernel/sys.c` | `irq_delay(100)` is 10,000 `nop`s; `sysbeep`, `sleep`, `parSendByte` all busy-wait | No calibrated delay primitive |

## C. Console, keyboard, libc

| ID | Where | Problem |
|---|---|---|
| C1 | `kernel/console.c:72`, `kernel/task.c:463` | Console switch happens in the ISR and is undone by the scheduler; the display and the active console disagree (**verified**) |
| C2 | `lib/blibc/strings.c:13` | `puts()` re-implements character output with its own rules (no `\b`, no `\t`, writes video memory directly), so `printf` and `kprintf` behave differently |
| C3 | `lib/blibc/string.c:15,25` | `strcmp` compares lengths first and returns -1 for any difference; `strncpy` writes a terminator at `dst[n]`. Neither is the C function. Missing: `strncmp`, `strcat`, `memcmp`, `memmove`, `strtol`, `atoi`, `snprintf` |
| C4 | `lib/blibc/strings.c:72`, `shell/main.c:537` | `gets()` has no length bound; the shell's 255-byte line buffer sits on a 4 KB task stack |
| C5 | `lib/blibc/chars.c:22`, `kernel/keyboard.c:130` | `getch()` discards pending input on entry; the queue is shifted one byte at a time per key |
| C6 | `kernel/keyboard.c:206` | No E0-prefixed keys (arrows alias the keypad), caps lock does not affect letters, no num lock, no key-release events to consumers, LED writes poll the controller while interrupts can interleave |
| C7 | `kernel/console.c:162` | `itoa` treats `%d` as unsigned (negative numbers print as large values) and `%u` as decimal `d` |
| C8 | `include/time.h:16` | `time_t` is a struct here (collides with the C type name); `get_time()` never checks CMOS status B for BCD/24-hour mode |
| C9 | `kernel/console.c:26` | Console count is fixed at 3 (`NUM_CONSOLES`); the 2004 TODO notes odd output with more. No scrollback |
| C10 | `kernel/console.c` (`kputch()`, `sscroll()`) | Nothing keeps two tasks apart on a console: `kputch()` sends a character to the serial mirror, then changes the grid, and `sscroll()` moves the grid up one byte at a time with preemption on, so a task preempted inside a scroll lets another task's line land in rows that the scroll then moves or clears; two tasks' lines can also split each other. Latent on x86, where a scroll takes microseconds; **verified** on SurfCPU, where one takes about a timer tick (the RTL system boot and the fw emulator: blank, half-written and doubled rows during `tasks`, the serial line whole) |

## D. Drivers

| ID | Where | Problem |
|---|---|---|
| D1 | `driver/floppy/floppy.c` | Never transferred a sector: `dma_xfer()` computes a page/offset and stops (`dma.c:34`); `fd_wait()` timeout test is inverted (`tmout < getticks()`, line 196) and `if(!tmout)` can never be true; `fd_seek()` returns false on success (line 267-270); `fd_rw()` leaves the motor ON (line 123); `tbaddr` is NULL on reads; geometry is hard-coded 1.44 MB |
| D2 | `driver/dma/dma.c:40`, `dma-asm.asm:4` | `LoadPageAndOffset()` derives the page register from address bits 24-27 instead of 16-23 (only correct below 64 KB); `dma_stuff` pops its return address as the port number, so `DMAComplete()` returns to garbage |
| D3 | `driver/pci.c` (reconstructed) | Bus 0, function 0 only; no bridges or multifunction devices; BAR sizing is done on live devices without disabling decode; `class` keeps 16 of 24 bits; no bus number in `pci_dev`; no device-to-driver matching; `include/sys/pci.h:327` declares `static` functions in a header |
| D4 | `driver/net/3c905b/` | Untestable in QEMU (no 3c905B model); interrupt handler full of debug prints; `set_media_type()` has a format/argument mismatch (`wnet.c:954`) and `outw(inw(...) & ~0x80, ioaddr+10)` has swapped arguments (`wnet.c:960`); `init_3c905b()` trusts all 32 `pci[]` entries to be non-NULL; `fake_inter()` (shell `inter`) dereferences a NULL `gcard` without a card |
| D5 | `driver/parport.c:62-109` | Every `parSetX(port, ...)` writes to `PAR0` instead of `port`; the base address comes from the BIOS data area only (LPT1) |
| D6 | `driver/drivers.c` | Fixed call list, no return codes, no registration or probe model; the e1000 that QEMU provides is found by the PCI scan and ignored |

## H. Hygiene

| ID | Problem |
|---|---|
| H1 | `include/surfos/types.h`: `bool` is `unsigned int`, `true` is `!false`, `size_t` is a typedef of `unsigned int`; no fixed-width types |
| H2 | Dead code: `kernel/setjmp.asm`, `keybISR`/`isrParPort`/`preempt` in `assem.asm`, `runTest`/`recurse`/`runTest2` in the shell, `net_3c905b_probe/detach`, empty `include/surfos/check.h`, `printInfo`/`parseCPUID`/`print_bits`/`removeHandler` declared but never defined |
| H3 | `driver/PCIDATA.H` (5,836 lines) is included by nothing; it is exactly what an `lspci` command needs |
| H4 | File headers are copy-pasted (`floppy.c` says "Parallel Port Driver", `floppy.h` says `driver.h`, `keyboard.h` says `template.h`); tabs and spaces are mixed; naming mixes camelCase and snake_case |
| H5 | 13 reconstructed files reference a `REBUILD-NOTES.md` that is not in the repository |
| H6 | No tests, no CI, no emulator target, no `.gitignore` |
