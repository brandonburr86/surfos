#!/usr/bin/env python3
"""SurfCPU card image builder
--------------------
File: mkcard.py Date: 10/15/26 (SurfCPU)
--------------------
Makes raw microSD card images (docs/MODULES.md section 5) laid out as docs/BOOT-ABI.md
section 5 says a card is: an MBR partition table with one FAT32 partition (type 0x0c by
default) and 512-byte sectors, holding the files and directories asked for. The images
are for SurfBoot's host tests, the firmware emulator and the RTL simulation; this tool only
ever writes regular files, never a device.

    tools/mkcard.py OUT.img [--size 64M] [--cluster 512|4K|...] [--part-start 2048]
                    [--part-type 0x0c] [--label SURFOS] [--file /BOOT/SURFOS.BIN=build/x.bin]...
                    [--dir /USERS/DOCUMENTS]... [--fragment none|interleave|reverse|random]
                    [--seed N] [--lfn] [--fill] [--standard-dirs]
    tools/mkcard.py --inspect IMG       the partition table, the boot sector and every file,
                                        read back with this tool's own reader

--fragment spreads files and multi-cluster directories over the disk: interleave takes one
cluster from each in turn, reverse links each object's clusters backwards, random draws
clusters in a seeded random order. --lfn adds a long-name entry (the lower-case name, as
macOS would store it) before each short entry. --fill fills every free cluster with
/FILL.DAT (a full volume). The default cluster size follows Microsoft's FAT32 table (512 B up
to 260 MB, 4 KiB to 8 GB, 8 KiB to 16 GB, 16 KiB to 32 GB, 32 KiB above); a volume with fewer
than 65525 clusters is refused (it would not be FAT32). The tests import this module: Card
builds images, Image reads and damages them. Only the Python standard library is used.
"""
import argparse
import random
import struct
import sys
from pathlib import Path

SECTOR = 512
DATE_2026_10_15 = ((2026 - 1980) << 9) | (10 << 5) | 15
TIME_NOON = 12 << 11
ATTR_RO, ATTR_HID, ATTR_SYS, ATTR_VOL, ATTR_DIR, ATTR_ARC, ATTR_LFN = 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x0F
EOC = 0x0FFFFFFF
STANDARD_DIRS = ("/BOOT", "/SYSTEM", "/PROGRAMS", "/USERS", "/USERS/DOCUMENTS")


def parse_size(text: str) -> int:
    t = text.strip().upper()
    mult = 1
    if t and t[-1] in "KMG":
        mult = {"K": 1 << 10, "M": 1 << 20, "G": 1 << 30}[t[-1]]
        t = t[:-1]
    return int(t, 0) * mult


def short_name(component: str) -> bytes:
    """The 11-byte directory name of an 8.3 component (upper case); ValueError otherwise."""
    if component in (".", ".."):
        return component.encode().ljust(11, b" ")
    base, _, ext = component.upper().partition(".")
    bad = set('"*+,./:;<=>?[\\]| ')
    if not base or len(base) > 8 or len(ext) > 3 or "." in ext or (component.endswith(".") and not ext):
        raise ValueError(f"{component!r} is not an 8.3 name")
    if any(c in bad or ord(c) < 0x21 or ord(c) > 0x7E for c in base + ext):
        raise ValueError(f"{component!r} has characters an 8.3 name cannot hold")
    raw = base.encode().ljust(8, b" ") + ext.encode().ljust(3, b" ")
    if raw[0] == 0xE5:
        raw = b"\x05" + raw[1:]
    return raw


def lfn_checksum(name11: bytes) -> int:
    s = 0
    for c in name11:
        s = ((((s & 1) << 7) | (s >> 1)) + c) & 0xFF
    return s


def is_83(component: str) -> bool:
    try:
        short_name(component)
        return True
    except ValueError:
        return False


