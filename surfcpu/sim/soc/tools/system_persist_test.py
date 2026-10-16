#!/usr/bin/env python3
"""
SurfOS on the RTL system: files written on the card persist across a reboot (milestone 8)
File: system_persist_test.py Date: 10/16/26 (SurfCPU, the os track)

make test-os-persist (mk/area-os.mk). The whole SoC in build/sim/surfsim, as the soc track's
system_boot_test.py runs it (its helpers are reused, the file is not changed): the real core
and peripherals with the DDR model and the io track's SD card, PS/2 and VGA models. The card
comes from tools/mkcard.py with /BOOT/CMDLINE.TXT "ascii sd=rw", so SurfBoot boots the Arty
kernel and the kernel mounts /sd0p1 read-write. Two runs, one after the other:

  write   typed on the UART, in /sd0p1/USERS/DOCUMENTS (cd: the lines stay shorter than the
          screen): mkdir a folder (a long name), write a small text file in it, cp a 160 KiB
          file into it (320 clusters of 512 bytes: entries in three FAT sectors and more),
          overwrite a file with shorter text, append a line to another, move one into the
          folder under a long name, delete one, list the folder, sync, cd / and eject sd0.
          Every command that changes the card is followed by the shell's own sync of each
          volume (arch/riscv32/kshell.c), and eject says the card is safe to remove.
          surfsim's --sd-flush then writes what the card model was given back into the image.
  reboot  the same image, booted again: SurfBoot, the kernel, the mount (no "not unmounted
          cleanly"), then every name and size listed and every file the write run made or
          changed read back by CRC and cat (DATA.BIN, which it only read, is left to the host:
          a 160 KiB read costs some 10 s of the run).

Then the image on the host: fsck.fat -n finds the volume clean, and mtools lists the folder
(long names and their 8.3 aliases) and gives back every file byte for byte.

Faults are not injected here: surfsim has no option to arm the card model's SdFaults or to
pull the card (set_present()) during a run. The os track's error tests run on QEMU instead
(boards/qemu_virt/test_os.py, with the same SD driver, block device and card watch, and a
card model with the same faults) and the driver against the io track's model on the host
(boards/arty_a7/tests/iomodel).

  system_persist_test.py --surfsim build/sim/surfsim --rom build/fw/surfboot.mem
                         --mkcard tools/mkcard.py --package ../build/riscv32/arty_a7/SURFOS.BIN
                         [--work DIR] [--evidence FILE] [--keep] [--seed N]

Exit status 0 when every check passed. The card image (64 MiB) is deleted unless --keep.
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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from system_boot_test import ANSI, FAIL_ON, HANDOFF, NL, PROMPT, run_args, rx, soc_h  # noqa: E402

# any of the shell's prompts: the breadcrumb of the current directory ("SURF", "sd0p1 > USERS > DOCUMENTS"), then
# ~#> with its colours (system_boot_test.py's PROMPT is the one at the root)
ANY_PROMPT = r'~(?:\x1b\[[0-9;]*m)*#(?:\x1b\[[0-9;]*m)*>(?:\x1b\[[0-9;]*m)* '
DOCS = '/sd0p1/USERS/DOCUMENTS'
NOTES = DOCS + '/Saved-notes'
PART_OFFSET = 2048 * 512
FAILS = FAIL_ON + ['not all of it reached', r': I/O error', r'Read-only file system', 'bcache: write of',
                   'the card was taken out']


def boot_expects(pkg, payload_crc):
    """SurfBoot to the shell prompt, the card mounted read-write"""
    return [('SurfBoot opens the package', r'boot: /BOOT/SURFOS\.BIN' + NL),
            ('SurfBoot loads it', r'boot: SurfOS 1\.0\.0 build [0-9a-f]{8}: %d bytes in \d+ fragments?, read in \d+ ms'
             % len(pkg) + NL),
            ('SurfBoot checks it', r'payload CRC 0x%08x matches \(card and DDR\)' % payload_crc + NL),
            ('the command line', r'boot: command line "ascii sd=rw"' + NL),
            ('the handoff', rx('boot: starting the kernel ' + HANDOFF) + NL),
            ('the card, read-write', r'\+SD: sd0: SDHC \d+ MB, [^\n]*; read-write [^\n]*\n'),
            ('the card mounted', r'\+VFS: sd0p1 mounted on /sd0p1 \(fat\)' + NL),
            ('the banner', r"Brandon Burr's SurfOS" + NL),
            ('the shell prompt', PROMPT)]


def crc_line(data, path):
    """as crc32 prints it: "    %08lx %8lu  %s" """
    return '%08x %8d  %s' % (zlib.crc32(data) & 0xFFFFFFFF, len(data), path)


def run(a, w, name, card, expects, sends, max_cycles, flush):
    argv = run_args(a, card, expects, sends, [], max_cycles,
                    (['--sd-flush'] if flush else []) + ['--transcript', os.path.join(w, name + '.raw')])
    for f in FAILS[len(FAIL_ON):]:
        argv += ['--fail-on', f]
    with open(os.path.join(w, name + '.cmd'), 'w') as f:
        f.write(' '.join("'%s'" % x if re.search(r'[^\w./=+:-]', x) else x for x in argv) + '\n')
    with open(os.path.join(w, name + '.out'), 'wb') as out, open(os.path.join(w, name + '.err'), 'wb') as err:
        status = subprocess.run(argv, stdout=out, stderr=err).returncode
    raw = open(os.path.join(w, name + '.raw'), 'rb').read().decode('utf-8', 'replace') \
        if os.path.exists(os.path.join(w, name + '.raw')) else ''
    text = ANSI.sub('', raw).replace('\r', '')
    with open(os.path.join(w, name + '-transcript.txt'), 'w') as f:
        f.write(text)
    errs = open(os.path.join(w, name + '.err'), errors='replace').read()
    summary = re.search(r'surfsim: (.*) after (\d+) cycles \(([\d.]+) s simulated\) in ([\d.]+) s wall '
                        r'\(([\d.]+) M cycles/s\)', errs)
    events = {int(n): int(c) for c, n in re.findall(r'surfsim: cycle (\d+): --expect (\d+) of', errs)}
    return dict(status=status, out=text, err=errs, sum=summary, events=events, expects=expects)


def section(out, command):
    """what a command printed: from its echo to the next prompt"""
    i = out.find('~#> ' + command)
    if i < 0:
        return ''
    rest = out[i + len('~#> ' + command):]
    j = rest.find('~#>')
    if j >= 0:
        rest = rest[:j]
        rest = rest[:rest.rfind('\n') + 1] if '\n' in rest else ''     # (the next prompt's breadcrumb goes)
    return rest


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--surfsim', required=True)
    ap.add_argument('--rom', required=True)
    ap.add_argument('--mkcard', required=True)
    ap.add_argument('--package', required=True)
    ap.add_argument('--work', default='build/os/system_persist')
    ap.add_argument('--evidence')
    ap.add_argument('--keep', action='store_true', help='keep the card image')
    ap.add_argument('--seed', type=int, default=1)
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

    # ---------------------------------------------------------------- the card
    data = bytes(random.Random(2026).getrandbits(8) for _ in range(160 * 1024))
    files = {'CMDLINE.TXT': b'ascii sd=rw\n', 'HELLO.TXT': b'Hello from the card.\n', 'DATA.BIN': data,
             'LOG.TXT': b'first line, from the Mac\n',
             'OLD.TXT': b'old contents, longer than what replaces them, written on the Mac\n',
             'RENAME.TXT': b'a file that moves into the new folder\n', 'DELETE.TXT': b'a file that goes\n'}
    for name, body in files.items():
        with open(os.path.join(w, name), 'wb') as f:
            f.write(body)
    card = os.path.join(w, 'card.img')
    argv = [sys.executable, a.mkcard, card, '--size', '64M', '--standard-dirs', '--file', '/BOOT/SURFOS.BIN=' + a.package,
            '--file', '/BOOT/CMDLINE.TXT=' + os.path.join(w, 'CMDLINE.TXT')]
    for name in ('HELLO.TXT', 'DATA.BIN', 'LOG.TXT', 'OLD.TXT', 'RENAME.TXT', 'DELETE.TXT'):
        argv += ['--file', '/USERS/DOCUMENTS/%s=%s' % (name, os.path.join(w, name))]
    r = subprocess.run(argv, capture_output=True, text=True)
    if not check(r.returncode == 0, 'card: mkcard.py makes it (/BOOT/CMDLINE.TXT "ascii sd=rw"; in /USERS/DOCUMENTS '
                 'HELLO.TXT, a 160 KiB DATA.BIN, LOG.TXT, OLD.TXT, RENAME.TXT and DELETE.TXT)'):
        print(r.stdout + r.stderr)
        sys.exit(1)

    # what the card must hold afterwards
    note = b'Written on the RTL by SurfOS.\n'
    want = {DOCS + '/HELLO.TXT': files['HELLO.TXT'], DOCS + '/DATA.BIN': data,
            DOCS + '/LOG.TXT': files['LOG.TXT'] + b'second line, appended by SurfOS\n',
            DOCS + '/OLD.TXT': b'new and shorter\n',
            NOTES + '/note.txt': note, NOTES + '/Big-copy.bin': data,
            NOTES + '/Renamed-file.txt': files['RENAME.TXT']}

    # ---------------------------------------------------------------- the write run
    cmds = ['cd ' + DOCS,
            'mkdir Saved-notes',
            'write Saved-notes/note.txt ' + note.decode().strip(),
            'cp DATA.BIN Saved-notes/Big-copy.bin',
            'write OLD.TXT new and shorter',
            'append LOG.TXT second line, appended by SurfOS',
            'mv RENAME.TXT Saved-notes/Renamed-file.txt',
            'rm DELETE.TXT',
            'ls Saved-notes',
            'sync',
            'cd /',
            'eject sd0']
    e = boot_expects(pkg, payload_crc) + [
        ('cd', ANY_PROMPT), ('mkdir', ANY_PROMPT), ('write', ANY_PROMPT),
        ('cp', r'\b%d bytes copied' % len(data) + NL), ('prompt', ANY_PROMPT),
        ('write over', ANY_PROMPT), ('append', ANY_PROMPT), ('mv', ANY_PROMPT), ('rm', ANY_PROMPT),
        ('ls', r'\n\s*3 entries, %d bytes' % (len(note) + len(data) + len(files['RENAME.TXT'])) + NL), ('prompt', ANY_PROMPT),
        ('sync', r'synced; block cache: [^\n]*\n'), ('prompt', ANY_PROMPT),
        ('eject: unmounted', r'/sd0p1 unmounted' + NL),
        ('eject: safe', r'sd0: safe to remove' + NL), ('prompt', ANY_PROMPT)]
    sends = [(ANY_PROMPT, c + '\\r') for c in cmds]
    wr = run(a, w, 'write', card, e, sends, 900000000, flush=True)
    last = [l for l in wr['err'].splitlines() if l.startswith('surfsim: ') and ' after ' in l]
    if not check(wr['status'] == 0 and wr['sum'] and wr['sum'].group(1) == 'output matched',
                 'write: every expected line in order (%s)' % (last[-1][9:] if last else 'no summary')):
        print(wr['err'][-3000:])
    out = wr['out']
    flushed = re.search(r'surfsim: (\d+) card blocks written back', wr['err'])
    check('SURF ~#>' in out and '+VFS: sd0p1 mounted on /sd0p1 (fat)\n' in out and 'read-write' in out,
          'write: SurfBoot boots the kernel with sd=rw from /BOOT/CMDLINE.TXT; /sd0p1 mounted read-write')
    quiet = all(not section(out, c).strip() for c in cmds[:3] + cmds[4:8])
    check(quiet and '%d bytes copied' % len(data) in section(out, cmds[3]),
          'write: mkdir, write, cp (%d bytes), write over, append, mv, rm: each done without a word of error, '
          'each followed by the shell\'s sync' % len(data))
    ls = section(out, cmds[8])
    check(re.search(r'^\s*-\s+%d\s.*\snote\.txt\s*$' % len(note), ls, re.M) and
          re.search(r'^\s*-\s+%d\s.*\sBig-copy\.bin\s*$' % len(data), ls, re.M) and
          re.search(r'^\s*-\s+%d\s.*\sRenamed-file\.txt\s*$' % len(files['RENAME.TXT']), ls, re.M),
          'write: ls %s: note.txt, Big-copy.bin and Renamed-file.txt with their sizes' % NOTES)
    ej = section(out, cmds[11])
    check(re.search(r'synced; block cache: \d+ hits, \d+ misses, \d+ blocks written', section(out, cmds[9])) and
          '/sd0p1 unmounted' in ej and 'sd0: safe to remove' in ej,
          'write: sync, then eject sd0: "/sd0p1 unmounted", "sd0: safe to remove"')
    check(flushed and int(flushed.group(1)) > 0, 'write: surfsim --sd-flush wrote %s card blocks back into the image'
          % (flushed.group(1) if flushed else 'no'))
    check(not any(re.search(f, out) for f in FAILS), 'write: no panic, no killed task, no I/O or read-only error')

    # ---------------------------------------------------------------- the reboot run
    # in the folder the shell prints relative names: (the name crc32 prints, the full path)
    top = [(n, DOCS + '/' + n) for n in ('HELLO.TXT', 'LOG.TXT', 'OLD.TXT')]
    sub = [(n, NOTES + '/' + n) for n in ('note.txt', 'Big-copy.bin', 'Renamed-file.txt')]
    texts = [DOCS + '/LOG.TXT', DOCS + '/OLD.TXT', NOTES + '/note.txt', NOTES + '/Renamed-file.txt']
    cmds2 = ['cd ' + DOCS, 'ls', 'crc32 ' + ' '.join(n for n, _ in top), 'cat LOG.TXT OLD.TXT', 'cd Saved-notes',
             'ls', 'crc32 ' + ' '.join(n for n, _ in sub), 'cat note.txt Renamed-file.txt']
    e2 = boot_expects(pkg, payload_crc) + [
        ('cd', ANY_PROMPT),
        ('ls', r'\n\s*5 entries, \d+ bytes' + NL), ('prompt', ANY_PROMPT),
        ('crc32', rx(crc_line(want[top[-1][1]], top[-1][0])) + NL), ('prompt', ANY_PROMPT),
        ('cat', r'new and shorter' + NL), ('prompt', ANY_PROMPT),
        ('cd the folder', ANY_PROMPT),
        ('ls the folder', r'\n\s*3 entries, \d+ bytes' + NL), ('prompt', ANY_PROMPT),
        ('crc32 the folder', rx(crc_line(want[sub[-1][1]], sub[-1][0])) + NL), ('prompt', ANY_PROMPT),
        ('cat the folder', r'a file that moves into the new folder' + NL), ('prompt', ANY_PROMPT)]
    rb = run(a, w, 'reboot', card, e2, [(ANY_PROMPT, c + '\\r') for c in cmds2], 900000000, flush=False)
    last = [l for l in rb['err'].splitlines() if l.startswith('surfsim: ') and ' after ' in l]
    if not check(rb['status'] == 0 and rb['sum'] and rb['sum'].group(1) == 'output matched',
                 'reboot: every expected line in order (%s)' % (last[-1][9:] if last else 'no summary')):
        print(rb['err'][-3000:])
    out2 = rb['out']
    check('+VFS: sd0p1 mounted on /sd0p1 (fat)\n' in out2 and 'not unmounted cleanly' not in out2,
          'reboot: the card mounts again, clean (eject set the clean-shutdown bit)')
    l1 = section(out2, 'ls\n')
    l2 = out2.split('~#> cd Saved-notes', 1)[-1]
    l2 = section(l2, 'ls\n')
    names_ok = all(re.search(r'\s%s/?\s*$' % re.escape(n), l1, re.M) for n in
                   ('HELLO.TXT', 'DATA.BIN', 'LOG.TXT', 'OLD.TXT', 'Saved-notes')) and \
        'RENAME.TXT' not in l1 and 'DELETE.TXT' not in l1 and \
        all(re.search(r'^\s*-\s+%d\s.*\s%s\s*$' % (len(want[NOTES + '/' + n]), re.escape(n)), l2, re.M)
            for n in ('note.txt', 'Big-copy.bin', 'Renamed-file.txt'))
    check(names_ok, 'reboot: the names and sizes: Saved-notes/ with note.txt, Big-copy.bin, Renamed-file.txt; '
          'RENAME.TXT moved away, DELETE.TXT gone')
    crcs = section(out2, cmds2[2]) + section(out2, cmds2[6])
    check(all(crc_line(want[p], n) in crcs for n, p in top + sub),
          'reboot: every file read back through the SD driver has the CRC it was written with (%d files, the '
          '160 KiB copy among them)' % len(top + sub))
    cat = section(out2, cmds2[3]) + section(out2, cmds2[7])
    check(all(want[p].decode() in cat for p in texts),
          'reboot: cat: the appended line after the first, the shorter text in OLD.TXT, the note, the moved file')
    check(not any(re.search(f, out2) for f in FAILS), 'reboot: no panic, no killed task, no I/O error')

    # ---------------------------------------------------------------- the image on the host
    part = os.path.join(w, 'part.img')
    with open(card, 'rb') as f, open(part, 'wb') as p:
        f.seek(PART_OFFSET)
        shutil.copyfileobj(f, p)
    r = subprocess.run(['fsck.fat', '-n', part], capture_output=True, text=True)
    check(r.returncode == 0, 'host: fsck.fat -n finds the volume clean')
    if r.returncode:
        print(r.stdout + r.stderr)
    os.remove(part)
    env = dict(os.environ, MTOOLS_SKIP_CHECK='1', LC_ALL='C.UTF-8', LANG='C.UTF-8')
    img = '%s@@%d' % (card, PART_OFFSET)
    r = subprocess.run(['mdir', '-i', img, '::/USERS/DOCUMENTS/Saved-notes'], capture_output=True, text=True, env=env)
    check(r.returncode == 0 and 'Big-copy.bin' in r.stdout and 'Renamed-file.txt' in r.stdout and
          re.search(r'^note\s+txt\s', r.stdout, re.M | re.I),
          'host: mdir lists the folder: Big-copy.bin and Renamed-file.txt by their long names (8.3 aliases beside '
          'them), note.txt as a plain 8.3 name')
    if not results[-1][0]:
        print(r.stdout + r.stderr)
    same = True
    for p, body in want.items():
        dst = os.path.join(w, 'back.bin')
        r = subprocess.run(['mcopy', '-n', '-i', img, '::' + p[len('/sd0p1'):], dst], capture_output=True, env=env)
        got = open(dst, 'rb').read() if r.returncode == 0 and os.path.exists(dst) else None
        if got != body:
            same = False
            print('  %s: %s' % (p, 'not found' if got is None else '%d bytes, not as written' % len(got)))
        if os.path.exists(dst):
            os.remove(dst)
    r = subprocess.run(['mdir', '-i', img, '::/USERS/DOCUMENTS'], capture_output=True, text=True, env=env)
    check(same and r.returncode == 0 and 'RENAME' not in r.stdout and 'DELETE' not in r.stdout,
          'host: mcopy gives back all %d files byte for byte; RENAME.TXT and DELETE.TXT are not in /USERS/DOCUMENTS'
          % len(want))

    # ---------------------------------------------------------------- the record
    passed = sum(1 for ok, _ in results if ok)
    parts = ['%s %s cycles = %s s simulated in %s s wall' % (n, x['sum'].group(2), x['sum'].group(3), x['sum'].group(4))
             for n, x in (('write', wr), ('reboot', rb)) if x['sum']]
    line = ('test-os-persist: %d of %d checks passed in %.0f s (%s; the RTL of surfcpu_soc with the C++ models, '
            'not the board)' % (passed, len(results), time.time() - t0, '; '.join(parts)))
    print(line)
    if a.evidence:
        os.makedirs(os.path.dirname(a.evidence) or '.', exist_ok=True)
        with open(a.evidence, 'w') as f:
            f.write('# %s\n# %s\n' % (line, time.strftime('%Y-%m-%d %H:%M:%S UTC', time.gmtime())))
            for ok, what in results:
                f.write('%s %s\n' % ('ok  ' if ok else 'FAIL', what))
            for n, x in (('write', wr), ('reboot', rb)):
                f.write('\n%s run: the cycle (after reset, at %d Hz) when each expected line had arrived:\n' % (n, sys_hz))
                for i, (what, _) in enumerate(x['expects'], 1):
                    if what != 'prompt' and i in x['events']:
                        f.write('  %-30s %11d  (%.3f s)\n' % (what, x['events'][i], x['events'][i] / sys_hz))
                f.write('  models: %s\n' % ' '.join(l[9:] for l in x['err'].splitlines() if l.startswith('surfsim: card')))
            f.write('\nthe write run, from the prompt (escape sequences removed):\n')
            i = out.find('~#> ')
            f.write(''.join('  %s\n' % l for l in out[i:].splitlines()))
            f.write('\nthe reboot run, from the mount (escape sequences removed):\n')
            i = out2.find('+VFS: sd0p1')
            f.write(''.join('  %s\n' % l for l in out2[i:].splitlines()))
    if not a.keep and os.path.exists(card):
        os.remove(card)
    sys.exit(0 if passed == len(results) else 1)


if __name__ == '__main__':
    main()
