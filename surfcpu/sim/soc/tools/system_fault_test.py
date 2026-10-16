#!/usr/bin/env python3
"""
SurfOS on the RTL system: write faults, the card taken out, and a full volume (milestone 8)
File: system_fault_test.py Date: 10/16/26 (SurfCPU, the os track)

make test-os-faults (mk/area-os.mk). The whole SoC in build/sim/surfsim, as test-os-persist
runs it (system_persist_test.py's helpers, and system_boot_test.py's, are reused; neither file
is changed), with surfsim's card actions: --sd-fault arms the io track's card model's faults
(SdFaults), --sd-remove and --sd-insert take the card out of its socket and put it back, so
card detect reaches the kernel through surf_spi's STATUS.CARD, each at a point the test
chooses on the UART output. Two runs at once, each on a mkcard.py card with sd=rw:

  faults  1. one block answered "write error" (write_error=1): the console says so, the next
             sync (at the file's close) writes it again, and the write succeeds;
          2. every block refused (write_error=1000000): the write says "not all of it reached
             /sd0p1: I/O error" at once, eject says the volume stays mounted; the fault
             cleared (write_error=0), sync writes everything;
          3. the card taken out after that sync: the kernel's watch says so within its window
             (two looks 200 ms apart), sdinfo reads card detect as absent, a write is refused
             with an I/O error; umount; the card put back is sd0 again, and mount sd0p1 says
             "not unmounted cleanly"; the files of 1 and 2 read back by CRC;
          4. the card taken out right after the 2nd block of a new file's write (--sd-remove
             2@): the write fails, the watch says so.
          surfsim's --sd-flush keeps what the card got; on the host fsck.fat -n says what it
          finds, and mtools reads every earlier file back whole and the new one not at all.
  full    the card made nearly full on the host (mtools adds /FILLER.BIN so that 83
          clusters, 41.5 KiB, stay free): cp of a 64 KiB file says "No space left on device"
          when the space runs out and keeps what fitted (the source's first bytes, every free
          cluster), a write on the full volume fails the same way, rm gives the space back, a
          write works, eject; on the host fsck.fat -n and mtools check the card.

QEMU runs the same faults on its own card model (boards/qemu_virt/test_os.py), the host the
driver against the io track's model (boards/arty_a7/tests/iomodel); this is the RTL's turn:
the real core, surf_spi, its card-detect pin and the io track's card model.

  system_fault_test.py --surfsim build/sim/surfsim --rom build/fw/surfboot.mem
                       --mkcard tools/mkcard.py --package ../build/riscv32/arty_a7/SURFOS.BIN
                       [--work DIR] [--evidence FILE] [--keep] [--seed N] [--serial]

Exit status 0 when every check passed. The card images (36 MiB each) are deleted unless --keep.
"""

import argparse
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from system_boot_test import ANSI, FAIL_ON, NL, PROMPT, run_args, soc_h  # noqa: E402
from system_persist_test import boot_expects, crc_line  # noqa: E402

PART_OFFSET = 2048 * 512
CARD_SIZE = '36M'
FULL_FREE = 83                      # clusters left free on the full card: 10 copy chunks of 8, and 3 more
# fsck.fat -n findings that an interrupted write may leave with the block cache's write order (audit B3)
FSCK_HARMLESS = ('Dirty bit is set', 'Automatically removing dirty bit', 'FATs differ but appear to be intact',
                 'Using first FAT', 'Reclaimed ', 'Free cluster summary wrong', 'Auto-correcting')
FSCK_NOISE = ('fsck.fat ', 'Leaving filesystem unchanged', 'part.img: ')


def mtools(args):
    env = dict(os.environ, MTOOLS_SKIP_CHECK='1', LC_ALL='C.UTF-8', LANG='C.UTF-8')
    return subprocess.run(args, env=env, capture_output=True)


