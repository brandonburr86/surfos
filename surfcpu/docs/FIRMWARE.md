# SurfCPU firmware: SurfBoot, the diagnostics and the card tools

SurfBoot is the boot ROM: it starts at the reset vector, waits for DDR, reads the microSD
card over SPI, checks and loads `/BOOT/SURFOS.BIN` into DDR and hands over to the kernel as
[BOOT-ABI.md](BOOT-ABI.md) says. Two diagnostic programs and five tools go with it. Owner's
card guide: [CARD.md](CARD.md).

**Status (2026-10-15).** Everything here is built and tested on the host: the portable
SD/FAT/image code natively against card images, the RV32 ROM images in a firmware emulator,
and the SD code against the io track's pin-level card model. Nothing has run on the RTL SoC
(that comes at integration, in `surfsim`) or on the board. Physical results are **pending**.

**SurfBoot 1.1.0 (2026-10-16, round ddr-3).** The open flow's DDR designs build LiteDRAM
without its own calibration CPU (DDR.md): their `sysinfo.DDR_STATUS.SWINIT` is 1, and SurfBoot
and the BRAM diagnostics initialize the DDR3L and level its read path themselves, through the
DDR controller's registers (the `ddrc` region), with `firmware/common/ddrinit.c`, LiteX's
`sdram_init` and read leveling ported (section 5, step 3). Tested in the host model, in the
emulator and in the RTL simulation of the DDR top against LiteDRAM's own controller; on the
board, **pending**. With SWINIT 0 (the MIG, the simulation models) nothing changed.

## 1. Layout

```
firmware/common/       start-up, traps and the fault reporter, UART, VGA text console, timer,
                       LEDs, printf (fmt.c), CRC-32, string functions, rom.ld, ram.ld;
                       ddrinit.c, ddrinit.h, ddrinit_phy.h (the DDR initialization)
firmware/surfboot/     SurfBoot: main.c, boot.c, sd.c, fat32.c, image.c, monitor.c, sberr.c,
                       port.h (the platform interface), port_surf.c (the SoC side), handoff.S
firmware/diagnostics/  bramdiag.c (milestone 3, a ROM image), ddrdiag.c (a SURFOS.BIN),
                       cputest.c, ddrtest.c, probes.S, report.c
firmware/tests/        host tests: fwtest.c, port_host.c, sdcard_sim.c, surfemu.c (emulator),
                       ddrc_model.c (LiteDRAM behind the ddrc registers), hello/ (handoff test
                       kernel), iomodel_glue.cpp, run_tests.py, run_emu.py, test_tools.py,
                       deploy/ (fake diskutil and mount)
tools/                 surfimg.py, bin2mem.py, mkcard.py, isa-audit.py, deploy-sd.py
```

## 2. Building

`make build-rom` (in the container; `make -C surfcpu build-rom` from the Mac) makes:

| File | What | Size |
|---|---|---|
| `build/fw/surfboot.{elf,bin,mem}` | SurfBoot; `.mem` is the boot ROM image | 43,680 bytes, **66.7 %** of the 64 KiB ROM (1.1.0, a clean build; 1.0.0 was 40,084: the DDR initialization takes about 3.6 KB) |
| `build/fw/bramdiag.{elf,bin,mem}` | the BRAM diagnostics, an alternative ROM image | 31,392 bytes, 47.9 % |
| `build/fw/ddrdiag.elf`, `DDRDIAG.BIN`, `ddrdiag.payload.bin` | the DDR diagnostic package | 15,012 bytes |
| `build/fw/hello.elf`, `HELLO.BIN` | the handoff test kernel (tests only) | 4,340 bytes |

The sizes are for a build from a clean tree; with uncommitted changes the banner's
"dirty tree" text adds about 90 bytes.

Flags: `-march=rv32im_zicsr_zifencei -mabi=ilp32` (`RV_CFLAGS`), `-Os -msave-restore`
(shared prologues: 2.9 KB less ROM; the hot loops are leaf functions and do not use them),
`-mstrict-align` (SurfCPU traps on misaligned accesses; on-disk fields are read a byte at a
time), `-fno-tree-loop-distribute-patterns` (no memcpy calls inside memcpy),
`-fno-delete-null-pointer-checks --param=min-pagesize=0` (the ROM is at address 0, so address
0 is valid memory), no C library (`firmware/common/libc/string.h` declares what `string.c`
provides), libgcc for the rv32im/ilp32 multilib. Every ELF is put through
`tools/isa-audit.py` before it is installed; the build stops if one instruction or CSR is
outside the ISA of [ISA.md](ISA.md). Words written as data in code (`.word`) are decoded like
any other: only the output section `.text.probes` (input section `.probes`, placed by
`rom.ld` and `ram.ld`) may hold data words, which is where the diagnostics' probes and the
test kernel keep their deliberately illegal instructions. The build identity (first 8 hex digits of the commit,
dirty flag) is generated into `build/fw/gen/fw_build.h` only when it changes.