def make_alias(long_name: str, taken: set[bytes]) -> bytes:
    """A short alias for a long name (BASIS~N.EXT, as Windows and macOS make them)."""
    keep = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!#$%&'()-@^_`{}~")
    up = long_name.upper().lstrip(".")
    base, dot, ext = up.rpartition(".") if "." in up else (up, "", "")
    base = "".join(c for c in base if c in keep) or "FILE"
    ext = "".join(c for c in ext if c in keep)[:3]
    for n in range(1, 1000000):
        tail = f"~{n}"
        cand = (base[:8 - len(tail)] + tail).encode().ljust(8, b" ") + ext.encode().ljust(3, b" ")
        if cand not in taken:
            return cand
    raise ValueError(f"no free short alias for {long_name!r}")


def lfn_entries(long_name: str, name11: bytes) -> list[bytes]:
    """VFAT long-name entries for long_name, in on-disk order (last part first)."""
    units = [ord(c) for c in long_name] + [0]
    while len(units) % 13:
        units.append(0xFFFF)
    if len(units) > 13 and units[len(long_name)] == 0 and len(long_name) % 13 == 0:
        units = units[:len(long_name)]
    parts = [units[i:i + 13] for i in range(0, len(units), 13)]
    csum = lfn_checksum(name11)
    out = []
    for seq in range(len(parts), 0, -1):
        u = parts[seq - 1]
        e = bytearray(32)
        e[0] = seq | (0x40 if seq == len(parts) else 0)
        for k in range(5):
            struct.pack_into("<H", e, 1 + 2 * k, u[k])
        e[11] = ATTR_LFN
        e[13] = csum
        for k in range(6):
            struct.pack_into("<H", e, 14 + 2 * k, u[5 + k])
        for k in range(2):
            struct.pack_into("<H", e, 28 + 2 * k, u[11 + k])
        out.append(bytes(e))
    return out


def dirent(name11: bytes, attr: int, cluster: int, size: int, ntres: int = 0) -> bytes:
    return struct.pack("<11sBBBHHHHHHHI", name11, attr, ntres, 0, TIME_NOON, DATE_2026_10_15, DATE_2026_10_15,
                       cluster >> 16, TIME_NOON, DATE_2026_10_15, cluster & 0xFFFF, size)


def default_cluster(volume_bytes: int) -> int:
    mb = volume_bytes / (1 << 20)
    if mb <= 260:
        return 512
    if mb <= 8 * 1024:
        return 4096
    if mb <= 16 * 1024:
        return 8192
    if mb <= 32 * 1024:
        return 16384
    return 32768


class Node:
    def __init__(self, path: str, is_dir: bool, data: bytes = b"", long_name: str | None = None):
        self.path, self.is_dir, self.data, self.long_name = path, is_dir, data, long_name
        self.children: list["Node"] = []
        self.clusters: list[int] = []
        self.parent: "Node | None" = None
        self.component = path.rsplit("/", 1)[1] if path != "/" else "/"
        self.name11 = b"/          "
        self.needs_lfn = False

    def place(self, parent: "Node"):
        """Decide the short name in parent: the 8.3 name itself, or an alias plus long-name entries."""
        taken = {ch.name11 for ch in parent.children}
        if is_83(self.component):
            self.name11 = short_name(self.component)
            self.needs_lfn = self.component != self.component.upper()
            if self.name11 in taken:
                raise ValueError(f"{self.path} clashes with another entry")
        else:
            self.name11 = make_alias(self.component, taken)
            self.needs_lfn = True
            self.long_name = self.long_name or self.component
        self.parent = parent
        parent.children.append(self)


