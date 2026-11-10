"""Tests for the development loop's host side (10/9/26): tools/devloop.py's own logic and the data stick image of
tools/mkimage.py, with the initrd's build id.

Run from the repository: python3 -m unittest discover -s tests -p test_devloop.py   (make test-devloop)

Nothing here connects anywhere: devloop.py's sessions, rx and the reboot are tested against the laptop's stand-in
under QEMU by tools/qemu-usb.py's devloop scenario. These tests need only Python; where fsck.fat is installed (the
Docker toolchain) it checks the FAT32 images as well.
"""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tarfile
import tempfile
import time
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[1]


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, REPO / 'tools' / file)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


devloop = load('devloop', 'devloop.py')
mkimage = load('mkimage', 'mkimage.py')
mkusb = load('mkusb', 'mkusb.py')

# lsblk as SurfOS prints it (docs/USB-BOOT.md section 9), a data stick with one partition
LSBLK = '''NAME     SIZE BUS RM TYPE                MODEL/LABEL      STATUS
rd0        1M ram  0 disk                initrd.tar       mounted
usb0     256M usb  1 disk gpt            QEMU QEMU HARDDI boot drive: protected
  usb0p1 254M usb  1 part gpt EFI system SURFOSUSB        boot drive: protected
usb1      16M usb  1 disk mbr            QEMU QEMU HARDDI
  usb1p1  15M usb  1 part mbr FAT16
'''


def tar_with(files):
    out = io.BytesIO()
    with tarfile.open(fileobj=out, mode='w', format=tarfile.USTAR_FORMAT) as t:
        for name, data in files.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            t.addfile(info, io.BytesIO(data))
    return out.getvalue()


class SessionOutputTests(unittest.TestCase):
    def test_clean_strips_escapes_and_the_pager(self):
        data = (b'\x1b[1;33mSURF\x1b[0m line one\r\n'
                b'\x1b[7m--More-- (Space: next page, Enter: next line, q: skip the rest)\x1b[27m\r\x1b[Kline two\r\n'
                b'caf\xc3\xa9\r\n')
        self.assertEqual(devloop.clean(data), 'SURF line one\nline two\ncaf\u00e9\n')

    def test_refused(self):
        for cmd in ('reboot', ' reboot', 'echo a; reboot', 'run reboot', 'spawn reboot', 'mkfs usb1p1',
                    'mkfs -t fat32 usb1p1 X', 'fdisk usb1 new mbr', 'fdisk usb1 add', 'ls & fdisk usb1 delete 1',
                    'telnetd off', '/initrd/bin/telnetd off', 'poweroff', 'echo a; poweroff', 'run poweroff',
                    'poweroff -nx', 'crashstack', 'crashstack near', 'echo a; crashstack'):
            self.assertIsNotNone(devloop.refused(cmd), cmd)
        for cmd in ('lsblk', 'fdisk usb1', 'fdisk', 'echo reboot', 'dmesg', 'cat /initrd/etc/build-id', 'rebooted',
                    'telnetd', 'telnetd status', 'poweroff -n', 'echo poweroff'):
            self.assertIsNone(devloop.refused(cmd), cmd)


