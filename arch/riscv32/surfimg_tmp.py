#!/usr/bin/env python3
"""
SurfOS riscv32: a temporary SURFOS.BIN packer
File: surfimg_tmp.py Date: 10/15/26 (SurfCPU)

Packs and checks the kernel package of surfcpu/docs/BOOT-ABI.md section 2 (version 1)
until the fw track's official surfcpu/tools/surfimg.py is merged; kernel.mk then calls
that instead. Written to BOOT-ABI.md alone, with every constant read from the generated
surfcpu/firmware/include/surfcpu/soc.h (never typed here):

  surfimg_tmp.py pack --elf surfos.elf --payload surfos.payload.bin --out SURFOS.BIN \
                      --version 1.0.0 --build-id 1234abcd --name SurfOS
  surfimg_tmp.py verify SURFOS.BIN      every check SurfBoot makes, in its order
  surfimg_tmp.py selftest               the CRC test vectors and a pack/verify round trip

The payload is the ELF's loadable bytes from the first to the end of initialized data
(objcopy -O binary); pack checks that the payload file is exactly those bytes, that the
entry is inside it, and records mem_size = payload + BSS.
"""

import argparse
import os
import re
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SOC_H = os.path.join(HERE, '..', '..', 'surfcpu', 'firmware', 'include', 'surfcpu', 'soc.h')

HEADER = struct.Struct('<IHHIIIIIIIIII12sI')


def soc_constants(path=SOC_H):
    consts = {}
    with open(path) as f:
        for line in f:
            m = re.match(r'#define\s+(SURF_\w+)\s+(0x[0-9A-Fa-f]+|\d+)u?\b', line)
            if m:
                consts[m.group(1)] = int(m.group(2), 0)
    return consts


C = soc_constants()
ISA_ALL = C['SURF_ISA_I'] | C['SURF_ISA_M'] | C['SURF_ISA_ZICSR'] | C['SURF_ISA_ZIFENCEI']


def crc32(data):
    """CRC-32/ISO-HDLC (BOOT-ABI.md section 3): zlib's"""
    return zlib.crc32(data) & 0xFFFFFFFF


def crc_selftest():
    vectors = [(b'', 0x00000000), (b'a', 0xE8B7BE43), (b'123456789', 0xCBF43926), (bytes(range(256)), 0x29058C73)]
    for data, want in vectors:
        got = crc32(data)
        if got != want:
            raise SystemExit('surfimg_tmp: CRC-32 of %r is 0x%08X, BOOT-ABI.md says 0x%08X' % (data[:16], got, want))


def elf_layout(path):
    """(entry, load address, end of initialized data, end of memory, the loadable bytes)"""
    with open(path, 'rb') as f:
        elf = f.read()
    if elf[:4] != b'\x7fELF' or elf[4] != 1 or elf[5] != 1:
        raise SystemExit('surfimg_tmp: %s is not a 32-bit little-endian ELF' % path)
    (e_type, e_machine, _ver, e_entry, e_phoff, _shoff, _flags, _ehsize, e_phentsize, e_phnum) = \
        struct.unpack_from('<HHIIIIIHHH', elf, 16)
    if e_machine != 243:
        raise SystemExit('surfimg_tmp: %s is not RISC-V (e_machine %d)' % (path, e_machine))
    loads = []
    for i in range(e_phnum):
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, _pf, _align = \
            struct.unpack_from('<IIIIIIII', elf, e_phoff + i * e_phentsize)
        if p_type == 1 and p_memsz:
            loads.append((p_paddr, p_offset, p_filesz, p_memsz))
    if not loads:
        raise SystemExit('surfimg_tmp: %s has no loadable segment' % path)
    loads.sort()
    start = min(p for p, _o, fs, _m in loads if fs)
    data_end = max(p + fs for p, _o, fs, _m in loads if fs)
    mem_end = max(p + m for p, _o, _fs, m in loads)
    image = bytearray(data_end - start)
    for p, o, fs, _m in loads:
        if fs:
            image[p - start:p - start + fs] = elf[o:o + fs]
    return e_entry, start, data_end, mem_end, bytes(image)


def parse_version(text):
    parts = text.split('.')
    if len(parts) != 3 or not all(x.isdigit() and int(x) < 256 for x in parts):
        raise SystemExit('surfimg_tmp: --version wants major.minor.patch, each below 256')
    major, minor, patch = (int(x) for x in parts)
    return major << 16 | minor << 8 | patch


def make_header(load, payload, mem_size, entry, image_version, build_id, name):
    fields = [C['SURF_IMAGE_MAGIC'], C['SURF_IMAGE_HEADER_VERSION'], C['SURF_IMAGE_HEADER_SIZE'], ISA_ALL,
              C['SURF_PLATFORM_ABI'], load, len(payload), mem_size, entry, crc32(payload), 0, image_version,
              build_id, name.encode('ascii')[:12].ljust(12, b'\0'), 0]
    head = HEADER.pack(*fields)
    return head[:0x3C] + struct.pack('<I', crc32(head[:0x3C]))


