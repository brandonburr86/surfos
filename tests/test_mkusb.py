"""Tests for tools/mkusb.py, the USB image builder.

Run from the repository: python3 -m unittest discover -s tests -p test_mkusb.py   (make test-mkusb)

The refusal tests need nothing. The image tests need mtools and run in the Docker
toolchain (or anywhere mtools is installed); a stand-in BOOTX64.EFI keeps GRUB out of
it. The images are inspected with the builder's own GPT and FAT32 readers and, when
present, with mtools as an independent second opinion.
"""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('mkusb', REPO / 'tools' / 'mkusb.py')
mkusb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mkusb)
HAVE_MTOOLS = all(shutil.which(t) for t in ('mformat', 'mmd', 'mcopy', 'mdir'))


def fake_kernel(extra=b''):
    """a 32-bit x86 ELF header with a valid Multiboot 2 header at offset 0x1010"""
    data = bytearray(0x2000)
    data[0:4] = b'\x7fELF'
    data[4] = 1                                   # ELFCLASS32
    struct.pack_into('<H', data, 18, 3)           # EM_386
    magic, arch, length = 0xE85250D6, 0, 24
    struct.pack_into('<IIII', data, 0x1010, magic, arch, length, (-(magic + arch + length)) & 0xFFFFFFFF)
    return bytes(data) + extra


class MkusbTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='surfos mkusb ')   # a space, like the project path
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)
        self.kernel = self.dir / 'surfos.bin'
        self.kernel.write_bytes(fake_kernel(b'kernel one'))
        self.initrd = self.dir / 'initrd.tar'
        self.initrd.write_bytes(b'initrd one' * 1000)
        self.efi = self.dir / 'BOOTX64.EFI'
        self.efi.write_bytes(b'MZ stand-in loader' + bytes(4096))
        self.cfg = REPO / 'boot' / 'grub-usb.cfg'
        self.out = self.dir / 'out' / 'surfos-ideapad-15iil05.img'
        self.out.parent.mkdir()

    def build(self, *extra, kernel=None, initrd=None, output=None):
        args = ['--kernel', str(kernel or self.kernel), '--initrd', str(initrd or self.initrd),
                '--output', str(output or self.out), '--grub-efi', str(self.efi), '--grub-cfg', str(self.cfg),
                '--optimization', 'O2', '--source-rev', 'abc123', '--source-dirty', 'no'] + list(extra)
        return mkusb.main(args)

    # ---- refusals: no tools needed ----

    def test_rejects_missing_inputs(self):
        self.assertEqual(self.build(kernel=self.dir / 'nothing.bin'), 1)
        self.assertEqual(self.build(initrd=self.dir / 'nothing.tar'), 1)
        self.assertFalse(self.out.exists())

    def test_rejects_empty_inputs(self):
        empty = self.dir / 'empty'
        empty.write_bytes(b'')
        self.assertEqual(self.build(kernel=empty), 1)
        self.assertEqual(self.build(initrd=empty), 1)

    def test_rejects_a_kernel_without_multiboot2(self):
        bad = self.dir / 'old.bin'
        data = bytearray(fake_kernel())
        data[0x1010:0x1014] = b'\0\0\0\0'
        bad.write_bytes(bytes(data))
        self.assertEqual(self.build(kernel=bad), 1)

    def test_rejects_device_outputs(self):
        self.assertEqual(self.build(output='/dev/null'), 1)
        self.assertEqual(self.build(output='/dev/disk2'), 1)
        link = self.dir / 'looks-like-a-file.img'
        link.symlink_to('/dev/null')
        self.assertEqual(self.build(output=link), 1)
        self.assertEqual(self.build(output=self.dir / 'out'), 1)              # a directory

    # ---- real images: mtools ----

    @unittest.skipUnless(HAVE_MTOOLS, 'mtools is not installed (run in the Docker toolchain)')
    def test_image_layout_files_and_manifest(self):
        self.assertEqual(self.build(), 0)
        size = self.out.stat().st_size
        self.assertEqual(size, 256 * 1024 * 1024)
        with open(self.out, 'rb') as f:
            mbr = f.read(512)
            self.assertEqual(mbr[510:512], b'\x55\xaa')
            self.assertEqual(mbr[446 + 4], 0xEE)                             # protective MBR
            part = mkusb.read_gpt(f, size // 512)                            # both GPT copies, CRCs
            self.assertEqual(part['type_guid'], 'C12A7328-F81F-11D2-BA4B-00A0C93EC93B')
            self.assertEqual(part['first_lba'] * 512, 1024 * 1024)            # 1 MB
            self.assertEqual((part['last_lba'] + 1) * 512, 255 * 1024 * 1024)  # to 255 MB
            fat = mkusb.Fat32(f, part['first_lba'] * 512)
            self.assertEqual(fat.label, 'SURFOSUSB')
            self.assertEqual(fat.read('/boot/surfos.bin'), self.kernel.read_bytes())
            self.assertEqual(fat.read('/boot/initrd.tar'), self.initrd.read_bytes())
            self.assertEqual(fat.read('/EFI/BOOT/BOOTX64.EFI'), self.efi.read_bytes())
            self.assertEqual(fat.read('/boot/grub/grub.cfg'), self.cfg.read_bytes())
            info = fat.read('/boot/BUILD-INFO.txt').decode()
            self.assertIn('Source revision: abc123', info)
            self.assertIn('Optimization: O2', info)
        manifest = json.loads((self.out.parent / 'build-manifest.json').read_text())
        image_sha = hashlib.sha256(self.out.read_bytes()).hexdigest()
        self.assertEqual(manifest['image_sha256'], image_sha)
        self.assertEqual((self.out.parent / (self.out.name + '.sha256')).read_text(), f'{image_sha}  {self.out.name}\n')
        self.assertEqual(manifest['optimization'], 'O2')
        self.assertEqual(manifest['source'], {'revision': 'abc123', 'dirty': False})
        self.assertEqual(manifest['files']['/boot/surfos.bin']['sha256'],
                         hashlib.sha256(self.kernel.read_bytes()).hexdigest())
        # mtools reads the same files
        listing = subprocess.run(['mdir', '-i', f'{self.out}@@1048576', '-/', '-b', '::'],
                                 env=mkusb.MTOOLS_ENV, capture_output=True, text=True).stdout
        for name in ('BOOTX64.EFI', 'grub.cfg', 'surfos.bin', 'initrd.tar', 'BUILD-INFO.txt'):
            self.assertIn(name.lower(), listing.lower())
        self.assertFalse([p for p in self.out.parent.iterdir() if p.name.startswith('.mkusb-')])

    @unittest.skipUnless(HAVE_MTOOLS, 'mtools is not installed (run in the Docker toolchain)')
    def test_rebuild_takes_the_new_components(self):
        self.assertEqual(self.build(), 0)
        self.kernel.write_bytes(fake_kernel(b'kernel two'))
        self.initrd.write_bytes(b'initrd two' * 2000)
        self.assertEqual(self.build(), 0)
        with open(self.out, 'rb') as f:
            fat = mkusb.Fat32(f, 2048 * 512)
            self.assertTrue(fat.read('/boot/surfos.bin').endswith(b'kernel two'))
            self.assertEqual(fat.read('/boot/initrd.tar'), b'initrd two' * 2000)

    @unittest.skipUnless(HAVE_MTOOLS, 'mtools is not installed (run in the Docker toolchain)')
    def test_failure_keeps_the_previous_image(self):
        self.assertEqual(self.build(), 0)
        before = self.out.read_bytes()
        sums = [(p.name, p.read_bytes()) for p in sorted(self.out.parent.iterdir())]
        self.assertEqual(self.build('--mformat', 'false'), 1)                # a failing tool
        self.assertEqual(self.build('--size-mib', '64', initrd=self._big_initrd()), 1)   # does not fit
        self.assertEqual(self.out.read_bytes(), before)
        self.assertEqual([(p.name, p.read_bytes()) for p in sorted(self.out.parent.iterdir())], sums)

    def _big_initrd(self):
        big = self.dir / 'big.tar'
        with open(big, 'wb') as f:
            f.truncate(80 * 1024 * 1024)
        return big

    @unittest.skipUnless(HAVE_MTOOLS, 'mtools is not installed (run in the Docker toolchain)')
    def test_smallest_image_is_still_fat32(self):
        self.assertEqual(self.build('--size-mib', '64'), 0)
        with open(self.out, 'rb') as f:
            part = mkusb.read_gpt(f, self.out.stat().st_size // 512)
            fat = mkusb.Fat32(f, part['first_lba'] * 512)                 # refuses fewer than 65525 clusters
            self.assertEqual(fat.read('/boot/initrd.tar'), self.initrd.read_bytes())

    @unittest.skipUnless(HAVE_MTOOLS, 'mtools is not installed (run in the Docker toolchain)')
    def test_inspect(self):
        self.assertEqual(self.build(), 0)
        self.assertEqual(mkusb.main(['--inspect', str(self.out)]), 0)


if __name__ == '__main__':
    unittest.main()
