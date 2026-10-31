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
| Kernel style | Monolithic; the debug shell runs in ring 0, programs run in ring 3 behind `int 0x80`; **ai-dev** (10/6/26): the shell is a ring-3 program, surfsh, on every console, and the ring-0 shell is the recovery console (section 17) |
| Segmentation | Flat: kernel code 0x08, data 0x10; user code 0x1B, data 0x23; one TSS (0x28) whose esp0 is the current task's kernel stack |
| Memory | Identity map of the low 16 MB, demand-paged kernel heap at 0xB0000000, a page directory per process with the user range 0x40000000-0x7FFFFFFF |
| Scheduling | Pre-emptive, 100 Hz PIT, software stack switching, 4 run levels + sleeping + removal queues |
| Interrupts | Two 8259 PICs remapped to 0x20-0x2F, shared IRQ handler chains, exceptions kill the current task; **ai-dev** `profile=laptop`: the 8259s reach the CPU through the local APIC (virtual wire), whose timer is the fallback tick |
| Devices | VGA text console, or **ai-dev** a framebuffer console on UEFI (4 virtual consoles, each a tty with a line editor) mirrored to a COM1 serial console when there is one, PS/2 keyboard plus serial input, PIT, CMOS RTC, parallel port, 8237 DMA (incomplete), floppy (incomplete), full PCI enumeration with names, block layer with ramdisks from boot modules, ATA PIO disks and MBR partitions (**ai-dev**: also GPT, and USB sticks through the xHCI controller, section 16), 3Com 3c905B NIC (no protocol stack); **ai-dev** (10/6/26): the console draws UTF-8 as code page 437 cells and pages long output (section 7) |
| libc | `blibc`: printf/snprintf, puts/gets (through the tty), getch, the standard string and memory functions, strtol, ctype, CMOS time |
| Files | VFS with a mount table; tar file system for the initrd (read-only), FAT12/16/32 with long names (read and write, `mkfs` for FAT16/32); 1024-block write-back cache |
| Processes | ELF32 executables loaded from any mounted file system, argc/argv, exit codes, wait, 44 system calls, a small C library (`user/libsurf`), the SurfOS shell surfsh and an older small shell (`sh`), file programs and a text editor; **ai-dev** (10/6/26): TCP connections, pseudo-terminals and `poll()` for programs (section 18) |
| Network | e1000 driver; Ethernet, ARP, IPv4, ICMP, UDP, TCP, a DHCP client and a DNS resolver in a kernel thread; `ifconfig arp ping dhcp nslookup tcpecho httpget netstat`; **ai-dev** (10/6/26): `telnetd`, a telnet server (section 18) |
| User interface | Ring-0 debug shell with about 70 commands; `run`/`spawn`, or type a program's name. **ai-dev** (10/6/26): surfsh on every console, with the SurfOS prompt (`initrd ▸ bin ≋>`), Tab completion, history, a pager, quotes, redirection and `$?`; it runs the ring-0 shell's 68 commands in its own task through `SYS_kcmd`. The ring-0 shell is the recovery console (`≋?>`) and the shell of `shell=kernel` (`≋#>`); a telnet session gets surfsh too (`≋@>`) |

