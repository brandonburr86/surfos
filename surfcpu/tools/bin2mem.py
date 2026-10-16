#!/usr/bin/env python3
"""SurfCPU ROM image converter
--------------------
File: bin2mem.py Date: 10/15/26 (SurfCPU)
--------------------
Turns a raw binary into the ROM image format of docs/MODULES.md section 5: for $readmemh,
one 32-bit word per line as 8 lowercase hex digits, no address markers, line i holding bytes
4i..4i+3 of the binary in little-endian order, zero padded to exactly --words lines (16384
for the 64 KiB boot ROM).

    tools/bin2mem.py build/fw/surfboot.bin build/fw/surfboot.mem            # 16384 words
    tools/bin2mem.py IN.bin OUT.mem --words 1024
    tools/bin2mem.py --check build/fw/surfboot.bin build/fw/surfboot.mem    # verify a pair

Refuses a binary larger than the ROM. Only the Python standard library is used.
"""
import argparse
import os
import struct
import sys

ROM_WORDS = 16384           # 64 KiB boot ROM (spec/soc.toml: bootrom size 0x10000)


def to_words(data: bytes, words: int) -> list[int]:
    if len(data) > words * 4:
        raise ValueError(f"the binary is {len(data)} bytes, larger than the {words * 4}-byte ROM")
    padded = data + bytes(words * 4 - len(data))
    return list(struct.unpack(f"<{words}I", padded))


def mem_text(data: bytes, words: int = ROM_WORDS) -> str:
    return "".join(f"{w:08x}\n" for w in to_words(data, words))


def parse_mem(text: str) -> list[int]:
    out = []
    for n, line in enumerate(text.splitlines(), 1):
        if len(line) != 8 or any(c not in "0123456789abcdefABCDEF" for c in line):
            raise ValueError(f"line {n}: {line!r} is not 8 hex digits")
        out.append(int(line, 16))
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="binary -> $readmemh ROM image (docs/MODULES.md section 5)")
    ap.add_argument("input", help="raw binary (objcopy -O binary)")
    ap.add_argument("output", help=".mem file to write (or to check with --check)")
    ap.add_argument("--words", type=int, default=ROM_WORDS, help=f"lines in the image (default {ROM_WORDS})")
    ap.add_argument("--check", action="store_true", help="verify OUTPUT against INPUT instead of writing")
    a = ap.parse_args(argv)
    data = open(a.input, "rb").read()
    try:
        words = to_words(data, a.words)
    except ValueError as e:
        print(f"bin2mem: {a.input}: {e}", file=sys.stderr)
        return 1
    if a.check:
        try:
            got = parse_mem(open(a.output).read())
        except (OSError, ValueError) as e:
            print(f"bin2mem: {a.output}: {e}", file=sys.stderr)
            return 1
        if len(got) != a.words:
            print(f"bin2mem: {a.output}: {len(got)} lines, expected {a.words}", file=sys.stderr)
            return 1
        for i, (x, y) in enumerate(zip(words, got)):
            if x != y:
                print(f"bin2mem: {a.output}: line {i + 1} is {y:08x}, the binary says {x:08x}", file=sys.stderr)
                return 1
        print(f"bin2mem: {a.output} matches {a.input} ({len(data)} bytes in {a.words} words)")
        return 0
    tmp = a.output + ".tmp"
    with open(tmp, "w") as f:
        f.write("".join(f"{w:08x}\n" for w in words))
    os.replace(tmp, a.output)
    print(f"bin2mem: {a.output}: {len(data)} bytes of {a.words * 4} ({100.0 * len(data) / (a.words * 4):.1f}%)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
