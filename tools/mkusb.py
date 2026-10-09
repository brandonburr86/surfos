#!/usr/bin/env python3
"""Build the raw USB disk image that boots SurfOS on a UEFI PC (the Lenovo IdeaPad 3 15IIL05).

    tools/mkusb.py --kernel build/O2/surfos.bin --initrd build/O2/initrd.tar \\
                   --output build/usb/surfos-ideapad-15iil05.img [--optimization O2]
    tools/mkusb.py --inspect build/usb/surfos-ideapad-15iil05.img

The image is a whole disk, written to a USB drive with a raw-image writer:

    GPT (primary at the start, backup at the end), one partition from 1 MB to 1 MB before
    the end: the EFI system partition, FAT32, label SURFOSUSB, holding
        /EFI/BOOT/BOOTX64.EFI   standalone GRUB for x86_64 UEFI (the removable-media path)
        /boot/grub/grub.cfg     the menu: multiboot2 + module2 with profile=laptop
        /boot/grub/x86_64-efi/fs.lst  (10/9/26) empty: GRUB loads no file system modules (it has
                                none on the drive) when it looks at a disk that is not FAT
        /boot/surfos.bin        the kernel
        /boot/initrd.tar        the user programs and rootfs/, ramdisk rd0
        /boot/BUILD-INFO.txt    what was built, from what, when

Only regular files are written: the output must not be a device, and nothing here touches
a disk, a boot entry or the machine's firmware. The image is assembled under a temporary
name next to the output, read back and checked (GPT checksums, every file's SHA-256 through
an independent FAT32 reader), and only then renamed over the output; the .sha256 file and
build-manifest.json are written after that. Any failure leaves the previous image as it was.

Needs python3, mtools (mformat, mmd, mcopy) and, unless --grub-efi supplies a prebuilt
loader, grub-mkstandalone with GRUB's x86_64-efi files (Debian: grub-efi-amd64-bin).
"""
import argparse
import datetime
import hashlib
import json
import os
import platform
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import uuid
import zlib

SECTOR = 512
ESP_TYPE = uuid.UUID('C12A7328-F81F-11D2-BA4B-00A0C93EC93B')   # EFI system partition
GPT_ENTRIES = 128
GPT_ENTRY_SIZE = 128
LABEL = 'SURFOSUSB'
DEFAULT_SIZE_MIB = 256
ALIGN_SECTORS = 2048                                              # 1 MB
TARGET = 'Lenovo IdeaPad 3 15IIL05 (machine type 81WE), x86-64 UEFI, Secure Boot disabled'

# GRUB modules built into BOOTX64.EFI (and nothing else: no fonts, themes or locales). Video is
# the firmware's GOP (or UGA) only, as on real hardware: no native drivers such as video_bochs,
# which QEMU's VGA would otherwise get and a laptop never does.
GRUB_MODULES = ['part_gpt', 'part_msdos', 'fat', 'normal', 'configfile', 'search', 'search_fs_file',
                'search_label', 'regexp', 'test', 'echo', 'sleep', 'reboot', 'halt', 'multiboot2', 'boot',
                'efi_gop', 'efi_uga', 'video', 'video_fb', 'efifwsetup', 'ls', 'cat', 'minicmd']

MTOOLS_ENV = dict(os.environ, MTOOLS_SKIP_CHECK='1', MTOOLSRC='/dev/null')


class BuildError(Exception):
    pass


def run(cmd, what, env=None):
    try:
        r = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    except FileNotFoundError:
        raise BuildError(f'{what}: {cmd[0]} is not installed')
    if r.returncode:
        raise BuildError(f'{what} failed ({" ".join(cmd)}):\n{r.stdout.strip()}')
    return r.stdout


def tool_version(cmd):
    try:
        out = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=20).stdout
        return out.strip().splitlines()[0] if out.strip() else 'unknown'
    except Exception:
        return 'not available'


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


# ---- inputs and output -------------------------------------------------------------------

def check_input(path, what):
    try:
        st = os.stat(path)
    except OSError:
        raise BuildError(f'{what} {path} does not exist')
    if not stat.S_ISREG(st.st_mode):
        raise BuildError(f'{what} {path} is not a regular file')
    if st.st_size == 0:
        raise BuildError(f'{what} {path} is empty')


