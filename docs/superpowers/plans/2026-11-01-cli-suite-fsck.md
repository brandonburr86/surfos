# The command-line suite: track fsck

The account of `cli/fsck`, for the integrator and the review. The brief is `2026-11-01-cli-suite.md`.

**`fsck` only reads. `fsck -r` is refused in this version,** however it is written, with status 2 and one line: `fsck: repairs are not available in this version, and the stick can be repaired on another computer`. Two second reviews of the fix round's tip found that the repairs could still lose data, and the rule set before that round was that `-r` then ships disabled. Nothing in the kernel reaches the repair code: the command gives the checker no write function and never asks it to repair. That code stays in the tree, built and tested on the host, for a later round the owner may ask for. What that round must fix is under **Why `-r` is off** below.

**It runs on nothing but QEMU so far. None of it has run on the laptop or on a real stick.** A pull here is a test harness cutting off a RAM disk, or QEMU taking an emulated stick off an emulated port.

**Commit hashes.** The read-only round rewrote the branch's history (below). Hashes in this account from before the rewrite (0df671a, aef87bf and the rest) are the ones the reviews used; the table in that section maps each to its new hash.

## The read-only round (11/02/26), in one paragraph

The reviews of aef87bf found the look sound and the repairs not. So `-r` is off, and this round made the look fit to ship alone: a check's memory is capped, and a volume that needs more is refused before anything is taken; netman's short mount on `/.netman` is never held; names a look could not compare make it say so, and never `clean`; bidirectional controls are cleaned from names; a look in the kernel says nothing of a repair it cannot make. The page says all of it. Then the branch's history was rewritten so that the early `short_name_bad()`, which was close to dosfstools', is in none of its commits.

## The last round (after the third review), in one paragraph

The third review found `-r` unable to write (18 spellings through the real `cmd_fsck()` and `fat_check()`, all status 2 and no read of the device) and netman's mount never left held, and three findings more and a few nits, all fixed here. The 4 MB cap bounded what a check held at once, not how far it made the kernel heap grow: the table of names doubled, and under first fit its old copies could not be taken again, so the owner's stick with 32769 names in its root grew the heap by 4548 KB. Now a look takes everything at its start, the table of names once at the size the volume can need, and the cap bounds the growth, proved over `mm/kalloc.c` itself. A backup boot sector that could not be read ended `clean`: a block a look cannot read is now never `clean`, each of them tried in turn. Ctrl+C in a look ended with advice to have the stick repaired, status 1: now it is 130, as for any command, with no advice. Five more invisible characters are cleaned from names, no memory to look at the mounts is status 2, and the page says that a look at the unmounted boot stick holds up a save of the settings. No history was rewritten in this round.

## Why `-r` is off: what a later round must fix

The reviews' findings, as a list for that round. Line numbers are of `fs/fat_check.c` as the reviews read it at aef87bf (the rewrite kept that file's text), with the functions named, since the lines have moved since.

1. **FOUND's directory can take bytes only the second FAT still reached.** `repair_step()` (:1447) copies the first FAT over the second before `fix_lost()` (:1323) runs, and `take_free()` (:1264-1271) then gives FOUND.nnn the lowest clusters the first FAT calls free. Clusters that only the second FAT still reached are written over: about 1% of the reviewers' fuzzed repairs. A new file whose FAT sector was lost loses its second cluster without a word (their T7b). This needs no lying flush: macOS writes the second FAT ahead of the first (their clone 37), so a pull in a Mac's write leaves exactly that.
2. **A device whose bytes change between two reads.** `root_slot()` (:1304-1318) and `frame_next()` (:880-884) read the root's chain again at repair time, and FOUND.000 was written into another file's cluster.
3. **The FAT copy trusts the first FAT as it reads at that moment.** `fix_sync()` (:1385-1393) copies whatever the first FAT holds then, without `fat_scan()`'s bar on a blank sector, so a sector erased after the look wiped the second FAT's good copy.
4. **A directory that lost its directory bit and gained a size passes every bar** (:814: only a size of 0 makes it a doubt).
5. **A FAT32 root with `.` and `..` and a zeroed backup field** (BPB_BkBootSec, so there is no backup to compare the root's cluster with): the real root is filed as a `.CHK` (:909 only reports a root's dots; :1567).
6. **The plan leaves things out:** sizes set after a cut, FSInfo written again, and a clean mark that will not happen after all (`plan_words()` :1620-1628, `repair_finish()` :1650-1652).
7. **A save of lost chains that fails still leaves the volume marked unclean** (`run()` :1699-1702 marks it first; `fix_lost()` :1327-1339 then finds no room, no slot or no memory).

The reviewers' fixes: **give lost chains root slots instead of allocating** (items 1 and 7: no cluster is taken, and nothing needs room); **take only clusters free in both FATs as first read** (item 1, wherever something is still allocated); **compare every value a repair uses against what the look checked** (items 2 and 3: the root's chain and slot, each FAT sector before it is copied, each entry before it is written). Items 4 and 5 need bars of their own, item 6 a plan made from the same list the repair runs, and item 7, if lost chains still get a directory, its room, slot and memory found before the mark is written.

## What changed for a look