Required RAM is 32 MB (`mm/memory.c:21`). The kernel version string is 0.007 and
the shell calls itself v0.008. **ai-dev** (10/6/26): SurfOS 1.0, from
`include/surfos/version.h`: the banner ("Brandon Burr's SurfOS", "Version 1.0 on an x86,
2004-2026"), `/initrd/etc/version`, the web client's User-Agent and the ring-0 shell's
help ("SurfOS kernel shell 1.0").

## 2. Source tree

```
boot/        boot.S (Multiboot 1 + 2 headers, entry), grub.cfg (the ISO),
             grub-usb.cfg + grub-usb-bootstrap.cfg (the USB image)
kernel/      main.c (kmain), gdt.c (GDT + TSS), interrupt.c (IDT, PICs, dispatch, IRQ chains),
             traps.asm (256 entry stubs), task.c (scheduler), wait.c + sync.c + ktimer.c (wait queues, locks,
             kernel timers), timer.c (PIT), panic.c (exceptions, panic), ksym.c (symbol lookup, backtraces),
             klog.c (kernel log), console.c (VGA or framebuffer text grid, escape sequences, UTF-8, the
             pager, kprintf), tty.c (input queues, line editor, raw mode, TTY_PAGE and TTY_CAPS, console
             switching), keyboard.c (scan codes, LEDs), process.c (spawn, ELF loader, entering ring 3),
             syscall.c (int 0x80 dispatch, the kernel commands for programs), pty.c (pseudo-terminals),
             proc.c (procinfo, detach),
             sys.c (sleep/beep/reboot), turf.c (Martin McCormick's reader/writer "turf" locks),
             multiboot.c + multiboot2.c (boot information), cpu.c, acpi.c, apic.c (profile=laptop)
mm/          memory.c (init), pmm.c (frame stack), memrange.c (64-bit address ranges), paging.c (page tables,
             page-fault handler, ioremap), kalloc.c (kernel and DMA heaps), uvm.c (per-process address spaces)
lib/blibc/   chars.c printf.c vsnprintf.c strings.c string.c memory.c ctype.c stdlib.c time.c
shell/       main.c (the ring-0 shell and the kernel's command table), prompt.c (the SurfOS prompt, also
             built into libsurf), fdisk.c, fscmd.c (mkfs mount umount eject df mv fsbench), demos.c,
             selftest.c, parport.c (lpstat)
fs/          vfs.c (mounts, paths, open files, descriptors), bcache.c, tarfs.c, fat.c, fat_mkfs.c (mkfs),
             devfs.c (/dev, /dev/kmsg), poll.c (poll()), pipe.c (pipes, 10/24/26)
net/         netbuf.c (buffers, devices, the net thread), eth.c, arp.c, ip.c, icmp.c, udp.c, tcp.c, dhcp.c, dns.c,
             sock.c (TCP connections as descriptors)
user/        user.ld; libsurf/ (the C library: crt0.S syscalls.c stdio.c printf.c stdlib.c string.c ctype.c
             malloc.c dirent.c time.c inet.c env.c, and prompt.c, which builds shell/prompt.c again); include/ (surf.h,
             the standard headers stdio.h stdlib.h string.h unistd.h fcntl.h errno.h dirent.h time.h ctype.h
             limits.h stdarg.h stddef.h stdint.h poll.h, sys/stat.h sys/statfs.h sys/types.h, and
             surfos/prompt.h kcmd.h version.h sock.h poll.h pty.h proc.h pipe.h spawn.h, which include the kernel's
             headers of those names); bin/ (hello crash cat ls count sh echo cp mv rm mkdir rmdir touch df wc head
             hexdump fstest edit pagetest kcmdtest nettest ptytest pipetest; 10/24/26 nc); surfsh/ (the SurfOS
             shell: main.c editor.c complete.c parse.c unparse.c expand.c exec.c run.c jobs.c util.c builtins.c
             surfsh.h); telnetd/ (the telnet server: main.c server.c telnet.c telnet.h telnetd.h); 10/24/26 ftpd/
             (the file transfer server: main.c server.c ftp.c ftp.h ftpd.h, section 22)
driver/      drivers.c (driver table), pci.c + pci_names.c + PCIDATA.H, pciinfo.c (lspci -v, wifiinfo), serial.c, bdev.c (block layer),
             ramdisk.c, ata.c, part.c + parttab.c (MBR and GPT partitions; mbr.c until 10/5/26),
             framebuffer.c + fbcore.c + font8x16.c (the UEFI console: the 256 code page 437 characters;
             fonts/ has the Spleen source and surfos-symbols.bdf), cp437.c (the character set, the UTF-8
             decoder),
             net/e1000.c, parport.c, dma/, floppy/, net/3c905b/ (2004, unwired),
             usb/ (xhci.c, usb.c, usbnet.c, usbdesc.c, msc.c: the USB stack, sections 15 and 16)
include/     surfos/ (kernel headers; prompt.h, kcmd.h and version.h use no kernel types and are shared with
             programs), mm/, sys/ (driver headers: bdev.h, part.h, usb.h, cp437.h, ...),
             fs/ (vfs.h, bcache.h, fat.h), net/, asm/io.h, blibc headers
tools/       qemu-run.py (headless QEMU harness), qemu-usb.py (the USB image under OVMF), gensyms.py (symbol
             table for backtraces), mkimage.py (test disk image, initrd, the "foreign" FAT32 stick),
             mkusb.py (the USB image), bdf2c.py (the font table), qemu-tcp.py + rawnet.py (TCP against
             hand-made segments)
rootfs/      packed into build/images/initrd.tar, the Multiboot module behind rd0; etc/version, etc/surfsh.conf
             (the shell's settings)
tests/       host/: unit tests built with the host compiler (make unittest): blibc, boot information,
             framebuffer, USB parsing, partition tables, mkfs, the user C library, the prompt, code page 437
             and the font, the pager (test_pager.c with pager_bridge.c and pager_shim/: the real console.c and
             tty.c on the host), the telnet protocol (test_telnet.c) and telnetd's loop (test_telnetd.c with
             telnetd/surf.h, a simulated kernel), PCI configuration space (test_pciinfo.c), ftpd's FTP
             protocol (test_ftp.c, 10/24/26);
             test_part_tools.py (partition tables against sfdisk and sgdisk), test_mkusb.py, test_devenv.py
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
       init_drivers()             the driver table: serial IRQ 4, parport, dma, floppy, pci, 3c905b, ramdisk, ata, e1000,
                                  usb (ai-dev: xHCI controllers and the USB devices plugged in)
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
information, `run_memcheck()`, `init_himem()`; with `profile=laptop` `init_acpi()`; after
`init_interrupt()`, `init_console_grid()` (a framebuffer's whole text grid, whose buffers
come from the kernel heap: section 7) and, with `profile=laptop`, `init_apic()`; then as
above, ending with `init_timer()` and `timer_confirm()`, which waits up to 100 ms for the
first tick (on a laptop it starts the local APIC timer when the PIT's never comes,
otherwise it stops with a message). `init_fs()` mounts no USB drive, and with
`profile=laptop` only the initrd (section 16).

**ai-dev** (10/6/26): `init` (pid 1) starts no ring-0 shells of its own accord any more.
On every console it starts `/initrd/bin/surfsh -l`, a user process flagged `TF_SHELL`,
and starts it again when it ends. The ring-0 shell takes a console with `shell=kernel` on
the command line (every console, elevated: `SURF ≋#>`) or as a recovery console
(`SURF ≋?>`, after init's line saying why): when surfsh cannot be started (no initrd, no
such file, not a file) or loaded (it never got as far as `TF_LOADED`), or after three
abnormal ends within 30 seconds (killed, or ended within 2 seconds of its start). A spawn
that fails for want of memory (or with EIO) leaves the console without a shell, and init
tries again every second (section 17).

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
  `kill_task()` turns a task into a zombie and `task_wait()` in the parent frees it
  (10/6/26: a task killed inside a system call or kernel command is only marked and ends
  at the end of its call, section 17);
  init (pid 1) collects detached and orphaned tasks and restarts a shell when one
  dies. Nothing frees a stack that may still be running, and every kernel stack is
  pre-faulted at creation (a stack page fault would double-fault). **ai-dev** (10/6/26):
  `task_exit()` closes the task's descriptors first, in its own context, so a background
  program's file is written and its volume can be unmounted before anyone collects it (a
  killed task's are closed at `reap()`, as before); `task_wait_opts(pid, &status,
  WNOHANG)` answers 0 at once while the caller's children run on, and
  `task_child_count()` counts them; init supervises the shell of each console (section 17).
* **ai-dev** (10/6/26): a task also carries the number of mutexes it holds (`mutexes`,
  kept by `kernel/sync.c`: the pager waits for a key only at 0), the UTF-8 character it
  has half printed (`utf`: each writer decodes its own, section 7), and `kcap`, its console
  output kept in memory for a redirected kernel command (section 10). `TF_LOADED` marks a
  process whose program was loaded and entered, which tells init a load failure from a
  program that ran and ended.
* 10/25/26 (robustness, L15): a user's kill ends processes and ring-0 shells only.
  `SYS_kill`, the `kill` command (which surfsh's `kill <pid>` runs) and the desktop's End
  task go through `task_user_may_kill()` (`include/surfos/task.h`; `task_kill_user()` finds
  and kills with interrupts off). A process is `TF_PROCESS` from its creation
  (`process_spawn_ex()`); a ring-0 shell (`TF_SHELL`) is started again by init, and its
  commands are calls that a kill waits for. A kernel thread is never inside a call, so a
  kill would end it on the spot holding its locks: `SYS_kill` answers EPERM, and the
  commands say "is a kernel thread". A thread that users need to stop gets a command of
  its own.
* 10/25/26 (robustness, L10-L13): floating point is the programs'. A process has an area
  for its registers (`fpu`, 512 bytes, made with the task and freed at `reap()`): the x87
  stack with its control and status words, and with SSE the XMM registers and MXCSR, as
  every program starts with them (control words 0x037F and 0x1F80, every register zero).
  They are switched lazily (`kernel/fpu.c`): `schedule()` sets CR0.TS whenever it switches
  to a task that does not own the FPU (`fpu_switch()`, which writes CR0 only when the bit
  changes), and that task's first floating-point instruction raises #NM, whose handler
  saves the owner's registers (fxsave, or fnsave on a CPU without FXSR) and loads the
  current task's. `become_zombie()` lets go of the FPU; kernel threads have no area. The
  kernel never uses the FPU: it is built with `-mno-80387` (a float in kernel code fails to
  link), and a #NM in ring 0 panics. CR4.OSFXSR and OSXMMEXCPT follow CPUID
  (`kernel/cpu.c`), `cpuinfo` has an FPU line, and `fputest` checks it all.
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
  framebuffer with an 8x16 font (`driver/font8x16.c`, Spleen; since 10/6/26 the 256
  characters of code page 437, below), scaled by the largest whole
  number at which 640x400 fits, with a software cursor; `driver/fbcore.c` is the renderer,
  tested on the host. On such a boot 0xB8000 and the CRTC ports are never touched.
  **ai-dev** (storage work): the grid is no longer fixed. `COLUMNS` and `LINES` are the
  variables `conColumns` and `conLines`, 80x25 in static buffers at first; once the kernel
  heap works, `init_console_grid()` gives a framebuffer as many cells as it holds (at most
  240x100), the pixels left over becoming an even margin: 120x33 at scale 2 on 1920x1080,
  170x48 at scale 1 on 1366x768, 80x25 as before on 1280x800. One heap block holds the four
  consoles and the screen, and the boot log so far is drawn again on the bigger grid. VGA
  text mode stays 80x25.
  Every character funnels through `kputch()`. `printf()`, `puts()`, `cputs()` and,
  from task context, `kprintf()` print on the calling task's console (`kprintf()`
  in yellow, boot code and interrupt handlers on the active console), so a shell
  on console 3 sees its own `ps` and its own fault report.
* Serial console (`driver/serial.c`): `kputch()` mirrors everything written to the
  active console to COM1 (newline becomes CR LF, backspace becomes "\b \b", a console
  clear becomes an ANSI clear; **ai-dev**: CR and ESC pass through, so the terminal draws
  the same escape sequences as the screen; since 10/6/26 so do the bytes of UTF-8
  characters, as they were printed: the terminal is taken to speak UTF-8; since 10/25/26
  TAB, which the terminal's tab stops take where the screen does).
  IRQ 4 feeds received bytes to the active tty (CR to newline, DEL to backspace,
  `ESC [ A/B/C/D/H/F` to the arrow/Home/End key codes; **ai-dev**: in raw mode, the bytes
  as they came; in line mode, a lone Esc reaches the pager's question). `qemu -nographic` therefore
  gives a complete terminal session; `tools/qemu-run.py` drives it. After a console switch
  the serial terminal is cleared and repainted from the new console's buffer (**ai-dev**:
  each cell's character as UTF-8, `cp437_utf8()`).
* `kernel/tty.c` (roadmap C1): one `struct tty` per console with a 256-byte input
  ring filled by the keyboard and serial ISRs under `irq_save()`, a wait queue for
  readers (`tty_getc()` blocks, `tty_trygetc()` polls), and a line discipline
  `tty_readline()` with echo, backspace, Ctrl-U, Ctrl-C and an eight-line history on
  the Up/Down keys. `gets()`/`cgets()` and the shell read through it; `getch()`
  blocks on the task's own tty. `tty_switch(n)` makes console n active, repaints
  the VGA screen and the serial terminal; F1-F4 call it from the keyboard handler.
  **ai-dev** (10/6/26): a read starts a new screenful for the pager, and a task that is
  about to wait for a key calls off a question another task is waiting at (below); the
  line editor's echo and `getchar()`'s go through `console_echo()`, which a kept output
  never takes.
* Keyboard (`kernel/keyboard.c`, roadmap C2): scan-code set 1 with the E0 prefix
  (arrows, Home/End, PgUp/PgDn, Insert/Delete), shift, caps lock on letters, num
  lock on the keypad, Ctrl-letter control characters, LEDs, Ctrl-Alt-Del reboots.
  Special keys arrive in the tty as codes above 0x80 (`include/surfos/keyboard.h`).
  The ISR does nothing but decode and queue; the 2004 debug hooks are gone.
* **Escape sequences and raw mode** (**ai-dev**, for the text editor; `include/surfos/tty.h`
  is the contract). Each console parses the ANSI subset full-screen programs need: cursor
  position and moves (`CSI H f A B C D`), erase in display and line (`CSI J K`), bright,
  inverse and the eight colours (`CSI m`), and hiding the cursor (`CSI ? 25 l/h`). Any
  other sequence is read to its end and dropped, and a console with no attributes set
  prints in the writer's colour, so `kprintf()` and the older programs look as they did.
  Wrapping is VT100's: writing the last column leaves the cursor there and the next
  character starts the next line, so a program can write the bottom right corner without
  scrolling. Until 10/6/26 only printable ASCII (32-126) was drawn (now UTF-8 is, below).
  Since 10/25/26 TAB moves the cursor to the next multiple of 8, as on a terminal: never past
  the last column, not at all at a pending wrap, and the cells it passes stay as they are;
  for the pager it starts a row as a character does. A pty's stream console (section 18)
  had these rules already. `tty_ctl()`
  (system call `ttyctl`) with `TTY_RAW` 1 stops echo and line editing: keys arrive one at a
  time, Ctrl-C is just 0x03, the keyboard's special keys become the sequences a terminal
  sends (`ESC [ A` ... `ESC [ 6 ~`, Backspace 0x7f, Enter CR), and serial input arrives as
  sent. `TTY_GETSIZE` gives `(columns << 16) | rows`. Raw mode, colours and a hidden cursor
  end with the task that set them (`tty_task_gone()`, from task exit and `trap_fatal()`),
  so a crashed editor cannot leave its console raw or unreadable. A read of `/dev/console`
  is `tty_read()`: a line with its newline, or in raw mode the keys queued (at least one).
* **Characters** (**ai-dev**, 10/6/26; `driver/cp437.c`, `include/sys/cp437.h`). A cell's byte
  is a code page 437 character: VGA text mode draws it with the card's own font, the
  framebuffer with `font8x16.c`, the same 256 characters in the same order
  (`tools/bdf2c.py` builds it from Spleen 8x16 and `driver/fonts/surfos-symbols.bdf`, which
  draws what Spleen lacks: ▸, ◄, ∟ and ≋). SurfOS's symbols take three places whose VGA
  glyph is close: ▸ U+25B8 at 0x10 (VGA ►), ≋ U+224B at 0xF7 (VGA ≈), … U+2026 at 0xF9
  (VGA ∙); ► ▶ ≈ ∙ are drawn in those places too, β μ and the ohm sign at ß µ Ω, and the
  typographic quotes and dashes ‘ ’ “ ” – — as ' " -. `kputch()` hands every byte that is
  not plain ASCII to `utf8_feed()`, a pure function tested on the host: a complete
  character becomes its cell, one code page 437 lacks is drawn as `?`, and bytes that are
  not UTF-8 (a stray byte, a sequence cut short or not allowed) are drawn as the code page
  437 characters they are, as the programs of 2004 meant them. Each writer has its own
  decoder state (`surf_task.utf`; the interrupt handlers and early boot share one), so two
  tasks printing at once cannot mix the halves of their characters. `ttyctl(TTY_CAPS)`
  answers `TTY_CAP_UTF8 | TTY_CAP_COLOR`; `ascii` on the kernel command line leaves UTF-8
  out, for a serial terminal that cannot show it (the shells then draw their prompt in
  ASCII). Input stays ASCII: in line mode the bytes above 0x7F are the keyboard's
  special keys.
* **The pager** (**ai-dev**, 10/6/26; `include/surfos/tty.h` is the contract,
  `kernel/console.c` counts the rows and asks, `kernel/tty.c` decides who is paged).
  `ttyctl(TTY_PAGE, 1)` makes the caller the owner on its console: what it and the tasks
  descended from it (the parent chain; 10/25/26: through the programs of its foreground,
  `TF_FOREGROUND`, so a job surfsh started with `&` is not paged with its next command) print
  there in task context is paged. The console
  counts the rows that start, by a newline or a VT100 wrap; once LINES - 1 rows have been
  shown and a paged task is about to draw on, or start, another, that task first waits at
  `TTY_MORE`, `--More-- (Space: next page, Enter: next line, q: skip the rest)`, in inverse
  video on that row (the serial terminal gets it too), reads one key and takes the question
  away again (the row as it was; CR ESC[K on the terminal). Space or any other key gives
  another screenful, Enter one more row; q, Q, Esc or Ctrl-C drops what the paged tasks
  print until paging ends, until a paged task reads (the dropped bytes since the last
  newline, the question a program asks before it reads, are shown first) or until a paged
  task ends (its fault report shows). A task waits only in task context, with interrupts
  on, outside `KCRIT_ENTER`, holding no mutex, not in raw mode, and while no other task
  waits to read the terminal; elsewhere the rows are counted and it asks at the next row
  where it may. The other order is covered too: a task that starts to wait for a key calls
  the question off, and so do `TTY_PAGE 0` and the owner's end or change; the asker goes on
  as after Space. While the question is up another paged task that may wait waits for the
  answer (`page_waiters`), and anyone else who prints there, a task or an interrupt
  handler, takes the question away first; it comes back after their newline. A read, raw
  mode on or off, `ESC[2J` (or `ESC[J` at home) and a paged task's own `TTY_PAGE` start a
  new screenful. An ancestor of the owner takes the pager over (a shell whose background
  job turned paging on); anyone else gets EBUSY. Kept output and other tasks' output are
  never paged. `tests/host/test_pager.c` runs the real `console.c` and `tty.c` on the host,
  with tasks as threads.
* **Kept output** (**ai-dev**, 10/6/26): between `console_capture_begin()` and
  `console_capture_end()`, what a task prints on its own console outside interrupts goes
  into a buffer (`struct kcapture`; what does not fit is counted) instead of the screen.
  The kernel-command bridge keeps a redirected command's output this way (section 10).
  Typed input's echo bypasses it (`console_echo()`), `shell_ask()` pauses it around a
  question, and a fault report or a panic ends it first.

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
| PCI | `driver/pci.c`, `driver/pci_names.c`, `driver/PCIDATA.H` | Config mechanism 1; every bus behind a bridge, every function; class, IRQ line/pin, BARs sized with decoding off; `pci_find_device()`, `pci_find_class()`, `pci_enable_device()`; vendor/device/class names from the 2003 id tables (`lspci`). **Verified** in QEMU: i440FX, PIIX3, PIIX4 PM, VGA, 82540EM. **ai-dev** (10/7/26): `lspci -v` and `lspci bb:dd.f` describe each function's 256 bytes of configuration space (`driver/pciinfo.c`, host-tested: the ids with the subsystem, command and status, the interrupt pin, a bridge's buses, and the capability list: power management and the power state, MSI, MSI-X, PCI Express with its port type and link); `wifiinfo` gathers what a Wi-Fi driver needs (the chipset, every wireless function in full with what its ids probably are, the USB devices with the Bluetooth half of a combo card: `docs/WIFI-PLANNING.md`). Reads only; extended capabilities (from 0x100) are not reached |
| Block layer | `driver/bdev.c`, `include/sys/bdev.h` | Named 512-byte block devices with read/write ops; partitions translate to their parent; range checks and per-device counters (`lsblk`, `hexdump`). **ai-dev** (USB storage): a disk's bus (RAM, ATA or USB; 0, what a driver that says nothing gets, is never formatted), removable, read-only, `protect` (why it must be left alone) and a `flush` op; `bdev_may_format()`; unplugged disks unregistered but never freed; mount reference counts (section 16) |
| Ramdisk | `driver/ramdisk.c` | Every Multiboot module is a writable ramdisk `rd0`, `rd1`, ... (`qemu -initrd`, GRUB `module`); modules above 16 MB are mapped with `ioremap()`. **ai-dev**: bus RAM, so `fdisk` and `mkfs` may write one that is not mounted; the smoke test adds a blank one as `rd1` (`tools/qemu-run.py --scratch`) |
| ATA | `driver/ata.c` | Polled PIO, LBA28, both legacy channels, IDENTIFY, cache flush after writes, a mutex per channel; disks are `hda`-`hdd`, ATAPI devices are reported and skipped. **ai-dev**: bus ATA, an internal disk, which `fdisk` and `mkfs` never change |
| Partitions | `driver/part.c`, `driver/parttab.c`, `include/sys/part.h` | The four primary partitions of each disk become `hda1`-`hda4` with their type (`driver/mbr.c` until 10/5/26). **ai-dev**: also the logical partitions of an extended one (from 5) and GPT; a disk whose name ends in a digit gets a `p` (`usb0p1`); the SurfOS boot drive is recognised; `fdisk`'s changes are planned, written, read back and checked (section 16) |
| e1000 NIC | `driver/net/e1000.c` | Intel 82540EM (QEMU's default): MMIO through `ioremap()`, 32-entry receive and transmit rings in the DMA heap, MAC from the EEPROM, receive interrupts, transmit completion polled. QEMU holds frames for one second after RCTL is written, so the first reply after boot is late |
| xHCI USB | `driver/usb/xhci.c`, `driver/usb/usb.c` | **ai-dev**: the USB 3 host controller (PCI 0c03, interface 30), polled; root hub ports, enumeration at boot and on every port change, `lsusb` (section 15); bulk transfers a task waits for, and endpoint resets, for the storage driver (section 16) |
| USB Ethernet | `driver/usb/usbnet.c`, `driver/usb/usbdesc.c` | **ai-dev**: CDC ECM and NCM adapters (RTL8153 through its CDC Ethernet configuration, QEMU's usb-net) as `eth0`, `eth1`, ... (section 15) |
| USB storage | `driver/usb/msc.c` | **ai-dev**: sticks, external drives and card readers (Bulk-Only Transport, SCSI commands) as `usb0`, `usb1`, ..., with their partitions; plugging, unplugging and medium changes; never mounted at boot (section 16), but for the moment the network manager reads its settings from the volume SurfOS was started from (section 19) |
| 3c905B NIC | `driver/net/3c905b/` | Martin's "alpha" driver: EEPROM MAC read, MII, TX/RX descriptor rings in `palloc` memory, interrupt handler with debug prints, `iface` abstraction (`SnagPackets`/`SendPackets`/`Setting`). Probes through `pci_find_device()`; not wired to the new stack (QEMU does not emulate the card) |

`driver/drivers.c` is a table of `{name, init, status}` run in order by `init_drivers()`;
`init` returns 0 (present), 1 (absent) or an error, and `drivers` prints the result.
**ai-dev**: with `profile=laptop` the ISA entries (parport, dma, floppy, ata) are skipped,
and `drivers` also prints the console, serial, timer, keyboard and APIC that were chosen.
The parallel port reads its address from the BIOS data area as a 16-bit word and accepts
only 0x378/0x278/0x3BC (UEFI leaves that area with arbitrary contents).
The test disk (`build/images/test.img`: MBR, FAT16 partition at block 2048 built by
`tools/mkimage.py` with mtools) and the initrd (`build/images/initrd.tar` from `rootfs/`)
are attached by `make run`/`make test` (which also adds the blank 64 MB scratch module
`rd1`); the GRUB ISO carries the initrd as a module.

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
* **ai-dev** (USB storage), `include/fs/vfs.h`: `vfs_rename()` (within one file system,
  `EXDEV` otherwise; it replaces a file, not a directory, at the target), `vfs_statfs()`
  (unit, units, free units, flags, type, device, label and mount path, the same structure
  `df` and user programs get), `vfs_ftruncate()`, `vfs_fsync()` (the file system's sync,
  its dirty blocks, then the device's own cache), and read-only mounts
  (`vfs_mount_flags(..., MNT_RDONLY)`; every path that writes answers `EROFS`). A mount
  holds its block device (`bdev_get()`) until it is unmounted, and a second mount of a
  device, or of a disk and one of its partitions, is refused (`vfs_bdev_busy()`). The VFS
  keeps a list of open files with the task that opened each, and a superblock counts its
  live vnodes, so `umount` refuses a mount in use instead of freeing it under its users,
  and `vfs_busy()` says who: an open file or directory and its task, a command using it
  right now, or the current directory, of the asking task only. Unmounting syncs the file
  system, writes its blocks, flushes the device's cache (`bdev_flush()`), drops its cached
  blocks and lets go of the device. **Storage milestone 1** (10/24/26): the last close of a
  written file returns EIO when it did not reach the disk, as `fsync` does (it returned 0
  whatever happened); `umount` says how many changed blocks it gave up because they could
  not be written (`vfs_umount_lost()`), and `eject` writes every mount out first, unmounts
  nothing when that fails, and takes the disk from its first step to its last
  (`vfs_claim_disk()`), so no mount of a partition comes in meanwhile.
* `fs/bcache.c`: 128 cached blocks (until the storage work), LRU, write-back. `bsync()`
  runs when a written file is closed, after `mkdir`/`unlink`/`rmdir` (and `rename`,
  through the file system's `sync`, which for FAT writes FSInfo too), on `sync` and at
  `umount`. **ai-dev** (USB storage): 1024 blocks found through a hash table and reused
  least recently used first.
  `bcache_lock` guards the table only and is never held while a device works: a block
  being read in is marked loading and only the tasks that want it wait; one being written
  out is marked writing and stays usable (`bdirty()` bumps a generation, so a write-back
  cleans only what it wrote). Dirty neighbours go out in one request of up to 64 KB, a
  miss can read the following blocks too (`breada()`), and `bread_blocks()` and
  `bwrite_blocks()` move runs of blocks between a device and the caller through one 64 KB
  bounce buffer, coherent with the cached copies (FAT sends whole-block runs of 8 KB or
  more that way). An unplugged device's blocks are never served again, its dirty blocks
  are dropped instead of written, and a block still held when it went is dropped when it
  is released. **Storage milestone 1** (10/24/26, the SurfCPU branch's B1-B41 where x86
  needs them): a block whose write failed stays dirty for the next sync and is never
  reused before; a device that refuses writes keeps at most a quarter of the cache, and its
  file system's writes fail once it holds that much (`bcache_room()`). Blocks carry the
  order a file system gives them (`bdirty_order()`: FAT's data 0, tables 1, directory
  entries 2, FSInfo 3); one of order k reaches its device only when the device has no dirty
  block of a lower order, checked where every write is issued, and a block someone holds is
  not written while one being written is not handed out, so what reaches the device is what
  was checked. `bsync()` returns 0 only when every block dirty when it began reached the
  device. A refused straight write changes nothing in the cache. `tests/host/test_fatfs.c`
  runs this file and `fs/fat.c` over RAM disks that refuse writes and are pulled out at
  every write.
* `fs/tarfs.c`: a ustar archive on a block device, read-only; the tree is built at
  mount time (GNU long names supported), data is read through the cache.
* `fs/fat.c`: FAT12/16/32 by cluster count, VFAT long names on read and write,
  8.3 generation with `~N` tails, cluster allocation with zeroing, directory growth,
  `.`/`..` for new directories, CMOS time stamps. Verified against mtools: files
  written by SurfOS are read back by `mcopy` in the smoke test. **ai-dev** (USB storage):
  `rename`, `statfs` with the volume label, and a `truncate` that also grows a file (with
  zeros). There is one vnode per object in use, so lookups of an open file share it, its
  size stays right and it follows its entry when renamed. Names are UTF-8: long names are
  UTF-16 on the disk, lookups fold ASCII letters only, an 8.3 alias puts `_` for what it
  cannot hold, and a short name without a long one is read as code page 437. The free
  cluster count comes from FAT32's FSInfo sector when that can be trusted (else it is
  counted) and goes back there at sync and unmount. The clean-shutdown bit in `FAT[1]` is
  cleared while a volume is mounted read-write and set again at unmount, so macOS and
  Windows check a stick that was pulled out while mounted, and SurfOS's own mount warns
  about one (`+FAT32: ... was not unmounted cleanly`). A read-only mount writes nothing; once the
  device is unplugged every operation fails with EIO and the volume can still be
  unmounted. **Storage milestone 1** (10/24/26): what an operation changes reaches the disk
  in an order a pulled stick leaves repairable. Taking away needs the reverse of the cache's
  order, so an operation that frees clusters syncs in between (an order point): an entry is
  off the disk before its clusters are freed, a cut before the clusters past it, a new name
  before the old one goes; a link into a new cluster waits for that cluster's own FAT entry
  when they are in different FAT sectors. An order point that fails puts back what the
  operation changed and it says EIO (nothing changed); an undo that cannot be done leaves
  the volume read-only until unmounted. A rename over a file (a save) takes the moved name
  away first and then changes the replaced file's entry in place, so the name holds a whole
  file, old or new, at every moment on the disk, and no two names ever share a chain; a
  chain another entry of the same directory holds (two names on one chain, as an
  interrupted rename leaves them) is never freed or cut for one of them. A write that stops
  early gives back the clusters it linked past the file's end. A volume that was not marked
  clean when it was mounted, or that this mount damaged or lost clusters on, stays marked
  as not cleanly unmounted: only a checker clears the mark.
* `fs/fat_mkfs.c` (**ai-dev**): `mkfs`, a new and empty FAT16 or FAT32 volume laid out as
  Microsoft's FAT specification (fatgen103) and Windows do it, so macOS, Windows and Linux
  take it as one of their own. FAT16 below 512 MB, FAT32 from 512 MB (or on request, down
  to its 65525-cluster minimum, about 33 MB); Windows' FAT32 cluster sizes (4 KB up to
  8 GB, then 8, 16, 32 KB) and fatgen103's FAT16 table; cluster counts kept away from the
  type boundaries, where implementations that count differently would disagree. FAT32's
  data region starts on a 1 MiB boundary of the disk (flash erase blocks), its backup boot
  sector and FSInfo are at sectors 6 and 7. Only the reserved area, the FATs and the root
  directory are written, in large requests, around the cache; the old boot sector goes
  first and the new one last, so an interrupted `mkfs` leaves no half volume that looks
  whole. It asks `bdev_may_format()` itself, and drops the cached blocks of the device and
  its disk afterwards. No FAT12.
* `init_fs()` mounts the first tar ramdisk on `/initrd` and every FAT volume on
  `/<device>` (`/hda1`). **ai-dev**: never a USB drive (`mount` by hand), only the initrd
  with `profile=laptop`, and a protected device read-only. Shell: `ls cat cd pwd stat cp
  write mkdir rm rmdir mount umount sync`, and `hexdump` on files; **ai-dev** also `mv`,
  `df`, `eject`, `mkfs`, `fsbench` and `mount -r` (section 12).

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
  collects the exit code (10/25/26: the stack 16-aligned at `_start`, and `crt0` calls
  `main` 16-aligned, as the i386 ABI has it). **ai-dev** (10/6/26): a process that gets as far as ring 3 is
  flagged `TF_LOADED`, and `process_spawn()` passes `TF_SHELL` through for init. **ai-dev**: `process_spawn_files()` (system call `spawnfd`)
  hands the child open files of its parent as 0, 1 and 2 instead, which is how the user
  shell redirects; a new task starts in its creator's current directory. Exceptions in
  ring 3 kill only that process (`trap_fatal()` prints ", user mode" and skips the kernel
  backtrace).
* **System calls** (`kernel/syscall.c`, numbers in `include/surfos/syscall.h`):
  `int 0x80`, number in eax, arguments in ebx/ecx/edx, result in eax, negative values
  are -errno. The handler enables interrupts and runs as ordinary task code (it may
  block in the VFS or sleep). Every user pointer is checked against the address space
  first (`uvm_check()`, which also grows the stack under a buffer that was never
  touched), so a bad pointer is EFAULT and never a kernel page fault with a lock held.
  Calls: exit write read open close lseek readdir stat chdir getcwd mkdir unlink rmdir
  getpid sleep yield spawn wait sbrk uptime kill; **ai-dev** (22-31, for files on USB
  sticks and the editor): rename fstat ftruncate fsync sync statfs dup2 time ttyctl
  spawnfd. `dup2` shares the open file and its position; `time` is the CMOS clock read
  as it is (local time, no time zone) in seconds since 1970, the scale FAT stamps files
  with; `ttyctl` is raw mode and the screen size (section 7). **ai-dev** (10/6/26): `wait`
  takes options, `WNOHANG` (0 at once while children run on); `ttyctl` also `TTY_PAGE`
  and `TTY_CAPS`; `kcmd` and `kcmdinfo` (32 and 33) run and list the kernel's shell
  commands (below); 34-45 are the network's, pseudo-terminals' and processes' (section 18,
  Undertow's `resolve` and `getentropy`). **ai-dev** (10/24/26): `pipe` (46) and `spawnx`
  (47), section 21; (10/25/26) `waitx` (48: how a child ended, killed by Ctrl+C or not),
  `ttyctl`'s `TTY_INTR`, `#!` scripts and `ENOEXEC`, `job` (49: job control), section 21;
  10/24/26 `shutdown` and `sockaddr` (50 and 51), section 22.
* **Kernel commands from programs** (**ai-dev**, 10/6/26; `kernel/syscall.c`,
  `include/surfos/kcmd.h`). `SYS_kcmd(argv)` runs one of the ring-0 shell's commands in
  the calling task through `shell_run_command()`, as the ring-0 shell would, and returns
  its status (0, 1 or 2; `run` the program's) or -ENOENT, -EINVAL, -EFAULT, -E2BIG,
  -ENOMEM. argv is checked and measured first (at most 32 arguments of at most 255 bytes;
  E2BIG beyond) and copied into one kernel block with exactly those lengths. When the
  caller's descriptor 1 is a file that is not the console (`devfs_is_console()`), the call
  takes a reference to it, keeps the command's output in a `KCMD_CAPTURE_MAX` (256 KB)
  heap buffer while it runs (`console_capture_begin()`), and writes it there afterwards,
  with a note when it was cut; a failed write turns a success into 1. What a call holds
  (the argv block, the buffer, the file reference) is on a record (`struct kcmd_call`, on
  the task's stack) that joins a list before anything is taken and leaves it after
  everything is let go, each field set together with what it names; a command that ends
  its own task (crashgp) or a task killed meanwhile is released at `reap()`
  (`kcmd_task_gone()`). A question is asked through `shell_ask()`, which pauses the
  capture, so it is seen and answered on the console while the rest goes to the file; the
  commands that read the keyboard as they go (`rawkeys`, `demo`, `hanoi`: `CMD_ASKS`)
  refuse a redirection (2). Inside a process a command prints on the caller's console,
  `cd` moves the caller, `run` starts the program as the caller's child with its
  descriptors 0-2, `spawn` and the tasks of `funky`, `tcpecho` and `die` are detached, a
  crash command kills the caller (init restarts a shell it kills), and fault reports and
  panics end the capture first, so they reach the screen. `SYS_kcmdinfo(i, &info)` copies
  entry i of the table (name, arguments, help; 176 bytes, zeroed first).
* **User space** (`user/`): `user.ld` links static ELF executables at 0x40000000 and
  `crt0.S` passes argc/argv to `main()`. **ai-dev** (10/5/26): `libsurf` is a small C
  library of its own, `libsurf.a` (programs see only `user/include`, never the kernel's
  headers, and `--gc-sections` keeps each to what it uses). A failed call returns -1 (or
  NULL) and sets `errno`, whose numbers are Linux's and the kernel's. It has buffered
  `FILE` streams (stdout line-buffered on the console, stderr unbuffered, everything
  flushed at `exit()`), one formatter for every `printf` (10/25/26: with floating point, `%f %e %g`
  and their capitals, the exact value's digits as glibc prints them, L14) and `sscanf`,
  `malloc`/`free`/`realloc` with boundary tags and first fit (the heap grows by `sbrk()`,
  64 KB at least), the string and memory functions, `strtol`, `qsort` and `bsearch`,
  `opendir`/`readdir`, the file calls (`open`, `lseek`, `stat`, `fstat`, `rename`,
  `ftruncate`, `fsync`, `statfs`, `dup2`, ...) and `time`, `localtime`, `mktime` and
  `strftime`. `surf.h` keeps what only SurfOS has: `spawn`, `spawnfd`, `wait`, `ttyctl`,
  `uptime`, `msleep`, `getdent` (**ai-dev** 10/6/26: also `waitpid()` with `WNOHANG`,
  `kcmd()`, `kcmdinfo()` and the `TTY_PAGE` and `TTY_CAPS` constants; `fpurge()` in stdio
  drops what a stream holds; `user/include/surfos/` gives programs the kernel's
  `prompt.h`, `kcmd.h` and `version.h`). Programs, in the initrd under `/bin`: `hello`, `crash`
  (NULL write, kernel write, cli, hlt, in, int 0x40, EFAULT, deep recursion, divide by
  zero), `count`, `sh` (spawns from `/initrd/bin` or a path and waits; `"quoted words"`,
  `<`, `>`, `>>`, a trailing `&`, and `cd`, `pwd`, `echo`, `exit` built in), `cat`,
  `ls [-la]`, `cp [-r]`, `mv`, `rm [-rf]`, `mkdir [-p]`, `rmdir`, `touch`, `echo`, `df`,
  `wc [-lwc]`, `head [-n N]`, `hexdump`, `fstest` (the file API from user space, `FSTEST
  PASS`) and `edit`; **ai-dev** (10/6/26): `surfsh` (section 17), `pagetest` (the pager and
  the character set: `pagetest N`, `ask N`, `crash N`, `hold MS`, `utf8`, `caps`) and
  `kcmdtest` (the kernel-command calls from user space: statuses, limits, errors,
  redirection, several callers at once).
* **The editor** (`user/bin/edit.c`, **ai-dev**): full screen, in the manner of nano. The
  file lives in memory as an array of lines (up to 1 MB); the screen is a title line, the
  text, a status line and a line of keys. Lines do not wrap: the cursor's line scrolls
  sideways and a `>` in the last column says a line goes on. Keys: the arrows, Home, End,
  PgUp, PgDn, Backspace, Delete, Tab; ^S save, ^Q quit (asking when there are unsaved
  changes), ^F find, ^K cut the line, ^U paste, ^G go to a line, ^C where the cursor is,
  ^L redraw. Saving writes `<file>.tmp`, fsyncs it and renames it over the file (or
  writes in place where a file system cannot rename), keeping the file's line ends (LF,
  or CR LF when its first line has one). Since 10/17/26 it saves as the desktop's Notepad
  does (`shell/guifs.c`): the new file is made with `O_EXCL`, as `<file>.1.tmp` and so on
  past the user's own files of those names, and it is deleted again when this save could
  not write it or the rename failed before it changed anything. After another error (EIO
  above all) the FAT rename may have written the new name and not yet removed the old
  one, both holding the same clusters: the `.tmp` is kept and the status line names it,
  and since `fs/fat.c` frees a chain when any one name of it is deleted, replaced or
  truncated, nothing is renamed or written over that file again in that session: `edit`
  saves later versions as further `.tmp` copies (to `.9.tmp`, and none when nothing
  changed), warns on its help line and says where the last copy is when it ends; the
  desktop (`gui_unsure()`: the first four such pairs, and after more of them any file)
  saves over, deletes and renames neither of the two, nor the folders they are in. Both
  also look for such a pair before they change a file, and when they open one: a
  `.tmp`, `.N.tmp` or `.part` beside it of its size and time (`edit`'s `twin_beside()`,
  `note_guarded()`; `fs/fat.c`'s rename copies the entry, so that is how such a pair
  usually looks, though a later write to either name, or the old entry found first in
  the directory, hides it). `rx` keeps its `.part` file by the same rule, says to have the
  disk checked, and does not start over a `.part` that is its file's twin. Two empty files
  are no pair (no clusters). The care lasts while the two look alike, and for the rest of
  a session that met them: once the disk is checked, `rm` of one and a new start end it.
  `tools/devloop.py` says to check the stick when its `mv` into place fails with an I/O
  error, but a deploy run again then would rename over the `.new` that may share its
  file's clusters: nothing looks for that pair. Outside that
  care: the shell's `rm`, `mv` and `cp`, `rx`'s rename over a file with a `.tmp` twin, a
  stick mounted again under another path, and a `.tmp` of which only the 8.3 alias was
  left. The real fix is in `fs/fat.c` (not freeing a chain that another entry still holds;
  ROADMAP.md section 7), made by storage milestone 1 (10/24/26): a rename that fails no longer
  leaves such a pair, nor does a rename over a file cut short (every save), and an `rm`, a
  `mv` over either name or a truncate to 0 keeps the chain for the other name of a pair in
  one directory (a plain rename cut short between its new name and its old one's removal
  still leaves one); the care above stays for the pairs older SurfOS and other systems left. `tests/host/test_edit.c` and `test_rx.c` run `edit.c` and `rx.c`
  as they are on an in-memory file system that fails on demand (`tests/host/sim_fs.c`). The terminal is used only through `ttyctl()`,
  raw mode's key sequences and the ANSI subset of `tty.h`, so the editor works on the
  screen and over the serial line alike; each refresh rewrites only the rows that
  changed.

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
  scaling. Eight sockets; `tcp_connect/listen/accept/send/recv/close`. **ai-dev** (10/6/26):
  16 sockets, partial writes, a persist timer, keepalive, no silly windows, and the
  connections of programs as descriptors (section 18).
* **DHCP and DNS** (`dhcp.c`, `dns.c`): DISCOVER/OFFER/REQUEST/ACK on port 68 applies
  address, netmask, router and DNS server (the lease is kept per interface and renewed
  since 10/17/26: section 19); one A query with name compression on the answer.
* **Shell**: `ifconfig [if ip mask [gw] [dns]]`, `arp`, `ping <ip|name> [n]`, `dhcp [renew] [if]`,
  `nslookup`, `tcpecho [port]` (a server task; 10/25/26: `tcpecho stop` ends them), `httpget <host> <port> [path]`,
  `netstat`. Under QEMU's user network the gateway is 10.0.2.2, DHCP gives 10.0.2.15,
  DNS is 10.0.2.3; the harness forwards a host port to guest port 7 and runs an HTTP
  server whose port reaches the kernel as `httpport=` on the command line.

## 12. The shell (`shell/main.c`)

**ai-dev**: a command table with argument splitting; `help` is generated from it. Since
10/6/26 the ring-0 shell around it is the recovery console and the shell of
`shell=kernel`, and surfsh (section 17) runs the same commands in its own task through
`SYS_kcmd`.

| Command | Result |
|---|---|
| `help`, `clear`, `echo`, `tick`, `uptime`, `date`, `ps`, `kill`, `sleep`, `memstat`, `dmesg`, `irqstat`, `bootinfo`, `lpstat`, `test` | Work |
| `heaptest`, `selftest` | Heap churn; the kernel self test (tasks, sleep, timers, mutex, semaphore, event, formatter): prints `SELFTEST PASS` |
| `crashdiv`, `crashgp`, `crashnull`, `crashint` | Real exceptions in the shell task (from surfsh: the surfsh process); the first three kill it and init restarts it, the last is reported and ignored |
| `funky`, `beep`, `hanoi`, `demo` | The 2004 demos (`shell/demos.c`) |
| `die` | Starts a ring-3 task; it faults at once until P1 adds user mode |
| `reboot` | Pulses the keyboard controller reset line |
| `lsblk [-s]` | **ai-dev**: disks and partitions as `NAME SIZE BUS RM TYPE MODEL/LABEL STATUS`, the status `mounted`, `boot drive: protected`, `internal: never formatted` or `read-only`; `-s` the blocks each device has moved |
| `fdisk <disk>` | **ai-dev**: the partition table, the free space and whether the disk may be changed; with `new mbr` or `new gpt`, `add [size] [type]`, `delete <n>` or `type <n> <type>`, a change (section 16) |
| `mkfs [-t fat16 or fat32] <dev> [label]` | **ai-dev**: a new FAT volume; a partition gets the matching type, a whole disk loses its old partitions |
| `mount [-r] [dev [path]]`, `umount <path or dev>`, `eject <disk>`, `df`, `mv`, `sync` | **ai-dev**: mounting by hand (on `/<dev>` unless a path is given, read-only with `-r`); `eject` unmounts every partition of a disk, all or none, writes its blocks and flushes the device before it says `safe to remove` |
| `fsbench <dir> [MB]`, `readbench <dev> [MB]` | **ai-dev**: time writing and reading a file in 4 KB and 64 KB pieces; read a device in 64 KB requests (nothing is written) |
| `rawkeys`, `ansi` | **ai-dev**: the bytes keys send in raw mode (until `q`); a test pattern of escape sequences |

**ai-dev**: `fdisk` and `mkfs` show the device, what is on it and the change, then ask for
the device's name (`Type usb1 to write the new partition table to usb1 (...):`); any other
answer prints `Cancelled` and nothing is written.

**ai-dev** (10/6/26, surfsh): every command returns `CMD_OK` (0), `CMD_FAIL` (1, a refusal
and a Cancelled question included) or `CMD_USAGE` (2) (`shell/shell.h`); `run` returns the
program's exit status (1 when it could not start or was killed), and a line that names no
command is 127. The ring-0 shell draws its prompt with `prompt_render()` (section 17):
elevated, `SURF ≋#>`, or, as init's recovery console, diagnostic, `SURF ≋?>`. Since those
states win over a failure, its `!` never shows, so a word `$?` is the last status (`echo
$?`). Each command runs with `TTY_PAGE` on. `help` is headed "SurfOS kernel shell 1.0" and
signed "(C)2004-2026 Brandon Burr.". The commands now also run inside user processes, and
on four consoles at once, which shapes how they are written: they print on the caller's
console (`clear` prints `ESC[0m ESC[?25h ESC[2J ESC[H`, so it clears the caller's console
and follows a redirection; `funky` and `die` start on the caller's console); a question is
asked through `shell_ask()`; a command that reads the keyboard as it goes is flagged
`CMD_ASKS`; and none keeps state in statics that two callers could share. `heaptest` and
`selftest` run one at a time (a claim by pid; a second is refused with status 1; heaptest's
tables come from the heap for each run), and `selftest` waits for its own threads by pid,
never for any child, since its caller may have programs of its own. `df`, `mount`,
`umount` (by device) and `eject` print from a copy of the mount table taken under its lock
(`vfs_mount_views()`), so an `umount` elsewhere cannot free what they walk while the pager
holds them; `lsblk` asks `vfs_dev_mounted()`. `icmp_ping()` takes `ping_lock`: one request
at a time (two pings used to share one request record).

## 13. Build system

One non-recursive `Makefile` at the top level (the 2004 per-directory Makefiles with
their GCC 3 flags were removed on `ai-dev`; `master` still has them). Objects and the
kernel go to `build/O<level>/`; `make O=2` builds an optimised kernel next to the
default `-O0 -g` one. `linker.ld` places the Multiboot header first, the `.ksyms`
symbol table after the data, and discards the sections a 2026 toolchain adds. The
kernel is linked twice so the symbol table can hold the final addresses. Targets:
`all`, `run` (QEMU, serial console on the terminal), `run-vga`, `test` (headless
smoke test through `tools/qemu-run.py`), `test-all` (-O0 and -O2), `unittest`
(the host tests in `tests/host/` and `tests/test_part_tools.py`; **ai-dev** 10/6/26 also
`test_prompt`, `test_cp437` with the font, and `test_pager`), `debug` (gdb stub),
`iso` (GRUB image), `run-iso`, `clean`.
`.github/workflows/ci.yml` runs the unit tests, the smoke tests and the ISO build.
See `CLAUDE.md` for the requirements and `docs/ROADMAP.md` Phase 0 for the flags.
**ai-dev**: `usb-image` and `test-usb` (section 14), `test-mkusb`; CI runs them too.
Most user programs are one `.c` file each in `user/bin/`. The multi-file programs
`user/surfsh/`, `user/telnetd/` and `user/undertow/` have their own link rules.
10/25/26 (robustness, L13): the kernel is built with `-mno-80387` as well as `-mno-sse`,
since the FPU is the programs'; `USER_CFLAGS` takes it out. Programs and libsurf are i486
code (x87); one may opt into SSE2 with flags of its own (the Makefile shows how).

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
  set-1 translation) with bounded waits, probes the auxiliary port for a PS/2
  mouse, and also polls both ports on every tick. IRQ 1/12 share a status-aware
  demultiplexer so no byte is taken twice. USB boot mice use the xHCI interrupt
  transfer path. Both feed the [desktop pointer queue](GUI.md). Only the
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
  against the font, with the grid and margins of `driver/fbcore.c` (since 10/6/26 the
  font's code-point comments say which character each glyph is, so `▸ ≋ …` and accents
  read back; `page_through()` answers the pager with Space on the keyboard). Twelve
  scenarios: serial, keyboard-only at 4 GB (raw mode and escape sequences included), the
  IdeaPad's 1920x1080 panel filled with 120x33 cells (`fullhd`), 6 GB with `himem` checked
  through the monitor, the troubleshooting entry, the kernel-shell entry (`shell=kernel`,
  section 17), no PIT, a USB Ethernet adapter (section 15), two USB storage scenarios
  (section 16), and images missing their initrd or kernel.

## 15. USB networking (ai-dev, October 2026)

The IdeaPad has no Ethernet socket, so the laptop reaches a wired network through a USB
Ethernet adapter (`docs/USB-BOOT.md` section 8). Four files in `driver/usb/` (a fifth,
`msc.c`, is the storage driver of section 16), API in `include/sys/usb.h`:

```
init_drivers() -> "usb" (init_usb, driver/usb/usb.c)
  xhci_probe()          every PCI 0c03/30 function: map, take over, reset, rings, run
  port_check() per port connected? -> xhci_port_enable() -> enumerate() -> choose_driver()
                                      (reset)              (slot, address,   (first configuration
                                                            descriptors)      a driver matches)
  kthread "usb"         every port change event, at least once a second: unplug / plug,
                        endpoint recovery, the drivers' service hooks (link messages)
usbnet_attach()         endpoints, SET_CONFIGURATION, (NCM parameters), SET_INTERFACE,
                        MAC string, packet filter, a netdev -> eth0
```

* **The controller** (`xhci.c`): the xHCI 1.2 model with one command ring, one event ring
  (interrupter 0) and a transfer ring per endpoint, one 4 KB segment each with a link TRB.
  The device context array, the input and output contexts (32 or 64 bytes, CSZ) and the
  scratchpad pages the controller asks for all live in the 1:1 DMA heap, aligned so that
  nothing crosses a 64 KB boundary. Before the reset the firmware is asked to hand the
  controller over (USB legacy support capability, SMIs off); the supported-protocol
  capabilities say which root ports are USB 2 and which USB 3. The registers come from a
  64-bit BAR, mapped by `ioremap64()`: below 4 GB with uncached 4 KB pages, above 4 GB
  (where OVMF and Ice Lake firmware put it) with an uncached PSE-36 4 MB page.
* **No interrupts.** The controller's interrupter is left disabled. A periodic kernel
  timer polls the event ring on every tick (10 ms), with interrupts off. So do whoever
  waits for a command or a control transfer, and the network driver when it runs out of
  transmit buffers. The I/O APIC stays masked on the laptop, and an 8259 route for the
  controller is not guaranteed, so polling works where an interrupt might not. The cost
  is up to 10 ms of extra latency (ping times of 10-20 ms) and a few MB/s.
* **Waiting.** Commands and control transfers are synchronous. During boot, as the idle
  task before the timer runs, they busy-wait (`udelay`). In a task they sleep a tick
  between polls (`usb_can_sleep()`). Every wait is bounded: a command that does not
  complete in 5 s marks the controller failed, and its devices are then dropped. A
  control transfer that times out or stalls has endpoint 0 stopped (Stop or Reset
  Endpoint) and moved past it (Set TR Dequeue Pointer).
* **Bulk and interrupt transfers** (`struct usb_xfer`, `usb_submit()`): one TRB each,
  interrupt on completion and on a short packet. The `done` callback runs from the poll
  with interrupts off. An error that halts an endpoint (stall, babble, transaction error)
  is left to the usb thread: Reset Endpoint, Set TR Dequeue past the failed transfer,
  CLEAR_FEATURE(ENDPOINT_HALT) after a stall, then whatever is still queued runs again.
* **Devices** (`usb.c`): USB 2 ports are reset, USB 3 ports train their links themselves
  (a warm reset if not within a second). Then Enable Slot and Address Device, the device
  descriptor in two steps (endpoint 0's packet size first), the strings, and each
  configuration in turn until a driver's `match()` accepts one. Unplugging sets
  `dev->gone` under `irq_save`, lets a task inside the driver come out (its `quiesce()`,
  added for storage), disables the slot (no more callbacks after that), and only then
  calls the driver's `detach()`. Hubs are not supported (they are listed as such);
  isochronous endpoints and streams are not used.
* **The adapter** (`usbnet.c`, parsing in `usbdesc.c`). CDC ECM puts one frame in one bulk
  transfer: sixteen 2 KB receive transfers stay queued and are handed to `netdev_rx()` and
  resubmitted from the poll. A frame to send is copied into one of sixteen transmit
  buffers. One that fills its last packet exactly gets a byte of padding, so it ends in a
  short packet (as Linux does). CDC NCM packs frames into 16-bit transfer blocks: four
  receive blocks of up to 16 KB (SET_NTB_INPUT_SIZE), one frame per transmitted block.
  The notification endpoint gives the link state (`NETWORK_CONNECTION`) and its speed.
  The MAC address is the adapter's own (the iMACAddress string), because the adapter only
  passes frames sent to it. Each adapter is a `struct usbnet` with a netdev registered
  once: unplugged it stays with `link down`, and the next adapter to arrive takes it,
  keeping the address configuration only if it has the same MAC address.
* **Configuration choice.** An RTL8153 offers Realtek's vendor protocol as configuration 1
  and CDC Ethernet as configuration 2. QEMU's usb-net offers RNDIS first and ECM second.
  Taking the first configuration a driver matches selects ECM in both cases, and NCM when
  a device has no ECM.
* **Tests.** `tests/host/test_usb.c` runs `usbdesc.c` against QEMU's descriptors and ones
  shaped like an RTL8153 (SuperSpeed companions) and an NCM adapter, and against NTB16
  blocks built and taken apart, malformed ones included. NCM has no QEMU device, so these
  tests are its only coverage before hardware. `tools/qemu-usb.py`'s `usbnet` scenario
  covers the rest under OVMF: boot, DHCP, ping, HTTP (1 MB), the self test, and unplug
  and replug through the QEMU monitor (the same adapter, then a different one on another
  subnet). `selftest` on a network other than QEMU's checks only the lease and the gateway;
  it also sends a USB device a request it must stall (GET_STATUS of interface 0xee) and
  then checks that endpoint 0 still answers, which is the only test of that recovery.
* **Limits.** No hubs, so docks and multi-port dongles do not work; no USB keyboards
  or mice yet (the layer is ready for class drivers: add one to `usb_drivers[]` in
  `usb.c`, as the storage driver of section 16 was); no vendor-protocol adapters (ASIX);
  the IP stack still routes everything through the first netdev. `usb=off` on the
  command line (a GRUB menu entry) leaves the controller alone.

## 16. USB storage and FAT32 (ai-dev, October 2026)

Files that outlast a restart go on a second USB stick (`docs/USB-BOOT.md` section 9): the
laptop's own disk has no driver, the boot drive is only ever read, and the initrd is
unpacked afresh at every boot. The layers, from the bottom:

```
xhci_bulk_sync()      a bulk transfer the caller waits for: spins for 5 ms, then a tick at a time
msc.c                 Bulk-Only Transport (command wrapper, data, status) carrying SCSI commands
                      -> the disks usb0, usb1, ...
bdev.c                bus, removable, protect, gone, refs; bdev_may_format(), the safety rule
part.c + parttab.c    MBR (+ logical) and GPT -> usb0p1, ...; the boot drive; fdisk's checked writes
bcache.c              1024 blocks, no lock held across I/O, runs of up to 64 KB
fat.c, fat_mkfs.c     FAT12/16/32 read and write, UTF-8 long names, FSInfo, the clean bit; mkfs
vfs.c                 mount -r, rename, statfs, ftruncate, fsync, an umount that says who is busy
syscall.c (22-31)     rename fstat ftruncate fsync sync statfs dup2 time ttyctl spawnfd
user/                 libsurf (the C library), sh, the file programs, edit
shell/                lsblk, fdisk, mkfs, mount, umount, eject, df, mv (main.c, fdisk.c, fscmd.c)
```

* **Transfers** (`xhci.c`, `usb.c`). A disk command runs in the task that reads or writes.
  `xhci_bulk_sync()` queues one transfer and polls the event ring itself: in a tight loop
  for the first 5 ms, because a stick answers within microseconds to milliseconds and a
  tick is 10 ms, then a tick at a time, up to a time limit. A transfer that times out is
  taken back off its ring (Stop Endpoint, Set TR Dequeue Pointer); one whose device goes
  away fails with `USB_ENODEV` and leaves no pointer into the waiting task's stack on the
  ring. A halted endpoint is recovered on the spot by `usb_endpoint_recover()`, one task at
  a time (`USB_PENDING` while another is at it). `xhci_endpoint_reset()` drops and adds an
  endpoint in one Configure Endpoint command, which restarts its data toggle (USB 2) or
  sequence number (USB 3) at 0, as CLEAR_FEATURE(ENDPOINT_HALT) does on the device; BOT's
  reset recovery needs both ends reset. Control transfers now come from tasks other than
  the usb thread too, so each device has an `ep0_lock`.
* **The storage driver** (`msc.c`). It takes an interface of class 08 with the Bulk-Only
  protocol (0x50) and the SCSI, RBC, MMC or SFF-8070i command set: sticks, external drives
  and card readers, up to four logical units each. A unit gets INQUIRY once (block devices
  only), TEST UNIT READY with retries while it becomes ready, READ CAPACITY (10, or 16 at
  2 TB and beyond, of which the first 2 TB are used) and MODE SENSE for the write-protect
  switch; then it is a disk with the lowest free name `usbN` (not registered, and not the
  device of a mount that outlived its stick), whose partitions are scanned. READ (10) and
  WRITE (10) move up to 32 KB per command through one DMA buffer. After a failed command
  REQUEST SENSE reads into a buffer of its own, so a retried WRITE still sends the
  caller's data. A unit attention (power on, reset) is retried, except "the medium may
  have changed" on a removable unit with a disk: that disk's cache and mounts belong to
  the old medium, so the command fails and the usb thread drops the disk and probes
  again. A garbled exchange gets BOT's reset recovery (a class reset, then both bulk
  endpoints reset at both ends). `msc_flush()` is SYNCHRONIZE CACHE; a device that does
  not know it (ILLEGAL REQUEST) has no cache to flush. `msc_service()` runs from the usb
  thread: every two seconds it asks a removable unit whether a medium is there (a card put
  in or taken out), it probes again a unit that was not ready at first, and it skips a
  device that is busy with a command. One mutex per device covers a whole command, and
  disks are unregistered only after it is let go.
* **Unplugging.** `usb.c` marks the device gone, calls the driver's new `quiesce()` hook,
  which waits for a task inside a command to come out (every wait in the driver gives up
  once the device is gone), has the controller let go of the slot, and only then calls
  `detach()`, which unregisters the disks. `bdev_unregister()` marks a disk and its
  partitions gone and takes them off the list, so the name can be used again, but frees
  neither them nor the driver's structure: a mount, a held cache block or a command may
  still point at them, and finds every I/O failing with `BDEV_EGONE`. The cache stops
  serving the device's blocks and drops its dirty ones, FAT fails every operation with
  EIO, and the volume can still be unmounted. Plugged in again, the stick gets the lowest
  free name, normally its old one.
* **Partitions** (`part.c`, `parttab.c`, `include/sys/part.h`). `parttab.c` is pure code,
  without I/O or allocation, shared with the host tests. It reads an MBR with the logical
  partitions of an extended one (numbered from 5, as Linux numbers them; the extended
  partition is listed by `fdisk` but not registered) and GPT (the protective entry, both
  headers and their CRC32s, the backup when the primary is damaged). Everything read is
  checked before it is believed, and a table with anything left out is not changed. A
  change is a plan: the whole sectors to write, in order, and what the table must say when
  it is read back. `part_commit()` carries a plan out only while `bdev_may_format()` still
  agrees and the disk still reads as the table the plan was made from; it writes, flushes
  the device, reads every sector back and compares it, reads the table again and checks it
  against the plan, and rescans the disk. A new MBR also zeroes where a primary and a
  backup GPT would be, so no stale GPT outlives it. New partitions start on 1 MB
  boundaries; `fdisk add` makes primary partitions only, and names a GPT basic-data
  partition "SurfOS data".
* **The safety rule.** `bdev_may_format()` (`driver/bdev.c`) is the one rule `fdisk` and
  `mkfs` obey; `part_commit()` and `fat_mkfs()` ask it again just before they write. Only a
  USB disk or a ramdisk qualifies (`BDEV_BUS_USB`, `BDEV_BUS_RAM`): never an ATA disk or one
  whose driver names no bus (a SATA or NVMe driver's disks would be refused the same way),
  and never one that is protected, read-only (the write-protect switch), unplugged, or
  mounted, the disk or any of its partitions. `bdev_scan_partitions()` recognises the
  SurfOS boot drive: a disk with a partition (or, without a table, itself) that holds a
  FAT volume labelled SURFOSUSB, as `tools/mkusb.py` writes it, gets `protect = "the SurfOS
  boot drive"`, which `lsblk` shows. That drive is only ever mounted read-only. `init_fs()`
  mounts no USB drive, and with `profile=laptop` only the initrd. The commands show the
  device and the change and go ahead only when the device's name is typed back.
* **One stick (10/17/26).** A FAT32 stick that the boot files were copied onto
  (`tools/stick.py`, USB-BOOT.md section 14) boots the same way, and keeps the owner's
  files. GRUB's menu asks for the serial number of the FAT volume it was started from
  (`probe --fs-uuid`, so BOOTX64.EFI carries `probe`) and passes it to every entry as
  `bootvol=5D0F-0901`; `bootinfo_arg()` reads it and `part_parse_serial()` /
  `part_fat_serial()` compare it with each partition's boot sector. A disk holding that
  volume is the boot drive whatever its label: it and all its partitions get
  `protect = bdev_boot_drive`, which `fdisk` and `mkfs` refuse as before, and the volume
  itself `protect = bdev_boot_volume` ("the volume SurfOS was started from"), which
  `bdev_mount_readonly()` lets `mount` write (lsblk: `boot drive: writable`, or
  `mounted (boot drive)`). SURFOSUSB wins when a volume has both: the flashed image stays
  read-only. `part_fat_serial()` reads the serial number where GRUB reads it, whatever the
  extended boot signature says (it still wants a FAT boot sector: the 55 AA signature, a jump,
  a sane BPB). On a stick that
  holds `boot/ONE-STICK.txt` (the stick folder has it, the flashed image never) the menu
  looks for `/surfos-dev/` on its own partition before the other disks, and
  `tools/devloop.py deploy` writes there first in the same order, refusing a `--stick`
  that names another disk and a boot stick without the marker; so one stick serves as
  boot stick and data stick. `tools/stick.py` refuses the flashed image and any stick whose
  boot loader or menu is another system's. The smoke test passes a `bootvol=` and the self
  test writes boot sectors with it onto the scratch ramdisk, partitioned and whole
  (`test_boot_volume`).
* **Files and programs** are in sections 9 and 10: FAT writes what macOS, Windows and
  Linux expect (FSInfo, the clean-shutdown bit, UTF-8 long names), `mkfs` lays volumes out
  as Windows does, and the editor and the file programs sit on `libsurf`.
* **Tests.** `make unittest`: `test_part` (MBR, logical chains and GPT built by hand,
  intact and damaged, the CRCs and the boot label, then fdisk's plans carried out in RAM);
  `tests/test_part_tools.py` (disks that SurfOS's plans write must satisfy sfdisk and
  sgdisk, and disks those tools write, a damaged GPT among them, must read back in SurfOS
  as the tools describe them); `test_mkfs` (every boot sector field, FSInfo, the backups
  and the alignment, on images from 16 MB to 40 GB, then `fsck.fat -n` and mtools when they
  are installed); `test_libc` (the formatter, `sscanf`, `strtol`, the strings, the
  calendar and `qsort` against the host libc, and `malloc` under a random workload). `make
  test`: the smoke test partitions a blank 64 MB scratch module, `rd1` (`tools/qemu-run.py
  --scratch`), MBR and GPT, formats it FAT16 and FAT32 and as a whole disk, mounts,
  remounts read-only and ejects it, checks the refusals (`hda`, the mounted `rd0`, a wrong
  answer), and runs `fstest`, `sh` with redirection, `cp -r`, `rm -r` and the editor over
  the serial line on `/hda1`. `selftest` adds the boot drive check and fdisk's second look
  on `rd1`, rename, statfs, UTF-8 names (left on `/hda1` for mtools to list), the cache
  under four tasks at once, a FAT volume whose device disappears while a file is open, and
  raw mode ending with its task. `make test-usb`: in `usbdisk` a copy of the test disk on a
  second emulated stick must become `usb1` with `usb1p1`, nothing may be mounted at boot,
  the boot drive is refused; the stick is mounted, read, written, unplugged while mounted
  (I/O errors, no crash, `umount` still works), plugged in again under the same name, and
  what was written is still there. `storage` boots with a blank 1 GB stick and a
  "foreign" one (`build/images/foreign-fat32.img`, made by `tools/mkimage.py` with
  mkfs.fat and mtools: long, accented and macOS-style names, folders, a 1 MB file): the
  refusals and the read-only boot drive, the foreign stick read and written, then `fdisk`,
  `mkfs`, `fstest`, `sh`, `mv`, the editor, `df` and `eject` on the blank one. Afterwards
  the host checks both images with macOS's `fsck_msdos` (attached by `hdiutil -nomount
  -readonly`), with `fsck.fat -n` (on the host or in the devenv container), and reads the
  files back with the FAT32 reader of `tools/mkusb.py`.
* **Limits.** Typing stays ASCII: a UTF-8 name is stored, read and (since 10/6/26, section
  7) shown correctly, but cannot be typed. `umount` and `eject` see only the asking
  task's current directory: a shell on another console that is still inside the mount does
  not stop them (its commands then find nothing there). No hubs, so a stick behind a hub,
  a dock or a dongle is not seen. `fdisk add` makes primary partitions only (four in an
  MBR); logical partitions are read, and deleted only with their extended partition. The
  cache keys a block by device and number, so a disk and one of its partitions could hold
  two copies of one sector: mounts are exclusive across a disk and its partitions, and
  `fdisk` and `mkfs` write around the cache and drop both devices' blocks, so nothing does
  this today, but the cache itself does not prevent it. `mkfs` makes no FAT12 (which is
  read and written). 512-byte sectors only: a drive with 4096-byte sectors is refused
  (`+USB: port …: 4096-byte blocks are not supported`), and a disk of 2 TB or more is used
  up to 2 TB. In user space `time_t` is a signed 32-bit count, so the clock wraps in
  January 2038; `lseek` takes a signed 32-bit offset, so a
  file can grow to FAT's 4 GB limit but positions past 2 GB cannot be sought.

## 17. surfsh, the SurfOS shell (ai-dev, October 2026)

The owner asked (2026-10-06) for the shell to become a program, as `bash` or `sh` is
elsewhere, started by the system on every console, with a prompt of SurfOS's own design,
Tab completion, a pager, and the banner of SurfOS 1.0 (`docs/USB-BOOT.md` section 10; the
brief is `docs/superpowers/plans/2026-10-06-surfsh.md`). The layers, from the top:

```
surfsh (user/surfsh/)  ring 3: the line editor in raw mode, Tab, parsing, its own commands, programs
prompt_render()        shell/prompt.c: breadcrumbs, the wave and the state (the ring-0 shell's too)
SYS_kcmd, SYS_kcmdinfo kernel/syscall.c: the ring-0 shell's commands in surfsh's task; kept output for >
shell_run_command()    shell/main.c: the command table, statuses, shell_ask()
TTY_PAGE, TTY_CAPS     kernel/tty.c: who is paged; what the console draws
kputch()               kernel/console.c: kept output, the pager, UTF-8 into code page 437 cells
init                   kernel/task.c: surfsh -l on F1-F4, started again; the ring-0 shell for recovery
```

* **The prompt** (`shell/prompt.c`, `include/surfos/prompt.h`), as the owner designed it:
  the working directory as breadcrumbs, ` ▸ ` between the names and no leading `/`
  (`/initrd/bin` is `initrd ▸ bin`), `SURF` for the root, then the wave `≋` and one state
  character before `>`: none (normal), `!` (the last command failed), `@` (a remote session,
  its host name the first breadcrumb), `#` (elevated), `?` (diagnostic). Only one is shown:
  diagnostic before elevated before remote before failed. A long path keeps its last names
  after `…`: at most `max_depth` items (5 unless set; `…` counts as one) and at most half the
  terminal's width (at least 20 columns); when even `… ▸ name` is too wide, the name keeps
  its last characters after `…`. Every character counts as one column, as on the console,
  and a control character in a name is drawn as `?`. Without Unicode the same prompt is
  ASCII (`initrd > bin ~>`, `...`); with colour it is light blue, the prompt's colour since
  2004, its state character light red (`!`), yellow (`#`), light magenta (`@`) or orange
  (`?`), and the colours end before the trailing space. The renderer is plain C without
  library calls: the kernel links it, libsurf builds it again (`user/libsurf/prompt.c`) and
  `tests/host/test_prompt.c` runs it on the host. surfsh is never elevated or diagnostic,
  so it shows `≋>` or `≋!>`, and `≋@>` in a telnet session (on a pseudo-terminal, section
  18); the ring-0 shell is elevated, and diagnostic as a recovery console.
* **surfsh's parts** (`user/surfsh/`, about 1,900 lines; `surfsh.h` lists them): `main.c`
  (the start: the settings from `/initrd/etc/surfsh.conf`, then with `-l` the banner of
  `do_banner()` from `surfos/version.h`; the loop), `editor.c` (the line), `complete.c`
  (Tab), `parse.c`, `run.c`, and `builtins.c`: `cd [dir]` (none: `/`; `-`: the one before),
  `pwd`, `echo [-n] [-e]`, `exit [n]`, `help [name]`, `history`, `set [key [value]]` for
  `prompt.unicode`, `prompt.color` and `prompt.max_depth` (also `key=value`, as the file
  writes them). The banner and the prompt are coloured only where `TTY_CAPS` says the
  console has colours, and drawn with `▸ ≋` only where it says UTF-8. With input that is
  not the console (`surfsh < file`) it runs the lines without a prompt. Statuses: 0, 1 a
  failure, 2 a usage or syntax error (a line too long included), 126 a program that could
  not be started, 127 no such command, 137 a program the kernel killed (128 + 9); `exit`
  keeps its status in 0-255, so a shell that ends is never mistaken for a killed one.
* **Lines** (`parse.c`). Words are separated by blanks; `"double"` and `'single'` quotes
  and `\` keep blanks and the characters `< > >> & ; #` in a word, and `|` is refused.
  `$?`, unquoted or in double quotes, is the one expansion: the status of the command
  before, in decimal. The whole line is checked first (`parse_check()`), then each command
  is read again just before it runs (`parse_next()`), so in `ls x; echo $?` the echo sees the
  status of ls. A line holds at most 1023 bytes and a command 32 words; a word of more than
  255 bytes, which the kernel would answer with EFAULT, is refused by surfsh with that
  reason. **10/24/26**: `parse.c` makes a tree of the whole command (lists, `&&` and `||`,
  pipelines, groups, redirections, here-documents), and the words are expanded only when
  their command runs, so `$?` is still the status just before (section 21).
* **Which command runs** (`run.c`): surfsh's own, then a program, `/initrd/bin/<name>` (the
  directories of `PATH`, section 21) or a path when the name has a `/`, then the kernel's (a
  name that `SYS_kcmdinfo` listed when surfsh started, run with `kcmd()`). **10/25/26**
  (track A): the programs come before the kernel's commands of the same names, which came
  first until then, in the ring-0 shell's order. So `ls`, `cat`, `cp`, `df`, `hexdump`,
  `mkdir`, `mv`, `rm` and `rmdir` are the programs (`ls -l`, `echo x | cat` and `cat < file`
  work); `kernel NAME` runs the kernel's command, and `type NAME` says which one runs and
  what it hides. `help` marks with `*` a kernel command that a program hides and a program
  that surfsh's own hides (`echo`). The `hexdump` program hands what is no file to the
  kernel's (`hexdump usb0 0`: a block device's blocks), and `df` has the kernel df's columns.
  The kernel's `help`, `echo`, `cd` and `pwd` give way to surfsh's own. On a kernel without
  the bridge (ENOSYS) there are no kernel commands; a name the table lists is the kernel's
  whatever `kcmd()` answers.
* **Redirection.** A program gets its files from `spawnfd()`. A builtin or a kernel command
  runs in surfsh's own task, so its file is put on descriptor 1 (`dup2`) and taken off
  again afterwards from a copy of descriptor 1 made at the start (`dup2(1, 15)`, the last
  slot, which no program inherits) or, failing that, from `/dev/console`; for a kernel
  command the kernel keeps the output and writes it into that file when the command is
  done (section 10). What stdout could not write (a full disk) is dropped with `fpurge()`
  rather than shown on the console later, and the command fails. `<` and `&` are refused
  for builtins and kernel commands, with a hint when a program of the name exists.
  **10/24/26**: every kind of command is redirected the same way, the descriptor's file kept
  on a spare descriptor while it runs, and a program gets surfsh's 0, 1 and 2 as they are
  then (`SYS_spawnx`); `<` is refused for kernel commands only, `&` for none (section 21).
* **Background programs** (`&`) are not waited for (`[pid] started`); before each prompt
  the ones that have ended are collected with `waitpid(-1, &status, WNOHANG)` and reported
  (`[pid] done`, `[pid] exit status n`, `[pid] killed`). Their files were closed when they
  exited (section 5). **10/24/26**: a job in the background has a number, `[1] started (pid
  42)`, and its report names it, `[1] done  cmd` (section 21).
* **Paging.** Every command in the foreground, builtin, kernel command or program, runs
  between `ttyctl(TTY_PAGE, 1)` and `ttyctl(TTY_PAGE, 0)`, so long output (`help`, long on
  purpose) stops at `--More--` (section 7). The lines surfsh itself prints after a program,
  `[exit status n]` and `[pid n killed]`, come after paging ends, so q cannot hide them.
  **10/24/26**: a command typed at the prompt is paged as a whole, a pipeline or a list
  included, between one `TTY_PAGE 1` and its `TTY_PAGE 0`.
* **Tab** (`complete.c`) reads the line up to the cursor with the parser's rules. The first
  word of a command, without a `/`, completes against the builtins, the kernel's commands
  and the programs; anything else is a path, relative to the current directory, with `.`,
  `..` and dot files only after a typed dot. One candidate is completed (`/` after a
  directory, a space otherwise), several as far as they agree, cut at a character
  boundary; a second Tab lists them in columns, asking first ("Display all N
  possibilities?") when they would not fit on the screen, since the list is written in raw
  mode, where the pager does not act. A name with blanks or special characters is
  completed inside double quotes.
* **The line editor** (`editor.c`) uses only raw mode, `TTY_GETSIZE` and the ANSI subset of
  `tty.h`, the same on the screen and over the serial line. Raw mode is on only while a
  line is typed: whatever runs finds the console in line mode, where the kernel's commands
  ask their questions and the pager can stop. Keys: Left, Right, Home, End (^A ^E),
  Backspace, Delete, ^U, ^K, ^W, Up and Down through 128 lines of history (no line twice in
  a row), Tab, ^L, ^C (the line dropped), ^D (on an empty line, the end); typed text is
  teal, and a terminal's escape sequences are read to their final byte, so one the editor
  does not know leaves nothing in the line. It draws only on its own rows. The cursor is a
  count of columns from the prompt's start, moved only with CR and `ESC[nA/B/C`, and never
  left in the console's pending wrap (CR LF where the prompt or the line ends exactly at a
  row's end). An edit rewrites the line from the first changed character and blanks only
  the cells it no longer needs, so whatever lies below (a program's drawing, `ansi`'s corner)
  stays unless the line grows over it. Before each prompt `fresh_row()` resets the colours
  and shows the cursor (a kernel command runs in surfsh's task, and what it leaves behind is
  not undone by a task's end), then writes a row of spaces, CR and `ESC[K`: the prompt
  starts on a fresh row wherever the last output stopped, without leaving an empty one.
  Only Ctrl-L clears the screen (`ESC[2J ESC[H`); the prompt is written again only after
  Ctrl-L, Ctrl-C or a list. Keys that arrive together (a paste) are all handled before the
  screen is written, and keys typed after Enter are dropped, as the console drops its queue
  when raw mode ends; 10/6/26: the rest of such a paste is then skipped until the input
  pauses (`TTY_SKIP`, section 18), so no part of a pasted line ever runs as a command.
* **Supervision** (`kernel/task.c`, section 3): init starts `/initrd/bin/surfsh -l` on F1-F4
  (`TF_SHELL`) and starts it again when it ends, quietly after an exit with status 0 and
  with a line otherwise. Three abnormal ends within 30 seconds (killed, or ended within 2
  seconds of its start), or a surfsh that cannot be started or loaded, give that console
  the ring-0 shell as a recovery console until the next boot, with init's line shown before
  its first prompt; a spawn that fails for want of memory is tried again every second.
  `shell=kernel` gives every console the ring-0 shell, and the USB image's GRUB menu has an
  entry for it ("SurfOS (kernel shell)").
* **Tests.** `make unittest`: `test_prompt` (the states and their priority, shortening by
  depth and by width, ASCII, colours, odd names, small buffers), `test_cp437` (the tables,
  the decoder's corner cases, the font's glyphs) and `test_pager` (the real `console.c` and
  `tty.c` on the host: exact fits and wraps, the keys, where a task may not wait, the
  call-off in both orders, a killed asker, the owner's end, takeover and EBUSY, the drop
  and its replay, the serial bytes of the question). `make test` drives surfsh on every
  console over the serial line; the harness's `PROMPT` pattern matches every prompt state
  and answers `--More--` with Space unless a step turns that off. surfsh's block starts a
  nested shell and covers the prompt and its states, `$?`, Tab for commands and paths,
  history, a line over the limit, kernel commands' statuses and redirection, `<` refused, a
  background program reported at the next prompt, a killed program, `help` paged, `set`
  and `exit`. The pager's steps answer with Space, Enter and q from the serial line and on
  the PS/2 keyboard (read back from VGA memory), and check a question after q, a crash
  report after q and a background job during a paged command. `@surfsh-corner` checks that
  `ansi`'s Z stays in VGA memory's corner through a prompt and an edited, cancelled line.
  `kcmdtest` covers the bridge's statuses, limits, errors and redirection, heaptests and
  pings from several processes at once, and selftest from a process with a child. `@boot`
  steps boot again with other kernel words or initrds: `shell=kernel` (`≋#>`), `ascii`
  (`~>`), no initrd (a recovery console at once), a surfsh that exits at once (started
  again twice, then recovery), one that cannot be loaded, and the old `sh` in its place.
  `make test-usb` drives it on the framebuffer and the keyboard: `help` page by page, Tab
  and history from the PS/2 keyboard, UTF-8 read back through the font in `fullhd`, and the
  `kernel-shell` scenario boots the GRUB entry.
* **Stopping a command** (`kernel/task.c`, `kernel/tty.c`). A task that another task kills
  while it is inside a call (a system call, a kernel command run by the ring-0 shell or by
  SYS_kcmd, a process until its program is loaded) is only marked: its interruptible waits
  end at once (terminal reads and the pager, `sleep_intr()`, `wait_on_timeout()` and the
  network's waits, the wait for a child; they answer -1 or -EINTR), the command gives up
  along its usual way out (fdisk and mkfs cancel their question, cp closes its files, ping,
  cat, fsbench and readbench stop, the self test ends between tests with `SELFTEST STOPPED`
  and releases its claim), and at the end of the call the task exits with -1 in its own
  context, so its files close and nothing is left held. Mutexes, semaphores, untimed
  events, the block cache and drivers' waits are not cut short. A task that has not ended
  its call 2 seconds after the kill is killed at once from the timer, with a line in the
  kernel log (`sleep -u` shows it); faults kill at once, as before. Ctrl+C in line mode
  while a shell pages a command in the foreground uses the same path: a kernel command
  running in the shell's own task is cut short and answers `KCMD_INTERRUPTED` (130) while
  the shell lives on, and the innermost programs started under the pager
  (`TF_FOREGROUND`) are killed (surfsh reports 137); programs in the background are left
  alone, and at a prompt, in raw mode or at `--More--` Ctrl+C is a key as before.
* **Limits.** Typing stays ASCII: in line mode the bytes above 0x7F are the keyboard's
  special keys, so accented letters show but cannot be typed. No pipes, no variables other
  than `$?`, no wildcards and no job control (a program in the background cannot be brought
  back): **10/24/26**, pipelines, lists and groups since then (section 21). A kill that has to fall back to killing at once after 2 seconds leaves what the
  task held (a lock, a file) as before, and a long `mkfs` is not cut short. The console's
  input queue holds 256 bytes, so a long paste over the serial line loses bytes. A serial
  terminal must be as wide as the console's grid for the editor's wrapping to look right
  there, since the width comes from `TTY_GETSIZE`. `heaptest` compares the heap's totals
  before and after, and `selftest` the number of tasks, so other consoles using the heap or
  starting programs meanwhile make them report failures; a self test killed at once (after
  the 2 seconds) keeps the next one refused until a reboot. A redirected kernel command
  takes a 256 KB buffer from the kernel heap each time.

## 18. Network connections, pseudo-terminals and telnetd (ai-dev, October 2026)

The owner asked (2026-10-06) to telnet from another computer on the network into the
laptop and land at a SurfOS prompt, with a program, `telnetd`, turned on and off and
running in the background, and nothing security-related (`docs/USB-BOOT.md` section 11;
the brief is `docs/superpowers/plans/2026-10-06-telnetd.md`). Programs had no network
connections (only kernel code used TCP), and a shell needs a terminal; so the work added
five things under the server. The layers, from the top:

```
telnetd (user/telnetd/)  ring 3: the CLI (on, off, status) and the server: one poll() loop, the telnet protocol
sockets                  net/sock.c: TCP connections and listening sockets as descriptors (SYS_listen ... SYS_netinfo)
pseudo-terminals         kernel/pty.c: a tty with a stream console; the master side is a descriptor (SYS_openpty ...)
poll()                   fs/poll.c: vnode_ops->poll, one wait queue woken by poll_wake()
processes                kernel/task.c: SYS_procinfo, SYS_detach; /dev/kmsg (fs/devfs.c) for the kernel log
TCP                      net/tcp.c: partial writes, the persist timer, keepalive, no silly windows, no Nagle for programs
```

* **Sockets** (`net/sock.c`, `include/surfos/sock.h`). A connection or a listening socket
  is a vnode of a small pseudo file system, opened with `vfs_open_vnode()`, so `read`,
  `write`, `close`, `dup2` and `spawnfd` work on it as on a file. `SYS_listen` (34) listens
  on a port on every interface, `SYS_accept` (35) takes the next connection with the
  peer's address, `SYS_connect` (36) makes one within 10 seconds, `SYS_netinfo` (37)
  describes the default interface; libsurf has `tcp_listen`, `tcp_accept`, `tcp_connect`,
  `netinfo`, `ipstr` and `ipparse`. A listener's backlog holds 4 connections waiting for
  `accept()`; since 10/17/26 `SYS_listen`'s second argument asks for up to 8
  (`tcp_listen_backlog()`; httpd asks for 6, as a browser opens six connections at once).
  There are 16 TCP sockets (8 before); programs and
  connections coming in leave the last free one to the kernel's own commands. `read()`
  waits as long as it takes (a kill or Ctrl+C ends the wait), `write()` queues what fits
  in the 8 KB send buffer and returns that count, waiting only while nothing fits, and
  `close()` never waits: the FIN follows the queued bytes and the socket finishes on its
  own. Errors are POSIX's (ECONNRESET, ETIMEDOUT, ECONNREFUSED, EHOSTUNREACH,
  ENETUNREACH, EADDRINUSE, ...), with Linux's numbers. 10/24/26 (section 22): `SYS_shutdown`
  (50) half-closes a connection, `SYS_sockaddr` (51) gives both its ends, and
  `SYS_connect`'s `CONNECT_NOWAIT` hands one out before its handshake is done.
* **TCP** (`net/tcp.c`), reviewed twice and hardened for a server that must outlive bad
  clients. A handshake that dies leaves the listener's backlog at once, the SYN-ACK gives
  up after 4 tries, a SYN that finds the backlog full replaces the oldest half-open entry
  (or is dropped when every entry is a whole connection), and a stray ACK reaching a listener is answered with a reset (RFC 793), as is anything
  for a connection that failed. A receiver that stops reading shuts its window and is
  probed by the persist timer (1 s, doubling to 60 s, not counted as retries) until it
  reads again; a window shrunk to zero with bytes in flight is probed the same way rather
  than retransmitted into, and a probe carrying a byte (BSD, macOS, Windows) is answered.
  A retransmission carries only bytes that were sent. Keepalive probes a connection silent
  for 120 s every 10 s and gives it up (ETIMEDOUT) after 5 unanswered probes
  (`tcpka=<idle>,<interval>` on the command line shortens it for tests). A program's
  writes go out at once, however small (no Nagle delay, which against the 40-200 ms
  delayed ACKs of real clients made typing over telnet lag); the kernel's own connections
  keep Nagle's rule, and on both only a window too small for a sensible segment holds a
  small one back, a second at most (silly-window avoidance on both sides). An orphan (a
  closed connection still sending) frees its receive buffer at once and its send buffer
  once that is empty, is reset after 30 s without progress, and TIME_WAIT keeps no
  buffers. Ports are checked as 32-bit numbers (above 65535: EINVAL), and every sequence
  comparison is modular. Not done: random initial sequence numbers, challenge ACKs (RFC
  5961).
* **Pseudo-terminals** (`kernel/pty.c`, `include/surfos/pty.h`; 8 of them). A pty is a
  `struct tty` whose console is a *stream console*: `kputch()` on it appends bytes to the
  master's 8 KB output ring instead of drawing cells (no UTF-8 decoding, the remote
  terminal does it; a kernel message's colour becomes an ANSI SGR sequence), and the pager
  counts the rows and columns that `PTY_SETSIZE` gave (80x24 at first), asking its question
  in the stream. `SYS_openpty` (39) gives the master side as a descriptor, `SYS_spawnpty`
  (40) starts a program whose terminal is the pty (its descriptors 0-2, `/dev/console`,
  `curTask->con` and `->tty`, so the kernel's commands it runs print there too), and
  `SYS_ptyctl` (41) sets the size (`PTY_SETSIZE`) or names the pty (`PTY_INDEX`). Reading
  the master gives what the session printed, as it was printed, and 0 once the session has
  ended and everything was read. Writing it types keys with the serial line's input rules
  (`tty_term_input()`, shared with the serial interrupt): CR is Enter and DEL Backspace in
  line mode, bytes pass as they are in raw mode, Ctrl+C interrupts the foreground job. A
  master write never waits: it takes the bytes that fit in the 256-key input queue and
  answers how many, or EAGAIN; a byte that needs no room (a line-mode Ctrl+C) always fits;
  POLLOUT says a key fits. A line-mode Ctrl+C also drops the keys typed ahead, on the
  consoles as on ptys, so type-ahead never runs after an interrupt, and it ends an escape
  sequence in progress, so cursor keys that filled the queue cannot swallow it. When raw
  mode ends with keys still queued, or a program says it dropped keys of its own
  (`ttyctl(TTY_SKIP, 1)`: surfsh after an Enter with more keys behind it), the serial line
  and the ptys skip their input until it pauses for 200 ms: the rest of a pasted block is
  dropped whole instead of arriving at the next prompt in pieces. A Ctrl+C always gets
  through, a lone LF or NUL left behind by an Enter starts no skip, and the PS/2
  keyboard is never skipped. `TTY_GETSIZE` on a
  pty answers its size, `TTY_CAPS` UTF-8 and colour; raw mode and the pager work as on a
  console, and a pty's output never reaches the screen or the serial mirror.
* **Hangup.** Closing the last master descriptor hangs the pty up: each process whose
  terminal it is gets a kill that waits for the end of its call however long that takes
  (never the 2-second fallback of section 17, which would leave a lock held: the kernel log
  says once that it is waiting), its reads end, its output is dropped, and the pty is free
  once they are gone. A kernel thread started from a session (`tcpecho`, the self test's
  helpers) is put on F1 instead of the pty, so it never belongs to a session: its output
  goes to the laptop's screen, and it outlives the session as it would on F1.
* **poll()** (`fs/poll.c`, `include/surfos/poll.h`, `SYS_poll` 38): up to 64 descriptors,
  a timeout in milliseconds (-1: none), POLLIN, POLLOUT, POLLERR, POLLHUP and POLLNVAL.
  `vnode_ops->poll` (appended to the table) says what a descriptor is ready for: sockets,
  pty masters and the console have one; a file or `/dev/null`, without one, is always
  ready. Whatever changes readiness calls `poll_wake()`, which wakes every task in
  `poll()` (one wait queue, a counter checked with interrupts off so no wake-up is lost);
  a poll op itself must never call it.
* **Processes** (`include/surfos/proc.h`). `SYS_procinfo` (42) gives the task list entry by
  entry (pid, parent, state, flags user/detached/shell, terminal: 0-3 for F1-F4,
  `PROC_TTY_PTY` + n for pty n, the name), for telnetd to find its server; `SYS_detach`
  (43) makes the caller init's child, out of its shell's job (Ctrl+C there no longer
  reaches it), keeping its descriptors and terminal. `/dev/kmsg` takes lines into the
  kernel log (`dmesg`).
* **telnetd** (`user/telnetd/`: `main.c` the commands, `server.c` the loop, `telnet.c` the
  protocol as a module without calls). `telnetd on [port]` looks for a running server in
  the process list (a detached user task named `telnetd`), takes the port itself with
  `tcp_listen()` so that "in use" is said at once, starts the server with `spawnfd()`, the
  listening descriptor as its descriptor 0, and prints the address `netinfo()` gives. The
  server `detach()`es and loops on `poll()` over the listener and each session's
  connection and pty master. A connection gets `openpty()`, `PTY_SETSIZE` 80x24 and
  `spawnpty("/initrd/bin/surfsh", {"surfsh", "-l"})`; the server offers WILL ECHO, WILL
  SUPPRESS-GO-AHEAD, DO SUPPRESS-GO-AHEAD, DO NAWS and DO BINARY (without it a BSD or
  macOS telnet strips the eighth bit of what is typed, so UTF-8 would arrive garbled; the
  server's own output stays text) and refuses every other option. From
  the client, IAC commands and sub-negotiations are read (NAWS becomes `PTY_SETSIZE`, IP
  and BRK Ctrl+C, EC DEL, EL Ctrl+U; AYT is answered, TIMING-MARK refused), CR LF and CR NUL
  become CR and IAC IAC a 0xFF byte; the keys wait in a 1 KB buffer per session and go to
  the master only on its POLLOUT. While the master takes keys, the client is read only
  when the buffer has room for a whole read, so TCP holds the rest at the client and a
  paste into a program that reads arrives whole; after a second in which the master took
  nothing, the client is read again and what does not fit is dropped, so a Ctrl+C or IAC IP
  always arrives. A Ctrl+C drops the keys waiting from earlier reads, writes the keys of
  its own read that came before it, then goes. Towards the client, LF becomes CR LF, a CR
  alone CR NUL and 0xFF
  IAC IAC, through a 4 KB buffer per session written on the socket's POLLOUT, with room kept
  for the protocol's answers, which go whole or not at all. Nothing blocks the loop, so a
  client that stops reading or floods keys slows no other. A session ends when surfsh does
  (the master reads 0 and is closed at once, the last output drains, then the connection
  closes), or when the client goes (the master is closed: a hangup). Up to 4 sessions: a
  fifth connection is told so and closed, as is one that finds no free pty, unless a
  session's client has taken no output for 60 s while output waits for it: the one stalled
  longest is ended to make room (a client behind a buffering proxy, or asleep, is never cut
  otherwise). The server collects its shells (polling at most every
  200 ms while one is ending), rests 1 s after an `accept()` error, and logs to
  `/dev/kmsg`: listening, each session (the client's address, the pty, surfsh's pid), each
  end with its reason. `telnetd off` logs, kills the server and waits for it; its
  descriptors close with it, so every session's master hangs up. surfsh shows the prompt's
  remote state, `≋@>`, when the process list says its terminal is a pty.
* **Tests.** `make unittest` adds `test_telnet` (the protocol, split at every byte),
  `test_telnetd` (`server.c` built unchanged against a simulated kernel and driven with
  random clients, floods, stalls and failures) and the stream console's pager checks in
  `test_pager`. `make test` adds `nettest` (an echo server and client through the
  forwarded port, connect errors, limits), `ptytest` (sessions, hangups at `--More--` and
  while a writer waits, the kill that waits, kernel threads on F1, a full input queue and
  Ctrl+C through it, poll over several descriptors, detach, a reused pty starting clean,
  isolation from F1 and the serial line), the `@tcp-raw` steps, and telnetd's steps:
  `@telnet-session` (the banner, the prompt, commands, Tab, a paged command, Ctrl+C,
  `exit`), `@telnet-two`, `@telnet-full` (a fifth connection), `@telnet-stalled` (a client
  that stops reading while another session works), `@telnet-flood` (a client flooding
  keys into a program that does not read, while another session stays quick),
  `@telnet-protocol` (a BSD-style opening, NAWS, AYT, TIMING-MARK, CR LF, DEL and BS, EC and
  EL, typed UTF-8, IAC IP, 0xFF in output, a reset), `@telnet-binary` (a client in BINARY
  mode typing UTF-8 and Enter as a bare CR), `@telnet-paste` (60 lines pasted into `edit`
  arrive whole), `@telnet-paste-lines` (a pasted block runs its first line only),
  `@telnet-ctrlc-keys` (keys in the same segment as a Ctrl+C), an arrow-key flood in
  `@telnet-flood`, `@telnet-off-inside` and `@telnet-port-busy`; `ptytest paste` checks the
  skip rule with a small raw-mode shell, and `@surfsh-crlf` that an Enter sent as CR LF over
  the serial line starts no skip. `make test-tcp` (`tools/qemu-tcp.py`, with `tools/rawnet.py`)
  drives the TCP stack with hand-made segments: half-open handshakes and backlog floods,
  an evicted client, a zero window, its probes and the persist timer, a shrunk window,
  silly windows, a lost segment, a burst's latency against a delayed-ACK peer, keepalive
  against a dead peer, resets in every state, orphans and TIME_WAIT, and a `poll()` server
  under a SYN scan and a stalled client. `make test-usb`'s `usbnet` scenario runs a telnet
  session over the USB Ethernet adapter.
* **Limits.** No security of any kind, by the owner's choice: anyone on the network who
  connects gets a shell with every kernel command, and telnet sends everything as plain
  text. The window size follows NAWS, but no terminal type (TTYPE) is asked for: a session
  always speaks the console's ANSI subset. surfsh has no type-ahead (section 17), so a
  pasted block of several lines runs only its first line.

## 19. The network manager and the settings on the stick (ai-dev, October 2026)

The owner asked (2026-10-17) for the Wi-Fi network they join to be saved as a file, joined
by itself at every start with DHCP after it, and for a settings file, telnet on at startup
for one, kept on the USB stick SurfOS runs from and read at boot (`docs/USB-BOOT.md` section
15; the brief is `docs/superpowers/plans/2026-10-17-network-manager.md`).

```
commands                 shell/settings.c (settings), shell/wifi.c (wifi save | saved | forget | connect), dhcp renew
the manager              kernel/netman.c: the settings and the saved networks in RAM, the stick read and written,
                         the netman thread (boot, Ethernet's DHCP, lease renewals, telnetd) and wifiman (the keeper)
the files' text          net/netcfg.c: settings.conf edited line by line, networks.conf written anew, SSIDs quoted,
                         the lease timers (pure, host-tested: tests/host/test_netcfg.c)
what it stands on        driver/part.c bdev_boot_vol(); net/wlan/wlan.c wlan_station(); net/dhcp.c leases per
                         interface, dhcp_renew(), option 12; kernel/autonet.c for the development entry
```

* **Where.** GRUB's menu names the volume it was started from (`bootvol=`, section 16, "One
  stick"); `driver/part.c` keeps it as `bdev_boot_vol()`: the one-stick volume, which may be
  written (`bdev_mount_readonly()` says no), or a SURFOSUSB image, which is only read. Its
  folder `surfos-config/` holds `settings.conf` and `networks.conf`. The volume is mounted only
  for the moment a file is read or written, on `/.netman` (read-only for a read, `MNT_QUIET`:
  its FAT line goes to the log), or used where the owner mounted it. Either mount is held for
  that moment (`vfs_hold_mount()`: its root vnode, so `umount` and `eject` say it is busy and
  the path cannot come to name another device), and `stick_lock` keeps it to one at a time. A
  mount being made is noted under the mount table's lock before the file system reads the
  volume, so a command's `mount` of the same stick meanwhile is refused instead of making a
  second superblock for one volume; so is a mount of a device still being unmounted (it stays
  noted until its blocks are let go, which would drop a new mount's) (`fs/vfs.c`). Without
  `bootvol=` (QEMU's `-kernel`, the ISO) nothing is read, nothing joined and no host name sent
  (unless `settings hostname` is typed); the kernel word `settings=off` (the menu's entry
  "without the settings on the stick") skips the reading the same way, and a change made then
  is saved for the next start without putting anything in force now.
* **Writing.** Every change first reads the file from the stick, so nothing another
  computer added is lost: `settings.conf` is edited one line at a time (the line of the key
  changed in place with its indentation and comment, the comment moved to a line of its own
  when the line would grow past the 255 bytes a reader takes, others setting it dropped, a
  commented-out `#key = value` of the new file's template made the setting, or a line added),
  `networks.conf` is written anew from the list read and changed, and what its reader could
  not take is kept byte for byte (`struct netcfg_span`): the lines before the first network
  ahead of the networks, the blocks of networks it left out after them under a comment, so a
  name written twice stays the second. A change is refused rather than written when the file
  has 16 such parts or more (those that follow each other counting as one), a part kept so with
  a password line in it (SurfOS would be writing that password), a password that makes no key,
  or when the text about to be written would not read back as exactly the networks meant
  (`netcfg_reads_back()`: forgetting the first of a name written twice by hand would make the
  second the network). The text goes to `<name>.new`, which is synced, renamed over the
  file and read back and compared (through the block cache: the check is of what the file
  system wrote); then the short mount goes (its unmount syncs the cache and the stick's own).
  FAT's rename over a file writes the new entry before it deletes the old: a stick pulled out
  in that moment can hold the file twice, which a check on another computer repairs; before
  the rename the old file is as it was.
* **The files' text** (`net/netcfg.c`). Lines end with LF, CR LF or a CR alone; a UTF-8 byte
  order mark is skipped (and kept by an edit); a line longer than 255 bytes or with a NUL in it
  is told, and skipped in `settings.conf`; `#` starts a comment outside quotes, and TextEdit's
  curly quotes count as quotes. In `networks.conf` a block with a problem (no name, a bad key or
  password, a bare password with a `#` in it, a security SurfOS does not speak, a name seen
  before, a 17th network, a line too long, with a NUL or not `key = value`) is left out whole,
  its note naming it; so is a block with a line SurfOS does not understand and no `security`
  line, which is never taken as an open network (a mistyped `password` would make one). An
  unknown key or an unreadable `hidden` value in a block with its `security` is only told: the
  block is used, and written anew without that line.
* **Keys, not passwords.** A saved WPA2 network holds its PSK, the PMK (PBKDF2 of the
  password and the SSID). `wifi save` takes the one the station joined with
  (`wlan_station(..., want_key)`: the station keeps it for its reconnects); `wifi save
  <ssid>` asks the password with `shell_ask_secret()` and makes it at once. A password typed
  into the file on another computer is made into the key when the file is read and is never
  written back. Keys stay in RAM for the keeper; `netman_networks()` gives the commands
  everything but the key, nothing logs one, and buffers that held one are wiped.
* **At boot** (the `netman` thread on F1, started after `init_autonet()`): it waits up to 20
  seconds for the volume (USB sticks are found during the boot; the laptop's take a few
  seconds), and under `stick_lock` reads both files (each line it does not understand said with
  its number, the rest used) and puts them in force, but for a key the `settings` command set
  meanwhile (`cmd_set`); then the host name of DHCP requests (option 12). Every second after
  that: an Ethernet interface whose link comes up gets a lease when `ethernet = dhcp` (30 s,
  doubling to 5 minutes, between tries without one, the repeats in the log only; not while
  autonet runs), and one whose link comes back with the lease it had when the link went has the
  address asked for again (`DHCP_REBOOTING`: option 50, no ciaddr, RFC 2131 4.3.2; a NAK drops
  it; an interface's first sight is no edge); every lease
  is kept, in every start (`netcfg_lease_step()`: at half its time `dhcp_renew()` asks the
  server that granted it, ciaddr set and no server identifier, as RFC 2131 4.4.5 has it, at
  seven eighths any server, with half the time left between tries; a NAK or the end drops the
  address, `dhcp_drop()`). A renewal and a drop name the lease they are about (`lease_at`) and
  do nothing when the interface has another by then or an address set by hand. A dhcp waits up
  to 30 s for one running on another interface and counts its own time from when it has port
  68; the manager's repeated tries say their waits and failures in the log only. `telnetd =
  on`, as the file said at boot, starts `telnetd on [port]` as a process on F1 once an interface
  has an address it may start for (`netman_trusted()`: Ethernet, or Wi-Fi up on a WPA2 network;
  never an open one, which anyone nearby may join; but SurfOS cannot tell whose network it is),
  unless a telnetd runs already, and collects it if it is slow. With the kernel word `autonet`
  (GRUB's development entry) it waits until autonet is done, which starts one itself, on 23, for
  an Ethernet address.
* **The Wi-Fi keeper** (`wifiman`, when `wifi = on`, a network is saved and a Wi-Fi device
  exists). It brings the card up, scans, and joins the first saved network heard in that
  scan (the scan list keeps a network a minute: one gone silent must not be picked), then
  each saved hidden one not heard, by a probe for its name; then it asks for the address, and
  for as long as that association has none asks again (30 s, doubling to 5 minutes). The scan
  list marks a BSSID whose beacons carry no SSID (`hidden` in `struct wlan_bss`), which `wifi
  save` keeps. A key refused three times in a row (`-EACCES` from the 4-way handshake, which a
  lost frame gives as well as a wrong key) is set aside for 10 minutes, said on F1 the first
  time. While a telnet server runs (any live user task named telnetd counts, a `telnetd
  status` too, looked for three times as the list shifts; the setting's start waits for the
  detached server itself) or is to start by itself (`telnetd =
  on` until it has started, autonet until it is done), an open saved network is not joined by
  itself, looked at again just before each: it is said, and `wifi join` joins it (saying the
  server answers there too). The keeper's repeated tries pass `quiet` to the station
  (`wlan_join_unless()`), whose failure to join, and the supplicant's `+WPA:` lines (`struct
  wpa_ops` `say`), then go to the log only. Afterwards the keeper
  reads `wlan_station()` every second, a
  snapshot that makes no request (so no `wifi` command is ever told the device is busy by it):
  while the station is up, joining or reconnecting it does nothing but get an address for an
  association of its own; after a `wifi leave` or `wifi down` (the station's `stops` count
  changed) it stays out until the station gives up a network (`WLAN_IDLE_GAVE_UP`, after its
  five reconnects: about a minute) or `wifi connect` hands the card back; then it looks for a
  saved one every 30 s, every 2 minutes after 20 tries, the repeats in the log only. Its up and
  join requests carry the `stops` count of its snapshot, and the station refuses them
  (`-ECANCELED`) when a leave or down came since, so a command typed meanwhile always wins. An
  association is its own when the station's `join_requests` is the number its join request
  got (`wlan_join_unless(..., &join)`). `wifi connect` runs the same connect on the command's
  console, under the same `connect_lock`.
* **autonet** is as it was, but for Ethernet only: it waits up to 90 seconds for an Ethernet
  link (the routes prefer Ethernet, and `tools/devloop.py` telnets to that address), asks for
  its address, and starts telnetd for it; a Wi-Fi address is the keeper's, and telnetd for Wi-Fi
  the setting's. So the development entry opens telnet where it always did, never because of
  a Wi-Fi network SurfOS joined by itself. While it runs, the manager leaves Ethernet to it
  (`autonet_busy()`). `SYS_netinfo` gives the first interface that has an address when the
  default one has none, so telnetd's line names the address that answers.
* **Tests.** `tests/host/test_netcfg.c` (the files' text, every SSID byte through the
  quoting, CR alone, a byte order mark, NUL bytes, curly quotes, a long line edited, the line
  ends kept, the parts not understood kept through a write and read back, a password in one
  marked, `netcfg_reads_back()` refusing a forgotten name written twice, the lease timers); in
  `make test`, boots
  from a settings disk kept from boot to boot (the boot word `settingsdisk=`): the
  hand-written file read (its bad network told, left out and kept), joined and leased with
  nobody typing, saving, forgetting, a restart that joins the saved network and starts telnetd
  for its WPA2 address, `wifi leave` left alone, the access point switched off (`wlansim off`,
  a test command of the simulated chip) and SurfSim, open, not joined while telnetd runs, then
  joined once it is off; `settings=off`, a change and a network saved then for the next start
  only; a hidden network saved by hand joined by a probe, with no DHCP server on its island, and
  saves under `wifi = off`; an unsaved `telnetd` change that changes nothing; leases of 8 s
  (`dhcplease=8`) renewed, and confirmed after the
  link went down and up (QEMU's `set_link`); the disk read on the host afterwards. `make
  test-usb`'s `settings` scenario does it through GRUB's menu on a one-stick, under the laptop
  profile, and boots the menu's entry without the settings.
* **Limits.** One Wi-Fi device; DHCP only (an address by hand is `ifconfig`, not a setting);
  no per-network priorities beyond the file's order; at most 16 saved networks; settings are
  read at boot, so a stick plugged in later is not read until the next start, and `telnetd`
  and `telnetd.port` act at the next start. Once telnetd runs it answers on every network
  SurfOS is on, a network joined by hand included.

## 20. httpd, the web server (October 2026)

The owner asked (2026-10-17) for a simple web server that hosts websites off SurfOS, just
basic HTML and CSS (`docs/USB-BOOT.md` section 16; the brief is
`docs/superpowers/plans/2026-10-17-httpd.md`). It is a program on the sockets, `poll()`
and process calls of section 18, with one small kernel change (a listener may ask for a
longer backlog, section 18): `/initrd/bin/httpd`, from `user/httpd/`, laid out as `telnetd`
is.

```
main.c     ring 3: the CLI. httpd on [folder] [port], httpd serve [folder] [port], httpd off, httpd [status]
server.c   the server: one poll() loop over the listening socket and up to 6 connections
http.c     the HTTP/1.1 protocol, no system calls: request heads, paths, header fields, pages; host-tested
```

* **Starting and stopping.** `httpd on` finds a running server by `procinfo` (a detached
  user task named `httpd`), makes the folder absolute (`http_normalize()`, from the current
  directory) and checks that it is one, takes the port with `SYS_listen` itself, then
  starts the server as the same program with `--server <port> <folder>`, the listening
  socket as its descriptor 0 and `/dev/null` as 1 and 2 (`spawnfd`). The server
  `detach()`es, writes its start and its errors to `/dev/kmsg`, and `chdir("/")`s so that it
  holds no volume through its current directory. `httpd off` kills it; the kill ends it at
  the end of its `poll()`, and its descriptors close with it. `httpd serve` runs the same
  loop in the foreground and prints each request on its console instead (never in the
  kernel log, whose 16 KB a few hundred requests would fill). 10/24/26: each line it prints
  starts a new screenful of the pager first (`ttyctl(TTY_PAGE, 0)`): the user shell pages
  every command, and the server stopped at `--More--` after 22 requests, answering none until
  a key came. A server started in a telnet
  session is on the session's pseudo-terminal and ends at its hangup; `httpd on` says so.
* **The loop.** A connection reads its request head (`http_head_end()`; at most 4 KB, else
  414 or 431), then sends the response: the header, then the body from the file, or from
  memory for a folder's listing, a piece of up to 8 KB at a time, written as far as the
  connection takes it at each `POLLOUT`. A connection that sends is not read, so a request
  behind it (pipelining) waits in the kernel and is answered after it; HTTP/1.1 connections
  then wait for the next request (keep-alive). A connection gets `REQUEST_MS` (10 s) to
  begin a request and as long again from its first byte to finish it (then 408, or a plain
  close when nothing came); a response the client takes nothing of for 30 s is given up. A
  client that half-closes while its response is written makes `poll()` say `POLLHUP` at
  once whenever its window is full; such a connection rests 20 ms before it is polled
  again, so the loop never spins. Closing never loses a response: the kernel sends the FIN
  behind the queued bytes and drops what the client still sends (section 18).
* **Places.** Six connections: a connection and the file it sends take two of a process's
  16 descriptors, which with 0, 1, 2 and the kernel log's makes all 16. When the places are
  taken, a new connection takes the place of the one idle longest (waiting for a request
  with nothing of it read); when none is idle, the listening socket is left out of
  `poll()` and new connections wait in the kernel's backlog, which httpd asks to be 6 long
  (section 18: with the default 4, a browser's six SYNs at once lost two to a full backlog
  before the server could accept the first). The listener, 6 places and 6 waiting are 13
  of the 15 sockets programs may have (16, one kept for the kernel), so `telnetd` still
  gets its listener and a session under a full load.
* **Requests** (`http.c`). The request line (origin and absolute forms; the query
  ignored), `Host` (exactly one in HTTP/1.1), `Connection`, `Content-Length` and
  `Transfer-Encoding` (a body is never read: the connection closes after the response) and
  `If-None-Match`. The path is percent-decoded and cut into names: "." and empty names go,
  ".." is refused (400: browsers resolve it before they send, so it only comes from a
  request trying to leave the folder; encoded too), as are a NUL, controls and backslashes;
  a name starting with a dot is hidden (404), which covers what macOS leaves on FAT sticks.
  FAT lookups fold case but match only a long name, never its 8.3 alias, so a hidden file
  cannot be reached as `SECRET~1`. Statuses: 200, 301, 304, 400, 404, 405 (with `Allow`),
  408, 414, 431, 500, 505; every error but 404 closes the connection.
* **Responses.** The file path is the folder and the request's path (414 when it is longer
  than a path can be). A folder asked without its `/` is redirected to it (301), since its
  page's relative links need it; its `index.html`, else `index.htm`, is its page, else it
  is listed (`http_listing()`: names escaped, links percent-encoded, folders first, sizes
  and times, at most 512 entries, dot names left out). A file is opened for each request
  and read as it is sent, so edits show at once; its ETag is its size and time in hex,
  `Cache-Control: no-cache` makes the browser ask every time, and an `If-None-Match` that
  holds the ETag gets 304. There is no `Date` and no `Last-Modified`: the CMOS clock keeps
  local time with no zone, which HTTP must not pass off as GMT. A file that ends before
  the length its header gave (it shrank, its stick went) ends the connection.
* **Tests.** `make unittest` adds `test_httpd` (`http.c` unchanged: heads in every prefix,
  every parse rule and status, traversal plain and encoded, path limits, fields, header
  bytes, types, ETags, encoding, listings, folder arguments). `make test` adds the
  commands' refusals and the checks `@httpd-site` (the sample site byte for byte with its
  types, HEAD, 304, 404s), `@httpd-protocol` (400s, 405, 414, 431, 505, the absolute form,
  HTTP/1.0), `@httpd-keepalive` (a connection for several requests, three pipelined, a body
  that closes), `@httpd-places` (idle connections making room, busy ones making a new one
  wait, eight clients side by side), `@httpd-timeouts`, `@httpd-initrd` (the initrd's root
  on port 8080: listings, a 301, a 188 KB file), `@httpd-serve` (a site made on the FAT
  disk, the request lines on the console, a hidden file refused also through its 8.3 alias
  `SECRET~1`, Ctrl+C) and `@httpd-slow` (a client that stops reading a 2.5 MB file on the
  FAT disk while others are served, then reads it whole).
  The smoke test forwards guest ports 80 and 8080 (`q.ports['www']`, `q.ports['www2']`).
  QEMU's user network takes the connections to a forwarded port one at a time (of two made
  in the same instant, one is reset on the host before the guest sees it); the checks make
  every connection 50 ms after the one before (`www_connect()`), and `make test-tcp`'s `backlog` scenario (`nettest sleepsrv <port>
  <ms> 8`) shows the guest holding eight whole connections and dropping a ninth SYN.
  `make test-usb`'s `usbnet` scenario fetches the sample site over the USB Ethernet adapter.
* **Limits.** GET and HEAD only: no forms, uploads, server-side programs or HTTPS; one
  background server; the address `httpd on` prints is the default interface's (`netinfo`).
  SurfOS has no loopback interface, so Undertow on the same machine cannot open the site.

## 21. Pipelines and scripting in surfsh (October 2026)

The owner asked (2026-10-24) for pipelines and useful scripting in surfsh, in stages:
bounded kernel pipes, pipelines, environment variables, script files, and job control
(`docs/USB-BOOT.md` section 10; the brief is
`docs/superpowers/plans/2026-10-24-surfsh-scripting.md`). The layers, from the top:

```
surfsh (user/surfsh/)  parse.c: a tree of lists, pipelines, groups; exec.c runs it; a new surfsh for a subshell
                       vars.c, expand.c, arith.c, glob.c: variables, the environment, $( ), $(( )), wildcards
                       if while until for case, functions, script.c test.c printf.c: the builtins of scripts
libsurf                pipe(); spawnx(), and spawn()/spawnfd() over it with environ; a broken pipe ends a program (141)
SYS_spawnx             kernel/process.c: descriptors 0-3 as the caller says, an environment, a job in the background;
                       #! scripts, and ENOEXEC for a file that is no program
SYS_waitx, TTY_INTR    kernel/task.c, kernel/tty.c: how a child ended (killed by Ctrl+C?), Ctrl+C for shells
SYS_job                kernel/task.c, kernel/tty.c: jobs, the terminal's foreground job, Ctrl+Z, stopped tasks
SYS_pipe               fs/pipe.c: 8 KB between two descriptors, 32 pipes at most
```

* **Pipes** (`fs/pipe.c`, `include/surfos/pipe.h`). `SYS_pipe` (46) makes a ring of
  `PIPE_SIZE` (8 KB) bytes from the kernel heap with two ends, each a vnode of `pipefs`, a
  file system that is never mounted, as `sockfs` and `ptyfs` are; `fds[0]` is an open file
  of the reading end, `fds[1]` of the writing end, so `dup2` and `spawnfd` pass them on and
  an end closes with its last descriptor, in whichever process. A reader waits while the
  pipe is empty and a writing end is open anywhere and then takes what is there; once the
  writing end is closed it gets 0. A writer writes everything, waiting while the pipe is
  full; a write of up to `PIPE_BUF` (4 KB) waits until all of it fits and goes in whole, so
  two writers' blocks never mix. With the reading end closed a write answers `EPIPE` (the
  bytes written before it, when there were some). The ring is changed with interrupts off and
  the bytes copied with them on, each side marked busy while it copies, so a reader and a
  writer work at once and two readers never get the same bytes. Every wait is one a kill,
  Ctrl+C or a hangup cuts short, and every change calls `poll_wake()`: `POLLIN` when there is
  something to read or the writing end is gone (`POLLHUP` then), `POLLOUT` when `PIPE_BUF`
  bytes fit, `POLLERR` with it when nobody reads. `lseek()` on an end answers `ESPIPE`. At
  most `PIPE_MAX` (32) pipes exist at once (`ENFILE`), so no program can take the kernel's
  memory with them. The pipe goes with its second end.
* **A broken pipe.** libsurf's `write()` ends a program with status 141 (128 + 13, as
  SIGPIPE does elsewhere) when a write to a pipe answers `EPIPE` (it asks `fstat()`: only
  `pipefs` counts, since a connection answers `EPIPE` too once it is closed both ways), so
  `cat file | head -3` ends `cat` quietly; `broken_pipe_exit(0)` asks for the error instead.
  A kernel command whose output goes to a pipe whose reader has gone (`dmesg | head`) is no
  failure: the rest of its kept output is dropped (`kernel/syscall.c`).
* **`SYS_spawnx`** (47, `include/surfos/spawn.h`): `spawnfd` with what a shell needs
  besides: the child's descriptors 0-3 from the caller's (-1 the console for 0-2,
  `SPAWN_CLOSED` none), an environment of at most 64 strings in 4 KB, and `SPAWN_BG`: not in
  the foreground of its terminal even while the caller's output is paged, so Ctrl+C there
  does not reach a job started with `&`. `user_entry()` puts the environment's strings
  above argv's on the new stack and `argc, argv, envp` under them; libsurf's crt0 makes the
  third word `environ` (and `main`'s third argument), `env.c` has `getenv`, `setenv`,
  `unsetenv` and `putenv`, and `spawn()` and `spawnfd()` are now `spawnx()` with `environ`,
  so an environment reaches what a program starts. The job field must be 0 (job control
  comes later). A program takes 128 arguments in 8 KB (`SPAWN_MAX_ARGS`, `SPAWN_ARG_BYTES`;
  until now 32 in 2 KB), and so does a kernel command (`SYS_kcmd`), so that the names a
  wildcard gives fit (`rm *.tmp`). `env` (`user/bin/env.c`) prints the environment it gets,
  or runs a program with assignments put over it (`env -i` from an empty one).
* **surfsh's parts** now (`user/surfsh/`): `parse.c` (the scanner and the grammar), `unparse.c`
  (a tree back to text), `expand.c` (words to fields), `vars.c` (variables, the environment,
  the positional parameters), `arith.c` (`$(( ))`), `glob.c` (patterns and wildcards),
  `exec.c` (the tree runs), `run.c` (the kernel's commands, programs of `PATH`, waiting),
  `jobs.c` (jobs in the background), `util.c` (strings that grow, arenas), with `main.c`,
  `editor.c`, `complete.c` and `builtins.c` as before.
* **Parsing** (`parse.c`). A complete command is what the text holds up to a newline outside
  every command: a list of and-or lists (`;` `&`), and-or lists of pipelines (`&&` `||`,
  left to right), pipelines of commands (`|`, `!` in front), and commands: a simple command
  (assignments, words and redirections in any order), `{ list; }` or `( list )` with
  redirections after them. Words are kept as typed, quotes and all; the scanner reads quotes,
  `$( )` (whose inside it parses, so that its `)` is the one that ends a command), `${ }`,
  `$(( ))` and backquotes to their ends, joins lines at `\` and a newline, and takes `#` at
  the start of a word to the end of the line. Redirections are `[n]< > >> >| <> <& >& << <<-`;
  a here-document is the lines after the next newline up to its end line (`<<-` drops their
  leading tabs; a quoted end line keeps the text as it is). Text that ends inside something
  answers `PARSE_MORE` with what is open; at the prompt the next line is read after the
  continuation prompt `… ≋>` (`shell/prompt.c`, where ^C or ^D on an empty line drops the
  command), in a file it is an error. Commands nest 64 deep at most, a pipeline has 16
  stages. An empty command (`;;`, a leading `;`) is skipped, as before.
* **Running** (`exec.c`). Redirections are made in surfsh itself: the descriptor's file is
  kept on a spare descriptor (10-15, then 3-9) and put back afterwards, so builtins, kernel
  commands, groups and programs are redirected alike, and a program gets surfsh's 0, 1 and
  2 as they are then (`2>&1`, `3>&1 1>&2 2>&3`). A here-document goes in through a pipe:
  for a program it is written once the program has started, as it reads; for surfsh's own
  command it must be in the pipe first, so one longer than `PIPE_SIZE` is written by a new
  surfsh running `cat`. A pipeline's programs are processes joined by pipes; a stage that is
  surfsh's own (a builtin, a kernel command, a group, a command whose name comes from an
  expansion) runs in a new surfsh, except the last stage, which runs in surfsh itself with
  the last pipe as its descriptor 0, so that `... | cd x` (and later `... | read line` and a
  loop) change this shell, as zsh and ksh have it. A kernel command after a `|` is refused:
  it reads only the console. The pipeline's status is its last stage's, and every process of
  it is waited for. `( list )` runs in a new surfsh too, and so does a job in the background
  that is surfsh's own (`{ ...; } &`, `ping 10.0.2.2 100 &`, which before was refused).
* **Variables** (`vars.c`). A variable has a name (letters, digits and `_`, no digit
  first), a value, and two marks: exported (`export`: programs get it in their environment)
  and read-only (`readonly`: no assignment or `unset` changes it). surfsh starts with its
  environment as exported variables and sets `PATH` (`/initrd/bin`), `SHELL` and `PWD`
  when they are missing; `cd` sets `PWD` and `OLDPWD` (`cd -` goes to `OLDPWD`). A line of
  assignments alone (`x=1 y=$x`) keeps them, left to right; assignments before a command
  are for that command alone: a program finds them in its environment (over the exported
  ones), surfsh's own command sees them while it runs and they are put back afterwards. The
  positional parameters (`$1`, `${10}`, `$#`, `"$@"`) are a script's or `-c`'s arguments;
  `set -- a b` gives new ones and `shift [n]` moves them down. `set` alone lists the
  settings and then the variables, `export` and `readonly` alone the marked ones, as lines
  that would set them again.
* **Expansions** (`expand.c`), when a command runs, in POSIX's order: parameters, `$( )` and
  backquotes, and `$(( ))`, left to right; then field splitting of what unquoted expansions
  gave, at `IFS` (POSIX's rule: blanks around a field are none, one other `IFS` character
  ends a field even an empty one); then pathname expansion; then the quotes go. Each
  character carries a flag through this (quoted; from an unquoted expansion), so what quotes
  hold is never split nor a pattern, `"$x"` is one field whatever `x` holds and `"$@"` is a
  field for each parameter. The `${ }` forms are `${x:-w} ${x-w} ${x:=w} ${x:?w} ${x:+w}
  ${#x} ${x#p} ${x##p} ${x%p} ${x%%p}`; `$?`, `$$`, `$!` (the last job's last pid), `$#`,
  `$0` and `$-`. `$( )` runs a plain program by itself and anything else in a new surfsh,
  reads its output to the end and takes off the newlines at its end; a line of assignments
  alone has the status of its last `$( )`. `$(( ))` (`arith.c`) is C's integer arithmetic on
  32-bit numbers, assignments, `++` and `--` included; dividing by 0 is an error, and the
  side of `&&`, `||`, `?:` that is not taken is not worked out. An expansion that fails
  (`${x:?}`, a division by 0, a parameter that is not one) says why and the command does not
  run (status 1); a script ends there. A here-document whose end line is not quoted has
  parameters, `$( )` and `$(( ))` in it, its quotes being themselves. A file's name in a
  redirection and an assignment's value are expanded but never split nor matched.
* **Wildcards** (`glob.c`). `*`, `?`, `[abc]`, `[a-z]`, `[!a]` outside quotes make a field
  the sorted names that match it, part by part between the `/`s (a part without a pattern is
  only joined on); a character is a whole UTF-8 sequence, names are matched as stored, a name
  that starts with `.` only by a part that does, and a pattern that matches nothing stays as
  it is (POSIX's rule). The same matcher does `${x#p}` and the rest.
* **`PATH`** (`run.c`). A name without a `/` that is not surfsh's own is looked for in each
  directory of `PATH` (an empty one is `.`), and only then among the kernel's commands
  (10/25/26; they came first until then, section 17); `help` and Tab list the programs of
  all of them, a name in two directories once.
* **A new surfsh** is `surfsh --sub STATUS PID LASTPID NAME`, started with `spawnx` with the
  given descriptors 0 and 1, surfsh's 2, the exported variables as its environment, and the
  reading end of a pipe as 3, into which surfsh writes what makes the new one this shell
  (`var_state()`: the variables that are not exported, the read-only marks, the positional
  parameters; and the history, when the text uses it), a NUL, and the text to run
  (`unparse()` of the tree: one line, its here-documents after it), and closes it; the new
  one reads it to the end, runs it as a script with `$?`, `$$`, `$!` and `$0` as they were,
  and ends with its status. That is how SurfOS, which has no `fork()`, gets the copy of the
  shell a subshell is.
* **Jobs** (`jobs.c`). A command started with `&` (a program, a pipeline's processes, or a
  new surfsh) is a job with the smallest free number: `[1] started (pid 42)` (the last
  process's pid); before each prompt the processes that ended are collected and a job whose
  processes have all ended is reported with its first line: `[1] done  cmd`, `[1] exit
  status 3  cmd`, `[1] killed  cmd`. A shell that runs a script says nothing about its jobs
  (10/25/26: it keeps one that ended for `wait`).
* **Input** (`main.c`). `surfsh -c TEXT` and `surfsh FILE` run without a prompt; input that is
  not the console is read a byte at a time from a pipe and a block at a time from a file,
  which is put back where the next command's own reading begins, so a command reads what
  follows it. A mistake in a file, or a file that ends inside a command, ends it with 2.
* **Compound commands and functions** (`parse.c`, `exec.c`, 10/25/26). `if`/`elif`/`else`,
  `while`, `until`, `for NAME [in WORDS]` (`"$@"` without `in`) and `case` (its patterns are
  `glob.c`'s, quoted characters themselves), each with redirections after it; `if then else
  elif fi do done case esac while until for { } !` are reserved where a command's name would
  be, and a list inside a compound command goes on over newlines to the word that ends it (at
  the prompt, the continuation prompt). A function, `NAME() COMPOUND`, is kept by `vars.c` as
  its definition's text (`unparse.c`), parsed again into memory of its own so that it
  outlives the line that defined it, and handed to a new surfsh with the variables; one that
  is replaced while it runs goes when its last call ends. It is called before surfsh's other
  builtins and after POSIX's special ones (`: . break continue eval exec exit export readonly
  return set shift unset`), with its own positional parameters, `local` variables put back
  when it returns, and loops of its own for `break`; calls nest 100 deep at most. `break N`,
  `continue N` and `return N` are counters that every list on the way out looks at
  (`stopping()`), and the loop or function they are for takes them.
* **The builtins of scripts** (`script.c`, `test.c`, `printf.c`). `test` and `[` (POSIX's rules
  by the number of arguments, then `! -a -o ( )`; SurfOS's files have no permissions, so `-r`
  and `-w` are "it exists"); `read [-r]` (one line and no more: from a terminal one read,
  from a file a block and `lseek()` back, from a pipe a byte at a time; split at `IFS`, the
  rest to the last name); `printf` (POSIX's conversions and `\` sequences on libsurf's
  formatter, the format used again for the arguments left); `eval`; `.` and `source` (`PATH`'s
  directories, then here; `return` leaves the file); `exec` (alone, its redirections stay;
  with a command, the shell ends with its status: SurfOS has no `exec()`); `kernel NAME` (the
  kernel's command where surfsh has one of its own, `kernel test`); `:`, `true`, `false`.
  `set` takes POSIX's options: `-e` (a failure whose status is not looked at, outside `if`
  and loop conditions, `&&`, `||` and `!`, ends the shell), `-u` (a parameter that is not set
  is an error), `-x` (each simple command on 2 after `+`), `-f` (no wildcards); `$-`; a new
  surfsh gets them.
* **Scripts.** `surfsh FILE ARGS`, or a file with `#!INTERPRETER [ARG]` on its first line run
  by its name: `process_spawn_ex()` reads the line in the caller's context and starts
  `INTERPRETER [ARG] FILE ARGS` (an ELF program; no `#!` behind a `#!`), the task named after
  the script. A file that is neither ELF nor `#!` is refused with `-ENOEXEC` before anything
  starts (until now the new task failed to load it), and surfsh then runs it as a surfsh
  script, as POSIX's shells do; a `#!` line that names something that cannot run is said
  ("its #! line names /bin/sh, which cannot run"). init words a surfsh that is no program as
  before.
* **Ctrl+C in loops and scripts** (`kernel/task.c`, `kernel/tty.c`). Whom a Ctrl+C reaches is
  unchanged: the innermost programs of the job of the pager's owner are killed, a kernel
  command is cut short. New: `SYS_waitx` (48, `include/surfos/job.h`) says that a child was
  killed by Ctrl+C (`WX_INTERRUPTED`: it ended with `TI_INT`), not just killed; every task of
  the job counts the Ctrl+C (`TTY_INTR` answers the count), except those above a shell that
  looks for Ctrl+C itself, so a script that runs a script stops as a whole; and a shell that
  asked (`TTY_INTR 1`, every interactive surfsh) is told instead of killed where it would be
  (a surfsh typed at a surfsh's prompt), and as the owner, when nothing else of its job was
  reached, it takes the key and has its own wait in a call cut short (`read` at the
  terminal). surfsh asks after each command: a script ends with 130, an interactive shell
  leaves the rest of its line, loop or function and is back at its prompt with `$?` 130,
  saying `^C` when nothing else did. A program Ctrl+C killed still reports `[pid n killed]`
  at the prompt, now with status 130.
* **Job control** (stage 5; `include/surfos/job.h`, `kernel/task.c`, `kernel/tty.c`,
  `user/surfsh/jobs.c`). A job is the tasks of one command an interactive surfsh runs: its id
  is the pid of its first process (`SYS_spawnx`'s job: `SPAWN_JOB_NEW`, then the job's id for
  the others; `SPAWN_JOB_NONE` for what surfsh starts outside a job, a `$( )` or a pipeline
  that ends in the shell), what they start on the same terminal is in it too (a pty's session
  is in none, and so is what a kernel command starts: `run` waits for its program and knows of
  no stop), and the shell that made it (`job_shell`) is the only one that may move it. Each
  terminal has a foreground job (`fg_job`), which surfsh sets with `SYS_job` (49, `JOB_FG`)
  while it waits for one; a task of a job in the background may not (`-EACCES`), but the
  foreground job's shell (`fg_shell`) gives the terminal back to its own job (a surfsh typed at
  a surfsh's prompt), and when that shell ends the foreground is its own job, for the shell
  above it to take back. Nor may a task in no job (10/26/26, CODE-AUDIT T14) unless it is a
  shell of that terminal's: the foreground job's, the session's (init's surfsh on a console,
  `TF_SHELL`; on a pty the program its session began with, `TF_SESSION` from its creation
  until a `detach()`), or a program the ring-0 shell started (a surfsh typed at it). An
  interactive surfsh in no job takes its terminal as it starts (`JOB_FG` 0), and again before
  each command (a nested surfsh whose shell above ended is in no job from then on); one the
  kernel refuses (a surfsh that `run` started) runs without job control: what it starts is in
  no job, as a script's is, and `fg` and `bg` say there is no job control. Ctrl+Z in line mode stops the foreground job's tasks (at `--More--`
  it is a key; over the serial line and a pty it passes a paste's skip and an escape
  sequence, as Ctrl+C does), and a task of another job that reads the terminal stops instead
  (`task_stop_reader()`: as `tty_read()` begins, before it looks at the mode, and in
  `tty_getc()`). A stopped task waits where it was: in its own code it is taken off its run
  queue at once (`TS_STOPPED`, and `irq_dispatch()` switches away from it); waiting for a key
  or in a sleep that a kill would cut short (`sleep_intr()`) it is woken and parks there, off
  the terminal's queue of readers, so the keys typed meanwhile are its shell's, and then
  reads or sleeps on to the same end; in another wait that a kill would cut short it counts
  as stopped and the wait goes on; anywhere else in a call it parks at the call's end
  (`task_call_leave()`) and counts as stopped as soon as it waits again
  (`wait_prepare_intr()`), so nothing is cut short or lost and its shell always learns of
  the stop. A kill's mark (`TI_KILL`) says how a task ended: a program's own `exit(-1)` is no
  kill. It leaves the foreground
  (Ctrl+C no longer reaches it); `JOB_FG` and `JOB_BG` let it go on, in the foreground or
  not, and a kill reaches a stopped task too. `SYS_waitx` with `WUNTRACED` says each stop
  once (`WX_STOPPED`, why: Ctrl+Z or a read from the background), and `pid < -1` waits for any
  child of a job. A shell that looks for Ctrl+C itself (`TF_INTR`) is never stopped, so
  surfsh's own commands (builtins, loops, kernel commands) do not stop. When the shell that
  made a job ends, the job's stopped tasks are killed and its other tasks leave it; a
  detached task is in no job. surfsh: the job table (`jobs`, `fg`, `bg`, `wait`, `kill %n`),
  `[1] stopped  cmd` when one stops in the foreground (status 148, and the rest of the line,
  loop or function is left, as for Ctrl+C), `[1] stopped (reading the terminal)` before a
  prompt; `ps` and `SYS_procinfo` say `stopped` (`PROC_STOPPED`). In line mode Ctrl+D on an
  empty line is the end of the input: `read(2)` answers 0 (`cat > file`, `read`).
* **What a review found** (10/25/26). Descriptors 10-15 are surfsh's own: the copies that
  redirections keep, a script's own file (so `exec 3< file` in a script no longer takes the
  script's place) and, in an interactive shell, a copy of its terminal (`term_fd`), on which
  it draws its prompt and line and writes its reports (`[exit status n]`, `[1] started`,
  `[1] stopped`, `[1] done`): `{ ...; } > file` and `exec > file` take only what the commands
  write. `<&` and `>&` take 0-9; a program gets a descriptor 3 that is the user's (`prog 3>
  file`, `exec 3> file`; for 4-9 surfsh says they do not reach it). surfsh never writes a
  here-document while what reads it cannot go on: a feeder is waited for once surfsh has
  closed its end of the pipe (so a command that reads part of a long document, `: <<'END'`
  or `read`, no longer hangs), and the document of a stage whose output another stage reads,
  or of a job in the background, is written by a new surfsh that runs that stage. A pipeline
  whose last command cannot start has its 126 or 127. Ctrl+Z leaves an `until` and a `for`
  loop as it leaves a `while`, and a program's own exit status of 148 is no stop (the stop is
  said apart). `eval`'s and `.`'s text runs as the command it is part of: `$?` there is the
  status before it, and in a condition `set -e` is ignored there, as it is in a subshell or a
  pipeline's stage in a condition (`cond_flag`; the new surfsh gets the options without
  `-e`); a text in which no command runs answers 0. `exit` alone leaves with `$?` (POSIX). A
  script keeps a job that ended until `wait` asks for it (`sleep 1 & p=$!; ...; wait $p`);
  `jobs` collects only the table's processes (in `true | jobs` the pipeline's first stage is
  not taken from its wait), and in a pipeline or `$( )` it shows the table of the shell (a new
  surfsh is given it as `jobs -s` lines). `exec` of a command that cannot start leaves an
  interactive shell running. A surfsh that reads a terminal that is not its prompt (`surfsh >
  log`) reads it a line at a time. Limits: `${ }` and `$(( ))` nest 64 deep (in the parser's
  scanner and in `braced()`), a `${#name}` longer than any name is an error, `printf`'s
  widths and precisions are at most 65536, a string that grows stops at what an `int`
  counts, and `tail -n` grows its ring with its input. The value of `${x#pattern}` is taken
  before the pattern expands (which may change `x`), `x=1 x=2 prog` gives the program `x=2`
  alone, and an empty quote (`""`, `''`, `"$empty"`) begins a field where it is (`""$x` of
  `x=" a"` is two fields, as in other shells).
* **Filters** (stage 6, `user/bin/`): `grep` (a basic regular expression: `. * ^ $ [...]`
  and `\`; `-i -v -n -c -l -q -F -e`), `sort` (`-r -n -f -u`), `uniq` (`-c -d -u -i`), `tail`
  (`-n N`), `tee` (`-a`), `tr` (ranges, `-d`, `-s`) and `seq`, the programs a pipeline wants
  after `cat`, `head` and `wc`; `lineutil.h` reads their lines of any length.
* **Tests.** `make unittest` adds `test_surfsh` (`parse.c`, `unparse.c`, `expand.c`, `vars.c`,
  `arith.c`, `glob.c` and `util.c` as they are on the host, `tests/host/surfsh_bridge.c`:
  lists, pipelines, groups, redirections and here-documents parsed and written back,
  `PARSE_MORE` for what is open, the mistakes, the limits, quote removal; variables, field
  splitting, `"$@"`, every `${ }` form, arithmetic, `$( )` against a stand-in, wildcards over
  a tree of names on `tests/host/sim_fs.c`; compound commands and functions parsed and written
  back, their mistakes; `test` and `printf`) and `test_prompt` the continuation prompt. `make
  test` adds `pipetest` (`user/bin/pipetest.c`: pipes from a program and its children,
  `spawnx` and `waitx`), a block of pipelines and lists in a nested surfsh, one of variables,
  the environment (`env`), expansions and wildcards, one of scripts (compound commands,
  functions, the builtins of scripts, scripts by name, `set -e -u -x`, `exec`), and
  `@surfsh-ctrl-c`: Ctrl+C in loops of builtins, of programs and of kernel commands, in
  `read`, in a surfsh inside a surfsh, and in a script; and `@surfsh-jobs`: Ctrl+Z, `jobs`,
  `ps`, `bg`, `fg`, `kill %1`, a pipeline as one job, a loop of builtins Ctrl+Z leaves alone, a
  reader in the background stopped and given the keys by `fg`, Ctrl+D, the stopped job of a
  surfsh that exits killed, a reader stopped while it waits for a key (the shell's next line
  is the shell's), a key in the same burst as the Ctrl+Z, two surfshs inside each other (the
  middle one keeps the terminal after the inner one's end), an `until` loop, and a program
  that `run` starts in a surfsh inside a surfsh, which Ctrl+Z leaves alone; a block of what a
  review found (a script's descriptor 3, a pipeline's 127, `$?` in `eval`, `set -e` in
  conditions, a script's `wait`, `exit` alone, a redirected group, `printf`'s widths, `jobs`
  in a pipeline and `$( )`, `x=1 x=2`, `exec` of a typo, long here-documents into `:`,
  `read` and a pipeline's stage, `surfsh > log`, `tail -n` of a huge count); and a block of
  the filters in pipelines. `test_surfsh` adds the nesting limits, a long `${#name}`,
  `printf`'s widths, empty quotes and a pattern that changes its variable.

## 22. Files to and from other computers: ftpd, nc and fetch (October 2026)

The owner asked (2026-10-24) for networking applications to transfer files remotely, some
sort of file transfer protocol, so that files go back and forth with other computers such
as their Mac, and then other tools like it (`docs/USB-BOOT.md` section 17; the brief is
`docs/superpowers/plans/2026-10-24-nettools.md`). Two programs on the sockets of section 18,
and what they needed of them:

```
ftpd (user/ftpd/)      ring 3: an FTP server. main.c the commands, server.c one poll() loop, ftp.c the protocol
nc (user/bin/nc.c)     ring 3: connections by hand, files to and from the Mac's nc
fetch (user/fetch/)    ring 3: HTTP and HTTPS downloads on Undertow's transport; fetchhttp.c reads the responses
sockets (net/sock.c)   SYS_shutdown (the half-close), SYS_sockaddr (both ends), SYS_connect's CONNECT_NOWAIT
TCP (net/tcp.c)        tcp_shutdown(), tcp_addrs(), tcp_open_nowait(); TIME_WAIT keeps what a program has not read
the network manager    a running ftpd keeps the Wi-Fi keeper off open networks, as telnetd does (section 19)
```

* **The sockets' new calls** (`include/surfos/sock.h`). `SYS_shutdown` (50, `shutdown(fd,
  SHUT_WR)`) is the half-close: the FIN goes out behind the bytes queued, as a close's does,
  but the descriptor stays open and `read()` goes on with what the other end still sends,
  until its FIN; `write()` says EPIPE afterwards and `poll()` no longer says POLLOUT. The
  socket is no orphan, so FIN_WAIT_1 and FIN_WAIT_2 take its data as ESTABLISHED does, and
  one that reaches TIME_WAIT with bytes unread keeps its receive buffer (TIME_WAIT gave it
  back at once, which only orphans and the kernel's own closes ever reached before);
  `tcp_shutdown()` never hands a failed connection to `start_close()`, which would free it
  under the program's descriptor. A half-closed connection the program still holds is probed
  by keepalive in FIN_WAIT_2 as an open one is, so a read waiting there for the other end
  ends (ETIMEDOUT) when that end has gone; only orphans had a bound there before. A shutdown
  during the handshake (`CONNECT_NOWAIT`) is ENOTCONN, as POSIX has it: no FIN could follow
  it then. `SYS_sockaddr` (51, `tcp_addrs()`) gives a socket's two
  ends, this one's being the address the other end reached (a listener's: 0, every
  interface). `SYS_connect`'s third argument, `CONNECT_NOWAIT` (`tcp_connect_start()`),
  hands the descriptor out at once with the SYN on its way: `poll()` says POLLOUT once the
  handshake is done and POLLERR when it failed, `read()` then says why (refused, the SYN's
  tries run out after about 27 s, ARP gave up), and a read or write before the end of the
  handshake waits for it (no program held a SYN_SENT socket before; `tcp_write()` would
  have said EPIPE). libsurf passes 0, so every program that connects is as it was. ENOTSOCK
  (88) is new in both errno tables.
* **ftpd's commands** (`main.c`), laid out as httpd's: `ftpd on [-r] [-a address] [folder]
  [port] [first-last]` (the folder `/` by default: every mounted volume at its own path;
  port 21; passive ports 50000-50999), `ftpd serve` the same in the foreground, each command
  printed with its reply's code, `ftpd off`, `ftpd [status]`. The words come in any order,
  told apart by their shape. The running server is a detached user task named `ftpd`; `on`
  takes the port itself and starts the server with `spawnfd()` and the internal word
  `--server`, the listening socket as its descriptor 0. A folder under `/dev` or `/.netman`
  is refused, and a control port inside the passive range.
* **The server** (`server.c`): one `poll()` loop over the listener and up to six sessions. A
  session's control connection is read into a buffer and cut into lines by `ftp.c` (Telnet's
  IAC sequences taken out: Python's ftplib sends ABOR as urgent data, which comes in line
  here, and other clients put IP and Synch before it); one command runs at a time and its
  replies go into a buffer written on POLLOUT. Commands wait in the buffer and in the kernel
  while 16 KB of replies wait for a client that does not read them, during a transfer too.
  During a transfer the control connection is still read: ABOR (426 for the transfer, then
  226) and STAT (213, how far it is) are answered at once, and up to four other commands are
  held, to run in their order once the transfer has ended (their replies come after ABOR's
  and STAT's); more wait unread. A client that closes its side of the control connection (as
  `nc` does when its input ends) has what it sent run and answered, then the session closes;
  while it takes nothing, its connection rests 20 ms between polls instead of spinning on
  POLLHUP, as httpd's do. A data connection is prepared by PASV or EPSV (a listener with a
  backlog of 1 on the next free port of the range; the connection is accepted at once if it
  comes from the client's own address, another is closed and the listener waits on) or by
  PORT or EPRT (the client's own address only, ports from 1024: no FTP bounce), whose
  connection out is made with `CONNECT_NOWAIT` and polled for, so a client whose firewall
  drops it holds up no other session. A transfer opens its file and answers 150 once its
  data connection is there (taken, begun for active mode, or there before the command), as
  vsftpd and ProFTPD do: one waiting for its passive connection holds the listener alone, so
  `accept()` always finds a descriptor for it (one got its 150 and then none: 425), and an
  upload whose connection never comes leaves its file as it was. It moves the bytes a piece
  of 8 KB at a time as far as the connection takes them, closes the data connection and
  answers 226. Listings are made in memory first (8 MB at most), so no folder stays open
  while a client reads one. Times: 30 s for a data connection to come or be made (425), 60 s
  for a transfer that moves nothing (426), 60 s for replies nobody takes, 5 minutes without
  a command (421). Six sessions fit the 16 descriptors (0, 1, 2 and the kernel log's leave
  twelve: a session holds its control connection, and a data connection and a file while it
  moves one): three sessions move files at once. A seventh connection is told 421, and a
  transfer that finds no descriptor is told to try again (425) instead of its 150. `ftpd
  serve` resets the pager before each line it prints (`ttyctl(TTY_PAGE, 0)`): the user shell
  pages every command, and a server held at `--More--` would answer nobody until a key came.
* **The protocol** (`ftp.c`, no system calls, host-tested): the control stream, the verb table
  (with the X forms of Windows' ftp, and the verbs known but not done: AUTH and the rest of
  RFC 2228, MDTM, MFMT, the hashes, all 502), a session's paths (its own `/` the folder
  served, `..` held there) and the file system's, the paths never reached (`/dev`, `/.netman`),
  the texts of PORT, EPRT, PASV and EPSV, REST's offsets, LIST's options, the lines of LIST
  (`ls -l`'s, the year for a time six months back or an hour ahead), NLST, MLSD and MLST,
  text mode's line ends both ways in pieces, quoting a path for 257, errno's replies.
* **What a client sees.** `RETR` reads a file, `STOR` writes it in place (with REST, cut at
  the offset and written from it, so a resumed upload ends as the file sent; there is no
  temporary name renamed over it, which on FAT would need the rename of a file over another,
  section 10), `APPE` appends, `STOU` makes `name.1` and so on; an upload is fsync()ed before
  its 226. `DELE`, `MKD`, `RMD` (whose refusal of a folder that holds only names starting with
  a dot says so), `RNFR` and `RNTO` (as `mv`); `SIZE`; `CWD`, `CDUP`, `PWD`. LIST and NLST leave
  names starting with a dot out unless asked (`-a`), as `ls` does; MLSD lists them, for
  programs that empty a folder. LIST gives SurfOS's clock as it is, local time as `ls -l`
  does; MDTM and MLSD's `modify` would have to be UTC, which the clock does not know, so they
  are not offered, and MLST is not in FEAT (GUI clients then list with LIST). Text mode (TYPE
  A) turns LF into CR LF going out and CR LF into LF coming in. `-a` names the address PASV
  replies give (a server behind a router, or QEMU's user network in the tests); by default
  it is the address the client reached (`tcp_addrs()`), right on a laptop with Ethernet and
  Wi-Fi on two networks. `-r` refuses every write with 550; a volume mounted read-only, the
  initrd and the root of the mount points refuse them by themselves, and LIST and MLSD show
  their permissions so.
* **nc** (`user/bin/nc.c`): `nc host port` connects, `nc -l port` takes one connection, `nc
  -z host port...` says which ports are open (ranges too, 1000 ports at most). One `poll()`
  loop: what is typed (or a file given with `<`) goes out, what comes back is written to
  stdout byte for byte. Each way ends by itself: the input's end is sent as the half-close,
  and the other end's FIN says it has sent all it will; `nc` ends when both have, or at that
  FIN when the input is typed (the console has no end of its own). That is what the Mac's
  `nc` does, so either one sends a file to the other, also to a Mac `nc -l` whose own input
  is at its end and half-closes at once (a smoke check: the first `nc` ended at that FIN and
  lost the rest of its file, which the Mac's real `nc` showed). After the FIN the socket is
  left out of `poll()` while nothing waits to go, and rests 20 ms when what waits does not
  fit. `-c` sends CR LF for each line typed, `-w` ends a connection silent that long
  (counted from the last byte in or out whatever `poll()` says, so a rest after the other
  end's FIN neither holds it off nor spins once it is up), `-v` says both ends
  (`tcp_addrs()`) and how it ended. The console's line mode reads a line at its Enter, so
  what arrives while one is being typed shows after it, and there is no key for the end of
  the input (Ctrl+C ends `nc`).
* **fetch** (`user/fetch/`, 10/25/26): `fetch [-o file|folder|-] [-c] [-f] [-q] [-t utc]
  address` downloads an `http://` or `https://` address into a file named after its path
  (`index.html` for a folder), in the current directory or the folder `-o` names (`-o -`
  standard output, `-o /dev/null` nothing). The connection is Undertow's: `transport.c` with
  `url.c`, `config.c`, `util.c` and the TLS glue, linked as they are (the transport gained
  one accessor, `ut_transport_eof_clean()`), so HTTPS has the browser's roots, checks and
  messages, and no way around them; `-t` gives the UTC time for one run as the browser's
  `clock` does. The response is read as it comes (`fetchhttp.c`, no system calls,
  host-tested), its head as strictly as Undertow's `http.c` reads one (the same refusals:
  obsolete folding, a CR inside a line, two lengths, a length with Transfer-Encoding, a
  coding other than chunked), the chunked coding undone a piece at a time and the body
  written 16 KB at a time, so a download is as large as a file may be (2 GB) and not as
  memory. Nothing is written before a 2xx response that has something in it (a 204 or 205
  changes no file); a file there already is kept unless `-f`, and one that appears while a
  new download is asked for is not replaced (`O_EXCL`). `-c` asks for the rest (`Range:
  bytes=N-`): a 206 must start at the file's end (which must not have moved meanwhile) and be
  as long as its range, which it may not pass; one that ends before the file does is added
  and fetch says `fetch -c` goes on; a 200 leaves the file as it was, as curl and wget
  refuse it (a network's login page never takes the place of half a download), and `-f`
  downloads it anew; 416 with the same total says the file is whole. A download cut short
  keeps what came and says `fetch -c` goes on with it. Over HTTPS a body that only the end
  of the connection delimits is whole only after TLS's close_notify (RFC 9112 section 9.8:
  anyone on the way can end a TCP connection); without it fetch keeps what came and says it
  may be cut short. A download may take 20 days. Redirects are the browser's five codes, ten
  at most, never in a loop or from HTTPS to HTTP; the file keeps the first address's name (a
  server's Content-Disposition names nothing on SurfOS). On a terminal a progress line is
  written over; the end says the size, the time, the rate, the TLS version and the file.
* **The network manager.** `netman_ftp_exposed()` is true while any user task named `ftpd`
  lives; the keeper then joins no open network by itself (`+NET: wlan0: SurfSim is in range,
  but it is open and an FTP server (ftpd) runs: not joined by itself`), `wifi join` of an open
  network says the server answers there too, and `wifi save` of one says SurfOS will not join
  it by itself meanwhile.
* **Tests.** `make unittest` adds `test_ftp` (`ftp.c` as it is, 131 checks: the control
  stream in every piece size and at random with Telnet commands in it, every path and
  address rule, the listing lines and their dates, text mode both ways at random). `make
  test` adds the commands' refusals and `@ftpd-session` (a login, FEAT, folders, every
  listing, the initrd's files byte for byte, EPSV), `@ftpd-upload` (uploads to the FAT disk
  read back, a shorter file over a longer one, APPE, REST both ways, STOU, a name with a
  space, renames, folders, hidden files), `@ftpd-text`, `@ftpd-active` (PORT and EPRT both
  ways, a refused connection, no bounce, IPv6 told, EPSV ALL), `@ftpd-abort` (ABOR, STAT and
  a NOOP during a 2.5 MB transfer read slowly; NOOP, STAT and ABOR together: the NOOP held,
  the others answered at once), `@ftpd-descriptors` (six sessions: three transfers at once
  whose clients send RETR before they connect, then three that connect first, a fourth told
  to try again each time), `@ftpd-protocol` (Telnet sequences, commands together, a NUL, a
  line of 600 bytes, every refusal), `@ftpd-places` (six sessions and a seventh refused; two
  half-closed clients answered whole), `@ftpd-data-time` (425 after 30 s), `@ftpd-readonly`
  and `@ftpd-serve` (sixty commands printed without `--More--`); `nettest limits` checks the
  new calls' errors; `nc`'s refusals, `@nc-receive` and `@nc-send` (200000 random bytes each
  way with this host playing the Mac's `nc`, and a receiver that half-closes first),
  `@nc-talk` and `@nc-scan`; `selftest` refuses a folder renamed into itself under another
  case of its name (audit R1), and so does `@ftpd-upload` over FTP; and the network
  manager's second settings boot runs `ftpd on` and sees `wifi connect` set the open network
  aside. The smoke test forwards guest port 21 and four passive ports, each to the same port
  of the guest, as EPSV needs (`q.ports['ftp']`, `q.ports['ftp_pasv_range']`). `make
  test-tcp` adds `halfclose` (the program's FIN, 12000 bytes after it read as its window
  allows, read() 0, TIME_WAIT; a program that reads late, in TIME_WAIT, gets what came),
  `nowait` (a SYN answered 2.5 s late while the program polls; a reset) `finwait` (a
  half-closed connection whose other end vanished: keepalive gives it up) and `ncstuck` (`nc
  -w` ends a connection whose other end half-closed and takes nothing; through QEMU's user
  network this host's buffers take the whole file under Linux, so it is no smoke check of
  its own). `make unittest` adds `test_fetch` too (`fetchhttp.c` as it is: the head and its
  refusals, the chunked coding at random in every piece size, the names, the sizes), and
  `make test` fetch's steps: this host's server through TestHandler's `/fetch/` paths (a
  file, one there already, one cut short and gone on with by a Range request, a server that
  ignores Range (refused, then `-f`), the rest in two parts, a part longer than its range, a
  204, chunked, the end of the connection, a redirect, a loop, a 404, a name with spaces, a
  folder, /dev/null, the read-only initrd, `-o ''`), the files read back through httpd.
  `make test-web` adds the fetch scenarios: HTTPS against the fixtures (TLS 1.3, 1.2,
  ECDSA), their framing, every refusal the browser makes and a close-delimited HTTPS body
  ended without close_notify (the fixtures' `/frame/close-cut`; over HTTP it is whole), the
  production roots, no RDRAND and a clock in 2004 (`-t`). `make test-usb`'s `usbnet`
  scenario logs in over the USB Ethernet adapter, lists the initrd, takes a file and is
  refused an upload. On the Mac
  against SurfOS in QEMU: curl (list, download, upload, delete, a download and an upload
  resumed), Python's ftplib, the Mac's own `nc` both ways (300000 random bytes each, byte
  for byte), and Finder's own FTP (`mount_ftp`, Go > Connect to Server), which mounted the
  server, listed it and copied a file out byte for byte (it tried PORT first and fell back
  to PASV when PORT was refused behind QEMU's NAT; through QEMU it needs `-a 127.0.0.1`).
* **Limits.** No logins or encryption (no FTPS, no SFTP) and no time zone (no MDTM). One
  background server, six sessions, files below 2 GB (SurfOS's file offsets). Text mode
  converts line ends only. `nc` speaks TCP alone (no UDP), one connection at a time.

## 23. A name on the local network: mdns (October 2026)

The owner's Mac reaches SurfOS by an address it has to look up first (`ifconfig` on the laptop).
`mdns on` makes the laptop `surfos.local` (multicast DNS, RFC 6762, which the Mac's Bonjour asks
by itself): `ftp://surfos.local/`, `http://surfos.local/`, `telnet surfos.local`, `ping
surfos.local`. It never starts by itself; `mdns off` stops it.

```
the responder (net/mdns.c)      a kernel thread, UDP port 5353: probes, announces, answers, per interface
the messages (net/mdns_wire.c)  queries read, answers, probes and announcements made: pure, host-tested
IP (net/ip.c, net/arp.c)        224.0.0.251 taken on any interface; multicast out to its MAC, IP TTL 255
UDP (net/udp.c)                 a datagram's interface and destination kept (udp_recvfrom_dev()), one sent
                                through a given interface (udp_sendto_dev()), a socket's IP TTL (udp_set_ttl())
the NICs                        NETDEV_ADD_MULTICAST: the e1000's MTA bit; the USB adapter all multicast, once asked
```

* **The name** is the DHCP hostname (`hostname` in `surfos-config/settings.conf`, surfos by
  default) or the one `mdns on <name>` gives: 1 to 63 letters, digits and hyphens, one label
  before `.local`.
* **Each interface** with an address and a link is followed: the thread asks its NIC for the
  group's MAC (`NETDEV_ADD_MULTICAST`; a device without the call, wlan0, passes multicast as it
  comes), probes for the name three times 250 ms apart, the first after a random 0 to 250 ms (a
  query for ANY with QU and our record in the authority section, RFC 6762 section 8.1), and then
  announces it twice a second apart (8.3). An answer to a probe from another host is a conflict:
  the thread takes `name-2` (up to `name-99`) and probes again on every interface; one after the
  announcements sends every interface back to probing (section 9). Another host probing for the
  name at the same moment is a tie (8.2), settled by the records in the probes' authority
  sections (class, type, then data; the longer list wins a draw): when its are the later, the
  thread probes again a second later, and that host, which has the name by then, answers; when
  ours are, it goes on. Fifteen conflicts in a row, each within 10 s of the last, make every
  probe after them wait 5 s (8.1), so a host at fault, ours or another, floods nobody. A new
  address on an interface probes again for it. `mdns off` says goodbye where the name was
  announced (the record with a TTL of 0, 10.1): the Mac forgets it at once, not 120 s later.
* **Answers.** A query for `<name>.local` in any case, through compression pointers, is answered
  from the interface it came in on with that interface's address. A or ANY gets the A record;
  another type (AAAA: SurfOS has no IPv6) gets an NSEC record saying A is the name's only type
  (6.1), and the NSEC record goes with every A answer too (the additional section), so a Mac that
  asks for both learns at once that no IPv6 address will come. To the group (the cache-flush bit,
  a TTL of 120 s, once a second at most per interface; an answer to another host's probe for the
  name 250 ms after the last, as section 6 allows, so the name is defended at once); to the
  querier alone when it asked so (QU) or sent the query to our own address (5.5); and to a
  one-shot querier (a port other than 5353, 6.7) as a plain DNS answer: its ID, RD and questions
  as they were written, a TTL of 10 s, no NSEC (a "no such record" for AAAA). Answers to one
  querier go with an IP TTL of 255 as well (section 11, for queriers that check it). A query that
  already holds our record with half its TTL left gets nothing (known-answer suppression, 7.1).
* **What is looked at** (sections 5.5, 6, 11 and 18): messages from the local link only, that is
  to the group (no router passes it on) or to our own address from the interface's subnet; with
  opcode and response code 0; a response (a conflict) only from port 5353, and while probing only
  once a probe for the name has gone out (during the random wait, a tie's second or a hold, the
  probes to come hear whoever has the name, and a forged answer cannot run through the names);
  never one from our own addresses: with Ethernet and Wi-Fi on one network each interface hears
  the other's announcements, which are no conflict.
* **IP.** A datagram to 224.0.0.251 is taken on any interface (no other group is), and only the
  responder listens on its port; TCP never takes a multicast destination. Multicast goes out on
  the interface the caller names, to `01:00:5e` and the address's low 23 bits (no ARP), with an IP
  TTL of 255 as section 11 asks; a socket may ask for another TTL (`udp_set_ttl()`,
  `ip_output_ttl()`), which the responder does for its unicast answers.
* **The NICs.** The e1000's multicast table array gets the group's bit (bits 47:36 of the MAC,
  `RCTL.MO` 0); QEMU's model filters by it, so the raw-link test sees the driver's change work.
  The USB Ethernet adapter's packet filter takes all multicast once the responder asks: the usb
  thread makes the CDC request (endpoint 0 is its) within a second, and again whenever the adapter
  is plugged in, so without `mdns on` the adapter passes what it always did; IP drops what is not
  the group. The laptop's Intel Wi-Fi passes every multicast frame from its access point already.
* **Tests.** `make unittest` adds `test_mdns` (`mdns_wire.c` as it is: every kind of query, the
  answers' bytes and their NSEC record, one-shot answers and their questions, known answers,
  opcodes and response codes, conflicts, ties, goodbyes, broken messages, the names). `make
  test-tcp` adds `mdns` on the raw link: the probes and announcements and their timing and TTL,
  answers to the group (with the NSEC record), the second's limit and a probe answered within it,
  answers to a QU querier (IP TTL 255), to a query sent to our address and none to one from off
  the link, to one-shot queriers (the question as written), the NSEC record for AAAA, nothing for
  other names, a goodbye at `mdns off`; a conflict while probing (surfos-2.local), the same claim
  from another port or from off the link changing nothing; nothing after `mdns off`; a tie lost
  (probing again a second later) and one won; a host that claims every name (the probe after the
  fifteenth conflict 5 s late). Queries go to the group's MAC, through the e1000's filter (also a
  smoke step, `@tcp-raw mdns`). Over QEMU's user network `make test` only checks the command's
  refusals: libslirp sends a guest's UDP on through a socket of the host's, so a responder
  started there would announce its name onto the host's own network. The Mac's own resolver
  against the laptop is the owner's check.

## 24. Windows for programs (October 2026)

The owner chose (2026-10-25) a platform for new applications: an ordinary ring-3 program opens a window on
the desktop, draws in it and takes its keys and pointer (milestone M12; the brief is
`docs/superpowers/plans/2026-10-25-app-windows.md`). Until then only kernel code drew, the 13 applications
inside `gui`; this reverses `docs/GRAPHICS.md`'s "no new user-memory mapping or system call" and the desktop
spec's "not a new userspace graphics ABI". The layers, from the top:

```
a program (ring 3)   draws in its own memory, with libgui (gui.h: the desktop's drawing kit built into libsurf) or
                     bare: SYS_winopen; SYS_winctl (WIN_PRESENT, WIN_SETSIZE, WIN_TITLE, WIN_INFO); read(), poll()
kernel/win.c         the window: a descriptor (a vnode of winfs), its buffer, its event queue; a desktop's registration
shell/guisys_win.c   the desktop's calls into it (struct gui_sys's win_* calls), and the launcher (run)
shell/guiclient.c    the desktop's model: programs' windows in GUI_CLIENTS slots after the fixed ones
```

* **The calls** (`include/surfos/win.h`; numbers 52 and 53). `SYS_winopen (const struct win_open *)` gives a
  descriptor: a version, the content's size and its limits, WIN_RESIZABLE, a background colour and a UTF-8
  title of 63 bytes at most, cut between characters. `SYS_winctl (fd, op, arg)`: WIN_PRESENT copies a rectangle
  of the program's image (rows at most 4096 pixels apart) into the window during the call; WIN_SETSIZE gives
  the window pixels of another size, all background; WIN_TITLE; WIN_INFO (its size, the desktop's screen and
  scale, the focus, the events lost). `read()` gives whole 16-byte events (a key, the pointer, the wheel, the
  focus, a new size, the close button), waits for one, and gives 0 once the desktop let the window go;
  `poll()` says POLLIN, POLLHUP, and POLLOUT always. libsurf has `winopen()` and `winctl()`.
* **A descriptor, as a pty's master.** A window is a vnode of winfs, a file system with no paths (as ptyfs and
  pipefs are), opened as a file of the program's: its last close, or the program's reap after a kill, ends it.
  A desktop registers with `win_register()`, a descriptor of the desktop's own task, so its end, by its exit
  or its task's reap after it died, lets every window of it go (the trick of `shell/guisys_term.c`'s notes of
  10/18/26). A window lives while its program or its desktop holds it, and the second to let go frees it.
  Programs are not killed by the desktop's end: their reads end (0) and their presents fail (EPIPE). A window
  is the program's that opened it: that program's death takes it away at once, also when a child of it holds
  the descriptor.
* **Which desktop.** A program's window goes to the desktop of its terminal: the console where `gui`
  registered one, or a pty that desktop's Terminal adopted (`win_adopt()`), until that pty hangs up or is free
  again (`win_pty_gone()`, from `kernel/pty.c`: the next session on it, a telnet session's say, opens none).
  Text mode, another console, telnet's ptys and a desktop whose task is dead have none: -ENODEV, after the
  arguments were checked, so `run wintest nodesk` in the smoke test sees EFAULT and EINVAL first, and
  `@telnet-win` the same in a session.
* **Pixels are copied, not shared.** No frame of the kernel is mapped into a program, and nothing in `mm/`
  changed. A window's buffer comes from the kernel heap, filled with its background before anything sees it.
  WIN_PRESENT checks the rectangle's whole span with `user_buf()` in 64-bit arithmetic and copies it in with
  interrupts on (1.75 MB for a whole window on a 1080p screen; a frame drawn meanwhile may show one torn frame,
  and the next mends it). A new size allocates a new buffer, and the old one lives while the desktop still
  draws it: a buffer counts its holders (the window, the desktop's view of it, a present copying into it).
* **Events.** 64 are queued at most: a pointer move with the same buttons as the move queued last takes its
  place (never a press's or a release's), as a new size does the last one's; past that an event is lost and
  counted.
* **Budgets.** 8 windows and 8 MB of pixels per desktop, 32 windows and 32 MB in all (EAGAIN, ENOMEM), counted
  from the buffers alive, the desktop's views too (a window its desktop let go keeps none, only its size); a buffer no larger than the desktop's largest content area
  (958x457 on a desktop of 960x540); and each window takes one of its program's 16 descriptors.
* **The clipboard and asking to be seen** (10/26/26, stage C12: the desktop's applications as programs).
  WIN_CLIP copies 1 to WIN_CLIP_MAX (128 KB, the desktop's clipboard) bytes of the program's text into the
  kernel, to a NUL if one comes first (none at all is EINVAL), accepted only while the window has the focus
  (the desktop's FOCUS events set it; asked before the copy, so a refusal copies nothing, and again after it),
  and leaves it in its desktop's record with the window's handle, the latest in place of what waited. The
  desktop takes it each tick (`win_clip_take()`, struct gui_sys's `win_clip`) naming the window that has its
  focus then: text from any other is dropped, since FOCUS 0 reaches a window only after the desktop moved its
  focus. A desktop's end frees what it never took. WIN_ALERT counts in the window's state
  (`win_state.alerts`), once in WIN_ALERT_MS (5 s) at most for a program, whichever of its windows asks and
  however often it opens one (EAGAIN; `alerters[]` remembers the programs that asked, the oldest making room).
  The desktop then shows the window in front, out of the taskbar, without the focus and with its taskbar
  button marked, as a new window: after the pass's pointer events and keys (`gui_input_done()`), never while
  a pointer button is down, a drag goes on or a program holds a grab (it waits for their end), and for
  GUI_GRACE_MS (500 ms) after it came up a press on it is nobody's (`client_grace()`: no focus, no drag,
  nothing for the program). The ABI stays version 1, the ops appended: an older kernel says EINVAL; struct
  win_clip's 8 bytes are checked at compile time, as the other structures are. `wintest show` copies on x and
  asks to be seen on y.
* **Locking.** kernel/win.c takes short interrupts-off sections and no mutex; memory is allocated, filled and
  freed with interrupts on. It never calls the framebuffer, and the desktop never calls it from inside a
  `framebuffer_graphics_draw()` callback.
* **Checks.** `tests/host/test_win.c` builds kernel/win.c as it is with stand-ins for the rest of the kernel:
  the queue, rectangles and strides, sizes, budgets, titles, which desktop, both orders of ending, the
  desktop's exit and its death, the owner's death, no memory at each allocation, a buffer outliving a new
  size, stale handles; every scenario ends with nothing allocated. `user/bin/wintest.c` has `nodesk`, `pty` (a
  pty of its own is refused while a desktop runs) and `show`.
* **The desktop's side** (`shell/gui.c`, `shell/guiclient.c`). The desktop registers once its frame has
  begun and lets go at its end. Each tick it takes the windows that are ready (presented, or 250 ms old) into
  its free slots (`GUI_SLOTS` = the 15 fixed windows + 8), in front, in a cascade from (72, 52), 24 by 20
  apart, and without the focus (a program in the background never catches keys typed into another window,
  the rest of a password say): its taskbar button is marked until a click on it, on that button or Next
  window gives it the focus. A window asked for takes it as it appears: the first of a program the launcher
  just started, and that of the focused Terminal's foreground job (the kernel's `win_state.foreground`: its
  program is in the foreground job of a pty the desktop adopted, a job that session's shell made, whose pid
  `win_adopt_shell()` records, not merely the terminal's foreground job). A frame is a pixel of border and the title bar, its content drawn with `gd_blit()` from the pixels
  `win_update()` lends it until the next update. Every event over a program's visible content is the
  program's: hover, the wheel and all three buttons; a press there brings the window to the front and holds
  the pointer for it until every button is up (a grab), while the corner grip stays the desktop's. A size
  given by the corner or by maximizing is told once the corner is let go (one RESIZE), the old pixels drawn
  on the background until the program presents pixels of the new size. FOCUS says when the window takes the
  focus or loses it; the focused window takes every key but Ctrl+G. The close button sends CLOSE;
  pressed again it asks "End the program?" (Cancel first), and End kills the program. Leaving the desktop
  with programs' windows open asks first. A console switch changes nothing: presents go on into the kernel's
  buffers, and the desktop draws them again when it has the screen back (its pointer in the middle).
* **The Terminal** adopts its session's pty (`win_adopt()`, `shell/guisys_term.c`), so that a program typed
  in it opens its window on the Terminal's desktop, and drops it when the session ends.
* **libgui** (`user/include/gui.h`, `user/libsurf/gui.c`, `guikit.c`). The desktop's drawing kit
  (`include/sys/guidraw.h`, `shell/guidraw.c`), the framebuffer renderer (`driver/fbcore.c`) and the console's
  font are built into libsurf by one-line wrappers, so a program draws its image as the desktop draws its
  scene: rectangles, lines, UTF-8 text in code page 437, images, buttons, in the desktop's colours.
  `gw_present()` presents a rectangle; `gw_next()` polls and reads, and remakes the image at a new size
  before it returns the RESIZE. Integer only: until the kernel keeps floating point across task switches
  (track B's B2), a windowed program must not use it. `wdemo` is the example (`hang` ignores CLOSE, `crash`
  faults after its first present).
* **Programs started by the desktop.** `wcalc` is the desktop's Calculator as a program (the arithmetic of
  `shell/guicalc.c` built into both): the first application out of the kernel, and the pattern for the
  others. Apps > Run program... is a chooser (`GUI_DLG_RUN`) of /initrd/bin's programs above the line to run;
  `guisys_win_run()` spawns that line on the desktop's console, its input `/dev/null` (it never takes the
  desktop's keys), detached so that init collects it: it outlives the Terminal and ends with the desktop,
  whose end hangs its windows up (libgui's `gw_next()` then fails with EPIPE).
  10/26/26 (stage C12): `wclock`, the Clock, is the first of the suite's applications to move out: its model
  (`user/wclock/clock.c`: the pages, the calendar's arithmetic, the stopwatch, the timer, the keys and buttons,
  with the time handed in, host-tested by `test_wclock`) and its window (`main.c`: drawing as `shell/guiclock.c`
  drew, the time from `localtime_r()`, `gw_alert()` at zero, `gw_clip()` for Copy). The suite keeps the
  application's entry as a launcher (`struct gui_app`'s `program`): its tile, Apps > Clock and Next window's
  list start the program, or bring its window back (`app_program()` in `guicore.c` keeps the pid it started,
  `client_of_pid()` finds its window; pressed again within GUI_START_MS, 2 s, before that window came, it
  starts no second program: a double-click on the tile), and the window opens where the application's window
  stood
  (`accept()` with `gui_app_slot_of()`), not in the cascade. Edit > Copy sends a program's window Ctrl+C
  (`ACT_CLIENT_COPY`). `wcharmap`, the Character Map, followed (`user/wcharmap/charmap.c` the model with the
  console's code page 437 from libsurf, host-tested by `test_wcharmap`; `main.c` the window, drawing each of the
  256 characters with libgui's `gw_glyph()`); Go to code, which was the desktop's dialog, is a band of its own
  window.
* **On the screen** (`tools/gui_apps_win.py`, the guiapps scenario of `tools/qemu-usb.py`): wintest show and
  wdemo at 2x, keys, a press and a release echoed with where they were, F2 and F1 keeping a window's content,
  a corner resize answered, the close button ending a program (and `ps`), `wdemo hang` ended through the
  question, `wdemo crash` losing only its window, `wintest pty` refused in the Terminal, `wdemo` typed
  without `&` taking the focus as the Terminal's foreground job, wcalc from the launcher counting by keys and
  clicks, the Clock and the Character Map as programs read by the checks the desktop's had
  (`tools/gui_apps_tools.py`; the Clock's alarm is its window back in front without the focus, a click
  answering it), and a launched wcalc outliving
  the Terminal and ending with the
  desktop (ps on the serial console once the desktop has closed); once the Terminal has closed, a
  `wintest pty` started by the desktop takes the Terminal's pty number (`wintest tty` named it) and is
  refused there.

## Undertow HTTP/HTTPS browser (2026-10-12)

`/initrd/bin/undertow` is a ring-3 text browser linked with libsurf and pinned Mbed TLS
3.6.7. `url.c` handles addresses; `transport.c` owns TCP/TLS connections; `http.c` owns
framing and redirects; `html.c` turns bounded HTML into text, links and anchors;
`session.c` owns history; `view.c`, `edit.c`, `command.c` and `main.c` provide the console.
`config.c` handles the host table and session clock. Their headers define ownership
and failure contracts; there is no kernel HTML or TLS parser.

HTTPS verifies the chain, hostname and dates using the 121 pinned Mozilla roots in
`/initrd/etc/ssl/cert.pem`. TLS 1.2/1.3 require RDRAND-backed `getentropy()`; there is no
verification bypass or automatic downgrade. The bounded DNS syscall retransmits at
1 and 3 seconds within its 5-second deadline. The browser imposes limits on loads,
response bytes, text, history and link/anchor storage; see [UNDERTOW.md](UNDERTOW.md).

`make unittest` includes browser module and fixture/harness tests. `make test-web`
boots the real program against ephemeral local HTTP/TLS fixtures, with test roots
added only to copies of the production initrd. It includes simulated Wi-Fi and USB
Ethernet. Public interoperability (`tools/qemu-web.py --public`) is separate. These
checks do not establish physical laptop operation.
