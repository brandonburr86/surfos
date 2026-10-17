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
| B1 | `fs/bcache.c` (`reuse()`, `bsync()`) | A dirty block that had to make room and could not be written was dropped ("lost, and said so"): the next sync found nothing wrong, so the command that wrote never heard of it, and a dropped FAT or directory block let FAT read its old copy back and hand out clusters a file already had. `bsync()` sent a failed run again for each of its blocks (up to 128 tries and console lines in one sync) and returned a failure even when a later try had written everything | the block stays dirty while its device is there and is passed over when room is needed; when nothing else is free one refused run is tried again, else the caller gets an I/O error (replaced by B6: a refusing device keeps a quarter of the cache, and its new writes fail); `bsync()` tries each run once and fails when one did; the console hears of a device's first failure and of its first good write after |
| B2 | `shell/fscmd.c` (`cmd_eject()`) | `eject` unmounted a disk whose writes failed, which dropped the blocks it could not write, and said "safe to remove" | each mount is written out first; a disk that refuses keeps its mounts, and eject says so ("sync tries the writes again; umount gives them up") |
| B3 | `fs/bcache.c`, `fs/fat.c` | Dirty blocks reached the disk in the order of the buffer table: a card pulled during a sync could keep a new file's directory entry while its cluster was still free in the FAT, or the entry and the FAT without the file's bytes (a file reading another's old data); unlink freed the clusters before the entry was gone | a write order (file data, FAT, directory entries, FSInfo: `bdirty_order()`), kept by `bsync()` and when room is made; FAT's order points (a sync) take an entry away before its clusters are freed and put a new name on the disk before the old one goes. It left holes, closed by B5-B8 and B11 (the order is now checked at every write); it is an order, not atomicity, and what FAT32 can still leave behind is in `surfcpu/docs/CARD.md` section 8 and `fs/fat.c`'s header |
| B4 | `fs/fat.c` (`run_blocks()`, `fat_rw()`) | A write allocates a run of clusters ahead of its data; when the volume ran out in the middle of a run (or a block could not be written) the clusters already linked stayed in the file's chain past its size, and the write returned ENOSPC without the bytes that fitted: fsck.fat found the chain longer than the file and the free count low (**verified** under QEMU with a nearly full stick, `boards/qemu_virt/test_os.py`) | a run ends where the volume does, so a write puts in what fits and returns its bytes (ENOSPC on the next); after a write that stopped early the chain is cut back to the file's size. `cp` (both shells) says why a write came up short |

Found by a review of B1-B4 (October 16, 2026) and fixed; every one is **verified**: B10 by
`selftest` (a RAM stick that refuses writes, in the smoke test), the others by
`tests/host/test_fatfs.c` (`make unittest`: the real `fs/bcache.c`, `fs/fat.c` and `fs/fat_mkfs.c`
over RAM disks that refuse writes on request, with a check of the image after every write the disk
stores), which fails on the code before the fixes (`surfcpu/docs/evidence/os/bcache-review.txt`):

| ID | Where | Problem | Fix |
|---|---|---|---|
| B5 | `fs/fat.c` (`fat_rename()`, `fat_unlink()`, `fat_truncate()`) | B3's order points (a sync) fail when the device refuses a write, and the operation stopped there with its first half done in the cache: rename left both names on one cluster chain, and the same mv again (replacing the stray name) or an rm of either name then freed the chain under the other, whose file then read the next file's bytes; unlink and truncate to zero said EIO but had taken the name or the size, and lost the clusters | what the operation changed before a failed order point is put back (the slots' first bytes, the short entry, the node's first cluster and size) and it fails with EIO: nothing changed, and the same command again is safe |
| B6 | `fs/bcache.c`, `fs/fat.c` | The failed blocks B1 keeps were never reused and the cache is shared: a device that refused every write filled it (about 512 KB of writes), and then every read of every device failed ("every buffer is busy"), the initrd's included, until a sync or umount; each such miss also sent one more write bound to fail. A block held back because something below it failed was marked failed without being tried; a failed run was tried again only whole, so one bad block held back its 127 neighbours for good; read-ahead stopped at the first dirty buffer of the old end of the list, so one failed block turned it off for every device | a device that refuses (one of its blocks failed) has its dirty blocks held back: making room tries none of them, and no block is marked failed that was not tried; a device keeps at most a quarter of the cache dirty (one that works writes some out first; one that refuses gets EIO for new writes: `bcache_room()`, `bget()`; since B14 it is asked again first, B15 says what the share counts); a sync tries a failed run again block by block, once (B13: in every sync); read-ahead looks past dirty buffers |
| B7 | `fs/bcache.c`, `fs/fat.c` | B3's order was kept by the callers that sorted blocks, not where writes are issued: blocks dirtied by another task after `bsync()` had passed their order (a close's sync while another task wrote) went out behind the higher orders that pointed at them, and so did those of a block made dirty while making room wrote below it; a block could also change while it was being written. Within the FAT order, sectors went out in no order: a link into a new cluster in another sector could reach the disk before the cluster's own end-of-chain (an append, a directory that grew: its chain then ran into a free cluster, whose next owner's bytes it would show as entries), and a truncate's frees before its cut; a directory's new cluster carried the data order of its zeros, so entries written into it went out ahead of what they named | `write_run()` sends a block of order k only when its device has no dirty block below k (the device's account, under the lock, just before the request), for every caller (B12: none with a change older than the block's last); `bsync()` goes round again when blocks below are dirtied meanwhile (at most 8 times, then 0: replaced by B12); a held block is not written and a block being written is not handed out. FAT: one more order point where a link crosses into another FAT sector or grows a directory, after `mkdir`'s new cluster, and between a cut and its frees (truncate; `trim_chain()`) |
| B8 | `fs/bcache.c` (`evict_write()`) | Making room wrote a block's lower orders first, which let the lock go, then wrote the block without looking at it again: written or dropped meanwhile, it went out as a zero-length request (and "takes writes again" was said of a device that took nothing); taken for another device's block, that block was written without its own device's order | the block is written only if it is still the same dirty block; `write_run()` sends no empty request |
| B9 | `fs/bcache.c`, `include/sys/bdev.h` | The console's "write of ... failed" was said once per device through a table of eight devices that only a write cleared: a device that failed and was then unplugged kept its slot for ever, and after eight such devices every failure was printed and "takes writes again" never was | each device carries the cache's account of it (`bc_dirty[]` by order, `bc_failed`, `bc_said`; since B12 also its dirty blocks listed by first change), kept at every change of a block's state and gone with the device; `bcache_audit()` checks the account (and the lists) against the blocks |
| B10 | `fs/vfs.c` (`vfs_close()`, `vfs_umount()`), `shell/fscmd.c`, `user/surfsh/run.c` | The last close of a written file synced it and returned 0 whatever happened, so `cp` (kernel and user) and `edit` said a file was saved that only the cache held (B1 made `bsync()` say -1, but nobody heard it); `umount` gave up what could not be written without a word, and eject's unmount could give up blocks written after its check and still say "safe to remove"; surfsh closed a `> file` with `dup2()` or let the program's exit close it, so its failure was lost too | close returns the sync's error; umount and eject say how many changed blocks they gave up (`vfs_umount_lost()`) and fail, and eject then does not say "safe to remove"; surfsh closes a redirected file itself after the command and fails the command when that fails; `selftest` checks the close and the umount on a RAM stick that refuses writes |
| B11 | `fs/fat.c` (`fat_set()`, `trim_chain()`) | Two gaps of B4: `fat_set()` changed the first FAT and then gave up when the second copy's block could not be read, so the FATs differed (and a cut or a link stood in one copy only); `trim_chain()` set the file's first cluster to none before freeing the chain, so a chain it could not free was lost to the file as well | every copy's block is in hand before any changes: both FATs change, or neither; the first cluster is let go once the chain's head is free |