| Fix | Where | Proof |
|---|---|---|
| **`-r` refused**, before anything is looked at: status 2 and the one line above. A look that finds problems ends with `repairs are not available in this version: the stick can be repaired on another computer` | `shell/fsckcmd.c` | `test_fsckcmd`: six spellings of `-r` (`-r`, `-vr`, after the device, with `-v` apart, alone, with no such device), each refused with exactly that line, no event, no look at the mounts. The smoke block, and the USB scenario on a damaged stick |
| **The heap's growth capped at 4 MB** (the last round; before it, only what a check held at once). A look takes everything at its start, one piece each: the three bitmaps, the FAT's windows, small tables, and the table of names once, for the largest directory the volume can hold (`names_size()`), halved as far as it must be to fit. What the pieces take with their headers (`tables_fixed()`), and what the heap may leave over above them (`CHECK_HEAP_SLACK`), are counted before anything is taken; past `CHECK_MEM_MOST` the look says `a volume of N clusters takes K KB to check, more than the 4096 KB a check may have: check it on another computer`, status 2 | `fs/fat_check.c` | `tests/host/test_fsckheap.c`, new: the look over `mm/kalloc.c` itself, each on a new heap in a child process. The third review's volumes: the owner's 118 GB stick with 10, 32769 and 65534 names in its root grows the heap by 3276 KB (4548 KB before), 275 million blocks with 40000 names by 3432 KB (4708 KB before); a second look on the same heap by nothing; the largest volume that fits by 3964 KB, and one a cluster larger is refused. `deep_and_wide()`: 80 levels and 700 names take as many pieces as the volume they were made on. `names_at_cap()`: the edges, by search. `big_refused()`: 118 GB with 512-byte clusters refused, having taken under 16 KB |
| **netman's mount never held.** The mounts are looked at in a copy of the table (`vfs_mount_views()`); while the device is on `/.netman` the command waits, 200 ms at a time for five seconds, holding nothing, then says `the network settings have usb1p1 mounted on /.netman for a moment: try again`, status 2. A hold that catches `/.netman` made in the moment after the look lets it go at once. Ctrl+C ends the wait | `shell/fsckcmd.c` | `test_fsckcmd`: gone after three looks, made for a write, staying (25 waits, nothing read), the race, no memory, Ctrl+C. The USB scenario, through the real kernel: the stick mounted on `/.netman` by hand, the look gives up after the wait, `umount /.netman` then succeeds at once, and Ctrl+C in the wait |
| **Names not compared make a look not whole.** When the table of names is full (since the last round: when the cap made it smaller than the volume needs), the look counts that directory and says `not checked whole: N directories held more names than a check may compare`; with nothing else found it ends with status 2, never `clean` | `fs/fat_check.c` | `deep_and_wide()`: every allocation refused in turn; wherever it was the table's, the look is not clean and says why. `names_at_cap()`. The reviewers' `hash_collide.img` (65534 names in one directory) under heaps of 1200, 1600 and 2000 KB: `not checked whole` each time |
| **Bidirectional controls cleaned.** U+202A-202E and U+2066-2069 come out as `?`, as other controls do; since the last round U+061C, U+200E, U+200F, U+2028 and U+2029 too | `fs/fat_check.c` | `names_said()`: a long name `a`, U+202E, `b`, U+2066, `c` is said as `/a?b?c`, and `d`, U+061C, `e`, U+200E, `f`, U+200F, `g`, U+2028, `h`, U+2029, `i` as `/d?e?f?g?h?i`, and no such character's bytes appear |
| **A block a look cannot read is never clean** (the last round: the backup boot sector was only a note). It is a problem, as FSInfo's is | `fs/fat_check.c` | `unreadable()`, new: on clean volumes of each type, every block a look reads in the reserved sectors, the FATs and the first 64 clusters, unreadable in turn: no look ends clean. On 89ffae9's checker, block 6 of FAT32, the backup, did |
| **Ctrl+C is 130, with no word of a repair** (the last round). An interrupted look said `interrupted`, then that the stick could be repaired elsewhere, status 1. Now the command ends with the shell's status for a command cut short (`KCMD_INTERRUPTED`), in a look or in the wait for `/.netman`; and no memory to look at the mounts is 2 | `shell/fsckcmd.c` | `test_fsckcmd`: a look the checker says was stopped, the wait cut short, no memory. The USB scenario: `echo status $?` after Ctrl+C in the wait says 130 |
| **A look in the kernel offers no repair.** `fsck -r would:`, `no repair:`, `nothing here can be repaired` and `N of them stay reported` only with `FAT_CHECK_PLAN`, which only the host tests give | `fs/fat_check.c` | Every scenario also looks as the command does, and finds none of those words |
| **The page** | `rootfs/man/fsck.txt` | Its doubts include more than 65536 slots, and what it could not check (a block it cannot read, past 128 levels, past 65536 slots, names past the memory) |

What was not done: netman's own mount in the real kernel was not raced with a look. Its moment is a few milliseconds and cannot be hit from a console; the wait and the race are proved on the host, and the real kernel's mount table, the wait, the give-up and the hold that never happened are proved in QEMU with a mount made by hand.

## The fix round, in one paragraph

Round 1 passed every gate, and two independent reviews of it (0df671a) then found memory corruption from a read-only run and real data loss in `-r`. The reviews were right, and the gates passed because
the test measured too little: it was judged by what the repair itself left. The fix round took the coordinator's rule, *when in doubt, `-r` changes nothing*, literally. Everything that made a repair clever went. One
function, `bar()`, is the only way a doubt is recorded, and one bar anywhere in a look means the run writes nothing at all, FSInfo and the clean bit included. The test's yardstick was rewritten from the original image, and every finding
has a test that fails on 0df671a (table below). Its second reviews found the look sound and the repairs still not, hence this round.

## What was built

