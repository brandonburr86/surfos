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
| CPU mode | 32-bit protected mode, paging on, single CPU; **ai-dev**: CR0/CR4 set explicitly, x87 initialised |
| Boot | Multiboot 1 (GRUB or `qemu -kernel`), ELF image linked at 1 MB; **ai-dev**: also Multiboot 2 (GRUB on UEFI, the USB image, section 14) |
| Kernel style | Monolithic; the debug shell runs in ring 0, programs run in ring 3 behind `int 0x80` |
| Segmentation | Flat: kernel code 0x08, data 0x10; user code 0x1B, data 0x23; one TSS (0x28) whose esp0 is the current task's kernel stack |
| Memory | Identity map of the low 16 MB, demand-paged kernel heap at 0xB0000000, a page directory per process with the user range 0x40000000-0x7FFFFFFF |
| Scheduling | Pre-emptive, 100 Hz PIT, software stack switching, 4 run levels + sleeping + removal queues |
| Interrupts | Two 8259 PICs remapped to 0x20-0x2F, shared IRQ handler chains, exceptions kill the current task; **ai-dev** `profile=laptop`: the 8259s reach the CPU through the local APIC (virtual wire), whose timer is the fallback tick |
| Devices | VGA text console, or **ai-dev** a framebuffer console on UEFI (4 virtual consoles, each a tty with a line editor) mirrored to a COM1 serial console when there is one, PS/2 keyboard plus serial input, PIT, CMOS RTC, parallel port, 8237 DMA (incomplete), floppy (incomplete), full PCI enumeration with names, block layer with ramdisks from boot modules, ATA PIO disks and MBR partitions, 3Com 3c905B NIC (no protocol stack) |
| libc | `blibc`: printf/snprintf, puts/gets (through the tty), getch, the standard string and memory functions, strtol, ctype, CMOS time |
| Files | VFS with a mount table; tar file system for the initrd (read-only), FAT12/16/32 with long names (read and write); 128-block write-back cache |
| Processes | ELF32 executables loaded from any mounted file system, argc/argv, exit codes, wait, 21 system calls, a user libc (`user/libsurf`) and a user-mode shell |
| Network | e1000 driver; Ethernet, ARP, IPv4, ICMP, UDP, TCP, a DHCP client and a DNS resolver in a kernel thread; `ifconfig arp ping dhcp nslookup tcpecho httpget netstat` |
| User interface | Ring-0 debug shell with about 50 commands; `run`/`spawn`, or type a program's name |

Required RAM is 32 MB (`mm/memory.c:21`). The kernel version string is 0.007 and
the shell calls itself v0.008.

## 2. Source tree