Found by a review of B5-B11 (October 16, 2026, round fs-fix-2: nine scenarios, a randomized test and a threaded
one, on d7452f4) and fixed; every one is **verified** by `tests/host/test_fatfs.c` (the review's scenarios R1, R2,
R2b, R4, R5, R6, R8, R9 under the IDs below, and new ones for each fix), each scenario failing on the code before
the fixes. The review's randomized test is in `make unittest` as `tests/host/test_fatfuzz.c` (on d7452f4 five of its
six runs fail: a short write that changed more than it returned, B15) and its threaded one as
`tests/host/test_fatconc.c` (it found nothing on d7452f4, nor on the fixes in this round's settings: kept as the guard
against waits that never end and for the claims under real threads; the next review found B25 with it in a new
setting). `surfcpu/docs/evidence/os/bcache-review.txt` section 5 has the numbers:

| ID | Where | Problem | Fix |
|---|---|---|---|
| B12 | `fs/bcache.c` (`bsync_order()`, `write_run()`, `evict_write()`) | B7 held a block back while its device had any dirty block of a lower order, and `bsync()` went round at most 8 times and then returned 0 whatever was left: another task that kept writing file data held a directory block back for good, so a close said a file was on the card when its entry was not (and an unlink's order point could say 0 with the entry still on the card, its clusters freed after it). Each block passed over while making room cost a walk of the whole cache per lower order, twice | every change has a number; a block records its first unwritten change and its last, and each device lists its dirty blocks of each order by first change. A block goes when no lower-order block of its device has an older unwritten change. `bsync()` owes the blocks dirty when it began and returns 0 only when all of them reached the device; blocks other tasks dirty meanwhile are written when they may be, never waited for; after two rounds, what others keep changing goes out as a copy (taken while nobody holds the block, written after every older lower-order change; the block stays open to changes). Making room writes what a block needs from the lists, oldest first |
| B13 | `fs/bcache.c` (`bsync_order()`, `write_singly()`) | After one failure a sync tried nothing more of the device (B6), tried a failed run again block by block only when it had several blocks, and stopped that after two failures: one bad sector kept every block dirtied after the first failed sync off the card, two at the head of a run kept the rest of it off; a block `write_singly()` could not try (RUN_BLOCKED) counted as written, so a sync whose FAT sector refused could return 0 | a sync tries every block it may (those that never failed first), and a failed run of several blocks again block by block, in every sync; RUN_BLOCKED is "not written"; a device that refused 4 requests in one sync and took none is asked nothing more in it |
| B14 | `fs/bcache.c` (`room()`) | With a failed block and a quarter of the cache dirty, `bcache_room()` said no without asking the device again: every write to a card that worked again failed with EIO until something synced, and FAT's order points for a growing file sync only data and tables, so a failed directory block was never tried there (a 1 MiB write stopped at 126 KiB) | `room()` first writes the device's oldest block of its lowest order, alone: one request per call; when it works, `room()` goes on as for a device that works |
| B15 | `fs/bcache.c` (`write_blocks()`), `fs/fat.c` | The straight path put a run's bytes into the cached copies (dirty) before the device had them: after a refused 64 KB write the file read the refused bytes back, the next sync that worked wrote them, and a refusing card held 379 dirty blocks against a share of 256; `fs/fat.c` said a refused write changes nothing | the copies are kept out of other writes while the device works and take the bytes once it has them: a refused write changes nothing, in the cache either (a write that returns an error or fewer bytes changed only those it returned; not exact, B22: a device may store part of a refused request). The share stated exactly: file data never takes a refusing device past it; an operation's own table, directory and FSInfo blocks are not counted; there is no share across devices |
| B16 | `fs/fat.c` (`fat_unlink()`, `fat_truncate()`, `fat_rename()`, `fat_free_chain()`), `fs/bcache.c` (`baside()`, `bdiscard()`) | With a data block of a file refusing, the order point that takes the file's entry off the card waits for that older block (B12, as B7 did), so rm of the file, a truncate past the block and a mv over the file said EIO for as long as it refused | the failed blocks of the clusters about to be freed are set aside before the order point (not written, nobody's dependency, no sync's debt), put back when it fails, dropped once the clusters are free; `fat_free_chain()` drops the failed blocks of whatever it frees. Other refusing blocks still hold an order point back: rm, mv, truncate and mkdir of other files fail with EIO meanwhile. (Corrected by the next review: B20, the mv over the file set them aside too early, and it fails with EIO now while they refuse; B21, only 16 were looked at, and a looping chain overran the array; B26, another task's sync owes them) |
| B17 | `fs/fat.c` (`fat_rename()`, `fat_create()`) | A mv over an existing file freed that file before deleting the old name, so an old name that could not be deleted then (its directory's block unreadable) failed the mv with the replaced file gone; a directory moved to another parent had its `..` pointed after the rename was done, and a read error there said EIO for a rename that had happened; create read the new entry back for its vnode, and a read error there said EIO for a file it had made | rename points `..` first and deletes the old name before the replaced one, each undone when a later step fails: nothing fails once the replaced file's clusters go; create makes the vnode from the entry it wrote (when that fails, ENOMEM, the entry goes again) |
| B18 | `fs/bcache.c` (`wait_for()`) | `bsync()` waited for ever for a block someone held: a task killed while it held a dirty buffer (`kill_escalate()`: still in a call 2 s after a kill, killed at once with what it holds) left every later sync of that device hanging | a sync waits 10 s for a hold of a block it owes, counted from when a sync first had to wait for a hold of that device (every hold is stamped then: a dead task's several holds cost one wait, and taking a block reads no clock), then gives up on it (not written: -1, said on the console); later syncs give up on that hold at once, until the block is let go |
| B19 | `fs/bcache.c`, `include/fs/bcache.h`, `fs/fat.c` (`trim_chain()`) | Small things the review found: `bpeek()` hands out a block that is being written, which its header did not say; `trim_chain()` also let go of a chain whose head's FAT entry could not be read; B6's header said the other devices always have the rest of the cache, which no share across devices makes true | the header says `bpeek()` is for readers; trim lets go only of a head it sees free; the headers say what the share is |

Found by a review of B12-B19 (October 16, 2026, round fs-fix-3: six scenarios S1-S6, the threaded test in new
settings, AddressSanitizer, on fd37406; it found B12's core sound: no copy torn, the lists consistent, no lock
inversion) and fixed; every one is **verified** by `tests/host/test_fatfs.c` (S1-S6 under the IDs below, and new
scenarios for B25 and B26), each scenario failing on the code before the fixes; S6 under AddressSanitizer shows the
overflow before and nothing after, and every host test of the cache and FAT runs clean under AddressSanitizer and
UndefinedBehaviorSanitizer. `make unittest` runs the threaded test in the review's setting too, its threads now tasks
of their own to the cache (so B26's waits run there).
`surfcpu/docs/evidence/os/bcache-review.txt` section 11 has the numbers:

| ID | Where | Problem | Fix |
|---|---|---|---|
| B20 | `fs/fat.c` (`fat_rename()`) | A regression of B16: rename set the replaced file's refused blocks aside before its first order point, which writes the new name; when the name went into the directory block that holds the replaced file's entry, that entry reached the card with a size covering the refused block, whose cluster held another file's old bytes (mv a b, b appended into such a cluster: the mv said 0 with that on the card in between); a mv that failed later (its old name's block unreadable, or the card refusing the directory block at the second order point) left it so, with the moved file's chain named twice (the B5 hazard) | the replaced file's failed blocks are set aside only for the second order point, once its entry is deleted in the cache: the first one owes them, so a mv over a file whose block refuses fails with EIO and changes nothing while it refuses (rm of that file works, then the mv). unlink and truncate already set aside only after the entry is deleted, emptied or cut back in the cache |
| B21 | `fs/fat.c` (`aside_failed()`, `fat_free_chain()`), `fs/bcache.c` (`bfailed_blocks()`) | B16 looked at the device's first 16 failed blocks only (rm of a file with 20 refused blocks said EIO for as long as they refused) and gathered a chain's into a 16-entry array on the stack, checking the count only between clusters: on a damaged volume, a chain looping through a cluster with several failed blocks wrote past it (AddressSanitizer: stack-buffer-overflow in `aside_failed()` from `fat_unlink()`), and `aside_end()` read past it; `fat_free_chain()` counted a looping cluster free twice | every failed block of the device is looked at (`bfailed_blocks()` says how many; a sorted kalloc'd list, a binary search per cluster), each is set aside once, the walks end after as many clusters as the volume has, `fat_free_chain()` stops at a cluster that is free already |
| B22 | `fs/bcache.c` (`write_blocks()`) | B15 took a refused request to have stored nothing, but `sdcard.c`'s CMD25 stores the blocks before the one it refuses, USB mass storage its 32 KB pieces and ATA its sectors one by one; the cache kept its clean copies of the run, so after a refused 64 KB write the file read unchanged until the cache made room, then 64 of its blocks read new, with no write in between | the clean copies of a refused run are dropped (one a reader holds is taken out of the table and dropped when let go); dirty ones keep their bytes. What is read of the clean ones is what the device holds (worded so since the next review: a dirty one keeps the cache's bytes, which the next sync writes over the device's); the claim made exact: a write that returns an error or a short count may also have changed, on the device, the file's bytes in the request it stopped in (up to 64 KiB past what it returned, never past the file's size) |
| B23 | `fs/fat.c` (`grow_zeros()`, `fat_rw()`, `fat_truncate()`) | A write past the end whose gap of zeros the card refused part of said ENOSPC and left the file grown by the zeros written (1 byte at 300000 of a 512-byte file: -28, size 66048); one whose gap was written but none of its bytes said EIO with the file grown by the gap | the gap is all or nothing and goes with a write that wrote nothing: the size stays, the error is what stopped it (EIO; ENOSPC on a full volume), and the chain is cut back; a truncate that grows likewise |
| B24 | `fs/fat.c` (`slots_restore()`, `entry_put()`, the undo paths of rename, unlink, truncate and create) | B5's and B17's undo was best effort and silent: mv s/a t/a with s's block unreadable, and t's block unreadable during the undo, said EIO, and once synced the card named one chain s/a and t/a (an rm of either then frees it under the other) | each undo step says when it could not be done; then the volume refuses every change (EROFS: rm and mv too) until it is unmounted, says so once on the console, and stays marked as not cleanly unmounted (to be checked before it is written again) |
| B25 | `fs/fat.c` (`trim_chain()`, `fat_sync()`, `fat_unmount()`) | B7's documented window, seen: a write cut back while the card refused the order point between the cut and the frees freed the clusters past the cut all the same; the next sync wrote the FAT sectors in their own order, and a card pulled between held a chain running into a freed cluster (the threaded test with six writers on a small volume: 2 of 9 runs; `surfcpu/docs/CARD.md` said it finds none) | the clusters past the cut wait (up to 8 such chains; more stay lost until fsck) until a sync begun after the cut works (an order point, `fat_sync()`, unmount): the cut reaches the card before their frees. Their failed blocks are dropped first (past the end of the file). A deterministic scenario puts the frees' FAT sector ahead of the cut's (Corrected by the next review: B28, a block of the part cut off that failed in that order point stayed owed; B31, the ninth chain was lost without a word) |
| B26 | `fs/bcache.c` (`tally()`, `owed()`, `wait_for()`), `include/sys/bdev.h` | B13's count of a device's refused requests was kept in the device for the sync call that counted last: making room or another task's sync in between restarted it, and a sync asked a card that takes nothing for every block (liveness only). B16's blocks set aside were no sync's debt: another task's sync could return 0 while an rm's order point had one aside, and the rm then failed and put it back, not written | each sync call keeps its own count (making room counts none: it asks once); a block set aside is the debt of every other task's sync (the device records whose operation set it aside), which waits for it as for a hold (B18's 10 s) and does not write it while it is aside (B32: blocks are set aside for their order point only; a sync of every device counts the first 8 devices it meets, the others asked with no early stop) |

Found by a review of B20-B26 (October 16, 2026, round fs-fix-4: scenarios R1-R6 and the randomized and threaded tests
with a census of lost clusters, on 15f0f3c; it confirmed B20-B24 and B25's frees, and B26 free of deadlock, in 1.4 M
operations clean under AddressSanitizer and UndefinedBehaviorSanitizer) and fixed; every one is **verified** by
`tests/host/test_fatfs.c` (R1-R5 under the IDs below, R6 as B22b, and scenarios of the fixes' own: B28c, B28d, B30b,
B31b, B32), each failing on the code before the fixes but B22b (B22 held: the wording is now exact) and B30b (a mkdir
that wrote nothing frees its cluster at once, as it did). The review's R3 also checked the documented loss of the ninth
chain; nothing is lost there now. `surfcpu/docs/evidence/os/bcache-review.txt` section 18 has the numbers:

| ID | Where | Problem | Fix |
|---|---|---|---|
| B27 | `fs/fat.c` (`fat_truncate()`) | **Critical**, from B5 (B24 rewrote the block): a truncate to a smaller, non-zero size whose `entry_get()` could not read the directory block put `raw[]` back in its undo anyway: 32 uninitialized bytes of the stack over the file's directory entry once the block read again (one passing read error: the file renamed to junk and its clusters lost; zeros there ended the directory at that entry, and every name after it vanished) | the saved entry starts zeroed and is put back only when `update_entry()` changed the entry (an explicit flag); every other undo checked: the slot undos record only slots they changed, and a rename puts `..` back only when `set_dotdot()` changed it |
| B28 | `fs/fat.c` (`trim_chain()`, `fat_free_chain()`, `aside_collect()`), `fs/bcache.c` (`bdirty_blocks()`) | A regression of B23 and B25 against B16: blocks of a part cut off that failed in the cut's order point stayed dirty while its chain waited to be freed, owed by every later sync: one refusing sector made every order point and sync fail for as long as it refused, and no rm could help (the blocks were in no file's chain); a write past the end that wrote nothing gave up its gap of dirty, never-tried zeros that way, and a chain freed at once (a file that had none) left its untried blocks to fail later. B16 also set aside only blocks that had failed: an rm whose own never-tried block refused in its order point said EIO once | every dirty block of a part cut off or given back is dropped before its order point, again when that fails, and before each later order point and sync while the chain waits (`drop_pending()`); a chain about to lose its last reference has all its dirty blocks set aside for that order point, failed or not (it neither writes nor waits for them); `fat_free_chain()` drops every dirty block of the clusters it frees. B16's guarantee, exactly: once the operation that gives a chain up has run, no sync owes a block of it, but for one a task held or was writing at that moment, which is written as any other (an rm whose order point the device refuses it in says EIO, nothing changed, and the next rm works); one of a waiting chain is dropped by the next order point or sync |
| B29 | `fs/fat.c` (`fat_truncate()`, `fat_rename()`) | B24 made the volume read-only when an undo's put-back could not read its block even when the step it undid (an `update_entry()` that could not read the same block) had changed nothing | an undo is tried, and can make the volume read-only, only for a change that was made |
| B30 | `fs/fat.c` (`fat_create()`) | A mkdir that failed after writing its entry (ENOMEM) put the slot back and freed the new directory's cluster with no order point between; when another task's sync had written the entry meanwhile, the next sync wrote the FAT's free first (tables go before entries) and the card named a free cluster | when something of the name was written, the cluster waits with B25's chains (its blocks dropped): freed once a sync begun after the slot was put back has worked; a mkdir that wrote nothing of its name frees it at once, as before |
| B31 | `fs/fat.c` (`pend_chain()`, `fat_unmount()`) | B25 kept 8 chains to free later and lost the ninth without a word (16 clusters lost until fsck) | 64 can wait; one more is said lost on the console (its blocks dropped all the same), and the unmount says how many clusters may be left so (allocated, nothing pointing at them, fsck.fat frees them) |
| B32 | `fs/fat.c` (`aside_collect()`, `aside_begin()`, `aside_end()`) | Blocks set aside stayed so from the walk to the end of the chain's free, and another task's sync owes them (B26) and waits 10 s at most: an rm of a long chain whose failed block was near its end kept them aside longer, and the other sync gave up ("an operation that does not end") | collected first, set aside right before the order point, dropped or put back right after it |


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
