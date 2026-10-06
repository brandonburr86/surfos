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
| Devices | VGA text console, or **ai-dev** a framebuffer console on UEFI (4 virtual consoles, each a tty with a line editor) mirrored to a COM1 serial console when there is one, PS/2 keyboard plus serial input, PIT, CMOS RTC, parallel port, 8237 DMA (incomplete), floppy (incomplete), full PCI enumeration with names, block layer with ramdisks from boot modules, ATA PIO disks and MBR partitions (**ai-dev**: also GPT, and USB sticks through the xHCI controller, section 16), 3Com 3c905B NIC (no protocol stack) |
| libc | `blibc`: printf/snprintf, puts/gets (through the tty), getch, the standard string and memory functions, strtol, ctype, CMOS time |
| Files | VFS with a mount table; tar file system for the initrd (read-only), FAT12/16/32 with long names (read and write, `mkfs` for FAT16/32); 1024-block write-back cache |
| Processes | ELF32 executables loaded from any mounted file system, argc/argv, exit codes, wait, 31 system calls, a small C library (`user/libsurf`), a user-mode shell, file programs and a text editor |
| Network | e1000 driver; Ethernet, ARP, IPv4, ICMP, UDP, TCP, a DHCP client and a DNS resolver in a kernel thread; `ifconfig arp ping dhcp nslookup tcpecho httpget netstat` |
| User interface | Ring-0 debug shell with about 70 commands; `run`/`spawn`, or type a program's name |

Required RAM is 32 MB (`mm/memory.c:21`). The kernel version string is 0.007 and
the shell calls itself v0.008.

## 2. Source tree

