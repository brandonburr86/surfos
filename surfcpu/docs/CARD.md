# The microSD card: preparing and updating it from the Mac

The Arty boots SurfOS from a microSD card in the Pmod MicroSD adapter (on JA). The same card
holds the kernel and your files. You prepare and update it on the MacBook, in its SD slot or
a USB card reader, then move it to the board. How SurfBoot reads it is in
[FIRMWARE.md](FIRMWARE.md); the format rules are in [BOOT-ABI.md](BOOT-ABI.md) section 5.

**Status (2026-10-15):** `deploy-sd` has been tested in the container against a directory
standing in for a card, with simulated `diskutil` and `mount` answers (including a model of
the Mac's cache, in which the card itself changes only when it is unmounted), and its
argument handling with the Mac's Homebrew Python 3.14 and its `/usr/bin/python3` (3.9.6). It
has not been run on a real card yet: the first real use is part of milestone 5, which is
pending.

## 1. What is on the card

```
/BOOT/SURFOS.BIN      the kernel package SurfBoot loads (made by make build-kernel)
/BOOT/BACKUP.BIN      the previous package, kept by deploy-sd (used if SURFOS.BIN fails)
/BOOT/CMDLINE.TXT     optional: one line handed to the kernel
/SYSTEM/              system files; SURFCARD.TXT marks this as the SurfOS card
/PROGRAMS/            later: RISC-V programs
/USERS/DOCUMENTS/     your files
```

The card needs an **MBR (Master Boot Record) partition table with one FAT32 partition**.
SurfBoot refuses exFAT (how cards larger than 32 GB come from the shop), FAT16, GPT
partition tables and cards without a partition table, and says which it found.

## 2. Preparing a card (once)

First look at what the card has. Put it in the Mac and run:

```sh
diskutil list
```

Find the card by its size (a 32 GB card shows as about 31.9 GB) and note its disk name, for
example `/dev/disk4`. **Make sure it is the card**: the next steps would erase anything you
point them at. Then:

```sh
diskutil info /dev/disk4 | grep -E "Content|Device / Media Name"    # want: FDisk_partition_scheme
diskutil list /dev/disk4                                            # want: one DOS_FAT_32 partition
```

If the card already shows `FDisk_partition_scheme` with one `DOS_FAT_32` partition, skip to
section 3. Otherwise it has to be erased and formatted, which **destroys everything on the
card**. Either with Disk Utility: View, Show All Devices; select the card's device (the top
line, not the volume under it); Erase; Name `SURFOS`, Format **MS-DOS (FAT)**, Scheme
**Master Boot Record**; Erase. Or in Terminal, with the disk name checked twice:

```sh
diskutil eraseDisk FAT32 SURFOS MBRFormat /dev/disk4
```

Afterwards `diskutil info /Volumes/SURFOS` should say `File System Personality: MS-DOS FAT32`
(for cards over 32 GB, check that it does not say ExFAT). No tool in this repository ever
formats or partitions a card: this one step is yours.

## 3. Installing SurfOS

Build the kernel package, then install it, from the repository root:

```sh
make -C surfcpu build-kernel                                   # makes build/riscv32/arty_a7/SURFOS.BIN
make -C surfcpu deploy-sd MOUNT=/Volumes/SURFOS DEPLOY_ARGS=--new-card     # the first time
```

`--new-card` is needed only the first time: it writes `/SYSTEM/SURFCARD.TXT`, the marker by
which later updates recognise the card (so that a USB stick or another card is never written
by mistake). Expected output:

```
card: /Volumes/SURFOS (/dev/disk4s1, MS-DOS FAT32, SDXC Reader, Secure Digital)
package: .../build/riscv32/arty_a7/SURFOS.BIN (812345 bytes, SurfOS 0.1.0 build 1a2b3c4d, CRC 0x12345678): valid
create directory /BOOT
...
write /BOOT/SURFOS.BIN (812345 bytes, sha256 ...) via a temporary file
write /SYSTEM/SURFCARD.TXT (94 bytes, sha256 ...) via a temporary file
written: 2 file(s); unmounting and mounting the card again to read them back
verified 2 file(s), 812439 bytes, read back from the card after unmounting and mounting it again (so from the card, not from the Mac's memory)
done. Eject the card before you take it out: diskutil eject /Volumes/SURFOS (or the eject button next to it in Finder). Do not use it on the Mac and the board at once.
```

