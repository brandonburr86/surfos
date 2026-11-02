# The command-line suite: track fsck

The account of `cli/fsck`, for the integrator and the review. The brief is `2026-11-01-cli-suite.md`.

## What was built

| File | What |
|---|---|
| `fs/fat_check.c`, `include/fs/fat_check.h` | The checker. Pure: reads, writes, words and the question go through `struct fat_check_io`. |
| `shell/fscmd.c`, `shell/main.c`, `shell/shell.h` | The kernel command: `fsck [-v] <dev>`, `fsck -r [-v] <dev>` |
| `tests/host/test_fsck.c` | Its test, over images the real driver made and reads back |
| `rootfs/man/fsck.txt` | Its page |
| `tools/qemu-run.py`, `Makefile` | One smoke block, one host-test block, by insertion |
| `docs/GUI.md` | One sentence, as the brief asks |

**It runs on nothing but QEMU so far. None of it has run on the laptop or on a real stick.** A pull here is a test harness stopping a
RAM disk, or QEMU taking an emulated stick off an emulated port. Until the owner tries it, nobody knows what a real stick does when it
comes out in the middle of a write, and "one sector is written whole" (below) is an assumption about hardware no test here has touched.

## What it checks

As the brief lists: the boot sector and BPB, the backup, FSInfo, whether the FATs agree, the reserved entries, the clean bit;
every directory's entries, `.` and `..`, long name sets (order, checksum), short names and duplicates of them; each chain (out of
range, free or bad clusters, loops, length against size); two entries on one chain; clusters in use that nothing reaches; the free
count. It names both holders of a shared cluster, which takes one more walk, only when there is a sharing to explain.

## What it repairs, and what it does not

Repaired, in this order, one kind in each pass, the whole volume looked at again between passes:

1. The second FAT from the first, sector by sector, when the first is sound.
2. A chain cut at its first bad link, the size set to match. A size past a whole chain. A file whose first cluster is no cluster
   keeps its name, with no clusters and size 0.
3. A chain longer than its size, cut at the size. What is cut off becomes lost clusters, which pass 5 saves.
4. Two names on one chain: the second gets its own copy when there is room.
5. Lost chains saved as `FOUND.nnn/FILEnnnn.CHK`.
6. FSInfo, then the clean bit, the bit only when no problem is left.

**Reported only**, because a repair could not be proved safe or is not on the brief's list:

| What | Why it stays |
|---|---|
| The boot sector, including its own dirty flag | Never written. There is one, and a write torn there loses the volume. Not on the list. |
| The backup boot sector differing | A note, not a problem (status 0), and never written. `fsck.fat -n` exits 0 on this too (the `backup` scenario). |
| Reserved FAT entries, `.`, `..`, long names, short names, duplicates, a directory's size | Not on the brief's list. |
| Two names on a directory | A copy would give every file in it two names. |
| A directory whose first cluster is bad, or that shares clusters with a file | The walk is then not whole (below). |
| A directory's chain into a free or bad cluster | `fs/fat.c` reads that cluster as part of the directory, so entries may be in it, and a cut would take them off the volume. Only a link to no cluster and a loop are cut: nothing can lie past those. A file's chain is cut in all these cases, as the brief says, and one cluster past the cut is then out of the file: a free or bad one, which nothing protected. |
| A subdirectory without `.` and `..` at its first two slots | It may be a file whose directory bit was damaged. Taking its bytes for entries, a "repair" of one would write into the file. It is not walked. |
| A file of size 0 on what starts as a directory | A directory that lost its bit. Letting go of the chain would take its tree apart. |
| A media byte that matches no FAT[0] | Reported, and the check goes on: the driver mounts such a volume. |
| A FAT12 entry across two sectors | One write cannot change it, and half of it links anywhere. The repair is left, and while a bad link stays, nothing is allocated: a free cluster taken could be the one that link reaches. |
| Anything, when the walk was not whole | Lost clusters may be what could not be read. |
| Anything, when the FATs differ and the first is not sound | Which is right is for a person to say. |
| Anything, when the FATs are not mirrored (`BPB_ExtFlags` bit 7) | SurfOS's driver does not know that mode. |

