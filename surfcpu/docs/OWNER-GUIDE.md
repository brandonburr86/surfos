# SurfCPU on the Arty A7: the owner's guide

This guide takes you from the MacBook to SurfOS running on the Arty, one step at a time, with the
output to expect at each step. It ties together the detailed documents:
[WIRING.md](WIRING.md) (every pin), [BOARD.md](BOARD.md) (designs, programming),
[CARD.md](CARD.md) (the microSD card), [FIRMWARE.md](FIRMWARE.md) (SurfBoot),
[OPEN-FLOW.md](OPEN-FLOW.md) (building bitstreams on the Mac), [COMMANDS.md](COMMANDS.md).
Commands are typed in Terminal in the repository folder
(`SurfOS Restoration Project/06-restoration-surfcpu`).

Every output below is labelled. **Seen on this Mac** means it was copied from a real run.
**Rehearsed** means the real script ran against a stand-in for the board or card. **Expected,
from simulation** means the same RTL or firmware produced it in simulation, not on the board.
Nothing in this guide has run on the Arty yet: those steps are marked **pending**.

## 0. Where things stand (2026-10-16)

| Step | State |
|---|---|
| CPU, SoC, SurfBoot and the SurfOS kernel | pass in simulation, including SurfBoot booting SurfOS from a card image on the RTL with the real CPU |
| Files written by SurfOS | pass in simulation: written on the RTL system, ejected, read back after a second boot and by the Mac's tools; write errors, a card taken out and a full card tested on the RTL system too |
| Bitstreams | built on this Mac with the open-source flow, for both boards: the milestone 1 design, and the full SoC with the CPU diagnostics or SurfBoot in its boot ROM (no DDR controller yet). Every flow check passes, and their synthesized netlists run the same programs in simulation |
| DDR memory on the board | the memory controller works in simulation, and SurfBoot boots SurfOS from it there; the open-source flow does not yet meet its 100 MHz timing, so there is no DDR bitstream yet, and booting SurfOS on the board waits for one |
| The board tests | one command per board milestone (`make -C surfcpu board-test M=m1`, [PHYSICAL-TESTS.md](PHYSICAL-TESTS.md)), rehearsed against the RTL system and fake boards; it reads the board's serial port itself and writes the evidence file |
| The release (milestone 9) | `make -C surfcpu release` builds every image from a clean commit with fixed time stamps, and `make -C surfcpu release-verify` rebuilds it from a fresh clone and compares. A pre-release made from a fresh checkout (1.0.0-pre-bb066932) rebuilt byte for byte, 18 files of 18. The DDR designs are missing until LiteDRAM's generated code is made reproducible (in progress). Nothing has been written to the board's flash ([RELEASE.md](RELEASE.md)) |
| The board itself | an A7-100T (IDCODE 0x03631093), with JP2 taken off. Loaded over USB from this Mac (configuration RAM only; the flash is untouched): the baseline passes m1, and the CPU passes m3 with all 203 BRAM diagnostics ([evidence](evidence/physical/)) |

## 1. Set up the Mac (once)

Docker Desktop must be running (whale icon in the menu bar).

1. Build the toolchain image (a few minutes the first time; seconds afterwards):

   ```
   make -C surfcpu image
   ```

   Seen on this Mac (the last lines, a version list):

   ```
   python3                      Python 3.13.5
   mtools                       mtools (GNU mtools) 4.0.48
   z3                           Z3 version 4.13.3 - 64 bit
   boolector                    1.5.118
   make                         GNU Make 4.4.1
   ```

2. Build the open-source FPGA flow's image (about 6 minutes the first time, 661 MB):

   ```
   make -C surfcpu openxc7-image
   ```

   Seen on this Mac (last lines):

   ```
   source-yosys           yosys 0.69 sha256:6dad6412...
   chipdb-xc7a100t.bin    sha256:b4098ce6... 39198709 bytes
   chipdb-xc7a50t.bin     sha256:ec876fe0... 29585641 bytes
   ```

3. Install the programmer (you run this one; it is the only thing installed on the Mac itself):

   ```
   brew install openfpgaloader
   ```