The card disappears from Finder for a moment near the end: `deploy-sd` unmounts it, which
makes the Mac write everything out and forget what it had cached, and mounts it again, so
that the comparison reads the card itself. If macOS mounts it under another name (such as
`/Volumes/SURFOS 1`), `deploy-sd` follows it and names it in the last line. Then **eject**
(`diskutil eject /Volumes/SURFOS`, or Finder's eject button) and only then take the card out.

## 4. Updating

After a new `make -C surfcpu build-kernel`:

```sh
make -C surfcpu deploy-sd MOUNT=/Volumes/SURFOS
```

The previous `SURFOS.BIN` becomes `/BOOT/BACKUP.BIN` (if it was a valid package and
different). Each new file is written under a fresh temporary name, flushed to the card and
then renamed into place. When everything is written, the card is unmounted and mounted again,
checked once more, and every installed file is read back from it and compared byte for byte
with what was meant to be written. Your directories and files are left as they are. The last
lines say how it went, and so does the exit status:

| Exit | Meaning | What to do |
|---|---|---|
| 0 | written, then read back from the remounted card: every byte matches | eject, take the card to the board |
| 1 | refused (the reason is printed): nothing was written | fix what it says |
| 2 | **written but NOT verified**: the card could not be unmounted or mounted again (or an error stopped the update part way) | eject the card (or take it out if the Mac no longer shows it), put it back, run the same command with `--verify-only` added |
| 3 | read back from the card and found **different** | deploy again; if it repeats, the card or the reader is failing (SurfBoot would refuse a damaged `SURFOS.BIN` by its CRC and boot `BACKUP.BIN`) |

Options go in `DEPLOY_ARGS`:

| Option | Effect |
|---|---|
| `--dry-run` | check everything and print what would be done; write nothing |
| `--verify-only` | write nothing: unmount and mount the card again and compare it with the files the same command would install |
| `--new-card` | the first install: write the card marker `/SYSTEM/SURFCARD.TXT` |
| `--cmdline "console=uart"` | write `/BOOT/CMDLINE.TXT` with this line (`--cmdline-file F` copies a file) |
| `--no-backup` | leave `/BOOT/BACKUP.BIN` as it is |
| `--backup-image PATH` | install this package as `/BOOT/BACKUP.BIN` |
| `--image PATH` | install another package (for example `build/fw/DDRDIAG.BIN`; paths are relative to `surfcpu/`) |
| `--system-dir DIR` | copy the files of DIR into `/SYSTEM/` |

Before writing anything `deploy-sd` checks, and refuses with the reason if one fails:

* the mount point you named is a mounted FAT32 volume, writable, on removable media: the
  built-in SD slot or a USB card reader counts; a fixed internal or external disk, a disk
  image or another virtual device does not;
* `diskutil` names the card's whole disk, and it has an MBR partition table;
* the folder it writes through is on that very device (its device number is compared with
  the device node `diskutil` reports);
* nothing it would use on the card is a symbolic link, and `/BOOT` and `/SYSTEM` hold none
  (macOS shows FAT "XSym" files as links; following one could write outside the card);
* the package would pass SurfBoot's checks (an x86 `surfos.bin`, an ELF, a disk image or a
  package without the base ISA is refused);
* the card carries the marker (unless `--new-card`), and there is room.

Every folder and file on the card is opened relative to the folder above it without
following links, and new files are created under fresh names that must not exist yet.

## 5. Booting

Put the card in the Pmod MicroSD on the Arty, connect the USB cable, open the serial port
(115200 baud, 8N1; for example `screen /dev/cu.usbserial-XXXX 115200`), and program or
power the board. SurfBoot prints what it does (FIRMWARE.md section 5) and the VGA monitor
shows the same text. The LEDs: LD0 blue while SurfBoot runs, green once the kernel starts.