| File | What |
|---|---|
| `fs/fat_check.c`, `include/fs/fat_check.h` | The checker. Pure: reads, writes, words and the question go through `struct fat_check_io`. Its repairs are reached by the host tests only |
| `shell/fsckcmd.c` (new), `shell/main.c`, `shell/shell.h` | The kernel command: `fsck [-v] <dev>`. `-r` is refused |
| `tests/host/test_fsck.c` | The checker's test, over images the real driver made and reads back |
| `tests/host/test_fsckcmd.c` (new) | The command's test: the real `cmd_fsck()` over a device layer and mount table it scripts |
| `tests/host/test_fsckheap.c` (new, the last round) | How far a look makes the kernel heap grow: the checker over `mm/kalloc.c` itself |
| `tools/qemu-usb.py` | The scenario `fsck`, by pure insertion: the real kernel, the real USB stack, a damaged stick, a pull |
| `tools/devloop.py` | `fsck -r` joins `REFUSED`, by insertion (round 1; it is refused in the kernel now as well) |
| `rootfs/man/fsck.txt` | Its page |
| `tools/qemu-run.py`, `Makefile` | One smoke block, three host-test blocks, by insertion, and one changed line of `tools/qemu-run.py`: a deviation, under the gates below |
| `docs/GUI.md` | One sentence, as the brief asks. It names `fsck` for checking only, so it stands as it is |

**A deviation from the brief:** it puts `cmd_fsck` in `shell/fscmd.c`. It is in `shell/fsckcmd.c`, and `fscmd.c` is back to what the base has, so that the command can be compiled alone over stubs and tested. The x86 kernel takes every C file of `shell/` by itself, and `arch/riscv32/kernel.mk` names its files and has neither this one nor
`fs/fat_check.c`.

## The repair code, kept but off

Nothing in the kernel reaches it. `tests/host/test_fsck.c` still runs it on every scenario, pulls it at every write and fuzzes it, so that the later round starts from code that is tested, and from the list above. What it does, when nothing in the look barred it, one kind in each pass and the whole volume looked at again between two:

1. **The second FAT from the first.**
2. **The end of a chain:** `FAT[c] := end of chain`, the first FAT, a flush, the second, a flush. For a link to no cluster or a loop's closing link, `c` is the last good cluster. For a link to a free cluster, `c` is *the free cluster itself*, which stays as the chain's last, because `fs/fat.c` reads it (`is_eoc()` takes a value below 2 as the end, after the cluster it is read from).
3. **An entry's size, smaller,** when a chain that ends well is shorter than the size. A later pass than the end of its chain, never the same one.
4. **Lost chains that end well,** into a new `FOUND.nnn` of free clusters no link names, named by one write of one root slot. Item 1 of the list above is about this one.
5. **FSInfo's count and hint,** in a sector that has its signatures.
6. **The clean bit,** second FAT first, first FAT last, and only when no problem is left.

No repair ever writes an entry's first cluster, a link to another cluster, or a cluster anything names. The write judge in the test enforces each of those on every write.

## What was dropped from the repairs in the fix round, and why

| Dropped | Why |
|---|---|
| **The second of two names getting its own copy** (`fix_copy`) | It is the one repair that makes a name read other bytes when a stick ignores a flush: the review's 48 of 120 trials, and its read-back can pass from the stick's cache (`driver/usb/msc.c:371`). No order of writes makes that safe. Two names bar repair. |
| **A chain longer than its size cut at the size** (`fix_is_safe`, `long_check`) | My own extension in round 1, not on the brief's list, and the source of K2. It stays reported and bars nothing. |
| **Emptying an entry with a bad first cluster** | It wrote an entry's first cluster, and it is how D1 emptied `a2.bin`. A first cluster that is no cluster bars repair. |
| **Cutting a directory's chain** | Past a bad link there may be entries. Any directory chain with anything wrong with it bars repair. |
| **Cutting at a cluster marked bad** | There may be data past it, and the driver reads the bad cluster. It bars repair. |
| **Cutting a lost chain that joins something** | Stays reported, bars nothing. |
| **Splitting a lost chain of 4 GB or more** | Stays reported. |
| **Writing FSInfo's signatures** | A sector without them may be no FSInfo, and a reserved sector can hold anything. `fs/fat.c` leaves such a sector alone too (`write_fsinfo()`). Reported. |

## The doubts

A look reports these as problems, but for three (below). In the repair code any one of them, in any look, means nothing is written (`bar()`); the page lists them for the reader as what a repair anywhere has to guess at.

| Doubt | Where it came from |
|---|---|
| The boot sector's shape differs from its backup's (bytes 11-23 and 32-51) | D5 |
| The FAT is far larger than its clusters need (`spf > nsec + nsec/2 + 2`), all types | D5 |
| Something past the FAT's last entry | Mine: a flipped bit in the total size, which D5's rule does not see (`boot-tail`) |
| FAT[0] or FAT[1] wrong | Mine: what says this is the FAT the boot sector means |
| FATs not mirrored | Round 1 |
| The FATs in dispute: a bad link the second does not share, the first ending a file short of its size or a directory where the second goes on, two different links | D2, D3 |
| A blank sector of the first FAT where the second differs; on a volume with one FAT, a sector of all ones | D2, D6 |
| A chain into a sector of the FAT that is all zeros | D6 |
| Two names on a chain, of files or directories; a second name of a directory; two chains into one free cluster | D4, and the end of `fix_copy` |
| A chain into a cluster marked bad; a first cluster that is no cluster | Dropped repairs |
| A walk not whole: a block that cannot be read, a depth past 128, past 65536 slots, no `.` and `..`, a `.` naming another cluster, a directory with no cluster, a directory's chain with anything wrong with it, non-zero bytes past the end mark, a label with clusters, a size-0 file that starts as a directory | D4, D9, K4 |
| Lost clusters that are most of what is in use, or a lost chain that starts as a directory | D6 |
| A FAT12 entry across two sectors that would have to change | Round 1 |

