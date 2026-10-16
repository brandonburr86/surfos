#!/usr/bin/env python3
"""
SurfOS riscv32: every instruction of the linked kernel is one SurfCPU executes
File: isacheck.py Date: 10/15/26 (SurfCPU)

The brief (surfcpu/docs/AGENT-INSTRUCTIONS.md section 5) asks for the linked disassembly
to be audited: the kernel and the libgcc routines linked into it must use nothing beyond
rv32im_zicsr_zifencei plus MRET and WFI (surfcpu/docs/ISA.md). This disassembles every
code section with objdump (canonical names, -M no-aliases) and refuses any other
mnemonic, any 16-bit (compressed) encoding and anything objdump cannot decode. (The
shell's crashill uses unimp, a CSR write the CPU refuses, which objdump shows as csrrw.)
It runs on every link. The fw track's surfcpu/tools/isa-audit.py, which decodes the
instructions itself and checks the CSRs as well, runs too where the tree has it: its
surfimg.py audits the ELF before packing SURFOS.BIN.

  isacheck.py [--objdump riscv64-unknown-elf-objdump] surfos.elf
"""

import argparse
import collections
import re
import subprocess
import sys

RV32I = {'lui', 'auipc', 'jal', 'jalr', 'beq', 'bne', 'blt', 'bge', 'bltu', 'bgeu', 'lb', 'lh', 'lw', 'lbu',
         'lhu', 'sb', 'sh', 'sw', 'addi', 'slti', 'sltiu', 'xori', 'ori', 'andi', 'slli', 'srli', 'srai', 'add',
         'sub', 'sll', 'slt', 'sltu', 'xor', 'srl', 'sra', 'or', 'and', 'fence', 'fence.tso', 'pause', 'ecall',
         'ebreak'}
M = {'mul', 'mulh', 'mulhsu', 'mulhu', 'div', 'divu', 'rem', 'remu'}
ZICSR = {'csrrw', 'csrrs', 'csrrc', 'csrrwi', 'csrrsi', 'csrrci'}
ZIFENCEI = {'fence.i'}
PRIV = {'mret', 'wfi'}
ALLOWED = RV32I | M | ZICSR | ZIFENCEI | PRIV

LINE = re.compile(r'^\s*([0-9a-f]+):\s+([0-9a-f]+)\s+(\S+)')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--objdump', default='riscv64-unknown-elf-objdump')
    ap.add_argument('elf')
    args = ap.parse_args()
    out = subprocess.run([args.objdump, '-d', '-M', 'no-aliases', args.elf], check=True,
                         capture_output=True, text=True).stdout
    counts = collections.Counter()
    bad = []
    total = 0
    for line in out.splitlines():
        m = LINE.match(line)
        if not m:
            continue
        addr, enc, mnem = m.groups()
        total += 1
        counts[mnem] += 1
        if mnem == 'unimp' and enc == 'c0001073':
            continue                    # csrrw zero, cycle, zero: Zicsr, refused by the CPU on purpose
        if len(enc) != 8:
            bad.append('%s: %s %s (a %d-bit encoding)' % (addr, enc, mnem, len(enc) * 4))
        elif mnem not in ALLOWED:
            bad.append('%s: %s %s' % (addr, enc, mnem))
    if not total:
        sys.exit('isacheck: no instructions found in %s' % args.elf)
    if bad:
        print('isacheck: %s has %d instructions outside rv32im_zicsr_zifencei + mret/wfi:' % (args.elf, len(bad)))
        for b in bad[:40]:
            print('  ' + b)
        sys.exit(1)
    print('isacheck: %s: %d instructions, %d distinct mnemonics, all rv32im_zicsr_zifencei (+ mret, wfi)'
          % (args.elf, total, len(counts)))


if __name__ == '__main__':
    main()
