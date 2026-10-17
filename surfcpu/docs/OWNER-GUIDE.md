# SurfCPU on the Arty A7: the owner's guide

This guide takes you from the MacBook to SurfOS running on the Arty, one step at a time, with the
output to expect at each step. It ties together the detailed documents:
[WIRING.md](WIRING.md) (every pin), [BOARD.md](BOARD.md) (designs, programming),
[CARD.md](CARD.md) (the microSD card), [FIRMWARE.md](FIRMWARE.md) (SurfBoot),
[OPEN-FLOW.md](OPEN-FLOW.md) (building bitstreams on the Mac), [COMMANDS.md](COMMANDS.md),
[PHYSICAL-TESTS.md](PHYSICAL-TESTS.md) (the board tests).
Commands are typed in Terminal in the repository folder
(`SurfOS Restoration Project/06-restoration-surfcpu`).

Every output below is labelled. **Seen on this board** means it was copied from a capture of
your Arty's serial port (the files in [evidence/physical/](evidence/physical/)). **Seen on this
Mac** means it was copied from a real run on the Mac. **Rehearsed** means the real script ran
against a stand-in for the board or card. **Expected, from simulation** means the same RTL or
firmware produced it in simulation, not on the board. A step that has not run on the board
says so.

## 0. Where things stand (2026-10-17)

Your board is an Arty **A7-100T** (IDCODE 0x03631093) with JP1 and JP2 off, the Pmod MicroSD in
JA, the Pmod VGA in JB+JC, the Pmod PS2 in JD (its JP1 on VB), a VGA monitor, and your 32 GB
SanDisk card with SurfOS installed by `deploy-sd`. Every design so far was loaded over USB into
the FPGA's configuration RAM; **nothing has been written to the board's flash**.

| Milestone | State |
|---|---|
| 1 The baseline: LEDs, clocks, reset, UART | **passed on the board** (m1, 2026-10-16). Open: Vivado's post-route timing and DRC reports, which need a Vivado host (section 10) |
| 2 The CPU in simulation | done: compared with a reference model instruction by instruction, with formal checks |
| 3 The CPU on the board | **done on the board** (m3, 2026-10-16: all 203 BRAM diagnostics) |
| 4 The DDR3L memory | **physical tests passed** (2026-10-17): the DDR diagnostics pass all 572 checks on the board. Open: Vivado sign-off |
| 5 The card and SurfBoot | **seen on the board**: SurfBoot reads your card, calibrates the DDR3L and boots SurfOS from the card (2026-10-16/17). The m5 board test has not been run yet |
| 6 VGA and PS/2 | VGA **done on the board**: SurfBoot's text and the colour-bar test pattern. The PS/2 keyboard is pending: yours does not speak PS/2 (section 10) |
| 7 SurfOS on the board | **seen on the board**: SurfOS boots from the card to its shell over the UART; `uptime`, `ps`, `memstat` and the two-task test work, and it reads the card (mounted read-only). Its VGA console and the keyboard are not shown yet, and the m7 board test has not been run |
| 8 Files written by SurfOS | **not yet on the board**: it needs `sd=rw` on the card and the m8 test. It passes in simulation (written, ejected, read back after a second boot, checked with `fsck.fat` and mtools), and the storage code is in its fourth round of review fixes |
| 9 The standalone release | the release tooling is done and reviewed; the release must be made again. The flash has never been written: that waits for your go-ahead |

On the board today: SoC 0.2.0 (memory map 2), SurfBoot 1.1.0 and SurfOS 1.0.0. SurfBoot
initializes and read-levels the DDR3L memory itself; the memory controller (LiteDRAM) runs at
80 MHz, the DDR3L at 640 MT/s.

**The open-source flow had a defect, fixed on 2026-10-17.** nextpnr could route ordinary
signals through unused global clock buffers (BUFGCTRL route-throughs). The flow's checks saw a
plain wire there, but on the silicon those signals never arrived: two builds of the DDR
diagnostics failed on your board that way. The flow now routes with a chip database without
those routes, and its check `gclkcheck` fails any build that has one (OPEN-FLOW.md section 6.8).
**Any bitstream the open flow made before the fix is suspect until it is rebuilt**; the commands
below build new ones.

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

   These are the image's own chip databases. The flow's first build for each FPGA generates a
   corrected one with the image's own generator (without the route-throughs of section 0; about
   a minute) and keeps it in `surfcpu/build/openxc7/chipdb/`.