## Decisions a reviewer would ask about

**"No worse" means this,** and it is what the test enforces: every entry there was is still there, and its chain holds at least the
bytes it held; each file reads the same bytes, as far as the repaired volume keeps them; no cluster in use is free; no more clusters
are named twice and no more chains reach a free cluster than before; the volume says it was not unmounted cleanly until the last
write; and a further `fsck -r` ends where the whole repair would have. A pull can leave *soft* problems that were not there before
(lost clusters from a copy not linked in yet, the FATs differing by a sector, FSInfo), which a further run repairs and which put no
byte at risk. Those are "no worse" by this definition, and a reviewer may want to know that.

**The model of a pull.** One sector is written whole, and a flush means what it says. Both are assumptions about the stick. The test
also has a device that loses what was not flushed, and the writes are ordered with flushes for that. A sector torn in half by a real
pull is outside the model: that is why the boot sector is never written.

**Nothing is deleted, literally.** No repair sets a FAT entry in use to free. Lost chains that cannot be saved stay lost. So a stick
that is full stays full, and `fsck -r` never gives space back: that is for `rm`.

**Passes, not one plan.** The look is repeated after each kind of repair, up to 64 times, at most 256 repairs and 1024 lost chains a
pass. It is slower on a badly damaged volume, and it means no repair ever acts on a plan its predecessor made stale.

**Sound.** When the FATs differ, the first is sound unless it has a bad link (out of range, free, bad, a loop, a join) that the second
does not share, or its reserved entries are wrong. A sector of zeros in the first FAT is exactly such a link, and cutting every chain
through it would lose what the second still has. A repair's own half-done write never looks like that, because the first FAT is
written first, which is what lets a second run finish. *One case a reviewer should weigh:* an early but well-formed end of chain in
the first FAT, the second going on, counts as sound. Every system reads the first FAT, a pull during a truncate leaves exactly this,
and the clusters past it stay in use and go to `FOUND`, so no byte is deleted. The cost is that the file is split, in the rare case
where damage wrote a valid end mark.

**Which of two names is second.** The one the walk reaches later: a directory's files before the directories under it. A file that comes
second gets the copy, also of a directory's clusters. A directory that comes second gets nothing: it stays reported, and leaves the walk not
whole unless its name is a second name of a directory already walked.

**A cut waits for a copy.** Cutting a chain at one file's size would take the rest from a longer name joining it before the cut (a twin
of a different size). `fix_is_safe()` holds the cut back until that name has its copy; with no room for the copy, the cut stays reported
too. The test has this (`twin-sizes`), and a mutation removing the rule is caught.

**FOUND.nnn is a new directory each time,** never an entry added to an old one: written whole into free clusters and named by one write
of one root slot, so it has a name entirely or not at all. A root with no free slot leaves the chains reported, rather than growing the
root. A name is taken if a short or a long name says so.

**The device is held across the question.** `vfs_bdev_change_begin()` before the first read of `-r`, so what is shown is what is repaired,
with no second look to compare. `mkfs` takes it after the answer because its plan does not depend on the device's contents; fsck's does.
The price: while the question waits, that device is busy for `mount` and `eject` on other consoles, in `mkfs`'s words. A kill or Ctrl+C
ends the question. `bdev_may_format()` is asked again after the answer.

**A read-only run** holds an unmounted device too, so that no mount comes in under it. A volume mounted read-only cannot be held and needs
not be. If someone remounts it read-write during the run, the report may be wrong and nothing is harmed: that is not checked.

**Status.** 0 clean or repaired, 1 problems left, 2 usage or trouble, as the brief says. Reading that closely: refused devices are 2 (the
smoke test checks each), Cancelled and interrupted are 1 (the volume has not been said to be clean). A boot sector damaged with a sound backup
at block 6 is 1; no FAT volume at all is 2.

**FSInfo.** The free count is a problem when it is wrong, not when it says "unknown" (`0xFFFFFFFF`). The next-free hint is
never a problem, and is put right when the sector is written. Signatures that are gone are restored, only those fields written. The backup
FSInfo is never compared or written: `fs/fat.c` does not keep it either, so it goes stale on any volume SurfOS writes.