```
boot/        boot.S (Multiboot header + entry), multiboot.h
kernel/      main.c (kmain), gdt.c (GDT + TSS), interrupt.c (IDT, PICs, dispatch, IRQ chains),
             traps.asm (256 entry stubs), task.c (scheduler), timer.c (PIT), panic.c (exceptions, panic),
             ksym.c (symbol lookup, backtraces), klog.c (kernel log), console.c (VGA + kprintf),
             tty.c (input queues, line editor, console switching), keyboard.c (scan codes, LEDs),
             process.c (spawn, ELF loader, entering ring 3), syscall.c (int 0x80 dispatch),
             sys.c (sleep/beep/reboot), turf.c (Martin McCormick's reader/writer "turf" locks)
mm/          memory.c (init), pmm.c (frame stack), paging.c (page tables, page-fault handler, ioremap),
             kalloc.c (kernel and DMA heaps), uvm.c (per-process address spaces)
lib/blibc/   chars.c printf.c strings.c string.c memory.c ctype.c time.c
shell/       main.c (the shell), demos.c, selftest.c, parport.c (lpstat)
fs/          vfs.c (mounts, paths, open files, descriptors), bcache.c, tarfs.c, fat.c, devfs.c (/dev)
net/         netbuf.c (buffers, devices, the net thread), eth.c, arp.c, ip.c, icmp.c, udp.c, tcp.c, dhcp.c, dns.c
user/        user.ld, libsurf/ (crt0.S, syscalls.c), include/surf.h, bin/ (hello crash cat ls count sh)
driver/      drivers.c (driver table), pci.c + pci_names.c + PCIDATA.H, serial.c, bdev.c (block layer),
             ramdisk.c, ata.c, mbr.c, net/e1000.c, parport.c, dma/, floppy/, net/3c905b/ (2004, unwired)
include/     surfos/ (kernel headers), mm/, sys/ (driver headers), net/, asm/io.h, blibc headers
tools/       qemu-run.py (headless QEMU harness), gensyms.py (symbol table for backtraces),
             mkimage.py (test disk image and initrd)
rootfs/      packed into build/images/initrd.tar, the Multiboot module behind rd0
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
       init_console()             VGA text mode, 4 virtual consoles, console 0 active
       init_tty()                 one tty per console (input ring, readers' wait queue, history)
       run_memcheck()             halts below 32 MB
       init_interrupt()           IDT with 256 stubs, PIC remap, exception handlers
       init_delay()               calibrates udelay()/mdelay() on PIT channel 2 (no interrupts needed)
       init_task()                6 run queues, int 0x40 = yield, idle task (pid 0), "Shell 0" task (pid 1)
       init_keyboard()            IRQ1 handler
       init_drivers()             the driver table: serial IRQ 4, parport, dma, floppy, pci, 3c905b, ramdisk, ata, e1000
       init_net()                 the "net" kernel thread (frames queue up until the scheduler starts)
       init_fs()                  block cache, rootfs on /, /dev, the first tar ramdisk on /initrd, every FAT volume on /<device>
       init_syscalls()            int 0x80 handler
       init_timer()               PIT 100 Hz, IRQ0 = timerISR
       for(;;) hlt                this loop IS the idle task; the first tick saves its ESP into task 0
```

The messages printed by `init_gdt()` and `init_mem()` only appear on the serial
console: the video console is initialised after them and `kputch()` has nowhere else
to put output while `conActive` is NULL. (On `master` they were lost entirely.)