Three of them are not problems, because they can be true of a healthy volume: a FAT far too large, something past the last entry, and FATs not mirrored say nothing without `-v`, and a volume with nothing else wrong reads `clean`. Names a look could not compare are not a doubt but a look not whole (above).

**Reported only:** names, long names, `..` (a volume may write the root's number there), a directory's size, duplicates, the boot sector's dirty flag, a chain past its size, FSInfo with no signatures, a lost chain that runs into another or into a bad cluster.

## Decisions a reviewer would ask about

**A look.** It holds an unmounted device from before its first read to after its last (`vfs_bdev_change_begin()`). A volume mounted read-only cannot be held that way, so its mount is held (`vfs_hold_mount()`), except netman's on `/.netman`, which is waited for and never held (above). A volume mounted read-write is refused, and so is a mount of the disk or a partition of it, with the reason. The look is given no write function at all: what it cannot call it cannot call by mistake.

**Memory.** The kernel heap keeps what it once grew to, and under first fit a freed piece is taken again only by one that fits in it: a table that doubles cannot use the copies it left (the third review measured 4548 KB of growth where 4 MB was promised). So a look takes everything at its start, one piece each, and `CHECK_MEM_MOST` (4 MB) bounds how far it makes the heap grow, counted before anything is taken: the pieces with their headers, and what the heap may leave over above the last of them, since it grows 64 KB at a time. The table of names is taken once, for the largest directory the volume can hold: twice its entries as a power of two, `SLOTS_MAX` at most, which is 1.5 MB (131072 slots of 12 bytes, a short name and the mark of the directory it is of) on any volume with more than 2 MB of data; a new directory takes a new mark instead of clearing the table, and every 255 directories the marks are cleared. Why 4 MB: the owner's 118 GB stick with its 32 KB clusters takes 1.8 MB, and the whole table 1.5 MB more. The price: a small volume's look takes 1.9 MB where it took 0.4 MB, while it runs; the heap keeps it for what comes next. In the 32-bit build, of FAT32 with 32 KB clusters, 5.75 million clusters (188 GB) keep the whole table of names and 9.93 million (325 GB, or 41 GB with 4 KB clusters) fit at all; past the first a smaller table may leave a directory's names not compared, which the look says, and past the second a volume is refused in words, to be checked elsewhere. Nothing recurses.

**"No worse",** for the repair code, and the test enforces it from the original image, never from what the repair left. Two readings of the image a run began on, by the real driver: through the first FAT, and through the second with the FATs swapped. Every block of every file in either is still under its name, or in `FOUND.nnn`. The tree's bytes are unique, so a block found is that block. Each name reads a prefix of what it read. No new name appears outside `FOUND`. It passed on aef87bf, and the reviewers' fuzz still found item 1 of the list above: this yardstick did not see it, which is for the later round to settle before it trusts the test again.

**The model of a pull.** `One sector is written whole` and `a flush means what it says` are assumptions, and the test breaks both: the write that is pulled lands whole, keeps a prefix with the old rest, a prefix with the rest erased, all ones or all zeros; and a stick's cache can drop what was not flushed, or, with a flush that lies, end each sector as any of its versions in no order at all.

**The look is one funnel.** `bar()` is the only way a doubt is recorded, and `doubt()` is `problem()` and `bar()`. The bar is reset at each look.

**Depth.** 128 levels, a fixed table of frames, no growth: a path of 256 bytes goes no deeper. Reached, the walk is not whole.

**Every index built from an on-disk field,** the audit K1 asked for. `lfn[]`: `lfn_next` nonzero holds `seq` to 1..20. `name[]`: 260 units at most 3 bytes each, a surrogate pair 4 for two. `found_used[]`: three decimal digits. `path[]`, `who[]`, `line[]`: `snprintf`, with `plen` from `strlen`. `win[].data`: at most `CHUNK` sectors into a buffer of `CHUNK`, and an offset inside the window; a cluster number past the end reads inside the device or fails. `dbuf`: `DIR_AHEAD`. `fix[]`, `lost[]`: their counts. `list[]`: `d` clusters. Every `bit_get()` and `bit_set()` follows an `in_range()` of that same value. Reads go through `rd()`, which refuses a range past the device, wrapped or not.

**Status.** 0 clean; 1 problems found; 2 usage, `-r`, a device it may not use, no FAT volume, a device that fails, no memory (to look at the mounts too), a volume past the memory, and a look not whole that found nothing else; 130 when Ctrl+C cut it short, in a look or in the wait for `/.netman`, as for any command (`KCMD_INTERRUPTED`), with no word of a repair.

**The block cache, names, SurfCPU.** Reads go straight to the device. Names printed are sanitized: controls, C1 controls and the bidirectional controls come out as `?`. `fs/fat_check.c` and `shell/fsckcmd.c` are not in `arch/riscv32/kernel.mk`: the brief puts SurfCPU out of scope.

## Where the code comes from

The coordinator asked whether anything here follows a specific program's source from memory. No tool call of this track ever opened the source of dosfstools, `fsck_msdos`, mtools or Linux's fat driver: `fsck.fat` and `fsck_msdos` are only run, as programs, to judge results. That settles what I read, not what I remember, so the file was gone through for the second.

| What | Source | How close, and what was done |
|---|---|---|
| `short_name_bad()`, the characters a short name may not hold | **Its first version was close to dosfstools' equivalent (GPL), as I remember that function:** the same characters in the same order, around the same checks | **Rewritten from fatgen103's text on `DIR_Name`:** its values in its order, as a table, a wider list than before. This round rewrote the branch's history so that the first version is in none of its commits (below). `name-plus`, `name-space`, `name-low`, `name-05` are its tests. DEL is not on the specification's list: it stays, as the control character it is, and the comment says so. |
| The boot sector's dirty flag: bit 0 of byte 37, or 65 on FAT32 | Linux's fat driver and dosfstools, from memory: fatgen103 only calls that byte reserved | One bit position, a fact about the format with no expression in it. Read and reported, never written. |
| `lost_walk()`: chains from their heads, then the rings | The idea is also in dosfstools' `reclaim_file()`, which I remember | The idea only, and it is what anyone would do with a graph. Mine is two passes over a bitmap of what has a predecessor; dosfstools counts references, and writes the FAT as it goes. Not close. |
| `FOUND.nnn/FILEnnnn.CHK` | Windows' `chkdsk`, as a convention | The brief's names. |

**`fatgen103`, from memory, with no copy on this machine:** FAT type by cluster count (4085, 65525), the BPB's offsets, FSInfo's signatures and offsets, end-of-chain and bad values, FAT[0] and FAT[1] with the clean and hard-error bits, `BPB_ExtFlags` bit 7, the FAT12 entry arithmetic of `fat_get()` and `fat_poke()`, `DIR_Name`'s rules, the long entry's layout (`pos[]`) and `ChkSum` (`lfn_checksum()`, which `fs/fat.c` has too), the date and time packing.

**From this tree:** the two lower-case flags of byte 12, as `fs/fat.c` has them (`NTRES_LOWER_BASE`); fatgen103 calls that byte reserved. The mask of a long entry's number, 0x1F, as `fs/fat.c` reads it. **Public:** FNV-1a in `names_hash()`, UTF-8 and surrogate pairs from Unicode, the bidirectional controls' code points. Nothing in this track comes from mtools or `fsck_msdos`.

## The branch's history, rewritten

The first `short_name_bad()` came in with the branch's first commit, c8db0a2 (the parent of 8b74c47, where the coordinator's message placed it), and stayed to 579f687; 1ec2ab2 replaced it. The rewrite, done with git's plumbing and no interactive step:

