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
consoles; C9's scrollback since 10/25/26, track A), **T9** (the input queue is filled under `irq_save()` and read the same way),
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

Found by the review of the network tools (October 2026, `docs/superpowers/plans/2026-10-24-nettools.md`)
in code that was there before them, and that an FTP client reaches now as well as `mv`:

| ID | Where | Problem | Fix |
|---|---|---|---|
| R1 | `fs/vfs.c`, `fs/fat.c` | `vfs_rename()` refuses to move a directory into itself by comparing the paths with their case, and FAT folds it: `mv dir DIR/sub` wrote the directory into itself and took it out of its parent, its `..` pointing at itself (a subtree nothing reaches, lost clusters) | `fat_rename()` follows `..` up from the new parent and refuses (EINVAL) whatever the spelling |

Found by the review of programs' windows (October 2026, `docs/superpowers/plans/2026-10-25-app-windows.md`) in job
control (surfsh's pipelines and scripting, October 2026), recorded and still open. The desktop's foreground exception
does not rely on it: `kernel/win.c`'s `in_foreground()` takes only a job the Terminal session's shell made
(`win_adopt_shell()`), so a foreground set this way gives no window the focus.

| ID | Where | Problem | Fix (to come) |
|---|---|---|---|
| T14 | `kernel/syscall.c` (`sys_job()`, `JOB_FG`) | the permission check (a caller in a job in the background may not, but the foreground job's shell may) applies only to a caller in a job: a task in no job that is still on the terminal (a program that called `detach()`, a child spawned with `SPAWN_JOB_NONE`, one a kernel command started) may make a job it made, or none, the terminal's foreground, taking the terminal from its shell: the only job whose tasks may read it, and the one Ctrl+C and Ctrl+Z reach | a caller in no job may set the foreground only if it is the tty's `fg_shell`, or the session shell the pty recorded at its spawn |

Found by the telnet stalled-client check's lost line and its "never filled" (10/25/26: gui's C5 and C6 gates, daily's
A6; October 2026, track B after its merge) in the pager and the stream consoles of the shell and terminals
(milestones M10, M11):

| ID | Where | Problem | Fix |
|---|---|---|---|
| C10 | `kernel/console.c` (`stream_put()`, `page_ask()`, `page_row()`), `kernel/pty.c` | two causes, one check: (1) a stream console counted a byte into its state (the column, the rows, a row's start) before the master's buffer had room for it. A writer waiting for room (a telnet client that stopped reading fills the buffer) held a newline the console had counted already, so the shell, paging its command line, found a row start and asked: the question (PTY_OUT_URGENT, the room kept for it) went into the stream ahead of the waiting newline, in the middle of the job's line ("`|................|\033[7m--More--`"), and the CR ESC[K that took it away cleared the next row instead: the line was lost to whatever reads the stream as lines. (2) Why the shell asked at all: every row on the console counted toward the owner's screenful, a job's in the background too, so a shell printing "[1] started" behind a screenful of its job's rows stopped at `--More--` for a key nobody typed, and the job printed on around the question; the check gave up when the line never came within its 10 s ("the send buffer never filled", gui's C6), or the shell's own text, released late, cut hexdump's lines a byte at a time (C11) | (1) room first (`pty_room()`, `pty_wait_room()`): the byte, what it does to the console's column, rows and escape parser, a question taken away before it and brought back after a newline are one step, interrupts off (`stream_put()`); a writer that finds no room has changed nothing, waits and does it all again (`stream_byte()`), the pager's look included; `page_ask()` and the re-show look again in their own step, interrupts off, that the row is at its start with no escape sequence open and no other question up (the review of the fix). A writer that may not wait (a mutex holder, the echo of a master's write, a call cut short) still counts bytes `pty_out()` drops. (2) a semantics change: a row counts toward the screenful only when a task the pager covers starts it (`page_row()`); a command's own output is still paged after a screenful of its own rows, and the rows of a job in the background or a kernel thread count toward none, at the accepted cost that with a job flooding the screen during a paged command some of the command's rows may scroll away before its question. **Verified** by test_pager (`t_stream_full_row`, `t_stream_background_rows`, `t_stream_question_tick`, `t_stream_ask_again`, the last two with a tick at a chosen `irq_save()`; each fails on the code before its part of the fix) and the stalled-client check over fresh boots |
| C11 | `kernel/console.c`, `kernel/pty.c` | (recorded, not fixed) two writers on a full pty's stream console take turns a byte at a time: each byte waits for room by itself, and a wake-up lets whichever runs first go on, so one `write()` (a shell's whole prompt) can be spread across another program's lines | a writer holding the stream for the length of one write, as K1 does for a kernel thread's line on the screen, would keep each write together |

### Storage milestone 1 (October 2026, branch `storage/main`)

Interrupted saves, failed writes and concurrent unmounts preserve existing files. The SurfCPU branch
found and closed these in the block cache and FAT it shares with x86 (its `docs/CODE-AUDIT.md`, items
B1-B82, thirteen review rounds); they are brought back here where x86 needs them, under the same IDs, with
one rule of x86's own in place of SurfCPU's copies (B12): a file system that must know what reached its
device syncs holding its volume's lock, so nothing makes that device's blocks dirty meanwhile. Not brought
back: SurfCPU's kill model and what rests on it (B39's `fs_hold`, B46-B57, B59-B82), whose ROADMAP.md note
says what stays open, but for two eject fixes x86 needs without it: B58, and eject's part of B46 (the
review's table below). The items W1-W3 are x86's own. The rows are **verified** by `tests/host/test_fatfs.c`
(`make unittest`: the real `fs/bcache.c`, `fs/fat.c` and `fs/fat_mkfs.c` over RAM disks that refuse writes,
or are pulled out, at every write, FAT16 and FAT32, with a check of the image after every write the disk
stores, and fsck.fat on the results), which fails 88 of its 655 checks on `ai-dev`'s code before these
changes (40 of its 46 scenarios), but for B2 and B10's shell side, verified by `selftest` (a RAM stick that
refuses writes, in the smoke test: close, rm, mv, eject, umount), and B58, from reading only:

| ID | Where | Problem | Fix |
|---|---|---|---|
| B1 | `fs/bcache.c` | a dirty block that had to make room and could not be written was dropped ("lost, and said so"): the next sync found nothing wrong, and a dropped FAT or directory block let FAT read its old copy back and hand out clusters a file had | the block stays dirty while its device is there, is never reused before a write of it works, and every sync tries it again |
| B3, B7 | `fs/bcache.c`, `fs/fat.c` | dirty blocks reached the disk in the order of the buffer table: a stick pulled in a sync could keep a new file's entry with its cluster still free, or (a save) the old file's name on clusters freed for the new one (test S1 on `ai-dev`: `NOTES.TXT` on a free cluster after a pull at the save's seventh write) | a write order (data, FAT, entries, FSInfo), checked where every write is issued; FAT syncs between the steps that need the reverse (an entry off the disk before its clusters are freed, a cut before the clusters past it, a new cluster's FAT entry before a link to it in another FAT sector, a new directory before anything names it) |
| B4, B23 | `fs/fat.c` | a write that stopped early (the volume full, a block refused) left the clusters it linked past the file's end; a gap of zeros refused in part grew the file by what was written | a run ends where the volume does, a write puts in what fits, the chain is cut back to the file's size; the gap is all or nothing |
| B5, B17, B24, B27, B29 | `fs/fat.c` | an operation whose sync failed stopped half done in the cache: a rename left two names on one chain, an rm the name gone and the clusters lost | what the operation changed is put back and it says EIO (nothing changed); ".." first in a move; an undo puts back only what it saved and changed; one that cannot be done leaves the volume read-only (EROFS) until unmounted, said once, and marked |
| B6, B14 | `fs/bcache.c`, `fs/fat.c` | with B1, a device that refused every write would fill the cache every device shares, and every read of every device would fail | a refusing device keeps at most a quarter of the cache (its writes fail with EIO past that), making room tries none of its blocks, and it is asked again with one write before a write is refused |
| B8, B9, B13 | `fs/bcache.c` | (in the order's own machinery) making room could write a block that changed hands while it wrote what the block needed; one bad sector held back the blocks of its run | the cache is looked at again after each write; each device keeps the cache's account of itself (`bcache_audit()`); a failed run is tried again block by block in the same sync |
| B10 | `fs/vfs.c`, `shell/fscmd.c` | the last close of a written file returned 0 whatever happened, so `cp` and `edit` said a file was saved that only the cache held; `umount` gave up what could not be written without a word | close returns the sync's error, as fsync does; `umount` and `eject` say how many changed blocks they gave up, and fail |
| B2, B58 | `shell/fscmd.c`, `fs/vfs.c` | `eject` unmounted a disk whose writes failed (giving up what it could not write) and said "safe to remove"; a mount of another partition between its listing and its drop of the disk's blocks had them dropped under it | every mount is written out first and none goes when one cannot be; the disk is the eject's from its first step to its last (`vfs_claim_disk()`) |
| B11 | `fs/fat.c` | `fat_set()` changed the first FAT and gave up when the second copy's block could not be read | both copies' blocks in hand before either changes |
| B15, B22 | `fs/bcache.c` | a straight write put its bytes into the cached copies before the device had them: a refused write was in the file as read back, and written by the next sync | the copies take the bytes once the device has them; when it refuses, the clean ones are dropped (it may have stored part of the request) |
| B16, B28, B32, B36 | `fs/fat.c`, `fs/bcache.c` | (with B3) a block of a file that refused held back the sync that takes that file's entry away: it could not be removed | the dirty blocks of a chain about to lose its last name are set aside for that sync, and dropped once the chain is free; so are those of every cluster freed or given back |
| B18 | `fs/bcache.c` | a sync waited for ever for a block someone held: a task killed at once while it held one left every later sync of that device hanging | a sync gives up on a hold after 10 s (-1, said) |
| B25, B30, B31 | `fs/fat.c` | (with B3) clusters cut off while the device refused the sync between the cut and their free had to be freed or lost | they wait (up to 64 chains) to be freed after a later sync that works |
| B38, B42, B43, B44 | `fs/fat.c` | the unmount marked a volume clean that had clusters a failed free left allocated, that the session damaged, or that was not clean when it was mounted: the next computer was told not to check it | such a volume stays marked as not cleanly unmounted and the unmount says why; only a checker clears the mark |
| B35, B40 | `fs/bcache.c` | `binvalidate()` kept blocks held or under I/O, and an umount left blocks of its volume cached | dropped when let go or when their I/O ends, out of the table at once |
| W1 | `fs/fat.c` | the ROADMAP's 10/17/26 note: two names on one chain (an interrupted rename's, SurfOS's or another system's), and an `rm`, a `mv` over either or a truncate freed the chain under the other (test S3 on `ai-dev`) | a chain that another entry of the same directory holds is never freed or cut: an rm or a replace keeps it for the other name, a truncate to 0 lets go of it, one to a smaller size says EBUSY |
| W2 | `fs/fat.c` | a rename over an existing file (every save of `edit`, the desktop and `rx`) wrote a second entry of the name, then deleted the replaced one, then the moved one: a stick pulled in between held the name twice, or two names on one chain | the moved name goes first, then the replaced file's short entry takes the moved file's cluster, size, times and attributes in place, keeping its name and long name (one sector, when both are in one directory block): on the disk the name always holds a whole file, the old one or the new, and no two names share a chain |