**ai-dev** (USB boot, 10/2026): `kmain()` now reads, in order: `init_serial()` (probes
COM1 first), `init_gdt()`, `mb_init()` (Multiboot 1 or 2 by the magic number),
`init_cpu()` (CPUID, CR0/CR4, 4 MB pages, PAT), `init_mem()` (arenas from the memory map,
section 4), `init_console()` (VGA text, or the framebuffer), `console_replay_log()` (the
lines above, from the kernel log, onto the new screen), `mb_print()`, a check of the boot
information, `run_memcheck()`, `init_himem()`; with `profile=laptop` `init_acpi()` and,
after `init_interrupt()`, `init_apic()`; then as above, ending with `init_timer()` and
`timer_confirm()`, which waits up to 100 ms for the first tick (on a laptop it starts the
local APIC timer when the PIT's never comes, otherwise it stops with a message).

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
| 0x00100000 - ~0x00190000 | Kernel image | .text, .rodata, .data, .ksyms (symbol table), .bss (16 KB boot stack, GDT, IDT, page directory) |
| placed at boot (**ai-dev**) | Page tables | 4 MB = 1024 tables, covers the whole 4 GB; every directory entry points here from boot |
| placed at boot (**ai-dev**) | Free-frame stack | 4 bytes per frame of the memory map, popped top-down (`mm/pmm.c`) |
| placed at boot (**ai-dev**) | DMA heap | 5 MB, `kalloc_dma()`, 1:1 mapped, for anything a device reads or writes |
| the rest of RAM below 4 GB | Free page frames | From the memory map, minus the reserved ranges (low memory and the kernel, modules, framebuffer, arenas) |
| 0x40000000 - 0x7FFFFFFF | User range (per process) | ELF image from 0x40000000, sbrk heap above it, stack growing down from 0x80000000 (up to 4 MB); `mm/uvm.c` |
| 0xB0000000 - 0xBFFFFFFF | Kernel heap (`kalloc()`) | Pages mapped on first touch by the page fault handler |
| 0xC0000000 - 0xCFFDFFFF | `ioremap()` window | Device registers (uncached) and boot modules above 16 MB |
| 0xCFFE0000 - 0xCFFFFFFF | `vmm_temp_map()` (**ai-dev**) | One read-only view of a firmware table at a time (ACPI) |
| 0xD0000000 - 0xDFFFFFFF | `vmm_map_large()` (**ai-dev**) | The framebuffer (write-combining) and 4 MB of RAM above 4 GB for `himem`: 4 KB pages below 4 GB, PSE-36 4 MB pages above |

**ai-dev** (USB boot): the page tables, the frame stack and the DMA heap used to be fixed at
2-15 MB. That was always free under QEMU's loader, but OVMF keeps ACPI NVS data at 8-9 MB
and GRUB on UEFI may put the initrd anywhere. `pmm_init()` now puts each arena in the
lowest free stretch of available RAM between 1 MB and 1 GB and identity-maps it; the
kernel image and every module are checked against the map. RAM totals are 64-bit
(`memstat` reports RAM above 4 GB, which only the frame allocator cannot use).

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

* `console.c` keeps four `surf_console` buffers plus `conVideo` for 0xB8000.
  Output goes to the task's console and, if that console is active, to video memory.
  **ai-dev**: when the boot loader leaves a linear framebuffer (UEFI), `conVideo` is a
  buffer in RAM and `driver/framebuffer.c` draws the cells that changed into the
  framebuffer with an 8x16 font (`driver/font8x16.c`, Spleen), scaled by a whole number and
  centred, with a software cursor; `driver/fbcore.c` is the renderer, tested on the host.
  On such a boot 0xB8000 and the CRTC ports are never touched.
  Every character funnels through `kputch()`. `printf()`, `puts()`, `cputs()` and,
  from task context, `kprintf()` print on the calling task's console (`kprintf()`
  in yellow, boot code and interrupt handlers on the active console), so a shell
  on console 3 sees its own `ps` and its own fault report.
* Serial console (`driver/serial.c`): `kputch()` mirrors everything written to the
  active console to COM1 (newline becomes CR LF, backspace becomes "\b \b", a console
  clear becomes an ANSI clear). IRQ 4 feeds received bytes to the active tty (CR to
  newline, DEL to backspace, `ESC [ A/B/C/D/H/F` to the arrow/Home/End key codes).
  `qemu -nographic` therefore gives a complete terminal session; `tools/qemu-run.py`
  drives it. After a console switch the serial terminal is cleared and repainted
  from the new console's buffer.
* `kernel/tty.c` (roadmap C1): one `struct tty` per console with a 256-byte input
  ring filled by the keyboard and serial ISRs under `irq_save()`, a wait queue for
  readers (`tty_getc()` blocks, `tty_trygetc()` polls), and a line discipline
  `tty_readline()` with echo, backspace, Ctrl-U, Ctrl-C and an eight-line history on
  the Up/Down keys. `gets()`/`cgets()` and the shell read through it; `getch()`
  blocks on the task's own tty. `tty_switch(n)` makes console n active, repaints
  the VGA screen and the serial terminal; F1-F4 call it from the keyboard handler.
* Keyboard (`kernel/keyboard.c`, roadmap C2): scan-code set 1 with the E0 prefix
  (arrows, Home/End, PgUp/PgDn, Insert/Delete), shift, caps lock on letters, num
  lock on the keypad, Ctrl-letter control characters, LEDs, Ctrl-Alt-Del reboots.
  Special keys arrive in the tty as codes above 0x80 (`include/surfos/keyboard.h`).
  The ISR does nothing but decode and queue; the 2004 debug hooks are gone.

## 8. Drivers