- In each commit from c8db0a2 to 579f687, that function's text was replaced by the fatgen103 version, byte for byte the text 1ec2ab2 brought, comment included. Nothing else in those trees changed.
- 1ec2ab2's change to `fs/fat_check.c` was thereby already made, so it is the four tests alone, and its message now says that. aef87bf's account quoted the first version: that passage is now the neutral row above, and its message no longer says what that version was.
- Every commit keeps its author and its dates, and its message where that is still true; the other message corrected is this round's own on the 4 MB cap, whose sum read as more than 4 MB. 1ec2ab2 and 6117116 keep their trees; aef87bf and this round's commits up to this file's carry the neutral passage in the account, and this file's commit makes the account what it is now.
- Every rewritten commit was built in a fresh container of the devenv's image, from that commit as it is (`git archive`): the kernel at O2 and at O0, and the fsck host tests linked; and its host tests were run natively. None failed.
- The last tree is byte for byte the tip's before the rewrite: the tree that passed the gates below, and this file. The proof, `git rev-parse <old>^{tree} <new>^{tree}`, is in this round's report, since it can only be made after this file is committed.

The commits, before and after; this file's own commit is the one after the last, and cannot name its hash:

| Before | After | Commit (as it is now) |
|---|---|---|
| c8db0a2 | fe7d655 | fs: fat_check.c, the checker and repairer behind fsck |
| 8b74c47 | f5cf370 | shell: the kernel command fsck |
| 8b33e19 | 569fbd5 | tests: test_fsck, the checker over images the real driver made |
| 7dbde3f | 0904af7 | smoke: fsck's refusals, its statuses, and a clean volume |
| 0df671a | 7177364 | docs: the account of track fsck, and GUI.md's advice names it |
| eae86a1 | 51278da | fsck: when in doubt, -r changes nothing |
| 10038cb | f625f4a | tests: the fsck command over a device layer the test scripts |
| 26c19bf | a57581f | tools: a stick repaired and pulled through the real kernel; devloop never repairs |
| 1b178be | 774213f | man: fsck, after the fix round: what bars repair, what is repaired, what "no worse" rests on |
| b801868 | 3ae9d87 | tests: fsck's write judge and a FAT12 entry across two sectors |
| 26e7d46 | 35a7037 | tools: qemu-usb.py's list of scenarios has fsck |
| 579f687 | 92ad730 | tools: the fsck scenario takes a pull after the last write as it is |
| 1ec2ab2 | 4debc28 | tests: short names against fatgen103's list |
| 6117116 | 12f7602 | tests: memhog hold takes the third way a program says it is out of memory |
| aef87bf | 7af358e | docs: fsck's account after the fix round |
| 111b6aa | 7209333 | fsck: -r refused: repairs are not available in this version |
| a3ebf32 | 0a98c20 | fsck: netman's mount on /.netman is never held, but waited for |
| 0db44ee | 03def6a | fsck: a look says what a repair would do only when asked (FAT_CHECK_PLAN) |
| cf35320 | 8257953 | fsck: names a look could not compare are said, and the look is not clean |
| a0e1aab | ceeb03d | fsck: a check takes at most 4 MB, and a volume that needs more is refused |
| fcbb0e7 | 376f49a | fsck: bidirectional controls in names come out as '?' |
| 7509b11 | 41f7614 | tools: the fsck scenario for a command that only reads |
| dd26e8c | fb5a9b5 | man: fsck only reads |
| 92b041a | 66a1be9 | tools: the fsck scenario's pull comes once the look is reading |