def fsck(img, w):
    """fsck.fat -n on the card's partition: (clean, harmless findings, other findings, the report)"""
    part = os.path.join(w, 'part.img')
    with open(img, 'rb') as f, open(part, 'wb') as p:
        f.seek(PART_OFFSET)
        p.write(f.read())
    r = subprocess.run(['fsck.fat', '-n', part], capture_output=True, text=True)
    os.remove(part)
    report = r.stdout + r.stderr
    harmless, other = [], []
    for line in report.splitlines():
        s = line.strip()
        if not s or any(s.startswith(n) or n in s for n in FSCK_NOISE):
            continue
        (harmless if any(s.startswith(h) for h in FSCK_HARMLESS) else other).append(s)
    return r.returncode == 0, harmless, other, report


def read_back(img, path, w):
    """a file of the card's partition through mtools, or None"""
    out = os.path.join(w, 'got.bin')
    r = mtools(['mcopy', '-n', '-i', img + '@@%d' % PART_OFFSET, '::' + path, out])
    if r.returncode:
        return None
    data = open(out, 'rb').read()
    os.remove(out)
    return data


def start(a, w, name, card, expects, steps, max_cycles, extra):
    """a surfsim run in the background: expects as test-os-persist has them, steps in order: ('send', REGEX, TEXT),
    ('fault', KIND, N, REGEX), ('remove', N, REGEX), ('insert', REGEX)"""
    argv = run_args(a, card, expects, [], [], max_cycles, ['--transcript', os.path.join(w, name + '.raw')] + extra)
    for s in steps:
        if s[0] == 'send':
            argv += ['--send', '%s=%s' % (s[1], s[2])]
        elif s[0] == 'fault':
            argv += ['--sd-fault', '%s=%d@%s' % (s[1], s[2], s[3])]
        elif s[0] == 'remove':
            argv += ['--sd-remove', ('%d@' % s[1] if s[1] else '') + s[2]]
        else:
            argv += ['--sd-insert', s[1]]
    with open(os.path.join(w, name + '.cmd'), 'w') as f:
        f.write(' '.join("'%s'" % x if re.search(r'[^\w./=+:-]', x) else x for x in argv) + '\n')
    out = open(os.path.join(w, name + '.out'), 'wb')
    err = open(os.path.join(w, name + '.err'), 'wb')
    return dict(name=name, proc=subprocess.Popen(argv, stdout=out, stderr=err), files=(out, err), expects=expects)


def finish(w, run):
    status = run['proc'].wait()
    for f in run['files']:
        f.close()
    name = run['name']
    raw = open(os.path.join(w, name + '.raw'), 'rb').read().decode('utf-8', 'replace') \
        if os.path.exists(os.path.join(w, name + '.raw')) else ''
    text = ANSI.sub('', raw).replace('\r', '')
    with open(os.path.join(w, name + '-transcript.txt'), 'w') as f:
        f.write(text)
    errs = open(os.path.join(w, name + '.err'), errors='replace').read()
    summary = re.search(r'surfsim: (.*) after (\d+) cycles \(([\d.]+) s simulated\) in ([\d.]+) s wall '
                        r'\(([\d.]+) M cycles/s\)', errs)
    events = {int(n): int(c) for c, n in re.findall(r'surfsim: cycle (\d+): --expect (\d+) of', errs)}
    return dict(run, status=status, out=text, err=errs, sum=summary, events=events)


def echo_end(out, command):
    """where a command's echo ends: after its prompt, or after other lines (the watch's words come after a
    prompt, and the echo after them); -1 when it is not there"""
    i = out.find('~#> ' + command)
    if i >= 0:
        return i + 4 + len(command)
    i = out.find('\n' + command + '\n')
    return i + 1 + len(command) if i >= 0 else -1


def after(out, command):
    """everything the run printed from a command's echo on"""
    i = echo_end(out, command)
    return out[i - len(command):] if i >= 0 else ''


def output_of(out, command):
    """what a command printed: from its echo to the next prompt, whose breadcrumb is left out"""
    i = echo_end(out, command)
    if i < 0:
        return ''
    rest = out[i:]
    j = rest.find('~#>')
    if j >= 0:
        rest = rest[:j]
        rest = rest[:rest.rfind('\n') + 1] if '\n' in rest else ''
    return rest