Found by a review of the above (10/24/26: the real files over the test's harness, seven scenarios of its own, now in
`tests/host/test_fatfs.c` under the IDs below) and fixed; each scenario fails with its fix taken out. The review
found the write order, the undo paths, the cache's account and the live vnodes of a rename sound (W2b-W2d and B25b:
a rename over an object in another directory and a save, pulled at every write and with one write refused and the
stick pulled at any later one; a truncate, an rm and a short write the same way):

| ID | Where | Problem | Fix |
|---|---|---|---|
| B30 | `fs/fat.c` (`fat_table_point()`) | a mkdir that failed after another task's sync had written its name put the name back and left its cluster waiting (B25's chains); the next table point (any mkdir, a folder or a file growing into another FAT sector) freed it, writing only data and tables, so the FAT's free could reach the disk before the name's removal, and a file made meanwhile took the cluster the disk's directory still named (scenario B30b) | a table point frees no waiting chain: only an order point or a sync, which write entries |
| B3 | `fs/fat.c` (`fat_order_point()`, `fat_sync()`) | order points and syncs owed FSInfo, which is rewritten at every sync: one refusing FSInfo sector failed every rm, mv and truncate of the volume, and every close (scenario B3s) | they owe data, tables and entries; FSInfo, a hint, is written after them and a refusal of it fails nothing but the unmount's clean mark (the next mount then counts the free clusters) |
| B13 | `fs/bcache.c` (`drop()`) | a buffer reused for another block kept the mark of the sync call that wrote its old one: made dirty by another task while that sync went on, the new block was passed over, and an entry block waiting for it made the sync say -1 though every write worked (scenario B13p) | a dropped buffer carries no sync call's mark |
| B18 | `fs/bcache.c` (`bsync_order()`) | the 10 s wait for a block someone holds outlasted a kill's 2 s: a task killed in it (an order point, holding the volume's lock) was killed at once there, and every later operation on the volume, umount's too, waited for that lock for ever (scenario B18k) | the sync of a task being killed gives up on a held block at once (-1: the operation fails and lets go of the lock) |
| B46 | `shell/fscmd.c` (`cmd_eject()`) | eject went on to unmount after a Ctrl+C or a kill that came while it wrote the mounts out | it unmounts nothing then and says so ("interrupted: it stays mounted") |
| W3 | `fs/fat.c` (`fat_unlink()`) | `fs/vfs.c`'s `remove_entry()` looks at the file's references and lets go of it before FAT removes it, so a file opened in between had its chain freed while open, and its writes went to clusters the next file would take (scenario W3) | FAT refuses to remove an object in use (EBUSY), as a rename over one already did |