The old commits are still in the shared repository's objects (nothing was pruned); no branch holds them. The last round's commits came after 89ffae9, on top of it, with no rewrite.

## Known limits

As the coordinator asked, left as they are: **colliding names in `names_put()`** take longer, bounded by the table, and Ctrl+C stops them: the reviewers' 65534 names on one hash took 6.5 s on the Mac. **A chain that crosses a FAT window at each step** reads slowly, one window read for each cluster, and Ctrl+C stops that too.

Also: a long name twice in a directory is not compared; the boot sector's label is not compared with the root's (the one difference `fsck.fat` may have); a volume past the second edge under **Memory** is refused rather than checked in pieces, and past the first a directory with more names than the smaller table holds is not compared, which the look says. A look at the stick SurfOS started from, unmounted, takes it for as long as it lasts: the network settings cannot mount it meanwhile, and a save in that time gives up after its own five seconds, saying the stick could not be mounted (`stick_open()`, `kernel/netman.c`).

## Each finding of the first reviews, and the test that fails on 0df671a

From the fix round, as it was measured: the same test, against the checker of 0df671a (`git show 0df671a:fs/fat_check.c`, its `ask` call and `copies` field renamed to build against the new header). The counts are failing checks of that one test. `san` is the native sanitizer build.

| Finding | Test | What it does on 0df671a |
|---|---|---|
| K1 | `lfn-zero`, `long-names` (san) | `index 4294967283 out of bounds for type 'u16[261]'`, on the first image. Unsanitized, `lfn0_fat16.img` is a SIGSEGV. |
| K2 | `changing`, on `d_windows` (san) | AddressSanitizer: BUS in `walk()`, at `long_check()`, with `fix_is_safe()` following a link read the second time. |
| K3 | `flood` | 501 names on one cluster: 501 lines |
| K4 | `too-deep` | 520048 bytes of memory where 40 levels took 377200; a directory at the cap with a file under it is repaired past |
| D1 | `free-link`, `free-taken`, `first-free`, `first-free-only` | 503, 341, 555 and 277 failures of "nothing lost": `/README.TXT: the 1024 bytes at 3072 are nowhere` |
| D2 | `fat1-ff`, `fat1-early`, `fat1-dir-early` | 5, 103 and 118 of them: `/ALBUM.BIN: the 1024 bytes at 4096 are nowhere`, `/DIRTWO/ONDIR.BIN is gone` |
| D3 | `fat1-free-link`, `fat1-bad-link`, `fat1-free-end` | 172 of them each on the first two; the judge: `a good link cut` |
| D4 | `dir-into-file` | 1534 of them: `/ZZZZ/DATA.LOG has other bytes`; the judge: a cluster written that is neither free nor only a directory's |
| D5 | `boot-flat`, `boot-flips`, `boot-tail`, `boot-total` | 882 and 15 of them: `/README.TXT is gone`, with one bit of the boot sector turned over; it writes on the other two |
| D6 a | `root-zero`, `root-ones`, `lost-most`, `lost-dir` | It repairs: 13, 8, 14 and 15 failures |
| D6 b | `fat1-ff`, `fat1-ff-free`, `fat-ff-one` | As D2; it trusts the sector |
| D6 c | `twin`, with a flush that lies | 66 trials end `/TWIN.TXT has other bytes`. That is `fix_copy`, and why it is gone. |
| D7 | All of the above | The oracle itself: the counts above are failures of the measured check, not of a scenario's expected result |
| D9 | `slot-zero`, `root-junk`, `dir-none`, `label-clusters` | It repairs: 8, 6, 9 and 9 failures |

## Tests

`make unittest` runs `tests/host/test_fsck.c`, `tests/host/test_fsckcmd.c` and, since the last round, `tests/host/test_fsckheap.c` (the heap). The driver, not the checker, says what a file holds.

**The counts, on the tree committed:** in the devenv, `test_fsck`: 34598 checks, 345 scenarios, 3630 pulls, 0 failures; `test_fsckcmd`: 88 checks, 0 failures; `test_fsckheap`: 12 checks, 0 failures. Natively on the Mac, `test_fsck`: 34511 checks, 345 scenarios across FAT12 with one FAT, FAT12 with two, FAT16 and FAT32, 3630 pulls, 0 failures, plain (41 s) and under the sanitizers (98 s, no report); `test_fsckcmd`: 88 checks; `test_fsckheap`: 12 checks; both ways, 0 failures. `test_fsck <seed> <images>`, `--lfn <seed> <cases>` and `--image <path>` run more by hand.

**Every commit** of the read-only round and of the last was tested natively and built as it is in a fresh container (the kernel at O2 and O0, the fsck host tests linked) before the next was made, but for 92b041a, a change to the USB scenario alone, which the gates ran.