**If it stops**, it says why on the serial port and the screen, shows a code on the green
LEDs LD4-LD7 with LD0 red, and waits for commands (type `help`). Common cases:

| Message (error name) | LED code | What to do |
|---|---|---|
| no card (NO_CARD) | 2 | insert the card: SurfBoot retries by itself |
| card does not answer / initialization failed (SD_NORESP, SD_INIT) | 3 | reseat the card and the Pmod; try another card |
| GPT partition table, no partition table, exFAT, FAT16 (GPT, SUPERFLOPPY, EXFAT, FAT1216) | 4, 5 | format the card as in section 2 |
| /BOOT/SURFOS.BIN does not exist (NOT_FOUND) | 6 | run `deploy-sd` again |
| payload checksum (IMG_CRC), package size mismatch (IMG_SIZE) | 7 | the file is damaged or incomplete: deploy again; if it keeps happening the card may be failing |
| DDR calibration failed or timed out (DDR_CALIB, DDR_TIMEOUT) | 1 | a hardware or bitstream problem: run the BRAM diagnostics |

When `SURFOS.BIN` fails a check, SurfBoot boots `BACKUP.BIN` by itself and tells the kernel
(boot flag bit 0). **Hold BTN0** while the board starts to boot `BACKUP.BIN` on purpose (for
example when a new kernel boots but misbehaves); **hold BTN1** to stop in SurfBoot's monitor.

## 6. The DDR diagnostic

To test the board's memory from the card (milestone 5), install the diagnostic package
instead of the kernel:

```sh
make -C surfcpu build-rom
make -C surfcpu deploy-sd MOUNT=/Volumes/SURFOS DEPLOY_ARGS="--image build/fw/DDRDIAG.BIN --cmdline full"
```

(`make -C surfcpu` runs in `surfcpu/`, so paths in `DEPLOY_ARGS` are relative to it.)

