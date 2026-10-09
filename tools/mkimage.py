#!/usr/bin/env python3
"""Build the images SurfOS is tested with (roadmap D2).

    tools/mkimage.py disk   OUT.img SIZE_MB FILE...   raw disk: an MBR with one FAT partition starting at
                                                      block 2048, formatted with mtools and holding FILEs
    tools/mkimage.py initrd OUT.tar DIR                ustar archive of DIR: the Multiboot module that the
                                                      ramdisk driver exposes as rd0
    tools/mkimage.py foreign OUT.img                    a 64 MB "stick from another computer": MBR, one FAT32
                                                      partition made by mkfs.fat and filled by mtools with long,
                                                      accented and macOS-style names, folders and a 1 MB file
                                                      (the USB storage tests mount it in SurfOS)
    tools/mkimage.py initrd OUT.tar DIR --build-id REV  (10/9/26) the same, with DIR/etc/build-id written first:
                                                      "REV YYYY-MM-DDTHH:MM:SSZ", the revision and the UTC time

Needs python3 and mtools (mformat, mmd, mcopy) for the disk images, and dosfstools (mkfs.fat) for the
foreign one.
"""
import datetime
import os
import struct
import subprocess
import sys
import tarfile

PART_START = 2048          # blocks: 1 MB of slack before the partition, as every partitioner leaves
MTOOLS_ENV = dict(os.environ, MTOOLS_SKIP_CHECK='1')


