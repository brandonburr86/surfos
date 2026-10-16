# SurfCPU physical tests: one command per board milestone

The os track's test kit (2026-10-16). When the Arty is connected, each board milestone of the
brief (AGENT-INSTRUCTIONS.md section 10) is one command on the Mac. It opens the board's serial
port first and reads it the whole time, asks you for each step, checks what the port shows,
and writes the evidence file itself, transcript included: nobody copies text by hand.

**Nothing here has run on the board yet.** The procedures have run against the RTL system
(`surfsim --uart-pty`, the whole SoC in simulation) and against fake boards on
pseudo-terminals (section 6). A run against the RTL system says SIMULATION in its evidence and
never goes to the physical evidence folder; the brief's three kinds of result (simulation,
programming, physical) stay apart.

## 1. Before you start

* Nothing to install. `tools/board-test.py` and `tools/uart.py` use only the standard library
  of the Mac's own `/usr/bin/python3` (3.9): termios, select, os. No pyserial.
* Plug the Arty's USB cable in. Its FT2232 shows two ports: `/dev/cu.usbserial-<serial>0`
  (channel A: JTAG, which `program-volatile` uses) and `/dev/cu.usbserial-<serial>1`
  (channel B: the UART). `make -C surfcpu uart-list` says which is which. The commands find the
  UART by themselves when one Arty is plugged in; with more than one, give `PORT=`. A port that
  shows only garbage or nothing at all is usually channel A, or the wrong baud rate.
* Leave the Arty's JP2 open (BOARD.md section 12). Opening a serial port raises DTR and RTS, as
  every terminal program does; with JP2 open they reach nothing on the Arty. Shorted, JP2 lets
  DTR drive the RESET net (WIRING.md U7), so opening the port could reset the SoC. The tools
  never change DTR or RTS unless asked (`uart.py console --dtr on|off --rts on|off`).
* Close other programs that hold the port (`screen`, `make uart-console`): a test needs it to
  itself.
* `make -C surfcpu board-test-selftest` (15 seconds) runs the kit against fake boards on the
  Mac's own pseudo-terminals: a check that the Mac's Python runs it, before any board is there.

## 2. The commands

```
make -C surfcpu board-test M=m1 [PORT=/dev/cu.usbserial-<serial>1] [ARGS="..."]
```

| M | Milestone | What you do when asked | Time on the board |
|---|---|---|---|
| m1 | the baseline design | load it; set the switches to 0101; hold BTN0 for 2 s; say whether the LEDs blink; press RESET | about a minute |
| m3 | the CPU and its BRAM diagnostics | load the bramdiag design; say whether LD4..LD7 and LD0 are lit | under a minute |
| m5 | SurfBoot and the card, the DDR diagnostic | copy `DDRDIAG.BIN` to the card; hold BTN1 and press RESET; later take the card out, press RESET, put it back | a few minutes |
| m7 | SurfOS from the card | install SurfOS on the card; load the design; type `echo ps2-ok` on the PS/2 keyboard; say whether the VGA monitor shows the shell | a few minutes |
| m8 | files that persist (OWNER-GUIDE.md section 8) | make a file on the Mac; move the card to the board and back; power-cycle; install the kernel again | 10 to 15 minutes |
| m9 | the standalone release | program the flash once; power-cycle the board N times (`--boots`, default 10); then wait for the soak (`--minutes`, default 60) | about 1.5 hours |

Each run ends with one line, for example `board-test m3: PASS (25 checks seen by this script
passed; 1 not done or not compared); evidence in .../docs/evidence/physical/m3-2026-10-20.txt`,
and lists anything that failed. The exit status is 0 for PASS only.

Options (`ARGS="..."`): `--board arty_a7_35t`; `--manifest FILE` (another build manifest);
`--kernel FILE` and `--ddrdiag FILE` (the packages that were put on the card, default
`../build/riscv32/arty_a7/SURFOS.BIN` and `build/fw/DDRDIAG.BIN`); `--lines N` (m1, default
10); `--skip-no-card` (m5); `--mount /Volumes/SURFOS` (m8: the script reads the card's files on
the Mac itself, read only; without it you compare the CRCs it shows and answer y or n);
`--boots N`, `--minutes M`, `--soak-only` (m9); `--evidence-dir DIR`. The other commands:

```
make -C surfcpu uart-list                     the ports, and which one is the UART
make -C surfcpu uart-console [LOG=file]       a terminal at 115200 8N1; Ctrl-] quits
make -C surfcpu board-test-selftest           the kit against fake boards on this Mac
```

When a step asks you to do something, do it and press Enter (or type `n` if you could not). A
question about something only you can see takes `y`, `n` or `s` (could not see it).

## 3. What each one checks

Every procedure reads the build identity the board prints and compares it with the bitstream's
build manifest (`build/fpga/<BOARD>/<DESIGN>/manifest.txt`, tools/fpga-build.sh): the build id,
the dirty flag, the board, and that the build passed its checks. Where a package from the card
is loaded, SurfBoot's report of it (build id, size, payload CRC-32) is compared with the file
(`--kernel`, `--ddrdiag`). Without a manifest or a file the comparison is listed as not done,
never as passed.

A step that may take the port away (programming the FPGA, a power cycle: the board is powered
by the same USB cable) is marked as such: while you work, and after you press Enter until the
board's next expected line arrives (15 seconds at most if the port is not back by then), a port
that goes away is opened again once it is back, and the transcript marks both moments with `*`. If SurfBoot's first lines came before the port was back, the boot is read
from the kernel load on, and the bitstream's identity comes from the kernel's `bootinfo`
instead; the evidence says so. At any other moment a port that goes away ends the test at once
with "USB unplugged, or the board lost power?".

### m1: the baseline design (milestone 1)

