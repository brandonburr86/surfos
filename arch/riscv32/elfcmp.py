#!/usr/bin/env python3
"""
SurfOS riscv32 port: is the x86 kernel unchanged?
File: elfcmp.py Date: 10/15/26 (SurfCPU)

The riscv32 port changes a few shared files under #ifdef SURFOS_RISCV32 (kernel/task.c,
lib/blibc/vsnprintf.c). This compares two builds of an ELF32 object or kernel section by
section: every allocated section (code, read-only data, data, the symbol table, BSS)
byte for byte, debug information ignored (moved line numbers change it). Used on the x86
kernel built before and after a guarded change (surfcpu/docs/evidence/os/x86-unchanged.txt).

  elfcmp.py before/build/O2/surfos.bin after/build/O2/surfos.bin     exit status 0 when identical
"""
import struct
import sys


def sections(path):
    with open(path, 'rb') as f:
        d = f.read()
    if d[:4] != b'\x7fELF' or d[4] != 1:
        sys.exit('elfcmp: %s is not a 32-bit ELF file' % path)
    shoff, = struct.unpack_from('<I', d, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x2E)
    hdrs = [struct.unpack_from('<IIIIIIIIII', d, shoff + i * shentsize) for i in range(shnum)]
    stro = hdrs[shstrndx][4]
    out = {}
    for name, typ, flags, addr, off, size, *_ in hdrs:
        n = d[stro + name:d.index(b'\0', stro + name)].decode()
        if flags & 2:                                   # SHF_ALLOC
            out[n] = (addr, size, d[off:off + size] if typ != 8 else b'')   # SHT_NOBITS: size only
    return out


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    a, b = sections(sys.argv[1]), sections(sys.argv[2])
    same = True
    for n in sorted(set(a) | set(b)):
        if a.get(n) != b.get(n):
            same = False
            print('  differs: %s %s -> %s' % (n, a[n][:2] if n in a else None, b[n][:2] if n in b else None))
    print('%s: %d allocated sections, %s' % (sys.argv[2], len(b), 'identical' if same else 'DIFFERENT'))
    sys.exit(0 if same else 1)


if __name__ == '__main__':
    main()