**Do the tests have teeth?** A test that passes proves nothing until it has been seen to fail. **The last round:** its changes taken out again one at a time, each compiled in and the tests run (`test_fsck` and `test_fsckheap` for the checker, `test_fsckcmd` for the command): **10 of 11 caught**. Caught: the table sized for the cap alone, the table not cut to fit the cap, the heap's leftover not counted, a new directory keeping the names of the one before, the marks not cleared when they come round again, a backup boot sector that cannot be read being no problem, the marks of direction and the separators let through; and of the command, an interrupted look advised to have the stick repaired, Ctrl+C in the wait for `/.netman` a failure, and no memory to look at the mounts status 1. The one that survives is the headers of the pieces left out of the count (`PIECE_EXTRA` 0): about 500 bytes, inside the 4 KB that `CHECK_HEAP_SLACK` keeps above the heap's 64 KB step, and the heap's last growth leaves about 4 KB over, since a window of 64 KB is the last piece taken. The marks' clearing first survived too: in a test of 300 directories of the same names, each name landed where the one before it had been and wrote over it, so no stale name could be met. The test now gives each directory a name it shares only with the one 255 before and after it, and catches it. The new tests also fail where they should on 89ffae9's checker: `unreadable()` at block 6 of FAT32, the backup; `names_said()` at the new characters; `deep_and_wide()` with 17 pieces where the volume it was made on took 15; `test_fsckheap` with the review's own numbers, 4548 and 4708 KB.

**The read-only round:** its changes taken out again: **17 of 17 caught**, 11 of `shell/fsckcmd.c` (`-r` not refused, refused only once a device is found, netman's mount held like any other, a hold that catches it kept, no end to the wait, Ctrl+C in the wait ignored, no memory to look going on, a read-write mount run on, the hold never let go of, no word of where to repair, `-v` lost) and 6 of `fs/fat_check.c` (no cap on a check's memory, the table of names growing past it, names not compared and still clean, bidirectional controls let through, a look without the plan saying it, a barred look without the plan saying why). The fix round's 61 breakages of the checker, run again on that round's checker (one of them anchored again, as the line it breaks was reworded): 59 of 61 caught, and the two that survive are the fix round's two equivalents: `take_free()` skipping what a link names cannot be reached, because FOUND comes only after every such link has been ended, and a lost chain saved before its bad end is ended reaches the same volume a pass later.

**A second opinion on the look.** The second kernel review's own fuzzer (`fuzzhost.c`, from its scratchpad, unchanged but for building it against this tree) under ASan and UBSan: 100000 cases over its five FAT12, FAT16 and FAT32 bases, 40000 of them with a device whose sectors read differently after a while. No sanitizer report, and its oracle (no cluster that began as a file's data changes) held, on the stable device and the changing one. That fuzzer runs the repairs too, which are still in the code.

**Coverage,** by clang's source coverage of `fs/fat_check.c` under the whole test, on the last round's checker: 1341 of 1341 lines, 97 of 97 functions, 97.3% of regions.

**The proofs, from the fix round,** for the repair code where they say so:

| Promise | Proof |
|---|---|
| A look never writes | It is given no write function. Every scenario and every fuzzed image is compared byte for byte. A run not asked to repair, handed a write function, never calls it. `test_fsckcmd` checks `io.write`, `io.ask` and `io.flush` are NULL and the flags are 0, and the USB scenario compares the sticks byte for byte. |
| The repair code never deletes data | `kept()`, on every scenario and every fuzzed image. `write_judged()` on every write: not the boot sector or its backup; each FAT entry, in both FATs, judged by what it was and what it becomes; no cluster written unless it is free and nothing names it, or wholly a directory's; in a directory only a smaller size, or a free slot of the root. Item 1 of the list above got past it. |
| A pull at any write of a repair is no worse | `pull_one()`: each scenario's repair pulled at each of its writes, whole, torn four ways on the small volumes and one way at random on FAT32, with a cache and with a flush that lies; then `kept()`, and again after a further repair. |
| No damaged volume panics a look | `fuzz()`: blows to FATs, directories, sectors, the boot sector, FSInfo, with a bound on requests. `lfn_fuzz()`: long name sets of every shape. `boot_flips()`: every bit of the boot sector. `changing()`: a FAT that does not read the same twice. |
| Failing devices, separately | `bad_reads()`, `dying()`, `stops()`, `stop_soon()`, `refusals()`, `no_memory()`, `truncated()`, `changed_under()`, `changed_later()`. Not in the fuzz. |

**False alarms.** Rules this strict could call a healthy volume damaged, which has its own cost. Real macOS-formatted volumes (`newfs_msdos`, FAT12, FAT16, FAT32, up to 328 files in 7 directories), and the reviewers' base images, all read `clean` (the fix round's measure; nothing this round changed what a look calls a problem, but for names it could not compare).

**Against `fsck.fat -n`** (dosfstools, run as a program in the devenv; not on the Mac, which says so): in `make unittest`'s fuzz, 46 images judged by both, as in the fix round: 1 both clean, 41 both found problems, 3 this one only, 1 `fsck.fat` only, the boot sector's label, which is the one difference the test allows.

## The gates

The last round's, all on the tree committed but for this account (50f641b), which reports them; the account's commit changes nothing but this file.

| Gate | Result |
|---|---|
| `make unittest` | Status 0, no failure in any host test: `test_fsck` 34598 checks, 345 scenarios, 3630 pulls; `test_fsckcmd` 88 checks; `test_fsckheap` 12 checks |
| `make test-all`, through the lock | Status 0 on its second run: O0 smoke 545, Files 51; O2 smoke 545, Files 51, with nothing else of this track running meanwhile. The first run failed 1 of 540 at O0, in httpd's six places (below), and so never reached O2 |
| `make -C surfcpu test-os-qemu` | 137 of 137 at O2 and at O0 (build 50f641bb) |
| `tools/qemu-usb.py --test --scenario storage` | 39 of 39 at O0 and at O2 |
| `tools/qemu-usb.py --test --scenario fsck` | 47 of 47 at O0 and at O2, on USB images of 50f641b: the status after Ctrl+C is 130, and at both levels the pull landed while the look read the FAT |