3. Install the programmer (you run this one; it is the only thing installed on the Mac itself;
   yours is openFPGALoader 1.1.1):

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
   openFPGALoader lines show the board's cable, its two serial ports and the FPGA's IDCODE:
   `0x0362D093` is an A7-35T, `0x03631093` an A7-100T. Yours read `0x03631093`, an A7-100T
   (`tools/program.sh detect`, 2026-10-16). Every command below takes `BOARD=arty_a7_100t`
   (the default).
5. Optional: `make -C surfcpu board-test-selftest` (15 seconds) runs the board test kit against
   fake boards on the Mac's own pseudo-terminals. It checks that the Mac's own `python3` runs
   the kit; nothing needs installing. Seen on this Mac (2026-10-17, last line):
   `test-os-kit (fake): 18 of 18 checks passed in 15 s`.

## 2. Check and build

1. Run every automated check, about 25 minutes:

   ```
   make -C surfcpu check
   ```

   Seen on this Mac, last line: `check: all automated SurfCPU checks passed`. Any failure stops it
   with the failing test's output.

2. The boot ROM and the diagnostics:

   ```
   make -C surfcpu build-rom
   ```

   Seen on this Mac (2026-10-17, last lines):

   ```
   build-rom: surfboot.elf text 43680 data 0 bss 7856: ROM 43680 of 65536 bytes (66.7%)
   build-rom: bramdiag.elf ROM 31392 of 65536 bytes (47.9%)
   ```

3. The SurfOS kernel package, `build/riscv32/arty_a7/SURFOS.BIN`:

   ```
   make -C surfcpu build-kernel
   ```

   Seen on this Mac (2026-10-17, one of its lines):
   `surfimg: build/riscv32/arty_a7/SURFOS.BIN: SurfOS 1.0.0 build 98b2b3c8, load 0x80000000, entry 0x80000000, payload 392340 bytes, mem_size 1246432, payload CRC 0xbea3e98d`

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

5. The two DDR designs (sections 5 and 6) also need LiteDRAM's generated memory controller,
   made once with `make -C surfcpu litedram-gen` (the first time it fetches LiteDRAM's pinned
   sources; the result is checked against `ip/litedram/arty.sha256`).

## 3. Wire the board (power off, USB unplugged) **done on 2026-10-16**

[WIRING.md](WIRING.md) has every pin; these are the rules that protect the hardware.

| Adapter | Where | How |
|---|---|---|
| Pmod MicroSD | JA (a 12-pin adapter: both rows) | component side up, pin 1 at JA pin 1; no card in it until SurfBoot runs (WIRING.md U8) |
| Pmod VGA | JB + JC together | **component side up, the VGA socket pointing away from the board, the adapter's J1 in JB.** Upside down it shorts FPGA outputs together and to ground through the 0 Ω JB/JC ports, which can damage the FPGA (WIRING.md U2) |
| Pmod PS2 | JD, **top row** (the upper row of sockets, farther from the board: pins 1-6) | component side up, its pin 1 at JD pin 1; **its jumper JP1 on VB** (the Arty's 3.3 V). Never 5 V from outside: that would pull the lines above the FPGA's limits (WIRING.md section 4) |

Two different jumpers are both called JP1:

* **The Arty's JP1 (MODE)**: take it **off** for bring-up, so the FPGA waits for the Mac
  instead of loading Digilent's demo from flash at power-on (that demo could drive the Pmod
  pins).
* **The Pmod PS2's JP1 (supply)**: **on VB**.
* **The Arty's JP2**: **off**. Your board came with it fitted, and then every program that opens
  the serial port (the test kit, the console, `screen`) resets the board: the first bring-up saw
  the count start again at 1 each time. You took it off on 2026-10-16; keep it off.

Also connect nothing to the Pmod PS2's JP2 header (its silkscreen has VE and GND swapped).
BOARD.md section 12 is the full checklist. **The VGA monitor's input must be set to VGA**:
yours showed nothing until it was.

Bring the adapters up in order: power on with only the USB cable first and load the baseline
design (section 4), which leaves JB, JC and JD unassigned so the FPGA drives none of their
pins; only then load designs that drive them. The microSD card goes in only when SurfBoot is
running (section 6).