def run(cmd):
    r = subprocess.run(cmd, env=MTOOLS_ENV, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode:
        sys.exit(f"{' '.join(cmd)}\n{r.stdout}")


def make_disk(out, size_mb, files):
    size = size_mb * 1024 * 1024
    part_blocks = size // 512 - PART_START
    offset = PART_START * 512
    tmp = out + '.tmp'
    with open(tmp, 'wb') as f:
        f.truncate(size)
    where = f'{tmp}@@{offset}'
    # 16 heads x 32 sectors: a geometry that divides every multiple of 256 KB, so mformat is happy
    # -c 2: two-sector clusters make a 15 MB volume FAT16; -H: the partition's offset for the BPB
    run(['mformat', '-i', where, '-T', str(part_blocks), '-h', '16', '-s', '32', '-c', '2', '-H', str(PART_START),
         '-v', 'SURFOS', '::'])
    run(['mmd', '-i', where, '::/DOCS'])
    for path in files:
        name = os.path.basename(path)
        dest = '::/DOCS/' + name if path.startswith('docs/') else '::/' + name
        run(['mcopy', '-i', where, '-o', path, dest])
    # the partition type follows what mformat chose
    with open(tmp, 'rb') as f:
        f.seek(offset)
        bs = f.read(512)
    if bs[82:87] == b'FAT32':
        ptype = 0x0C
    elif bs[54:59] == b'FAT12':
        ptype = 0x01
    else:
        ptype = 0x06
    mbr = bytearray(512)
    mbr[0:16] = b'SurfOS test disk'
    # entry 1: active, CHS fields set to "use LBA", type, start, size
    mbr[446:462] = struct.pack('<B3sB3sII', 0x80, b'\xfe\xff\xff', ptype, b'\xfe\xff\xff', PART_START, part_blocks)
    mbr[510:512] = b'\x55\xaa'
    with open(tmp, 'r+b') as f:
        f.write(mbr)
    os.replace(tmp, out)
    print(f"{out}: {size_mb} MB, partition 1 type {ptype:02x} at block {PART_START}, {part_blocks} blocks, "
          f"{len(files)} files")


# what the foreign stick holds: (path on the volume, content)
FOREIGN_FILES = [
    ('A long file name written elsewhere.txt', b'Written on another computer, long name and all.\n'),
    ('caf\u00e9 menu.txt', b'Accented name: the long-name entry is UTF-16 on disk.\n'),
    ('._A long file name written elsewhere.txt', b'\x00\x05\x16\x07AppleDouble stand-in\n'),
    ('.DS_Store', b'\x00\x00\x00\x01Bud1 stand-in\n'),
    ('Photos/2026/October/notes.txt', b'Three folders down.\n'),
    ('README.TXT', b'A plain 8.3 name.\n'),
]


def foreign_big():
    """the 1 MB file: 16384 lines of 64 bytes, numbered, so a wrong block shows"""
    return b''.join(b'SurfOS foreign stick, line %05d of 16384, 64 bytes per line...\n' % i for i in range(16384))


def make_foreign(out):
    size = 64 * 1024 * 1024
    part_blocks = size // 512 - PART_START
    offset = PART_START * 512
    tmp = out + '.tmp'
    part = out + '.part'
    with open(part, 'wb') as f:
        f.truncate(part_blocks * 512)
    # FAT32 by dosfstools, 512-byte clusters so a 63 MB volume can be FAT32, the partition's offset in the BPB
    run(['mkfs.fat', '-F', '32', '-s', '1', '-n', 'FOREIGN', '-h', str(PART_START), part])
    env = dict(MTOOLS_ENV, LC_ALL='C.UTF-8', LANG='C.UTF-8')
    work = out + '.files'
    os.makedirs(work, exist_ok=True)
    try:
        made = set()
        for name, data in FOREIGN_FILES + [('BIG.BIN', foreign_big())]:
            parts = name.split('/')
            for i in range(1, len(parts)):
                d = '/'.join(parts[:i])
                if d not in made:
                    r = subprocess.run(['mmd', '-i', part, '::/' + d], env=env, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, text=True)
                    if r.returncode:
                        sys.exit(r.stdout)
                    made.add(d)
            src = os.path.join(work, 'f%d' % len(os.listdir(work)))
            with open(src, 'wb') as f:
                f.write(data)
            r = subprocess.run(['mcopy', '-i', part, '-o', src, '::/' + name], env=env, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True)
            if r.returncode:
                sys.exit(r.stdout)
    finally:
        for n in os.listdir(work):
            os.remove(os.path.join(work, n))
        os.rmdir(work)
    mbr = bytearray(512)
    mbr[0:16] = b'SurfOS foreign  '
    mbr[440:444] = b'SFOS'                                        # disk signature
    mbr[446:462] = struct.pack('<B3sB3sII', 0x00, b'\xfe\xff\xff', 0x0C, b'\xfe\xff\xff', PART_START, part_blocks)
    mbr[510:512] = b'\x55\xaa'
    with open(tmp, 'wb') as f:
        f.write(mbr)
        f.truncate(offset)
        f.seek(offset)
        with open(part, 'rb') as p:
            while True:
                chunk = p.read(1 << 20)
                if not chunk:
                    break
                f.write(chunk)
    os.remove(part)
    os.replace(tmp, out)
    print(f"{out}: 64 MB, FAT32 'FOREIGN' at block {PART_START}, {len(FOREIGN_FILES) + 1} files")


def write_build_id(directory, rev):
    """10/9/26 (the development loop): etc/build-id, which tools/devloop.py compares after a reboot"""
    when = datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')
    os.makedirs(os.path.join(directory, 'etc'), exist_ok=True)
    with open(os.path.join(directory, 'etc', 'build-id'), 'w') as f:
        f.write(f'{rev} {when}\n')


def make_initrd(out, directory, build_rev=None):
    if build_rev is not None:
        write_build_id(directory, build_rev)
    tmp = out + '.tmp'
    with tarfile.open(tmp, 'w', format=tarfile.USTAR_FORMAT) as tar:
        for root, dirs, names in os.walk(directory):
            dirs.sort()
            for d in dirs:
                tar.add(os.path.join(root, d), arcname=os.path.relpath(os.path.join(root, d), directory), recursive=False)
            for n in sorted(names):
                tar.add(os.path.join(root, n), arcname=os.path.relpath(os.path.join(root, n), directory))
    os.replace(tmp, out)
    print(f"{out}: {os.path.getsize(out)} bytes from {directory}/")


def main(argv):
    if len(argv) >= 4 and argv[1] == 'disk':
        make_disk(argv[2], int(argv[3]), argv[4:])
    elif len(argv) == 4 and argv[1] == 'initrd':
        make_initrd(argv[2], argv[3])
    elif len(argv) == 6 and argv[1] == 'initrd' and argv[4] == '--build-id' and argv[5]:
        make_initrd(argv[2], argv[3], argv[5])
    elif len(argv) == 3 and argv[1] == 'foreign':
        make_foreign(argv[2])
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main(sys.argv)
