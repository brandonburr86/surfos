"""Tests for tools/stick.py (10/17/26, one stick): SurfOS's boot files and builds onto a FAT32 stick from this computer.

Run from the repository: python3 -m unittest discover -s tests -p test_stick.py   (make unittest)

Nothing here touches a disk: the "stick" is a temporary directory, and the checks of the volume are tested on the
answers diskutil and Linux give (a dict, a mounts table and a sysfs tree made here).
"""
import collections
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import struct
import tarfile
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[1]


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, REPO / 'tools' / file)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


stick = load('stick', 'stick.py')
mkusb = load('mkusb', 'mkusb.py')


def fake_kernel(extra=b''):
    """a 32-bit x86 ELF header with a valid Multiboot 2 header at offset 0x1010 (as tests/test_mkusb.py makes one)"""
    data = bytearray(0x2000)
    data[0:4] = b'\x7fELF'
    data[4] = 1
    struct.pack_into('<H', data, 18, 3)
    magic, arch, length = 0xE85250D6, 0, 24
    struct.pack_into('<IIII', data, 0x1010, magic, arch, length, (-(magic + arch + length)) & 0xFFFFFFFF)
    return bytes(data) + extra


def initrd_with(build_id):
    out = io.BytesIO()
    with tarfile.open(fileobj=out, mode='w', format=tarfile.USTAR_FORMAT) as t:
        for name, data in (('etc/build-id', (build_id + '\n').encode()), ('bin/surfsh', b'\x7fELF a program')):
            info = tarfile.TarInfo(name)
            info.size = len(data)
            t.addfile(info, io.BytesIO(data))
    return out.getvalue()


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='surfos stick ')      # a space, like the project path
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)
        self.vol = self.dir / 'Volumes' / 'DATA STICK'
        (self.vol / 'My Files').mkdir(parents=True)
        (self.vol / 'My Files' / 'notes.txt').write_bytes(b'mine, never touched')
        self.kit = self.dir / 'usb' / 'stick'
        self.make_kit('build one')

    def make_kit(self, bid):
        """a stick folder as tools/mkusb.py writes one, with its manifest beside it"""
        self.files = {'EFI/BOOT/BOOTX64.EFI': b'MZ loader ' + bid.encode(), 'boot/grub/grub.cfg': b'menu ' + bid.encode(),
                      'boot/grub/x86_64-efi/fs.lst': b'', 'boot/surfos.bin': fake_kernel(bid.encode()),
                      'boot/initrd.tar': initrd_with(bid), 'boot/BUILD-INFO.txt': b'info ' + bid.encode(),
                      'surfos-dev/surfos.bin': fake_kernel(bid.encode()), 'surfos-dev/initrd.tar': initrd_with(bid)}
        for rel, data in self.files.items():
            (self.kit / rel).parent.mkdir(parents=True, exist_ok=True)
            (self.kit / rel).write_bytes(data)
        listed = {rel: {'bytes': len(d), 'sha256': hashlib.sha256(d).hexdigest()} for rel, d in self.files.items()}
        (self.kit.parent / 'build-manifest.json').write_text(json.dumps({'stick': {'folder': 'stick', 'files': listed}}))

    def main(self, *args, label=''):
        """stick.main on the temporary "stick"; (status, stdout, stderr, the eject mock)"""
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(stick, 'volume_info', return_value=(f'{self.vol} (a test)', label)), \
                mock.patch.object(stick, 'eject') as ej, contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = stick.main([str(a) for a in args])
        return rc, out.getvalue(), err.getvalue(), ej

    def on_stick(self, rel):
        return (self.vol / rel).read_bytes()

    def leftovers(self):
        return [str(p) for p in self.vol.rglob('*.new')]