class Card:
    """A card image to build: add directories and files, then build(path)."""

    def __init__(self, size: int = 64 << 20, cluster: int | None = None, part_start: int = 2048,
                 part_type: int = 0x0C, label: str = "SURFOS", fragment: str = "none", seed: int = 1,
                 lfn: bool = False, oem: str = "SURFCPU", reserved: int = 32, nfats: int = 2):
        self.size, self.part_start, self.part_type = size & ~(SECTOR - 1), part_start, part_type
        self.label, self.fragment, self.seed, self.lfn, self.oem = label, fragment, seed, lfn, oem
        self.reserved, self.nfats = reserved, nfats
        self.cluster = cluster or default_cluster(self.size - part_start * SECTOR)
        if self.cluster % SECTOR or self.cluster & (self.cluster - 1) or not SECTOR <= self.cluster <= 65536:
            raise ValueError(f"cluster size {self.cluster} must be a power of two from 512 to 65536")
        self.root = Node("/", True)
        self.nodes = {"/": self.root}
        self.fill = False

    def _node(self, path: str) -> Node:
        return self.nodes[path]

    def add_dir(self, path: str) -> Node:
        path = "/" + path.strip("/")
        if path in self.nodes:
            return self.nodes[path]
        parent = self.add_dir(path.rsplit("/", 1)[0] or "/") if path.count("/") > 1 else self.root
        n = Node(path, True)
        n.place(parent)
        self.nodes[path] = n
        return n

    def add_file(self, path: str, data: bytes, long_name: str | None = None) -> Node:
        path = "/" + path.strip("/")
        parent_path = path.rsplit("/", 1)[0] or "/"
        parent = self.add_dir(parent_path) if parent_path != "/" else self.root
        if path in self.nodes:
            raise ValueError(f"{path} added twice")
        n = Node(path, False, bytes(data), long_name)
        n.place(parent)
        self.nodes[path] = n
        return n

    # ---- layout ----
    def _geometry(self):
        total = self.size // SECTOR
        part_sectors = total - self.part_start
        spc = self.cluster // SECTOR
        fatsz = 1
        while True:
            clusters = (part_sectors - self.reserved - self.nfats * fatsz) // spc
            need = ((clusters + 2) * 4 + SECTOR - 1) // SECTOR
            if need <= fatsz:
                break
            fatsz = need
        if clusters < 65525:
            raise ValueError(f"{self.size} bytes with {self.cluster}-byte clusters give {clusters} clusters; "
                             "FAT32 needs at least 65525 (use a bigger image or smaller clusters)")
        if clusters > 0x0FFFFFF5:
            raise ValueError("too many clusters for FAT32")
        return part_sectors, spc, fatsz, clusters

    def _entries(self, n: Node) -> int:
        count = 0 if n is not self.root else (1 if self.label else 0)
        if n is not self.root:
            count += 2                                  # . and ..
        for ch in n.children:
            count += 1
            if self.lfn or ch.needs_lfn:
                count += len(lfn_entries(self._long(ch), ch.name11))
        return count

    def _long(self, n: Node) -> str:
        return n.long_name or (n.component if n.needs_lfn else n.component.lower())

    def _allocate(self, clusters: int):
        objs = []
        for n in self.nodes.values():
            if n.is_dir:
                need = max(1, (self._entries(n) * 32 + self.cluster - 1) // self.cluster)
            else:
                need = (len(n.data) + self.cluster - 1) // self.cluster
            objs.append((n, need))
        objs.sort(key=lambda o: (0 if o[0] is self.root else 1, not o[0].is_dir, o[0].path))
        free = list(range(2, clusters + 2))
        total = sum(k for _, k in objs)
        if total > clusters:
            raise ValueError(f"the files need {total} clusters; the volume has {clusters}")
        if self.fragment == "random":
            rng = random.Random(self.seed)
            window = free[:min(len(free), max(64, total * 3))]
            rest = free[len(window):]
            root_cl = window.pop(0)
            rng.shuffle(window)
            free = [root_cl] + window + rest
        pos = 0
        if self.fragment == "interleave":
            pending = [[n, k] for n, k in objs]
            # the root directory's first cluster is cluster 2, as BPB_RootClus says
            self.root.clusters.append(free[pos])
            pos += 1
            for p in pending:
                if p[0] is self.root:
                    p[1] -= 1
            while any(k for _, k in pending):
                for p in pending:
                    if p[1]:
                        p[0].clusters.append(free[pos])
                        pos += 1
                        p[1] -= 1
        else:
            for n, k in objs:
                n.clusters = free[pos:pos + k]
                pos += k
                if self.fragment == "reverse" and len(n.clusters) > 1 and n is not self.root:
                    n.clusters.reverse()
        self._used = pos
        return [c for c in free[pos:]]

    # ---- writing ----
    def build(self, path: str) -> dict:
        part_sectors, spc, fatsz, clusters = self._geometry()
        remaining = self._allocate(clusters)
        if self.fill and remaining:
            fill = self.add_file("/FILL.DAT", b"")
            fill.clusters = remaining
            fill.data = None                            # written as a pattern, cluster by cluster
            fill.fill_size = len(remaining) * self.cluster
            # the root directory gained an entry: it must still fit its clusters
            if self._entries(self.root) * 32 > len(self.root.clusters) * self.cluster:
                raise ValueError("--fill: no room for /FILL.DAT in the root directory")
        part_lba = self.part_start
        fat_lba = part_lba + self.reserved
        data_lba = fat_lba + self.nfats * fatsz
        fat = bytearray((clusters + 2) * 4)
        struct.pack_into("<II", fat, 0, 0x0FFFFFF8, 0x0FFFFFFF)
        for n in self.nodes.values():
            for i, c in enumerate(n.clusters):
                nxt = n.clusters[i + 1] if i + 1 < len(n.clusters) else EOC
                struct.pack_into("<I", fat, c * 4, nxt)
        used = sum(len(n.clusters) for n in self.nodes.values())
        with open(path, "wb") as f:
            f.truncate(self.size)

            def put(lba: int, data: bytes):
                f.seek(lba * SECTOR)
                f.write(data)

            # MBR
            mbr = bytearray(SECTOR)
            struct.pack_into("<I", mbr, 440, 0x53524643)            # disk signature "CFRS"
            mbr[446:462] = mbr_entry(0x00, self.part_type, part_lba, part_sectors)
            mbr[510:512] = b"\x55\xaa"
            put(0, bytes(mbr))
            # boot sector, FSInfo, backups
            bs = boot_sector(self.oem, spc, self.reserved, self.nfats, part_lba, part_sectors, fatsz,
                             self.label, 0x20261015)
            fsinfo = fsinfo_sector(clusters - used, (2 + used) if used < clusters else 0xFFFFFFFF)
            put(part_lba, bs)
            put(part_lba + 1, fsinfo)
            put(part_lba + 6, bs)
            put(part_lba + 7, fsinfo)
            for k in range(self.nfats):
                put(fat_lba + k * fatsz, bytes(fat))
            # directories
            for n in self.nodes.values():
                if n.is_dir:
                    raw = self._dir_bytes(n)
                    self._write_clusters(put, data_lba, spc, n.clusters, raw)
            # files
            for n in self.nodes.values():
                if not n.is_dir:
                    if n.data is None:                  # --fill: a recognisable pattern
                        for i, c in enumerate(n.clusters):
                            put(data_lba + (c - 2) * spc, struct.pack("<I", i) * (self.cluster // 4))
                    else:
                        self._write_clusters(put, data_lba, spc, n.clusters, n.data)
        files = {}
        for p, n in self.nodes.items():
            files[p] = {"clusters": list(n.clusters), "size": None if n.is_dir else
                        (getattr(n, "fill_size", None) if n.data is None else len(n.data)), "dir": n.is_dir}
        return {"part_lba": part_lba, "part_sectors": part_sectors, "fat_lba": fat_lba, "fat_sectors": fatsz,
                "data_lba": data_lba, "spc": spc, "cluster": self.cluster, "clusters": clusters, "nodes": files}

    def _write_clusters(self, put, data_lba, spc, clusters, data: bytes):
        for i, c in enumerate(clusters):
            chunk = data[i * self.cluster:(i + 1) * self.cluster]
            if chunk:
                put(data_lba + (c - 2) * spc, chunk)

    def _dir_bytes(self, n: Node) -> bytes:
        out = bytearray()
        if n is self.root:
            if self.label:
                out += dirent(self.label.upper().encode()[:11].ljust(11, b" "), ATTR_VOL, 0, 0)
        else:
            out += dirent(b".          ", ATTR_DIR, n.clusters[0], 0)
            parent = n.parent
            out += dirent(b"..         ", ATTR_DIR, 0 if parent is self.root else parent.clusters[0], 0)
        for ch in n.children:
            if self.lfn or ch.needs_lfn:
                for e in lfn_entries(self._long(ch), ch.name11):
                    out += e
            first = ch.clusters[0] if ch.clusters else 0
            if ch.is_dir:
                out += dirent(ch.name11, ATTR_DIR, first, 0)
            else:
                size = ch.fill_size if ch.data is None else len(ch.data)
                out += dirent(ch.name11, ATTR_ARC, first, size)
        return bytes(out)


def chs(lba: int) -> bytes:
    if lba >= 1024 * 255 * 63:
        return b"\xfe\xff\xff"
    c, rem = divmod(lba, 255 * 63)
    h, s = divmod(rem, 63)
    return bytes([h, ((s + 1) & 0x3F) | ((c >> 2) & 0xC0), c & 0xFF])


def mbr_entry(status: int, ptype: int, lba: int, sectors: int) -> bytes:
    return bytes([status]) + chs(lba) + bytes([ptype]) + chs(lba + sectors - 1) + struct.pack("<II", lba, sectors)


def boot_sector(oem: str, spc: int, reserved: int, nfats: int, hidden: int, total: int, fatsz: int,
                label: str, serial: int) -> bytes:
    b = bytearray(SECTOR)
    b[0:3] = b"\xeb\x58\x90"
    b[3:11] = oem.encode()[:8].ljust(8, b" ")
    struct.pack_into("<HBHBHHBHHHII", b, 11, SECTOR, spc, reserved, nfats, 0, 0, 0xF8, 0, 63, 255, hidden, total)
    struct.pack_into("<IHHIHH", b, 36, fatsz, 0, 0, 2, 1, 6)
    b[64] = 0x80
    b[66] = 0x29
    struct.pack_into("<I", b, 67, serial)
    b[71:82] = (label or "NO NAME").upper().encode()[:11].ljust(11, b" ")
    b[82:90] = b"FAT32   "
    b[90:92] = b"\xf4\xfe"                                     # hlt; jmp $: not bootable on a PC
    b[510:512] = b"\x55\xaa"
    return bytes(b)


def fsinfo_sector(free: int, next_free: int) -> bytes:
    b = bytearray(SECTOR)
    struct.pack_into("<I", b, 0, 0x41615252)
    struct.pack_into("<III", b, 484, 0x61417272, free, next_free)
    struct.pack_into("<I", b, 508, 0xAA550000)
    return bytes(b)


class Image:
    """An existing image, read and changed in place (the tests damage images with it)."""

    def __init__(self, path: str):
        self.path = path
        self.f = open(path, "r+b")
        self.mbr = self.read(0)
        self.part_index = next((i for i in range(4) if self.mbr[446 + 16 * i + 4] in (0x0B, 0x0C)), 0)
        e = 446 + 16 * self.part_index
        self.part_lba, self.part_sectors = struct.unpack_from("<II", self.mbr, e + 8)
        self.bs = self.read(self.part_lba)
        b = self.bs
        self.spc = b[13]
        self.reserved = struct.unpack_from("<H", b, 14)[0]
        self.nfats = b[16]
        self.fatsz = struct.unpack_from("<I", b, 36)[0]
        self.root = struct.unpack_from("<I", b, 44)[0]
        self.fat_lba = self.part_lba + self.reserved
        self.data_lba = self.fat_lba + self.nfats * self.fatsz
        total = struct.unpack_from("<I", b, 32)[0]
        self.clusters = (total - self.reserved - self.nfats * self.fatsz) // self.spc if self.spc else 0

    def close(self):
        self.f.close()

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    def read(self, lba: int, count: int = 1) -> bytes:
        self.f.seek(lba * SECTOR)
        return self.f.read(count * SECTOR)

    def write(self, lba: int, data: bytes):
        self.f.seek(lba * SECTOR)
        self.f.write(data)

    def poke(self, offset: int, data: bytes):
        self.f.seek(offset)
        self.f.write(data)

    def peek(self, offset: int, n: int) -> bytes:
        self.f.seek(offset)
        return self.f.read(n)

    def cluster_lba(self, c: int) -> int:
        return self.data_lba + (c - 2) * self.spc

    def fat_get(self, c: int) -> int:
        return struct.unpack("<I", self.peek(self.fat_lba * SECTOR + 4 * c, 4))[0] & 0x0FFFFFFF

    def fat_set(self, c: int, v: int, fats: int | None = None):
        for k in range(self.nfats if fats is None else fats):
            self.poke((self.fat_lba + k * self.fatsz) * SECTOR + 4 * c, struct.pack("<I", v))

    def chain(self, first: int, limit: int = 1 << 20) -> list[int]:
        out = []
        c = first
        while 2 <= c <= self.clusters + 1 and len(out) < limit:
            out.append(c)
            c = self.fat_get(c)
        return out

    def dir_entries(self, first: int):
        """(offset, raw32) of every entry slot of a directory, in order."""
        for c in self.chain(first):
            for s in range(self.spc):
                lba = self.cluster_lba(c) + s
                data = self.read(lba)
                for k in range(0, SECTOR, 32):
                    yield lba * SECTOR + k, data[k:k + 32]

    def find(self, path: str) -> dict:
        """The short entry of path (each component an 8.3 or a long name): offset, attr,
        cluster, size."""
        cur = self.root
        found = None
        for comp in [p for p in path.split("/") if p]:
            want = short_name(comp) if is_83(comp) else None
            found = None
            lfn = []
            for off, e in self.dir_entries(cur):
                if e[0] == 0:
                    break
                if e[0] == 0xE5:
                    lfn = []
                    continue
                if (e[11] & 0x3F) == ATTR_LFN:
                    units = struct.unpack_from("<5H", e, 1) + struct.unpack_from("<6H", e, 14) + \
                        struct.unpack_from("<2H", e, 28)
                    lfn.insert(0, "".join(chr(u) for u in units if u not in (0, 0xFFFF)))
                    continue
                long = "".join(lfn)
                lfn = []
                if e[11] & ATTR_VOL:
                    continue
                if e[:11] == want or (long and long.lower() == comp.lower()):
                    hi, lo = struct.unpack_from("<H", e, 20)[0], struct.unpack_from("<H", e, 26)[0]
                    found = {"offset": off, "attr": e[11], "cluster": hi << 16 | lo,
                             "size": struct.unpack_from("<I", e, 28)[0]}
                    break
            if not found:
                raise FileNotFoundError(path)
            cur = found["cluster"] or self.root
        return found

    def set_size(self, path: str, size: int):
        self.poke(self.find(path)["offset"] + 28, struct.pack("<I", size))

    def set_first_cluster(self, path: str, c: int):
        off = self.find(path)["offset"]
        self.poke(off + 20, struct.pack("<H", c >> 16))
        self.poke(off + 26, struct.pack("<H", c & 0xFFFF))

    def read_file(self, path: str) -> bytes:
        e = self.find(path)
        out = bytearray()
        for c in self.chain(e["cluster"]):
            out += self.read(self.cluster_lba(c), self.spc)
        return bytes(out[:e["size"]])


def inspect(path: str) -> int:
    with Image(path) as im:
        print(f"{path}:")
        for i in range(4):
            e = im.mbr[446 + 16 * i:462 + 16 * i]
            lba, n = struct.unpack_from("<II", e, 8)
            if e[4]:
                print(f"  partition {i + 1}: type {e[4]:#04x}, status {e[0]:#04x}, sectors {lba}..{lba + n - 1}")
        b = im.bs
        print(f"  boot sector: OEM {b[3:11].decode('latin-1')!r}, {struct.unpack_from('<H', b, 11)[0]} B/sector, "
              f"{im.spc} sectors/cluster, {im.reserved} reserved, {im.nfats} FATs of {im.fatsz} sectors, "
              f"{im.clusters} clusters, root at {im.root}, label {b[71:82].decode('latin-1').rstrip()!r}")

        def walk(cluster: int, prefix: str, depth: int):
            if depth > 16:
                return
            lfn = []
            for off, e in im.dir_entries(cluster):
                if e[0] == 0:
                    break
                if e[0] == 0xE5:
                    lfn = []
                    continue
                if (e[11] & 0x3F) == ATTR_LFN:
                    units = struct.unpack_from("<5H", e, 1) + struct.unpack_from("<6H", e, 14) + \
                        struct.unpack_from("<2H", e, 28)
                    lfn.insert(0, "".join(chr(u) for u in units if u not in (0, 0xFFFF)))
                    continue
                if e[11] & ATTR_VOL:
                    continue
                name = e[:8].decode("latin-1").rstrip() + ("." + e[8:11].decode("latin-1").rstrip()
                                                         if e[8:11].strip() else "")
                if name in (".", ".."):
                    continue
                hi, lo = struct.unpack_from("<H", e, 20)[0], struct.unpack_from("<H", e, 26)[0]
                first = hi << 16 | lo
                size = struct.unpack_from("<I", e, 28)[0]
                chain = im.chain(first) if first else []
                runs = sum(1 for k in range(len(chain)) if k == 0 or chain[k] != chain[k - 1] + 1)
                long = f" ({''.join(lfn)})" if lfn else ""
                lfn = []
                if e[11] & ATTR_DIR:
                    print(f"  {prefix}{name}/{long}  cluster {first}, {len(chain)} cluster(s) in {runs} fragment(s)")
                    walk(first, prefix + name + "/", depth + 1)
                else:
                    print(f"  {prefix}{name}{long}  {size} bytes, cluster {first}, {len(chain)} cluster(s) in "
                          f"{runs} fragment(s)")
        walk(im.root, "/", 0)
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="make MBR + FAT32 card images (never a device)")
    ap.add_argument("output", nargs="?", help="image file to write")
    ap.add_argument("--inspect", metavar="IMG", help="describe an image instead")
    ap.add_argument("--size", type=parse_size, default=64 << 20)
    ap.add_argument("--cluster", type=parse_size)
    ap.add_argument("--part-start", type=int, default=2048)
    ap.add_argument("--part-type", type=lambda s: int(s, 0), default=0x0C)
    ap.add_argument("--label", default="SURFOS")
    ap.add_argument("--file", action="append", default=[], metavar="DEST=SRC")
    ap.add_argument("--dir", action="append", default=[])
    ap.add_argument("--standard-dirs", action="store_true", help="/BOOT /SYSTEM /PROGRAMS /USERS/DOCUMENTS")
    ap.add_argument("--fragment", choices=("none", "interleave", "reverse", "random"), default="none")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--lfn", action="store_true")
    ap.add_argument("--fill", action="store_true")
    a = ap.parse_args(argv)
    if a.inspect:
        return inspect(a.inspect)
    if not a.output:
        ap.error("an output image (or --inspect IMG) is needed")
    out = Path(a.output)
    if out.exists() and not out.is_file():
        print(f"mkcard: {out} exists and is not a regular file; refusing", file=sys.stderr)
        return 1
    if str(out.resolve()).startswith("/dev/") or str(out.resolve()).startswith("/Volumes/"):
        print(f"mkcard: {out}: mkcard writes image files only", file=sys.stderr)
        return 1
    try:
        card = Card(a.size, a.cluster, a.part_start, a.part_type, a.label, a.fragment, a.seed, a.lfn)
        card.fill = a.fill
        for d in (STANDARD_DIRS if a.standard_dirs else ()):
            card.add_dir(d)
        for d in a.dir:
            card.add_dir(d)
        for spec in a.file:
            dest, _, src = spec.partition("=")
            if not src:
                raise ValueError(f"--file {spec!r}: use DEST=SRC")
            card.add_file(dest, Path(src).read_bytes())
        info = card.build(str(out))
    except (OSError, ValueError) as e:
        print(f"mkcard: {e}", file=sys.stderr)
        return 1
    print(f"mkcard: {out}: {a.size >> 20} MiB, FAT32 partition at sector {info['part_lba']}, "
          f"{info['cluster']}-byte clusters, {info['clusters']} clusters, {len(info['nodes']) - 1} entries")
    return 0


if __name__ == "__main__":
    sys.exit(main())