Still open: the PCB revision printed on the Arty and the adapters' revisions (INVENTORY.md
section 4). Your keyboard is answered: it is a USB keyboard on a passive PS/2 adapter, and a
real PS/2 keyboard is needed (section 10).

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
   /dev/cu.usbserial-<serial>1 115200` works too; quit it with Ctrl-A, then K, then Y. One line
   a second. Seen on this board (m1, 2026-10-16), after you set the switches to 0101 and the
   script had typed `S`:

   ```
   SurfCPU baseline arty_a7_100t 0x00000100 build 2d1f32b2 clean 1792165445 | up 000488 s | pix 25173611 Hz | sw 0101 btn 0000 rx 53
   ```

   The board and its id, the build's commit, `clean` or `dirty`, the build time, seconds since
   reset, the pixel clock as measured (25,173,611 ± 1 Hz), the switches, the buttons and the
   last key you typed (`53` is `S`).
4. On the board: LD0 and LD1 blink green together once a second, switches SW0..SW3 light
   LD4..LD7, buttons light the RGB LEDs blue, RESET restarts the count. You confirmed this on
   2026-10-16.

That was milestone 1's physical test: `make -C surfcpu board-test M=m1` passed, 11 checks plus
your answer about the LEDs ([evidence](evidence/physical/m1-2026-10-16-2.txt)). The command asks
for each step, for example "load the design", "set the switches to 0101" or "do the LEDs
blink?", checks the UART lines itself and writes `surfcpu/docs/evidence/physical/m1-<date>.txt`,
transcript included. [PHYSICAL-TESTS.md](PHYSICAL-TESTS.md) describes the six tests (m1, m3, m5,
m7, m8, m9). Milestone 1 stays open only for Vivado's timing and DRC reports.

## 5. The CPU on the board (milestone 3) **passed on 2026-10-16**

The full SoC with the CPU and memory diagnostics in its boot ROM; only the USB cable is
needed:

```
make -C surfcpu build-fpga FLOW=openxc7 BOARD=arty_a7_100t DESIGN=surfcpu_arty_a7_bramdiag   # about 14 minutes
make -C surfcpu program-volatile BOARD=arty_a7_100t DESIGN=surfcpu_arty_a7_bramdiag
```

Seen on this board (2026-10-16, SoC 0.2.0, build e3961772; the whole run takes about 0.16 s):

```

SurfCPU BRAM diagnostics 1.0.0 (build e3961772)
board Arty A7-100T (0x100), SoC 0.2.0, memory map 2, sys 25000000 Hz, timebase 1000000 Hz
bitstream build e3961772 flags 0, features 7f, DDR status 0 size 0 MiB
identity                           ok (7 checks)
ALU, immediates, x0                ok (33 checks)
M extension                        ok (23 checks)
branches and jumps                 ok (9 checks)
loads and stores                   ok (8 checks)
CSRs and counters                  ok (30 checks)
scratch RAM (March C-)             (56 KiB) ok (49 checks)
text RAM (cells 2400-4095)         ok (1 checks)
exceptions, each recovered         ok (34 checks)
interrupts, timer and WFI          (10 timer ticks in 10186 us) ok (7 checks)
FENCE.I, code written to RAM       ok (2 checks)
DDR                                skipped: no DDR controller in this bitstream

BRAMDIAG: PASS (203 checks)

UART echo: type; each character comes back with its code. Ctrl-R runs the diagnostics again.
```

Then each character you type comes back with its code (seen on this board: `echo   1: 'A'
0x41`). LD4-LD7 light and LD0 is green (red on a failure); from then on LD7 blinks about once a
second, the heartbeat of the loop that echoes typed characters. This is milestone 3's physical
test; as one command, `make -C surfcpu board-test M=m3`, it passed on 2026-10-16 with the
earlier SoC 0.1.0 build of this design (19 checks plus your answer about the LEDs,
[evidence](evidence/physical/m3-2026-10-16.txt)).

### The DDR diagnostics (milestone 4) **passed on 2026-10-17**

The same diagnostics in the design with the DDR3L memory: after the CPU and RAM sections they
initialize the DDR3L themselves through the controller's registers, level its read path (a
delay and a bitslip for each of its two byte lanes) and test it. Only the USB cable is needed;
the adapters may stay plugged in (section 3). After `make -C surfcpu litedram-gen build-rom`
(section 2):

```
make -C surfcpu build-fpga FLOW=openxc7 BOARD=arty_a7_100t DESIGN=surfcpu_arty_a7_ddr_bramdiag   # about 22 minutes
make -C surfcpu program-volatile BOARD=arty_a7_100t DESIGN=surfcpu_arty_a7_ddr_bramdiag
```

Seen on this board (2026-10-17, built at 0bcadbc with the fixed flow; the capture leaves out the
16 read-leveling scan lines, one for each lane and bitslip, whose form BOARD.md section 12
shows):

```