def check(blob):
    """SurfBoot's checks (BOOT-ABI.md section 2), in order; the fields, or SystemExit naming the failed one"""
    if len(blob) < 64:
        raise SystemExit('check 1: the file is shorter than 64 bytes')
    (magic, version, hsize, isa, platform, load, psize, msize, entry, pcrc, flags, iver, bid, name, hcrc) = \
        HEADER.unpack_from(blob, 0)
    if magic != C['SURF_IMAGE_MAGIC'] or version != C['SURF_IMAGE_HEADER_VERSION'] or hsize != C['SURF_IMAGE_HEADER_SIZE']:
        raise SystemExit('check 1: magic, version or header size is wrong (0x%08X, %d, %d)' % (magic, version, hsize))
    if crc32(blob[:0x3C]) != hcrc:
        raise SystemExit('check 1: header CRC 0x%08X does not match 0x%08X' % (hcrc, crc32(blob[:0x3C])))
    if isa & ~ISA_ALL or platform != C['SURF_PLATFORM_ABI']:
        raise SystemExit('check 2: ISA bits 0x%X or platform %d are not SurfCPU\'s' % (isa, platform))
    if psize == 0 or len(blob) != 64 + psize:
        raise SystemExit('check 3: the file is %d bytes, the header says 64 + %d' % (len(blob), psize))
    ddr_lo, ddr_hi = C['SURF_DDR_BASE'], C['SURF_DDR_TOP_BASE']
    if load & 3 or msize < psize or load < ddr_lo or load + msize > ddr_hi:
        raise SystemExit('check 4: load 0x%08X + %d bytes is not aligned DDR below ddr_top (0x%08X-0x%08X)'
                         % (load, msize, ddr_lo, ddr_hi))
    if entry & 3 or not load <= entry < load + psize:
        raise SystemExit('check 5: entry 0x%08X is not aligned inside the payload' % entry)
    if crc32(blob[64:]) != pcrc:
        raise SystemExit('check 6: payload CRC 0x%08X does not match 0x%08X' % (crc32(blob[64:]), pcrc))
    if flags:
        raise SystemExit('flags must be 0 (reserved), not 0x%X' % flags)
    return dict(load=load, payload=psize, mem=msize, entry=entry, crc=pcrc, version=iver, build=bid,
                name=name.rstrip(b'\0').decode('ascii', 'replace'))


def cmd_pack(args):
    crc_selftest()
    entry, load, data_end, mem_end, image = elf_layout(args.elf)
    with open(args.payload, 'rb') as f:
        payload = f.read()
    if payload != image:
        raise SystemExit('surfimg_tmp: %s is not the ELF\'s loadable bytes (%d against %d bytes)'
                         % (args.payload, len(payload), len(image)))
    head = make_header(load, payload, mem_end - load, entry, parse_version(args.version),
                       int(args.build_id, 16), args.name)
    blob = head + payload
    check(blob)
    tmp = args.out + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(blob)
    os.replace(tmp, args.out)


def cmd_verify(args):
    crc_selftest()
    with open(args.image, 'rb') as f:
        blob = f.read()
    h = check(blob)
    print('%s: SIMG v1, %s %d.%d.%d (build %08x), load 0x%08X, payload %d bytes, memory %d bytes, '
          'entry 0x%08X, CRC 0x%08X: ok' % (args.image, h['name'], h['version'] >> 16, (h['version'] >> 8) & 255,
                                            h['version'] & 255, h['build'], h['load'], h['payload'], h['mem'],
                                            h['entry'], h['crc']))


def cmd_selftest(_args):
    crc_selftest()
    payload = bytes(range(256)) * 4
    blob = make_header(0x80000000, payload, 4096, 0x80000000, 0x010000, 0x1234ABCD, 'SurfOS') + payload
    check(blob)
    bad = [('truncated', blob[:-1]), ('padded', blob + b'\0'),
           ('payload byte', blob[:100] + bytes([blob[100] ^ 1]) + blob[101:]),
           ('header byte', blob[:0x10] + b'\x01' + blob[0x11:])]
    for what, b in bad:
        try:
            check(b)
        except SystemExit:
            continue
        raise SystemExit('surfimg_tmp selftest: a %s package passed the checks' % what)
    print('surfimg_tmp selftest: CRC vectors and %d corrupted packages: ok' % len(bad))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[1])
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('pack')
    p.add_argument('--elf', required=True)
    p.add_argument('--payload', required=True)
    p.add_argument('--out', required=True)
    p.add_argument('--version', default='1.0.0')
    p.add_argument('--build-id', default='00000000')
    p.add_argument('--name', default='SurfOS')
    v = sub.add_parser('verify')
    v.add_argument('image')
    sub.add_parser('selftest')
    args = ap.parse_args()
    {'pack': cmd_pack, 'verify': cmd_verify, 'selftest': cmd_selftest}[args.cmd](args)


if __name__ == '__main__':
    main()