def multiboot2_header(data):
    """offset of a valid Multiboot 2 header in the first 32 KB, or None"""
    for off in range(0, min(len(data), 32768) - 16 + 1, 8):
        magic, arch, length, checksum = struct.unpack_from('<IIII', data, off)
        if magic == 0xE85250D6 and (magic + arch + length + checksum) & 0xFFFFFFFF == 0:
            return off
    return None


def check_kernel(path):
    with open(path, 'rb') as f:
        head = f.read(65536)
    if head[:4] != b'\x7fELF' or head[4] != 1 or struct.unpack_from('<H', head, 18)[0] != 3:
        raise BuildError(f'kernel {path} is not a 32-bit x86 ELF file')
    if multiboot2_header(head) is None:
        raise BuildError(f'kernel {path} has no Multiboot 2 header in its first 32 KB')


def check_output(path):
    """the output must be (or become) a regular file, never a device"""
    real = os.path.realpath(path)
    if real.startswith('/dev/') or real == '/dev':
        raise BuildError(f'refusing to write to {path}: this tool only writes image files, never devices')
    if os.path.lexists(path):
        st = os.stat(real)
        if not stat.S_ISREG(st.st_mode):
            raise BuildError(f'refusing to write to {path}: it exists and is not a regular file')
    parent = os.path.dirname(os.path.abspath(path))
    if not os.path.isdir(parent):
        raise BuildError(f'output directory {parent} does not exist')


# ---- GPT ---------------------------------------------------------------------------------

def gpt_header(my_lba, alt_lba, entries_lba, total, disk_guid, entries_crc):
    first_usable = 2 + GPT_ENTRIES * GPT_ENTRY_SIZE // SECTOR
    last_usable = total - 2 - GPT_ENTRIES * GPT_ENTRY_SIZE // SECTOR
    fields = [b'EFI PART', 0x00010000, 92, 0, 0, my_lba, alt_lba, first_usable, last_usable,
              disk_guid.bytes_le, entries_lba, GPT_ENTRIES, GPT_ENTRY_SIZE, entries_crc]
    hdr = struct.pack('<8sIIIIQQQQ16sQIII', *fields)
    crc = zlib.crc32(hdr) & 0xFFFFFFFF
    hdr = hdr[:16] + struct.pack('<I', crc) + hdr[20:]
    return hdr + bytes(SECTOR - len(hdr))


def write_gpt(f, total, first, last, disk_guid, part_guid):
    entry = struct.pack('<16s16sQQQ72s', ESP_TYPE.bytes_le, part_guid.bytes_le, first, last, 0,
                        LABEL.encode('utf-16-le').ljust(72, b'\0'))
    entries = entry + bytes(GPT_ENTRIES * GPT_ENTRY_SIZE - len(entry))
    ecrc = zlib.crc32(entries) & 0xFFFFFFFF
    entry_sectors = len(entries) // SECTOR

    # protective MBR: one partition of type 0xEE covering the disk, not bootable (UEFI 5.2.3)
    mbr = bytearray(SECTOR)
    mbr[446:462] = struct.pack('<B3sB3sII', 0x00, b'\x00\x02\x00', 0xEE, b'\xff\xff\xff', 1,
                               min(total - 1, 0xFFFFFFFF))
    mbr[510:512] = b'\x55\xaa'
    f.seek(0)
    f.write(mbr)
    f.write(gpt_header(1, total - 1, 2, total, disk_guid, ecrc))
    f.write(entries)
    f.seek((total - 1 - entry_sectors) * SECTOR)
    f.write(entries)
    f.write(gpt_header(total - 1, 1, total - 1 - entry_sectors, total, disk_guid, ecrc))


def read_gpt(f, total):
    """parse and check both GPT copies; returns the first partition entry as a dict"""
    def header(lba):
        f.seek(lba * SECTOR)
        h = f.read(92)
        (sig, rev, size, crc, _, my, alt, first_usable, last_usable, dguid, elba, n, esize,
         ecrc) = struct.unpack('<8sIIIIQQQQ16sQIII', h)
        check = zlib.crc32(h[:16] + b'\0\0\0\0' + h[20:]) & 0xFFFFFFFF
        if sig != b'EFI PART' or size != 92 or crc != check or my != lba:
            raise BuildError(f'GPT header at LBA {lba} is invalid')
        f.seek(elba * SECTOR)
        entries = f.read(n * esize)
        if zlib.crc32(entries) & 0xFFFFFFFF != ecrc:
            raise BuildError(f'GPT partition entries of the header at LBA {lba} fail their CRC')
        return alt, uuid.UUID(bytes_le=dguid), entries, first_usable, last_usable
    alt, disk_guid, entries, first_usable, last_usable = header(1)
    if alt != total - 1:
        raise BuildError('the primary GPT header does not point at the last sector')
    _, disk_guid2, entries2, _, _ = header(total - 1)
    if entries2 != entries or disk_guid2 != disk_guid:
        raise BuildError('the backup GPT differs from the primary')
    tguid, pguid, first, last, attrs, name = struct.unpack_from('<16s16sQQQ72s', entries, 0)
    return {'type_guid': str(uuid.UUID(bytes_le=tguid)).upper(), 'unique_guid': str(uuid.UUID(bytes_le=pguid)).upper(),
            'first_lba': first, 'last_lba': last, 'name': name.decode('utf-16-le').rstrip('\0'),
            'disk_guid': str(disk_guid).upper(), 'first_usable_lba': first_usable, 'last_usable_lba': last_usable}


