#!/usr/bin/env python3
"""
The whole SurfCPU system in surfsim: SurfBoot, the Arty kernel, the card and the console
File: system_boot_test.py Date: 10/15/26 (SurfCPU)

make test-soc-system-boot (docs/SOC.md section 9). build/sim/surfsim is the RTL of surfcpu_soc
(HAS_DDR=1, the real core and peripherals) with the C++ models at its pins: the DDR model, the
UART model, and the io track's SD card, PS/2 keyboard and VGA capture. It runs the real boot
ROM (build/fw/surfboot.mem), which loads the Arty kernel package (SURFOS.BIN, packed by
tools/surfimg.py) from a card image made by tools/mkcard.py. Two runs go side by side:

  boot      card: the standard directories, /BOOT/SURFOS.BIN, /BOOT/CMDLINE.TXT ("ascii"),
            /USERS/DOCUMENTS/HELLO.TXT and an 8 KiB DATA.BIN there. In order on the UART:
            SurfBoot finds the package and checks it (payload CRC good, card and DDR); the
            handoff; the kernel's boot messages and banner; the shell prompt. Then typed on
            the UART: bootinfo, ls and cat in /USERS/DOCUMENTS, ps (the kernel's tasks), tasks
            (two more kernel tasks that count, sleep and wake), crc32 of both files read back
            through the kernel's SD driver. Last, `drivers` typed on the PS/2 keyboard model:
            its output on the UART, and on the screen decoded from the VGA pixels.
  recovery  card: /BOOT/SURFOS.BIN with one payload byte flipped, and the good package as
            /BOOT/BACKUP.BIN. SurfBoot reports the bad CRC, tries BACKUP.BIN and starts it;
            the kernel boots to the prompt and its bootinfo says ", from BACKUP.BIN".

The expectations follow boards/arty_a7/tests/surfemu_test.py (the fw track's instruction-level
emulator) where they fit. Two of its steps are left out of these runs for time: crc32 of the
353 KiB package (23 s of simulated time on this SoC) and the kernel self test (about 80 s,
some 2 billion cycles); --selftest makes a third kind of run, boot and `selftest` alone
(make test-soc-system-selftest, not part of check).

  system_boot_test.py --surfsim build/sim/surfsim --rom build/fw/surfboot.mem
                      --mkcard tools/mkcard.py --package ../build/riscv32/arty_a7/SURFOS.BIN
                      [--work DIR] [--evidence FILE] [--keep] [--serial] [--seed N] [--selftest]

The card images (64 MiB each) are deleted at the end unless --keep; the transcripts (raw and
without escape sequences), the surfsim command lines and logs and the decoded screen stay in
--work. Exit status 0 when every check passed.
"""

import argparse
import os
import random
import re
import shutil
import subprocess
import sys
import time
import zlib

ANSI = re.compile(r'\x1b\[[0-9;?]*[A-Za-z]')
# the shell's prompt, colours and all: ESC[0;1;34mSURF ~ESC[1;33m#ESC[0;1;34m>ESC[0m and a space
PROMPT = r'SURF ~(?:\x1b\[[0-9;]*m)*#(?:\x1b\[[0-9;]*m)*>(?:\x1b\[[0-9;]*m)* '
NL = r'\r?\n'
FAIL_ON = ['Kernel panic', 'Kernel Wipeout', 'BOOT PROBLEM', 'killed by', 'SurfBoot stopped']
HANDOFF = 'at 0x80000000 (a0 = 0, a1 = 0x0001f000, a2 = 0x49465253)'
DOCS = '/sd0p1/USERS/DOCUMENTS'


def rx(text):
    """text as an ECMAScript regular expression (std::regex): only its special characters escaped"""
    return re.sub(r'([\\^$.|?*+()\[\]{}])', r'\\\1', text)


def soc_h(name, path='firmware/include/surfcpu/soc.h'):
    """a value of the generated header"""
    m = re.search(r'^#define %s (\d+)' % name, open(path).read(), re.M)
    return int(m.group(1))