class InstallUpdateTests(Base):
    def test_install_puts_every_file_and_keeps_mine(self):
        rc, out, err, ej = self.main('install', self.vol, '--kit', self.kit)
        self.assertEqual(rc, 0, err)
        for rel, data in self.files.items():
            self.assertEqual(self.on_stick(rel), data, rel)
        self.assertEqual(self.on_stick('My Files/notes.txt'), b'mine, never touched')
        self.assertEqual(self.leftovers(), [])
        self.assertIn('the stick starts SurfOS', out)
        self.assertIn('build build one', out)
        ej.assert_not_called()

    def test_install_again_brings_everything_up_to_date(self):
        self.assertEqual(self.main('install', self.vol, '--kit', self.kit)[0], 0)
        self.make_kit('build two')
        rc, out, err, ej = self.main('install', self.vol, '--kit', self.kit, '--eject')
        self.assertEqual(rc, 0, err)
        for rel, data in self.files.items():
            self.assertEqual(self.on_stick(rel), data, rel)
        ej.assert_called_once_with(os.path.realpath(self.vol))

    def test_update_writes_surfos_dev_only(self):
        self.assertEqual(self.main('install', self.vol, '--kit', self.kit)[0], 0)
        before = {rel: (self.on_stick(rel), (self.vol / rel).stat().st_mtime_ns) for rel in self.files
                  if not rel.startswith('surfos-dev/')}
        self.make_kit('build two')
        rc, out, err, _ = self.main('update', self.vol, '--kit', self.kit)
        self.assertEqual(rc, 0, err)
        self.assertEqual(self.on_stick('surfos-dev/surfos.bin'), self.files['surfos-dev/surfos.bin'])
        self.assertEqual(self.on_stick('surfos-dev/initrd.tar'), self.files['surfos-dev/initrd.tar'])
        for rel, (data, mtime) in before.items():
            self.assertEqual((self.on_stick(rel), (self.vol / rel).stat().st_mtime_ns), (data, mtime), rel)
        self.assertIn('surfos-dev/ holds build build two', out)

    def test_update_from_a_kernel_and_its_initrd(self):
        kernel, initrd = self.dir / 'surfos.bin', self.dir / 'initrd.tar'
        kernel.write_bytes(fake_kernel(b'mine'))
        initrd.write_bytes(initrd_with('my build'))
        rc, out, err, _ = self.main('update', self.vol, '--kernel', kernel, '--initrd', initrd)
        self.assertEqual(rc, 0, err)
        self.assertEqual(self.on_stick('surfos-dev/surfos.bin'), kernel.read_bytes())
        self.assertFalse((self.vol / 'boot').exists())
        self.assertIn('EFI/BOOT/BOOTX64.EFI is missing', out)                 # not bootable yet: said so
        rc, _, err, _ = self.main('update', self.vol, '--kernel', kernel)
        self.assertEqual(rc, 1)
        self.assertIn('go together', err)
        kernel.write_bytes(b'not an ELF file at all' * 100)
        rc, _, err, _ = self.main('update', self.vol, '--kernel', kernel, '--initrd', initrd)
        self.assertEqual(rc, 1)
        self.assertIn('not a 32-bit x86 ELF', err)
        initrd.write_bytes(b'no tar')
        kernel.write_bytes(fake_kernel(b'mine'))
        rc, _, err, _ = self.main('update', self.vol, '--kernel', kernel, '--initrd', initrd)
        self.assertEqual(rc, 1)
        self.assertEqual(self.leftovers(), [])

    def test_the_flashed_image_is_written_with_a_note(self):
        rc, out, err, _ = self.main('install', self.vol, '--kit', self.kit, label='SURFOSUSB  ')
        self.assertEqual(rc, 0, err)
        self.assertIn('flashed image (SURFOSUSB)', out)


