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