The read-only round's passed the same way on 92b041a (66a1be9 after the rewrite): `make unittest` status 0; `test-all` smoke 545 and Files 51 at O0 and at O2; SurfCPU 137 of 137 at both levels; storage 39 of 39 and fsck 46 of 46 at both.

**The USB scenario, now:** `python3 tools/qemu-usb.py --image <img> --test --scenario fsck`. A guest cannot damage a volume, so a copy of `foreign-fat32.img` is damaged on the host: the clean mark, FSInfo, a size past its chain, a blank second FAT, a lost chain of three clusters. A look finds all of it and says where it can be repaired, status 1; `-r`, `-vr` and `-r` after the device are each refused with the one line, status 2; the look with its output in `/dev/null` keeps its status; a look under a read-only mount, and the mount let go of; the stick mounted on `/.netman` by hand, a look that waits and gives up with status 2, and `umount /.netman` at once after it, so nothing was held; Ctrl+C in that wait, and `echo status $?` then says 130; no look speaks of a repair; and the stick byte for byte what it was. A second copy, with a second name on BIG.BIN's chain: reported, `-r` refused; pulled as soon as a look says it is reading the volume, which at both levels cut it short (`the FAT cannot be read at block 32: stopped`, status 2, the prompt back); plugged in again and the same report; byte for byte as it was; then a look at a read-write mount refused, and `-r` there too.

The USB images were built with `./devenv/build.sh O=<n> usb-image` rather than `devenv/build-usb.sh`, which fetches Wi-Fi firmware from the internet: the brief allows no host outside the harness. Those images hold no Wi-Fi firmware.

**A deviation from the brief, on purpose, from the fix round: one existing line of `tools/qemu-run.py`,** which is otherwise pure insertion. `make test-all` could not pass without it, and it is a commit of its own (6117116 before the rewrite) so that it can be dropped. O2 failed 13 of 540 on that tree every time: `memhog` takes 64 KB blocks until `malloc` fails, and in between a program can get past the shell and fail at its stack (`/initrd/bin/ls: out of memory for the stack`, in `kernel/process.c`), a third outcome `memhog_hold_check()` did not know. Which outcome comes is fixed for a given size of kernel and initrd and moves when they do, so it can come and go with any commit. With the third outcome accepted, O2 went from 13 failures to none. This round changed nothing of it; `tools/qemu-usb.py` and the Makefile are still pure insertion.

## Incidents, for the record

**Round 1:** my first probe ran a `gcc -m32 -fsanitize=address,undefined` binary in the Docker image, before the coordinator's correction. It hung at 99% CPU under `qemu-i386` for about 15 minutes on the shared machine, and was killed by ID after its `docker inspect` mount confirmed it was this track's. Round 1's `test-all` also failed O2 once at 2 of 540 (`ata(absent)` at load 20) and passed on a rerun: called load, not proved.

**The fix round:** one `df` read 5 GB free, under the brief's 8 GB; the next two read 8.5 and 14 GB, and nothing of mine was building, so I went on and read it before each Docker step. My first run of the USB scenario hit my own time limit, which killed Python and left its QEMU running; it was found by `pgrep` (parent 1, a file only that scenario makes on its command line) and killed by PID. My first script of the gates read the lock twice per decision and could have read my ticket turning into a slot as "clear"; it was replaced before it did any harm. `test-all` failed O2 at 13 of 540 three times, and I called it load twice before the whole sequence on the Mac showed what it was.

**The read-only round:** the USB scenario's first run passed, and its transcript showed that one check said more than it proved: the pull it called "in the middle of a look" came 0.3 s after the command, and the look at this small stick had already ended at O0; it was a pull at the prompt. The pull now waits for the look's first line about the volume, the check says no more than "while a look reads", and the run prints where the pull landed (92b041a before the rewrite). Every gate then ran on that commit: `make unittest` and the USB scenarios again, `test-all` and SurfCPU's for the first time, as they had not started yet. The rest went as planned: `df` read 13 to 16 GB free at every Docker step.

**The last round:** the volume of my heap test came from the review's probe with its loop for the FAT's size, which settles by going back and forth until two turns agree; for the 118 GB stick it never did, and the test ran 10 minutes under my own time limit before `sample` showed it there. The loop now only lets the FAT grow. Then `kheap_check()` called the heap corrupt after every look but the first: `heap_init()` is for once, and the heap's counts go on from where they were, so each look now runs in a child process on a heap of its own. The marks' clearing passed a first test without it (above, under the tests). And a copy of 89ffae9's checker, built to see the new tests fail on it, was left in the heap test's folder and broke the next native build with a duplicate symbol; it was moved out. `df` read 12 to 15 GB free at each Docker step. The first `test-all` failed one smoke check at O0, nothing of fsck: httpd's six places, where eight clients start at once and client 4's `/wave.svg` got `Remote end closed connection without response`. The same failure stands in other runs' records in `06-gates/results` (hand-1, daily-3, wt-m3-1), and CLAUDE.md says why: QEMU's user network resets one of two connections made in the same instant, before the guest sees it. The second run, on the same tree, passed whole; so this is a known flake met once, not a proof that it was nothing, and it is said here as it happened.