# ---- an independent FAT32 reader, to check what mtools wrote --------------------------------

class Fat32:
    def __init__(self, f, offset):
        self.f, self.offset = f, offset
        f.seek(offset)
        bs = f.read(SECTOR)
        if bs[510:512] != b'\x55\xaa' or bs[82:90] != b'FAT32   ':
            raise BuildError('the system partition does not start with a FAT32 boot sector')
        self.bps = struct.unpack_from('<H', bs, 11)[0]
        self.spc = bs[13]
        reserved = struct.unpack_from('<H', bs, 14)[0]
        nfats = bs[16]
        self.total = struct.unpack_from('<I', bs, 32)[0]
        fatsz = struct.unpack_from('<I', bs, 36)[0]
        self.root = struct.unpack_from('<I', bs, 44)[0]
        self.label = bs[71:82].decode('ascii', 'replace').rstrip()
        self.fat = offset + reserved * self.bps
        self.data = offset + (reserved + nfats * fatsz) * self.bps
        self.csize = self.bps * self.spc
        self.clusters = (self.total - reserved - nfats * fatsz) // self.spc
        if self.bps != SECTOR or not self.spc or self.clusters < 65525:
            raise BuildError('the system partition is not a valid FAT32 volume')

    def chain(self, cluster):
        seen = 0
        while 2 <= cluster < 0x0FFFFFF8:
            yield cluster
            seen += 1
            if seen > self.clusters:
                raise BuildError('a FAT chain loops')
            self.f.seek(self.fat + cluster * 4)
            cluster = struct.unpack('<I', self.f.read(4))[0] & 0x0FFFFFFF

    def read_chain(self, cluster, size=None):
        out = bytearray()
        for c in self.chain(cluster):
            self.f.seek(self.data + (c - 2) * self.csize)
            out += self.f.read(self.csize)
            if size is not None and len(out) >= size:
                break
        return bytes(out if size is None else out[:size])

    def entries(self, cluster):
        """(name, is_dir, first cluster, size, disk offsets of its directory entries)"""
        lfn, offsets = {}, []
        for c in self.chain(cluster):
            base = self.data + (c - 2) * self.csize
            self.f.seek(base)
            data = self.f.read(self.csize)
            for i in range(0, len(data), 32):
                e = data[i:i + 32]
                if e[0] == 0:
                    return
                if e[0] == 0xE5:
                    lfn, offsets = {}, []
                    continue
                if e[11] == 0x0F:
                    lfn[e[0] & 0x1F] = (e[1:11] + e[14:26] + e[28:32]).decode('utf-16-le', 'replace')
                    offsets.append(base + i)
                    continue
                if e[11] & 0x08:
                    lfn, offsets = {}, []
                    continue
                short = e[0:8].decode('ascii', 'replace').rstrip()
                ext = e[8:11].decode('ascii', 'replace').rstrip()
                name = short + ('.' + ext if ext else '')
                if lfn:
                    name = ''.join(lfn[k] for k in sorted(lfn)).split('\0')[0]
                first = (struct.unpack_from('<H', e, 20)[0] << 16) | struct.unpack_from('<H', e, 26)[0]
                yield name, bool(e[11] & 0x10), first, struct.unpack_from('<I', e, 28)[0], offsets + [base + i]
                lfn, offsets = {}, []

    def remove(self, path):
        """mark a file's directory entries deleted (for test images; the clusters stay allocated)"""
        parent, _, leaf = path.rpartition('/')
        cluster = self.root
        for part in [p for p in parent.split('/') if p]:
            cluster = next(first for name, is_dir, first, _, _ in self.entries(cluster)
                           if is_dir and name.lower() == part.lower())
        for name, _, _, _, offsets in self.entries(cluster):
            if name.lower() == leaf.lower():
                for off in offsets:
                    self.f.seek(off)
                    self.f.write(b'\xe5')
                return
        raise BuildError(f'{path} is not in the image')

    def read(self, path):
        cluster = self.root
        parts = [p for p in path.split('/') if p]
        for n, part in enumerate(parts):
            for name, is_dir, first, size, _ in self.entries(cluster):
                if name.lower() == part.lower():
                    if n == len(parts) - 1:
                        if is_dir:
                            raise BuildError(f'{path} is a directory')
                        return self.read_chain(first, size) if size else b''
                    if not is_dir:
                        raise BuildError(f'{path}: {name} is not a directory')
                    cluster = first
                    break
            else:
                raise BuildError(f'{path} is not in the image')
        raise BuildError(f'{path}: not a file')