SurfCPU BRAM diagnostics 1.0.0 (build 0bcadbcf)
board Arty A7-100T (0x100), SoC 0.2.0, memory map 2, sys 25000000 Hz, timebase 1000000 Hz
bitstream build 0bcadbcf flags 0, features ff, DDR status 14 size 256 MiB
identity                           ok (7 checks)
ALU, immediates, x0                ok (33 checks)
M extension                        ok (23 checks)
branches and jumps                 ok (9 checks)
loads and stores                   ok (8 checks)
CSRs and counters                  ok (30 checks)
scratch RAM (March C-)             (56 KiB) ok (49 checks)
text RAM (cells 2400-4095)         ok (1 checks)
exceptions, each recovered         ok (34 checks)
interrupts, timer and WFI          (10 timer ticks in 10186 us) ok (7 checks)
FENCE.I, code written to RAM       ok (2 checks)
DDR initialization (DDR_STATUS.SWINIT), read leveling:
  ... the 16 scan lines (m0 and m1, b00 to b07) ...
  lane 0: bitslip 2, delay 16 (window +-8)
  lane 1: bitslip 2, delay 16 (window +-8)
  229 ms
DDR initialization                 ok (3 checks)
DDR calibration                    ok (1 checks)
DDR data lines                     ok (192 checks)
DDR byte lanes                     ok (9 checks)
DDR address lines and aliases      ok (54 checks)
DDR boundaries                     ok (98 checks)
DDR patterns                       (1024 KiB in full, 4096 blocks sampled) ok (2 checks)
DDR sustained copies               (4 x 256 KiB copied and checked, 720 KiB/s) ok (4 checks)
code running from DDR              ok (6 checks)

BRAMDIAG: PASS (572 checks)

UART echo: type; each character comes back with its code. Ctrl-R runs the diagnostics again.
```

`DDR status 14` is hexadecimal: the controller is present and waits for software to initialize
it. Those are milestone 4's physical tests (calibration, data lines, byte lanes, address lines
and aliases, boundaries, patterns, sustained copies, code copied into DDR and run there). As one
command: `make -C surfcpu board-test M=m3 ARGS="--design surfcpu_arty_a7_ddr_bramdiag"`, which
checks every section and the build identity against this design's manifest (not yet run on the
board with this design).

## 6. SurfBoot, the DDR memory and the card **seen on this board, 2026-10-16/17**

`surfcpu_arty_a7_ddr` is the full SoC with the DDR3L memory and SurfBoot 1.1.0 in its boot ROM:
the design that boots SurfOS. SurfBoot initializes and read-levels the DDR3L itself, tests it
briefly, reads the card over SPI and loads `/BOOT/SURFOS.BIN` into the DDR3L. With the adapters
plugged in (section 3) and your card in the slot (section 7), after `make -C surfcpu
litedram-gen build-rom` (section 2):

```
make -C surfcpu build-fpga FLOW=openxc7 BOARD=arty_a7_100t DESIGN=surfcpu_arty_a7_ddr   # about 22 minutes
make -C surfcpu program-volatile BOARD=arty_a7_100t DESIGN=surfcpu_arty_a7_ddr
```

Start `make -C surfcpu uart-console` first, in another window, to see SurfBoot's first lines.
Seen on this board (2026-10-17, built at 0bcadbc; SurfOS build 22cbdc9a on your card), SurfBoot's
part:

```