**The block cache.** Reads and writes go straight to the device (`bdev_read()`, `bdev_write()`), as mkfs's do: the device is held and nothing
else writes it, and the cache's copies are dropped after a repair. On a read-only mount nothing is dirty.

**A name printed is sanitized.** Names come off a stick nobody vouches for, and go to a terminal: control characters, escapes included, are `?`.

**Memory.** Three bitmaps of one bit a cluster, four 64 KB windows on the FAT and a few small tables, all checked. Measured for the owner's 118
GB: see below. `fat_check.c` allocates nothing on the stack past small locals; nothing recurses (the walk has its own stack, grown on the heap).

**SurfCPU.** `cmd_fsck` is under `#ifndef SURFOS_RISCV32`, and `fs/fat_check.c` is not in `arch/riscv32/kernel.mk`: the brief puts SurfCPU out of
scope and `arch/` is not this track's. That kernel is unchanged, and `make -C surfcpu test-os-qemu` still ran. Adding it later is one word in
`SHARED_C`, the guard off, and a line in `arch/riscv32/kshell.c`.

## Left out, and why

- **A long name twice in a directory.** Short names are compared exactly; long names are not.
- **The boot sector's label against the root's.** `fsck.fat` looks at this and this does not: it is not on the list and touches no data. It
  is the one verdict difference allowed the other way, and the test enforces that it is the only one.
- **Reconnecting a lost directory.** It is saved as a file, like any chain.
- **Growing the root for FOUND.nnn**, and **adding to a FOUND.nnn that exists.** Both are safe in principle and neither is needed.
- **A check in `tools/qemu-usb.py`.** Not on this track's list of files. A guest has no way to damage a volume, so the smoke block cannot reach a
  repair. A run that does was made by hand and is not committed (see the gates below). *The integrator may want it as a scenario in `tools/qemu-usb.py`.*

## Tests

`tests/host/test_fsck.c`, run by `make unittest`, builds `fs/fat_check.c` as it is but for its allocator, which the test counts and refuses, with the
real `fs/fat.c`, `fs/bcache.c` and `fs/fat_mkfs.c`. The driver, not the checker, says what a file holds, and a small reader of the test's own
computes from the raw image what each entry's chain holds, so a repair that emptied every file would fail.

**The counts, on the tree committed, in the devenv:** 12704 checks, 0 failures: 174 scenarios across FAT12 with one FAT, FAT12 with two, FAT16 and FAT32, 999 pulls, and a fuzz of 200
images in `make unittest`. It takes about 160 s of the devenv's emulated time on a loaded machine: a FAT32 image is 35 MB, and the brief asks for a pull at every write, so it was not cut
further. `test_fsck <seed> <images>` runs more by hand.

**The owner's 118 GB stick, measured in the 32-bit build the kernel uses:** 3865664 clusters, read in pieces of at most 128 blocks (64 KB), 1809 KB of memory, 1066 device requests for a
look at a volume with a chain over half of it. The FAT is synthetic there: the device stores nothing, so what this measures is memory and request size, not a real stick's speed.

**The brief's proofs, and where each is:**

| Promise | Proof |
|---|---|
| A read-only run never writes | It is given no write function. In every scenario and on every fuzzed image, the image is compared byte for byte. A run not asked to repair, handed a write function, is checked too. |
| A repair never deletes data | `write_judged()` judges each write as it is made: never the boot sector or backup, no FAT entry in use set free, no data cluster in use written, in a directory block only an entry's first cluster and size or one free slot. After it the driver and the raw reader compare every file. Exact results are pinned per scenario, and `FOUND`'s bytes. |
| A pull at any write is no worse | Each scenario's repair is pulled after each of its writes on FAT12 (one FAT and two), FAT16 and FAT32; a third of the pulls with the device's cache losing what was not flushed; then repaired again, which must end where the whole repair ended. |
| No damaged volume panics the kernel | The fuzz: blows to FATs, directories, sectors, the boot sector and FSInfo, on all three types, with a bound on device requests, reads that fail, allocations refused one at a time, devices cut short. |

