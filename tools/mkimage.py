#!/usr/bin/env python3
"""Build the images SurfOS is tested with (roadmap D2).

    tools/mkimage.py disk   OUT.img SIZE_MB FILE...   raw disk: an MBR with one FAT partition starting at
                                                      block 2048, formatted with mtools and holding FILEs
    tools/mkimage.py initrd OUT.tar DIR                ustar archive of DIR: the Multiboot module that the
                                                      ramdisk driver exposes as rd0

Needs python3 and mtools (mformat, mmd, mcopy) for the disk image.
"""
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


def make_initrd(out, directory):
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
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main(sys.argv)