SurfBoot 1.1.0 (build 0bcadbcf), the SurfCPU boot ROM
soc: Arty A7-100T (0x100), SoC 0.2.0, memory map 2, 25000000 Hz; bitstream 0bcadbcf
ddr: initialized by SurfBoot, read leveling lane 0: bitslip 2, delay 16 (window +-8); lane 1: bitslip 2, delay 16 (window +-8)
ddr: calibrated after 187 ms, 256 MiB, quick test passed (data, address, byte lanes)
sd: SDHC/SDXC, 29.7 GiB; ready after 11 ms; SPI 6250 kHz, data CRC checked
fat32: partition 1 (type 0x0c) at sector 8192, 32768-byte clusters, 973584 clusters, label "SURFOS"
boot: /BOOT/SURFOS.BIN
boot: loading 379 KiB .....
boot: SurfOS 1.0.0 build 22cbdc9a: 388740 bytes in 2 fragments, read in 2054 ms
boot: load 0x80000000, entry 0x80000000, mem_size 1242768, ISA IM_Zicsr_Zifencei; payload CRC 0x2db948e5 matches (card and DDR)
boot: no /BOOT/CMDLINE.TXT; empty command line
boot: starting the kernel at 0x80000000 (a0 = 0, a1 = 0x0001f000, a2 = 0x49465253)
```

SurfOS follows (section 8); its prompt came about 5 s after SurfBoot's banner (in the captures
of 2026-10-16, which have times). The same text appears on the VGA monitor: you confirmed
SurfBoot's text readable with nothing cut off (2026-10-16, SurfBoot 1.0.0).

**SurfBoot's monitor.** Hold **BTN1** while SurfBoot starts (press RESET with BTN1 held) to stop
in its monitor; any boot failure stops there too, with the reason on the UART and its code on
LD4..LD7 (FIRMWARE.md section 5). Its commands (`help` lists them): `boot`, `backup`, `check`,
`info`, `ddr`, `sd`, `part`, `fs`, `ls`, `cat`, `chain`, `md`, `mw`, `spi`, `reset`. Seen on this
board, with SurfBoot 1.0.0 in the design without DDR (`surfcpu_arty_a7_top`), which stops in its
monitor by itself (`SurfBoot stopped: no DDR controller (DDR_ABSENT)`, LED code 1):

```
surfboot> sd
sd: SDHC/SDXC, sector addressing, OCR c0ff8000, ready after 173 ms, data CRC on
sd: SPI 390 kHz for identification, 6250 kHz for data (DIV 1)
sd: CSD version 2, 62333952 sectors (30436 MiB)
sd: CID manufacturer 03, OEM SD, product SD32G rev 8.5, serial 67375102, made 2026-06
surfboot> ls /BOOT
  d-h--  .                  <dir>  cluster 17
  d-h--  ..                 <dir>  cluster 0
  ----a  SURFOS.BIN        388724  cluster 23
  1 file (388724 bytes), 2 directories
```

(`sd` before SurfOS was on the card; `ls /BOOT` after `deploy-sd`, when `cat` of the whole
`SURFOS.BIN` also agreed with the Mac's copy at every byte position.) The colour-bar test
pattern: `mw 0x10006000 0x1` at the monitor, and `mw 0x10006000 0x1b` back to text. Seen on this
board: the white border, the eight bars and the four 16-step bands exactly as designed (SurfBoot
1.0.0; the video block has the same address in memory map 2). Not yet seen on the board with
SurfBoot 1.1.0: the BTN1 stop, and the stop without a card (`SurfBoot stopped: no card
(NO_CARD)`, LED code 2, expected from simulation).

Milestone 5's whole test, `make -C surfcpu board-test M=m5`, asks for `surfcpu_arty_a7_ddr`. It
needs the DDR diagnostic next to the kernel on the card (on the Mac, after `make -C surfcpu
build-rom`: `cp surfcpu/build/fw/DDRDIAG.BIN /Volumes/SURFOS/BOOT/`, then eject). It boots that
diagnostic from the card, tries a missing and an invalid image, then has you take the card out
and put it back. Not yet run on the board.

## 7. Prepare the microSD card (once) and install SurfOS **done on your card, 2026-10-16**

[CARD.md](CARD.md) has the details. Your card is a 32 GB SanDisk SDHC (SD32G) with one FAT32
partition on an MBR, label `SURFOS`; SurfOS was installed with step 2 (build fd374065, then
22cbdc9a), and SurfBoot and SurfOS read it on the board. In short:

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

## 8. Booting SurfOS **seen on this board over the UART; VGA console and keyboard not yet**

SurfOS runs from the DDR3L memory, so it needs `surfcpu_arty_a7_ddr` (section 6). The steps:

1. Your card with SurfOS (section 7) in the Pmod MicroSD.
2. `make -C surfcpu uart-console` in one window.
3. In another: `make -C surfcpu program-volatile BOARD=arty_a7_100t DESIGN=surfcpu_arty_a7_ddr`
   (after building it, section 6).

Seen on this board (2026-10-17, built at 0bcadbc, SurfOS build 22cbdc9a), after SurfBoot's lines
of section 6; selected lines, `...` marks those left out (the whole capture:
[evidence](evidence/physical/ddr-fixed-flow-2026-10-17.txt)):

```

