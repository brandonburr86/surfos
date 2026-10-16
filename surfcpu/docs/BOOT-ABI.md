# SurfBoot boot ABI: the kernel package, the handoff and the card layout

What SurfBoot (the boot ROM firmware) and a kernel agree on. The constants come from
`[bootabi]` in [`spec/soc.toml`](../spec/soc.toml); the C structures are in
[`firmware/include/surfcpu/bootabi.h`](../firmware/include/surfcpu/bootabi.h); the packer is
`tools/surfimg.py`. Version 1, frozen 2026-10-15. This is a SurfCPU board ABI, not an SBI or
OpenSBI boot protocol.

## 1. Build artifacts

| Artifact | What it is |
|---|---|
| `surfboot.elf` | SurfBoot with symbols; `surfboot.mem` initializes the boot ROM in the bitstream and in simulation |
| `surfos.elf` | the RISC-V kernel with symbols, sections and debug information |
| `surfos.payload.bin` | `surfos.elf` as a raw image from its first loadable byte to the end of its initialized data (intermediate) |
| `SURFOS.BIN` | the package installed on the card: a 64-byte header followed by the payload |

RISC-V outputs live under `build/riscv32/arty_a7/` (kernel) and `surfcpu/build/` (hardware and
firmware), never in the x86 build directories. The x86 `surfos.bin` and the GRUB USB image are
never a RISC-V kernel and the tools refuse them (they have no `SIMG` header).

## 2. SURFOS.BIN header (64 bytes, little-endian, at offset 0)

| Offset | Size | Field | Value / rule |
|---|---|---|---|
| `0x00` | 4 | `magic` | `SURF_IMAGE_MAGIC` = `0x474D4953` ("SIMG") |
| `0x04` | 2 | `version` | `SURF_IMAGE_HEADER_VERSION` = 1 |
| `0x06` | 2 | `header_size` | `SURF_IMAGE_HEADER_SIZE` = 64 |
| `0x08` | 4 | `isa` | the ISA the payload needs: OR of `SURF_ISA_I` 1, `SURF_ISA_M` 2, `SURF_ISA_ZICSR` 4, `SURF_ISA_ZIFENCEI` 8 |
| `0x0C` | 4 | `platform` | `SURF_PLATFORM_ABI` (1): the handoff in section 4 |
| `0x10` | 4 | `load_addr` | where the payload's first byte goes; 4-byte aligned |
| `0x14` | 4 | `payload_size` | bytes of payload after the header; > 0 |
| `0x18` | 4 | `mem_size` | bytes the image spans once running, payload plus BSS; >= `payload_size` |
| `0x1C` | 4 | `entry` | first instruction; 4-byte aligned, inside the payload |
| `0x20` | 4 | `payload_crc32` | CRC-32 of the payload bytes (section 3) |
| `0x24` | 4 | `flags` | 0 (reserved) |
| `0x28` | 4 | `image_version` | the kernel's version, `major << 16 \| minor << 8 \| patch` |
| `0x2C` | 4 | `build_id` | first 8 hex digits of the source commit |
| `0x30` | 12 | `name` | ASCII, NUL-padded, e.g. `SurfOS` |
| `0x3C` | 4 | `header_crc32` | CRC-32 of bytes `0x00`..`0x3B` |

SurfBoot accepts an image only if all of these hold, and says which check failed otherwise:

1. The file is at least 64 bytes; `magic`, `version` and `header_size` are as above; `header_crc32` matches.
2. `isa` has no bit SurfBoot's CPU lacks (it has I, M, Zicsr, Zifencei); `platform` = 1.
3. The file is exactly `64 + payload_size` bytes (a truncated or padded file is refused).
4. `load_addr` is aligned; `mem_size >= payload_size`; the range `[load_addr, load_addr + mem_size)`
   computed without overflow lies inside the DDR that calibrated, and outside SurfBoot's own
   reservations (`ddr_top` in the memory map).
5. `entry` is aligned and inside `[load_addr, load_addr + payload_size)`.
6. The CRC of the payload as read from the card matches `payload_crc32`, and so does the CRC
   of the bytes read back from DDR after copying.

The image's BSS is not cleared by SurfBoot: the kernel's startup clears `[end of payload,
load_addr + mem_size)` from its own linker symbols.

## 3. CRC-32

CRC-32/ISO-HDLC, the CRC of zlib, PNG and Ethernet: polynomial `0x04C11DB7` processed
reflected (`0xEDB88320`), initial value `0xFFFFFFFF`, input and output reflected, final XOR
`0xFFFFFFFF`. Test vectors, which the C and Python implementations both check:

| Input | CRC-32 |
|---|---|
| empty | `0x00000000` |
| `"a"` | `0xE8B7BE43` |
| `"123456789"` | `0xCBF43926` |
| 256 bytes `0x00, 0x01, ..., 0xFF` | `0x29058C73` |

A CRC detects accidental corruption. It is not authentication: anyone can make a valid
package.

## 4. Handoff

SurfBoot jumps to `entry` with:

| State | Value |
|---|---|
| `a0` | hart ID (0) |
| `a1` | address of `struct surf_boot_info` (`SURF_BOOTINFO_BASE`, `0x0001F000`) |
| `a2` | `SURF_BOOTINFO_MAGIC` (a second check that `a1` is meaningful) |
| other registers | undefined; `sp` and `gp` must be set by the kernel before use |
| privilege | machine mode |
| `mstatus` | `MIE` = 0 |
| `mie` | 0; `intc.ENABLE` = 0; the timer's `mtimecmp` = all ones |
| `mtvec` | SurfBoot's fault reporter in ROM, which prints `mcause`/`mepc`/`mtval` on the UART and stops; the kernel replaces it early |
| memory | DDR calibrated; the payload at `load_addr` and verified; `FENCE.I` executed |
| UART | 115200 8N1, TX drained |
| SPI | chip select released; the card stays initialized in SPI mode at the fast clock |
| video | text mode showing SurfBoot's log; cursor wherever SurfBoot left it |

The boot information lives in scratch RAM at `SURF_BOOTINFO_BASE`; the kernel copies it before
it uses scratch RAM for anything else. Version 1, 256 bytes:

| Offset | Size | Field | Meaning |
|---|---|---|---|
| `0x00` | 4 | `magic` | `SURF_BOOTINFO_MAGIC` = `0x49465253` ("SRFI") |
| `0x04` | 2 | `version` | 1 |
| `0x06` | 2 | `size` | 256 |
| `0x08` | 4 | `hart_id` | 0 |
| `0x0C` | 4 | `board_id` | `sysinfo.BOARD_ID` |
| `0x10` | 4 | `soc_version` | `sysinfo.SOC_VERSION` |
| `0x14` | 4 | `map_version` | `sysinfo.MAP_VERSION` |
| `0x18` | 4 | `sys_hz` | `sysinfo.SYS_HZ` |
| `0x1C` | 4 | `timebase_hz` | `timer.TIMEBASE` |
| `0x20` | 4 | `ram_base` | DDR base (`0x80000000`) |
| `0x24` | 4 | `ram_size` | bytes of DDR SurfBoot verified usable |
| `0x28` | 4 | `image_base` | the header's `load_addr` |
| `0x2C` | 4 | `image_mem_size` | the header's `mem_size` |
| `0x30` | 4 | `boot_device` | `SURF_BOOT_DEVICE_SD` (1) |
| `0x34` | 4 | `part_lba` | first sector of the FAT32 partition the image came from |
| `0x38` | 4 | `boot_flags` | bit 0: the image was `BACKUP.BIN`; bit 1: the command line was truncated |
| `0x3C` | 4 | `surfboot_version` | SurfBoot's version, `major << 16 \| minor << 8 \| patch` |
| `0x40` | 4 | `surfboot_build_id` | SurfBoot's source commit (8 hex digits) |
| `0x44` | 4 | `bootinfo_crc32` | CRC-32 of this structure with this field as 0 |
| `0x48` | 56 | `reserved` | 0 |
| `0x80` | 128 | `cmdline` | kernel command line from `/BOOT/CMDLINE.TXT` (first line), NUL-terminated, empty if absent |

## 5. Card layout

The card has an MBR partition table with one primary FAT32 partition (type `0x0B` or `0x0C`)
and 512-byte logical sectors. SurfBoot refuses exFAT, FAT12/16, GPT-only cards and other sector
sizes, saying why. It reads 8.3 names only, so boot paths are short:

```
/BOOT/SURFOS.BIN     the kernel package SurfBoot loads
/BOOT/BACKUP.BIN     optional last known-good package (tried when SURFOS.BIN fails a check,
                     or chosen by holding BTN0 at power-on)
/BOOT/CMDLINE.TXT    optional kernel command line
/SYSTEM/             configuration and supporting data
/PROGRAMS/           later RISC-V programs
/USERS/DOCUMENTS/    the owner's files
```

SurfBoot only reads the card. After the handoff the kernel owns it, mounting it read-write
once its SD write path is validated. Updates from the Mac (`make deploy-sd`) replace files in
`/BOOT/` and `/SYSTEM/` only, keep the user directories, never repartition or format, and verify
every copied byte. FAT32 is not journaled: an interrupted update can damage the file system,
and a backup file does not make an update atomic.