# ---- assembly ----------------------------------------------------------------------------

def build_grub_efi(dest, bootstrap_cfg, mkstandalone):
    run([mkstandalone, '-O', 'x86_64-efi', '-o', dest,
         '--modules=' + ' '.join(GRUB_MODULES), '--install-modules=' + ' '.join(GRUB_MODULES),
         '--locales=', '--fonts=', '--themes=',
         f'boot/grub/grub.cfg={bootstrap_cfg}'], 'grub-mkstandalone')
    check_input(dest, 'GRUB EFI loader')


def build_info(args, kernel_sha, initrd_sha, built):
    return (f'SurfOS USB boot image\n'
            f'Target: {TARGET}\n'
            f'Built: {built} (UTC)\n'
            f'Source revision: {args.source_rev}{" (uncommitted changes)" if args.source_dirty == "yes" else ""}\n'
            f'Optimization: {args.optimization}\n'
            f'Kernel /boot/surfos.bin sha256 {kernel_sha}\n'
            f'Initrd /boot/initrd.tar sha256 {initrd_sha}\n'
            f'Boot: GRUB (x86_64 UEFI) -> multiboot2 /boot/surfos.bin profile=laptop, module2 /boot/initrd.tar\n'
            f'This file is information only; nothing reads it.\n')


def assemble(args, tmp_image, staging):
    size = args.size_mib * 1024 * 1024
    total = size // SECTOR
    first = ALIGN_SECTORS
    last = total - ALIGN_SECTORS - 1
    part_sectors = last - first + 1
    offset = first * SECTOR

    files = {'/EFI/BOOT/BOOTX64.EFI': os.path.join(staging, 'BOOTX64.EFI'),
             '/boot/grub/grub.cfg': args.grub_cfg,
             '/boot/grub/x86_64-efi/fs.lst': os.path.join(staging, 'fs.lst'),
             '/boot/surfos.bin': args.kernel,
             '/boot/initrd.tar': args.initrd,
             '/boot/BUILD-INFO.txt': os.path.join(staging, 'BUILD-INFO.txt')}
    need = sum(os.path.getsize(p) for p in files.values())
    if need > part_sectors * SECTOR * 0.9:
        raise BuildError(f'the files need {need} bytes: increase --size-mib (now {args.size_mib})')

    disk_guid, part_guid = uuid.uuid4(), uuid.uuid4()
    with open(tmp_image, 'wb') as f:
        f.truncate(size)
        write_gpt(f, total, first, last, disk_guid, part_guid)

    where = f'{tmp_image}@@{offset}'
    serial = os.urandom(4).hex().upper()
    # FAT32 needs at least 65525 clusters: the largest cluster (up to 4 KB) that leaves a margin
    spc = 8
    while spc > 1 and part_sectors // spc < 70000:
        spc //= 2
    run([args.mformat, '-i', where, '-F', '-T', str(part_sectors), '-h', '64', '-s', '32', '-H', str(first),
         '-c', str(spc), '-v', LABEL, '-N', serial, '::'], 'mformat', MTOOLS_ENV)
    for d in ('::/EFI', '::/EFI/BOOT', '::/boot', '::/boot/grub', '::/boot/grub/x86_64-efi'):
        run([args.mmd, '-i', where, d], 'mmd', MTOOLS_ENV)
    for dest, src in files.items():
        run([args.mcopy, '-i', where, '-o', src, '::' + dest], 'mcopy', MTOOLS_ENV)

    # read everything back through the GPT and our own FAT32 reader
    layout_files = {}
    with open(tmp_image, 'rb') as f:
        part = read_gpt(f, total)
        if (part['type_guid'] != str(ESP_TYPE).upper() or part['first_lba'] != first or part['last_lba'] != last):
            raise BuildError('the partition entry does not describe the system partition')
        fat = Fat32(f, part['first_lba'] * SECTOR)
        if fat.label != LABEL:
            raise BuildError(f'the volume label is {fat.label!r}, not {LABEL}')
        for dest, src in files.items():
            got = hashlib.sha256(fat.read(dest)).hexdigest()
            want = sha256_file(src)
            if got != want:
                raise BuildError(f'{dest} in the image does not match {src}')
            layout_files[dest] = {'bytes': os.path.getsize(src), 'sha256': want}
    part.update({'filesystem': 'FAT32', 'label': LABEL, 'cluster_bytes': fat.csize, 'volume_serial': serial,
                 'offset_bytes': first * SECTOR, 'bytes': part_sectors * SECTOR})
    return {'partition_table': 'GPT', 'sector_bytes': SECTOR, 'disk_sectors': total, 'esp': part}, layout_files