SurfOS: serial console on the arty_a7 UART (115200 8N1)
Memory: kernel 0x80000000-0x8012f68f, DMA heap 0x80130000-0x8022ffff, kernel heap 0x80230000-0x8effffff (237 MB)
Booting SurfOS Kernel.....
...
+GPIO: LEDs, switches 0x5, buttons; LD4 is the heartbeat
+PS2: surf_ps2 at 0x10005000, intc source 1: scan code set 2, translated to set 1 for the keyboard code
+SD: sd0: SDHC 29 GB, SD32G rev 8.5, manufacturer 0x03 'SD', serial 67375102, made 2026-06, CRC checked; read-only (sd=rw on the command line writes)
+PART: sd0p1: MBR partition 1, blocks 8192-62333951 (29G), type 0c FAT32 LBA

Drivers: serial gpio ps2kbd sdcard

File Systems
+FAT32: sd0p1: label SURFOS, 973584 clusters of 32768 bytes, 7607 FAT sectors x 2, data at block 16384; 973447 free (FSInfo)
+VFS: sd0p1 mounted on /sd0p1 (fat, read-only)
...
Brandon Burr's SurfOS
Version 1.0 on a RISC-V (SurfCPU on the Arty A7), 2004-2026
SURF ≋#>
```

The prompt is coloured (the escape codes are left out here). At the shell, seen on this board
(2026-10-16, the earlier build 92a2b51 of this design with SurfOS build fd374065):

```
SURF ≋#> uptime
    up 36.78 s (3678 ticks at 100 Hz), 8 tasks, 1 timers pending
    mtime 41102688 at 1000000 Hz; heartbeat 74
SURF ≋#> ps

SurfOS Task List
--------------------------
  PID  STATE     PRIO    CPU(ticks)  SWITCHES  PARENT  NAME
    0  ready     LOW           4425       313      -1  Kernel Idle Task
    1  blocked   HIGH             4         2      -1  init
    2  sleeping  LOW              0        91       1  heartbeat
    3  sleeping  LOW              0       225       1  sdwatch
    4  running   HIGH            85        13       1  Shell 0
    5  blocked   HIGH             3         5       1  Shell 1
    6  blocked   HIGH             4         5       1  Shell 2
    7  blocked   HIGH             4         4       1  Shell 3

8 tasks
SURF ≋#> memstat

SurfOS Memory Statistics
--------------------------
Physical memory: 262144KB at 0x80000000 (machine mode: no paging, no protection between tasks)
Kernel: 0x80000000-0x8012f67f (BSS 0x8005ee40-0x8012f67f)
Memstart: 0x8012f680  Memend: 0x90000000

kernel heap: 0x80230000-0x80260000 (192 KB grown in 3 steps, room to 0x8f000000)
  used: 143392 bytes in 22 blocks   free: 52848 bytes in 1 blocks
dma heap: 0x80130000-0x80130000 (0 KB grown in 0 steps, room to 0x80230000)
  used: 0 bytes in 0 blocks   free: 0 bytes in 0 blocks
heap check: ok

SURF ≋#> tasks
    started task A (pid 8) and task B (pid 9)
    task A: round 1 of 4, tick 6142, sleeping 100 ms
    task B: round 1 of 3, tick 6147, sleeping 170 ms
    task A: round 2 of 4, tick 6166, sleeping 100 ms
    task B: round 2 of 3, tick 6182, sleeping 170 ms
    task A: round 3 of 4, tick 6185, sleeping 100 ms
    task B: round 3 of 3, tick 6207, sleeping 170 ms
    task A: round 4 of 4, tick 6210, sleeping 100 ms
    task A ran 4 rounds, task B 3, in 99 ticks; both ended (status 0, 0)
```

And with SurfOS build 22cbdc9a, which reads the card (2026-10-16):

```
SURF ≋#> ls /sd0p1
    d        0  2026-10-16 13:59:10  .Spotlight-V100/
    d        0  2026-10-16 15:22:32  .fseventsd/
    d        0  2026-10-16 13:59:56  BOOT/
    d        0  2026-10-16 13:59:56  SYSTEM/
    d        0  2026-10-16 13:59:56  PROGRAMS/
    d        0  2026-10-16 13:59:56  USERS/
    6 entries, 0 bytes