4. Check everything:

   ```
   make -C surfcpu doctor
   ```

   Seen on this Mac, before the board was connected:

   ```
   SurfCPU doctor
     Mac          MacBook Pro (Mac16,5), Apple M4 Max, 36 GB, macOS 15.1.1 arm64, 89.2 GiB free
     Toolchain    surfcpu-toolchain:2026-10-15 (aarch64): Verilator 5.032 ...; Icarus Verilog version 12.0 ...; Yosys 0.52 ...
                  riscv64-unknown-elf-gcc (14.2.0+19) 14.2.0; rv32im libgcc rv32im/ilp32; spike 5a760667bf0d
     Board USB    pending: no Arty (FTDI 0403:6010) connected
     Serial       pending: no /dev/cu.usbserial-* port
     openFPGALoader pending: not installed (brew install openfpgaloader)
     Card         pending: no FAT volume mounted
     Open flow    surfcpu-openxc7:2026-10-15 present: make build-fpga FLOW=openxc7 builds bitstreams on this Mac (docs/OPEN-FLOW.md)
     Vivado host  not configured (optional, for Vivado timing sign-off and the MIG): ...
   OK: build and simulation can run
   ```

   With the board plugged in and openFPGALoader installed, the Board USB, Serial and
   openFPGALoader lines change from `pending` to the board's cable, its two serial ports and the
   FPGA's IDCODE: `0x0362D093` is an A7-35T, `0x03631093` an A7-100T. **Tell me which one you
   have**; every command below takes `BOARD=arty_a7_35t` or `BOARD=arty_a7_100t` (the default).
5. Optional, before the board: `make -C surfcpu board-test-selftest` (15 seconds) runs the board
   test kit against fake boards on the Mac's own pseudo-terminals. It checks that the Mac's own
   `python3` runs the kit; nothing needs installing.

## 2. Check and build

1. Run every automated check, about 20 minutes:

   ```
   make -C surfcpu check
   ```

   Seen on this Mac, last line: `check: all automated SurfCPU checks passed`. Any failure stops it
   with the failing test's output.

2. The boot ROM and the diagnostics:

   ```
   make -C surfcpu build-rom
   ```

   Seen on this Mac (last lines):

   ```
   build-rom: surfboot.elf text 40084 data 0 bss 7600: ROM 40084 of 65536 bytes (61.2%)
   build-rom: bramdiag.elf ROM 27936 of 65536 bytes (42.6%)
   ```

3. The SurfOS kernel package, `build/riscv32/arty_a7/SURFOS.BIN`:

   ```
   make -C surfcpu build-kernel
   ```

   Seen on this Mac (one of its lines):
   `surfimg: build/riscv32/arty_a7/SURFOS.BIN: SurfOS 1.0.0 build ..., load 0x80000000, entry 0x80000000, ...`

4. A bitstream, here the milestone 1 design (LEDs, switches, buttons, a UART line a second):

   ```
   make -C surfcpu build-fpga FLOW=openxc7 DESIGN=arty_a7_baseline BOARD=arty_a7_100t
   ```

   Seen on this Mac (about 22 seconds; selected lines):

   ```
   openxc7-build.sh: clock pix_clk: constraint 25.17 MHz, achieved 187.27 MHz, slack +34.384 ns  PASS
   openxc7-build.sh: clock sys_clk: constraint 25.00 MHz, achieved 126.09 MHz, slack +32.069 ns  PASS
   openxc7-build.sh: checks: synthesis PASS, bitstream PASS, round trip PASS, block RAM none in the design, LVS PASS, clocks PASS
   fpga-build: PASS: build/fpga/arty_a7_100t/arty_a7_baseline/arty_a7_baseline.bit (open flow: nextpnr's timing is an estimate; the 11 constraints above are not checked)
   fpga-build:   idcode = "0x03631093"
   ```

   It also lists the 11 constraints only Vivado can check (clock-crossing bounds, I/O false
   paths, configuration options); that is expected with the open flow (OPEN-FLOW.md).

## 3. Wire the board (power off, USB unplugged)

[WIRING.md](WIRING.md) has every pin; these are the rules that protect the hardware.