**What the fuzz found.** Worth reading, because it is why three rows of the table above exist. A larger sanitizer campaign found a repair taking names off a
volume: the root's second cluster, marked free, held real entries; the driver read them, the walk stopped at the bad link, and the cut hid every one of them. Nothing
was freed and no byte changed, and the entries were gone all the same. The same log showed a file with its directory bit flipped being walked as a directory. Both are
now refused and each has a scenario (`dir-free-link`, `dir-bit`, `file-bit`).

**What reading it found.** No test found these two; reading the finished file through did. The line that says what a repair will change was built with `n += snprintf()`
in a 200-byte buffer of the kernel's stack, and the pieces come to about 219 bytes with ten-digit counts: `snprintf` returns the length it wanted, so the next size wrapped, and a write
ran past the buffer on a sufficiently damaged volume. `clause()` now appends from the string's own length and cannot pass the end. **No test reaches this**, because no image here
has such counts: it is safe by construction, not by a test, and a reviewer should read `clause()`. The other: every write dropped the FAT's windows, so a copy read 64 KB of FAT per
cluster. That one has a test (`copy_cost`), which fails at 396697 blocks for a 3000-cluster copy without the fix.

**Do the tests have teeth?** Yes, and it was checked, because a test that passes proves nothing until it has been seen to fail. 30 deliberate breakages of `fs/fat_check.c`, one promise each
(linking a copy before marking it, a cut that frees, no mark before repairing, no flush, FOUND named before its clusters are marked, a disputed FAT trusted, a walk not whole repaired, and so
on), each compiled in and the whole test run: **30 of 30 caught**, on the tree committed. They did not all start that way, and the survivors are worth knowing. One showed a scenario passing for
the wrong reason (a zeroed FAT sector that also hit a directory, so it was refused as a walk not whole, and the rule meant to be tested never ran); others showed a rule added in a hurry with
nothing testing it; one showed the test checked that nothing was lost without ever saying what a repair must leave, which is why each scenario now pins the exact clusters and size.