def cyc(r, name):
    """the cycle at which the named expected line had arrived, or None"""
    names = [n for n, _ in r['expects']]
    return r['events'].get(names.index(name) + 1) if name in names else None


def summary_line(r):
    last = [l for l in r['err'].splitlines() if l.startswith('surfsim: ') and ' after ' in l]
    return last[-1][9:] if last else 'no summary'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--surfsim', required=True)
    ap.add_argument('--rom', required=True)
    ap.add_argument('--mkcard', required=True)
    ap.add_argument('--package', required=True)
    ap.add_argument('--work', default='build/os/system_faults')
    ap.add_argument('--evidence')
    ap.add_argument('--keep', action='store_true', help='keep the card images')
    ap.add_argument('--serial', action='store_true', help='one run after the other')
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

    # ---------------------------------------------------------------- the two cards
    rng = random.Random(2026)
    data16 = bytes(rng.getrandbits(8) for _ in range(16 * 1024))
    data64 = bytes(rng.getrandbits(8) for _ in range(64 * 1024))
    hello = b'Hello from the card.\n'
    host = {'CMDLINE.TXT': b'ascii sd=rw\n', 'HELLO.TXT': hello, 'DATA16.BIN': data16, 'DATA64.BIN': data64}
    for name, body in host.items():
        with open(os.path.join(w, name), 'wb') as f:
            f.write(body)

    def make_card(path, data_name):
        argv = [sys.executable, a.mkcard, path, '--size', CARD_SIZE, '--standard-dirs',
                '--file', '/BOOT/SURFOS.BIN=' + a.package, '--file', '/BOOT/CMDLINE.TXT=' + os.path.join(w, 'CMDLINE.TXT'),
                '--file', '/USERS/DOCUMENTS/HELLO.TXT=' + os.path.join(w, 'HELLO.TXT'),
                '--file', '/USERS/DOCUMENTS/DATA.BIN=' + os.path.join(w, data_name)]
        return subprocess.run(argv, capture_output=True, text=True)

    fcard = os.path.join(w, 'faults.img')
    r = make_card(fcard, 'DATA16.BIN')
    if not check(r.returncode == 0, 'cards: mkcard.py makes the faults card (%s, /BOOT/CMDLINE.TXT "ascii sd=rw", '
                 'HELLO.TXT and a 16 KiB DATA.BIN in /USERS/DOCUMENTS)' % CARD_SIZE):
        print(r.stdout + r.stderr)
        sys.exit(1)
    ucard = os.path.join(w, 'full.img')
    r = make_card(ucard, 'DATA64.BIN')
    filler_size = 0
    if r.returncode == 0:
        with open(ucard, 'rb') as f:
            f.seek(PART_OFFSET)
            cluster = f.read(512)[13] * 512
            f.seek(PART_OFFSET + 512)
            free, = struct.unpack_from('<I', f.read(512), 488)
        filler_size = (free - FULL_FREE) * cluster
        filler = os.path.join(w, 'filler.bin')
        with open(filler, 'wb') as f:
            f.write(b'\x5a' * filler_size)
        r = mtools(['mcopy', '-i', ucard + '@@%d' % PART_OFFSET, filler, '::/FILLER.BIN'])
        os.remove(filler)
        with open(ucard, 'rb') as f:
            f.seek(PART_OFFSET + 512)
            left, = struct.unpack_from('<I', f.read(512), 488)
    if not check(r.returncode == 0 and left == FULL_FREE,
                 'cards: the full card: mkcard.py, a 64 KiB DATA.BIN, then mtools adds a %d-byte /FILLER.BIN: %d clusters '
                 'of %d bytes stay free (FSInfo)' % (filler_size, FULL_FREE, cluster)):
        sys.exit(1)

    keep_txt = 'written before the faults'
    once_txt = 'one block refused, then written again'
    stuck_txt = 'written while every block is refused'
    new_name = 'Notes-from-the-RTL.txt'
    files = {'/KEEP.TXT': (keep_txt + '\n').encode(), '/ONCE.TXT': (once_txt + '\n').encode(),
             '/STUCK.TXT': (stuck_txt + '\n').encode()}

    # ---------------------------------------------------------------- the faults run
    P = PROMPT
    crc_cmd = 'crc32 /sd0p1/KEEP.TXT /sd0p1/ONCE.TXT /sd0p1/STUCK.TXT'
    fe = boot_expects(pkg, payload_crc) + [
        ('KEEP.TXT written', P),
        ('1. a block refused', r'bcache: write of sd0p1 blocks \d+-\d+ failed \(-3\); kept for the next sync[^\n]*\n'),
        ('1. written again', r'bcache: sd0p1 takes writes again' + NL), ('1. prompt', P),
        ('2. refused', r'write: not all of it reached /sd0p1: I/O error' + NL), ('2. prompt', P),
        ('2. eject', r'eject: sd0: /sd0p1 could not be written: I/O error' + NL),
        ('2. stays mounted', r'eject: sd0 stays mounted[^\n]*\n'), ('2. eject prompt', P),
        ('2. works again', r'bcache: sd0p1 takes writes again' + NL), ('2. synced', r'synced; block cache: [^\n]*\n'),
        ('2. sync prompt', P),
        ('3. taken out', r'\+SD: sd0: the card was taken out' + NL),
        ('3. what is lost', r'\+SD: /sd0p1 was mounted read-write: what was not yet written to the card is lost; '
                            r'umount /sd0p1' + NL),
        ('3. sdinfo: card detect', r'card absent'), ('3. sdinfo', r'no card in the socket' + NL), ('3. sdinfo prompt', P),
        ('3. write refused', r'write: /sd0p1/AFTER\.TXT: I/O error' + NL), ('3. write prompt', P),
        ('3. umount', P),
        ('3. put back', r'\+SD: sd0: SDHC \d+ MB[^\n]*\n'),
        ('3. sd0 again', r'\+SD: sd0: a card was put in: mount sd0p1 to use it' + NL),
        ('3. not clean', r'\+FAT32: sd0p1 was not unmounted cleanly[^\n]*\n'),
        ('3. mounted', r'sd0p1 on /sd0p1: fat32[^\n]*\n'), ('3. mount prompt', P),
        ('3. crc32', re.escape(crc_line(files['/STUCK.TXT'], '/sd0p1/STUCK.TXT')) + NL), ('3. crc32 prompt', P),
        ('4. half written', r'write: not all of it reached /sd0p1[^\n]*\n'), ('4. prompt', P)]
    fsteps = [('send', P, 'write /sd0p1/KEEP.TXT %s\\r' % keep_txt),
              ('fault', 'write_error', 1, P), ('send', P, 'write /sd0p1/ONCE.TXT %s\\r' % once_txt),
              ('fault', 'write_error', 1000000, P), ('send', P, 'write /sd0p1/STUCK.TXT %s\\r' % stuck_txt),
              ('send', P, 'eject sd0\\r'),
              ('fault', 'write_error', 0, P), ('send', P, 'sync\\r'),
              ('remove', 0, P), ('send', r'/sd0p1 was mounted read-write: [^\n]*\n', 'sdinfo\\r'),
              ('send', P, 'write /sd0p1/AFTER.TXT written after it came out\\r'),
              ('send', P, 'umount /sd0p1\\r'),
              ('insert', P), ('send', r'a card was put in: mount sd0p1 to use it' + NL, 'mount sd0p1\\r'),
              ('send', P, crc_cmd + '\\r'),
              ('remove', 2, P), ('send', P, 'write /sd0p1/%s a new file, the card out in its middle\\r' % new_name)]
    settle = sys_hz * 6 // 10                   # 0.6 s after the last line: the watch's two looks and its words
    # ---------------------------------------------------------------- the full run
    copy_cmd = 'cp /sd0p1/USERS/DOCUMENTS/DATA.BIN /sd0p1/COPY.BIN'
    ue = boot_expects(pkg, payload_crc) + [
        ('df', r'sd0p1\s+fat32[^\n]*\n'), ('df prompt', P),
        ('cp: no space', r'cp: write: No space left on device' + NL), ('cp prompt', P),
        ('crc32', r'[0-9a-f]{8}\s+\d+\s+/sd0p1/COPY\.BIN' + NL), ('crc32 prompt', P),
        ('df full', r'sd0p1\s+fat32[^\n]*\n'), ('df full prompt', P),
        ('write: no space', r'write: No space left on device' + NL), ('write prompt', P),
        ('rm', P), ('write after the rm', P),
        ('df again', r'sd0p1\s+fat32[^\n]*\n'), ('df again prompt', P),
        ('eject: unmounted', r'/sd0p1 unmounted' + NL), ('eject: safe', r'sd0: safe to remove' + NL), ('eject prompt', P)]
    usteps = [('send', P, 'df\\r'), ('send', P, copy_cmd + '\\r'), ('send', P, 'crc32 /sd0p1/COPY.BIN\\r'),
              ('send', P, 'df\\r'), ('send', P, 'write /sd0p1/MORE.TXT one more file\\r'),
              ('send', P, 'rm /sd0p1/COPY.BIN\\r'), ('send', P, 'write /sd0p1/MORE.TXT written after the rm\\r'),
              ('send', P, 'df\\r'), ('send', P, 'eject sd0\\r')]

    runs = []
    fr = start(a, w, 'faults', fcard, fe, fsteps, 500000000, ['--sd-flush', '--settle', str(settle)])
    if a.serial:
        fr = finish(w, fr)
    ur = start(a, w, 'full', ucard, ue, usteps, 400000000, ['--sd-flush'])
    fr = fr if a.serial else finish(w, fr)
    ur = finish(w, ur)
    runs = [fr, ur]

    # ---------------------------------------------------------------- the faults run's checks
    out = fr['out']
    check(fr['status'] == 0 and fr['sum'] and fr['sum'].group(1) == 'output matched',
          'faults: every expected line in order (%s)' % summary_line(fr))
    if fr['status'] != 0:
        print(fr['err'][-3000:])
    once = output_of(out, 'write /sd0p1/ONCE.TXT')
    check('bcache: write of sd0p1 blocks' in once and 'takes writes again' in once and 'not all of it' not in once,
          'faults 1: one block answered "write error": "bcache: write of sd0p1 blocks ... failed (-3); kept for the '
          'next sync", then "takes writes again" at the close\'s sync; the write succeeds')
    stuck = output_of(out, 'write /sd0p1/STUCK.TXT')
    ej = output_of(out, 'eject sd0')
    m0, m1 = cyc(fr, '1. prompt'), cyc(fr, '2. prompt')
    check('write: not all of it reached /sd0p1: I/O error' in stuck and 'stays mounted' in ej,
          'faults 2: every block refused: "write: not all of it reached /sd0p1: I/O error" (its prompt %s after the one '
          'it was typed at), eject: "%s"' % ('%.0f ms' % ((m1 - m0) * 1000.0 / sys_hz) if m0 and m1 else '?',
                                              next((l.strip() for l in ej.splitlines() if 'stays mounted' in l), '?')))
    sync = output_of(out, 'sync')
    check('takes writes again' in sync and 'synced;' in sync,
          'faults 2: the fault cleared, sync writes what the cache kept: "bcache: sd0p1 takes writes again", "synced"')
    # 3: when the card came out (surfsim's log) and when the watch said so (the expect's cycle)
    out_at = [int(c) for c in re.findall(r'surfsim: cycle (\d+): --sd-remove: the card is out \(', fr['err'])]
    said = cyc(fr, '3. taken out')
    lat = (said - out_at[0]) * 1000.0 / sys_hz if out_at and said else None
    check(lat is not None and 0 < lat < 600,
          'faults 3: taken out after the sync (surfsim --sd-remove, card detect off on surf_spi): "+SD: sd0: the card '
          'was taken out" %s later, and that what was not yet written is lost' % ('%.0f ms' % lat if lat else '?'))
    info = output_of(out, 'sdinfo')
    check('card absent' in info and 'no card in the socket' in info,
          'faults 3: sdinfo reads surf_spi\'s STATUS.CARD: "%s"' % next((l.strip() for l in info.splitlines()
                                                                           if 'card absent' in l), '?'))
    check('write: /sd0p1/AFTER.TXT: I/O error' in output_of(out, 'write /sd0p1/AFTER.TXT'),
          'faults 3: a write afterwards is refused: "write: /sd0p1/AFTER.TXT: I/O error"')
    mount = after(out, 'umount /sd0p1')
    check(re.search(r'\+SD: sd0: SDHC \d+ MB', mount) and '+SD: sd0: a card was put in: mount sd0p1 to use it' in mount
          and '+FAT32: sd0p1 was not unmounted cleanly' in mount and 'sd0p1 on /sd0p1: fat32' in mount,
          'faults 3: umount; put back (--sd-insert): sd0 again, "a card was put in"; mount sd0p1: "+FAT32: sd0p1 was '
          'not unmounted cleanly", mounted')
    crcs = output_of(out, crc_cmd)
    check(all(crc_line(body, '/sd0p1' + p) in crcs for p, body in files.items()),
          'faults 3: KEEP.TXT, ONCE.TXT and STUCK.TXT read back through the card put back, by CRC')
    late = after(out, 'write /sd0p1/' + new_name)
    pulled = re.search(r'--sd-remove: the card is out, after block (\d+)', fr['err'])
    check('write: not all of it reached /sd0p1' in late and '+SD: sd0: the card was taken out' in late,
          'faults 4: the card out right after the 2nd block of %s\'s write (block %s the card took): "write: not all '
          'of it reached /sd0p1", and the watch says the card was taken out' % (new_name, pulled.group(1) if pulled else '?'))
    flushed = re.search(r'surfsim: (\d+) card blocks written back', fr['err'])
    check(flushed, 'faults: surfsim --sd-flush wrote %s card blocks back into the image' % (flushed.group(1) if flushed else 'no'))
    clean, harmless, other, report = fsck(fcard, w)
    check(not other, 'faults: fsck.fat -n on the card as it came out finds %s'
          % ('; '.join(harmless + other) if harmless or other else 'the volume clean'))
    if other:
        print(report)
    got = {p: read_back(fcard, p, w) for p in files}
    new = read_back(fcard, '/' + new_name, w)
    check(got == files and read_back(fcard, '/USERS/DOCUMENTS/HELLO.TXT', w) == hello and
          read_back(fcard, '/USERS/DOCUMENTS/DATA.BIN', w) == data16 and read_back(fcard, '/AFTER.TXT', w) is None and
          new in (None, b'a new file, the card out in its middle\n'),
          'faults: on the host (mtools) KEEP.TXT, ONCE.TXT, STUCK.TXT and the card\'s own files are whole, AFTER.TXT '
          'is not there, and %s is %s' % (new_name, 'not there' if new is None else 'whole'))
    check(not any(f in out for f in FAIL_ON), 'faults: no panic, no killed task')

    # ---------------------------------------------------------------- the full run's checks
    out = ur['out']
    check(ur['status'] == 0 and ur['sum'] and ur['sum'].group(1) == 'output matched',
          'full: every expected line in order (%s)' % summary_line(ur))
    if ur['status'] != 0:
        print(ur['err'][-3000:])
    c0, c1 = cyc(ur, 'df prompt'), cyc(ur, 'cp prompt')
    df0 = re.search(r'sd0p1\s+fat32\s+\S+ MB\s+\S+ \S+\s+(\S+ \S+)\s', output_of(out, 'df'))
    cp = output_of(out, copy_cmd)
    check('cp: write: No space left on device' in cp and 'bytes copied' not in cp,
          'full: %s free (df); cp of a 64 KiB file: "cp: write: No space left on device" when the space ran out, '
          'the prompt %s later' % (df0.group(1) if df0 else '?', '%.0f ms' % ((c1 - c0) * 1000.0 / sys_hz) if c0 and c1 else '?'))
    m = re.search(r'([0-9a-f]{8})\s+(\d+)\s+/sd0p1/COPY\.BIN', output_of(out, 'crc32 /sd0p1/COPY.BIN'))
    part = int(m.group(2)) if m else -1
    check(m and part == FULL_FREE * cluster and int(m.group(1), 16) == zlib.crc32(data64[:part]) & 0xFFFFFFFF,
          'full: the copy keeps what fitted: the first %d bytes of DATA.BIN, every free cluster (%d)' % (part, FULL_FREE))
    dfs = [re.search(r'sd0p1\s+fat32\s+\S+ MB\s+\S+ \S+\s+(\S+ \S+)\s', s) for s in out.split('~#> df')[1:]]
    wr = output_of(out, 'write /sd0p1/MORE.TXT one more file')
    check(len(dfs) == 3 and dfs[1] and dfs[1].group(1) == '0 KB' and 'write: No space left on device' in wr,
          'full: df says 0 KB free; a write on the full volume: "write: No space left on device"')
    check(len(dfs) == 3 and dfs[2] and dfs[2].group(1) != '0 KB' and
          not output_of(out, 'write /sd0p1/MORE.TXT written after the rm').strip(),
          'full: rm gives the space back (df: %s free), and a write works' % (dfs[2].group(1) if len(dfs) == 3 and dfs[2] else '?'))
    check('sd0: safe to remove' in output_of(out, 'eject sd0'), 'full: eject: "sd0: safe to remove"')
    clean, harmless, other, report = fsck(ucard, w)
    check(clean, 'full: fsck.fat -n finds %s' % ('the volume clean' if clean else '; '.join(harmless + other)))
    if not clean:
        print(report)
    filler = read_back(ucard, '/FILLER.BIN', w)
    check(read_back(ucard, '/USERS/DOCUMENTS/DATA.BIN', w) == data64 and read_back(ucard, '/MORE.TXT', w) ==
          b'written after the rm\n' and read_back(ucard, '/COPY.BIN', w) is None and filler is not None and
          len(filler) == filler_size and filler == b'\x5a' * filler_size,
          'full: on the host DATA.BIN and the %d-byte filler are whole, MORE.TXT holds what was written after the rm, '
          'COPY.BIN is gone' % filler_size)
    check(not any(f in out for f in FAIL_ON), 'full: no panic, no killed task')

    # ---------------------------------------------------------------- the end
    passed = sum(1 for ok, _ in results if ok)
    elapsed = time.time() - t0

    def what(r):
        s = r['sum']
        return '%s %s cycles = %s s simulated in %s s wall' % (r['name'], s.group(2), s.group(3), s.group(4)) if s else r['name']
    head = ('test-os-faults: %d of %d checks passed in %.0f s (%s; %s; the RTL of surfcpu_soc with the C++ models, '
            'not the board)' % (passed, len(results), elapsed, what(fr), what(ur)))
    print(head)
    if a.evidence:
        with open(a.evidence, 'w') as f:
            f.write('# %s\n# %s\n' % (head, time.strftime('%Y-%m-%d %H:%M:%S UTC', time.gmtime())))
            for ok, txt in results:
                f.write('%s %s\n' % ('ok  ' if ok else 'FAIL', txt))
            for r in runs:
                f.write('\n%s run: the cycle (after reset, at %d Hz) when each expected line had arrived:\n'
                        % (r['name'], sys_hz))
                for i, (n, _) in enumerate(r['expects']):
                    c = r['events'].get(i + 1)
                    f.write('  %-28s %12s%s\n' % (n, c if c is not None else '-',
                                                  '  (%.3f s)' % (c / sys_hz) if c is not None else ''))
                for l in r['err'].splitlines():
                    if '--sd-' in l or 'card actions' in l or l.startswith('surfsim: card '):
                        f.write('  %s\n' % l[9:])
            for r in runs:
                text = r['out']
                i = text.find('~#> ')
                f.write('\nthe %s run, from the first prompt (escape sequences removed):\n' % r['name'])
                for line in text[i - 5 if i >= 5 else 0:].splitlines():
                    f.write('  %s\n' % line)
    if not a.keep:
        for img in (fcard, ucard):
            if os.path.exists(img):
                os.remove(img)
    sys.exit(0 if passed == len(results) else 1)


if __name__ == '__main__':
    main()