SURF ≋#> df
    Device  Type       Size     Used    Avail  Use%  Mounted on    Label
    sd0p1   fat32    29.7 GB   4.2 MB  29.7 GB    1%  /sd0p1        SURFOS
```

(`.Spotlight-V100` and `.fseventsd` are macOS's own folders, and harmless.)

**Allowing SurfOS to write to the card.** SurfOS mounts the card read-only unless the kernel's
command line says `sd=rw` (its `+SD:` line says so). On the Mac, with the card mounted:

```
make -C surfcpu deploy-sd MOUNT=/Volumes/SURFOS DEPLOY_ARGS="--cmdline sd=rw"
```

This writes `/BOOT/CMDLINE.TXT` with the one line `sd=rw` (and installs the current `SURFOS.BIN`
as usual, the previous one kept as `BACKUP.BIN`). SurfBoot then hands that line to the kernel
(instead of saying `boot: no /BOOT/CMDLINE.TXT; empty command line`), and SurfOS mounts
`/sd0p1` read-write. **Not yet run on the board.** The fourth review of the storage code found defects
that are being fixed (one of them, a truncation whose directory read fails, can rename the file
to junk); until those fixes are in, let SurfOS write only to a card whose files you also have on
the Mac. `--cmdline ""` makes the card read-only again.

**Not shown on the board yet:** SurfOS's console on the VGA monitor, and the PS/2 keyboard
typing into it (it needs a real PS/2 keyboard, section 10). Milestone 7's test as one command:
`make -C surfcpu board-test M=m7` (it asks for `surfcpu_arty_a7_ddr`; not yet run on the board).
It also asks you to type on the PS/2 keyboard: without one, answer `n` at that step and the
keyboard is listed as not done, which leaves milestone 7 open.

### Writing files from SurfOS (milestone 8) **not yet run on the board**

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

Expected, from simulation (the RTL system, with a card image made by `tools/mkcard.py` whose
command line is `ascii sd=rw`, hence `~` in place of `≋` in the prompt):

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

**The board test for milestone 8** (the brief's exit evidence). The Mac's `crc32` command
prints the same CRC-32 as SurfOS's `crc32`:

1. On the Mac, make a file on the card and note its CRC:
   `printf 'made on the Mac\n' > /Volumes/SURFOS/USERS/DOCUMENTS/MAC.TXT`, then
   `crc32 /Volumes/SURFOS/USERS/DOCUMENTS/MAC.TXT`. Eject: `diskutil eject /Volumes/SURFOS`.
2. On the board: `crc32 /sd0p1/USERS/DOCUMENTS/MAC.TXT` gives the same CRC (a file made on
   the Mac, read on the board).
3. Make and change files (`write`, `append`, `cp`) and note their `crc32`. Then `eject sd0`,
   switch the board off and on, load the design again, and boot: the same CRCs (persistence
   through a power cycle after a clean eject).
4. `eject sd0` again and take the card to the Mac: `crc32` of each file there gives the same
   CRCs (the Mac reads matching bytes).
5. Install a new kernel with `deploy-sd` (section 7): the files in `/USERS/DOCUMENTS` keep
   their CRCs (a kernel update leaves your files alone).

Or let the kit lead you through these steps and compare the CRCs itself: `make -C surfcpu
board-test M=m8 ARGS="--mount /Volumes/SURFOS"` (it asks for `surfcpu_arty_a7_ddr`). It reads
the card's files on the Mac, read only. Not yet run on the board.

Milestone 9's ten cold boots and one-hour soak are `make -C surfcpu board-test M=m9`. They
need the release made again (RELEASE.md) and its `surfcpu_arty_a7_ddr` bitstream written to the
configuration flash, which waits for your go-ahead. With a release, the test takes the
release's manifest and kernel (RELEASE.md section 10):
`make -C surfcpu board-test M=m9 ARGS="--manifest build/release/<name>/fpga/arty_a7_100t/surfcpu_arty_a7_ddr/manifest.txt --kernel build/release/<name>/SURFOS.BIN"`.
From the flash, a 100T should take about 10 s to start (computed from the bitstream's length,
not yet measured): the open flow's bitstreams cannot set the faster loading options, so the
FPGA reads its flash over one data line at about 3 MHz.

## 9. Recovery

| Problem | What to do |
|---|---|
| A design misbehaves | Power-cycle the board, or load another design: volatile programming never touches the flash, so the board always comes back |
| A bitstream built before 2026-10-17 misbehaves | Rebuild it: the open flow's defect of section 0 could make a build fail on the silicon although every check passed |
| The VGA monitor shows nothing | Set the monitor's input to VGA (yours showed nothing until it was) |
| SURFOS.BIN is damaged | SurfBoot falls back to `/BOOT/BACKUP.BIN` by itself and says so (seen in simulation) |
| You want the previous kernel | Hold **BTN0** while SurfBoot starts: it boots `BACKUP.BIN` |
| You want SurfBoot's monitor | Hold **BTN1** while SurfBoot starts |
| Any SurfBoot failure | It never stops silently: it prints the reason on the UART and stops in its monitor, with the failure's code in binary on the green LEDs LD4..LD7 and LD0 red (FIRMWARE.md lists the codes; seen on this board: DDR_ABSENT, code 1, from the design without DDR) |
| deploy-sd exit 2 | Eject, reinsert, `make -C surfcpu deploy-sd MOUNT=/Volumes/SURFOS DEPLOY_ARGS=--verify-only` |
| The card's file system is damaged | On the Mac: `diskutil verifyVolume /Volumes/SURFOS`, then copy your files off; reformat (section 7) only as a last resort. FAT32 is not journaled: an update cut off by pulling the card can damage it, and a backup file does not make an update atomic |
| Writing the flash | `make -C surfcpu program-flash BOARD=arty_a7_100t BIT=<file.bit>`: a separate command that checks the board, the bitstream and its build manifest, then asks you to type the board's name. I never run it without your go-ahead. Not used yet |
| A bad design in the flash | JTAG recovers it, whatever the flash holds (RELEASE.md section 7): unplug the USB cable and take **JP1 off**, plug it back in, `make -C surfcpu program-volatile BOARD=arty_a7_100t BIT=<a known-good .bit>` to see the board work, then `make -C surfcpu program-flash` with that file. Put JP1 back on and power-cycle: SurfBoot's banner should come after about 10 s. Keep the previous release until a new one has passed m9: its bitstream is the known-good one |

## 10. What I need from you

1. **A PS/2 keyboard the board can use.** Your MCSAITE PS/2 keyboard is specified for 5 V and
   does not start on the Pmod PS2's 3.3 V: its lights do not flash at power-up, it does not
   answer the LED command, and nothing arrives from it
   ([evidence](evidence/physical/ps2-2026-10-17.txt)). The Amazon Basics USB keyboard on a
   passive adapter before it does not speak PS/2 at all. There are two ways forward:
   * a 4-channel bidirectional logic level converter of the BSS138 type (pins already soldered)
     and female-to-female jumper wires, which let that keyboard run on 5 V from the Arty's 5V
     pin while the FPGA side stays at 3.3 V (WIRING.md section 4). The wiring is given, and
     checked with you, once you have them;
   * a PS/2 keyboard that runs at 3.3 V.

   Milestone 6's keyboard part and milestone 7's shell on the keyboard wait for one. Until the
   wiring is given, keep the Pmod PS2 in JD's top row with JP1 on VB, and connect no 5 V to it
   or to JD.
2. **Reporting the openXC7 defect upstream: your call.** The route-through defect of section 0
   is in the open toolchain's chip database, and any openXC7 user can hit it. Whether it is
   reported to the openXC7 project, and by whom, is your decision; it has not been reported from
   here.
3. **Writing the configuration flash: your go-ahead.** Milestone 9 needs the board to start
   from its flash. Nothing is written to it without your word; before that, the release is made
   again (RELEASE.md) and the flash part read (INVENTORY.md G7).
4. **A Vivado host: optional.** Milestones 1 and 4 have passed their physical tests with the
   open flow; only the brief's Vivado sign-off (post-route timing and DRC reports) is open. That
   needs an x86-64 Linux machine (a PC, or a cloud VM) with AMD's free Vivado, reached over ssh
   (`FLOW=vivado-ssh`). Vivado in a Linux container on this Mac also exists as an option,
   unsupported and tens of GB ([BUILD-HOST.md](BUILD-HOST.md)).
5. **Still open:** the PCB revision printed on the Arty and the adapters' revisions (INVENTORY.md
   section 4).