class DataStickTests(unittest.TestCase):
    def test_parse_lsblk(self):
        rows = devloop.parse_lsblk(LSBLK)
        self.assertEqual([r['name'] for r in rows], ['rd0', 'usb0', 'usb0p1', 'usb1', 'usb1p1'])
        self.assertEqual([r['part'] for r in rows], [False, False, True, False, True])
        self.assertEqual(rows[1]['status'], 'boot drive: protected')
        self.assertEqual(rows[3]['status'], '')
        self.assertEqual([r['type'] for r in rows], ['disk', 'disk gpt', 'part gpt EFI system', 'disk mbr', 'part mbr FAT16'])
        self.assertEqual([r['label'] for r in rows], ['initrd.tar', 'QEMU QEMU HARDDI', 'SURFOSUSB', 'QEMU QEMU HARDDI', ''])

    def test_the_stick_and_its_first_partition(self):
        disk, dev = devloop.data_stick(devloop.parse_lsblk(LSBLK))
        self.assertEqual((disk['name'], dev['name']), ('usb1', 'usb1p1'))
        two = LSBLK + '  usb1p2  1M usb  1 part mbr FAT16\n'
        self.assertEqual(devloop.data_stick(devloop.parse_lsblk(two))[1]['name'], 'usb1p1')

    def test_a_stick_without_partitions_is_used_whole(self):
        text = LSBLK.replace('usb1      16M usb  1 disk mbr', 'usb1      16M usb  1 disk    ').replace(
            '  usb1p1  15M usb  1 part mbr FAT16\n', '')
        disk, dev = devloop.data_stick(devloop.parse_lsblk(text))
        self.assertEqual((disk['name'], dev['name']), ('usb1', 'usb1'))

    def test_no_stick_or_two(self):
        alone = '\n'.join(LSBLK.splitlines()[:4]) + '\n'
        with self.assertRaisesRegex(devloop.DevloopError, 'no data stick'):
            devloop.data_stick(devloop.parse_lsblk(alone))
        two = LSBLK + 'usb2      32M usb  1 disk mbr            Kingston\n  usb2p1  31M usb  1 part mbr FAT32 LBA\n'
        with self.assertRaisesRegex(devloop.DevloopError, 'more than one.*FAT partition.*usb1, usb2.*--stick'):
            devloop.data_stick(devloop.parse_lsblk(two))
        self.assertEqual(devloop.data_stick(devloop.parse_lsblk(two), 'usb2')[1]['name'], 'usb2p1')
        with self.assertRaisesRegex(devloop.DevloopError, 'usb0 is not a USB disk'):
            devloop.data_stick(devloop.parse_lsblk(two), 'usb0')          # the boot drive never

    def test_the_owners_sticks(self):
        """10/9/26: the owner's two sticks laid out as lsblk lays them out: the boot stick (118 GB, GPT, its EFI system
        partition SURFOSUSB, protected) and the data stick (30 GB, MBR, one FAT32 LBA partition); usb0 and usb1 change
        places between boots, and the choice must not"""
        boot = ('usb{b}     118G usb  1 disk gpt            General USB Flas boot drive: protected\n'
                '  usb{b}p1 254M usb  1 part gpt EFI system SURFOSUSB        boot drive: protected\n')
        data = ('usb{d}      29G usb  1 disk mbr            VendorCo Product\n'
                '  usb{d}p1  29G usb  1 part mbr FAT32 LBA\n')
        head = ('NAME     SIZE BUS RM TYPE                MODEL/LABEL      STATUS\n'
                'rd0        2M ram  0 disk                initrd.tar       mounted\n')
        for b, d, order in ((0, 1, (boot, data)), (1, 0, (data, boot))):
            text = head + ''.join(x.format(b=b, d=d) for x in order)
            disk, dev = devloop.data_stick(devloop.parse_lsblk(text))
            self.assertEqual((disk['name'], disk['label'], dev['name'], dev['type']),
                             (f'usb{d}', 'VendorCo Product', f'usb{d}p1', 'part mbr FAT32 LBA'))

    def test_only_a_disk_with_a_fat_partition(self):
        """a second stick beside the data stick that is blank, or NTFS, is not a candidate; without the data stick
        neither is chosen, and the message says what each one holds"""
        blank = 'usb2      64M usb  1 disk                QEMU QEMU HARDDI\n'
        ntfs = 'usb3      64G usb  1 disk mbr            SanDisk\n  usb3p1  64G usb  1 part mbr NTFS/exFAT\n'
        disk, dev = devloop.data_stick(devloop.parse_lsblk(LSBLK + blank + ntfs))
        self.assertEqual((disk['name'], dev['name']), ('usb1', 'usb1p1'))
        alone = '\n'.join(LSBLK.splitlines()[:4]) + '\n'
        with self.assertRaisesRegex(devloop.DevloopError, 'usb2: no partition table, usb3: its first partition is '
                                                          'mbr NTFS/exFAT.*never formats'):
            devloop.data_stick(devloop.parse_lsblk(alone + blank + ntfs))
        disk, dev = devloop.data_stick(devloop.parse_lsblk(alone + blank))       # mounting it says whether it is FAT
        self.assertEqual((disk['name'], dev['name']), ('usb2', 'usb2'))
        gpt = 'usb4      32G usb  1 disk gpt            Mac formatted\n  usb4p1 200M usb  1 part gpt EFI system EFI\n'
        disk, dev = devloop.data_stick(devloop.parse_lsblk(alone + gpt))          # GRUB looks at gpt1 too
        self.assertEqual(dev['name'], 'usb4p1')

    def test_no_table(self):
        with self.assertRaises(devloop.DevloopError):
            devloop.parse_lsblk('lsblk: something else\n')


