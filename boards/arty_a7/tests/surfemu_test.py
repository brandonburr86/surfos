#!/usr/bin/env python3
"""
SurfOS on the Arty: the kernel package booted by SurfBoot in the fw track's firmware emulator
File: surfemu_test.py Date: 10/15/26 (SurfCPU)

The fw track's firmware emulator (surfcpu/firmware/tests/surfemu.c) is an instruction-level
model of the SurfCPU SoC: the core of ISA.md, the memory map of MEMORY-MAP.md and its
peripherals (uart at its baud rate, timer, intc, gpio, spi with a simulated SD card, ps2,
video, sysinfo). Here it runs the real boot ROM, SurfBoot, which loads the Arty kernel
package (build/riscv32/arty_a7/SURFOS.BIN) from a card image made by tools/mkcard.py (the
standard directories, /BOOT/CMDLINE.TXT, a file in /USERS/DOCUMENTS) and hands over to it.
The harness then types on the emulated UART: bootinfo, ls and cat in /USERS/DOCUMENTS, the
package read back from the card by CRC, tasks, drivers, the self test; at the end the text
screen (surf_video's text RAM, which the console draws on) is checked as well.

A test aid, as the emulator says of itself: not the RTL and not the board. Its card is read
only and has no CMD18 or CMD25, so the kernel's SD driver reads one block per command there.

  surfemu_test.py --surfemu build/fw/host/surfemu --rom build/fw/surfboot.mem --mkcard tools/mkcard.py
                  --package ../build/riscv32/arty_a7/SURFOS.BIN [--evidence FILE] [--transcript FILE]

Exit status 0 when every check passed.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import zlib

ANSI = re.compile(r'\x1b\[[0-9;?]*[A-Za-z]')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--surfemu', required=True)
    ap.add_argument('--rom', required=True)
    ap.add_argument('--mkcard', required=True)
    ap.add_argument('--package', required=True)
    ap.add_argument('--evidence')
    ap.add_argument('--transcript')
    a = ap.parse_args()

    results = []

    def check(ok, what):
        results.append((bool(ok), what))
        print('%s %s' % ('ok  ' if ok else 'FAIL', what), flush=True)
        return bool(ok)

    t0 = time.time()
    work = tempfile.mkdtemp(prefix='surfemu-os-', dir='/tmp' if os.path.isdir('/tmp') else None)
    out = ''
    try:
        with open(a.package, 'rb') as f:
            pkg = f.read()
        crcline = '%08x   %d  /sd0p1/BOOT/SURFOS.BIN' % (zlib.crc32(pkg) & 0xFFFFFFFF, len(pkg))
        cmdline, hello = os.path.join(work, 'CMDLINE.TXT'), os.path.join(work, 'HELLO.TXT')
        with open(cmdline, 'w') as f:
            f.write('ascii\n')
        with open(hello, 'w') as f:
            f.write('Hello from the card.\n')
        card, screen = os.path.join(work, 'card.img'), os.path.join(work, 'screen.txt')
        r = subprocess.run([sys.executable, a.mkcard, card, '--size', '64M', '--standard-dirs',
                            '--file', '/BOOT/SURFOS.BIN=' + a.package, '--file', '/BOOT/CMDLINE.TXT=' + cmdline,
                            '--file', '/USERS/DOCUMENTS/HELLO.TXT=' + hello], capture_output=True, text=True)
        if check(r.returncode == 0, 'card: mkcard.py makes the card (standard directories, /BOOT/SURFOS.BIN, /BOOT/CMDLINE.TXT, '
                 '/USERS/DOCUMENTS/HELLO.TXT)'):
            session = [                                     # (seen in the output, then typed, then expected)
                ('SURF ~', 'bootinfo', "command line: 'ascii'"),
                ("command line: 'ascii'", 'ls /sd0p1/USERS/DOCUMENTS', '1 entries'),
                ('1 entries', 'cat /sd0p1/USERS/DOCUMENTS/HELLO.TXT', 'Hello from the card.'),
                ('Hello from the card.', 'crc32 /sd0p1/BOOT/SURFOS.BIN', crcline),
                (crcline, 'tasks', 'task A ran 4 rounds, task B 3'),
                ('task A ran 4 rounds', 'drivers', 'sdcard    sd0'),
                ('sdcard    sd0', 'selftest', 'SELFTEST PASS'),
                ('SELFTEST PASS', 'uptime', 'ticks at 100 Hz'),    # (and the screen catches up meanwhile)
            ]
            argv = [a.surfemu, '--rom', a.rom, '--card', card, '--screen', screen, '--max-ms', '240000',
                    '--fail-on', 'Kernel panic', '--fail-on', 'BOOT PROBLEM', '--fail-on', 'Wipeout',
                    '--expect', 'boot: starting the kernel', '--expect', "Brandon Burr's SurfOS"]
            for seen, typed, want in session:
                argv += ['--send', '%s=%s\\r' % (seen, typed), '--expect', want]
            r = subprocess.run(argv, capture_output=True, text=True, errors='replace', timeout=900)
            out = ANSI.sub('', (r.stdout + r.stderr).replace('\r', ''))
            last = out.strip().splitlines()[-1] if out.strip() else ''
            check(r.returncode == 0, 'the emulator saw every expected line, in order (%s)' % last.replace('surfemu: ', ''))
            check(re.search(r'boot: SurfOS 1\.0\.0 build [0-9a-f]{8}: %d bytes' % len(pkg), out) and 'matches (card and DDR)' in out,
                  'SurfBoot loads /BOOT/SURFOS.BIN from the card and checks it')
            check('boot: starting the kernel at 0x80000000 (a0 = 0, a1 = 0x0001f000, a2 = 0x49465253)' in out,
                  'SurfBoot hands over: entry 0x80000000, a0 = 0, a1 = 0x0001f000, a2 = the magic')
            check('SurfOS: serial console on the arty_a7 UART' in out and
                  'Version 1.0 on a RISC-V (SurfCPU on the Arty A7), 2004-2026' in out and 'SURF ~#>' in out,
                  'the kernel boots to the shell, its console on surf_uart (ascii, from /BOOT/CMDLINE.TXT)')
            check(re.search(r'boot information: SurfBoot \S+ \([0-9a-f]{8}\), version 1, CRC 0x[0-9a-f]{8} ok', out) and
                  'board: arty_a7, handoff a0 0, a1 0x0001f000, a2 0x49465253' in out and "command line: 'ascii'" in out,
                  'bootinfo: SurfBoot\'s block taken (its CRC checked), the handoff registers, the command line')
            check(re.search(r'\+SD: sd0: SDHC \d+ MB', out) and '+VFS: sd0p1 mounted on /sd0p1 (fat, read-only)' in out,
                  'the card through surf_spi: sd0, its FAT32 partition mounted on /sd0p1 (read-only)')
            check(re.search(r'HELLO\.TXT\s*\n\s*1 entries', out) and 'Hello from the card.' in out,
                  '/sd0p1/USERS/DOCUMENTS found by its long name (alias DOCUME~1), HELLO.TXT read')
            check(crcline in out, 'the package read back from the card through the SD driver has the file\'s CRC')
            check('task A ran 4 rounds, task B 3' in out, 'tasks: two kernel tasks count, sleep and wake on the machine timer')
            check(all(re.search(r'^\s+%s\s+ok\s*$' % d, out, re.M) for d in ('serial', 'gpio', 'ps2kbd', 'sdcard')) and
                  re.search(r'irq\s+surf_intc at 0x[0-9a-f]{8}', out),
                  'drivers: serial, gpio, ps2kbd and sdcard started; surf_intc and the timer listed')
            check('SELFTEST PASS' in out, 'selftest: SELFTEST PASS on the emulated SoC')
            check(re.search(r'kernel heap 0x8[0-9a-f]{7}-0x8effffff', out),
                  'the kernel heap ends 16 MiB below the 256 MiB SurfBoot reports (0x8F000000, as before)')
            check(re.search(r'up [\d.]+ s \(\d+ ticks at 100 Hz', out), 'uptime: the tick counts at 100 Hz')
            check('Kernel panic' not in out and 'Wipeout' not in out and 'killed by' not in out and 'BOOT PROBLEM' not in out,
                  'no panic, no killed task, no boot problem')
            text = open(screen, errors='replace').read() if os.path.exists(screen) else ''
            m = re.search(r'scroll (\d+)', text)
            check('SELFTEST PASS' in text and 'SURF ~#>' in text and m and int(m.group(1)) > 0,
                  'the text screen: surf_video\'s text RAM shows the console (the self test\'s end, the prompt), '
                  'hardware scroll in use')
            if not results[-1][0] and text:
                print(text)
    finally:
        shutil.rmtree(work, ignore_errors=True)

    passed = sum(1 for ok, _ in results if ok)
    summary = ('test-os-surfemu arty_a7 %s: %d of %d checks passed in %.0f s (the fw track\'s firmware emulator: '
               'not the RTL, not the board)' % (os.path.basename(a.package), passed, len(results), time.time() - t0))
    print(summary)
    if a.transcript:
        with open(a.transcript, 'w') as f:
            f.write(out)
    if a.evidence:
        os.makedirs(os.path.dirname(a.evidence) or '.', exist_ok=True)
        with open(a.evidence, 'w') as f:
            f.write('# %s\n' % summary)
            f.write('# %s\n' % time.strftime('%Y-%m-%d %H:%M:%S UTC', time.gmtime()))
            for ok, what in results:
                f.write('%s %s\n' % ('ok  ' if ok else 'FAIL', what))
    sys.exit(0 if passed == len(results) else 1)


if __name__ == '__main__':
    main()