class RefusalTests(Base):
    def assert_untouched(self):
        self.assertEqual(sorted(str(p.relative_to(self.vol)) for p in self.vol.rglob('*')),
                         ['My Files', 'My Files/notes.txt'])

    def test_a_folder_that_is_not_what_the_build_made(self):
        (self.kit / 'boot/surfos.bin').write_bytes(fake_kernel(b'changed after the build'))
        rc, _, err, _ = self.main('install', self.vol, '--kit', self.kit)
        self.assertEqual(rc, 1)
        self.assertIn('not what the build made', err)
        self.assert_untouched()
        self.assertEqual(self.main('kit', '--kit', self.kit)[0], 1)

    def test_no_manifest_or_a_file_missing(self):
        (self.kit / 'surfos-dev/initrd.tar').unlink()
        rc, _, err, _ = self.main('install', self.vol, '--kit', self.kit)
        self.assertEqual(rc, 1)
        self.assertIn('has no surfos-dev/initrd.tar', err)
        (self.kit.parent / 'build-manifest.json').unlink()
        rc, _, err, _ = self.main('update', self.vol, '--kit', self.kit)
        self.assertEqual(rc, 1)
        self.assertIn('no build manifest', err)
        self.assert_untouched()

    def test_no_room(self):
        usage = collections.namedtuple('usage', 'total used free')(1 << 30, (1 << 30) - 5000, 5000)
        with mock.patch.object(stick.shutil, 'disk_usage', return_value=usage):
            rc, _, err, _ = self.main('install', self.vol, '--kit', self.kit)
        self.assertEqual(rc, 1)
        self.assertIn('free; the files need', err)
        self.assert_untouched()

    def test_a_file_that_does_not_read_back(self):
        """nothing is renamed into place, the old files stay, the .new files go"""
        self.assertEqual(self.main('install', self.vol, '--kit', self.kit)[0], 0)
        old = {rel: self.on_stick(rel) for rel in self.files}
        self.make_kit('build two')
        real = stick.read_back
        with mock.patch.object(stick, 'read_back', side_effect=lambda p: real(p) + (b'x' if p.endswith('.tar.new') else b'')):
            rc, _, err, _ = self.main('install', self.vol, '--kit', self.kit)
        self.assertEqual(rc, 1)
        self.assertIn('does not read back the same; nothing was renamed', err)
        self.assertEqual({rel: self.on_stick(rel) for rel in self.files}, old)
        self.assertEqual(self.leftovers(), [])

    def test_order_and_a_stick_pulled_halfway(self):
        """the renames come in install order, the boot loader last; when the stick goes before that, the files
        renamed already stay, the loader is not there, no .new file is left, and the message says what to do"""
        renamed = []
        real = os.replace

        def replace(src, dst):
            rel = os.path.relpath(dst, os.path.realpath(self.vol)).replace(os.sep, '/')
            if rel == stick.LOADER:
                raise OSError(5, 'Input/output error', dst)
            renamed.append(rel)
            real(src, dst)
        with mock.patch.object(stick.os, 'replace', side_effect=replace):
            rc, _, err, _ = self.main('install', self.vol, '--kit', self.kit)
        self.assertEqual(rc, 1)
        self.assertEqual(renamed, stick.INSTALL[:-1])
        self.assertEqual(stick.INSTALL[-1], stick.LOADER)
        self.assertLess(stick.INSTALL.index('surfos-dev/initrd.tar'), stick.INSTALL.index('surfos-dev/surfos.bin'))
        self.assertFalse((self.vol / stick.LOADER).exists())
        self.assertEqual(self.leftovers(), [])
        self.assertIn('run the same command again', err)


class CheckTests(Base):
    def test_what_a_stick_holds(self):
        rc, out, err, _ = self.main('check', self.vol, '--kit', self.kit)
        self.assertEqual(rc, 0, err)
        self.assertIn('does not start from this stick yet', out)
        self.assertEqual(self.main('install', self.vol, '--kit', self.kit)[0], 0)
        rc, out, _, _ = self.main('check', self.vol, '--kit', self.kit)
        self.assertIn('there, as in the stick folder', out)
        self.assertIn('build build one, as in the stick folder', out)
        self.assertIn('SurfOS starts from this stick, the development build first', out)
        self.make_kit('build two')
        rc, out, _, _ = self.main('check', self.vol, '--kit', self.kit)
        self.assertIn('build build one, not the stick folder\'s build', out)
        (self.vol / 'surfos-dev/surfos.bin').unlink()
        rc, out, _, _ = self.main('check', self.vol, '--kit', self.kit)
        self.assertIn('incomplete: surfos-dev/surfos.bin missing', out)
        self.assertIn('SurfOS starts from this stick\n', out)

    def test_kit(self):
        rc, out, err, _ = self.main('kit', '--kit', self.kit)
        self.assertEqual(rc, 0, err)
        self.assertIn('8 files as the manifest lists them; build build one', out)