The `.mem` files follow [MODULES.md](MODULES.md) section 5: exactly 16,384 lines of 8
lowercase hex digits, line *i* = bytes 4*i*..4*i*+3 little-endian, zero padded
(`tools/bin2mem.py --check` verifies a pair).

## 3. Memory

| Range | SurfBoot | BRAM diagnostics | SURFOS.BIN programs (`ram.ld`) |
|---|---|---|---|
| ROM `0x0000_0000` | code, constants (`.data` image if any) | code, constants | (SurfBoot) |
| scratch `0x0001_0000`... | `.data` + `.bss` (7,600 bytes incl. the 1.5 KiB trap stack, the CRC table, sector buffers) | 56 KiB tested destructively | `.fast` code at `0x0001_0000` (copied by start.S) |
| ... `0x0001_EFFF` | stack, growing down from `0x0001_F000` (53 KiB free) | `.data`, `.bss`, stack in the top 8 KiB | |
| `0x0001_F000` | `struct surf_boot_info`, written just before the handoff | | read (copied) first thing |
| DDR `0x8000_0000` | the image, wherever its header says | DDR tests when calibrated | the program, then `.bss` and a 16 KiB stack |

SurfBoot's data, buffers and stack stay inside the `surfboot_data` reservation
(`0x0001_0000`-`0x0001_EFFF`); the link scripts assert that `.bss` follows `.data`, that the
stack keeps at least 8 KiB and that everything fits the ROM.

## 4. The runtime (`firmware/common`)

**Start-up** (`start.S`): `mie` = 0, `MIE` = 0, `gp`, `sp`, `mtvec` = `trap_entry`,
`mscratch` = 0x5A1 (firmware context), the integer registers zeroed (their reset value is
undefined), `.data` copied from ROM, `.fast` copied to scratch RAM with FENCE.I (RAM images),
`.bss` cleared, then `main()`. A RAM image (`-DFW_RAM_IMAGE`) keeps SurfBoot's a0..a2 and
passes them to `main(hart, bootinfo, magic)`.