| Driver | Files | State |
|---|---|---|
| PIT timer | `kernel/timer.c` | Works, 100 Hz, `getticks()` |
| PS/2 keyboard | `kernel/keyboard.c` | Full set-1 decoding with E0 keys, lock keys and LEDs; feeds the active tty |
| Serial console | `driver/serial.c` | COM1, 115200 8N1, polled TX, IRQ 4 RX. Mirrors the active console, decodes arrow-key escapes, feeds the active tty. Added on `ai-dev` |
| VGA text | `kernel/console.c`, `kernel/tty.c` | Four consoles, switching with F1-F4 (VGA and serial repaint) |
| CMOS RTC | `lib/blibc/time.c` | Reads BCD time; the shell `time` command is commented out |
| Parallel port | `driver/parport.c`, `shell/parport.c` | Status readout works in QEMU. Its IRQ 7 handler is `reboot()` |
| ISA DMA | `driver/dma/` | Register helpers only. `DMAComplete()`, `dma_alloc()`, `dma_xfer()` are broken |
| Floppy | `driver/floppy/floppy.c` | Detects drive type, resets controller, takes IRQ 6. Read/write path never worked (no DMA start, wrong arg counts, inverted timeout). Needs 3 prototype fixes to compile today |
| PCI | `driver/pci.c`, `driver/pci_names.c`, `driver/PCIDATA.H` | Config mechanism 1; every bus behind a bridge, every function; class, IRQ line/pin, BARs sized with decoding off; `pci_find_device()`, `pci_find_class()`, `pci_enable_device()`; vendor/device/class names from the 2003 id tables (`lspci`). **Verified** in QEMU: i440FX, PIIX3, PIIX4 PM, VGA, 82540EM |
| Block layer | `driver/bdev.c`, `include/sys/bdev.h` | Named 512-byte block devices with read/write ops; partitions translate to their parent; range checks and per-device counters (`lsblk`, `hexdump`) |
| Ramdisk | `driver/ramdisk.c` | Every Multiboot module is a writable ramdisk `rd0`, `rd1`, ... (`qemu -initrd`, GRUB `module`); modules above 16 MB are mapped with `ioremap()` |
| ATA | `driver/ata.c` | Polled PIO, LBA28, both legacy channels, IDENTIFY, cache flush after writes, a mutex per channel; disks are `hda`-`hdd`, ATAPI devices are reported and skipped |
| MBR | `driver/mbr.c` | The four primary partitions of each disk become `hda1`-`hda4` with their type |
| e1000 NIC | `driver/net/e1000.c` | Intel 82540EM (QEMU's default): MMIO through `ioremap()`, 32-entry receive and transmit rings in the DMA heap, MAC from the EEPROM, receive interrupts, transmit completion polled. QEMU holds frames for one second after RCTL is written, so the first reply after boot is late |
| 3c905B NIC | `driver/net/3c905b/` | Martin's "alpha" driver: EEPROM MAC read, MII, TX/RX descriptor rings in `palloc` memory, interrupt handler with debug prints, `iface` abstraction (`SnagPackets`/`SendPackets`/`Setting`). Probes through `pci_find_device()`; not wired to the new stack (QEMU does not emulate the card) |

`driver/drivers.c` is a table of `{name, init, status}` run in order by `init_drivers()`;
`init` returns 0 (present), 1 (absent) or an error, and `drivers` prints the result.
**ai-dev**: with `profile=laptop` the ISA entries (parport, dma, floppy, ata) are skipped,
and `drivers` also prints the console, serial, timer, keyboard and APIC that were chosen.
The parallel port reads its address from the BIOS data area as a 16-bit word and accepts
only 0x378/0x278/0x3BC (UEFI leaves that area with arbitrary contents).
The test disk (`build/images/test.img`: MBR, FAT16 partition at block 2048 built by
`tools/mkimage.py` with mtools) and the initrd (`build/images/initrd.tar` from `rootfs/`)
are attached by `make run`/`make test`; the GRUB ISO carries the initrd as a module.

## 9. File systems (`fs/`)

* `include/fs/vfs.h`: a **vnode** is a file or directory handed out by a file system
  (reference counted, `vget()`/`vput()`, released through `ops->release`); a
  **superblock** is a mounted instance with its device, root vnode and a mutex that
  serializes metadata updates; a **file** is a vnode plus position and open flags.
  Errors are negative errno values (`strerror()`); open flags and `SEEK_*` use the
  Linux numbers so a user libc can share them.
* Path resolution (`fs/vfs.c`): `vfs_normalize()` turns cwd + path into an absolute
  path without `.`/`..`; the mount with the longest matching prefix owns it and the
  rest is walked one component at a time through `lookup()`. `/` is a built-in file
  system whose entries are the mount points. Each task has a `cwd` and an `NR_OPEN`
  descriptor table (`fd_install()`, `fd_get()`, closed when the task is reaped).
* `fs/bcache.c`: 128 cached blocks, LRU, write-back. `bsync()` runs when a written
  file is closed, after `mkdir`/`unlink`/`rmdir`, on `sync` and at `umount`.
* `fs/tarfs.c`: a ustar archive on a block device, read-only; the tree is built at
  mount time (GNU long names supported), data is read through the cache.
* `fs/fat.c`: FAT12/16/32 by cluster count, VFAT long names on read and write,
  8.3 generation with `~N` tails, cluster allocation with zeroing, directory growth,
  `.`/`..` for new directories, CMOS time stamps. Verified against mtools: files
  written by SurfOS are read back by `mcopy` in the smoke test.
* `init_fs()` mounts the first tar ramdisk on `/initrd` and every FAT volume on
  `/<device>` (`/hda1`). Shell: `ls cat cd pwd stat cp write mkdir rm rmdir mount
  umount sync`, and `hexdump` on files.

## 10. Processes and system calls

* **Address spaces** (`mm/uvm.c`): a process has its own page directory. The kernel
  half is a copy of the kernel directory's entries, so the kernel page tables (and
  everything mapped into them later, heap pages included) are shared; the user range
  0x40000000-0x7FFFFFFF is built from per-process page tables kept in kernel heap
  pages. User frames come from `pmm_alloc()` and are zeroed through their user
  mapping, which is why a process builds its own image inside its own task.
  `schedule()` reloads CR3 only when the next task belongs to another address space;
  kernel threads run in whichever directory is loaded. The user stack grows from a
  page fault (`uvm_grow_stack()`, up to 4 MB below 0x80000000), `sbrk()` grows a heap
  above the image, and `reap()` frees everything.