class VolumeTests(unittest.TestCase):
    """what diskutil (macOS) and /proc/self/mounts with sysfs (Linux) say decides; the stick is never guessed"""
    STICK = {'MountPoint': '/Volumes/DATASTICK', 'FilesystemType': 'msdos', 'FilesystemName': 'MS-DOS FAT32',
             'Internal': False, 'RemovableMediaOrExternalDevice': True, 'WritableVolume': True,
             'VolumeName': 'DATASTICK', 'DeviceIdentifier': 'disk4s1', 'BusProtocol': 'USB'}

    def test_mac(self):
        where, label = stick.check_mac('/Volumes/DATASTICK', dict(self.STICK))
        self.assertEqual(label, 'DATASTICK')
        self.assertIn('disk4s1', where)
        for change, why in (({'Internal': True}, 'own disks'), ({'RemovableMediaOrExternalDevice': False}, 'own disks'),
                            ({'FilesystemType': 'apfs', 'FilesystemName': 'APFS'}, 'not FAT'),
                            ({'FilesystemType': 'exfat'}, 'not FAT'), ({'WritableVolume': False}, 'read-only'),
                            ({'MountPoint': '/Volumes/OTHER'}, 'not the mount point')):
            with self.assertRaisesRegex(stick.StickError, why):
                stick.check_mac('/Volumes/DATASTICK', dict(self.STICK, **change))
        # the Mac's own EFI system partition, if it were mounted: FAT, but internal
        efi = dict(self.STICK, MountPoint='/Volumes/EFI', Internal=True, RemovableMediaOrExternalDevice=False)
        with self.assertRaisesRegex(stick.StickError, 'own disks'):
            stick.check_mac('/Volumes/EFI', efi)

    def sysfs(self, root, name, disk_path, removable):
        """sys/class/block/<name> -> devices/<disk_path>/<disk>/<name>, as Linux lays a partition out"""
        disk = Path(root, 'devices', disk_path)
        part = disk / name
        part.mkdir(parents=True)
        (part / 'partition').write_text('1\n')
        (disk / 'removable').write_text(removable + '\n')
        (Path(root) / 'class' / 'block').mkdir(parents=True, exist_ok=True)
        (Path(root) / 'class' / 'block' / name).symlink_to(part)

    def test_linux(self):
        with tempfile.TemporaryDirectory() as tmp:
            sys_root, dev_root = os.path.join(tmp, 'sys'), os.path.join(tmp, 'dev')
            self.sysfs(sys_root, 'sdb1', 'pci0000:00/0000:00:14.0/usb1/1-2/host6/block/sdb', '1')
            self.sysfs(sys_root, 'nvme0n1p1', 'pci0000:00/0000:00:1d.0/nvme/nvme0/nvme0n1', '0')
            self.sysfs(sys_root, 'sdc1', 'pci0000:00/0000:00:17.0/ata2/host1/block/sdc', '0')
            os.makedirs(os.path.join(dev_root, 'disk', 'by-label'))
            sdb1 = os.path.join(dev_root, 'sdb1')
            Path(sdb1).write_text('')
            mounts = (f'{sdb1} /media/me/DATA\\040STICK vfat rw,nosuid,nodev 0 0\n'
                      f'/dev/nvme0n1p1 /boot/efi vfat rw 0 0\n'
                      f'/dev/sdc1 /mnt/old vfat rw 0 0\n'
                      f'/dev/sdd1 /media/me/NTFS ntfs3 rw 0 0\n'
                      f'/dev/sde1 /media/me/RO vfat ro 0 0\n')
            where, label = stick.check_linux('/media/me/DATA STICK', mounts, sys_root, dev_root)
            self.assertEqual(label, '')
            os.symlink(sdb1, os.path.join(dev_root, 'disk', 'by-label', 'SURFOSUSB'))
            self.assertEqual(stick.check_linux('/media/me/DATA STICK', mounts, sys_root, dev_root)[1], 'SURFOSUSB')
            for path, why in (('/boot/efi', 'own system or boot partition'), ('/mnt/old', 'not on a removable'),
                              ('/media/me/NTFS', 'not FAT'), ('/media/me/RO', 'read-only'),
                              ('/media/me/NONE', 'not in /proc/self/mounts')):
                with self.assertRaisesRegex(stick.StickError, why):
                    stick.check_linux(path, mounts, sys_root, dev_root)

    def test_a_path_that_is_not_a_mount_point(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(stick.StickError, 'not where a volume is mounted'):
                stick.volume_info(tmp)


if __name__ == '__main__':
    unittest.main()