**Traps** (`trap.S`, `trap.c`): with `mscratch` = 0x5A1, the entry switches to the trap
stack, saves all 31 registers and `mepc`/`mcause`/`mtval`/`mstatus`, sets `mscratch` = 0 (so
a trap in the handler cannot loop) and calls `fw_trap_dispatch()`: a program's hook may
handle the trap (the diagnostics step over their deliberate ones), otherwise the full report
is printed and the program resumes at its recovery point (SurfBoot's monitor) or stops,
blinking LED code 8. With any other `mscratch` (after SurfBoot's handoff, before the kernel
installs its own `mtvec`, or inside the trap handler) the **stackless reporter** runs: it
uses no RAM and no stack, only t0..t6, prints `mcause`, `mepc`, `mtval`, the trapped `sp`
and the other 23 registers on the UART, then blinks LED code 8 forever. That is the
"fault reporter in ROM" of the handoff.

**Console**: output goes to the UART (115200 8N1, polled; a byte that cannot be sent within
about 40 ms is dropped rather than hanging) and to the VGA text screen (80x30, hardware
scrolling with `SCROLL`, the cursor kept in `CURSOR`); input comes from the UART.
`video_init()` clears the screen and selects text mode (SurfBoot, bramdiag); `video_attach()`
continues below what SurfBoot left (the DDR diagnostic).

**LED codes**: a failure shows its code in binary on the green LEDs LD4..LD7 with LD0 red.

| Code | Meaning | Code | Meaning |
|---|---|---|---|
| 1 | DDR missing, calibration failed or timed out, DDR test failed | 6 | file or directory not found, not an 8.3 path |
| 2 | no card | 7 | the package failed a check |
| 3 | the card does not answer, initialize or read | 8 | a trap (fault reporter), CRC self-test failed |
| 4 | partition table (none, GPT, no FAT32 partition, outside the card) | 9 | stopped in the monitor on request (BTN1) |
| 5 | file system (exFAT, NTFS, FAT12/16, bad boot sector, bad cluster chain, bad directory) | | |

While SurfBoot runs LD0 is blue; at the handoff it turns green. The diagnostics end with all
green LEDs on and LD0 green (pass) or red (fail).

## 5. SurfBoot

### Boot sequence

```
SurfBoot 1.1.0 (build 1cd47417), the SurfCPU boot ROM
soc: Arty A7-100T (0x100), SoC 0.2.0, memory map 2, 25000000 Hz; bitstream 0123abcd
ddr: initialized by SurfBoot, read leveling lane 0: bitslip 3, delay 16 (window +-10); lane 1: bitslip 5, delay 15 (window +-5)
ddr: calibrated after 240 ms, 256 MiB, quick test passed (data, address, byte lanes)
sd: SDHC/SDXC, 29.7 GiB; ready after 230 ms; SPI 6250 kHz, data CRC checked
fat32: partition 1 (type 0x0c) at sector 8192, 32768-byte clusters, 973808 clusters, label "SURFOS"
boot: /BOOT/SURFOS.BIN
boot: loading 1234 KiB ...................
boot: SurfOS 0.1.0 build 1a2b3c4d: 1263683 bytes in 3 fragments, read in 2210 ms
boot: load 0x80000000, entry 0x80000000, mem_size 2000000, ISA IM_Zicsr_Zifencei; payload CRC 0x12345678 matches (card and DDR)
boot: command line "console=uart"
boot: starting the kernel at 0x80000000 (a0 = 0, a1 = 0x0001f000, a2 = 0x49465253)
```

(The sizes, times and leveling above are illustrative: the leveling line is the host model's
windows, and there is no such line where SWINIT is 0; the emulator's real transcripts are in
`docs/evidence/fw/`.)

1. **Banner and self-tests.** The identity registers (a wrong `sysinfo.MAGIC` or memory map
   version is reported), the CRC-32 test vectors of BOOT-ABI.md section 3 (a failure means a
   faulty CPU or ROM: SurfBoot stops in the monitor), and the timer: if `mtime` does not move,
   SurfBoot says so and counts time in software (about 4 us per read) so that every wait
   below stays bounded, if approximate.
2. **Buttons**, read 20 ms after reset (after the debouncers settle): **BTN1** held = stay in
   the monitor; **BTN0** held = boot `/BOOT/BACKUP.BIN` (only that one).
3. **DDR**: `DDR_STATUS.PRESENT` must be set. With `DDR_STATUS.SWINIT` set (LiteDRAM
   without its CPU) SurfBoot first initializes the controller through the `ddrc` registers
   (`firmware/common/ddrinit.c`: LiteX 2026.08's `sdram_init`, its generated
   `init_sequence()` and its exhaustive read leveling, ported; DDR.md section 10): the PHY
   reset, the DDR3L's reset, CKE, mode registers MR2, MR3, MR1, MR0 and ZQ calibration, then
   for each byte lane every bitslip (8) and read delay (32 taps) scanned with three
   pseudo-random bursts, the bitslip with the best score chosen and its delay set to the
   middle of the widest working window and checked, and the controller given the DRAM back
   with `init_done` (and `init_error` when a lane has no working delay). This takes about
   0.2 s at 25 MHz and logs one line, `ddr: initialized by SurfBoot, read leveling lane 0:
   bitslip B, delay D (window +-W); lane 1: ...`; a lane without a window stops with
   DDR_CALIB, the lanes named; a `CALIB_FAIL` already set (the bridge's 1.5 s from reset
   passed) stops at once. A PHY whose phase registers do not read back (LiteDRAM's DRAM
   model in simulation) is initialized without leveling, as LiteX does for a PHY that is
   not read-leveling capable, and the line says so. Then, and without SWINIT straight away,
   `CALIB_DONE` is awaited for at most **2 s**
   (`CALIB_FAIL` stops at once); `DDR_SIZE` must be a whole number of MiB up to 256 MiB; then
   a quick destructive test (walking one and zero on the data lines, a distinct word at every
   power-of-two offset for the address lines, byte and halfword lanes). `ram_size` in the
   boot information is the size that passed this test. Images may use
   `[0x8000_0000, 0x8000_0000 + size - 16 MiB)`: `ddr_top`, the top 16 MiB of the DDR that
   calibrated (from `0x8F00_0000` with 256 MiB, from `0x8300_0000` with 64 MiB), stays free;
   a `DDR_SIZE` of 16 MiB or less leaves nothing below it and stops (DDR_ABSENT).
4. **The card** (`sd.c`), always initialized afresh (it may have been swapped):

   | Step | Limit |
   |---|---|
   | card-detect switch (`spi.STATUS.CARD`) | absent: NO_CARD |
   | SCLK 390.625 kHz (DIV 31), 1 ms, 80 clocks with CS high | |
   | wait while MISO reads 0x00 (a card busy from before a reset) | 500 ms |
   | CMD0 until R1 = 0x01 | 10 tries |
   | CMD8 0x1AA (no: SD 1.x; echo checked) | |
   | CMD59 1 (data CRC-16 checked from then on; refused: not checked) | |
   | CMD58 (3.2-3.4 V must be in the OCR) | |
   | CMD55 + ACMD41 (HCS for SD 2.0) until R1 = 0 | 2 s (the spec says 1 s) |
   | CMD58: CCS = block addressing (SDHC/SDXC), else byte addressing (SDSC) | |
   | CMD16 512 for SDSC | |
   | SCLK 6.25 MHz (DIV 1), CMD9 (CSD: capacity), CMD10 (CID) | |
   | before every command: wait for MISO = 0xFF | 500 ms |
   | CMD17 per 512-byte sector; data token | 250 ms (the spec says 100 ms) |
   | per sector | 4 attempts; after a CRC error the clock is halved (down to 390 kHz) |

   SurfBoot never writes the card and never uses CMD12/CMD18. A sector beyond the capacity in
   the CSD is refused before it is asked for; the card's own answer to one (R1 0x40) is also
   handled. Every wait is bounded by time (`mtime`), so a dead, missing or unplugged card
   always ends in an error naming the step.
5. **MBR and FAT32** (`fat32.c`): sector 0 must carry 55 AA; a sector 0 that is a boot
   sector (no partition table) is SUPERFLOPPY, an exFAT one EXFAT; the first partition of type
   0x0B or 0x0C is used (a hybrid MBR with an 0xEE entry next to it is fine); without one,
   GPT (0xEE alone), EXFAT or NTFS (0x07, looking at its boot sector), FAT1216 (0x01, 0x04,
   0x06, 0x0E) or NO_FAT32_PART is reported. The partition must lie inside the card. The boot
   sector is checked strictly: 55 AA, a jump, 512-byte sectors, a power-of-two cluster,
   reserved sectors, 1 or 2 FATs, the FAT type from the **cluster count** as Microsoft's
   specification defines it (fewer than 65,525 clusters is FAT12/16 whatever the boot sector
   says), FAT16 fields zero, version 0.0, the volume inside the partition, the FAT large
   enough for every cluster, the root cluster in range, the active FAT (ExtFlags) present.
   These limits bound every later sector number.

   Names are 8.3, matched without regard to case; long-name entries, deleted entries and the
   volume label are skipped. Every FAT entry is checked (free, reserved, bad and
   out-of-range clusters are errors); a file's chain must have exactly the clusters its size
   needs (shorter or longer is an error) and is walked with Brent's cycle detection; a
   directory may not exceed FAT's 65,536 entries (2 MiB). No walk can run away.
6. **The package** (`image.c`), the checks of BOOT-ABI.md section 2 in its order: size,
   magic (an ELF or x86 boot code is named as such), version and header size, header CRC;
   ISA bits (`SURF_ISA_I` required, no bit SurfCPU lacks) and platform; file size = 64 +
   `payload_size` (truncated/extra bytes); load
   alignment, `mem_size` >= `payload_size`, the range inside usable DDR without overflow;
   entry aligned inside the payload. Then the payload is copied sector by sector through a
   scratch buffer into DDR with its CRC-32 updated as it goes, compared with the header, and
   read back from DDR and checked again. Nonzero header `flags` are reported and ignored.
7. **BACKUP.BIN**: when `/BOOT/SURFOS.BIN` fails anything (missing, damaged chain, any
   package check), `/BOOT/BACKUP.BIN` is tried; the first failure is the one reported if
   both fail. With BTN0, BACKUP.BIN alone is tried.
8. **CMDLINE.TXT**: the first line of `/BOOT/CMDLINE.TXT` (up to CR, LF or NUL; a UTF-8 byte
   order mark is skipped; control characters become spaces), at most 127 characters (longer:
   cut, and `boot_flags` bit 1 set). Missing or unreadable: an empty command line.
9. **Boot information** at `0x0001_F000`, every field of BOOT-ABI.md section 4, with its CRC.
10. **Handoff** (`main.c`, `handoff.S`): `intc.ENABLE` = 0, `mtimecmp` = all ones (lo, hi,
    lo), SPI chip select released, UART/GPIO/video interrupt enables cleared, UART drained
    (100 ms at most), `mie` = 0, `MIE` = 0, `mtvec` = the fault reporter in ROM with
    `mscratch` = 0, FENCE, FENCE.I, then `jr entry` with a0 = 0, a1 = `0x0001F000`,
    a2 = `0x49465253`. The video stays in text mode showing the log.

### Error names

Every failure is one of these names (`sberr.h`) with a sentence giving the numbers; the
monitor's `info` repeats the last boot failure.

| Name | LED | Typical cause, and what to do |
|---|---|---|
| DDR_ABSENT, DDR_CALIB, DDR_TIMEOUT, DDR_TEST | 1 | bitstream without DDR, DDR calibration or memory fault: check the build; run the BRAM diagnostics |
| NO_CARD | 2 | no card in the socket (or it was pulled out): insert it (SurfBoot then retries by itself) |
| SD_NORESP, SD_INIT, SD_UNSUPPORTED, SD_READ | 3 | wiring, an unpowered or unsupported card (MMC, no 3.3 V), a failing card: reseat, try another card |
| MBR, GPT, SUPERFLOPPY, NO_FAT32_PART, PART_RANGE | 4 | the card's layout: format it as CARD.md says (MBR, one FAT32 partition) |
| EXFAT, NTFS, FAT1216, BPB, FAT_CHAIN, FAT_CYCLE, DIR | 5 | wrong or damaged file system: reformat, or repair it on a computer |
| NOT_FOUND, NOT_FILE, NOT_DIR, BAD_PATH | 6 | `/BOOT/SURFOS.BIN` missing: `make deploy-sd` |
| IMG_SHORT ... IMG_VERIFY | 7 | not a SurfCPU package, damaged, or built for other memory: rebuild and deploy; IMG_VERIFY points at DDR |
| TRAP, CRC_SELFTEST | 8 | a fault in SurfBoot or the CPU: the report shows where |
| STOPPED | 9 | BTN1 was held |

### The monitor

When booting fails, or with BTN1, SurfBoot prints the reason and waits for commands on the
UART (the screen shows the same). A trap during a command is reported and the monitor
carries on; `md`/`mw` turn an access fault into a message. Inserting a card (the
card-detect switch closing, stable for 250 ms) retries the boot by itself.

| Command | What it does |
|---|---|
| `boot [PATH]` | boot `/BOOT/SURFOS.BIN` (BACKUP.BIN if it fails), or the 8.3 path given (for example `boot /BOOT/DDRDIAG.BIN`) |
| `backup` | boot `/BOOT/BACKUP.BIN` |
| `check PATH` | every package check, loading it into DDR, without starting it |
| `info` | board, SoC, clocks, DDR (with `software init` when SWINIT is set) and card state, the last boot failure |
| `ddr` | initialize the controller again when SWINIT is set (and print the leveling), wait for calibration again and rerun the quick test |
| `sd` | initialize the card again: type, addressing, OCR, CSD capacity, CID |
| `part` / `fs` | the MBR partition table / the FAT32 volume's geometry |
| `ls [PATH]`, `cat PATH [N]`, `chain PATH` | list a directory; print a file; show a file's clusters by fragment |
| `md ADDR [N]`, `mw ADDR VALUE` | read / write memory words |
| `spi [DIV\|auto]` | show or set the data clock used from the next card initialization: DIV 0-255 (DIV 0 = 12.5 MHz, to try a faster card link), `auto` = back to the default (DIV 1, 6.25 MHz) |
| `reset` | restart SurfBoot |

## 6. The diagnostics

**BRAM diagnostics** (`build/fw/bramdiag.mem`, put in the boot ROM instead of
`surfboot.mem`: milestone 3). Needs only the core, block RAM, UART and timer. Sections:
identity; ALU and immediates (33 checks); M extension (23); branches and jumps (9); loads and
stores (8); CSRs and counters (30); scratch RAM (data and address lines, March C- over
56 KiB, address-in-address) and the spare text RAM; exceptions (34 checks: illegal 0 and
0xFFFFFFFF, a write to `mhartid`, `satp`, SLLI with imm[5], SRET, ECALL, EBREAK, misaligned
LW/LH/SW/SH, loads from an unmapped address, an MMIO hole, a byte load of an MMIO register,
the reserved eth window, stores to the ROM and halfword to MMIO, fetches from unmapped,
MMIO and text RAM, a JALR and a branch to 2-aligned targets; each with its exact `mcause`,
`mtval` and `mepc`, and no register or memory changed); interrupts (software, timer ticking
10 x 1 ms, external from the UART, priority MEI > MSI > MTI, MIE gating, WFI, the timer rate
against `mcycle`); FENCE.I on code written to scratch RAM; with SWINIT, the DDR
initialization (as SurfBoot's, printing every lane's scan: one line per bitslip with a 1 for
each of the 32 delays that read the patterns back, then each lane's choice and the time;
checks: each lane has a window, the leveling succeeded); DDR when calibrated (milestone 4:
all the DDR tests below, from the ROM, including code copied from the ROM into DDR and run
there). It ends `BRAMDIAG: PASS (572 checks)` (in the emulator, with SWINIT) or
`BRAMDIAG: FAIL (...)`, then
echoes UART input (Ctrl-R runs it all again). A stray trap fails its section and the rest
still runs.

**DDR diagnostic** (`build/fw/DDRDIAG.BIN`, a SURFOS.BIN: milestone 5's "real card boots a
DDR diagnostic image"). Install it as `/BOOT/SURFOS.BIN`, or copy it to `/BOOT/DDRDIAG.BIN`
and type `boot /BOOT/DDRDIAG.BIN` in the monitor. It checks its boot information, then tests
DDR from the first MiB above itself to the top (including `ddr_top`): data lines, byte
lanes, address lines and aliases, boundaries (the area's ends, a 1 MiB and the 128 MiB line,
every width at the last word of DDR), patterns (4 MiB in full and two words per 64 KiB
everywhere), sustained copies checked by CRC-32 (4 x 1 MiB, with the rate) and a routine
copied to three places in DDR and run, then patched in place and run again after FENCE.I.
Its hot loops run from scratch RAM (`.fast`). The command line picks the depth: `full`
(every word: minutes on the board), `size=16M` (a smaller area), `loops=N`. It ends
`DDRDIAG: PASS (n checks)` or `FAIL`; `q` and `f` rerun the quick or full tests.

## 7. The tools

| Tool | Use |
|---|---|
| `tools/surfimg.py` | `pack --elf surfos.elf -o SURFOS.BIN [--payload-out surfos.payload.bin] --name SurfOS --version X.Y.Z [--build-id HEX]`; `pack --bin` with `--load-addr`, `--entry`, `--mem-size`; `info`, `verify`, `crc`, `selftest`. The payload is the PT_LOAD segments by load address; a segment the program copies elsewhere itself (`.fast`) is allowed and noted; the entry must run where it loads. `pack` audits the ELF and refuses to write a package SurfBoot would refuse; `verify` applies SurfBoot's checks in its order with its error names (a test keeps the two in agreement). Constants come from `spec/soc.toml`. |
| `tools/bin2mem.py` | `bin2mem.py IN.bin OUT.mem [--words 16384]`, `--check` |
| `tools/mkcard.py` | `mkcard.py OUT.img --standard-dirs --file /BOOT/SURFOS.BIN=PATH [--size 64M] [--cluster 4K] [--fragment interleave|reverse|random] [--lfn] [--fill]`; `--inspect IMG`. Pure Python; image files only. Long names get short aliases (`/USERS/DOCUMENTS` is not an 8.3 name). Imported by the tests to damage images precisely. |
| `tools/isa-audit.py` | `isa-audit.py FILE.elf...`: decodes every word of the executable sections, `.word`/`.4byte`/`.half` data (`$d`) included, except `$d` words in the output section `.text.probes`; checks RV32I/M/Zicsr/Zifencei + ECALL/EBREAK/MRET/WFI, the CSR table and read-only CSR writes, the ELF flags and arch attribute, and cross-checks with objdump. **For the kernel build**: run it on `surfos.elf` (`surfimg.py pack --elf` does by default); deliberately illegal words belong in an input section the link script places in `.text.probes`, and constants in non-executable sections (`.rodata`), as GCC and these link scripts place them. |
| `tools/deploy-sd.py` | the Mac installer ([CARD.md](CARD.md)): refuses anything but a FAT32 volume on removable, physical media whose whole disk has an MBR, binds the mount point to diskutil's device node (st_dev = st_rdev), refuses symbolic links, writes through directory descriptors with O_NOFOLLOW and O_EXCL temporary files, then unmounts and remounts the card and compares every installed byte. Exit 0 verified, 1 refused, 2 written but NOT verified, 3 different; `--verify-only`. Runs with Python 3.9 (the Mac's `/usr/bin/python3`) and later. |

## 8. Tests (`make test-fw`)

| Part | What | Result (2026-10-16) |
|---|---|---|
| `test-fw-unit` | CRC-32 vectors, CRC-7/CRC-16 of known frames, printf conversions, 8.3 names, CSD parsing, SPI dividers | pass |
| `test-fw-tools` | `test_tools.py`: 21 tests (section 7's tools, fsck.fat and mtools on 8 image layouts, isa-audit on 7 bad programs and on the same kind of encodings hidden as `.word` data, the `.text.probes` rule, deploy-sd against a stand-in with a model of the Mac's cache: symbolic links, device binding, every refusal, damage caught only by the remount, unmount and mount failures; Python and C package checks agreeing on 79 packages, 98 verdicts with 256 and 64 MiB of DDR; the Mac tools parsing as Python 3.9) | 21 pass |
| `test-fw-scenarios` | `run_tests.py`: the portable SurfBoot code under ASan/UBSan on the simulated board, 146 card scenarios (the brief's storage list and more; the DDR initialization against the LiteDRAM model, `firmware/tests/ddrc_model.c`, which fails a run on any protocol violation: leveled, a lane without a window, too late, no PHY registers, and the monitor), then 5 seeds x 300 runs of random metadata damage | 146 pass; fuzzing: every run ends with a result |
| `test-fw-emu` | `run_emu.py`: 37 runs of the real ROM images in `surfemu` (boots, handoff checked from the kernel's side, traps after the handoff, the monitor, buttons, card insertion, DDR failures and injected faults, a dead timer, the diagnostics; SurfBoot and the BRAM diagnostics initializing the LiteDRAM model, with DDR data that reads back wrong on a lane leveled outside its window) | 37 pass |
| `test-fw-iomodel` | the scenarios against the io track's pin-level SD model (`sim/models/sdcard_model.cpp`), failing on any protocol violation | 129 of 129 pass (the 17 scenarios that need `sdcard_sim`'s own fault options are not run) |

The outputs are in `docs/evidence/fw/` (`test-fw.txt`, `build-rom.txt`, `emulator.txt`,
`iomodel.txt`); `review-fixes.txt` shows the review's repro scripts failing safe after the
fix round (symbolic links on the card, the unchecked partition table, `spi 0`, instructions
hidden as `.word`).

`surfemu` is an instruction-level model of the SoC written for these tests (the core as
ISA.md describes it, the memory map with its bus errors, every peripheral SurfBoot uses, the
SD card model on the SPI port); it is a test aid, not a reference model and not the RTL.
It found a real bug before any RTL existed: `.bss` linked at the start of scratch RAM
instead of after `.data` (fatal for the BRAM diagnostics, harmless for SurfBoot only by
coincidence). The ISA audit caught another: a CRC table placed among the ROM's code.

## 9. Pending and known limits

* **Pending (needs the integration or the board):** SurfBoot on the RTL SoC in `surfsim`
  with the SD, UART and DDR models; every physical result (milestones 3-5: the BRAM
  diagnostics' transcript, DDR tests, booting from a real card, timing of a real card at
  6.25 and 12.5 MHz through the Pmod).
* Reads only, one sector per command (CMD17): about 0.25-0.6 MB/s at 6.25 MHz in the
  emulator (fragmentation costs FAT lookups). A 12.5 MHz clock (`spi 0`) or CMD18 would be
  faster; the round-trip margin at 12.5 MHz is a board question (docs/IO.md).
* SDSC cards above 4 GiB, SDUC and MMC cards are not supported (MMC is refused by name).
* The monitor takes input from the UART only (not the PS/2 keyboard).
