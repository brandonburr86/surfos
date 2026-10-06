#!/usr/bin/env python3
"""Partition tables between SurfOS and the Linux tools, both ways.

    python3 tests/test_part_tools.py build/O0/host/test_part      (make unittest runs it)

Disk images that SurfOS's own plans write (driver/parttab.c, through test_part --image) must
be accepted as they are by sfdisk (util-linux) and sgdisk (gdisk); images that sfdisk and
sgdisk write, logical partitions and a damaged GPT among them, must read back in SurfOS
(test_part --parse) exactly as the tools describe them. The tools are in the devenv image;
where they are missing the checks are skipped. Only image files in a temporary directory
are written.
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

BASIC = 'EBD0A0A2-B9E5-4433-87C0-68B6B72699C7'
LINUX = '0FC63DAF-8483-4772-8E79-3D69D8477DE4'
EFI = 'C12A7328-F81F-11D2-BA4B-00A0C93EC93B'
BLOCKS = 32768                                  # test_part's images: 16 MB

checks = failures = 0


def check(ok, what):
    global checks, failures
    checks += 1
    if not ok:
        failures += 1
        print('FAIL: ' + what)


def run(cmd, stdin_text=None):
    return subprocess.run(cmd, input=stdin_text, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)


def blank_image(path):
    with open(path, 'wb') as f:
        f.truncate(BLOCKS * 512)


def surfos_image(test_part, kind, path):
    with open(path, 'wb') as f:
        r = subprocess.run([test_part, '--image', kind], stdout=f, timeout=60)
    if r.returncode:
        raise RuntimeError(f'test_part --image {kind} failed')


def surfos_parse(test_part, path):
    """what SurfOS reads: scheme, disk fields, partitions by number, notes"""
    with open(path, 'rb') as f:
        out = subprocess.run([test_part, '--parse'], stdin=f, stdout=subprocess.PIPE, text=True, timeout=60).stdout
    t = {'scheme': None, 'disk': [], 'parts': {}, 'notes': []}
    for line in out.splitlines():
        word, _, rest = line.partition(' ')
        if word == 'scheme':
            t['scheme'] = rest
        elif word == 'disk':
            t['disk'] = rest.split()
        elif word == 'part':
            f = (rest.split(' ', 5) + [''] * 6)[:6]
            p = {'start': int(f[1]), 'size': int(f[2])}
            if t['scheme'] == 'gpt':
                p.update(type=f[3], uuid=f[4], name=f[5])
            else:
                p.update(type=f[3], bootable=f[4] == '1', extended=f[5] == '1')
            t['parts'][int(f[0])] = p
        elif word == 'note':
            t['notes'].append(rest)
    return t


def sfdisk(path):
    r = run(['sfdisk', '--json', path])
    if r.returncode:
        return {'label': None, 'partitions': [], 'error': r.stdout}
    return json.loads(r.stdout)['partitiontable']


def number(node):
    return int(re.search(r'(\d+)$', node).group(1))


def gpt_uuid_v4(u):
    return len(u) == 36 and u[14] == '4' and u[19] in '89AB'


def test_surfos_mbr(test_part, tmp):
    img = os.path.join(tmp, 'surfos-mbr.img')
    surfos_image(test_part, 'mbr', img)
    pt, ours = sfdisk(img), surfos_parse(test_part, img)
    check(pt['label'] == 'dos', 'sfdisk reads the MBR SurfOS wrote')
    check(ours['disk'] and pt.get('id') == ours['disk'][0] and pt.get('id') != '0x00000000', 'sfdisk sees the disk signature SurfOS chose')
    got = [(number(p['node']), p['start'], p['size'], p['type'], p.get('bootable', False)) for p in pt['partitions']]
    want = [(1, 2048, 8192, 'c', False), (2, 10240, 4096, '83', False), (3, 14336, BLOCKS - 14336, 'ef', False)]
    check(got == want, f'sfdisk lists the partitions SurfOS added: {got}')
    v = run(['sfdisk', '--verify', img])
    check(v.returncode == 0 and 'No errors detected' in v.stdout, 'sfdisk --verify accepts the MBR: ' + v.stdout.strip())


def test_surfos_gpt(test_part, tmp):
    img = os.path.join(tmp, 'surfos-gpt.img')
    surfos_image(test_part, 'gpt', img)
    pt, ours = sfdisk(img), surfos_parse(test_part, img)
    check(pt['label'] == 'gpt', 'sfdisk reads the GPT SurfOS wrote')
    check(ours['disk'] and pt.get('id') == ours['disk'][0] and gpt_uuid_v4(pt.get('id', '')), 'sfdisk sees the version 4 disk GUID')
    check(pt.get('firstlba') == 34 and pt.get('lastlba') == BLOCKS - 34, 'sfdisk: usable blocks 34 to the end - 34')
    got = [(number(p['node']), p['start'], p['size'], p['type'], p.get('name')) for p in pt['partitions']]
    want = [(1, 2048, 8192, BASIC, 'SurfOS data'), (2, 10240, 4096, LINUX, 'SurfOS data'),
            (3, 14336, BLOCKS - 34 - 14336 + 1, EFI, 'EFI system')]
    check(got == want, f'sfdisk lists the partitions SurfOS added, retyped and named: {got}')
    uuids = [p.get('uuid', '') for p in pt['partitions']]
    check(all(gpt_uuid_v4(u) for u in uuids) and len(set(uuids)) == 3, 'every partition has its own version 4 GUID')
    check(uuids == [ours['parts'][n]['uuid'] for n in (1, 2, 3)], 'SurfOS reads back the GUIDs sfdisk sees')
    v = run(['sgdisk', '-v', img])
    check(v.returncode == 0 and 'No problems found' in v.stdout, 'sgdisk -v finds no problems: ' + v.stdout.strip()[-300:])
    p = run(['sgdisk', '-p', img])
    check(p.returncode == 0 and re.search(r'\n\s+1\s+2048\s+10239\s.*SurfOS data', p.stdout) is not None and
          re.search(r'\n\s+3\s+14336\s+32734\s.*EF00\s+EFI system', p.stdout) is not None, 'sgdisk -p lists them: ' + p.stdout[-400:])


def test_surfos_new_mbr_over_gpt(test_part, tmp):
    img = os.path.join(tmp, 'surfos-mbr-over-gpt.img')
    surfos_image(test_part, 'mbr-over-gpt', img)
    pt = sfdisk(img)
    got = [(number(p['node']), p['start'], p['size'], p['type']) for p in pt['partitions']]
    check(pt['label'] == 'dos' and got == [(1, 2048, BLOCKS - 2048, 'c')], f'new mbr over a GPT: a dos label, one partition: {got}')
    with open(img, 'rb') as f:
        data = f.read()
    check(data[512:34 * 512] == bytes(33 * 512) and data[-33 * 512:] == bytes(33 * 512), 'new mbr over a GPT: both GPT areas are zero')
    if shutil.which('wipefs'):
        w = run(['wipefs', '--no-act', '--noheadings', '--output', 'TYPE', img])
        check(w.returncode == 0 and w.stdout.split() == ['dos'], 'wipefs finds the MBR and no GPT signature: ' + w.stdout.strip())


def test_sfdisk_logical(test_part, tmp):
    img = os.path.join(tmp, 'sfdisk-logical.img')
    blank_image(img)
    script = ('label: dos\nlabel-id: 0x5eed1234\nunit: sectors\n\n'
              'start=2048, size=8192, type=c, bootable\n'
              'start=10240, size=16384, type=5\n'
              'start=26624, size=4096, type=82\n'
              'start=12288, size=2048, type=83\n'           # logical partitions, 5 to 7
              'start=16384, size=4096, type=7\n'
              'start=22528, size=2048, type=c\n')
    r = run(['sfdisk', '-q', img], script)
    check(r.returncode == 0, 'sfdisk writes the MBR with logical partitions: ' + r.stdout.strip())
    pt, ours = sfdisk(img), surfos_parse(test_part, img)
    check(ours['scheme'] == 'mbr' and ours['disk'] == ['0x5eed1234'] and not ours['notes'], 'SurfOS reads the sfdisk MBR, no notes')
    theirs = {number(p['node']): p for p in pt['partitions']}
    check(sorted(theirs) == [1, 2, 3, 5, 6, 7] and sorted(ours['parts']) == sorted(theirs),
          f'the same partition numbers, logical ones from 5: {sorted(ours["parts"])}')
    for n, p in theirs.items():
        o = ours['parts'].get(n, {})
        check(o.get('start') == p['start'] and o.get('size') == p['size'] and o.get('type') == p['type'] and
              o.get('bootable') == p.get('bootable', False) and o.get('extended') == (p['type'] in ('5', 'f', '85')),
              f'partition {n}: {o} as sfdisk has it: {p}')


def test_sgdisk_gpt(test_part, tmp):
    img = os.path.join(tmp, 'sgdisk.img')
    blank_image(img)
    r = run(['sgdisk', '-o', '-n', '1:2048:+4M', '-t', '1:ef00', '-c', '1:EFI System', '-n', '2:0:+2M', '-t', '2:0700',
             '-c', '2:DATA stick', '-n', '4:0:0', '-t', '4:8300', '-c', '4:Linux root', img])
    check(r.returncode == 0, 'sgdisk writes a GPT: ' + r.stdout.strip()[-300:])
    pt, ours = sfdisk(img), surfos_parse(test_part, img)
    check(ours['scheme'] == 'gpt' and not ours['notes'], 'SurfOS reads the sgdisk GPT, no notes')
    check(ours['disk'][:3] == [pt.get('id'), str(pt.get('firstlba')), str(pt.get('lastlba'))] and ours['disk'][3:] == ['0', '0'],
          f'the disk GUID and usable blocks: {ours["disk"]}')
    theirs = {number(p['node']): p for p in pt['partitions']}
    check(sorted(theirs) == [1, 2, 4] and sorted(ours['parts']) == [1, 2, 4], 'the same entry numbers (1, 2, 4)')

    def same(o, p):
        return (o.get('start') == p['start'] and o.get('size') == p['size'] and o.get('type') == p['type'] and
                o.get('uuid') == p['uuid'] and o.get('name') == p.get('name', ''))
    for n, p in theirs.items():
        check(same(ours['parts'].get(n, {}), p), f'entry {n}: {ours["parts"].get(n)} as sfdisk has it: {p}')

    with open(img, 'r+b') as f:                   # the primary header damaged: the backup must be used
        f.seek(512 + 60)
        b = f.read(1)
        f.seek(512 + 60)
        f.write(bytes([b[0] ^ 1]))
    damaged = surfos_parse(test_part, img)
    check(damaged['disk'][3:4] == ['1'] and any('using the backup' in n for n in damaged['notes']),
          f'a damaged primary header: the backup is used, with a warning: {damaged["notes"]}')
    check(all(same(damaged['parts'].get(n, {}), p) for n, p in theirs.items()), 'the backup has the same partitions')


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    test_part = os.path.abspath(sys.argv[1])
    missing = [t for t in ('sfdisk', 'sgdisk') if not shutil.which(t)]
    if missing:
        print(f'PARTITION TOOL CHECKS SKIPPED: {", ".join(missing)} not installed (the devenv image has them)')
        return 0
    with tempfile.TemporaryDirectory(prefix='surfos part ') as tmp:
        for test in (test_surfos_mbr, test_surfos_gpt, test_surfos_new_mbr_over_gpt, test_sfdisk_logical, test_sgdisk_gpt):
            try:
                test(test_part, tmp)
            except Exception as e:                # a tool or the test program failing is a failed check, not a crash
                check(False, f'{test.__name__}: {e!r}')
    print(f'PARTITION TOOL CHECKS {"FAILED" if failures else "PASSED"}: {checks} checks, {failures} failures')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