def boot_expects(pkg, payload_crc, backup=False):
    """SurfBoot to the shell prompt, as surfemu_test.py expects it"""
    e = [('SurfBoot opens the package', r'boot: /BOOT/SURFOS\.BIN' + NL)]
    if backup:
        e += [('SurfBoot finds the damage', r'boot: /BOOT/SURFOS\.BIN: the payload CRC as read from the card is '
                                            r'0x[0-9a-f]{8}, the header says 0x%08x[^\n]*\n' % payload_crc),
              ('SurfBoot tries the backup', r'boot: trying /BOOT/BACKUP\.BIN instead' + NL),
              ('SurfBoot opens the backup', r'boot: /BOOT/BACKUP\.BIN' + NL)]
    e += [('SurfBoot loads it', r'boot: SurfOS 1\.0\.0 build [0-9a-f]{8}: %d bytes in \d+ fragments?, read in \d+ ms'
           % len(pkg) + NL),
          ('SurfBoot checks it', r'payload CRC 0x%08x matches \(card and DDR\)' % payload_crc + NL),
          ('the command line', r'boot: command line "ascii"' + NL),
          ('the handoff', rx('boot: starting %s %s' % ('BACKUP.BIN' if backup else 'the kernel', HANDOFF)) + NL),
          ('the kernel on the serial console', r'SurfOS: serial console on the arty_a7 UART \(115200 8N1\)' + NL),
          ('the card mounted', r'\+VFS: sd0p1 mounted on /sd0p1 \(fat, read-only\)' + NL),
          ('the banner', r"Brandon Burr's SurfOS" + NL + r'Version 1\.0 on a RISC-V \(SurfCPU on the Arty A7\), '
           r'2004-2026' + NL),
          ('the shell prompt', PROMPT)]
    return e