Asks you to load `arty_a7_baseline`, then reads `--lines` consecutive 1 Hz lines and checks:
their form (OWNER-GUIDE.md section 4), the seconds counting up by one a line, a line every
second by the Mac's clock (0.7 to 1.3 s apart), the pixel clock at 25,173,611 ± 1 Hz (the first
line's measurement may be partial and is left out), the board and its id agreeing and being
the `--board` asked for, and the line's build id, clean or dirty, build time and board against
the manifest. It types `S` and checks a later line shows `rx 53` (the FPGA's receive pin). Then
you set the switches to 0101 (a line must show `sw 0101`), hold BTN0 for two seconds (a line
must show `btn ...1`), answer whether LD0 and LD1 blink and LD4..LD7 follow the switches (your
answer), and press RESET (a line must count from 1 or 2 again).

Proven against a fake board only: the baseline design is not in surfsim. The fake board's
checks, from `make test-os-kit`:

```
  ok       4 lines a second, each of the baseline's form ("SurfCPU baseline arty_a7_100t 0x00000100 build 1a2b3c4d clean 1760560000 | up ...")
  ok       the seconds since reset count up by one a line (1, 2, 3, 4)
  ok       a line every second by the Mac's clock (1.02, 0.98, 1.05 s)
  ok       the pixel clock measured 25,173,611 +- 1 Hz (25173611)
  ok       the board arty_a7_100t and its id 0x100 agree
  ok       the design was built for arty_a7_100t, the board asked for (--board)
  ok       the line's build 1a2b3c4d clean, time 1760560000 and board arty_a7_100t match the manifest's (...)
  ok       the FPGA's receive pin: the byte typed (S) shows as rx 53
  ok       the switches: a line shows sw 0101 as the owner set them
  ok       the buttons: a line shows BTN0 pressed (btn ...1)
  skip     not observed: Do LD0 and LD1 blink green together, once a second; do LD4..LD7 follow the switches; and did BTN0 light an RGB LED blue?
  ok       RESET restarts the count (up 000001 s)
```

### m3: the CPU on the board (milestone 3)

Asks you to load `surfcpu_arty_a7_bramdiag`, then reads the BRAM diagnostics to the end:
the identity lines (ROM build, board and id, the bitstream's build id and flags against the
manifest, the 25 MHz system clock), each section (identity, ALU, M extension, branches, loads
and stores, CSRs, scratch RAM, exceptions each recovered, interrupts with timer and WFI,
FENCE.I) `ok`, the timer's progress within 20 % of 1 ms a tick, any other section `ok` or
`skipped` (the bramdiag bitstream has no DDR: `DDR ... skipped`), and `BRAMDIAG: PASS`. Then it
types `Arty7` and checks each character comes back with its code (`echo 1: 'A' 0x41` ...). If
the first run came before the port was open, it types Ctrl-R to run the diagnostics again and
says so. You answer whether LD4..LD7 are lit and LD0 green.

On the RTL system (`surfsim` with bramdiag in its ROM; this SoC has the DDR model, so the DDR
sections run too), PASS in 20 s:

```
  ok       the bitstream's build id 2355dba2 is the manifest's (2355dba2, .../manifest-sim.txt)
  ok       its dirty flag (0) matches the manifest's source_dirty = false
  ok       the system clock is 25,000,000 Hz (25000000)
  ok       identity: ok
  ok       ALU, immediates, x0: ok
  ok       M extension: ok
  ok       branches and jumps: ok
  ok       loads and stores: ok
  ok       CSRs and counters: ok
  ok       scratch RAM: ok (56 KiB)
  ok       exceptions, each recovered: ok
  ok       interrupts, timer and WFI: ok (10 timer ticks in 10186 us)
  ok       FENCE.I, code written to RAM: ok
  ok       the timer's progress: 10 timer ticks in 10186 us (1 ms a tick, +- 20 %)
  ok       text RAM (cells 2400-4095): ok (1 checks)
  ok       DDR calibration: ok (1 checks)
  ok       DDR data lines: ok (192 checks)
  ok       DDR byte lanes: ok (9 checks)
  ok       DDR address lines and aliases: ok (54 checks)
  ok       DDR boundaries: ok (98 checks)
  ok       DDR patterns: ok (2 checks)
  ok       DDR sustained copies: ok (4 checks)
  ok       code running from DDR: ok (6 checks)
  ok       BRAMDIAG: PASS (569 checks)
  ok       the UART both ways: each byte typed (Arty7) came back with its code
  skip     not observed: Are LD4..LD7 lit and LD0 green (red would mean a failure)?
```

On the board the bramdiag bitstream prints `BRAMDIAG: PASS (203 checks)` with the DDR section
skipped (OWNER-GUIDE.md section 5).

### m5: SurfBoot and the card (milestone 5)

Needs the DDR design (`surfcpu_arty_a7_top`) and a SurfOS card with `DDRDIAG.BIN` copied to
`/BOOT/`. You hold BTN1 and press RESET, so SurfBoot stays in its monitor. The script checks
SurfBoot's identity against the manifest, then types monitor commands: `info` (DDR calibrated,
the card present), `boot /BOOT/NOSUCH.BIN` (a missing image: diagnosed `NOT_FOUND`, the monitor
back), `ls /SYSTEM` and `check /SYSTEM/SURFCARD.TXT` (an invalid image: SurfBoot's card marker
is no package, diagnosed `IMG_...`), `check /BOOT/DDRDIAG.BIN` (every package check passes) and
`boot /BOOT/DDRDIAG.BIN`: SurfBoot's report of the package against `--ddrdiag`, then the DDR
diagnostic running from DDR to `DDRDIAG: PASS`. Last, you take the card out and press RESET
(SurfBoot must stop with `no card (NO_CARD)`), and put it back (SurfBoot must say `card
inserted: booting` and read `/BOOT/SURFOS.BIN` by itself). The DDR diagnostic's depth comes
from `/BOOT/CMDLINE.TXT` (FIRMWARE.md; the default is its quick test of all of DDR).

On the RTL system (the card image's command line `size=2M loops=1` keeps the diagnostic to
2 MiB), PASS in 46 s; in the transcript, the RESET with the card out is a new surfsim run:

```
  ok       the bitstream's build id 2355dba2 is the manifest's (2355dba2, .../manifest-sim.txt)
  ok       its dirty flag (0) matches the manifest's source_dirty = false
  ok       info: DDR calibrated (the DDR bitstream): ddr: status 5 (present, calibrated), size 256 MiB
  ok       info: the card is present (card: present; buttons 2)
  ok       a missing image: boot /BOOT/NOSUCH.BIN is diagnosed (NOT_FOUND) and the monitor is back
  ok       an invalid image: check /SYSTEM/SURFCARD.TXT (not a package) is diagnosed (IMG_SHORT), the monitor is back
  ok       check /BOOT/DDRDIAG.BIN: every package check passes
  ok       DDRDIAG.BIN on the card is .../build/fw/DDRDIAG.BIN (build 2355dba2, 14992 bytes, payload CRC 0x5d7eaf8a)
  ok       the DDR diagnostic booted from the card: DDRDIAG: PASS (318 checks; 2 MiB from 0x80100000)
  ok       no card: SurfBoot stopped: no card (NO_CARD), in the monitor
  ok       the card back: "card inserted: booting", SurfBoot retries by itself
```

### m7: SurfOS from the card (milestone 7)

Needs the DDR design and a card made with `make deploy-sd`. You load the design (or press
RESET). The script follows the boot: SurfBoot's identity, `boot: SurfOS ... build ...` with the
size and payload CRC compared with `--kernel`, the handoff, the kernel's lines, the card mounted,
the banner and the prompt; any `SurfBoot stopped`, kernel panic, `SYSTEM HALTED`, `BOOT PROBLEM`
or killed task ends it as a FAIL with the line printed. At the shell: `bootinfo` (the kernel
reads the bitstream's identity), `tasks` twice (two kernel tasks counting, sleeping and waking,
their rounds interleaved, both ending with status 0), `uptime` twice three seconds apart (the
uptime, ticks and heartbeat advance, and on the board the uptime keeps time with the Mac's
clock within 25 %), then `echo kbd-ready` and you type `echo ps2-ok` on the PS/2 keyboard: the
port must show `ps2-ok`. You answer whether the VGA monitor shows the same shell.

On the RTL system (the keyboard model types `echo ps2-ok` when `kbd-ready` appears), PASS in
56 s:

```
  ok       the bitstream's build id 2355dba2 is the manifest's (2355dba2, .../manifest-sim.txt)
  ok       its dirty flag (0) matches the manifest's source_dirty = false
  ok       SurfOS 1.0.0 on the card is .../SURFOS.BIN (build 2355dba2, 376596 bytes, payload CRC 0x9794518d)
  ok       SurfOS booted from the card to the shell (SurfBoot, the kernel read and checked, the card mounted read-write)
  ok       bootinfo: the kernel reads the bitstream's identity (sysinfo: board 0x00f0, SoC 0x000100, map 1, sys clock 25000000 Hz, bitstream 2355dba2 (simulation))
  ok       two kernel tasks make progress, sleep and wake (run 1: task A ran 4 rounds, task B 3, in 82 ticks; both ended (status 0, 0); their rounds interleaved: BAABABA)
  ok       two kernel tasks make progress, sleep and wake (run 2: task A ran 4 rounds, task B 3, in 82 ticks; both ended (status 0, 0); their rounds interleaved: BAABABA)
  ok       uptime twice
  ok       the uptime advances: 3.09 s then 3.72 s, ticks 309 then 372, heartbeat 6 then 7
  note     the uptime's rate not checked against the clock: --slow 10
  ok       the PS/2 keyboard types into the shell: echo ps2-ok answered ps2-ok
  skip     not observed: Does the VGA monitor show the same shell: the banner and the commands just run?
```

The transcript's boot, from the same run:

```
  +    0.117 | SurfBoot 1.0.0 (build 2355dba2), the SurfCPU boot ROM
  +    0.153 | soc: simulation (0xf0), SoC 0.1.0, memory map 1, 25000000 Hz; bitstream 2355dba2 (simulation)
  +    0.255 | ddr: calibrated after 0 ms, 256 MiB, quick test passed (data, address, byte lanes)
  +    0.368 | sd: SDHC/SDXC, 36 MiB; ready after 5 ms; SPI 6250 kHz, data CRC checked
  +   13.840 | boot: SurfOS 1.0.0 build 2355dba2: 376596 bytes in 1 fragment, read in 1837 ms
  +   14.031 | boot: starting the kernel at 0x80000000 (a0 = 0, a1 = 0x0001f000, a2 = 0x49465253)
  +   16.954 | SurfOS: serial console on the arty_a7 UART (115200 8N1)
  +   25.199 | +VFS: sd0p1 mounted on /sd0p1 (fat)
  +   27.895 | Brandon Burr's SurfOS
  +   29.295 | SURF ~#> bootinfo
```