| Adapter | Where | How |
|---|---|---|
| Pmod MicroSD | JA (a 12-pin adapter: both rows) | component side up, pin 1 at JA pin 1; no card in it until SurfBoot runs (WIRING.md U8) |
| Pmod VGA | JB + JC together | **component side up, the VGA socket pointing away from the board, the adapter's J1 in JB.** Upside down it shorts FPGA outputs together and to ground through the 0 Ω JB/JC ports, which can damage the FPGA (WIRING.md U2) |
| Pmod PS2 | JD, **top row** | component side up, its pin 1 at JD pin 1; **its jumper JP1 on VB** (the Arty's 3.3 V). Never 5 V from outside: that would pull the lines above the FPGA's limits (WIRING.md section 4) |

Two different jumpers are both called JP1:

* **The Arty's JP1 (MODE)**: take it **off** for bring-up, so the FPGA waits for the Mac
  instead of loading Digilent's demo from flash at power-on (that demo could drive the Pmod
  pins).
* **The Pmod PS2's JP1 (supply)**: **on VB**.
* **The Arty's JP2**: **off**. Your board came with it fitted, and then every program that opens
  the serial port (the test kit, the console, `screen`) resets the board: the first bring-up saw
  the count start again at 1 each time. You took it off on 2026-10-16; keep it off.

Also connect nothing to the Pmod PS2's JP2 header (its silkscreen has VE and GND swapped).
BOARD.md section 12 is the full checklist.

Bring the adapters up in order: power on with only the USB cable first and load the baseline
design (section 4), which leaves JB, JC and JD unassigned so the FPGA drives none of their
pins; only then load designs that drive them. The microSD card goes in only when SurfBoot is
running (section 6).

Also open: the PCB revision printed on the Arty, the adapters' revisions, and whether your
keyboard is a real PS/2 keyboard that works at 3.3 V (INVENTORY.md section 4, G4).

## 4. First power-up: the baseline design (milestone 1) **passed on 2026-10-16**

1. Plug the Arty's USB cable into the Mac (it powers the board). `make -C surfcpu doctor` should
   now show the cable, two `/dev/cu.usbserial-...` ports and the IDCODE.
2. Load the design into the FPGA's configuration RAM (nothing is written to the board's flash;
   power-cycling the board forgets it):

   ```
   make -C surfcpu program-volatile BOARD=arty_a7_100t DESIGN=arty_a7_baseline
   ```

   Rehearsed with a stand-in programmer:

   ```
   program: JTAG: IDCODE 0x13631093 = BOARD arty_a7_100t (xc7a100tcsg324-1)
   program: loading .../build/fpga/arty_a7_100t/arty_a7_baseline/arty_a7_baseline.bit into configuration RAM (lost at power-off)
   program: done; the FPGA runs the new design (press PROG or power-cycle to return to the flash)
   ```

   On the wrong board it refuses: `program: the FPGA's IDCODE is 0x0362D093, but
   BOARD=arty_a7_100t means 0x03631093 (bits 27:0); refusing`. It also refuses a bitstream
   built for another board, one whose build checks failed, and more than one Arty on the USB
   bus unless you name one (`CABLE_SERIAL=...`).
3. Watch the UART: `make -C surfcpu uart-console`. It finds the board's UART by itself (the
   second of its two ports) and needs nothing installed; Ctrl-] quits. `screen
   /dev/cu.usbserial-<serial>1 115200` works too; quit it with Ctrl-A, then K, then Y. Expected:
   one line a second, in this format (BOARD.md; simulation prints `sim` where the board prints
   `clean` or `dirty`):

   ```
   SurfCPU baseline arty_a7_100t 0x00000100 build 1a2b3c4d dirty 1760560000 | up 000012 s | pix 25173611 Hz | sw 0101 btn 0000 rx 41
   ```

   The board and its id, the build's commit, `clean` or `dirty`, the build time, seconds since
   reset, the pixel clock as measured (25,173,611 ± 1 Hz), the switches, the buttons and the
   last key you typed.
4. On the board: LD0 and LD1 blink green together once a second, switches SW0..SW3 light
   LD4..LD7, buttons light the RGB LEDs blue, RESET restarts the count.

That is milestone 1's physical test. Please send me the lines you see, or a photo of the LEDs.

Or run the whole test as one command: `make -C surfcpu board-test M=m1`. It asks for each step,
for example "load the design", "set the switches to 0101" or "do the LEDs blink?". It checks the
UART lines itself and writes `surfcpu/docs/evidence/physical/m1-<date>.txt`, transcript
included. [PHYSICAL-TESTS.md](PHYSICAL-TESTS.md) describes the six tests (m1, m3, m5, m7, m8,
m9).

## 5. The CPU on the board (milestone 3) **passed on 2026-10-16**

