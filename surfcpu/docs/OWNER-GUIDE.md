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
| Bitstreams | built on this Mac with the open-source flow for the milestone 1 design; the full SoC is next |
| DDR memory on the board | the bridge is done; the memory controller in the bitstream is a later step, so booting SurfOS on the board waits for it |
| The board itself | nothing has been connected or programmed yet |

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
| Pmod MicroSD | JA, top row | component side up, pin 1 at JA pin 1 |
| Pmod VGA | JB + JC together | **component side up, the VGA socket pointing away from the board, the adapter's J1 in JB.** Upside down it shorts FPGA outputs together and to ground through the 0 Ω JB/JC ports, which can damage the FPGA (WIRING.md U2) |
| Pmod PS2 | JD, **top row** | component side up, its pin 1 at JD pin 1; **its jumper JP1 on VB** (the Arty's 3.3 V). Never 5 V from outside: that would pull the lines above the FPGA's limits (WIRING.md section 4) |

Two different jumpers are both called JP1:

* **The Arty's JP1 (MODE)**: take it **off** for bring-up, so the FPGA waits for the Mac
  instead of loading Digilent's demo from flash at power-on (that demo could drive the Pmod
  pins).
* **The Pmod PS2's JP1 (supply)**: **on VB**.

Bring the adapters up in order: power on with only the USB cable first and load the baseline
design (section 4), which leaves JB, JC and JD unassigned so the FPGA drives none of their
pins; only then load designs that drive them. The microSD card goes in only when SurfBoot is
running (section 6).

Also open: the PCB revision printed on the Arty, the adapters' revisions, and whether your
keyboard is a real PS/2 keyboard that works at 3.3 V (INVENTORY.md section 4, G4).

## 4. First power-up: the baseline design (milestone 1) **pending**

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
3. Watch the UART: `screen /dev/cu.usbserial-<serial>1 115200`. The second of the two ports
   is the UART; quit `screen` with Ctrl-A then K, then Y. Expected one line a second, in this
   format (BOARD.md; simulation prints `sim` where the board prints `clean` or `dirty`):

   ```
   SurfCPU baseline arty_a7_100t 0x00000100 build 1a2b3c4d dirty 1760560000 | up 000012 s | pix 25173611 Hz | sw 0101 btn 0000 rx 41
   ```

   The board and its id, the build's commit, `clean` or `dirty`, the build time, seconds since
   reset, the pixel clock as measured (25,173,611 ± 1 Hz), the switches, the buttons and the
   last key you typed.
4. On the board: LD0 and LD1 blink green together once a second, switches SW0..SW3 light
   LD4..LD7, buttons light the RGB LEDs blue, RESET restarts the count.

That is milestone 1's physical test. Please send me the lines you see, or a photo of the LEDs.

## 5. The CPU on the board (milestone 3) **pending: bitstream in progress**

The full SoC with the boot ROM holding `bramdiag` (the CPU and memory diagnostics) is being
built now. Expected, from simulation, on the UART: the diagnostics' report, ending in
`BRAMDIAG: PASS (569 checks)` (with DDR not yet in the design, its DDR tests report DDR
absent).

## 6. SurfBoot and the card reader **pending**

The full SoC with SurfBoot in its boot ROM. Without DDR in the design yet, SurfBoot reports DDR
absent and stops in its monitor, which already exercises the CPU, the UART, the card reader and
the VGA output. Its monitor commands (`help` lists them): `boot`, `check`, `ls`, `cat`,
`chain`, `part`, `fs`, `sd`, `ddr`, `md`, `mw`, `spi`, `info`, `reset`. Expected, from simulation,
the first lines:

```
SurfBoot 1.0.0 (build ...), the SurfCPU boot ROM
```

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
(`sd=rw` on the kernel command line, `/BOOT/CMDLINE.TXT`).

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
| Flash | Not used yet. Writing the board's configuration flash (milestone 9) will be a separate command that asks you to type the board's name |

## 10. What I need from you

1. **Your board.** A7-35T or A7-100T, plus the PCB revision printed on it.
2. **The adapters.** Their revisions, and whether the keyboard is a real PS/2 keyboard.
3. **Bring-up.** Run `brew install openfpgaloader`, connect the board, and run steps 1.4 and 4. Send me the doctor output and the UART lines.
4. **The card.** Its size and type, when you get to section 7.
