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
    tools/mkimage.py devstick OUT.img KERNEL INITRD    (10/9/26) a data stick for the development loop: MBR, one
                                                      FAT32 partition holding /surfos-dev/surfos.bin and
                                                      /surfos-dev/initrd.tar, which GRUB's development entry
                                                      boots (tools/qemu-usb.py's devloop scenario)

Needs python3 and mtools (mformat, mmd, mcopy) for the disk images, and dosfstools (mkfs.fat) for the
foreign one. The data stick needs nothing else: make_fat32() below writes FAT32 itself.
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


# ---- 10/9/26: a FAT32 data stick written here, for the development loop ------------------------------------
#
# The layout Microsoft's FAT specification gives (and mkfs.fat makes): 32 reserved sectors (the boot sector,
# FSInfo at 1, their copies at 6 and 7), two FATs, the data region with the root directory at cluster 2. Every
# file and directory is one run of clusters; names that are not upper-case 8.3 get long-name entries (UTF-16,
# checksummed against a ~n alias) as Windows and macOS write them, so GRUB and SurfOS read them by their names.
# Nothing here needs mtools, so the USB tests can make a stick on any host.

FAT_EOC = 0x0FFFFFFF
DOS_STAMP = ((2026 - 1980) << 9 | 10 << 5 | 9, 12 << 11)      # 2026-10-09 12:00:00: every entry's time


def short_alias(name, taken):
    """(11-byte short name, whether long-name entries are needed) for name in a directory whose short names are
    in taken (a set, which gets the new one)"""
    allowed = set(b"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!#$%&'()-@^_`{}~")
    base, dot, ext = name.rpartition('.') if '.' in name.lstrip('.') else (name, '', '')
    raw = name.encode('ascii', 'replace')
    plain = (raw.upper() == raw and 0 < len(base) <= 8 and len(ext) <= 3 and
             all(c in allowed for c in (base + ext).encode('ascii', 'replace')))
    if plain:
        short = base.encode().ljust(8) + ext.encode().ljust(3)
        if short not in taken:
            taken.add(short)
            return short, False
    b = bytes(c for c in base.upper().encode('ascii', 'replace') if c in allowed) or b'X'
    e = bytes(c for c in ext.upper().encode('ascii', 'replace') if c in allowed)[:3]
    for n in range(1, 1000000):
        tail = b'~%d' % n
        short = (b[:8 - len(tail)] + tail).ljust(8) + e.ljust(3)
        if short not in taken:
            taken.add(short)
            return short, True
    raise ValueError(f'no short name left for {name}')


def lfn_entries(name, short):
    """the long-name entries for name, in the order they precede the short entry (the last part first)"""
    csum = 0
    for c in short:
        csum = (((csum & 1) << 7) + (csum >> 1) + c) & 0xFF
    units = [name.encode('utf-16-le')[i:i + 2] for i in range(0, len(name.encode('utf-16-le')), 2)]
    count = (len(units) + 12) // 13
    if len(units) % 13:
        units += [b'\0\0'] + [b'\xff\xff'] * (13 - len(units) % 13 - 1)
    out = []
    for seq in range(count, 0, -1):
        part = units[(seq - 1) * 13:seq * 13]
        e = bytearray(32)
        e[0] = seq | (0x40 if seq == count else 0)
        e[1:11] = b''.join(part[0:5])
        e[11] = 0x0F
        e[13] = csum
        e[14:26] = b''.join(part[5:11])
        e[28:32] = b''.join(part[11:13])
        out.append(bytes(e))
    return out


def dir_entry(short, attr, cluster, size):
    date, time_ = DOS_STAMP
    return struct.pack('<11sBBBHHHHHHHI', short, attr, 0, 0, time_, date, date, cluster >> 16, time_, date,
                       cluster & 0xFFFF, size)


def make_fat32(out, files, size_mb=512, label='SURFDEV', cluster_bytes=4096):
    """a disk image: an MBR whose partition 1 (type 0c, from block PART_START to the end) holds a FAT32 volume
    with files, {'dir/name': bytes}; the image is sparse and only what is used is written"""
    sector = 512
    total = size_mb * 1024 * 1024 // sector
    part_sectors = total - PART_START
    spc, reserved, nfats = cluster_bytes // sector, 32, 2
    per = (256 * spc + nfats) // 2
    fatsz = (part_sectors - reserved + per - 1) // per
    data_start = reserved + nfats * fatsz
    nclusters = (part_sectors - data_start) // spc
    if nclusters < 65525:
        raise ValueError(f'{size_mb} MB with {cluster_bytes}-byte clusters is too small for FAT32')

    # the tree: a directory is {name: dict or bytes}
    root = {}
    for path, data in sorted(files.items()):
        node, parts = root, [p for p in path.split('/') if p]
        for d in parts[:-1]:
            node = node.setdefault(d, {})
        node[parts[-1]] = data

    fat = [0x0FFFFFF8, FAT_EOC, FAT_EOC]            # the media byte, a reserved entry, the root (cluster 2)
    room = {2: cluster_bytes}                       # a chain's first cluster: the bytes the chain holds
    todo = []                                       # (first cluster, its bytes, or (node, parent) for a directory)

    def alloc(nbytes):
        n = max(1, -(-nbytes // cluster_bytes))
        first = len(fat)
        fat.extend(range(first + 1, first + n))
        fat.append(FAT_EOC)
        room[first] = n * cluster_bytes
        return first

    def entries_of(node, own, parent):
        """a directory's entries; its children get their clusters now"""
        taken, ents = set(), []
        if own != 2:
            ents.append(dir_entry(b'.          ', 0x10, own, 0))
            ents.append(dir_entry(b'..         ', 0x10, 0 if parent == 2 else parent, 0))
        elif label:
            ents.append(dir_entry(label.upper().encode()[:11].ljust(11), 0x08, 0, 0))
        for name in sorted(node):
            short, long_ = short_alias(name, taken)
            if long_:
                ents.extend(lfn_entries(name, short))
            child = node[name]
            if isinstance(child, dict):
                # room for ".", ".." and each entry with its long name, however the aliases turn out
                first = alloc(32 * (2 + sum(2 + len(c) // 13 for c in child)))
                todo.append((first, (child, own)))
                ents.append(dir_entry(short, 0x10, first, 0))
            else:
                first = alloc(len(child)) if child else 0
                if child:
                    todo.append((first, child))
                ents.append(dir_entry(short, 0x20, first, len(child)))
        return b''.join(ents)

    contents = {2: entries_of(root, 2, 0)}
    i = 0
    while i < len(todo):                            # a directory's children join the end of the list
        first, what = todo[i]
        contents[first] = entries_of(what[0], first, what[1]) if isinstance(what, tuple) else what
        i += 1
    for first, data in contents.items():
        if len(data) > room[first]:
            raise ValueError('a directory has more entries than its clusters hold')
    used = len(fat) - 2
    if used > nclusters:
        raise ValueError('the files do not fit')

    offset = PART_START * sector
    bs = bytearray(sector)
    bs[0:3] = b'\xeb\x58\x90'
    bs[3:11] = b'SURFOS  '
    struct.pack_into('<HBHBHHBHHHII', bs, 11, sector, spc, reserved, nfats, 0, 0, 0xF8, 0, 32, 64, PART_START,
                     part_sectors)
    struct.pack_into('<IHHIHH', bs, 36, fatsz, 0, 0, 2, 1, 6)
    bs[64], bs[66] = 0x80, 0x29
    bs[67:71] = struct.pack('<I', 0x5D0F0901)
    bs[71:82] = label.upper().encode()[:11].ljust(11)
    bs[82:90] = b'FAT32   '
    bs[510:512] = b'\x55\xaa'
    fsinfo = bytearray(sector)
    struct.pack_into('<I', fsinfo, 0, 0x41615252)
    struct.pack_into('<III', fsinfo, 484, 0x61417272, nclusters - used, len(fat))
    struct.pack_into('<I', fsinfo, 508, 0xAA550000)
    mbr = bytearray(sector)
    mbr[0:16] = b'SurfOS data     '
    mbr[440:444] = b'SDEV'
    mbr[446:462] = struct.pack('<B3sB3sII', 0x00, b'\xfe\xff\xff', 0x0C, b'\xfe\xff\xff', PART_START, part_sectors)
    mbr[510:512] = b'\x55\xaa'
    fat_bytes = struct.pack('<%dI' % len(fat), *fat)

    tmp = out + '.tmp'
    with open(tmp, 'wb') as f:
        f.truncate(total * sector)
        f.write(mbr)
        for at, block in ((0, bs), (1, fsinfo), (6, bs), (7, fsinfo)):
            f.seek(offset + at * sector)
            f.write(block)
        for n in range(nfats):
            f.seek(offset + (reserved + n * fatsz) * sector)
            f.write(fat_bytes)
        for first, data in contents.items():
            f.seek(offset + (data_start + (first - 2) * spc) * sector)
            f.write(data)
    os.replace(tmp, out)
    return {'clusters': nclusters, 'used': used, 'cluster_bytes': cluster_bytes, 'offset': offset}


def make_devstick(out, kernel, initrd, size_mb=512):
    """10/9/26: a data stick holding a development build, as tools/devloop.py leaves one"""
    info = make_fat32(out, {'surfos-dev/surfos.bin': kernel, 'surfos-dev/initrd.tar': initrd}, size_mb)
    print(f"{out}: {size_mb} MB, FAT32 'SURFDEV' at block {PART_START}, /surfos-dev/surfos.bin ({len(kernel)} bytes) "
          f"and /surfos-dev/initrd.tar ({len(initrd)} bytes)")
    return info


def main(argv):
    if len(argv) >= 4 and argv[1] == 'disk':
        make_disk(argv[2], int(argv[3]), argv[4:])
    elif len(argv) == 4 and argv[1] == 'initrd':
        make_initrd(argv[2], argv[3])
    elif len(argv) == 6 and argv[1] == 'initrd' and argv[4] == '--build-id' and argv[5]:
        make_initrd(argv[2], argv[3], argv[5])
    elif len(argv) == 3 and argv[1] == 'foreign':
        make_foreign(argv[2])
    elif len(argv) == 5 and argv[1] == 'devstick':
        with open(argv[3], 'rb') as k, open(argv[4], 'rb') as i:
            make_devstick(argv[2], k.read(), i.read())
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main(sys.argv)