def write_atomic(path, text):
    tmp = path + '.tmp'
    with open(tmp, 'w') as f:
        f.write(text)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def build(args):
    check_input(args.kernel, 'kernel')
    check_kernel(args.kernel)
    check_input(args.initrd, 'initrd')
    check_input(args.grub_cfg, 'GRUB configuration')
    if args.grub_efi:
        check_input(args.grub_efi, 'GRUB EFI loader')
    else:
        check_input(args.bootstrap_cfg, 'GRUB bootstrap configuration')
    check_output(args.output)

    outdir = os.path.dirname(os.path.abspath(args.output))
    staging = tempfile.mkdtemp(prefix='.mkusb-', dir=outdir)
    tmp_image = os.path.join(staging, 'image.tmp')        # same file system: the rename is atomic
    try:
        efi = os.path.join(staging, 'BOOTX64.EFI')
        if args.grub_efi:
            shutil.copyfile(args.grub_efi, efi)
        else:
            build_grub_efi(efi, args.bootstrap_cfg, args.grub_mkstandalone)
        built = datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')
        kernel_sha, initrd_sha = sha256_file(args.kernel), sha256_file(args.initrd)
        with open(os.path.join(staging, 'BUILD-INFO.txt'), 'w') as f:
            f.write(build_info(args, kernel_sha, initrd_sha, built))
        # 10/9/26: when the bootstrap sets $prefix to the drive's /boot/grub, GRUB reads its list of file system
        # modules to load on demand from here; empty, it tries none (the menu looks at every disk for a development
        # build, and a disk that is not FAT would otherwise print an error for each module it cannot find)
        open(os.path.join(staging, 'fs.lst'), 'w').close()
        layout, files = assemble(args, tmp_image, staging)
        with open(tmp_image, 'rb+') as f:
            os.fsync(f.fileno())
        os.replace(tmp_image, args.output)
    finally:
        shutil.rmtree(staging, ignore_errors=True)

    # only now, for the image that is actually in place
    image_sha = sha256_file(args.output)
    name = os.path.basename(args.output)
    checksum = args.checksum or args.output + '.sha256'
    manifest_path = args.manifest or os.path.join(outdir, 'build-manifest.json')
    write_atomic(checksum, f'{image_sha}  {name}\n')
    manifest = {
        'format': 'surfos-usb-manifest-1',
        'image': name,
        'image_sha256': image_sha,
        'image_bytes': os.path.getsize(args.output),
        'target': TARGET,
        'built_utc': built,
        'source': {'revision': args.source_rev,
                   'dirty': {'yes': True, 'no': False}.get(args.source_dirty)},
        'optimization': args.optimization,
        'boot': 'UEFI removable-media path /EFI/BOOT/BOOTX64.EFI -> GRUB -> multiboot2 /boot/surfos.bin '
                'profile=laptop, module2 /boot/initrd.tar',
        'layout': layout,
        'files': files,
        'components': {
            'kernel': {'source': args.kernel, 'sha256': kernel_sha},
            'initrd': {'source': args.initrd, 'sha256': initrd_sha},
            'grub_cfg': {'source': args.grub_cfg, 'sha256': sha256_file(args.grub_cfg)},
            'grub_efi': {'source': args.grub_efi or 'grub-mkstandalone (bootstrap ' + args.bootstrap_cfg + ')',
                         'sha256': files['/EFI/BOOT/BOOTX64.EFI']['sha256'], 'modules': GRUB_MODULES},
        },
        'tools': {
            'python': platform.python_version(),
            'grub-mkstandalone': 'prebuilt loader supplied' if args.grub_efi else tool_version([args.grub_mkstandalone, '--version']),
            'mformat': tool_version([args.mformat, '--version']),
            'gcc': tool_version(['gcc', '--version']),
            'ld': tool_version(['ld', '--version']),
            'nasm': tool_version(['nasm', '-v']),
        },
        'reproducible': False,
        'notes': 'GUIDs, the volume serial and FAT time stamps differ between builds, so two builds of the same '
                 'source have different image hashes; compare the files section instead.',
    }
    write_atomic(manifest_path, json.dumps(manifest, indent=2) + '\n')
    print(f'{args.output}: {manifest["image_bytes"] // (1024 * 1024)} MB GPT disk image, FAT32 system partition '
          f'{LABEL} at 1 MB, {len(files)} files, {args.optimization}')
    print(f'sha256 {image_sha}  ({checksum})')
    print(f'manifest {manifest_path}')
    return 0