(The RTL system runs about seven times slower than the board: the kernel load's 1837 ms of
simulated time took 13 s.)

### m8: persistent files (milestone 8)

OWNER-GUIDE.md section 8's CRC procedure, with sd=rw on the card. The Mac-side steps are
prompts: you make `MAC.TXT` on the card on the Mac and move the card to the board. The script
never writes or formats a disk; with `--mount /Volumes/SURFOS` it reads the card's files there
itself (read only), otherwise it shows you the CRCs and asks. On the board it checks the card is
mounted read-write, reads `MAC.TXT`'s CRC (a file made on the Mac, read on the board), types
`write`, `append` and `cp` in `/sd0p1/USERS/DOCUMENTS`, and checks the CRCs of `BOARD.TXT`,
`COPY.TXT` and `MAC.TXT` against the CRC-32 it computes itself, then `eject sd0` (`safe to
remove`). You power-cycle the board (and load the design again: the FPGA forgets it at
power-off): after the boot the same CRCs. The card goes to the Mac: the same CRCs there. You
install the kernel again with `deploy-sd` and power-cycle: the files keep their CRCs. The
evidence ends with the list of files and the CRCs they must have.

On the RTL system (the Mac's side done with mtools on the card image while no surfsim runs; a
power cycle a new surfsim run on the image the last one flushed), PASS in about 2 minutes:

```
  ok       on the Mac (.../mount, read only): MAC.TXT has CRC b0cfa011, 16 bytes
  ok       the bitstream's build id 2355dba2 is the manifest's (2355dba2, .../manifest-sim.txt)
  ok       its dirty flag (0) matches the manifest's source_dirty = false
  ok       SurfOS 1.0.0 on the card is .../SURFOS.BIN (build 2355dba2, 376596 bytes, payload CRC 0x9794518d)
  ok       the card is mounted read-write (sd=rw in /BOOT/CMDLINE.TXT)
  ok       bootinfo: the kernel reads the bitstream's identity (sysinfo: board 0x00f0, SoC 0x000100, map 1, sys clock 25000000 Hz, bitstream 2355dba2 (simulation))
  ok       a file made on the Mac, read on the board: MAC.TXT b0cfa011 (b0cfa011 on the Mac)
  ok       write BOARD.TXT made on the board 2026-10-16T12:37:56Z: done without a word of error
  ok       append BOARD.TXT a second line: done without a word of error
  ok       cp MAC.TXT COPY.TXT: done without a word of error (16 bytes copied)
  ok       write MAC.TXT changed on the board: done without a word of error
  ok       create, edit, read: BOARD.TXT e1df0376, COPY.TXT b0cfa011, MAC.TXT f60b6140
  ok       eject sd0: safe to remove
  ok       after a power cycle the same CRCs: BOARD.TXT e1df0376, COPY.TXT b0cfa011, MAC.TXT f60b6140
  ok       eject sd0 again: safe to remove
  ok       on the Mac (read only, .../mount) the same CRCs: BOARD.TXT e1df0376, COPY.TXT b0cfa011, MAC.TXT f60b6140
  ok       after a kernel update the files keep their CRCs: BOARD.TXT e1df0376, COPY.TXT b0cfa011, MAC.TXT f60b6140
  ok       eject sd0 at the end: safe to remove
```