The full SoC with the CPU and memory diagnostics in its boot ROM; only the USB cable is
needed:

```
make -C surfcpu build-fpga FLOW=openxc7 BOARD=arty_a7_100t DESIGN=surfcpu_arty_a7_bramdiag   # about 14 minutes
make -C surfcpu program-volatile BOARD=arty_a7_100t DESIGN=surfcpu_arty_a7_bramdiag
```

Expected, from simulation (the RTL and the bitstream's own synthesized netlist), about 0.17 s
after the load:

```
SurfCPU BRAM diagnostics 1.0.0 (build <id>)
board Arty A7-100T (0x100), SoC 0.1.0, memory map 1, sys 25000000 Hz, timebase 1000000 Hz
identity                           ok (7 checks)
ALU, immediates, x0                ok (33 checks)
...
interrupts, timer and WFI          (10 timer ticks in 10186 us) ok (7 checks)
FENCE.I, code written to RAM       ok (2 checks)
DDR                                skipped: no DDR controller in this bitstream

BRAMDIAG: PASS (203 checks)
```

Then LD4-LD7 light and LD0 is green (red on a failure); from then on LD7 blinks about once a
second, the heartbeat of the loop that echoes typed characters. BOARD.md
section 12 has the whole transcript. This is milestone 3's physical test; as one command:
`make -C surfcpu board-test M=m3`.

## 6. SurfBoot and the card reader **pending**

The full SoC with SurfBoot in its boot ROM (`DESIGN=surfcpu_arty_a7_top`), with the adapters
plugged in (section 3) and then a FAT32 card (section 7) in the slot. Without DDR in this
bitstream SurfBoot stops in its monitor, which still exercises the CPU, the UART both ways, the
card over SPI and the VGA console (the same text appears on the monitor). The keyboard is not
tested yet: no ROM program reads it; SurfOS does. The monitor's commands (`help` lists them):
`boot`, `check`, `ls`, `cat`, `chain`, `part`, `fs`, `sd`, `ddr`, `md`, `mw`, `spi`, `info`,
`reset`. Expected, from simulation (with a simulated 64 MiB card):

```
SurfBoot 1.0.0 (build <id>), the SurfCPU boot ROM
soc: Arty A7-100T (0x100), SoC 0.1.0, memory map 1, 25000000 Hz; bitstream <id>
ddr: this bitstream has no DDR controller (DDR_STATUS 0x00000000): there is nowhere to load a kernel

SurfBoot stopped: no DDR controller (DDR_ABSENT)
...
surfboot> sd
sd: SDHC/SDXC, sector addressing, OCR c0ff8000, ready after 5 ms, data CRC on
sd: SPI 390 kHz for identification, 6250 kHz for data (DIV 1)
...
surfboot> ls /
...
```

Your card answers `sd` and `ls /` with its own details. Please send me these lines.

Milestone 5's whole test, `make -C surfcpu board-test M=m5`, needs the DDR design: it also runs
the DDR diagnostic `DDRDIAG.BIN` from the card, then takes the card out and puts it back.

## 7. Prepare the microSD card (once) and install SurfOS

[CARD.md](CARD.md) has the details. In short:

1. Insert the card in the Mac. If it is not already MBR + FAT32, erase it yourself with Disk
   Utility (View, Show All Devices; select the card's device; Erase; Name `SURFOS`, Format
   **MS-DOS (FAT)**, Scheme **Master Boot Record**). This destroys everything on the card. No
   SurfCPU tool ever formats or partitions a card.
2. Install SurfOS the first time (`--new-card` writes the card's marker file):

   ```
   make -C surfcpu deploy-sd MOUNT=/Volumes/SURFOS DEPLOY_ARGS=--new-card
   ```

   Without `MOUNT` it refuses (seen on this Mac): `deploy-sd: give the card's mount point
   explicitly: make deploy-sd MOUNT=/Volumes/<card> (the card's name is in Finder's sidebar)`.
   It checks that the mount point is really a removable FAT32 card on an MBR disk (never a disk
   image, the internal disk or a symbolic link), copies `/BOOT/SURFOS.BIN`, keeps the previous
   one as `/BOOT/BACKUP.BIN`, then unmounts and mounts the card again and compares every byte it
   wrote. Exit 0 means verified. Exit 2 means written but **not verified**: eject, reinsert,
   and run it again with `DEPLOY_ARGS=--verify-only`. Your own files are never touched. Add
   `DEPLOY_ARGS=--dry-run` to see the plan without writing.
3. Eject (`diskutil eject /Volumes/SURFOS` or Finder's eject button) before taking the card
   out. Never use the card in the Mac and the Arty at the same time.

Later updates are the same command without `--new-card`.

## 8. Booting SurfOS **pending: needs the DDR design**

SurfOS runs from the board's DDR memory, so this step waits for the bitstream with the memory
controller. Expected, from the RTL simulation of the whole system (with a 64 MiB card image):

```
SurfBoot 1.0.0 (build b4717a6a), the SurfCPU boot ROM
ddr: calibrated after 0 ms, 256 MiB, quick test passed (data, address, byte lanes)
sd: SDHC/SDXC, 64 MiB; ready after 5 ms; SPI 6250 kHz, data CRC checked
boot: SurfOS 1.0.0 build b4717a6a: 361800 bytes in 1 fragment, read in 1765 ms
boot: load 0x80000000, entry 0x80000000, mem_size 1141616, ISA IM_Zicsr_Zifencei; payload CRC 0x59afede3 matches (card and DDR)
boot: starting the kernel at 0x80000000 (a0 = 0, a1 = 0x0001f000, a2 = 0x49465253)
SurfOS: serial console on the arty_a7 UART (115200 8N1)
+VFS: sd0p1 mounted on /sd0p1 (fat, read-only)
Brandon Burr's SurfOS / Version 1.0 on a RISC-V (SurfCPU on the Arty A7), 2004-2026
SURF ~#>
```

The same console appears on the VGA monitor (640x480 at 60 Hz), and the PS/2 keyboard types
into it. The card stays read-only in SurfOS until the write path has been tried on your card
(`sd=rw` on the kernel command line, `/BOOT/CMDLINE.TXT`). Milestone 7's test as one command:
`make -C surfcpu board-test M=m7`.

### Writing files from SurfOS (milestone 8) **pending: needs the DDR design**

CARD.md section 8 has the details and what FAT32 cannot make safe. In short:

1. Allow writing (on the Mac, card mounted):
   `make -C surfcpu deploy-sd MOUNT=/Volumes/SURFOS DEPLOY_ARGS="--cmdline sd=rw"`.
2. Boot, then work in `/sd0p1/USERS/DOCUMENTS`. Every command that changes files puts it on
   the card before the prompt returns; a refused write makes the command fail with
   `... not all of it reached /sd0p1: I/O error`, and `sync` tries again.
   - SurfOS holds at most 128 KiB of changes for a card that keeps refusing; after that,
     writes fail at once.
   - `eject` then keeps the volume mounted:
     `eject: sd0 stays mounted: sync tries the writes again; umount gives them up`.
   - `umount` gives the changes up and says how many blocks were lost.
3. **Always `eject sd0` before taking the card out or switching off**, and wait for
   `sd0: safe to remove`.

Expected, from simulation (the RTL system, with a card image made on the Mac):

```
SURF ~#> cd /sd0p1/USERS/DOCUMENTS
sd0p1 > USERS > DOCUMENTS ~#> mkdir Saved-notes
sd0p1 > USERS > DOCUMENTS ~#> write Saved-notes/note.txt Written on the RTL by SurfOS.
sd0p1 > USERS > DOCUMENTS ~#> cp DATA.BIN Saved-notes/Big-copy.bin
    163840 bytes copied
sd0p1 > USERS > DOCUMENTS ~#> append LOG.TXT second line, appended by SurfOS
sd0p1 > USERS > DOCUMENTS ~#> cd /
SURF ~#> eject sd0
    /sd0p1 unmounted
    sd0: safe to remove
```

After a second boot, `crc32 Big-copy.bin` in that folder printed `081761ac   163840  Big-copy.bin`.

**The board test for milestone 8** (the brief's exit evidence), once SurfOS boots on the board.
The Mac's `crc32` command prints the same CRC-32 as SurfOS's `crc32`:

1. On the Mac, make a file on the card and note its CRC:
   `printf 'made on the Mac\n' > /Volumes/SURFOS/USERS/DOCUMENTS/MAC.TXT`, then
   `crc32 /Volumes/SURFOS/USERS/DOCUMENTS/MAC.TXT`. Eject: `diskutil eject /Volumes/SURFOS`.
2. On the board: `crc32 /sd0p1/USERS/DOCUMENTS/MAC.TXT` gives the same CRC (a file made on
   the Mac, read on the board).
3. Make and change files (`write`, `append`, `cp`) and note their `crc32`. Then `eject sd0`,
   switch the board off and on, and boot again: the same CRCs (persistence through a power
   cycle after a clean eject).
4. `eject sd0` again and take the card to the Mac: `crc32` of each file there gives the same
   CRCs (the Mac reads matching bytes).
5. Install a new kernel with `deploy-sd` (section 7): the files in `/USERS/DOCUMENTS` keep
   their CRCs (a kernel update leaves your files alone).

Send me the CRCs from each step, or the transcripts. Or let the kit lead you through these
steps and compare the CRCs itself: `make -C surfcpu board-test M=m8 ARGS="--mount
/Volumes/SURFOS"`. It reads the card's files on the Mac, read only.

Milestone 9's ten cold boots and one-hour soak are `make -C surfcpu board-test M=m9`. They
need the configuration flash programmed first, which waits for your go-ahead. With a release,
the test takes the release's manifest and kernel (RELEASE.md section 10):
`make -C surfcpu board-test M=m9 ARGS="--manifest build/release/<name>/fpga/arty_a7_100t/surfcpu_arty_a7_ddr/manifest.txt --kernel build/release/<name>/SURFOS.BIN"`.
From the flash, a 100T takes about 10 s to start: the open flow's bitstreams cannot set the
faster loading options, so the FPGA reads its flash over one data line at about 3 MHz.

## 9. Recovery

| Problem | What to do |
|---|---|
| A design misbehaves | Power-cycle the board, or load another design: volatile programming never touches the flash, so the board always comes back |
| SURFOS.BIN is damaged | SurfBoot falls back to `/BOOT/BACKUP.BIN` by itself and says so (seen in simulation) |
| You want the previous kernel | Hold **BTN0** while SurfBoot starts: it boots `BACKUP.BIN` |
| You want SurfBoot's monitor | Hold **BTN1** while SurfBoot starts |
| Any SurfBoot failure | It never stops silently: it prints the reason on the UART and stops in its monitor, with the failure's code in binary on the green LEDs LD4..LD7 and LD0 red (FIRMWARE.md lists the codes) |
| deploy-sd exit 2 | Eject, reinsert, `make -C surfcpu deploy-sd MOUNT=/Volumes/SURFOS DEPLOY_ARGS=--verify-only` |
| The card's file system is damaged | On the Mac: `diskutil verifyVolume /Volumes/SURFOS`, then copy your files off; reformat (section 7) only as a last resort. FAT32 is not journaled: an update cut off by pulling the card can damage it, and a backup file does not make an update atomic |
| Writing the flash | `make -C surfcpu program-flash BOARD=arty_a7_100t BIT=<file.bit>`: a separate command that checks the board, the bitstream and its build manifest, then asks you to type the board's name. I never run it without your go-ahead. Not used yet |
| A bad design in the flash | JTAG recovers it, whatever the flash holds (RELEASE.md section 7): unplug the USB cable and take **JP1 off**, plug it back in, `make -C surfcpu program-volatile BOARD=arty_a7_100t BIT=<a known-good .bit>` to see the board work, then `make -C surfcpu program-flash` with that file. Put JP1 back on and power-cycle: SurfBoot's banner comes after about 10 s. Keep the previous release until a new one has passed m9: its bitstream is the known-good one |

## 10. What I need from you

1. **Your board.** A7-35T or A7-100T, plus the PCB revision printed on it.
2. **The adapters.** Their revisions, and whether the keyboard is a real PS/2 keyboard.
3. **Bring-up.** Run `brew install openfpgaloader`, connect the board, and run steps 1.4 and 4 (or `make -C surfcpu board-test M=m1`). Send me the doctor output and the UART lines.
4. **The card.** Its size and type, when you get to section 7.
5. **A Vivado host, if you have one.** An x86-64 Linux machine (a PC, or a cloud VM) with AMD's free Vivado, reached over ssh. It is the fallback if the open-source flow cannot meet the DDR controller's timing (`FLOW=vivado-ssh`). Vivado in a Linux container on this Mac also exists as an option, unsupported and tens of GB ([BUILD-HOST.md](BUILD-HOST.md)).