```
boot/        boot.S (Multiboot 1 + 2 headers, entry), grub.cfg (the ISO),
             grub-usb.cfg + grub-usb-bootstrap.cfg (the USB image)
kernel/      main.c (kmain), gdt.c (GDT + TSS), interrupt.c (IDT, PICs, dispatch, IRQ chains),
             traps.asm (256 entry stubs), task.c (scheduler), wait.c + sync.c + ktimer.c (wait queues, locks,
             kernel timers), timer.c (PIT), panic.c (exceptions, panic), ksym.c (symbol lookup, backtraces),
             klog.c (kernel log), console.c (VGA or framebuffer text grid, escape sequences, kprintf),
             tty.c (input queues, line editor, raw mode, console switching), keyboard.c (scan codes, LEDs),
             process.c (spawn, ELF loader, entering ring 3), syscall.c (int 0x80 dispatch),
             sys.c (sleep/beep/reboot), turf.c (Martin McCormick's reader/writer "turf" locks),
             multiboot.c + multiboot2.c (boot information), cpu.c, acpi.c, apic.c (profile=laptop)
mm/          memory.c (init), pmm.c (frame stack), memrange.c (64-bit address ranges), paging.c (page tables,
             page-fault handler, ioremap), kalloc.c (kernel and DMA heaps), uvm.c (per-process address spaces)
lib/blibc/   chars.c printf.c vsnprintf.c strings.c string.c memory.c ctype.c stdlib.c time.c
shell/       main.c (the shell), fdisk.c, fscmd.c (mkfs mount umount eject df mv fsbench), demos.c,
             selftest.c, parport.c (lpstat)
fs/          vfs.c (mounts, paths, open files, descriptors), bcache.c, tarfs.c, fat.c, fat_mkfs.c (mkfs),
             devfs.c (/dev)
net/         netbuf.c (buffers, devices, the net thread), eth.c, arp.c, ip.c, icmp.c, udp.c, tcp.c, dhcp.c, dns.c
user/        user.ld; libsurf/ (the C library: crt0.S syscalls.c stdio.c printf.c stdlib.c string.c ctype.c
             malloc.c dirent.c time.c); include/ (surf.h, the standard headers stdio.h stdlib.h string.h
             unistd.h fcntl.h errno.h dirent.h time.h ctype.h limits.h stdarg.h stddef.h stdint.h, and
             sys/stat.h sys/statfs.h sys/types.h); bin/ (hello crash cat ls count sh echo cp mv rm mkdir
             rmdir touch df wc head hexdump fstest edit)
driver/      drivers.c (driver table), pci.c + pci_names.c + PCIDATA.H, serial.c, bdev.c (block layer),
             ramdisk.c, ata.c, part.c + parttab.c (MBR and GPT partitions; mbr.c until 10/5/26),
             framebuffer.c + fbcore.c + font8x16.c (the UEFI console; fonts/ has the Spleen source),
             net/e1000.c, parport.c, dma/, floppy/, net/3c905b/ (2004, unwired),
             usb/ (xhci.c, usb.c, usbnet.c, usbdesc.c, msc.c: the USB stack, sections 15 and 16)
include/     surfos/ (kernel headers), mm/, sys/ (driver headers: bdev.h, part.h, usb.h, ...),
             fs/ (vfs.h, bcache.h, fat.h), net/, asm/io.h, blibc headers
tools/       qemu-run.py (headless QEMU harness), qemu-usb.py (the USB image under OVMF), gensyms.py (symbol
             table for backtraces), mkimage.py (test disk image, initrd, the "foreign" FAT32 stick),
             mkusb.py (the USB image), bdf2c.py (the font table)
rootfs/      packed into build/images/initrd.tar, the Multiboot module behind rd0
tests/       host/: unit tests built with the host compiler (make unittest): blibc, boot information,
             framebuffer, USB parsing, partition tables, mkfs, the user C library;
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
  framebuffer with an 8x16 font (`driver/font8x16.c`, Spleen), scaled by the largest whole
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
  the same escape sequences as the screen, and nothing outside printable ASCII is sent).
  IRQ 4 feeds received bytes to the active tty (CR to newline, DEL to backspace,
  `ESC [ A/B/C/D/H/F` to the arrow/Home/End key codes; **ai-dev**: in raw mode, the bytes
  as they came). `qemu -nographic` therefore gives a complete terminal session;
  `tools/qemu-run.py` drives it. After a console switch the serial terminal is cleared and repainted
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
* **Escape sequences and raw mode** (**ai-dev**, for the text editor; `include/surfos/tty.h`
  is the contract). Each console parses the ANSI subset full-screen programs need: cursor
  position and moves (`CSI H f A B C D`), erase in display and line (`CSI J K`), bright,
  inverse and the eight colours (`CSI m`), and hiding the cursor (`CSI ? 25 l/h`). Any
  other sequence is read to its end and dropped, and a console with no attributes set
  prints in the writer's colour, so `kprintf()` and the older programs look as they did.
  Wrapping is VT100's: writing the last column leaves the cursor there and the next
  character starts the next line, so a program can write the bottom right corner without
  scrolling. Only printable ASCII (32-126) is drawn; other bytes are dropped. `tty_ctl()`
  (system call `ttyctl`) with `TTY_RAW` 1 stops echo and line editing: keys arrive one at a
  time, Ctrl-C is just 0x03, the keyboard's special keys become the sequences a terminal
  sends (`ESC [ A` ... `ESC [ 6 ~`, Backspace 0x7f, Enter CR), and serial input arrives as
  sent. `TTY_GETSIZE` gives `(columns << 16) | rows`. Raw mode, colours and a hidden cursor
  end with the task that set them (`tty_task_gone()`, from task exit and `trap_fatal()`),
  so a crashed editor cannot leave its console raw or unreadable. A read of `/dev/console`
  is `tty_read()`: a line with its newline, or in raw mode the keys queued (at least one).

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
| Block layer | `driver/bdev.c`, `include/sys/bdev.h` | Named 512-byte block devices with read/write ops; partitions translate to their parent; range checks and per-device counters (`lsblk`, `hexdump`). **ai-dev** (USB storage): a disk's bus (RAM, ATA or USB; 0, what a driver that says nothing gets, is never formatted), removable, read-only, `protect` (why it must be left alone) and a `flush` op; `bdev_may_format()`; unplugged disks unregistered but never freed; mount reference counts (section 16) |
| Ramdisk | `driver/ramdisk.c` | Every Multiboot module is a writable ramdisk `rd0`, `rd1`, ... (`qemu -initrd`, GRUB `module`); modules above 16 MB are mapped with `ioremap()`. **ai-dev**: bus RAM, so `fdisk` and `mkfs` may write one that is not mounted; the smoke test adds a blank one as `rd1` (`tools/qemu-run.py --scratch`) |
| ATA | `driver/ata.c` | Polled PIO, LBA28, both legacy channels, IDENTIFY, cache flush after writes, a mutex per channel; disks are `hda`-`hdd`, ATAPI devices are reported and skipped. **ai-dev**: bus ATA, an internal disk, which `fdisk` and `mkfs` never change |
| Partitions | `driver/part.c`, `driver/parttab.c`, `include/sys/part.h` | The four primary partitions of each disk become `hda1`-`hda4` with their type (`driver/mbr.c` until 10/5/26). **ai-dev**: also the logical partitions of an extended one (from 5) and GPT; a disk whose name ends in a digit gets a `p` (`usb0p1`); the SurfOS boot drive is recognised; `fdisk`'s changes are planned, written, read back and checked (section 16) |
| e1000 NIC | `driver/net/e1000.c` | Intel 82540EM (QEMU's default): MMIO through `ioremap()`, 32-entry receive and transmit rings in the DMA heap, MAC from the EEPROM, receive interrupts, transmit completion polled. QEMU holds frames for one second after RCTL is written, so the first reply after boot is late |
| xHCI USB | `driver/usb/xhci.c`, `driver/usb/usb.c` | **ai-dev**: the USB 3 host controller (PCI 0c03, interface 30), polled; root hub ports, enumeration at boot and on every port change, `lsusb` (section 15); bulk transfers a task waits for, and endpoint resets, for the storage driver (section 16) |
| USB Ethernet | `driver/usb/usbnet.c`, `driver/usb/usbdesc.c` | **ai-dev**: CDC ECM and NCM adapters (RTL8153 through its CDC Ethernet configuration, QEMU's usb-net) as `eth0`, `eth1`, ... (section 15) |
| USB storage | `driver/usb/msc.c` | **ai-dev**: sticks, external drives and card readers (Bulk-Only Transport, SCSI commands) as `usb0`, `usb1`, ..., with their partitions; plugging, unplugging and medium changes; never mounted at boot (section 16) |
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
  blocks and lets go of the device.
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
  is released.
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
  unmounted.
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
  collects the exit code. **ai-dev**: `process_spawn_files()` (system call `spawnfd`)
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
  with; `ttyctl` is raw mode and the screen size (section 7).
* **User space** (`user/`): `user.ld` links static ELF executables at 0x40000000 and
  `crt0.S` passes argc/argv to `main()`. **ai-dev** (10/5/26): `libsurf` is a small C
  library of its own, `libsurf.a` (programs see only `user/include`, never the kernel's
  headers, and `--gc-sections` keeps each to what it uses). A failed call returns -1 (or
  NULL) and sets `errno`, whose numbers are Linux's and the kernel's. It has buffered
  `FILE` streams (stdout line-buffered on the console, stderr unbuffered, everything
  flushed at `exit()`), one formatter for every `printf` (no floating point) and `sscanf`,
  `malloc`/`free`/`realloc` with boundary tags and first fit (the heap grows by `sbrk()`,
  64 KB at least), the string and memory functions, `strtol`, `qsort` and `bsearch`,
  `opendir`/`readdir`, the file calls (`open`, `lseek`, `stat`, `fstat`, `rename`,
  `ftruncate`, `fsync`, `statfs`, `dup2`, ...) and `time`, `localtime`, `mktime` and
  `strftime`. `surf.h` keeps what only SurfOS has: `spawn`, `spawnfd`, `wait`, `ttyctl`,
  `uptime`, `msleep`, `getdent`. Programs, in the initrd under `/bin`: `hello`, `crash`
  (NULL write, kernel write, cli, hlt, in, int 0x40, EFAULT, deep recursion, divide by
  zero), `count`, `sh` (spawns from `/initrd/bin` or a path and waits; `"quoted words"`,
  `<`, `>`, `>>`, a trailing `&`, and `cd`, `pwd`, `echo`, `exit` built in), `cat`,
  `ls [-la]`, `cp [-r]`, `mv`, `rm [-rf]`, `mkdir [-p]`, `rmdir`, `touch`, `echo`, `df`,
  `wc [-lwc]`, `head [-n N]`, `hexdump`, `fstest` (the file API from user space, `FSTEST
  PASS`) and `edit`.
* **The editor** (`user/bin/edit.c`, **ai-dev**): full screen, in the manner of nano. The
  file lives in memory as an array of lines (up to 1 MB); the screen is a title line, the
  text, a status line and a line of keys. Lines do not wrap: the cursor's line scrolls
  sideways and a `>` in the last column says a line goes on. Keys: the arrows, Home, End,
  PgUp, PgDn, Backspace, Delete, Tab; ^S save, ^Q quit (asking when there are unsaved
  changes), ^F find, ^K cut the line, ^U paste, ^G go to a line, ^C where the cursor is,
  ^L redraw. Saving writes `<file>.tmp`, fsyncs it and renames it over the file (or
  writes in place where a file system cannot rename), keeping the file's line ends (LF,
  or CR LF when its first line has one). The terminal is used only through `ttyctl()`,
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
| `lsblk [-s]` | **ai-dev**: disks and partitions as `NAME SIZE BUS RM TYPE MODEL/LABEL STATUS`, the status `mounted`, `boot drive: protected`, `internal: never formatted` or `read-only`; `-s` the blocks each device has moved |
| `fdisk <disk>` | **ai-dev**: the partition table, the free space and whether the disk may be changed; with `new mbr` or `new gpt`, `add [size] [type]`, `delete <n>` or `type <n> <type>`, a change (section 16) |
| `mkfs [-t fat16 or fat32] <dev> [label]` | **ai-dev**: a new FAT volume; a partition gets the matching type, a whole disk loses its old partitions |
| `mount [-r] [dev [path]]`, `umount <path or dev>`, `eject <disk>`, `df`, `mv`, `sync` | **ai-dev**: mounting by hand (on `/<dev>` unless a path is given, read-only with `-r`); `eject` unmounts every partition of a disk, all or none, writes its blocks and flushes the device before it says `safe to remove` |
| `fsbench <dir> [MB]`, `readbench <dev> [MB]` | **ai-dev**: time writing and reading a file in 4 KB and 64 KB pieces; read a device in 64 KB requests (nothing is written) |
| `rawkeys`, `ansi` | **ai-dev**: the bytes keys send in raw mode (until `q`); a test pattern of escape sequences |

**ai-dev**: `fdisk` and `mkfs` show the device, what is on it and the change, then ask for
the device's name (`Type usb1 to write the new partition table to usb1 (...):`); any other
answer prints `Cancelled` and nothing is written.

## 13. Build system

One non-recursive `Makefile` at the top level (the 2004 per-directory Makefiles with
their GCC 3 flags were removed on `ai-dev`; `master` still has them). Objects and the
kernel go to `build/O<level>/`; `make O=2` builds an optimised kernel next to the
default `-O0 -g` one. `linker.ld` places the Multiboot header first, the `.ksyms`
symbol table after the data, and discards the sections a 2026 toolchain adds. The
kernel is linked twice so the symbol table can hold the final addresses. Targets:
`all`, `run` (QEMU, serial console on the terminal), `run-vga`, `test` (headless
smoke test through `tools/qemu-run.py`), `test-all` (-O0 and -O2), `unittest`
(the host tests in `tests/host/` and `tests/test_part_tools.py`), `debug` (gdb stub),
`iso` (GRUB image), `run-iso`, `clean`.
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
  against the font, with the grid and margins of `driver/fbcore.c`. Eleven scenarios:
  serial, keyboard-only at 4 GB (raw mode and escape sequences included), the IdeaPad's
  1920x1080 panel filled with 120x33 cells (`fullhd`), 6 GB with `himem` checked through
  the monitor, the troubleshooting entry, no PIT, a USB Ethernet adapter (section 15), two
  USB storage scenarios (section 16), and images missing their initrd or kernel.

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
* **Limits.** The screen, the serial mirror and the font show printable ASCII only, and
  the keyboard types nothing else: a UTF-8 name is stored and read correctly but shows
  without its other letters, and cannot be typed. `umount` and `eject` see only the asking
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
  January 2038; `printf` has no floating point; `lseek` takes a signed 32-bit offset, so a
  file can grow to FAT's 4 GB limit but positions past 2 GB cannot be sought.