**Sanitizers.** At the coordinator's correction: no `-fsanitize` binary runs in Docker (one hung at 99% CPU under `qemu-i386`) and none enters `make
unittest`. `test_fsck` was built natively on the Mac with clang, `-fsanitize=address,undefined -fno-sanitize-recover=undefined`, and run under a time limit:
the whole test, and 148000 fuzzed images over 142 seeds on the tree committed: no failure, no sanitizer report. (Before that, 24000 images on earlier trees, which is what found the problems above.) The
flags to do it again: compile `lib/blibc` (`vsnprintf.c` with `-DSURFOS_RISCV32`, for its division), `fs/bcache.c`, `fs/fat.c`, `fs/fat_mkfs.c`,
`fs/fat_check.c` (with `-Dkalloc=fsck_kalloc -Dkcalloc=fsck_kcalloc -Dkfree=fsck_kfree`) and `tests/host/test_fsck.c` with `-O1 -g -Itests/host/pager_shim
-ffreestanding -fno-builtin -nostdinc -Iinclude -include tests/host/surf_names.h -fgnu89-inline` and link with `-fsanitize=address,undefined`. It found a use after
free in a message on the path of a copy that does not read back, which is fixed.

**Against `fsck.fat -n`** (dosfstools, in the devenv; not on the Mac, which says so and skips it): every volume this repairs to clean, it finds clean too. On
2700 fuzzed images, 483 of them also judged by `fsck.fat`: 50 both clean, 427 both found problems, 3 this one only, 3 `fsck.fat` only. In `make unittest`'s smaller fuzz: 46 judged, no difference.

The differences, none of them a check this lacks:

- **Past the end marker.** `fsck.fat` reads on past a directory's `0x00` slot; `fs/fat.c` stops there, as the FAT specification says. A stray byte in those slots is a bad name to
  it and nothing to the driver. So its copy of a fuzzed image has those slots cleared first (`zap_dir()`), and the comparison is about the volume. The other way round: a
  slot zeroed in the middle of a directory hides the files after it from the driver, and this reports their clusters as lost where `fsck.fat` still sees them. That is right
  for every reader that stops at the marker, and those entries are safe: `root_slot()` takes a slot at the marker only when the next one ends the directory too, so nothing
  is written over them, and saving the clusters in `FOUND` deletes nothing.
- **`fsck.fat` only:** the boot sector's label, above. The test fails on any other.
- **This only:** a long name's checksum (`lfn-sum`, the only scenario of the whole table it passes and this does not) or order, where `fsck.fat -n` exits 0; a first byte of the boot
  sector that is no jump, which `fs/fat.c` refuses to mount; a media byte that does not match FAT[0].

## The gates

All on the tree committed but for this account, which reports them. A checksum of the ten source files was taken before them and compared after each.

| Gate | Result |
|---|---|
| `make unittest` | Status 0 |
| `make test-all`, through the lock | O0: smoke 545, Files 51. O2: smoke 545, Files 51, in a run of its own after the first failed (below). Two runs, one tree: the checksum above held across both |
| `make -C surfcpu test-os-qemu` | 137 of 137 at O2 and at O0 |
| `tools/qemu-usb.py --test --scenario storage` | 39 of 39 at O2 and at O0, on USB images of this tree |

The USB images were built with `./devenv/build.sh O=<n> usb-image` rather than `devenv/build-usb.sh`, which first fetches Wi-Fi firmware from the internet: the brief allows no host outside
the harness, and the storage scenario does not need it. Those images hold no Wi-Fi firmware.

**A real repair through the real kernel, by hand.** A guest cannot damage a volume, so the smoke block never reaches a repair. So a copy of `foreign-fat32.img` was damaged on the Mac (the clean
mark, FSInfo, a 1 MB twin of `BIG.BIN`, a lost chain of three clusters; `fsck_msdos` confirmed each of those before the run) and attached as an emulated USB stick, through xHCI and USB storage.
48 of 48 checks at O2 and at O0, on one sequence: a look; a wrong answer; Ctrl+C at the question, with the device let go of; `-r` refused while mounted; the repair; the two names reading the same
SHA-256; the lost chain in `FOUND.000`. Then, on a second copy: Ctrl+C in the middle of a repair, and QEMU's `device_del` in the middle of another. The pull landed mid-copy at both levels:
`+USB: usb1: read of block 5644 failed (device gone)`, then `cluster 1580 cannot be read: no copy`, status 2, no panic (a first run had it fail on a write instead). Plugged in again, `fsck -r`
finished it. **macOS's `fsck_msdos` and `fsck.fat` found no error on either stick afterwards.** It was run through `tools/qemu-usb.py`'s own classes by a script outside the tree, and it is not a
repeatable test of the tree until someone adds it: a pull's moment is timing, and the script's first run failed one check through an error of its own (the order it looked for names in).

**Something that went against the plan.** This track's slot for `test-all` arrived while a USB image was finishing on the same Docker volume, because the waiter ahead of it left the queue, so the two
overlapped by some seconds. The tree did not change and that build writes only `build/usb`.

**A failure of `test-all` that is not this track's, as far as can be told.** The first run passed O0 whole (smoke 545, Files 51) and failed O2 at 2 of 540: `@boot settingsdisk=shutdown kbd=irq` and the check of
the stick after it. That boot's driver line says `ata(absent)`: QEMU's IDE disk was not found, so `/hda1` was not there and `echo key > /hda1/CADEL.TXT` could not work, and the second failure follows from the
first. The load was about 20. This tree changes nothing in the ATA driver or the boot, and `fsck` does not run in that block. Run alone on the same O2 kernel (`block.py ... 'shutdown'`, with `PYTHONPATH=tools`
for its `import mkusb`) the block passes, 11 of 11. That is why it is called load, and it is not proof: a real race in the ATA probe under load would look the same. It may be worth a look by whoever owns
that probe.

## An incident, for the record

This track's first probe ran a `gcc -m32 -fsanitize=address,undefined` binary in the Docker image, before the coordinator's correction. It hung, as the
correction describes, at 99% CPU under `qemu-i386` for about 15 minutes on the shared machine. It was found when the correction came, confirmed as this track's by its
`docker inspect` mount, and killed by ID. Nothing else in this track ran a sanitizer in Docker.