# 10/17/26 (one stick): the owner's data stick made bootable by copying the boot files onto it, as lsblk shows it
# (driver/bdev.c): its disk protected, the volume SurfOS was started from writable
HEAD = ('NAME     SIZE BUS RM TYPE                MODEL/LABEL      STATUS\n'
        'rd0        2M ram  0 disk                initrd.tar       mounted\n')
ONE = ('usb{n}      29G usb  1 disk mbr            VendorCo Product boot drive: protected\n'
       '  usb{n}p1  29G usb  1 part mbr FAT32 LBA                   {status}\n')
IMAGE = ('usb{n}     118G usb  1 disk gpt            General USB Flas boot drive: protected\n'
         '  usb{n}p1 254M usb  1 part gpt EFI system SURFOSUSB        boot drive: protected\n')
DATA = ('usb{n}      16G usb  1 disk mbr            Kingston\n'
        '  usb{n}p1  16G usb  1 part mbr FAT32 LBA\n')


class OneStickTests(unittest.TestCase):
    def pick(self, text, name=None):
        disk, dev = devloop.data_stick(devloop.parse_lsblk(text), name)
        return disk['name'], dev['name']

    def test_the_stick_it_was_started_from(self):
        for status in devloop.BOOT_VOLUME:                       # mounted or not
            self.assertEqual(self.pick(HEAD + ONE.format(n=0, status=status)), ('usb0', 'usb0p1'))
            self.assertEqual(self.pick(HEAD + ONE.format(n=0, status=status), 'usb0'), ('usb0', 'usb0p1'))

    def test_before_a_data_stick(self):
        """GRUB's development entry looks at its own partition first, so the build goes there too, whichever of
        the two is usb0; --stick naming the other one is refused, since GRUB would not start a build there"""
        one, data = ONE.format(n=0, status='boot drive: writable'), DATA.format(n=1)
        self.assertEqual(self.pick(HEAD + one + data), ('usb0', 'usb0p1'))
        self.assertEqual(self.pick(HEAD + data.replace('usb1', 'usb0') + one.replace('usb0', 'usb1')), ('usb1', 'usb1p1'))
        with self.assertRaisesRegex(devloop.DevloopError, r'usb1 is not the stick SurfOS was started from \(usb0\).*'
                                                          r'ONE-STICK.txt.*deploy without --stick'):
            self.pick(HEAD + one + data, 'usb1')

    def test_the_image_is_never_written(self):
        """the flashed image (SURFOSUSB) is only read: with it, the data stick, as before; alone, nothing"""
        self.assertEqual(self.pick(HEAD + IMAGE.format(n=0) + DATA.format(n=1)), ('usb1', 'usb1p1'))
        with self.assertRaisesRegex(devloop.DevloopError, 'no data stick'):
            self.pick(HEAD + IMAGE.format(n=0))
        with self.assertRaisesRegex(devloop.DevloopError, 'usb0 is not a USB disk'):
            self.pick(HEAD + IMAGE.format(n=0) + DATA.format(n=1), 'usb0')

    def test_its_other_partitions_are_not_it(self):
        """a stick the Mac made GPT has its EFI partition first; SurfOS was started from the second, which alone is
        writable (the partition GRUB's menu looks at)"""
        mac = ('usb0      29G usb  1 disk gpt            SanDisk          boot drive: protected\n'
               '  usb0p1 200M usb  1 part gpt EFI system EFI              boot drive: protected\n'
               '  usb0p2  29G usb  1 part gpt basic data DATASTICK        boot drive: writable\n')
        self.assertEqual(self.pick(HEAD + mac), ('usb0', 'usb0p2'))

    def test_a_stick_without_a_partition_table(self):
        whole = 'usb0      29G usb  1 disk                VendorCo Product boot drive: writable\n'
        self.assertEqual(self.pick(HEAD + whole), ('usb0', 'usb0'))

    def test_two_that_say_so(self):
        text = HEAD + ONE.format(n=0, status='boot drive: writable') + ONE.format(n=1, status='mounted (boot drive)')
        with self.assertRaisesRegex(devloop.DevloopError, 'more than one volume.*usb0p1, usb1p1'):
            self.pick(text)

    class Session:                              # the telnet session, as deploy uses it
        host = '127.0.0.1'

        def __init__(self, marker, status='boot drive: writable'):
            self.marker, self.status, self.commands = marker, status, []

        def check(self, cmd, timeout=60, ok=None):
            self.commands.append(cmd)
            return {'lsblk': HEAD + ONE.format(n=0, status=self.status),
                    'mount': '    rd0    on /initrd       type tarfs (ro)\n'}.get(cmd, '')

        def run(self, cmd, timeout=60):
            self.commands.append(cmd)
            if cmd.startswith('mount usb0p1 '):
                return 0, '    usb0p1 on /surfos-dev-stick: fat32, label DATASTICK, 29 GB, 29 GB free, read-write\n'
            if cmd.startswith('cat '):
                return (0, mkusb.STICK_NOTE) if self.marker else (1, '    cat: no such file\n')
            return 0, '    usb0: safe to remove\n' if cmd.startswith('eject ') else ''

    def test_deploy_needs_the_marker(self):
        """GRUB looks in the surfos-dev of the stick it was started from only beside boot/ONE-STICK.txt
        (tools/stick.py install writes it): without it nothing is pushed, and the stick is ejected as it was"""
        s = self.Session(marker=False)
        with self.assertRaisesRegex(devloop.DevloopError, 'usb0p1, the stick SurfOS was started from, has no '
                                                          'boot/ONE-STICK.txt.*tools/stick.py install'):
            devloop.deploy(s, b'kernel bootvol=', b'initrd', 'a build')
        self.assertIn('cat /surfos-dev-stick/boot/ONE-STICK.txt', s.commands)
        self.assertEqual(s.commands[-1], 'eject usb0')
        self.assertFalse([c for c in s.commands if c.startswith(('mkdir', 'rx ', 'mv '))])
        self.assertEqual(devloop.MARKER, mkusb.STICK_MARKER)

    def test_deploy_with_the_marker_pushes_there(self):
        """with the marker, the build goes into that stick's surfos-dev: the folder is made and the initrd pushed
        first (the push itself is tested against QEMU, tools/qemu-usb.py's onestick scenario)"""
        class Pushed(Exception):
            pass
        s = self.Session(marker=True)
        with mock.patch.object(devloop, 'push', side_effect=Pushed) as push:
            with self.assertRaises(Pushed):
                devloop.deploy(s, b'kernel bootvol=', b'initrd', 'a build')
        self.assertIn('mkdir /surfos-dev-stick/surfos-dev', s.commands)
        self.assertEqual(push.call_args[0][1:3], (b'initrd', '/surfos-dev-stick/surfos-dev/initrd.tar.new'))

    def test_deploy_says_to_check_the_stick_when_mv_fails_with_io(self):
        """10/17/26: an mv into place cut short by an I/O error may leave both names on one cluster chain, so
        deploy says to check the stick before deploying again; it still ejects the stick"""
        class Failing(self.Session):
            def check(self, cmd, timeout=60, ok=None):
                if cmd.startswith('mv '):
                    self.commands.append(cmd)
                    raise devloop.DevloopError(f'{cmd}: mv: I/O error')
                return super().check(cmd, timeout, ok)
        s = Failing(marker=True)
        with mock.patch.object(devloop, 'push'):
            with self.assertRaisesRegex(devloop.DevloopError, 'I/O error: the rename may have half happened, so check '
                                                              'the stick'):
                devloop.deploy(s, b'kernel bootvol=', b'initrd', 'a build')
        self.assertEqual(s.commands[-1], 'eject usb0')

    def test_deploy_says_when_a_kernel_does_not_know_the_stick(self):
        s = self.Session(marker=False)
        out = io.StringIO()
        with contextlib.redirect_stdout(out), self.assertRaises(devloop.DevloopError):
            devloop.deploy(s, b'an older kernel', b'initrd', 'a build')
        self.assertIn('this kernel is from before 10/17/26', out.getvalue())

    def test_a_write_protected_boot_stick(self):
        with self.assertRaisesRegex(devloop.DevloopError, 'usb0p1, the volume SurfOS was started from, is '
                                                          'write-protected: slide'):
            self.pick(HEAD + ONE.format(n=0, status='boot drive: read-only') + DATA.format(n=1))
        with self.assertRaisesRegex(devloop.DevloopError, 'more than one volume.*usb0p1, usb1p1.*same FAT serial'):
            self.pick(HEAD + ONE.format(n=0, status='boot drive: read-only') +   # two with one serial: that first
                      ONE.format(n=1, status='boot drive: writable'))

    def test_no_note_for_a_kernel_that_knows_the_stick(self):
        s = self.Session(marker=False)
        out = io.StringIO()
        with contextlib.redirect_stdout(out), self.assertRaises(devloop.DevloopError):
            devloop.deploy(s, b'a kernel that reads bootvol=', b'initrd', 'a build')
        self.assertNotIn('from before 10/17/26', out.getvalue())


class BuildTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='surfos devloop ')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.saved = devloop.REPO
        devloop.REPO = str(self.root)
        self.addCleanup(setattr, devloop, 'REPO', self.saved)

    def put(self, level, when, docker=True, initrd_when=None):
        d = self.root / 'build' / ('docker/build' if docker else '') / f'O{level}'
        d.mkdir(parents=True, exist_ok=True)
        for name, t in (('surfos.bin', when), ('initrd.tar', initrd_when or when)):
            (d / name).write_bytes(b'x')
            os.utime(d / name, (t, t))
        return str(d / 'surfos.bin'), str(d / 'initrd.tar')

    def test_the_newest_build(self):
        now = time.time()
        self.put(0, now - 100)
        o2 = self.put(2, now - 50)
        self.assertEqual(devloop.default_build(), o2)
        o0 = self.put(0, now)
        self.assertEqual(devloop.default_build(), o0)
        self.assertEqual(devloop.default_build('2'), o2)

    def test_linux_builds_and_none(self):
        with self.assertRaisesRegex(devloop.DevloopError, 'no build'):
            devloop.default_build()
        o2 = self.put(2, time.time(), docker=False)
        self.assertEqual(devloop.default_build(), o2)

    def test_an_initrd_older_than_its_kernel_is_refused(self):
        self.put(2, time.time(), initrd_when=time.time() - 600)
        with self.assertRaisesRegex(devloop.DevloopError, 'older than'):
            devloop.default_build()

    def test_build_id_of(self):
        self.assertEqual(devloop.build_id_of(tar_with({'bin/x': b'1', 'etc/build-id': b'abc123def456 2026-10-09T12:00:00Z\n'})),
                         'abc123def456 2026-10-09T12:00:00Z')
        with self.assertRaisesRegex(devloop.DevloopError, 'no etc/build-id'):
            devloop.build_id_of(tar_with({'bin/x': b'1'}))
        with self.assertRaisesRegex(devloop.DevloopError, 'no tar'):
            devloop.build_id_of(b'not a tar archive' * 100)

    def test_mkimage_writes_the_build_id(self):
        d = self.root / 'initrd'
        (d / 'bin').mkdir(parents=True)
        (d / 'bin' / 'hello').write_bytes(b'\x7fELF')
        mkimage.make_initrd(str(self.root / 'initrd.tar'), str(d), '34e7a1f3bea9-dirty')
        with tarfile.open(self.root / 'initrd.tar') as t:
            got = t.extractfile('etc/build-id').read().decode()
        self.assertRegex(got, r'^34e7a1f3bea9-dirty \d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ\n$')