or keep the kernel and copy it next to it (`cp surfcpu/build/fw/DDRDIAG.BIN /Volumes/SURFOS/BOOT/`),
then start the board with BTN1 held and type `boot /BOOT/DDRDIAG.BIN`. Without `full` on the
command line it runs the quick tests (seconds); with it, every word of DDR (minutes). It ends
with `DDRDIAG: PASS` or `DDRDIAG: FAIL` and the failing addresses. `CMDLINE.TXT` stays on the
card: when you go back to the kernel, deploy with `--cmdline ""` (or the kernel's own line).

## 7. Good to know

* Never have the card mounted on the Mac and in the board at once, and always eject on the
  Mac before taking it out. FAT32 has no journal: pulling the card out during a copy can
  damage the file system, and `BACKUP.BIN` does not make an update safe against that.
* macOS adds hidden files and folders (`.Spotlight-V100`, `.fseventsd`, `._*`). They are
  harmless: SurfBoot reads only `/BOOT`.
* SurfBoot reads the card only; the kernel takes it over after the handoff and writes to it
  once its write path is validated.
* For tests, card images are made with `surfcpu/tools/mkcard.py` (regular files only):
  `python3 surfcpu/tools/mkcard.py card.img --standard-dirs --file /BOOT/SURFOS.BIN=build/riscv32/arty_a7/SURFOS.BIN`.

## 8. Writing from SurfOS

**Status (2026-10-16):** tested in simulation only, not on a real card or the board (pending):
on the RTL system in surfsim, with the io track's card model (`make -C surfcpu
test-os-persist`: files written, ejected, read back after a second boot, the image checked by
`fsck.fat` and mtools), and under QEMU with the same driver and a card model that fails on
request (`make -C surfcpu test-os`: refused writes, a card that stops answering, the card
taken out). How it works and the test results are in [PORTING-RISCV.md](PORTING-RISCV.md)
section 6.

**Allow it.** SurfOS mounts the card read-only unless `/BOOT/CMDLINE.TXT` holds the word
`sd=rw`:

```sh
make -C surfcpu deploy-sd MOUNT=/Volumes/SURFOS DEPLOY_ARGS="--cmdline sd=rw"
```

Or, at the prompt: `umount /sd0p1`, `sdmode rw`, `mount sd0p1`. SurfOS never partitions or
formats the card: do that on the Mac (section 2).

**Write.** Every shell command that changes files (`write`, `append`, `cp`, `mv`, `rm`,
`mkdir`, `rmdir`) puts everything on the card before the prompt comes back, also when another
program writes to the card at the same time (since 2026-10-16; before, such a command could
report success with its folder entry still in memory). If the card refuses, the command says
`... not all of it reached /sd0p1: I/O error` and fails ($? 1); what it wrote stays in memory,
and `sync` tries again. SurfOS keeps at most a quarter of its block cache (128 KiB) of file
data waiting for a card that refuses (each command's own FAT and folder blocks come on top of
that: a few); past that, further writes to it fail at once with `I/O error`, and nothing
already kept is dropped. It asks the card again at each command, so once the card takes
writes again the next command works, without a `sync` first. A write the card refuses leaves
the file's size as it was (also one past the end of a file: the gap of zeros comes with the
bytes or not at all). A command that writes less than it was given changed what it says it
wrote and, where it wrote over the file's existing bytes, possibly some of the next 64 KiB as
well: a card may store the first blocks of a request before it refuses one (SurfOS then reads
those blocks as the card holds them, before and after it makes room in its cache, except blocks
with changes of its own still waiting to be written: those show the waiting changes, which the
next sync writes over the card's). An `mv`, `rm` or `mkdir` the card refuses changes nothing
(the file keeps its one name and its bytes; a folder not made leaves no name, and its space
comes back once the card holds that), so the same command later is safe. While blocks of one
file refuse, an `rm` of that file works (its unwritten bytes go with it, however many, refused
before or not yet tried), but an `mv` onto that file, and `rm`, `mv`, `mkdir` and truncations
of other files on the card, fail with `I/O error` until the card takes those blocks: they must
not reach the card before bytes they might point at (to replace such a file, `rm` it first).
Bytes nobody will read (past the end of a copy that stopped, a gap of zeros given up, the new
cluster of a folder that could not grow) are dropped, never written, and hold nothing back; one
that another program or the cache was writing at that very moment is dropped once that write
ends (since 2026-10-17). The one exception: an `rm` of a file whose block another program or
the cache was writing at that very moment can fail once with `I/O error` (nothing changed; the
same `rm` then works). A `sync` waits about 10 seconds for a program that holds part of what it must
write and does not let go, then says `I/O error` for that part; later syncs say so at once. Other
programs deleting, cutting or moving files at the same time do not make a `sync`, or a command's
own writing out, fail on a card that takes every write: it waits until they pause (since
2026-10-17; before, enough of them could make it say `I/O error`).

**Stopping a command.** Ctrl+C, or a `kill` of the program, while it waits on the card (for a block
another program holds, or for another program's write) ends the wait at once: the command fails
(`I/O error` from a `sync`, `Interrupted system call` from a write), and what it had not written
stays in memory for the next `sync`. One reading or writing a large file stops before its next piece
and says how much it did (it wrote what it says: `cp` says `cp: cut short after N bytes: <file> is
incomplete`, and `fsbench` says `fsbench: interrupted`, since 2026-10-18; before, `cp` said `short
write`, its message for a full card, and `fsbench` `I/O error`; and a full or refusing card is said
as that, Ctrl+C or not, `cp: write: No space left on device` or `I/O error`, N counts every byte
written, and a copy whose last piece went whole is complete, `N bytes copied` with status 0, since
later that day): a piece is one 512-byte block where the bytes go through SurfOS's cache, up to 64
KiB where they go straight to the card (until 2026-10-17 this said 64 KiB for both). One in the
middle of a request to the card (a write of its own, or `mkfs` and `fdisk` writing the card
directly) ends when that request does (a card that stops answering times out), and one in the middle
of changing a folder or the FAT finishes that change first: stopped at once there, it left the card
locked for every other program and SurfOS's records of its writes in a mess (since 2026-10-17).
`mkfs` stopped says how far it got (`interrupted at sector ...`: the card holds no usable file
system then, so make it again) or that it wrote nothing. `umount`, and `eject` once it unmounts, are
not stopped: they finish as they would have and give up nothing the card takes (until 2026-10-17,
Ctrl+C or a `kill` then gave changes up and left a working card marked as not cleanly unmounted),
and their status is then their own: 0 after `safe to remove` (until 2026-10-18 it was 130, the
status of a command Ctrl+C cut short). They wait for another program's write to the card as long as
it takes: the card's driver ends every request on its own (it waits at most 500 ms for the card
before a command and after each block it writes, so a 64 KiB write takes at most about a minute),
and a fault inside SurfOS while a program is in the middle of the card's file system or of a request
to the card stops SurfOS (`Kernel Wipeout`, `inside a file system`) rather than leave the card
locked (since 2026-10-18; earlier that day they gave such a write up after 30 seconds, and the write
could then land on the card after it was formatted again). While a card is being unmounted, `mount`,
`eject` and `sdmode` refuse it and say it `is being unmounted`: type the command again once the
umount has finished (since 2026-10-18; before, they could take the card while the umount still wrote
to it, and `eject` could say `safe to remove` too soon); `lsblk` shows it as `being unmounted`.
`mount` and `sdmode` refuse it as well while it is being mounted (`being mounted`) or a request to
it has not ended (`in the middle of a request`), and `eject` holds the card from its first step: a
`mount` typed while `eject` still writes the card out is refused (since 2026-10-18). `eject` stopped
while it still writes the card's files out unmounts nothing and says `interrupted: it stays
mounted`: type it again. A program that is ending by itself still writes its files out. SurfOS's own
tasks (`sdwatch`, which watches the card slot) cannot be killed: `kill` says so (since 2026-10-18;
before, a `kill` of it while it held the card's lock left every later request to the card waiting
for good).

If a failed `mv`, `rm`, `mkdir` or truncation cannot be undone (the card refuses to read back,
in the middle of putting things back, a block the command had changed; a block it could not read
in the first place it never changed, and there is nothing to put back), SurfOS says
`+FAT32: sd0p1: a rename that failed could not be undone (...): read-only until unmounted`,
refuses every further change on that volume (`Read-only file system`), and leaves it marked as
not cleanly unmounted: unmount it, and check the card on the Mac (Disk Utility, First Aid) before
writing to it again (the file may be under two names, and deleting either would free the other's
bytes).

**Before taking the card out or switching off**, type `eject sd0` and wait for
`sd0: safe to remove`. If the card refuses the last writes, eject says `sd0 stays mounted`:
type `sync` to try again, or `umount /sd0p1` to give those changes up (nothing of them is written
after that, also when another program's `sync` was writing then: since 2026-10-17).

**Taken out while mounted** (or the board switched off): SurfOS says so within half a second
(`+SD: sd0: the card was taken out`, and for a read-write mount that what was not yet written
is lost), refuses every further read and write (`I/O error`), and keeps `/sd0p1` until you
type `umount /sd0p1`. Put the card back and type `mount sd0p1`: it says `was not unmounted
cleanly`, and does so at every mount until the card has been checked on the Mac (Disk Utility,
First Aid): SurfOS's own unmount leaves that mark (and says so), since only a check can tell that
nothing was lost (since 2026-10-17; before, the next unmount cleared it). A card taken out and put back in less than half a second is not noticed: it lost its
power and answers nothing, so every read and write fails until it has been out for a moment.

**What FAT32 cannot make safe.** FAT32 keeps no journal, so a card that comes out in the
middle of a write leaves the volume half-changed. SurfOS writes in an order chosen so that the
damage stays small: a file's bytes first, then the FAT (both copies), then the directory
entry, then the free-space count; when something is deleted, the entry goes before its
clusters are freed; when a file is cut shorter, its new end goes before the clusters past it
are freed; and when a file or folder grows into another sector of the FAT, that sector is
written before the link into it. It is an order, not atomicity. Tested by taking the card out
after each block of a write, `rm`, `mv` and an overwrite, the most `fsck.fat` found was:

* the "not cleanly unmounted" mark;
* clusters marked used that no file points at (space lost until a check frees it), or the
  two FATs differing;
* a wrong free-space count;
* a file moved into another folder under both names (`fsck.fat` keeps the first);
* when the card refused writes in the middle of a large copy: the copy's last clusters
  allocated beyond its size.

Never, in those tests, a file pointing at free space or at bytes that were not written. The
host test of the same code (`tests/host/test_fatfs.c`, `make unittest`) checks the image after
every block written, for writes, deletes, renames, truncates, folders that grow and two tasks
writing at once, and finds none either; nor do thousands of random operations on a card that
refuses writes and reads at random (`tests/host/test_fatfuzz.c`), nor several tasks at once on
real threads (`tests/host/test_fatconc.c`; since 2026-10-16 also with six writers on a small
card, the setting in which a review found one such moment: a copy cut back while the card
refused, its last clusters freed before its new end was on the card; fixed, they are freed
once it is). Since 2026-10-17 the last two also check, after every block written, that no file
shows bytes it never wrote: each block written carries a mark naming its file, and a block in a
file with the mark of a file that never had that name there is reported; a review had found, by
a targeted test, a moment when a folder block could reach the card naming a file just deleted
by another program over a block that file never wrote (fixed; with the fix taken out, the threaded
test reports it in 7 of 16 runs of its streaming setting). One narrow case is left: FAT12's entries that straddle two sectors (FAT12 is for
floppies, never a SurfOS card). A copy that stopped because the card began to refuse keeps
its last clusters linked past its end until a sync works (pulled before that, they are space
lost until a check frees it). SurfOS keeps up to 64 such runs of clusters waiting to be freed;
a 65th (65 copies cut short while the card keeps refusing) it cannot free, and says so:
`... cannot wait to be freed ...: lost until the volume is checked (fsck.fat)`, and again when
the card is unmounted or ejected (`... may stay allocated with nothing pointing at them`); the
card is then left marked as not cleanly unmounted (since 2026-10-17), so the next mount, and
the Mac, say it should be checked, and SurfOS's later unmounts leave it so until it is. Clusters
an `rm` or a truncation could not give back because the card refused to read or write part of the
FAT are counted and said the same way (`... could not be freed (the FAT could not be read or
written there): lost until the volume is checked`, "at least" when the FAT could not be read to the
chain's end), and the `rm` itself works: the file is gone (since 2026-10-17; before, it said `I/O
error` with the file gone, and nothing counted those clusters). A truncate cut short can also leave a file whose size runs
past its clusters. But:

* the file being written comes out either whole or not at all only if it is new: a file being
  overwritten (`write` over it, `cp` onto it) can be left empty, and one being appended to keeps
  its old bytes and may lose the new ones;
* SurfOS does not repair a volume; it mounts one that was not cleanly unmounted and warns.
  Check such a card on the Mac (Disk Utility, First Aid) before trusting it;
* the card itself may lose or damage the block it was writing when its power went: no file
  system can know.

**A full card** (tested 2026-10-16, in simulation only, like the rest of this section: on the
RTL system with a card made nearly full on the Mac, and under QEMU). A command that does not fit
stops when the space runs out and says so: `cp: write: No space left on device`, and `write` or
`mkdir` the same. The copy keeps what fitted (the first bytes of its source); delete it if you do
not want it. `df` shows what is left. Deleting files gives the space back at once. The card stays
consistent: `fsck.fat` found nothing to repair after a full card.

Since 2026-10-16 a copy that stops early (no space, or a card that refuses writes) also gives back
the clusters it had taken past what it wrote: the finding "the copy's last clusters allocated
beyond its size" in the list of what `fsck.fat` found no longer happened in the tests.