* **Spawn** (`kernel/process.c`): `process_spawn(path, argc, argv, con, flags)`
  copies the arguments into kernel memory and starts a task whose first job is to
  create the address space, switch to it, map and read the ELF `PT_LOAD` segments to
  their addresses, put the strings, `argv[]`, `argc` and `argv` on the user stack, open
  `/dev/console` as descriptors 0-2 and `iret` to ring 3 (`enter_user()`). A load
  failure ends the task with exit code 127; the pid is returned at once and `wait`
  collects the exit code. Exceptions in ring 3 kill only that process
  (`trap_fatal()` prints ", user mode" and skips the kernel backtrace).
* **System calls** (`kernel/syscall.c`, numbers in `include/surfos/syscall.h`):
  `int 0x80`, number in eax, arguments in ebx/ecx/edx, result in eax, negative values
  are -errno. The handler enables interrupts and runs as ordinary task code (it may
  block in the VFS or sleep). Every user pointer is checked against the address space
  first (`uvm_check()`, which also grows the stack under a buffer that was never
  touched), so a bad pointer is EFAULT and never a kernel page fault with a lock held.
  Calls: exit write read open close lseek readdir stat chdir getcwd mkdir unlink rmdir
  getpid sleep yield spawn wait sbrk uptime kill.
* **User space** (`user/`): `user.ld` links static ELF executables at 0x40000000,
  `crt0.S` passes argc/argv to `main()`, `libsurf` wraps the system calls and adds
  `printf` (over `write`), `readline`, a bump `malloc` on `sbrk` and `strerror`; the
  string, memory, ctype and formatting code is `lib/blibc` compiled a second time.
  Programs: `hello`, `crash` (NULL write, kernel write, cli, hlt, in, int 0x40, EFAULT,
  deep recursion, divide by zero), `cat`, `ls`, `count`, `sh` (a user shell that spawns
  from `/initrd/bin` and waits). They live in the initrd under `/bin`.

