# Brief: USB storage, partitions, FAT32, a C library and a text editor

Approved by the owner on 2026-10-05. SurfOS (branch `ai-dev`) boots on the Lenovo
IdeaPad 3 15IIL05 from a USB drive and reaches the network through a USB Ethernet
adapter. This brief adds stateful files: a second USB drive (a data stick, never the
boot drive) holds a FAT32 file system that both SurfOS and macOS can read and write.

**Status, 2026-10-06:** phases 1 to 5 are done on `ai-dev` and pass under emulation
(`make test-usb` scenarios `usbdisk` and `storage`, with macOS `fsck_msdos` and `fsck.fat`
checking the sticks); the test on the laptop is pending. The user guide is
[USB-BOOT.md section 9](../../USB-BOOT.md#9-files-on-a-usb-stick), the design
[ARCHITECTURE.md section 16](../../ARCHITECTURE.md). The device names below were written
before the code: drives are named in the order they are found, so with the boot drive as
`usb0` (as in the tests) a data stick is `usb1`.

## Decisions (the owner's)

| Question | Decision |
|---|---|
| File system | **FAT32** (with long names). exFAT is not part of this work. |
| Mounting | **By hand**: `mount usb0p1 [path]`, `umount`, `eject`. Nothing mounts a stick by itself on the laptop. |
| New partition tables | **MBR** by default; GPT on request (`fdisk usb0 new gpt`). Both are read. |
| Text editor | **Yes**: a small full-screen editor, `edit <file>`, in user space. |
| Screen | **More text, same letters**: the framebuffer console grows its grid to fill the panel (about 120x33 on a 1920x1080 panel) instead of centring 80x25. The VGA text mode (QEMU `-kernel`) stays 80x25. No graphics driver. |
| Order | Phases 1-5 below; phase 6 (exFAT) is not done. Independent parts are built in parallel. |

## Safety model (non-negotiable)

1. **Internal disks stay invisible**: no SATA/AHCI or NVMe driver is added. If one ever is, it
   registers its disks read-only.
2. **One gate for every destructive operation**: `bdev_may_format()` (driver/bdev.c). `fdisk`
   (any write) and `mkfs` call it and stop with its reason. It allows only a disk on the USB bus
   or a ramdisk (RAM only, what the tests use), never one with `protect` set (the SurfOS boot
   drive), never a read-only, unplugged or mounted one (the disk or any of its partitions).
   Every bus value other than USB and RAM, including 0 (a driver that did not say), is refused.
3. **The boot drive is protected**: a USB disk whose partition carries the FAT volume label
   `SURFOSUSB` (what tools/mkusb.py writes) gets `protect = "the SurfOS boot drive"`. It may be
   mounted read-only (`mount -r`).
4. **Confirmation**: before writing, `fdisk` and `mkfs` show the device (model, size, current
   partitions or file system) and the user types the device name to go ahead.
5. Nothing writes to any disk unless the user typed a command that says so. The tests write only
   image files and RAM.

## Phases

1. **USB sticks as disks**: a USB mass storage driver (Bulk-Only Transport, SCSI: INQUIRY, TEST
   UNIT READY, READ CAPACITY 10/16, READ/WRITE 10, REQUEST SENSE, SYNCHRONIZE CACHE, reset
   recovery, several LUNs, "no medium"), fast synchronous bulk transfers in the xHCI driver,
   disks `usb0`, `usb1` with partitions `usb0p1`, unplugging (`bdev_unregister`).
2. **Classification and partitions**: `lsblk` with bus, size, removable and protection; GPT
   reading (both headers, CRCs), MBR logical partitions; boot drive detection; `fdisk`
   (show, `new mbr|gpt`, `add [size] [type]`, `delete N`, `type N T`) behind the gate.
3. **FAT32 that macOS accepts**: `mkfs` (FAT32, FAT16 below 512 MB), FSInfo, the clean-shutdown
   flag, rename, UTF-8 names, statfs, faster I/O; `mount [-r]`, `umount`, `eject`, `df`.
4. **C library and programs**: system calls 22-31 (include/surfos/syscall.h), a small libc
   with standard headers (unistd, fcntl, sys/stat, dirent, stdio with buffering, stdlib,
   string, ctype, time, errno), programs (`ls -l`, `cp`, `mv`, `rm`, `mkdir`, `touch`, `df`,
   `wc`, `head`, `hexdump`), redirection in `sh` (`>`, `>>`, `<`), the editor `edit`.
5. **Verification and docs**: an emulated data stick in tools/qemu-usb.py (format, files,
   unplug while mounted, refusals), host checks with `fsck.fat`, macOS `fsck_msdos` and mtools,
   the reverse direction (a host-made FAT32 volume read by SurfOS), the USB guide and the
   hardware record.

## Tracks (built in parallel, merged into `ai-dev` in this order: A, E, B, C, D)

Each track works in its own git worktree on its own branch, starting from the foundation
commit, with its own Docker volume: `SURFOS_VOLUME=surfos-<track> devenv/build.sh ...`.

| Track | Branch | Phase | Owns (may create and change) |
|---|---|---|---|
| A | `ai-dev` itself | 1 | `driver/usb/*` (new `msc.c`), `include/sys/usb.h` |
| B | `fs/b-partitions` | 2 | `driver/mbr.c` (becomes the partition code; may be renamed `part.c`), new `shell/fdisk.c`, `bdev_print()` in `driver/bdev.c`, new `tests/host/test_part.c` |
| C | `fs/c-fat32` | 3 | `fs/fat.c`, new `fs/fat_mkfs.c`, `fs/bcache.c`, new `shell/fscmd.c` (mkfs, mount -r, eject, df), `cmd_mount`/`cmd_umount` in shell/main.c, new `tests/host/test_mkfs.c` |
| D | `fs/d-libc` | 4 | `kernel/syscall.c`, `kernel/process.c`, `user/**` (library, headers, programs, `edit`, `sh`) |
| E | `fs/e-console` | (screen) | `kernel/console.c`, `kernel/tty.c`, `include/surfos/console.h`, `include/surfos/tty.h`, `driver/fbcore.c`, `driver/framebuffer.c`, `include/sys/framebuffer.h`, `tests/host/test_framebuffer.c`, the screen reader in `tools/qemu-usb.py` |

Shared files (anyone may add to them; keep the change small, it is merged by hand): the command
table in `shell/main.c`, `SMOKE` in `tools/qemu-run.py`, `Makefile` (new sources and tests),
`include/fs/vfs.h` (C only), `include/sys/bdev.h` (B and A only, additions only).

## Interfaces fixed by the foundation commit

* **Block devices** (`include/sys/bdev.h`): `bus` (`BDEV_BUS_OTHER` 0, `RAM`, `ATA`, `USB`),
  `removable`, `protect`, `gone`, `refs`, `part_index`, `ops->flush`; `bdev_unregister()`,
  `bdev_get/put()`, `bdev_disk()`, `bdev_part_name()` (`usb0` + 1 = `usb0p1`), `bdev_flush()`,
  `bdev_may_format()`. Unplugged devices are never freed.
* **VFS** (`include/fs/vfs.h`): `vnode_ops.rename`, `superblock_ops.statfs`, `struct statfs`,
  `MNT_RDONLY`, `vfs_mount_flags()`, `vfs_rename()`, `vfs_statfs()`, `vfs_ftruncate()`,
  `vfs_fsync()`, `vfs_bdev_busy()`. The VFS side is done; FAT's `rename` and `statfs` are
  track C's (until then they return `-ENOSYS` or zeros).
* **System calls 22-31** (`include/surfos/syscall.h`): numbers and arguments fixed; the kernel
  side and the library wrappers are track D's.
* **Terminal** (`include/surfos/tty.h`): `tty_ctl(TTY_RAW, 0|1)`, `tty_ctl(TTY_GETSIZE)`, the key
  sequences of raw mode and the ANSI subset the console draws are specified there. Track E
  implements them (the foundation's stub returns the size and `-ENOSYS` for raw mode); track D's
  editor uses only that interface.
* **Tests**: `tools/qemu-run.py --scratch MB` adds a blank module, the ramdisk `rd1` (RAM only),
  for fdisk and mkfs tests under QEMU `-kernel` (give the VM enough memory, `--mem 256`).

## Rules for every track

* Read `CLAUDE.md` and `docs/README.md` first; follow the conventions (2004 file headers on
  new files with today's date, the comment density of the surrounding code, kprintf in the
  kernel).
* Every commit builds and passes `make test-all` and `make unittest`; a commit that touches the
  boot path, memory, console, timer, keyboard, the USB stack or anything `profile=laptop` uses
  also passes `make test-usb` at -O0 and -O2. Add SMOKE steps (tools/qemu-run.py) and
  qemu-usb.py checks for what the shell can exercise.
* Small commits, one topic each, ending with the `Co-Authored-By` line. Commit on your own
  branch only; never push, never touch `master` or another track's branch.
* Do not edit the docs (`docs/`, `CLAUDE.md`, READMEs): the merge step updates them. Report
  what they should say.