Left as it is (as on SurfCPU, its B16): while a dirty block of the volume refuses writes, an order point waits for it
and fails, so rm, mv and truncate of other files, and closes, say EIO (nothing changed) until the block is written,
its file is removed (B16 lets that rm through) or the volume is unmounted (which gives it up and says so).

### Robustness (October 2026, branch `robust/main`)

SurfOS should survive a misbehaving program (`docs/superpowers/plans/2026-10-25-robustness.md`, track B). The
track's IDs use a letter of their own: L1-9 memory, L10-14 floating point, L15-19 kills and stacks, L20-29
shutdown. L2 also closes the out-of-memory half of M6, and L17 closes M10. Left as they are (B1): the kernel objects
made for programs (tasks, spawn arguments, pipes, sockets) still come from the kernel heap, bounded by the spawn check
and their caps (their kernel stacks no longer do: L17); the heap never shrinks, so what the desktop's 8.7 MB grew it
by stays mapped after it closes (trimming the heap's top is a follow-up). Left as it is (B3): the idle task runs on
the 16 KB boot stack in `.bss`, which has no guard; the trap frame check (L18) covers it. It sits right above
`.ksyms` (linker.ld puts COMMON first), so an overflow there would write over the symbol table that backtraces read. Closed so far:

| ID | Where | Problem | Fix |
|---|---|---|---|
| L1 | `mm/pmm.c`, `mm/uvm.c` | programs and the kernel took frames from one pool with one rule: a program that allocated until malloc() failed took the last frame, and the kernel's next need found none | frame classes (`pmm_alloc_as()`): programs (`PMM_USER`) stop while the kernel's reserve is left (a 32nd of the frames, 2 MB to 16 MB), large kernel buffers (`PMM_BULK`) at half of it, the kernel (`PMM_KERNEL`, `pmm_alloc()`) may take the last frame; refusals and the fewest frames left are counted. The rule is a pure inline (`pmm_class_may()`), **verified** by `test_kalloc`, and by `memhog` in the smoke test and under OVMF (it stops at the reserve to the page). Where programs may have more than about 1 GB (the laptop), one memhog stops at its address space's end instead (a 1 GB user range less 4 MB of stack): several at once reach the reserve (the review of B1) |
| L2 | `mm/kalloc.c`, `mm/paging.c` | the kernel heap is demand-paged, and its page fault panicked when no frame was left ("out of memory: no frame for kernel heap page"): out of memory halted the machine (the other half of M6). Any fault in the heap's 256 MB was given a frame, also far above its top | the heap stays demand-paged (ROADMAP section 1), but growing it promises the frames its pages will take (`pmm_commit()`: as `PMM_BULK` for a request over 64 KB, as `PMM_KERNEL` otherwise), and a growth that cannot be promised fails, so `kalloc()` says NULL (a free block at the top counted: only what it lacks is grown and promised, the review of B1); the fault takes a promised frame (`pmm_take_committed()`), always there, and only below the heap's top: above it is a stray pointer, reported as any fault. A promise is a counter: an 8 MB growth does no work per frame with interrupts off. **Verified** by `test_kalloc` (the real `mm/kalloc.c` over a stand-in that refuses promises on demand: NULL and the heap as it was, every page promised before the top reaches it, 20000 random steps with `heap_check()` after each), and by `memhog` (the kernel works on at the reserve) |
| L3 | `mm/uvm.c`, `mm/paging.c` | a program whose stack could not grow for want of memory got the page fault of a wild pointer ("access to unmapped page") | `uvm_grow_stack()` says -ENOMEM, and the program is ended with "out of memory: its stack could not grow". **Verified** by `memhog stack` in the smoke test |
| L4 | `mm/uvm.c`, `include/mm/memory.h` | each page table and page directory of a program took 8 KB of kernel heap (a page, aligned inside two): the route by which a program could drain the kernel | they are frames of the programs' class, mapped for the kernel in a window of its own (0x80000000-0x8FFFFFFF, `UVMWIN`), which every address space shares |
| L5 | `mm/uvm.c` | `sbrk()` that could map only part of a growth left those pages mapped and counted, unreachable by the program | what the call mapped is given back |
| L6 | `mm/uvm.c` | mapping said -1 for every failure, so neither the loader nor the page fault could tell out of memory from a bad address | -ENOMEM and -EINVAL |
| L7 | `kernel/task.c` | a zombie kept its user frames and page tables until its parent collected it, which a shell may do much later | `task_exit()` destroys the address space after closing the files (detached, the kernel's directory loaded, interrupts off); a task killed on the spot (a fault, Ctrl+C in a program's own code, a kill that waited too long) hands it to the `uvmreap` thread from `become_zombie()`, which may run in an interrupt; `schedule()` compares with the directory loaded (`uvm_current()`), not the outgoing task's. The review of B1: every page and table taken for an address space, and the address space itself, is recorded in it before interrupts come back on (`table_for()`, `uvm_map_page()`, `user_entry()`), and a page given back leaves it the same way (`unmap_page()`), since a kill that has waited 2 s ends the task on the spot from the timer (`kill_escalate()`): what was taken but not recorded leaked. **Verified** by `selftest` (64 programs at once, all there together until the pipe that is their input closes, twice: the heap's bytes in use and the frames left come back, but for what the heap grew by; 16 killed in their own code, twice: the same once uvmreap has freed them), the smoke test (`memhog hold`: free pages less those promised back within 1%, surfsh's prompt throughout, no word from init) and `guiapps` (the kernel heap holds what it held before `gui`) |
| L8 | `kernel/process.c`, `kernel/task.c` | a program started on a machine with no memory left failed while it loaded | a spawn says ENOMEM at once when fewer than 16 frames are left for programs ("Out of memory", the smoke test's `memhog hold`). The review of B1: a loading that fails later for want of memory marks its task (`TF_LOADNOMEM`), and init tries a console's surfsh again as it does after a spawn's ENOMEM, where it used to take the console for the recovery console until a reboot; surfsh's ends while fewer than 256 frames are left for programs do not count toward the recovery console either. **Verified** by `@boot surfsh=toobig` (a "shell" of 1000 MB: tried again every second, never the recovery console) |
| L9 | `mm/memory.c` | `memstat` could not say why an allocation failed | two lines: the reserve, the frames promised to the heap, the fewest ever left, the pages each class was refused, what programs hold, and what ended programs left to free (uvmreap's, and those `uvm_destroy()` is freeing, counted down as each frame is free: the review of B1) |
| L10 | `kernel/task.c`, new `kernel/fpu.c` | nothing saved the floating-point registers between tasks: no fxsave, fnsave or clts, CR0.TS never set, the device-not-available trap (#NM) not handled. Two programs using floating point at once corrupted each other's values | each process has a 512-byte area for its registers, made with it (every register zero, control words 0x037F and 0x1F80) and freed when it is collected; kernel threads have none. Switched lazily: `schedule()` sets CR0.TS for a task that does not own the FPU (`fpu_switch()`, which writes CR0 only when the bit changes), and the #NM handler saves the owner's registers (fxsave, or fnsave without FXSR) and loads the current task's; `become_zombie()` lets go of the FPU, and the trap clears what a dead owner left pending before the next restore (`fnclex`: found by the review of B2, below). **Verified** by `fputest race` (8 x87 and 8 XMM registers and both control words held across 300 ms, compared bit for bit), two at once in `selftest`, and by hand under `-cpu pentium` (fnsave) |
| L10b | `kernel/fpu.c` | (recorded by the review of B2, not fixed) on AMD's K7 through Zen 1, fxsave and fxrstor skip the x87 instruction and data pointers and the opcode (FIP, FDP, FOP) while no exception is pending, so a program reads its previous owner's last x87 instruction and operand addresses. The laptop is Intel, QEMU is not affected, and SurfOS randomizes no address those would reveal | Linux's workaround for its X86_BUG_FXSAVE_LEAK: before fxrstor, clear the exceptions and run one x87 load of a kernel address (`fnclex; emms; fildl`), which sets the three to known values |
| L11 | `kernel/cpu.c` | CR4.OSFXSR, OSXMMEXCPT and OSXSAVE were left as the boot loader set them, so whether a program's SSE ran or raised #UD depended on the boot loader (GRUB on OVMF hands over CR4 0x648, OSFXSR and OSXMMEXCPT on; QEMU's loader 0), and firmware that leaves OSXSAVE on lets AVX run, whose registers no switch would keep | OSFXSR and OSXMMEXCPT set from CPUID (FXSR, SSE), OSXSAVE cleared (AVX raises #UD); `cpuinfo` says what a program's floating point has. **Verified** by `fputest` (SSE2 results, MXCSR 0x1F80) in the smoke test and under OVMF (`make test-usb`) |
| L12 | `user/libsurf/crt0.S`, `kernel/process.c` | the kernel started `_start` 4-aligned and `crt0` called `main` off the 16-byte alignment the i386 ABI makes the rule at a call. GCC realigns `main`'s own frame, so its programs did not notice, but code that trusts the ABI there (another compiler's, or a `_start` of its own) faults on its 16-byte SSE stack slots (`movaps`) | the kernel starts `_start` 16-aligned and `crt0` calls `main` 16-aligned; a program opts into SSE2 with flags of its own (the Makefile shows how). **Verified** by `fputest` (main's first argument, which `crt0` placed, is 16-aligned; it was not before this change), the kernel's half by reading |
| L13 | `Makefile`, `shell/demos.c` | the kernel used the FPU itself (`funky`'s tempo was a float), with no thought for the program whose registers the FPU held | `funky` counts in integers, and the kernel is built with `-mno-80387` (a float in kernel code becomes a libgcc call, and the kernel does not link libgcc); a #NM in ring 0 panics ("FPU used by the kernel") |
| L14 | `user/libsurf/printf.c` | `printf` had no floating point (`docs/ROADMAP.md`'s open item), and `%f` printed itself without taking its argument, so the conversions after it read the wrong ones | `%f %F %e %E %g %G` with the flags, width and precision the integers take, and `L` (a long double, printed as the nearest double). A double is an integer times a power of two, so its digits come exactly from a number in base 10^9 (767 digits at most), rounded to nearest with a tie to even: what glibc prints, digit for digit, with integer arithmetic only (one difference, on C11's side: when `%#g` rounds up into exponent P, glibc 2.36 drops the zeros `#` keeps, `1.e+06`; libsurf prints `1.00000e+06`). **Verified** by `test_libc` (every conversion and flag against the host's printf over ties, carries, the extreme doubles, infinities and NaNs, and 4000 doubles of random bits) and `fputest printf` in the smoke test |
| L15 | `kernel/syscall.c`, `shell/main.c`, `shell/guisys.c` | `SYS_kill` refused only pids 0 and 1, and the `kill` command only idle and init, so `kill` on the usb, net, iwl or touchpad thread ended it at once (a kernel thread is never inside a call, where a kill waits for the call's end), holding whatever locks it held. The flags could not tell a process from a kernel thread: a process carries `TF_KTHREAD` too, and `TF_USER` only once its loading starts | `TF_PROCESS`, set by `process_spawn_ex()`; a user's kill (`task_user_may_kill()`, `task_kill_user()`) ends processes and ring-0 shells (init starts them again, and their commands are calls a kill waits for), and nothing else: `SYS_kill` says EPERM, `kill` and End task say "is a kernel thread". `tcpecho`, the kernel thread a user starts, stops with `tcpecho stop`. **Verified** by `selftest` (idle, init, the net thread and a thread of its own refused, a child process ended), the smoke test (`kill` of the net thread's pid; `tcpecho stop`, and port 7 free again) and `test_guisys` (End task: a process still being loaded, a ring-0 shell, kernel threads) |
| L16 | `kernel/gdt.c`, `kernel/interrupt.c`, `kernel/traps.asm`, `kernel/panic.c` | every IDT entry was an interrupt gate, so the double fault ran on the stack that had just failed: a fault whose frame could not be pushed (a kernel stack overflow, once stacks have guards) became a triple fault, a silent reset | vector 8 is a task gate to a second TSS (selector 0x30) with an 8 KB stack of its own, the kernel's page directory and interrupts off. `double_fault_task()` reads what the main TSS saved: when the fault's address (CR2) or the stack pointer is in a stack's guard it says "kernel stack overflow in 'name' (pid n)", otherwise "Double Fault", with the registers and a backtrace (repeated frames folded, so a recursion's report shows its start and fits on the screen), on the console shown and the serial line, and halts. **Verified** by `@boot crashstack` in the smoke test, and by qemu-usb's `crashstack` scenario (UEFI, the framebuffer console: on the screen, and QEMU halted rather than reset) |
| L17 | `kernel/task.c`, `include/mm/memory.h` | (M10) kernel stacks were 16 KB kernel-heap blocks with nothing below them but other blocks: an overflow wrote over them, silently, and a program's stack came out of the kernel heap | a window of their own (0x90000000-0x93FFFFFF): 32 KB slots, each a 16 KB stack mapped from frames at its top (a process's from `PMM_USER`, a kernel thread's from the kernel's) over 16 KB left unmapped; `kstack_alloc()` and `kstack_free()` from `task_alloc()` and `reap()`; `crashstack` overflows the stack of the task that runs it. **Verified** by `selftest` (its own stack and a new thread's mapped in full over an unmapped guard, the thread's unmapped once it is collected) and `crashstack` |
| L18 | `kernel/interrupt.c` | nothing noticed a kernel stack that was all but used up until it overflowed | `trap_dispatch()` halts the machine when a trap frame lands within 1 KB (`KSTACK_LOW`) of the running task's stack bottom: "the kernel stack of 'name' (pid n) is nearly full", reported from the double fault's task (`int $8`), since the report would not fit in what is left. **Verified** by `@boot crashstack near` (a trap 512 bytes above the bottom) |
| L20 | `driver/amlfield.c` | nothing read the sleep states' values, so SurfOS could not turn the machine off | `aml_sleep_type()`: `\_S5_`'s package from a copy of the DSDT, through the field reader's bounded walk: the Name at the root, declared once, outside any If, Else or While, its elements integer constants (one element: its two bytes, as ACPICA reads it). Never guessed (Intel's is usually 7). **Verified** by `test_amlfield` (OVMF's bytes and Intel's, a root prefix, Scope (\\), one element, a short package; refused: another scope, inside If, twice, a name for an element, an Alias, a Method called `_S5_`; every truncation and 5000 mutations) |
| L21 | `kernel/acpi.c` | the FADT's sleep registers were not read, and the legacy profile read no ACPI at all; the BIOS data area was read at its address, unmapped after boot | the PM1a and PM1b control blocks (their extended forms when those name ports), SMI_CMD and ACPI_ENABLE, the sleep control register of hardware-reduced ACPI. `acpi_s5_enter()`: interrupts off, caches written back, ACPI mode through SMI_CMD when SCI_EN is clear (300 ms at most), SLP_TYP and then SLP_TYP with SLP_EN into PM1a and PM1b (or the sleep control register), and failure after 3 s. The legacy profile reads the tables quietly when poweroff first needs them, the BIOS data area through the firmware window. The review of B4: an SLP_TYP above 7 is refused, never cut to 3 bits (0x0D would be 5, S3 on Intel's), and a sleep control register or reset register in I/O space must be below 0x10000 (0x10060 was written as port 0x60). **Verified** by `test_acpidump` (the FADT's registers, \\_S5_ read from a DSDT, out of range refused, the port limit), the smoke test (`poweroff -n`'s values; poweroff stops QEMU in the legacy profile and with profile=laptop) and qemu-usb's `shutdown` (under OVMF, QEMU ends) |
| L21b | `kernel/acpi.c` | (found by the review of B4) nothing cleared the PM1 and GPE status before SLP_EN, unlike ACPICA, which disables the GPEs and clears WAK_STS, PWRBTN_STS and the GPE status bits first: some chipsets wake straight up again, or ignore the sleep | before the S5 writes, as ACPICA's `AcpiHwLegacySleep()` does (`acpi_s5_clear()`): each enable byte of GPE0_BLK (FADT 80, X_ 220) and GPE1_BLK (84, X_ 232) zeroed (the second half of GPE0_BLK_LEN or GPE1_BLK_LEN, 92 and 93), the PM1 status cleared with the fixed status bits 0xC731, WAK_STS among them, at PM1a_EVT_BLK (56, X_ 148) and PM1b_EVT_BLK (60, X_ 160) when PM1_EVT_LEN (88) is at least 4, then each GPE status byte (0xFF); hardware-reduced ACPI: WAK_STS in the sleep status register (256), as `AcpiHwExtendedSleep()` does. A block is used only when it is an I/O port with all of it below 0x10000, a GPE length even; the wakeup GPEs ACPICA enables next are unknown without running the DSDT, so only the power button wakes the machine. `poweroff -n` says what it clears. **Verified** by test_acpidump (the order and values for an Intel PCH's blocks, QEMU's PIIX4, PM1b and GPE1; the extended forms; blocks past 0xFFFF, an odd length and PM1_EVT_LEN under 4 refused; hardware-reduced ACPI at a port, in memory and past 0xFFFF), the smoke test's `poweroff -n` (PIIX4: GPE0 at 0xafe0, PM1 at 0x600) and qemu-usb's `shutdown` (ICH9: GPE0 at 0x620; poweroff still ends QEMU through S5); whether the laptop stays off is the owner's check |
| L22 | new `kernel/shutdown.c`, `kernel/task.c`, `kernel/process.c`, `kernel/netman.c` | `reboot` reset the machine at once: what programs had not written was lost, and a volume mounted read-write stayed marked not cleanly unmounted (B42 then keeps it so until it is checked) | the shutdown thread (ARCHITECTURE.md section 5): init starts no shell and a spawn says EAGAIN; the hooks (`shutdown_hook()`), which may still write the stick; every process and ring-0 shell ended with the kill that waits for its call (`kill_task_patient()`), again on each pass of the wait so that one a spawn under way makes meanwhile goes too, 10 s at most, any left named; then the network manager stopped at the stick (`shutdown_unmounting()`, `netman_stick_idle()`) and the read-write volumes, listed however many there are, written out and unmounted in a worker waited for 15 s at most, a busy one tried again; one left mounted is written out and stays marked; then the reset or S5, else "It is now safe to turn off your computer." The review of B4: a task made after a single kill pass escaped it, a spawn refused with EAGAIN made init start a recovery console, the stick was shut to the hooks (A10's history could not be written), mounts past the 16th were neither unmounted nor synced, and `event_wait_timeout()` could miss an event set between its look and its wait (a worker's end missed: 15 s lost and a clean unmount reported unclean); all fixed. **Verified** by three smoke boots on one stick (reboot, poweroff, Ctrl+Alt+Del: each next boot finds it cleanly unmounted; FAT[1], the files and fsck.fat after the last) and qemu-usb's `shutdown` (the next mount trusts FSInfo; fsck_msdos and fsck.fat after poweroff) |
| L23 | `kernel/keyboard.c`, `kernel/task.c` | Ctrl+Alt+Del reset the machine inside the keyboard's interrupt handler, spinning its delays there, with nothing written out | it wakes init (`task_wake_init()`: TI_INT cuts init's wait short), which starts the shutdown, a reboot; pressed again it resets at once. A key held down is one press (`del_key_event()`: a make after a break; the review of B4 found the keyboard's repeats counted, so a press held half a second reset the machine in the middle of the unmounts), and during a shutdown begun otherwise the first press only says so. **Verified** by the smoke test (the monitor's `sendkey ctrl-alt-delete`) and `selftest` (make, repeats, break, make; Delete alone repeats) |
| L24 | `shell/main.c`, `tools/devloop.py` | there was no way to switch the machine off | `reboot [-f]` (the shutdown; `-f` resets at once, as reboot did), `poweroff [-n]` (the shutdown, then S5; `-n` a dry run: the programs, the mounts, the S5 values). `tools/devloop.py run` refuses poweroff, which would end the loop, but not `poweroff -n`. **Verified** by the smoke test and `test_devloop` |

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
| C9 | `kernel/console.c:26` | Console count is fixed at 3 (`NUM_CONSOLES`); the 2004 TODO notes odd output with more. No scrollback (**closed** 10/25/26, track A, A6: F1-F4 on a framebuffer keep what scrolls off, Shift+PgUp shows it) |

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