## 11. Network (`net/`)

* **Devices and buffers** (`include/net/net.h`, `net/netbuf.c`): `struct netdev` has
  the MAC, the IPv4 configuration (address, netmask, gateway, DNS), counters and a
  `send` operation; `struct netbuf` is one frame with 64 bytes of headroom so each
  layer prepends its header (`netbuf_push`) or strips it (`netbuf_pull`). Drivers call
  `netdev_rx()` from their interrupt handler; frames queue up for the **net thread**,
  which runs every protocol under `net_lock` and ticks every 100 ms for ARP retries,
  ageing and TCP retransmission. API calls from other tasks take `net_lock` too and
  wait on events with the lock released.
* **Ethernet and ARP** (`eth.c`, `arp.c`): a 16-entry ARP table that answers requests,
  parks one frame per unresolved address and sends it when the reply arrives (three
  retries a second apart, entries age out after ten minutes); a gratuitous ARP
  announces a new address.
* **IPv4 and ICMP** (`ip.c`, `icmp.c`): header, checksum, don't-fragment, routing
  through the gateway for other subnets, no fragments or options; echo requests are
  answered in place and `icmp_ping()` waits for one reply at a time.
* **UDP** (`udp.c`): eight kernel sockets with a receive queue each
  (`udp_open/sendto/recvfrom/close`).
* **TCP** (`tcp.c`): active and passive open, in-order receive into an 8 KB window,
  an 8 KB send buffer retransmitted on a one-second timer (eight tries, backing off to
  four seconds), FIN in both directions with CLOSING, LAST_ACK and a two-second
  TIME_WAIT, RST in and out. No congestion control, out-of-order queueing or window
  scaling. Eight sockets; `tcp_connect/listen/accept/send/recv/close`.
* **DHCP and DNS** (`dhcp.c`, `dns.c`): DISCOVER/OFFER/REQUEST/ACK on port 68 applies
  address, netmask, router and DNS server (the lease is reported, not renewed); one A
  query with name compression on the answer.
* **Shell**: `ifconfig [if ip mask [gw] [dns]]`, `arp`, `ping <ip|name> [n]`, `dhcp`,
  `nslookup`, `tcpecho [port]` (a server task), `httpget <host> <port> [path]`,
  `netstat`. Under QEMU's user network the gateway is 10.0.2.2, DHCP gives 10.0.2.15,
  DNS is 10.0.2.3; the harness forwards a host port to guest port 7 and runs an HTTP
  server whose port reaches the kernel as `httpport=` on the command line.

## 12. The shell (`shell/main.c`)

**ai-dev**: a command table with argument splitting; `help` is generated from it.

| Command | Result |
|---|---|
| `help`, `clear`, `echo`, `tick`, `uptime`, `date`, `ps`, `kill`, `sleep`, `memstat`, `dmesg`, `irqstat`, `bootinfo`, `lpstat`, `test` | Work |
| `heaptest`, `selftest` | Heap churn; the kernel self test (tasks, sleep, timers, mutex, semaphore, event, formatter): prints `SELFTEST PASS` |
| `crashdiv`, `crashgp`, `crashnull`, `crashint` | Real exceptions in the shell task; the first three kill it and init restarts it, the last is reported and ignored |
| `funky`, `beep`, `hanoi`, `demo` | The 2004 demos (`shell/demos.c`) |
| `die` | Starts a ring-3 task; it faults at once until P1 adds user mode |
| `reboot` | Pulses the keyboard controller reset line |

## 13. Build system

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
**ai-dev**: `usb-image` and `test-usb` (section 14), `test-mkusb`; CI runs them too.

## 14. UEFI laptop boot (ai-dev, October 2026)