(`BOARD.TXT` holds the run's start time, so its CRC differs from run to run; the others do not.)

### m9: the standalone release (milestone 9)

Needs the bitstream in the board's flash (`program-flash`, JP1 MODE on) and a SurfOS card with
sd=rw. For each of `--boots` cold boots you unplug the USB cable, wait, plug it back in, and
press Enter; the script follows the boot from the flash to the prompt, runs `uptime` (a fresh
boot) and checks that every boot ran the same bitstream (the first one also against the
manifest and the kernel against `--kernel`). Then the soak: for `--minutes`, rounds of
`uptime` (it must advance; the task count must not pile up), `ps`, `tasks` (both end with
status 0), `write`, `cp`, `crc32` (both files' CRCs right), `rm` and `memstat` (`heap check:
ok`), failing at once on a panic, a killed task, `SYSTEM HALTED` or a wrong CRC. The evidence
notes the task count and the kernel heap's use over the soak. Over an hour the transcript is
long: the evidence keeps its first and last 2000 lines and the whole of it goes to
`build/board-test/`.

With a release (RELEASE.md section 10): the release's `surfcpu_arty_a7_ddr` bitstream in the
flash, its `card/BOOT` files on the card, and `ARGS="--manifest
build/release/<name>/fpga/<board>/surfcpu_arty_a7_ddr/manifest.txt --kernel
build/release/<name>/SURFOS.BIN"`. The open flow's bitstreams load from the flash at about 3 MHz,
one bit at a time: SurfBoot starts about 10 s after power-on on a 100T (6 s on a 35T), well
inside the 60 s the script waits for the kernel's load (RELEASE.md section 6).

On the RTL system (two cold boots, each a new surfsim run, and a one-minute soak), PASS in about
3 minutes:

```
  ok       the bitstream's build id 2355dba2 is the manifest's (2355dba2, .../manifest-sim.txt)
  ok       its dirty flag (0) matches the manifest's source_dirty = false
  ok       SurfOS 1.0.0 on the card is .../SURFOS.BIN (build 2355dba2, 376596 bytes, payload CRC 0x9794518d)
  ok       bootinfo: the kernel reads the bitstream's identity (sysinfo: board 0x00f0, SoC 0x000100, map 1, sys clock 25000000 Hz, bitstream 2355dba2 (simulation))
  ok       cold boot 1: SurfBoot, the kernel and the shell; up 1.19 s, a fresh boot; bitstream 2355dba2
  ok       cold boot 2: SurfBoot, the kernel and the shell; up 0.29 s, a fresh boot; bitstream 2355dba2
  ok       2 cold boots in a row, each one checked, all from the bitstream 2355dba2
  ok       the soak: /sd0p1 is mounted read-write (sd=rw)
  ok       the soak: 3 rounds of uptime, ps, tasks, write, cp, crc32, rm and memstat in 1.4 minutes, every CRC right, the heap checked every round, no panic and no killed task
  note     the soak: 8 tasks at each uptime
  note     the soak: the kernel heap's use went from 144304 to 144304 bytes (between 144304 and 144304)
```

## 4. The evidence file

`docs/evidence/physical/<M>-<UTC date>.txt` (`-2`, `-3` ... for more runs that day):

* `Verdict:` PASS when every check the script made passed and you answered no question with
  no; FAIL when a check failed or you answered no; INCOMPLETE when nothing failed but nothing
  could be checked; INTERRUPTED after Ctrl-C. PASS covers only what the script saw.
* `Source:` the board and its port, or SIMULATION when the SoC reports BUILD_FLAGS.SIM or the
  simulation board id (such a run goes to `build/board-test/` unless `--evidence-dir` says
  otherwise). `Owner:` appears when the answers came from a program (`--owner`), not a person.
* `Date`, the `Command`, and the `Tools` (each tool's SHA-256 and the commit).
* The build identity read from the port and what it was compared with.
* The steps you were asked to do, each with its time and your answer.
* The checks: `ok` and `FAIL` (seen by the script), `owner` and `owner-no` (your answers about
  what only you can see), `skip` (not done, not observed or not compared, with the reason),
  `note`.
* The transcript: every line with the seconds since the port was opened, escape sequences and
  CR removed, bytes that are not text shown as `�`; `*` lines mark the port going away and
  coming back.

## 5. uart.py

```
tools/uart.py list
tools/uart.py console PORT [--log FILE] [--dtr on|off] [--rts on|off]
tools/uart.py capture PORT --until REGEX [--timeout S] --out FILE
tools/uart.py script PORT FILE [--timeout S] [--log FILE]
```

PORT may be `auto` (the only Arty UART plugged in). The port is set to 115200 8N1, raw: no echo,
no line editing, no CR or LF translation, no flow control (the Arty's UART has no RTS/CTS),
CLOCAL (the modem lines are ignored) and not HUPCL (closing does not drop DTR). What arrived
before the port was opened is thrown away (`--no-flush` keeps it). Typed bytes go out one at a
time 2 ms apart (`--pace`), about 500 characters a second against the line's 11,520, so a ROM
or kernel that polls its 64-byte receive FIFO between other work loses nothing; a line ends with
CR, as a terminal's Enter. `console` is a terminal (Ctrl-] quits; `--log` appends what arrives).
`capture` records lines with their times until REGEX matches: exit 0, 2 on the time limit, 1 if
the port goes away. `script` runs a file of steps written as surfsim's options: `--expect REGEX`
(in order, each after the last match), `--send REGEX=TEXT` (once REGEX matched, in their own
order; TEXT takes `\r \n \t \e \b \\ \xHH`), `--fail-on REGEX`, `--timeout S`, `#` comments.
Exit 0 when every expect matched, 1 on a fail-on or a port that went away, 2 when the time ran
out.

## 6. How the kit was proven, and what only the board can show

`make -C surfcpu test-os-kit` (on `make check`, about a minute) and `make -C surfcpu
test-os-kit-rtl` (about 3 minutes, in `evidence-os`) run `sim/soc/tools/kit_test.py`: the
procedures exactly as the owner runs them, with a program answering for the owner. Results in
`docs/evidence/os/test-os-kit.txt` and `test-os-kit-rtl.txt`, the RTL runs' whole evidence files
(transcripts included) in `docs/evidence/os/kit/`, and the fake boards run on the Mac with its
own Python 3.9 in `test-os-kit-mac.txt`. The excerpts above come from those files (build
2355dba2, a clean tree).

| Scenario | Against | Must give |
|---|---|---|
| m3, m5, m7, m8, m9 (2 boots, 1-minute soak) | the RTL system: surfsim `--uart-pty` with SurfBoot or bramdiag in the ROM, the DDR, card and keyboard models | PASS, labelled SIMULATION, every line read |
| m1 | a fake board (the baseline's 1 Hz line, with switches, a button, RESET and the receive pin) | PASS |
| a wrong build manifest | a fake board | FAIL: "do NOT match the manifest" |
| a lost second | a fake board | FAIL on the count |
| the port going away mid-run (USB unplugged) | a fake board; and surfsim ending in the middle of m5's DDR diagnostic | FAIL at once: "the port went away ... USB unplugged, or the board lost power?" |
| garbage on the line | a fake board sending random bytes | FAIL: "The bytes look garbled: the wrong port ... or the wrong baud rate?" |
| nothing arriving (a timeout) | a fake board that never prints | FAIL in 5 s: "Nothing at all arrived on the port" |
| a card without a kernel | the RTL system | FAIL at once with SurfBoot's line: `SurfBoot stopped: file not found (NOT_FOUND)` (then: `/BOOT/SURFOS.BIN does not exist`) |
| uart.py capture, script, console, list | fake boards | the exit codes above; a port that goes away ends them at once |

The owner's stand-in: a power cycle or RESET is surfsim ended by SIGTERM (`--sd-flush` keeps
what the card got) and a new run on the same image; the card taken out and put back is a run
whose card leaves at SurfBoot's banner (`--sd-remove`) and SIGUSR2; the Mac's side is mtools on
the card image while no surfsim runs; the keyboard is the io track's model. Things only a
person sees (the LEDs, the VGA monitor) are answered "cannot be observed here" and listed as
not observed. The fake-board scenarios also run on the Mac itself (`board-test-selftest`, with
the Mac's Python 3.9 and its pseudo-terminals).

What only the board can show: everything physical. The FT2232's real port on macOS (its
driver, how fast it comes back after a power cycle, whether `program-volatile` makes it go
away), DTR and RTS on a real port, the timing checks that `--slow` turns off in simulation (a
line a second by the Mac's clock, the uptime keeping time), the bitstream's real identity
against its manifest (the RTL runs compare with a manifest made from surfsim's own build id),
the LEDs, the VGA monitor, a real PS/2 keyboard, a real card and the Mac's own reading of it,
cold boots from the flash, and an hour's soak.