def inspect(path):
    size = os.path.getsize(path)
    with open(path, 'rb') as f:
        part = read_gpt(f, size // SECTOR)
        fat = Fat32(f, part['first_lba'] * SECTOR)
        print(f'{path}: {size // (1024 * 1024)} MB, GPT disk {part["disk_guid"]}')
        print(f'  partition 1: {part["name"]}, type {part["type_guid"]}, LBA {part["first_lba"]}-{part["last_lba"]}')
        print(f'  FAT32 volume {fat.label}, {fat.csize}-byte clusters, {fat.clusters} clusters')
        for dest in ('/EFI/BOOT/BOOTX64.EFI', '/boot/grub/grub.cfg', '/boot/grub/x86_64-efi/fs.lst', '/boot/surfos.bin',
                     '/boot/initrd.tar', '/boot/BUILD-INFO.txt'):
            data = fat.read(dest)
            print(f'  {dest:30} {len(data):9} bytes  sha256 {hashlib.sha256(data).hexdigest()}')
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--kernel', help='the SurfOS kernel (ELF with a Multiboot 2 header)')
    ap.add_argument('--initrd', help='the initrd tar archive (ramdisk rd0)')
    ap.add_argument('--output', help='the image file to write (a regular file, never a device)')
    ap.add_argument('--grub-cfg', default='boot/grub-usb.cfg', help='the menu, copied to /boot/grub/grub.cfg')
    ap.add_argument('--bootstrap-cfg', default='boot/grub-usb-bootstrap.cfg',
                    help='configuration embedded in BOOTX64.EFI that finds the partition')
    ap.add_argument('--grub-efi', help='use this prebuilt BOOTX64.EFI instead of running grub-mkstandalone')
    ap.add_argument('--grub-mkstandalone', default='grub-mkstandalone')
    ap.add_argument('--mformat', default='mformat')
    ap.add_argument('--mmd', default='mmd')
    ap.add_argument('--mcopy', default='mcopy')
    ap.add_argument('--size-mib', type=int, default=DEFAULT_SIZE_MIB, help='image size (default 256)')
    ap.add_argument('--optimization', default='unknown', help='recorded in the manifest, for example O2')
    ap.add_argument('--source-rev', default='unknown', help='source revision recorded in the manifest')
    ap.add_argument('--source-dirty', default='unknown', choices=['yes', 'no', 'unknown'])
    ap.add_argument('--checksum', help='where to write the sha256 line (default: OUTPUT.sha256)')
    ap.add_argument('--manifest', help='where to write the manifest (default: build-manifest.json next to OUTPUT)')
    ap.add_argument('--inspect', metavar='IMAGE', help='check and list an existing image, write nothing')
    a = ap.parse_args(argv)
    try:
        if a.inspect:
            return inspect(a.inspect)
        if not (a.kernel and a.initrd and a.output):
            ap.error('--kernel, --initrd and --output are required')
        if a.size_mib < 64 or a.size_mib > 4096:
            raise BuildError('--size-mib must be between 64 and 4096')
        return build(a)
    except BuildError as e:
        print(f'mkusb: {e}', file=sys.stderr)
        print('mkusb: no image was written; any previous image is unchanged', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