def run_args(a, card, expects, sends, ps2_sends, max_cycles, extra):
    argv = [a.surfsim, '--rom', a.rom, '--sd', card, '--seed', str(a.seed), '--max-cycles', str(max_cycles)]
    for f in FAIL_ON:
        argv += ['--fail-on', f]
    for _, pattern in expects:
        argv += ['--expect', pattern]
    for pattern, text in sends:
        argv += ['--send', '%s=%s' % (pattern, text)]
    for pattern, text in ps2_sends:
        argv += ['--ps2-send', '%s=%s' % (pattern, text)]
    return argv + extra


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--surfsim', required=True)
    ap.add_argument('--rom', required=True)
    ap.add_argument('--mkcard', required=True)
    ap.add_argument('--package', required=True)
    ap.add_argument('--work', default='build/soc/system_boot')
    ap.add_argument('--evidence')
    ap.add_argument('--keep', action='store_true', help='keep the card images')
    ap.add_argument('--serial', action='store_true', help='one run after the other')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--selftest', action='store_true', help='boot and run the kernel self test instead')
    a = ap.parse_args()

    t0 = time.time()
    results = []

    def check(ok, what):
        results.append((bool(ok), what))
        print('%s %s' % ('ok  ' if ok else 'FAIL', what), flush=True)
        return bool(ok)

    w = a.work
    os.makedirs(w, exist_ok=True)
    for old in os.listdir(w):
        p = os.path.join(w, old)
        shutil.rmtree(p) if os.path.isdir(p) else os.remove(p)
    pkg = open(a.package, 'rb').read()
    payload_crc = zlib.crc32(pkg[64:]) & 0xFFFFFFFF
    sys_hz = soc_h('SURF_SYS_HZ')

    # ---------------------------------------------------------------- the cards
    files = {'CMDLINE.TXT': b'ascii\n', 'HELLO.TXT': b'Hello from the card.\n',
             'DATA.BIN': bytes(random.Random(2026).getrandbits(8) for _ in range(8192))}
    for name, data in files.items():
        with open(os.path.join(w, name), 'wb') as f:
            f.write(data)
    if not a.selftest:
        damaged = bytearray(pkg)
        damaged[64 + (len(pkg) - 64) // 2] ^= 0xFF      # one payload byte: the header stays good
        with open(os.path.join(w, 'SURFOS.BAD'), 'wb') as f:
            f.write(damaged)
    common = ['--size', '64M', '--standard-dirs', '--file', '/BOOT/CMDLINE.TXT=' + os.path.join(w, 'CMDLINE.TXT'),
              '--file', '/USERS/DOCUMENTS/HELLO.TXT=' + os.path.join(w, 'HELLO.TXT'),
              '--file', '/USERS/DOCUMENTS/DATA.BIN=' + os.path.join(w, 'DATA.BIN')]
    cards = {'boot': ['--file', '/BOOT/SURFOS.BIN=' + a.package],
             'recovery': ['--file', '/BOOT/SURFOS.BIN=' + os.path.join(w, 'SURFOS.BAD'),
                          '--file', '/BOOT/BACKUP.BIN=' + a.package]}
    if a.selftest:
        del cards['recovery']
    made = []
    for name, extra in cards.items():
        r = subprocess.run([sys.executable, a.mkcard, os.path.join(w, 'card-%s.img' % name)] + extra + common,
                           capture_output=True, text=True)
        made.append(r)
    if not check(all(r.returncode == 0 for r in made),
                 'cards: mkcard.py makes them (standard directories, /BOOT/CMDLINE.TXT, HELLO.TXT and DATA.BIN in '
                 '/USERS/DOCUMENTS; SURFOS.BIN%s)' % ('' if a.selftest else ', or a damaged SURFOS.BIN and BACKUP.BIN')):
        print(''.join(r.stdout + r.stderr for r in made))
        sys.exit(1)

    # ---------------------------------------------------------------- the runs
    # as the shell prints them: "    %08lx %8lu  %s"
    hello_crc = '%08x %8d  %s/HELLO.TXT' % (zlib.crc32(files['HELLO.TXT']), len(files['HELLO.TXT']), DOCS)
    data_crc = '%08x %8d  %s/DATA.BIN' % (zlib.crc32(files['DATA.BIN']), len(files['DATA.BIN']), DOCS)
    docs_bytes = len(files['HELLO.TXT']) + len(files['DATA.BIN'])
    runs = {}
    if a.selftest:
        e = boot_expects(pkg, payload_crc) + [
            ('selftest', r'SELFTEST PASS[^\n]*\n'),
            ('prompt', PROMPT)]
        runs['selftest'] = dict(card='boot', expects=e, sends=[(PROMPT, 'selftest\\r')], ps2=[], max=4000000000,
                                extra=[])
    else:
        e = boot_expects(pkg, payload_crc) + [
            ('bootinfo', r"boot information: SurfBoot \S+ \([0-9a-f]{8}\), version 1, CRC 0x[0-9a-f]{8} ok" + NL),
            ('bootinfo: command line', r"command line: 'ascii'" + NL),
            ('prompt', PROMPT),
            ('ls', r'\n\s*2 entries, %d bytes' % docs_bytes + NL),
            ('prompt', PROMPT),
            ('cat', r'Hello from the card\.' + NL),
            ('prompt', PROMPT),
            ('ps', r'SurfOS Task List' + NL),
            ('ps: the count', r'\n\d+ tasks' + NL),
            ('prompt', PROMPT),
            ('tasks', r'task A ran 4 rounds, task B 3, in \d+ ticks; both ended \(status 0, 0\)' + NL),
            ('prompt', PROMPT),
            ('crc32', rx(hello_crc) + NL + r'\s*' + rx(data_crc) + NL),
            ('prompt', PROMPT),
            ('drivers (typed on PS/2)', r'keyboard  PS/2 on surf_ps2 \(set 2 translated to set 1\), \d+ keys, '
                                        r'a keyboard answered' + NL),
            ('prompt', PROMPT)]
        sends = [(PROMPT, c + '\\r') for c in ('bootinfo', 'ls ' + DOCS, 'cat %s/HELLO.TXT' % DOCS, 'ps', 'tasks',
                                               'crc32 %s/HELLO.TXT %s/DATA.BIN' % (DOCS, DOCS))]
        ps2 = [(rx(data_crc) + NL + r'[\s\S]*' + PROMPT, 'drivers\\n')]
        os.makedirs(os.path.join(w, 'vga'))
        runs['boot'] = dict(card='boot', expects=e, sends=sends, ps2=ps2, max=600000000,
                            extra=['--vga-dir', os.path.join(w, 'vga'), '--settle', '1000000'])
        e = boot_expects(pkg, payload_crc, backup=True) + [
            ('bootinfo: from the backup', r'image 0x80000000 \+ \d+ bytes; boot device SD, FAT32 at LBA 2048, '
                                          r'from BACKUP\.BIN' + NL),
            ('prompt', PROMPT)]
        runs['recovery'] = dict(card='recovery', expects=e, sends=[(PROMPT, 'bootinfo\\r')], ps2=[], max=600000000,
                                extra=[])

    procs = {}
    for name, r in runs.items():
        argv = run_args(a, os.path.join(w, 'card-%s.img' % r['card']), r['expects'], r['sends'], r['ps2'], r['max'],
                        r['extra'] + ['--transcript', os.path.join(w, name + '.raw')])
        with open(os.path.join(w, name + '.cmd'), 'w') as f:
            f.write(' '.join("'%s'" % x if re.search(r'[^\w./=+:-]', x) else x for x in argv) + '\n')
        out, err = open(os.path.join(w, name + '.out'), 'wb'), open(os.path.join(w, name + '.err'), 'wb')
        procs[name] = (subprocess.Popen(argv, stdout=out, stderr=err), out, err)
        if a.serial:
            procs[name][0].wait()
    for name, (p, out, err) in procs.items():
        r = runs[name]
        r['status'] = p.wait()
        out.close()
        err.close()
        rawp = os.path.join(w, name + '.raw')
        raw = open(rawp, 'rb').read().decode('utf-8', 'replace') if os.path.exists(rawp) else ''
        r['out'] = ANSI.sub('', raw).replace('\r', '')
        with open(os.path.join(w, name + '-transcript.txt'), 'w') as f:
            f.write(r['out'])
        r['err'] = open(os.path.join(w, name + '.err'), errors='replace').read()
        r['sum'] = re.search(r'surfsim: (.*) after (\d+) cycles \(([\d.]+) s simulated\) in ([\d.]+) s wall '
                             r'\(([\d.]+) M cycles/s\)', r['err'])
        r['events'] = {int(n): int(c) for c, n in re.findall(r'surfsim: cycle (\d+): --expect (\d+) of', r['err'])}

    def run_ok(name):
        r = runs[name]
        last = [l for l in r['err'].splitlines() if l.startswith('surfsim: ') and ' after ' in l]
        ok = check(r['status'] == 0 and r['sum'] and r['sum'].group(1) == 'output matched',
                   '%s: every expected line in order (%s)' % (name, last[-1][9:] if last else 'no summary'))
        if not ok:
            print(r['err'][-3000:])
        return r['out']

    def boot_checks(out, what):
        check(re.search(r'SurfBoot 1\.0\.0 \(build [0-9a-f]{8}[^)]*\), the SurfCPU boot ROM', out) and
              re.search(r'ddr: calibrated after \d+ ms, 256 MiB, quick test passed', out) and
              re.search(r'sd: SDHC/SDXC, 64 MiB; ready after \d+ ms; SPI \d+ kHz, data CRC checked', out) and
              'fat32: partition 1 (type 0x0c) at sector 2048' in out,
              '%s: SurfBoot from the boot ROM: the DDR model calibrated and the quick test passed, the card '
              'initialized through surf_spi, its FAT32 partition found' % what)
        check(re.search(r'boot: SurfOS 1\.0\.0 build [0-9a-f]{8}: %d bytes' % len(pkg), out) and
              ('payload CRC 0x%08x matches (card and DDR)' % payload_crc) in out,
              '%s: SurfBoot loads /BOOT/SURFOS.BIN over surf_spi and checks it: payload CRC 0x%08x (as zlib '
              'computes it here), as read from the card and from DDR' % (what, payload_crc))
        check(('boot: starting the kernel ' + HANDOFF) in out,
              '%s: SurfBoot hands over: entry 0x80000000, a0 = 0, a1 = 0x0001f000 (boot information), a2 = the magic'
              % what)
        check('SurfOS: serial console on the arty_a7 UART' in out and
              'Version 1.0 on a RISC-V (SurfCPU on the Arty A7), 2004-2026' in out and 'SURF ~#>' in out,
              '%s: the kernel boots to the shell, its console on surf_uart (ascii, from /BOOT/CMDLINE.TXT)' % what)
        check(re.search(r'\+SD: sd0: SDHC \d+ MB', out) and '+VFS: sd0p1 mounted on /sd0p1 (fat, read-only)' in out,
              '%s: the kernel\'s SD driver on surf_spi and the card model: sd0, its FAT32 partition on /sd0p1' % what)

    # ---------------------------------------------------------------- checks
    srows, head = [], ''
    if a.selftest:
        out = run_ok('selftest')
        boot_checks(out, 'selftest')
        check('SELFTEST PASS' in out and not re.search(r'^\s*FAIL', out, re.M),
              'selftest: the kernel self test passes on the RTL (%d ok, %d skipped)' %
              (len(re.findall(r'^\s+ok\s', out, re.M)), len(re.findall(r'^\s+skip\s', out, re.M))))
        check(not any(f in out for f in FAIL_ON), 'selftest: no panic, no killed task, no boot problem')
    else:
        out = run_ok('boot')
        boot_checks(out, 'boot')
        check(re.search(r'boot information: SurfBoot \S+ \([0-9a-f]{8}\), version 1, CRC 0x[0-9a-f]{8} ok', out) and
              'board: arty_a7, handoff a0 0, a1 0x0001f000, a2 0x49465253' in out and "command line: 'ascii'" in out,
              'bootinfo: SurfBoot\'s block taken (its CRC checked), the handoff registers, the command line')
        lsect = out.split('SURF ~#> ls ', 1)[1].split('SURF ~#>', 1)[0] if 'SURF ~#> ls ' in out else ''
        check(re.search(r'^\s*-\s+8192\s.*\sDATA\.BIN\s*$', lsect, re.M) and
              re.search(r'^\s*-\s+21\s.*\sHELLO\.TXT\s*$', lsect, re.M) and 'Hello from the card.' in out,
              'ls and cat: %s holds DATA.BIN and HELLO.TXT (%d bytes), HELLO.TXT read' % (DOCS, docs_bytes))
        sect = out.split('SurfOS Task List', 1)[1].split(' tasks\n', 1)[0] if 'SurfOS Task List' in out else ''
        rows = re.findall(r'^\s*(\d+)\s+(\S+)\s+(\S+)\s+(\d+)\s+(\d+)\s+(-?\d+)\s+(.+?)\s*$', sect, re.M)
        names = [x[6] for x in rows]
        check(len(rows) >= 2 and 'heartbeat' in names, 'ps: %d kernel tasks (%s)' % (len(rows), ', '.join(names)))
        check('task A ran 4 rounds, task B 3' in out,
              'tasks: two more kernel tasks count, sleep and wake on the machine timer, then end')
        check(hello_crc in out and data_crc in out,
              'crc32: both files read back through the kernel\'s SD driver have their CRCs (%s, %s)' %
              (hello_crc.split()[0], data_crc.split()[0]))
        check(all(re.search(r'^\s+%s\s+ok\s*$' % d, out, re.M) for d in ('serial', 'gpio', 'ps2kbd', 'sdcard')) and
              re.search(r'keyboard\s+PS/2 on surf_ps2 .*, \d+ keys, a keyboard answered', out),
              'drivers, typed on the PS/2 keyboard model (open-drain lines): serial, gpio, ps2kbd and sdcard ok; '
              'the keyboard answered the kernel and its keys were counted')
        check(not any(f in out for f in FAIL_ON), 'boot: no panic, no killed task, no boot problem')
        # the screen, decoded from the pixels of the last whole frame
        sp = os.path.join(w, 'vga', 'screen.txt')
        head, _, body = (open(sp, errors='replace').read() if os.path.exists(sp) else '').partition('\n')
        srows = body.split('\n')[:30]
        joined = ''.join(x.ljust(80)[:80] for x in srows)
        check(re.search(r'line 800 pixels \(sync 96 negative\), frame 525 lines \(sync 2 negative\), 59\.9\d+ Hz', head)
              and 'text 80x30' in head and ', 0 unmatched cells' in head,
              'VGA: 800x525 timing, negative syncs, about 59.9 Hz; every cell matches a glyph of the font ROM')
        check(any(re.match(r'SURF ~#> drivers\s*$', x) for x in srows) and
              re.search(r'keyboard\s+PS/2 on surf_ps2 \(set 2 translated to set 1\), \d+ keys, a keyboard answered',
                        joined) and re.search(r'ps2kbd\s+ok', joined),
              'VGA: the screen shows the console: the command typed on the keyboard and its output')
        mc = re.search(r'cursor at column (\d+) row (\d+)', head)
        check(mc and int(mc.group(2)) < len(srows) and srows[int(mc.group(2))].startswith('SURF ~#>'),
              'VGA: the cursor is on the new prompt')

        rout = run_ok('recovery')
        check(re.search(r'boot: /BOOT/SURFOS\.BIN: the payload CRC as read from the card is 0x[0-9a-f]{8}, '
                        r'the header says', rout) and 'boot: trying /BOOT/BACKUP.BIN instead' in rout and
              ('boot: starting BACKUP.BIN ' + HANDOFF) in rout,
              'recovery: SurfBoot refuses the damaged SURFOS.BIN (payload CRC), loads /BOOT/BACKUP.BIN and starts it')
        check(re.search(r'image 0x80000000 \+ \d+ bytes; boot device SD, FAT32 at LBA 2048, from BACKUP\.BIN', rout)
              and 'SURF ~#>' in rout, 'recovery: the kernel boots to the shell and bootinfo reports the backup')
        check(not any(f in rout for f in FAIL_ON), 'recovery: no panic, no boot problem')

    # ---------------------------------------------------------------- the record
    passed = sum(1 for ok, _ in results if ok)
    parts = ['%s %s cycles = %s s simulated in %s s wall (%s M cycles/s)' %
             (n, r['sum'].group(2), r['sum'].group(3), r['sum'].group(4), r['sum'].group(5))
             for n, r in runs.items() if r['sum']]
    target = 'test-soc-system-selftest' if a.selftest else 'test-soc-system-boot'
    line = ('%s: %d of %d checks passed in %.0f s (%s; the RTL of surfcpu_soc with the C++ models, not the board)'
            % (target, passed, len(results), time.time() - t0, '; '.join(parts)))
    print(line)
    if a.evidence:
        os.makedirs(os.path.dirname(a.evidence) or '.', exist_ok=True)
        with open(a.evidence, 'w') as f:
            f.write('# %s\n# %s\n' % (line, time.strftime('%Y-%m-%d %H:%M:%S UTC', time.gmtime())))
            for ok, what in results:
                f.write('%s %s\n' % ('ok  ' if ok else 'FAIL', what))
            for n, r in runs.items():
                f.write('\n%s run: the cycle (after reset, at %d Hz) when each expected line had arrived:\n' % (n, sys_hz))
                for i, (what, _) in enumerate(r['expects'], 1):
                    if what != 'prompt' and i in r['events']:
                        f.write('  %-36s %11d  (%.3f s)\n' % (what, r['events'][i], r['events'][i] / sys_hz))
                f.write('  models: %s\n' % ' '.join(l[9:] for l in r['err'].splitlines() if l.startswith('surfsim: card')))
            if srows:
                f.write('\nthe screen at the end of the boot run, decoded from the VGA pixels:\n%s\n' % head)
                f.write(''.join('  |%s|\n' % x.ljust(80)[:80] for x in srows))
    if not a.keep:
        for name in cards:
            p = os.path.join(w, 'card-%s.img' % name)
            if os.path.exists(p):
                os.remove(p)
    sys.exit(0 if passed == len(results) else 1)


if __name__ == '__main__':
    main()