The USB image boots SurfOS on a UEFI PC, the Lenovo IdeaPad 3 15IIL05 in particular
(`docs/USB-BOOT.md`, hardware record in `docs/hardware/`):

```
firmware -> USB stick, GPT, FAT32 system partition (label SURFOSUSB)
  -> \EFI\BOOT\BOOTX64.EFI   standalone GRUB 2 for x86_64 UEFI; its built-in bootstrap
                             (boot/grub-usb-bootstrap.cfg) finds the partition it came from
  -> /boot/grub/grub.cfg     (boot/grub-usb.cfg) multiboot2 /boot/surfos.bin profile=laptop,
                             module2 /boot/initrd.tar
  -> kmain(0x36D76289, info) boot services exited, 32-bit protected mode, paging off
```

* **Multiboot 2.** `boot/boot.S` carries both headers (Multiboot 1 at file offset 0x1004,
  Multiboot 2 at 0x1010); the Multiboot 2 header asks for a linear framebuffer in the
  firmware's current mode, preferably 32 bits per pixel, and page-aligned modules. No EFI
  tags: GRUB exits boot services and enters like Multiboot 1. `kernel/multiboot2.c` checks
  every tag against the block's size (the envelope is checked first, at most 256 KB) and
  rejects truncated, misaligned, oversized or unterminated data as a whole
  (`tests/host/test_bootinfo.c`). The memory map holds 512 entries and fails rather than
  dropping any; more than 8 modules are counted and reported. A rejected block, or one
  without a memory map, stops the boot before any memory is written (`require_memory_map()`;
  without a framebuffer only a serial port can say why).
* **CPU state.** Multiboot 2 leaves CR0/CR4 undefined and OVMF hands over CR0.WP set.
  `init_cpu()` sets CR0 to PE, MP, ET, NE (x87 `fninit`) and clears the CR4 bits that
  would change behaviour (SMEP, SMAP, PVI, PCIDE, PGE, …), then sets PSE and makes PAT
  entry 4 write-combining.
* **profile=laptop.** `kernel/acpi.c` reads the FADT, MADT and HPET tables (checksums and
  lengths checked; the root pointer comes from GRUB, or the BIOS area on a BIOS machine).
  `kernel/apic.c` puts the local APIC in virtual wire mode (LINT0 ExtINT, LINT1 NMI; xAPIC
  or x2APIC), masks every I/O APIC input and acknowledges interrupts the firmware left in
  service. `kernel/timer.c` calibrates its delays on the first clock it sees counting (PIT
  channel 2, ACPI PM timer, HPET; the HPET's legacy routing is switched off so IRQ 0 is the
  PIT's) and `timer_confirm()` falls back to the local APIC timer (vector 0x30) when the
  PIT's tick never arrives. `kernel/keyboard.c` rewrites the 8042's command byte (IRQ 1,
  set-1 translation, mouse off) with bounded waits and also polls it on every tick; the
  IRQ handler checks the status register first, so no byte is taken twice. Only the
  initrd is mounted. Debug words: `timer=lapic`, `kbd=poll`, `kbd=irq`, `clock=pmtmr`,
  `clock=hpet`.
* **The framebuffer lock.** Whoever draws holds a busy flag and cannot be preempted until
  it lets go (a task killed while drawing would keep it, and the screen would freeze);
  a task killed by an exception gives it back, and `panic()` and the wipeout path take
  the screen over so their report is drawn.
* **Bounded probes everywhere.** COM1 is probed through its scratch register and its
  receive loop is bounded; the CMOS update wait, the reset path (ACPI reset register,
  0xCF9, the 8042, then a triple fault) and every 8042 wait are bounded.
* **Testing.** `tools/qemu-usb.py` boots the image file itself under OVMF with fresh
  firmware variables from an emulated xHCI USB stick, without serial if asked, types on
  the PS/2 keyboard through the monitor and reads the framebuffer back cell by cell
  against the font. Seven scenarios: serial, keyboard-only at 4 GB, 6 GB with `himem`
  checked through the monitor, the troubleshooting entry, no PIT, and images missing
  their initrd or kernel.