class DataStickImageTests(unittest.TestCase):
    """tools/mkimage.py's FAT32 writer, read back with tools/mkusb.py's own FAT32 reader (and fsck.fat when there)"""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='surfos devstick ')
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)

    def read_back(self, path):
        """the image's FAT32 volume and the open file (the caller closes it)"""
        f = open(path, 'rb')
        mbr = f.read(512)
        self.assertEqual(mbr[510:512], b'\x55\xaa')
        self.assertEqual(mbr[446 + 4], 0x0C)                                        # FAT32 LBA
        start = struct.unpack_from('<I', mbr, 446 + 8)[0]
        self.assertEqual(start, 2048)
        return mkusb.Fat32(f, start * 512), f

    def fsck(self, path, offset):
        if not shutil.which('fsck.fat'):
            return
        part = self.dir / 'part.img'
        with open(path, 'rb') as f, open(part, 'wb') as o:
            f.seek(offset)
            o.write(f.read())
        r = subprocess.run(['fsck.fat', '-n', str(part)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_devstick(self):
        kernel, initrd = os.urandom(1300000), os.urandom(2100000)
        out = str(self.dir / 'stick.img')
        info = mkimage.make_devstick(out, kernel, initrd)
        self.assertEqual(os.path.getsize(out), 512 << 20)
        fat, f = self.read_back(out)
        with f:
            self.assertEqual(fat.label, 'SURFDEV')
            self.assertEqual(fat.read('surfos-dev/surfos.bin'), kernel)
            self.assertEqual(fat.read('/surfos-dev/initrd.tar'), initrd)
            names = [n for n, _, _, _, _ in fat.entries(fat.root)]
            self.assertEqual(names, ['surfos-dev'])                                  # its long name, not SURFOS~1
        self.fsck(out, info['offset'])
        # 10/17/26: the volume serial number, which GRUB passes as bootvol=: the data stick has its own, and any
        # image can be given one (two sticks of one test must not share it)
        with open(out, 'rb') as f:
            f.seek(info['offset'] + 67)
            self.assertEqual(struct.unpack('<I', f.read(4))[0], 0x0DA7A517)
        other = str(self.dir / 'other.img')
        mkimage.make_fat32(other, {'a.txt': b'a'}, 512, serial=0x12345678)
        with open(other, 'rb') as f:
            f.seek(info['offset'] + 67)
            self.assertEqual(struct.unpack('<I', f.read(4))[0], 0x12345678)

    def test_names_folders_and_empty_files(self):
        files = {'README.TXT': b'plain 8.3\n', 'A name longer than twenty-six characters, with spaces.txt': b'long\n',
                 'caf\u00e9.txt': b'accent\n', 'empty': b'', 'Photos/2026/October/notes.txt': b'deep\n'}
        files.update({f'many/file number {i:03d}.bin': bytes([i]) * i for i in range(150)})   # several clusters of entries
        out = str(self.dir / 'files.img')
        info = mkimage.make_fat32(out, files, size_mb=64, cluster_bytes=512)
        fat, f = self.read_back(out)
        with f:
            for path, data in files.items():
                self.assertEqual(fat.read(path), data, path)
            root = sorted(n for n, _, _, _, _ in fat.entries(fat.root))
            self.assertEqual(root, sorted(['README.TXT', 'A name longer than twenty-six characters, with spaces.txt',
                                           'caf\u00e9.txt', 'empty', 'Photos', 'many']))
        self.fsck(out, info['offset'])

    def test_a_root_of_several_clusters(self):
        # 11/09/26 (the desktop applications' integration): a root whose entries take more than one cluster (every
        # track's folder on the guiapps stick) has a chain of its own; one whose entries fit keeps one cluster
        files = {f'folder number {i:02d}/f.txt': bytes([65 + i % 26]) * (i + 1) for i in range(40)}
        files['zz-last.bin'] = b'last\n'
        out = str(self.dir / 'root.img')
        info = mkimage.make_fat32(out, files, size_mb=64, label='GUISTICK', cluster_bytes=512)
        fat, f = self.read_back(out)
        with f:
            self.assertGreater(len(list(fat.chain(fat.root))), 1)
            root = sorted(n for n, _, _, _, _ in fat.entries(fat.root))
            self.assertEqual(root, sorted({p.split('/')[0] for p in files}))
            for path, data in files.items():
                self.assertEqual(fat.read(path), data, path)
        self.fsck(out, info['offset'])
        small = str(self.dir / 'small-root.img')
        mkimage.make_fat32(small, {'a.txt': b'a', 'b/c.txt': b'c'}, size_mb=64, cluster_bytes=512)
        fat, f = self.read_back(small)
        with f:
            self.assertEqual(len(list(fat.chain(fat.root))), 1)

    def test_too_small_for_fat32(self):
        with self.assertRaises(ValueError):
            mkimage.make_fat32(str(self.dir / 'small.img'), {'x': b'1'}, size_mb=16)

    def test_short_aliases(self):
        taken = set()
        self.assertEqual(mkimage.short_alias('README.TXT', taken), (b'README  TXT', False))
        self.assertEqual(mkimage.short_alias('surfos-dev', taken), (b'SURFOS~1   ', True))
        self.assertEqual(mkimage.short_alias('surfos-development', taken), (b'SURFOS~2   ', True))
        self.assertEqual(mkimage.short_alias('initrd.tar', taken), (b'INITRD~1TAR', True))


if __name__ == '__main__':
    unittest.main()
