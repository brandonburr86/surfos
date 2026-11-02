#!/usr/bin/env python3
"""The tools of the command-line suite's diff track (11/01/26), run as programs beside the real ones.

Usage: test_cli_diff.py DIR, where DIR holds the tools built for the host (user/bin/*.c as they are, over the host's C
library: tests/host/cli/cli_host.h). `make unittest` runs this in the Docker image, against GNU tools; on the Mac it runs
against BSD ones. A comparison whose tool is not installed, or whose output is another system's own, is skipped and
counted. Nothing here is sanitized in Docker; tests/host/cli/mac-sanitize.sh runs the same on the Mac under ASan and UBSan.
user/diff/'s logic has its own test, tests/host/test_diff.c.
"""
import base64 as b64
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

TOOLS = os.path.abspath(sys.argv[1])
ENV = dict(os.environ, LC_ALL="C", ASAN_OPTIONS="exitcode=99:abort_on_error=0:detect_leaks=0",
           UBSAN_OPTIONS="halt_on_error=1:abort_on_error=0:exitcode=99")
ENV.pop("CLI_FAIL", None)
TIMEOUT = 120
SCRATCH = tempfile.TemporaryDirectory()  # all the test's directories, gone when it ends

checks = failures = 0
skipped = {}
rng = random.Random(20261101)


def ok(cond, what):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        if failures < 40:
            print("FAIL:", what)
    return cond


def skip(what):
    skipped[what] = skipped.get(what, 0) + 1


def run(argv, stdin=b"", cwd=None, env=None):
    p = subprocess.run(argv, input=stdin, cwd=cwd, env=env or ENV, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       timeout=TIMEOUT)
    return p.returncode, p.stdout, p.stderr


def ours(name, *args, stdin=b"", cwd=None, env=None):
    """A tool of ours; a sanitizer's report or a signal is never an acceptable answer."""
    r = run([os.path.join(TOOLS, name)] + list(args), stdin, cwd, env)
    ok(r[0] != 99 and r[0] >= 0 and b"AddressSanitizer" not in r[2] and b"runtime error" not in r[2],
       "%s %s crashed: %r" % (name, args, (r[0], r[2][:300])))
    return r


def real(name):
    return shutil.which(name)


def is_gnu(name):
    if not real(name):
        return False
    try:
        return b"GNU" in run([name, "--version"])[1]
    except (OSError, subprocess.SubprocessError):
        return False


def put(path, data):
    if isinstance(data, str):
        data = data.encode()
    d = os.path.dirname(path)
    if d:
        os.makedirs(d, exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def get(path):
    with open(path, "rb") as f:
        return f.read()


WORDS = ["", "", "alpha", "beta", "gamma", "delta", "x", "y", "  indented", "trailing  ", "\ttabbed", "a b  c", "#include <x.h>",
         "}", "{", "long " * 30, "--- dash", "+++ plus", "@@ at", "*** star", "a,b,c", "1 2 3"]


def text(lines=40, words=WORDS, newline=True):
    s = "\n".join(rng.choice(words) for _ in range(lines))
    return (s + ("\n" if newline and lines else "")).encode()


def edited(data, rate=6, words=WORDS):
    out = []
    for line in data.split(b"\n"):
        r = rng.randrange(rate)
        if r:
            out.append(line)
        if r <= 1:
            out.append(rng.choice(words).encode())
    return b"\n".join(out)


BINARY = bytes(rng.randrange(256) for _ in range(5000))
PRINTABLE = b"".join(bytes(rng.choice(b"abcdefgh \t") for _ in range(rng.randrange(1, 90))) + b"\n" for _ in range(60))
UTF8 = "héllo wörld\nΣίσυφος ☃ 雪\n\nplain\n".encode()
CORPUS = [b"", b"\n", b"a", b"a\n", b"\n\n\n", PRINTABLE, text(), text(newline=False), BINARY, BINARY[:17], b"\x00" * 64, b"x" * 100000 + b"\n"]


def same(tool, args, data, realname=None, what="", stdin=None, fix=None):
    """the tool and the real one, on a file holding data (or standard input): the same output and whether it failed"""
    rname = realname or tool
    if not real(rname):
        skip("%s: not installed" % rname)
        return
    with tempfile.TemporaryDirectory() as d:
        put(os.path.join(d, "in"), data)
        a = ours(tool, *args, cwd=d, stdin=stdin if stdin is not None else b"")
        b = run([rname] + list(args), stdin if stdin is not None else b"", cwd=d)
    fa, fb = (fix(a[1]), fix(b[1])) if fix else (a[1], b[1])
    ok(fa == fb and (a[0] == 0) == (b[0] == 0), "%s %s %s: %r against %r" % (tool, " ".join(args), what, fa[:200], fb[:200]))


# -- the small tools ------------------------------------------------------------------------------------------------


def test_rev():
    for data in CORPUS:
        if b"\x00" in data:
            continue                    # the real one stops at a NUL, or says so
        same("rev", ["in"], data, fix=lambda s: s if s.endswith(b"\n") else s + b"\n")   # the Mac's adds a last newline
    ok(ours("rev", stdin="héllo\n".encode())[1] == "olléh\n".encode(), "rev: a UTF-8 sequence stays whole")
    ok(ours("rev", stdin=b"a\xffb\n")[1] == b"b\xffa\n", "rev: an invalid byte goes as one")
    ok(ours("rev", stdin=b"ab")[1] == b"ba", "rev: no newline made up")
    ok(ours("rev", "/nowhere/x")[0] == 2, "rev: a missing file is status 2")


def test_paths():
    names = ["a", "a/b", "a/b/", "/", "//", "///", "/a", "/a/", "a//b", "a/b//", ".", "..", "./a", "../a", "", "/a/b.txt", "a.txt"]
    for n in names:
        same("basename", [n], b"", what="of %r" % n)
        same("dirname", [n], b"", what="of %r" % n)
    for n, suffix in (("a.txt", ".txt"), (".txt", ".txt"), ("/x/a.txt/", ".txt"), ("a", "b"), ("ab", "b"), ("b", "b")):
        same("basename", [n, suffix], b"")
    ok(ours("basename")[0] == 2 and ours("dirname")[0] == 2, "no operand is a usage error")
    ok(ours("dirname", "a/b", "c")[1] == b"a\n.\n", "dirname: several")


def test_cmp():
    # the words and the widths are each system's own
    fix = lambda s: b"\n".join(re.sub(rb" +", b" ", line).strip() for line in re.sub(rb"\b(char|byte)\b", b"byte", s).split(b"\n"))
    pairs = [(b"", b""), (b"abc", b"abc"), (b"abc", b"abd"), (b"a\nb\nc\n", b"a\nb\nd\n"), (b"abc", b"ab"), (b"ab", b"abc"), (b"", b"x"),
             (BINARY, BINARY[:3000] + b"!" + BINARY[3001:]), (b"x" * 70000, b"x" * 65536 + b"y" * 4464)]
    for a, b in pairs:
        with tempfile.TemporaryDirectory() as d:
            put(d + "/a", a)
            put(d + "/b", b)
            for args in ([], ["-l"], ["-s"]):
                x = ours("cmp", *args, "a", "b", cwd=d)
                if real("cmp"):
                    y = run(["cmp"] + args + ["a", "b"], cwd=d)
                    ok(x[0] == y[0] and fix(x[1]) == fix(y[1]), "cmp %s: %r against %r" % (args, x[:2], y[:2]))
                else:
                    skip("cmp: not installed")
            x = ours("cmp", "-n", "2", "a", "b", cwd=d)
            ok(x[0] == (0 if a[:2] == b[:2] else 1), "cmp -n 2")
    with tempfile.TemporaryDirectory() as d:
        put(d + "/a", b"x")
        ok(ours("cmp", "a", "gone", cwd=d)[0] == 2, "cmp: a missing file is status 2")
        ok(ours("cmp", "-", "a", cwd=d, stdin=b"x")[0] == 0, "cmp: - is standard input")
        ok(ours("cmp", "-", "-", cwd=d, stdin=b"x")[0] == 0, "cmp: - against itself")
        ok(ours("cmp", ".", "a", cwd=d)[0] == 2, "cmp: a directory is trouble")
        e = ours("cmp", "a", os.devnull, cwd=d)
        ok(e[0] == 1 and b"EOF" in e[2], "cmp: an empty file is on the standard error")


def sorted_lines(n, key=lambda: "k%03d" % rng.randrange(60)):
    return "".join(sorted(key() + " v%d\n" % rng.randrange(9) for _ in range(n))).encode()


def test_comm():
    for _ in range(12):
        a, b = sorted_lines(rng.randrange(1, 40)), sorted_lines(rng.randrange(1, 40))
        with tempfile.TemporaryDirectory() as d:
            put(d + "/a", a)
            put(d + "/b", b)
            for args in ([], ["-1"], ["-2"], ["-3"], ["-12"], ["-13"], ["-23"], ["-123"]):
                if real("comm"):
                    x = ours("comm", *args, "a", "b", cwd=d)
                    y = run(["comm"] + args + ["a", "b"], cwd=d)
                    ok(x[1] == y[1] and x[0] == y[0], "comm %s: %r against %r" % (args, x[1][:100], y[1][:100]))
                else:
                    skip("comm: not installed")
    ok(ours("comm", "-", "-")[0] == 2, "comm: both standard input is refused")
    ok(ours("comm", "x", "y", cwd="/")[0] == 2, "comm: missing files")


def test_join():
    for _ in range(12):
        a = sorted_lines(rng.randrange(1, 30), lambda: "k%02d" % rng.randrange(25))
        b = sorted_lines(rng.randrange(1, 30), lambda: "k%02d" % rng.randrange(25))
        with tempfile.TemporaryDirectory() as d:
            put(d + "/a", a)
            put(d + "/b", b)
            for args in ([], ["-a", "1"], ["-a", "2"], ["-a", "1", "-a", "2"], ["-v", "1"], ["-v", "2"], ["-1", "2", "-2", "2"],
                         ["-o", "0,1.2,2.2"], ["-a", "1", "-e", "EMPTY", "-o", "0,2.2"], ["-t", "v"], ["-o", "2.1,1.1", "-t", " "]):
                if real("join"):
                    x = ours("join", *args, "a", "b", cwd=d)
                    y = run(["join"] + args + ["a", "b"], cwd=d)
                    ok(x[1] == y[1], "join %s: %r against %r" % (args, x[1][:120], y[1][:120]))
                else:
                    skip("join: not installed")
    with tempfile.TemporaryDirectory() as d:     # the review: -o given twice adds to the list
        put(d + "/j1", b"a 1\n")
        put(d + "/j2", b"a x\n")
        long_list = ["-o", "1.1,1.2,2.1,2.2,0,0,0,0,0,0,0,0,0,0,0,0", "-o", "0"]
        for args in (long_list, ["-o", "1.2", "-o", "2.2"]):
            r = ours("join", *args, "j1", "j2", cwd=d)
            if is_gnu("join"):
                ok(r[1] == run(["join"] + args + ["j1", "j2"], cwd=d)[1], "join: -o twice, as GNU's: %r" % (r,))
            ok(r[0] == 0 and r[1].count(b" ") == (len(args[1].split(",")) + len(args[3].split(",")) - 1), "join: -o twice: %r" % (r,))
    ok(ours("join", "-t", "ab", "x", "y")[0] == 2, "join: -t is one character")
    ok(ours("join", "-t", "é", "-", os.devnull, stdin="aéb\n".encode())[0] == 0, "join: -t takes a UTF-8 character")
    ok(ours("join", "-o", "3.1", "x", "y")[0] == 2, "join: a bad -o list")
    ok(ours("join", "-1", "0", "x", "y")[0] == 2, "join: fields count from 1")


def test_paste():
    with tempfile.TemporaryDirectory() as d:
        put(d + "/a", b"1\n2\n3\n")
        put(d + "/b", b"x\ny")
        put(d + "/c", b"")
        put(d + "/e", b"\n\n")
        cases = [["a", "b"], ["b", "a"], ["a", "b", "c"], ["c", "c"], ["-s", "a"], ["-s", "a", "b"], ["-s", "c"], ["-d", ":", "a", "b"],
                 ["-d", ":,", "a", "b", "a"], ["-s", "-d", ":,", "a"], ["-d", "\\n", "a", "b"], ["-d", "\\t", "a", "e"], ["-d", "\\0", "a", "b"],
                 ["-s", "-d", "\\0", "a"]]
        for args in cases:
            if is_gnu("paste"):
                x = ours("paste", *args, cwd=d)
                y = run(["paste"] + args, cwd=d)
                ok(x[1] == y[1] and x[0] == y[0], "paste %s: %r against %r" % (args, x[1], y[1]))
            else:
                skip("paste: exact output only against GNU's (BSD's -s leaves off the last newline)")
        if real("paste"):
            s = b"1\n2\n3\n4\n5\n"
            ok(ours("paste", "-", "-", cwd=d, stdin=s)[1] == run(["paste", "-", "-"], s, d)[1], "paste - -")
        ok(ours("paste", "-d", "é,", "a", "b", cwd=d)[1] == "1éx\n2éy\n3é\n".encode(), "paste: a UTF-8 separator")
        ok(ours("paste", "-d", "x\\", "a", cwd=d)[0] == 2, "paste: a list that ends in a backslash")
        ok(ours("paste", "gone", cwd=d)[0] == 2, "paste: a missing file")
        ok(ours("paste", "-s", "a", "gone", cwd=d)[0] == 2, "paste -s: goes on, and says so")


def test_fold():
    data = PRINTABLE + b"\tt\tt\tt\tt\tt\tt\tt\tt\tt\tt\tt\tt\n" + b"ab\bcd\b\b\be\rf\n" + b"a" * 300 + b" b\n" + b"w " * 200 + b"\n"
    for args in ([], ["-w", "10"], ["-w", "1"], ["-w", "7"], ["-w", "8"], ["-w", "9"], ["-s"], ["-s", "-w", "10"], ["-w", "20", "-s"],
                 ["-b"], ["-b", "-w", "5"], ["-s", "-b", "-w", "12"], ["-w5"]):
        if is_gnu("fold"):
            same("fold", args + ["in"], data)
            same("fold", args + ["in"], b"no newline at the end")
        else:
            skip("fold: exact output only against GNU's (BSD's counts a tab and a backspace its own way)")
    ok(ours("fold", "-w", "3", stdin="héllo\n".encode())[1] == "hél\nlo\n".encode(), "fold: columns are characters")
    ok(ours("fold", "-b", "-w", "2", stdin="héllo\n".encode())[1] == "h\né\nll\no\n".encode(), "fold -b: a sequence goes whole to the next line")
    ok(ours("fold", "-b", "-w", "1", stdin="é\n".encode())[1] == "é\n".encode(), "fold -b: wider than the width, still whole")
    ok(ours("fold", "-w", "2", stdin=b"\xff\xfe\xfd\n")[1] == b"\xff\xfe\n\xfd\n", "fold: an invalid byte counts one")
    ok(ours("fold", "-w", "0")[0] == 2 and ours("fold", "-w", "x")[0] == 2, "fold: a bad width")


def test_nl():
    data = b"one\n\n  two\n\n\nthree\n"
    for args in ([], ["-b", "a"], ["-b", "t"], ["-b", "n"], ["-ba"], ["-w", "3"], ["-w", "1"], ["-s", ": "], ["-s", ""], ["-v", "10"], ["-v", "-3", "-i", "2"],
                 ["-i", "5"], ["-b", "a", "-w", "2", "-s", "|"], ["-i", "0"], ["-b", "pt"], ["-b", "p^t"]):
        if is_gnu("nl"):
            same("nl", args + ["in"], data)
        else:
            skip("nl: exact output only against GNU's (BSD's keeps the separator on a line without a number)")
    if is_gnu("nl"):
        same("nl", ["in"], b"no newline")
    with tempfile.TemporaryDirectory() as d:
        put(d + "/a", b"x\n")
        put(d + "/b", b"y\n")
        ok(ours("nl", "a", "b", cwd=d)[1] == b"     1\tx\n     2\ty\n", "nl: the count goes on through the files")
    ok(ours("nl", "-b", "z", stdin=b"")[0] == 2 and ours("nl", "-w", "0", stdin=b"")[0] == 2, "nl: bad options")
    ok(ours("nl", "-s", "é", stdin=b"\n")[1] == b"       \n", "nl: the separator's width is in characters")


def test_od():
    if not is_gnu("od"):
        skip("od: exact output only against GNU's")
    data = bytes(range(256)) + b"\x00" * 64 + b"hello\n" + b"\xff" * 33
    for sample in (data, b"", b"a", b"abc", BINARY[:1000]):
        for args in ([], ["-c"], ["-x"], ["-b"], ["-t", "x1"], ["-t", "x2"], ["-t", "x4"], ["-t", "x8"], ["-t", "o1"], ["-t", "o2"], ["-t", "o4"],
                     ["-t", "d1"], ["-t", "d2"], ["-t", "d4"], ["-t", "d8"], ["-t", "u1"], ["-t", "u2"], ["-t", "u4"], ["-t", "u8"], ["-t", "a"], ["-t", "c"],
                     ["-t", "o8"], ["-t", "x1z"], ["-A", "x"], ["-A", "d"], ["-A", "n"], ["-A", "o"], ["-A", "x", "-t", "x1"], ["-N", "20"], ["-j", "5"], ["-j", "5", "-N", "9"],
                     ["-j", "0x10"], ["-j", "010"], ["-N", "1b"], ["-v"], ["-v", "-c"], ["-t", "x1", "-c"], ["-c", "-t", "x1"], ["-t", "x2", "-t", "d1", "-c"], ["-t", "x4", "-t", "o1"],
                     ["-t", "dC"], ["-t", "uS"], ["-t", "xI"], ["-t", "x1x2"], ["-b", "-c", "-x"]):
            if is_gnu("od"):
                if args[:1] == ["-j"] and not sample:
                    continue
                same("od", args + ["in"], sample)
    with tempfile.TemporaryDirectory() as d:
        put(d + "/a", b"abc")
        put(d + "/b", b"def")
        ok(ours("od", "-c", "a", "b", cwd=d)[1] == b"0000000   a   b   c   d   e   f\n0000006\n", "od: the files as one stream")
        ok(ours("od", "-j", "100", "a", cwd=d)[0] == 2, "od: a skip past the end")
        ok(ours("od", "-t", "f4", "a", cwd=d)[0] == 2, "od: -t f is refused, with a reason")
        ok(ours("od", "-t", "q", "a", cwd=d)[0] == 2 and ours("od", "-A", "z", "a", cwd=d)[0] == 2, "od: bad types")
        ok(ours("od", "gone", cwd=d)[0] == 2, "od: a missing file")
        ok(ours("od", stdin=b"")[1] == b"0000000\n", "od: nothing is an address")


def test_base64():
    gnu = is_gnu("base64")
    for data in CORPUS + [bytes(range(256)) * 40, b"ab", b"abc", b"abcd"]:
        x = ours("base64", stdin=data)
        ok(x[0] == 0, "base64 runs")
        ok(x[1].replace(b"\n", b"") == b64.b64encode(data), "base64: as Python's")
        ok(ours("base64", "-d", stdin=x[1])[1] == data, "base64: and back")
        ok(ours("base64", "-w", "0", stdin=data)[1] == b64.b64encode(data), "base64 -w 0: one line, no newline")
        w = ours("base64", "-w", "7", stdin=data)[1]
        ok(all(len(k) <= 7 for k in w.split(b"\n")) and ours("base64", "-d", stdin=w)[1] == data, "base64 -w 7")
        if gnu:
            same("base64", ["in"], data)
            same("base64", ["-w", "13", "in"], data)
    if not gnu:
        skip("base64: exact line widths only against GNU's")
    ok(ours("base64", "-d", stdin=b"YWJj\nZGVm\n")[1] == b"abcdef", "base64 -d: newlines are passed over")
    ok(ours("base64", "-d", stdin=b"YWJj\r\nZA==\r\n")[1] == b"abcd", "base64 -d: so are a CR's")
    r = ours("base64", "-d", stdin=b"YW*j")
    ok(r[0] == 2 and b"not base64" in r[2], "base64 -d: what is no base64 is status 2")
    ok(ours("base64", "-d", stdin=b"YQ")[0] == 2, "base64 -d: cut off")
    ok(ours("base64", "-d", stdin=b"Y")[0] == 2, "base64 -d: one character")
    ok(ours("base64", "-d", stdin=b"====")[0] == 2, "base64 -d: only padding")
    ok(ours("base64", "-d", stdin=b"YQ==YQ==")[1] == b"aa", "base64 -d: joined files")
    ok(ours("base64", "--wrap=4", stdin=b"abcdef")[1] == b"YWJj\nZGVm\n", "--wrap=")
    ok(ours("base64", "--decode", stdin=b"YQ==")[1] == b"a", "--decode")
    ok(ours("base64", "a", "b")[0] == 2 and ours("base64", "gone")[0] == 2 and ours("base64", "-w", "x")[0] == 2, "base64: bad use")


def test_strings():
    data = b"\x00abc\x00abcd\x01abcde\xff\xfeprintable text\twith a tab\x00\x00\nline\nabc\n" + BINARY
    ascii_only = data.replace(b"\xc3", b".").replace(b"\xc2", b".")
    ascii_only = bytes(c if c < 0x80 else 0 for c in ascii_only)
    for args in ([], ["-n", "4"], ["-n", "2"], ["-n", "8"], ["-n", "1"], ["-6"]):
        if is_gnu("strings"):
            same("strings", args + ["in"], ascii_only)
        else:
            skip("strings: exact output only against GNU's")
    ok(ours("strings", stdin="\x00héllo\x00".encode())[1] == "héllo\n".encode(), "strings: a UTF-8 sequence is a character")
    ok(ours("strings", "-n", "3", stdin="\x00éa\x00éab\x00".encode())[1] == "éab\n".encode(), "strings: -n counts characters")
    ok(ours("strings", stdin=b"abc\xffdefg\xffxyz")[1] == b"defg\n", "strings: an invalid byte ends a run")
    ok(ours("strings", stdin=b"abcd\xc2\x85efgh")[1] == b"abcd\nefgh\n", "strings: a C1 control does too")
    ok(ours("strings", stdin=b"ab\xc3")[1] == b"", "strings: a cut sequence at the end")
    ok(ours("strings", "-n", "0")[0] == 2 and ours("strings", "-n", "x")[0] == 2, "strings: a bad length")


def tar_header(name=b"a.txt", magic=b"ustar\x0000"):
    h = bytearray(512)
    h[:len(name)] = name
    h[100:108] = b"0000644\0"
    h[108:116] = b"0000000\0"
    h[116:124] = b"0000000\0"
    h[124:136] = b"00000000000\0"
    h[136:148] = b"00000000000\0"
    h[148:156] = b" " * 8
    h[156] = 0x30
    h[257:263] = magic[:6]
    h[263:265] = magic[6:8]
    h[148:156] = b"%06o\0 " % sum(h)
    return bytes(h)


def fat_boot(total=20480, label=b"SURFTEST   "):
    b = bytearray(512)
    b[0:3] = b"\xeb\x3c\x90"
    b[3:11] = b"MSDOS5.0"
    b[11:13] = struct.pack("<H", 512)
    b[13] = 4
    b[14:16] = struct.pack("<H", 1)
    b[16] = 2
    b[17:19] = struct.pack("<H", 512)
    b[19:21] = struct.pack("<H", total)
    b[21] = 0xF8
    b[22:24] = struct.pack("<H", 20)
    b[43:54] = label
    b[54:62] = b"FAT16   "
    b[510:512] = b"\x55\xaa"
    return bytes(b)


def test_file():
    png = b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR" + struct.pack(">II", 640, 480) + b"\x08\x02\x00\x00\x00"
    bmp = b"BM" + struct.pack("<IHHI", 70, 0, 0, 54) + struct.pack("<IiiHHI", 40, 2, 2, 1, 24, 0) + b"\0" * 28
    elf = b"\x7fELF\x01\x01\x01" + b"\0" * 9 + struct.pack("<HHI", 2, 3, 1) + b"\0" * 28
    cases = [
        (png, b"PNG image data, 640 x 480"), (bmp, b"PC bitmap, 2 x 2 x 24"), (b"\xff\xd8\xff\xe0\x00\x10JFIF\x00\x01\x01" + b"\0" * 20, b"JPEG image data"),
        (b"GIF89a" + struct.pack("<HH", 16, 9) + b"\0" * 8, b"GIF image data, version 89a, 16 x 9"), (b"\x1f\x8b\x08\0" + b"\0" * 6, b"gzip compressed data"),
        (b"PK\x03\x04\x14\0\0\0\x08\0" + b"\0" * 20 + b"a.txt", b"Zip archive data"), (b"PK\x05\x06" + b"\0" * 18, b"Zip archive data"), (b"%PDF-1.7\n", b"PDF document, version 1.7"),
        (elf, b"ELF 32-bit LSB executable, Intel 80386"), (b"#!/initrd/bin/surfsh\necho\n", b"a /initrd/bin/surfsh script, ASCII text"),
        (b"#!/bin/sh\r\n", b"a /bin/sh script, ASCII text, with CRLF line terminators"), (tar_header() + b"\0" * 1024, b"POSIX tar archive"),
        (tar_header(magic=b"ustar  \0") + b"\0" * 512, b"GNU tar archive"), (tar_header(magic=b"\0" * 8), b"tar archive"),
        (fat_boot() + b"\0" * 512, b"FAT16 file system image, label SURFTEST"), (b"plain\n", b"ASCII text"), ("héllo\n".encode(), b"UTF-8 text"),
        (b"a\r\nb\r\n", b"ASCII text, with CRLF line terminators"), (b"a\rb\r", b"ASCII text, with CR line terminators"),
        (b"a\nb\r\n", b"ASCII text, with CRLF, LF line terminators"), (b"a\rb\n", b"ASCII text, with CR, LF line terminators"), (b"no newline", b"ASCII text, with no line terminators"),
        ("é".encode() * 3, b"UTF-8 text, with no line terminators"), (b"\x00\x01\x02", b"data"), (b"\xff\xfe\xfd", b"data"), (b"ab\xc3", b"data"), (b"PNG", b"ASCII text, with no line terminators"),
    ]
    with tempfile.TemporaryDirectory() as d:
        for i, (data, want) in enumerate(cases):
            put("%s/f%d" % (d, i), data)
            x = ours("file", "f%d" % i, cwd=d)
            ok(x[0] == 0 and x[1] == b"f%d: " % i + want + b"\n", "file: %r: %r" % (data[:24], x[1]))
        put(d + "/empty", b"")
        os.mkdir(d + "/dir")
        x = ours("file", "empty", "dir", "gone", cwd=d)
        ok(x[0] == 2 and x[1] == b"empty: empty\ndir: directory\n" and b"gone" in x[2], "file: empty, directory, and missing goes on")
        ok(ours("file", "-", cwd=d, stdin=b"hello\n")[1] == b"-: ASCII text\n", "file: - is standard input")
        ok(ours("file", cwd=d)[0] == 2, "file: needs a name")
        # across the block ends: a UTF-8 sequence at 4096 and 8192, a CR LF across them
        for size in (4094, 4095, 4096, 12287, 12288):
            put(d + "/edge", b"a" * size + "é\n".encode())
            ok(ours("file", "edge", cwd=d)[1] == b"edge: UTF-8 text\n", "file: a sequence across a block at %d" % size)
            put(d + "/edge", b"a" * size + b"\r\n")
            ok(ours("file", "edge", cwd=d)[1] == b"edge: ASCII text, with CRLF line terminators\n", "file: CR LF across a block at %d" % size)
        put(d + "/late", b"a\n" * 600000 + b"\0")
        ok(ours("file", "late", cwd=d)[1] == b"late: ASCII text\n", "file: only the first megabyte is looked at")
        for bits, kib, extra in ((12, 1440, []), (16, 20480, []), (32, 70000, ["-s", "1"])):
            if not real("mkfs.fat"):
                skip("file: mkfs.fat not installed")
                break
            with open(d + "/vol", "wb") as f:
                f.truncate(kib * 1024)
            run(["mkfs.fat", "-F", str(bits), "-n", "SURFVOL"] + extra + ["vol"], cwd=d)
            ok(ours("file", "vol", cwd=d)[1] == b"vol: FAT%d file system image, label SURFVOL\n" % bits, "file: a real FAT%d volume: %r" % (bits, ours("file", "vol", cwd=d)[1]))
        put(d + "/name.png", b"plain\n")
        ok(ours("file", "name.png", cwd=d)[1] == b"name.png: ASCII text\n", "file: the name means nothing")
        with zipfile.ZipFile(d + "/real.zip", "w") as z:
            z.writestr("a.txt", "hello\n")
        ok(ours("file", "real.zip", cwd=d)[1] == b"real.zip: Zip archive data\n", "file: a real zip")
        if real("file"):                # the real one's words for what is the same thing
            for i, (data, want) in enumerate(cases):
                word = {b"PNG": b"PNG", b"JPEG": b"JPEG", b"GIF": b"GIF", b"gzip": b"gzip", b"PDF": b"PDF", b"ELF": b"ELF"}.get(want.split()[0])
                if word:
                    ok(word in run(["file", "f%d" % i], cwd=d)[1], "file: the real one calls %r %r too" % (data[:12], word))
            ok(b"Zip" in run(["file", "real.zip"], cwd=d)[1], "file: and the real one calls a zip that")
        else:
            skip("file: not installed")


def test_tree():
    with tempfile.TemporaryDirectory() as d:
        put(d + "/t/b.txt", b"12345")
        put(d + "/t/a/x", b"")
        put(d + "/t/a/deep/y", b"")
        put(d + "/t/.hidden", b"")
        put(d + "/t/Z", b"")
        os.mkdir(d + "/t/empty")
        x = ours("tree", "t", cwd=d)
        want = ("t\n├── a\n│   ├── deep\n│   │   └── y\n│   └── x\n├── b.txt\n├── empty\n└── Z\n\n3 directories, 4 files\n").encode()
        ok(x[1] == want, "tree: %r" % x[1])
        ok(ours("tree", "-L", "1", "t", cwd=d)[1] == "t\n├── a\n├── b.txt\n├── empty\n└── Z\n\n2 directories, 2 files\n".encode(), "tree -L")
        ok(ours("tree", "-d", "t", cwd=d)[1] == "t\n├── a\n│   └── deep\n└── empty\n\n3 directories\n".encode(), "tree -d")
        ok(b".hidden" in ours("tree", "-a", "t", cwd=d)[1], "tree -a")
        ok("├── [          5]  b.txt\n".encode() in ours("tree", "-s", "t", cwd=d)[1], "tree -s")
        ok(ours("tree", "t/b.txt", cwd=d)[0] == 2, "tree: a file is no directory")
        ok(ours("tree", "gone", "t", cwd=d)[0] == 2, "tree: goes on after a missing one")
        ok(ours("tree", "-L", "0", "t", cwd=d)[0] == 2, "tree -L 0")
        one = ours("tree", "t/empty", cwd=d)[1]
        ok(one == b"t/empty\n\n0 directories, 0 files\n", "tree: an empty directory")
        os.chdir(d + "/t")
        ok(ours("tree", "-L", "1")[1].startswith(b".\n"), "tree: . without a directory")
        os.chdir(TOOLS)
    skip("tree: the real one is not in the image or on the Mac: expected output by hand")


def test_cal():
    months = ["%d %d" % (m, y) for y in (1753, 1900, 1999, 2000, 2024, 2026, 9999) for m in (1, 2, 3, 9, 10, 12)]
    fix = lambda s: b"\n".join(line.rstrip() for line in s.split(b"\n")).strip(b"\n")
    if real("cal") and run(["cal", "2", "2026"])[0] == 0:
        for spec in months:
            x = ours("cal", *spec.split())
            y = run(["cal"] + spec.split())
            ok(fix(x[1]) == fix(y[1]), "cal %s: %r against %r" % (spec, x[1], y[1]))
        for year in ("2026", "1999", "2024"):
            x = ours("cal", year)
            y = run(["cal", year])
            ok(fix(x[1]) == fix(y[1]), "cal %s: %r against %r" % (year, x[1][:200], y[1][:200]))
    else:
        skip("cal: not installed")
    ok(ours("cal", "11", "2026")[1] == b"   November 2026\nSu Mo Tu We Th Fr Sa\n 1  2  3  4  5  6  7\n 8  9 10 11 12 13 14\n15 16 17 18 19 20 21\n22 23 24 25 26 27 28\n29 30\n",
       "cal: a month as the Clock copies it")
    ok(ours("cal", "13", "2026")[0] == 2 and ours("cal", "0")[0] == 2 and ours("cal", "10000")[0] == 2 and ours("cal", "-x")[0] == 2 and ours("cal", "1", "2", "3")[0] == 2, "cal: bad use")
    ok(ours("cal")[0] == 0 and b"Su Mo" in ours("cal")[1], "cal: this month")
    ok(ours("cal", "9", "1752")[1].count(b"\n") == 7, "cal: no gap in 1752: Gregorian throughout")


# -- diff ------------------------------------------------------------------------------------------------------------


def strip_labels(s):
    return re.sub(rb"(?m)^(---|\+\+\+|\*\*\*) (\S+)\t.*$", rb"\1 \2", s)


def changed_lines(out):
    """lines an edit in normal form touches"""
    return sum(1 for line in out.split(b"\n") if line[:2] in (b"< ", b"> "))


def test_diff_against_real():
    have_diff, have_patch = real("diff"), real("patch")
    gnu = is_gnu("diff")
    same_text = total = 0
    with tempfile.TemporaryDirectory() as d:
        for i in range(160):
            a = text(rng.randrange(0, 60), newline=rng.random() > 0.2)
            b = edited(a, rng.randrange(2, 12))
            if i % 9 == 0:
                b = text(rng.randrange(0, 60))
            put(d + "/a", a)
            put(d + "/b", b)
            for args in ([], ["-u"], ["-c"], ["-U", "1"], ["-C", "1"], ["-U", "0"]):
                x = ours("diff", *args, "a", "b", cwd=d)
                ok(x[0] == (1 if a != b else 0), "diff %s: the status" % args)
                if have_patch and x[0] == 1:
                    put(d + "/t", a)
                    put(d + "/p", x[1])
                    y = run(["patch", "-s", "t", "-i", "p"], cwd=d)
                    if b"***************" in x[1] and not is_gnu("patch"):
                        skip("the Mac's patch misreads the context form, the Mac's own diff -c's too: no last newline, an old side left out")
                    else:
                        ok(y[0] == 0 and get(d + "/t") == b, "diff %s: the real patch gives the second file: %r %r" % (args, y, x[1][:300]))
                if have_diff:
                    y = run(["diff"] + args + ["a", "b"], cwd=d)
                    ok(x[0] == y[0], "diff %s: status as the real one's" % args)
                    if gnu and x[0] == 1:
                        total += 1
                        same_text += strip_labels(x[1]) == strip_labels(y[1])
                    if x[0] == 1 and not args:
                        ok(changed_lines(x[1]) <= changed_lines(run(["diff", "a", "b"], cwd=d)[1]), "as short as the real one")
                    # the real diff's text through our patch
                    if y[0] == 1:
                        put(d + "/t", a)
                        put(d + "/p", y[1])
                        z = ours("patch", "t", "p", cwd=d)
                        if get(d + "/t") != b and not gnu:
                            skip("patch: the Mac's diff marks a missing newline where there is none (its bug)")
                        else:
                            ok(z[0] == 0 and get(d + "/t") == b, "the real diff %s through our patch: %r\n%r" % (args, z, y[1][:300]))
            if gnu:
                y = run(["diff", "--minimal", "a", "b"], cwd=d)
                x = ours("diff", "a", "b", cwd=d)
                ok(changed_lines(x[1]) == changed_lines(y[1]), "diff: as short as --minimal")
    if gnu:
        print("test_cli_diff: the same text as GNU diff on %d of %d (the rest are other edits, as short: --minimal is checked)" % (same_text, total))
    else:
        skip("diff: exact text only against GNU's")


def test_diff_options():
    with tempfile.TemporaryDirectory() as d:
        put(d + "/a", b"Hello  World\n\nkeep\n\n\nthe end\n")
        put(d + "/b", b"hello world \nkeep\nthe end\nmore\n")
        for args in (["-i"], ["-b"], ["-w"], ["-B"], ["-i", "-b"], ["-i", "-w"], ["-w", "-B"], ["-q"], ["-i", "-w", "-B", "-q"], ["-u", "-w"], ["-c", "-B"], ["-u", "-B", "-w"]):
            x = ours("diff", *args, "a", "b", cwd=d)
            if is_gnu("diff"):
                y = run(["diff"] + args + ["a", "b"], cwd=d)
                ok(x[0] == y[0] and strip_labels(x[1]) == strip_labels(y[1]), "diff %s: %r against %r" % (args, x[1], y[1]))
        if not is_gnu("diff"):
            skip("diff: options' exact text only against GNU's")
        put(d + "/c", b"a\n\nb\n\nc\n")
        put(d + "/e", b"a\nb\nc\n")
        ok(ours("diff", "-B", "c", "e", cwd=d)[0] == 0, "diff -B: only blank lines: no difference")
        ok(ours("diff", "-q", "-B", "c", "e", cwd=d)[0] == 0, "diff -q -B")
        r = ours("diff", "-q", "c", "e", cwd=d)
        ok(r[0] == 1 and r[1] == b"Files c and e differ\n", "diff -q")
        ok(ours("diff", "-u", "5", "c", "e", cwd=d)[1].count(b"\n") >= 5 and ours("diff", "-u5", "c", "e", cwd=d)[0] == 1, "-u 5 and -u5")
        put(d + "/5", b"x\n")
        ok(ours("diff", "-u", "5", "c", cwd=d)[0] == 1, "diff -u 5 c: with two left they are the files")
        put(d + "/x1", b"a\0b\n")
        put(d + "/x2", b"a\0c\n")
        r = ours("diff", "x1", "x2", cwd=d)
        ok(r[0] == 1 and r[1] == b"Binary files x1 and x2 differ\n", "Binary files differ")
        ok(ours("diff", "x1", "x1", cwd=d)[0] == 0, "the same binary file")
        r = ours("diff", "-", "c", cwd=d, stdin=get(d + "/e"))
        ok(r[0] == 1 and r[1] == b"1a2\n> \n2a4\n> \n", "diff: - is standard input: %r" % (r,))
        ok(ours("diff", "-", "-", cwd=d, stdin=b"x")[0] == 0, "diff - -")
        ok(ours("diff", "c", os.devnull, cwd=d)[1].startswith(b"1,5d0\n"), "diff against /dev/null")
        ok(ours("diff", "gone", "c", cwd=d)[0] == 2, "diff: a missing file is status 2")
        ok(ours("diff", "c", cwd=d)[0] == 2 and ours("diff", "-z", "c", "e", cwd=d)[0] == 2, "diff: usage")
        ok(ours("diff", "-U", "x", "c", "e", cwd=d)[0] == 2, "diff: a bad count")
        # a file against a directory
        os.mkdir(d + "/dir")
        put(d + "/dir/c", b"a\nb\n")
        ok(ours("diff", "c", "dir", cwd=d)[0] == 1 and ours("diff", "dir", "c", cwd=d)[0] == 1, "a file against a directory: the one of its name")
        ok(ours("diff", "-", "dir", cwd=d)[0] == 2, "standard input against a directory")
        # huge common ends: only the middle is read
        big = b"".join(b"line %d\n" % i for i in range(200000))
        put(d + "/big1", big)
        put(d + "/big2", big.replace(b"line 100000\n", b"LINE\n"))
        r = ours("diff", "-u", "big1", "big2", cwd=d)
        ok(r[0] == 1 and b"-line 100000\n+LINE\n" in r[1] and b"@@ -99998,7 +99998,7 @@" in r[1], "diff: the middle of a big file, with right numbers")
        # nothing in common at all
        put(d + "/other", b"".join(b"other %d\n" % i for i in range(200000)))
        r = ours("diff", "big1", "other", cwd=d)
        ok(r[0] == 1 and r[1].startswith(b"1,200000c1,200000\n"), "diff: two big files with nothing in common, at once")


def test_diff_trees():
    with tempfile.TemporaryDirectory() as d:
        put(d + "/a/same", b"s\n")
        put(d + "/b/same", b"s\n")
        put(d + "/a/diff", b"1\n")
        put(d + "/b/diff", b"2\n")
        put(d + "/a/onlya", b"")
        put(d + "/b/onlyb", b"")
        put(d + "/a/sub/deep", b"x\n")
        put(d + "/b/sub/deep", b"y\n")
        put(d + "/a/sub/lone", b"")
        put(d + "/a/kind", b"")
        put(d + "/b/kind/x", b"")
        put(d + "/a/bin", b"\0a")
        put(d + "/b/bin", b"\0b")
        for args in (["-r", "a", "b"], ["-r", "-q", "a", "b"], ["-ru", "a", "b"], ["a", "b"], ["-c", "-r", "a", "b"]):
            x = ours("diff", *args, cwd=d)
            ok(x[0] == 1, "diff %s: status 1" % args)
            if is_gnu("diff"):
                y = run(["diff"] + args, cwd=d)
                ok(strip_labels(x[1]) == strip_labels(y[1]), "diff %s:\n%s\nagainst\n%s" % (args, x[1].decode(), y[1].decode()))
        if not is_gnu("diff"):
            skip("diff -r: exact text only against GNU's")
        put(d + "/s/x", b"1\n")
        put(d + "/s2/x", b"1\n")
        ok(ours("diff", "-r", "s", "s2", cwd=d)[0] == 0, "the same trees")
        put(d + "/c/README.TXT", b"1\n")
        put(d + "/e/readme.txt", b"1\n")
        ok(ours("diff", "-r", "c", "e", cwd=d)[0] == 0, "names pair up without regard to case, as on FAT")


# -- patch -----------------------------------------------------------------------------------------------------------

BASE = "".join("line %d\n" % i for i in range(1, 31))
UNI = "--- old/f.txt\t2026-01-01 00:00:00\n+++ new/f.txt\t2026-01-02 00:00:00\n@@ -14,7 +14,7 @@\n line 14\n line 15\n line 16\n-line 17\n+LINE 17\n line 18\n line 19\n line 20\n"


def patch_case(files, patch, *args, stdin=None, cwd_sub=None):
    d = tempfile.mkdtemp(dir=SCRATCH.name)
    for name, data in files.items():
        put(os.path.join(d, name), data)
    put(d + "/the.patch", patch)
    argv = list(args) if stdin is False else list(args) + ["-i", os.path.join(d, "the.patch")]
    r = ours("patch", *argv, cwd=os.path.join(d, cwd_sub) if cwd_sub else d, stdin=patch.encode() if stdin is False and isinstance(patch, str) else b"")
    return d, r


def tree_of(d):
    out = {}
    for root, _, names in os.walk(d):
        for n in names:
            if n != "the.patch":
                p = os.path.join(root, n)
                out[os.path.relpath(p, d)] = get(p)
    return out


def test_patch_files():
    new = BASE.replace("line 17\n", "LINE 17\n").encode()
    d, r = patch_case({"f.txt": BASE}, UNI)
    ok(r[0] == 0 and tree_of(d) == {"f.txt": new} and r[1] == b"patching file f.txt\n", "patch: without -p the last part of the name: %r" % (r,))
    d, r = patch_case({"new/f.txt": BASE}, UNI, "-p0")
    ok(r[0] == 0 and get(d + "/new/f.txt") == new, "patch -p0: the first name that exists")
    d, r = patch_case({"f.txt": BASE}, UNI, "-p1")
    ok(r[0] == 0 and get(d + "/f.txt") == new, "patch -p1")
    d, r = patch_case({"f.txt": BASE}, UNI, "-p5")
    ok(r[0] == 1 and tree_of(d) == {"f.txt": BASE.encode()}, "patch -p5: no name left: %r" % (r,))
    d, r = patch_case({"f.txt": BASE}, UNI, "--strip=1", stdin=False)
    ok(r[0] == 0 and get(d + "/f.txt") == new, "patch reads standard input, --strip=")
    d, r = patch_case({"other": BASE}, UNI, "other")
    ok(r[0] == 0 and get(d + "/other") == new, "patch file: what the patch names does not matter")
    d, r = patch_case({"sub/f.txt": BASE}, UNI, "-d", "sub")
    ok(r[0] == 0 and get(d + "/sub/f.txt") == new, "patch -d")
    d, r = patch_case({"f.txt": BASE}, UNI, "-d", "gone")
    ok(r[0] == 2, "patch -d: no such directory")
    d, r = patch_case({"f.txt": new}, UNI, "-R")
    ok(r[0] == 0 and get(d + "/f.txt") == BASE.encode(), "patch -R")
    d, r = patch_case({"f.txt": BASE}, UNI, "--dry-run")
    ok(r[0] == 0 and tree_of(d) == {"f.txt": BASE.encode()} and b"checking file f.txt" in r[1], "patch --dry-run changes nothing")
    d, r = patch_case({"f.txt": BASE}, UNI, "-o", "out")
    ok(r[0] == 0 and tree_of(d) == {"f.txt": BASE.encode(), "out": new}, "patch -o: the file stays")
    d, r = patch_case({"f.txt": BASE, "out": "was here\n"}, UNI, "-o", "out")
    ok(r[0] == 0 and get(d + "/out") == new and tree_of(d).keys() == {"f.txt", "out"}, "patch -o over a file: replaced, nothing left beside it")
    d, r = patch_case({"f.txt": BASE}, UNI, "-o", "-")
    ok(r[0] == 0 and r[1] == new and b"patching file" in r[2], "patch -o -: the result on standard output, the talk on standard error")
    d, r = patch_case({"f.txt": BASE}, UNI, "--dry-run", "-o", "out")
    ok(r[0] == 0 and tree_of(d) == {"f.txt": BASE.encode()}, "patch --dry-run -o")
    # a file already patched: skipped, whatever -N says
    for flags in ([], ["-N"]):
        d, r = patch_case({"f.txt": new}, UNI, *flags)
        ok(r[0] == 1 and get(d + "/f.txt") == new and b"Reversed (or previously applied)" in r[1] and "f.txt.rej" in tree_of(d), "patch %s: already applied: %r" % (flags, r))
    d, r = patch_case({"f.txt": new}, UNI, "-f")
    ok(r[0] == 1 and b"Reversed" not in r[1] and b"FAILED" in r[1] and "f.txt.rej" in tree_of(d), "patch -f: no such look")
    # a hunk that fits nowhere
    other = BASE.replace("line 17\n", "something else\n")
    d, r = patch_case({"f.txt": other}, UNI)
    t = tree_of(d)
    ok(r[0] == 1 and t["f.txt"] == other.encode() and b"@@ -14,7 +14,7 @@" in t["f.txt.rej"] and b"-line 17\n+LINE 17\n" in t["f.txt.rej"], "a .rej: %r" % (r,))
    ok(b"1 out of 1 hunk FAILED -- saving rejects to file f.txt.rej" in r[1], "and it is said")
    d, r = patch_case({"f.txt": other}, UNI, "--dry-run")
    ok(r[0] == 1 and tree_of(d) == {"f.txt": other.encode()}, "--dry-run: no .rej either")
    rej = "--- a\n+++ b\n@@ -1 +1 @@\n-z\n+y\n"
    d, r = patch_case({"f.txt": other, "f.txt.rej": "old rejects\n"}, UNI)
    ok(r[0] == 1 and get(d + "/f.txt.rej") == b"old rejects\n" and b"-line 17" in get(d + "/f.txt.1.rej"), "a .rej there before stays")
    # part of a patch goes in
    two = UNI + "@@ -24,3 +24,3 @@\n-nothing like it\n+no\n line 25\n line 26\n"
    d, r = patch_case({"f.txt": BASE}, two)
    t = tree_of(d)
    ok(r[0] == 1 and t["f.txt"] == new and b"nothing like it" in t["f.txt.rej"] and b"line 17" not in t["f.txt.rej"], "one hunk in, one in the .rej")
    # several files in one patch
    multi = UNI + "--- x/g.txt\n+++ y/g.txt\n@@ -1 +1 @@\n-a\n+b\n"
    d, r = patch_case({"f.txt": BASE, "g.txt": "a\n"}, multi)
    ok(r[0] == 0 and get(d + "/f.txt") == new and get(d + "/g.txt") == b"b\n", "two files in a patch")
    d, r = patch_case({"f.txt": BASE, "g.txt": "a\n"}, multi, "-o", "out")
    ok(r[0] == 0 and get(d + "/out") == new + b"b\n", "patch -o: one after the other")
    # made and removed through /dev/null
    make = "--- /dev/null\n+++ b/sub/dir/new.txt\n@@ -0,0 +1,2 @@\n+one\n+two\n"
    d, r = patch_case({}, make, "-p1")
    ok(r[0] == 0 and tree_of(d) == {"sub/dir/new.txt": b"one\ntwo\n"}, "a file made, with its directories: %r" % (r,))
    d, r = patch_case({"sub/dir/new.txt": "one\ntwo\n"}, make, "-p1", "-R")
    ok(r[0] == 0 and tree_of(d) == {}, "-R removes what it made")
    d, r = patch_case({"new.txt": "mine\n"}, "--- /dev/null\n+++ new.txt\n@@ -0,0 +1 @@\n+one\n")
    ok(r[0] == 1 and get(d + "/new.txt") == b"mine\n", "a file to be made that is already there is left alone")
    d, r = patch_case({"new.txt": ""}, "--- /dev/null\n+++ new.txt\n@@ -0,0 +1 @@\n+one\n")
    ok(r[0] == 0 and get(d + "/new.txt") == b"one\n", "an empty one is filled")
    gone = "--- a/old.txt\n+++ /dev/null\n@@ -1,2 +0,0 @@\n-one\n-two\n"
    d, r = patch_case({"old.txt": "one\ntwo\n"}, gone, "-p1")
    ok(r[0] == 0 and tree_of(d) == {}, "a file removed")
    d, r = patch_case({"old.txt": "one\ntwo\n"}, gone, "-p1", "--dry-run")
    ok(r[0] == 0 and tree_of(d) == {"old.txt": b"one\ntwo\n"}, "--dry-run removes nothing")
    d, r = patch_case({"old.txt": "one\ntwo\nthree\n"}, gone, "-p1")
    ok(r[0] == 1 and tree_of(d) == {"old.txt": b"three\n"} and b"Not deleting" in r[1], "more in it than the patch had: it stays, with the rest")
    d, r = patch_case({"old.txt": "x\n"}, gone, "-p1")
    ok(r[0] == 1 and get(d + "/old.txt") == b"x\n", "one that does not match is not removed")
    d, r = patch_case({}, "--- /dev/null\n+++ /dev/null\n@@ -0,0 +1 @@\n+x\n")
    ok(r[0] == 1 and tree_of(d) == {}, "no name at all")
    # refused names
    for bad in ("/etc/passwd", "../outside", "a/../../outside", "..", "/dev/null/x"):
        p = "--- %s\n+++ %s\n@@ -1 +1 @@\n-a\n+b\n" % (bad, bad)
        d, r = patch_case({"outside": "a\n"}, p, "-p0", cwd_sub=None)
        ok(r[0] == 2 and b"refused" in r[2] and get(d + "/outside") == b"a\n", "patch -p0 refuses %s: %r" % (bad, r))
    d, r = patch_case({"x": "a\n"}, "--- ../../x\n+++ ../../x\n@@ -1 +1 @@\n-a\n+b\n", "-p2")
    ok(r[0] == 0 and get(d + "/x") == b"b\n", "what -p takes off does not count: the name left is x")
    make = "--- /dev/null\n+++ ../made\n@@ -0,0 +1 @@\n+x\n"
    d, r = patch_case({}, make, "-p0", cwd_sub=None)
    ok(r[0] == 2 and not os.path.exists(d + "/../made"), "a new file outside is refused")
    d, r = patch_case({}, "--- /dev/null\n+++ /tmp/cli-diff-made\n@@ -0,0 +1 @@\n+x\n", "-p0")
    ok(r[0] == 2 and not os.path.exists("/tmp/cli-diff-made"), "an absolute new file is refused")
    # what is around it, and what is wrong with it
    d, r = patch_case({"f.txt": BASE}, "some mail\n\n" + UNI + "-- \nsignature\n")
    ok(r[0] == 0, "text around it")
    d, r = patch_case({"f.txt": BASE}, "nothing here\n")
    ok(r[0] == 2 and b"garbage" in r[2], "only garbage is status 2")
    d, r = patch_case({"f.txt": BASE}, "")
    ok(r[0] == 0, "an empty patch")
    d, r = patch_case({"f.txt": BASE}, UNI[:-30])
    ok(r[0] == 2 and tree_of(d) == {"f.txt": BASE.encode()} and b"line" in r[2], "a cut hunk: status 2, and the file as it was: %r" % (r,))
    d, r = patch_case({"f.txt": BASE, "g.txt": "a\n"}, multi[:-5])
    ok(r[0] == 2 and get(d + "/f.txt") == new and get(d + "/g.txt") == b"a\n", "the first file done, the damaged one left alone")
    d, r = patch_case({}, UNI)
    ok(r[0] == 1 and tree_of(d) == {}, "a file not there: nothing made: %r" % (r,))
    d, r = patch_case({"d/x": "a\n"}, UNI, "d")
    ok(r[0] == 2 and tree_of(d) == {"d/x": b"a\n"}, "a directory is no file to patch: %r" % (r,))
    ok(ours("patch", "-i", "gone", cwd=tempfile.gettempdir())[0] == 2, "no such patch")
    ok(ours("patch", "a", "b", "c")[0] == 2 and ours("patch", "--what")[0] == 2 and ours("patch", "-p", "x")[0] == 2, "patch: usage")
    # names
    d, r = patch_case({"a b.txt": "a\n"}, "--- a b.txt\t2026\n+++ a b.txt\t2026\n@@ -1 +1 @@\n-a\n+b\n", "-p0")
    ok(r[0] == 0 and get(d + "/a b.txt") == b"b\n", "a name with a space, up to the TAB")
    d, r = patch_case({"a b.txt": "a\n"}, "--- a b.txt 2026-01-01 00:00:00\n+++ a b.txt 2026-01-01 00:00:00\n@@ -1 +1 @@\n-a\n+b\n", "-p0")
    ok(r[0] == 0 and get(d + "/a b.txt") == b"b\n", "a name with a space, up to the date")
    d, r = patch_case({"f.txt": BASE}, "Index: f.txt\n" + "@@ -14,7 +14,7 @@\n" + UNI.split("@@\n", 1)[1])
    ok(r[0] == 0 and get(d + "/f.txt") == new, "Index: names the file of hunks with no headers")
    normal = "diff -r old/f.txt new/f.txt\n17c17\n< line 17\n---\n> LINE 17\n"
    d, r = patch_case({"f.txt": BASE}, normal)
    ok(r[0] == 0 and get(d + "/f.txt") == new, "diff -r's line names the file of a normal diff")
    d, r = patch_case({"f.txt": BASE}, "17c17\n< line 17\n---\n> LINE 17\n")
    ok(r[0] == 1 and b"no file name" in r[2], "a normal diff names nothing: %r" % (r,))
    d, r = patch_case({"f.txt": BASE}, "17c17\n< line 17\n---\n> LINE 17\n", "f.txt")
    ok(r[0] == 0 and get(d + "/f.txt") == new, "file gives it")


def test_patch_saving():
    new = BASE.replace("line 17\n", "LINE 17\n").encode()
    # the user's own files of the new names are not touched
    d, r = patch_case({"f.txt": BASE, "f.txt.tmp": "mine\n"}, UNI)
    ok(r[0] == 0 and get(d + "/f.txt") == new and get(d + "/f.txt.tmp") == b"mine\n", "a file of the new file's name stays: %r" % (r,))
    # a twin: the same size and time beside it, what a half-done rename leaves
    d = tempfile.mkdtemp(dir=SCRATCH.name)
    put(d + "/f.txt", BASE)
    put(d + "/f.txt.tmp", BASE)
    st = os.stat(d + "/f.txt")
    os.utime(d + "/f.txt.tmp", (st.st_atime, st.st_mtime))
    put(d + "/the.patch", UNI)
    r = ours("patch", "-i", "the.patch", cwd=d)
    ok(r[0] == 2 and b"may share its data" in r[2] and get(d + "/f.txt") == BASE.encode(), "a likely twin: left alone, and said: %r" % (r,))
    # pulled at each write, fsync and rename: the file is whole and old, or whole and new, and nothing is left beside it
    many = "".join("line %d of a file too big for one buffer of patch's output\n" % i for i in range(3000))
    hunks = ""
    for at in range(100, 3000, 700):
        hunks += "@@ -%d,1 +%d,1 @@\n-line %d of a file too big for one buffer of patch's output\n+changed line %d\n" % (at, at, at - 1, at - 1)
    p = "--- f.txt\n+++ f.txt\n" + hunks
    done = many
    for at in range(100, 3000, 700):
        done = done.replace("line %d of a file too big for one buffer of patch's output\n" % (at - 1), "changed line %d\n" % (at - 1))
    for op, error in (("write", 28), ("write", 5), ("fsync", 5), ("rename", 28)):
        for n in range(1, 12):
            d = tempfile.mkdtemp(dir=SCRATCH.name)
            put(d + "/f.txt", many)
            put(d + "/the.patch", p)
            r = ours("patch", "-i", "the.patch", cwd=d, env=dict(ENV, CLI_FAIL="%s:%d:%d" % (op, n, error)))
            if r[0] == 0:
                ok(tree_of(d) == {"f.txt": done.encode()}, "%s %d: all done" % (op, n))
                break
            ok(r[0] == 2 and tree_of(d) == {"f.txt": many.encode()}, "pulled at %s %d (%d): the old file, whole, and nothing else: %r %r" % (op, n, error, r[0], list(tree_of(d))))
    # a rename that may have half happened: nothing is deleted, and it says where the new contents are
    d = tempfile.mkdtemp(dir=SCRATCH.name)
    put(d + "/f.txt", BASE)
    put(d + "/the.patch", UNI)
    r = ours("patch", "-i", "the.patch", cwd=d, env=dict(ENV, CLI_FAIL="rename:1:5"))
    t = tree_of(d)
    ok(r[0] == 2 and t["f.txt"] == BASE.encode() and t.get("f.txt.tmp") == new and b"check the disk" in r[2], "a rename that may be half done: both stay: %r" % (r,))
    # a new file pulled at any write is gone again
    big = "".join("+a line of a new file that needs more than one block of output, %d\n" % i for i in range(1500))
    p = "--- /dev/null\n+++ new.txt\n@@ -0,0 +1,1500 @@\n" + big
    for op in ("write", "fsync"):
        for n in (1, 2, 3):
            d = tempfile.mkdtemp(dir=SCRATCH.name)
            put(d + "/the.patch", p)
            r = ours("patch", "-p0", "-i", "the.patch", cwd=d, env=dict(ENV, CLI_FAIL="%s:%d" % (op, n)))
            if r[0] == 0:
                break
            ok(r[0] == 2 and tree_of(d) == {}, "a new file pulled at %s %d is gone again: %r" % (op, n, r))
    # the .rej pulled: the status says so
    other = BASE.replace("line 17\n", "something else\n")
    d = tempfile.mkdtemp(dir=SCRATCH.name)
    put(d + "/f.txt", other)
    put(d + "/the.patch", UNI)
    r = ours("patch", "-i", "the.patch", cwd=d, env=dict(ENV, CLI_FAIL="write:1"))
    ok(r[0] == 2 and tree_of(d) == {"f.txt": other.encode()}, "a .rej that cannot be written: said, and nothing half left: %r" % (r,))


stricter = [0]


def test_patch_against_real():
    if not is_gnu("patch"):
        skip("patch: its messages and fuzz only against GNU's")
    have = real("patch")
    if not have:
        skip("patch: not installed")
        return
    with tempfile.TemporaryDirectory() as d:
        moved = 0
        for i in range(150):
            a = text(rng.randrange(5, 60))
            b = edited(a, rng.randrange(3, 12))
            put(d + "/a", a)
            put(d + "/b", b)
            # not less context: GNU's fuzz takes all of one line's and then puts a hunk where it says, unseen. Ours will not
            for args in (["-u"], ["-c"]):
                y = run(["diff"] + args + ["a", "b"], cwd=d)
                if y[0] != 1:
                    continue
                put(d + "/p", y[1])
                # the file has moved on: lines added at the top or some lines changed
                t = (b"new first\n" * rng.randrange(0, 4)) + a
                if i % 3 == 0:
                    t = edited(t, 30)
                put(d + "/t1", t)
                put(d + "/t2", t)
                x = ours("patch", "t1", "p", "-f", cwd=d)
                z = run(["patch", "-f", "t2", "p"], cwd=d)
                moved += x[0] == 0 and t != a
                orig1, orig2 = os.path.exists(d + "/t1.orig"), os.path.exists(d + "/t2.orig")
                same_orig = not orig1 or not orig2 or get(d + "/t1.orig") == get(d + "/t2.orig")
                for extra in ("/t1.rej", "/t2.rej", "/t1.orig", "/t2.orig"):
                    if os.path.exists(d + extra):
                        os.unlink(d + extra)
                if is_gnu("patch"):
                    # The same decisions, the same result: file, status, messages and .orig. Ours may refuse more: a hunk made at an
                    # edge of the file stays there at any fuzz here, where GNU's fuzz moves it. Never the other way round.
                    ours_failed = set(re.findall(rb"Hunk #(\d+) FAILED", x[1]))
                    theirs_failed = set(re.findall(rb"Hunk #(\d+) FAILED", z[1]))
                    if ours_failed == theirs_failed:
                        ok(same_orig and (x[0] != 0 or orig1 == orig2), "the .orig against GNU's %s: ours %r theirs %r" % (args, orig1, orig2))
                        ok(x[0] == z[0] and get(d + "/t1") == get(d + "/t2") and x[1].replace(b"t1", b"t2") == z[1],
                           "patch against GNU's %s: ours %r theirs %r\n--ours\n%r\n--theirs\n%r\n--patch\n%r\n--target\n%r" %
                           (args, x, z, get(d + "/t1")[:300], get(d + "/t2")[:300], y[1][:400], t[:300]))
                    else:
                        stricter[0] += 1
                        ok(theirs_failed < ours_failed, "patch placed a hunk GNU's could not %s: ours %r theirs %r" % (args, x, z))
        ok(moved > 0, "some patches went in with the file moved on")
        if is_gnu("patch"):
            print("test_cli_diff: patch refused hunks GNU's placed in %d runs (an edge of the file kept), and agreed in all the rest" % stricter[0])


def test_property_real_files():
    """generated pairs on real files, forward and with -R, and every form"""
    with tempfile.TemporaryDirectory() as d:
        for i in range(120):
            a = text(rng.randrange(0, 80), newline=rng.random() > 0.3)
            b = edited(a, rng.randrange(2, 9))
            if i % 7 == 0:
                b = b[:-1] if b.endswith(b"\n") else b + b"\n"
            for style in ([], ["-u"], ["-c"], ["-U", "0"], ["-C", "0"], ["-U", "7"]):
                put(d + "/a", a)
                put(d + "/b", b)
                x = ours("diff", *style, "a", "b", cwd=d)
                if a == b:
                    ok(x[0] == 0, "the same")
                    continue
                put(d + "/p", x[1])
                put(d + "/t", a)
                r = ours("patch", "t", "p", cwd=d)
                ok(r[0] == 0 and get(d + "/t") == b, "diff %s then patch: %r" % (style, r))
                r = ours("patch", "-R", "t", "p", cwd=d)
                ok(r[0] == 0 and get(d + "/t") == a, "and -R: %r" % (r,))


# -- the review of 114ac02 --------------------------------------------------------------------------------------------

def test_review_output_file():
    """1: -o must not take a file from the user unless the whole result is there"""
    bad = "--- foo\n+++ foo\n@@ -1 +1 @@\n-nothere\n+b\n"
    good = "--- foo\n+++ foo\n@@ -1 +1 @@\n-a\n+b\n"
    d, r = patch_case({"foo": "a\n", "out": "PRECIOUS\n"}, bad, "-o", "out", "foo")
    ok(r[0] == 1 and get(d + "/out") == b"PRECIOUS\n", "-o over a file, a hunk fails: it stays: %r" % (r,))
    d, r = patch_case({"foo": "a\n", "out": "PRECIOUS\n"}, "", "-o", "out", "foo")
    ok(r[0] == 0 and get(d + "/out") == b"PRECIOUS\n", "-o, an empty patch: the file stays: %r" % (r,))
    d, r = patch_case({"out": "PRECIOUS\n"}, good, "-o", "out")
    ok(r[0] == 1 and get(d + "/out") == b"PRECIOUS\n", "-o, the file to patch is not there: it stays: %r" % (r,))
    d, r = patch_case({"foo": "b\n", "out": "PRECIOUS\n"}, good, "-o", "out", "foo")
    ok(r[0] == 1 and get(d + "/out") == b"PRECIOUS\n", "-o, a reversed patch: it stays: %r" % (r,))
    d, r = patch_case({"g": "keep\nme\n"}, "--- g\n+++ g\n@@ -1 +1 @@\n-nothere\n+b\n", "-o", "g", "g")
    ok(r[0] == 1 and get(d + "/g") == b"keep\nme\n", "-o naming the input, a hunk fails: it stays: %r" % (r,))
    d, r = patch_case({"foo": "a\n"}, bad, "-o", "new", "foo")
    ok(r[0] == 1 and not os.path.exists(d + "/new"), "-o, a new file, a hunk fails: none is made: %r" % (r,))
    d, r = patch_case({"foo": "a\n"}, "", "-o", "new", "foo")
    ok(r[0] == 0 and not os.path.exists(d + "/new"), "-o, a new file, an empty patch: none is made: %r" % (r,))
    part = good + "@@ -3 +3 @@\n-nothere\n+c\n"
    d, r = patch_case({"foo": "a\nx\ny\n", "out": "PRECIOUS\n"}, part, "-o", "out", "foo")
    ok(r[0] == 1 and get(d + "/out") == b"PRECIOUS\n", "-o, one hunk in and one not: no partial result: %r" % (r,))
    d, r = patch_case({"f": "a\n", "out": "PRECIOUS-AND-LONG\n"}, "--- f\n+++ f\n@@ -1 +1 @@\n-a\n+b\n--- ../x\n+++ ../x\n@@ -1 +1 @@\n-a\n+b\n", "-p0", "-o", "out")
    ok(r[0] == 2 and get(d + "/out") == b"PRECIOUS-AND-LONG\n", "-o, a name refused after a file went in: no partial result: %r" % (r,))
    d, r = patch_case({"foo": "a\n", "out": "old\n"}, good, "-o", "out", "foo")
    ok(r[0] == 0 and get(d + "/out") == b"b\n" and tree_of(d).keys() == {"foo", "out"}, "-o, all well: replaced: %r" % (r,))
    d, r = patch_case({"g": "a\n"}, "--- g\n+++ g\n@@ -1 +1 @@\n-a\n+b\n", "-o", "g", "g")
    ok(r[0] == 0 and get(d + "/g") == b"b\n" and tree_of(d).keys() == {"g"}, "-o naming the input, all well: it is the result, and nothing is left beside it: %r" % (r,))
    d, r = patch_case({"foo": "a\n", "out": "PRECIOUS\n"}, bad, "-o", "out", "foo")
    ok(b"out" in r[2] and b"not" in r[2], "-o left alone: it is said: %r" % (r,))


def test_review_reversed_first():
    """5: at each fuzz, forward and then reversed, so a patch applied twice does not land in the next block"""
    block = "x=1\ny=2\nz=3\nw=4\nv=5\nu=6\n"
    cfg = "[alpha]\n" + block + "[beta]\n" + block
    new = cfg.replace("z=3", "z=9", 1)
    patch = "--- cfg\n+++ cfg\n@@ -1,7 +1,7 @@\n [alpha]\n x=1\n y=2\n-z=3\n+z=9\n w=4\n v=5\n u=6\n"
    for flags in ([], ["-N"]):
        d, r = patch_case({"cfg": new}, patch, *flags)
        ok(r[0] == 1 and get(d + "/cfg") == new.encode() and b"Reversed (or previously applied)" in r[1], "patch %s applied twice: nothing changed: %r" % (flags, r))
    d, r = patch_case({"cfg": new}, patch, "-f")
    ok(get(d + "/cfg") != new.encode() or r[0] == 1, "-f: no such look")
    d, r = patch_case({"cfg": cfg}, patch)
    ok(r[0] == 0 and get(d + "/cfg") == new.encode(), "the first time it goes in")


def test_review_rej():
    """6: a .rej adds up within a run, and one that was there before stays"""
    two = "--- f\n+++ f\n@@ -1 +1 @@\n-no\n+one\n--- f\n+++ f\n@@ -1 +1 @@\n-nor\n+two\n"
    d, r = patch_case({"f": "a\n"}, two, "-f")
    rej = get(d + "/f.rej") if os.path.exists(d + "/f.rej") else b""
    ok(r[0] == 1 and b"-no\n" in rej and b"-nor\n" in rej, "two failing sections of one file: both in the .rej: %r" % (rej,))
    d, r = patch_case({"notes.rej": "my rejected ideas\n"}, "--- /dev/null\n+++ notes\n@@ -1 +1 @@\n-x\n+y\n", "-f")
    t = tree_of(d)
    ok(t.get("notes.rej") == b"my rejected ideas\n", "a .rej of the user's is not replaced: %r" % (t,))
    ok(b"-x\n" in b"".join(v for k, v in t.items() if k != "notes.rej"), "the rejects are in another file: %r" % (t,))
    ok(b"notes.1.rej" in r[1] + r[2], "and it says which: %r" % (r,))
    d, r = patch_case({"f": "a\n", "f.rej": "mine\n"}, two, "-f")
    t = tree_of(d)
    ok(t["f.rej"] == b"mine\n" and b"-no\n" in t.get("f.1.rej", b"") and b"-nor\n" in t.get("f.1.rej", b""), "both sections go to the one new name: %r" % (t,))


def test_review_small_patch_things():
    # 7: a file named on the command line is no way round "already there"
    make = "--- /dev/null\n+++ foo\n@@ -0,0 +1 @@\n+x\n"
    d, r = patch_case({"foo": "mine\n"}, make, "foo")
    ok(r[0] == 1 and get(d + "/foo") == b"mine\n", "a patch that makes a file, on a named file with text in it: refused: %r" % (r,))
    # 8: more in it than the patch had: not all went in
    gone = "--- a/old.txt\n+++ /dev/null\n@@ -1,2 +0,0 @@\n-one\n-two\n"
    d, r = patch_case({"old.txt": "one\ntwo\nthree\n"}, gone, "-p1")
    ok(r[0] == 1 and b"Not deleting" in r[1], "a file with more in it than the patch had: status 1: %r" % (r,))
    # 9: a word after a space is no date
    p = "--- My Doc About cats.txt\n+++ My Doc About cats.txt\n@@ -1 +1 @@\n-a\n+b\n"
    d, r = patch_case({"My Doc About cats.txt": "a\n", "My": "a\n"}, p, "-p0")
    ok(r[0] == 0 and get(d + "/My Doc About cats.txt") == b"b\n" and get(d + "/My") == b"a\n", "a name with words in it is the whole name: %r" % (r,))
    p = "--- a b.txt Sun Nov  1 15:30:27 2026\n+++ a b.txt Sun Nov  1 15:30:27 2026\n@@ -1 +1 @@\n-a\n+b\n"
    d, r = patch_case({"a b.txt": "a\n"}, p, "-p0")
    ok(r[0] == 0 and get(d + "/a b.txt") == b"b\n", "a real date still ends a name: %r" % (r,))
    # a CRLF patch's names
    d, r = patch_case({}, "--- /dev/null\r\n+++ made\r\n@@ -0,0 +1 @@\r\n+x\r\n", "-p0")
    ok(r[0] == 0 and os.path.exists(d + "/made") and not os.path.exists(d + "/made\r"), "no CR in a name: %r" % (sorted(tree_of(d)),))
    # lines past a hunk's counts are said
    p = "--- f\n+++ f\n@@ -1 +1 @@\n-a\n+b\n+and this one, past the counts\n"
    d, r = patch_case({"f": "a\n"}, p)
    ok(r[0] == 0 and b"past" in r[2], "lines past a hunk's counts are said: %r" % (r,))
    d, r = patch_case({"f": "a\n"}, "--- f\n+++ f\n@@ -1 +1 @@\n-a\n+b\n-- \nsignature\n")
    ok(r[0] == 0 and b"past" not in r[2], "a mail's signature is not such a line: %r" % (r,))
    # control characters from a patch do not reach the terminal
    p = "--- /dev/null\n+++ evil\x1b[2Jname\n@@ -0,0 +1 @@\n+x\n"
    d, r = patch_case({}, p, "-p0")
    ok(b"\x1b" not in r[1] + r[2], "patch: a name's escape is not printed: %r" % (r,))


def test_review_orig():
    """decided in the review: <file>.orig when a hunk needed fuzz or offset, as GNU keeps it; made with O_EXCL"""
    old = BASE
    moved = "top\n" + BASE
    new = moved.replace("line 17\n", "LINE 17\n")
    d, r = patch_case({"f.txt": BASE}, UNI)
    ok(tree_of(d).keys() == {"f.txt"}, "a hunk where it said: no .orig")
    d, r = patch_case({"f.txt": moved}, UNI)
    t = tree_of(d)
    ok(r[0] == 0 and t.get("f.txt") == new.encode() and t.get("f.txt.orig") == moved.encode(), "an offset: the old file in .orig: %r" % (sorted(t),))
    fuzzy = BASE.replace("line 14\n", "changed 14\n")
    d, r = patch_case({"f.txt": fuzzy}, UNI)
    t = tree_of(d)
    ok(r[0] == 0 and b"fuzz" in r[1] and t.get("f.txt.orig") == fuzzy.encode(), "fuzz: the old file in .orig: %r" % (sorted(t),))
    d, r = patch_case({"f.txt": moved, "f.txt.orig": "the user's\n"}, UNI)
    t = tree_of(d)
    ok(r[0] == 0 and t["f.txt"] == new.encode() and t["f.txt.orig"] == b"the user's\n" and b".orig" in r[2], "an .orig that was there stays, and it is said: %r" % (r,))
    d, r = patch_case({"f.txt": moved}, UNI, "--dry-run")
    ok(tree_of(d).keys() == {"f.txt"}, "--dry-run: no .orig")
    d, r = patch_case({"f.txt": moved}, UNI, "-o", "out")
    ok(tree_of(d).keys() == {"f.txt", "out"}, "-o: the file stays, so no .orig")
    d = tempfile.mkdtemp(dir=SCRATCH.name)
    put(d + "/f.txt", moved)
    put(d + "/the.patch", UNI)
    r = ours("patch", "-i", "the.patch", cwd=d, env=dict(ENV, CLI_FAIL="write:1"))
    ok(r[0] == 2 and tree_of(d) == {"f.txt": moved.encode()}, "pulled writing .orig: the old file, and nothing beside it: %r %r" % (r, sorted(tree_of(d))))


def test_review_second():
    """the second review: an addition applied again, the first hunk's own place before any other, diff -N's dates, -o with
    nothing to write, one file under two names"""
    for what, p in (("-U0", "--- f\n+++ f\n@@ -1,0 +2 @@\n+NEW\n"), ("normal", "1a2\n> NEW\n")):
        d, r = patch_case({"f": "a\nNEW\nb\nc\n"}, p, "f")
        ok(r[0] == 1 and get(d + "/f") == b"a\nNEW\nb\nc\n" and b"Reversed (or previously applied)" in r[1],
           "%s: an addition that went in before is seen: %r" % (what, r))
    d, r = patch_case({"f": "NEW\na\nb\nc\n"}, "--- f\n+++ f\n@@ -2,0 +3 @@\n+NEW\n", "f")
    ok(r[0] == 0 and get(d + "/f") == b"NEW\na\nNEW\nb\nc\n", "-U0: the same line elsewhere does not stop it: %r" % (r,))
    made = "--- f\t1969-12-31 18:00:00.000000000 -0600\n+++ f\t2026-11-02 04:00:00.000000000 -0600\n@@ -0,0 +1,2 @@\n+one\n+two\n"
    d, r = patch_case({}, made)
    ok(r[0] == 0 and get(d + "/f") == b"one\ntwo\n", "diff -N: the file is made: %r" % (r,))
    for n in (2, 3):
        r = ours("patch", "-i", os.path.join(d, "the.patch"), cwd=d)
        ok(r[0] == 1 and get(d + "/f") == b"one\ntwo\n" and b"Reversed (or previously applied)" in r[1],
           "diff -N, applied a %s time: seen, and the file as it was: %r" % ("second" if n == 2 else "third", r))
    d, r = patch_case({"f": "mine\n"}, made)
    ok(r[0] == 1 and get(d + "/f") == b"mine\n" and b"already there" in r[2], "diff -N over a file with other text: it stays: %r" % (r,))
    d, r = patch_case({"f": "one\ntwo\n"}, made.replace("f\t1969-12-31 18:00:00.000000000 -0600", "/dev/null"))
    ok(r[0] == 1 and get(d + "/f") == b"one\ntwo\n" and b"Reversed (or previously applied)" in r[1],
       "/dev/null, the file there with just that text: seen as gone in before: %r" % (r,))
    gone = "--- f\t2026-11-02 04:00:00.000000000 -0600\n+++ f\t1969-12-31 18:00:00.000000000 -0600\n@@ -1,2 +0,0 @@\n-one\n-two\n"
    d, r = patch_case({"f": "one\ntwo\n"}, gone)
    ok(r[0] == 0 and not os.path.exists(d + "/f"), "diff -N: a file taken out is deleted: %r" % (r,))
    d, r = patch_case({"f": "one\ntwo\nthree\n"}, gone.replace("-1,2", "-1,2"))
    ok(r[0] == 1 and b"Not deleting" in r[1] and get(d + "/f") == b"three\n", "diff -N: more in it than the patch had: it stays: %r" % (r,))
    # a second run: the patch's own place, both ways, before a block like it further on
    block = "a\nb\nc\nd\ne\nf\ng\n"
    base = "".join("%d\n" % i for i in range(1, 11)) + block + "".join("%d\n" % i for i in range(11, 21)) + block
    new = base.replace("d\n", "D\n", 1)
    p = "--- f\n+++ f\n@@ -11,7 +11,7 @@\n a\n b\n c\n-d\n+D\n e\n f\n g\n"
    d, r = patch_case({"f": new}, p)
    ok(r[0] == 1 and get(d + "/f") == new.encode() and b"Reversed (or previously applied)" in r[1], "applied again: not into the like block: %r" % (r,))
    # -o with nothing to write
    d, r = patch_case({"e": "x\n", "out": "keep me\n"}, "--- e\n+++ e\n@@ -1 +0,0 @@\n-x\n", "-o", "out", "e")
    ok(r[0] == 0 and get(d + "/out") == b"keep me\n" and b"out: not written: the result is empty" in r[2], "-o, an empty result: said: %r" % (r,))
    # one file under two names
    two = "--- g\n+++ g\n@@ -5,3 +5,3 @@\n 5\n-NOT\n+x\n 7\n--- ./g\n+++ ./g\n@@ -12,3 +12,3 @@\n 12\n-NOR\n+y\n 14\n"
    d, r = patch_case({"g": "".join("%d\n" % i for i in range(1, 21))}, two, "-p0")
    t = tree_of(d)
    ok(r[0] == 1 and set(t) == {"g", "g.rej"} and b"NOT" in t["g.rej"] and b"NOR" in t["g.rej"] and b"there already" not in r[2],
       "g and ./g: one .rej, both hunks in it: %r %r" % (sorted(t), r))
    moved = "--- h\n+++ h\n@@ -5,3 +5,3 @@\n 5\n-6\n+six\n 7\n--- ./h\n+++ ./h\n@@ -12,3 +12,3 @@\n 12\n-13\n+thirteen\n 14\n"
    old = "top\n" + "".join("%d\n" % i for i in range(1, 21))
    d, r = patch_case({"h": old}, moved, "-p0")
    t = tree_of(d)
    ok(r[0] == 0 and t.get("h.orig") == old.encode() and b"there already" not in r[2], "h and ./h: one .orig, the file before the run: %r" % (r,))


def test_review_nits():
    ok(ours("od", "-N", "17592186044416m", stdin=b"abc")[0] == 2, "od -N: a suffix that overflows")
    ok(ours("od", "-j", "17592186044416m", stdin=b"abc")[0] == 2, "od -j: a suffix that overflows")
    r = ours("nl", "-v", "9223372036854775807", stdin=b"a\nb\n")
    ok(r[0] == 2 and b"9223372036854775807\ta\n" in r[1] and b"nl:" in r[2], "nl: a number that would overflow: %r" % (r,))
    r = ours("nl", "-v", "-9223372036854775807", "-i", "-5", stdin=b"a\nb\n")
    ok(r[0] == 2, "nl: and downwards: %r" % (r,))
    ok(ours("nl", "-w", "2147483647", "-b", "n", stdin=b"")[0] == 2, "nl: a width that overflows")
    with tempfile.TemporaryDirectory() as d:
        put(d + "/vol", fat_boot(label=b"\x1b[2JX     ") + b"\0" * 512)
        put(d + "/s", b"#!/bin/\x1b[2Jsh\n")
        for name in ("vol", "s"):
            r = ours("file", name, cwd=d)
            ok(b"\x1b" not in r[1], "file: no escape from a file reaches the terminal: %r" % (r,))
        os.mkdir(d + "/t")
        put(d + "/t/a\x1b[2Jb", b"")
        ok(b"\x1b" not in ours("tree", "t", cwd=d)[1], "tree: nor from a directory")
        os.mkdir(d + "/u")
        ok(b"\x1b" not in ours("diff", "-r", "t", "u", cwd=d)[1], "diff -r: nor from a directory")


# -- what no tool may do -----------------------------------------------------------------------------------------------


def test_bad_input():
    """every tool on junk: an answer and a status, never a crash"""
    junk = [b"", b"\xff" * 5000, BINARY, bytes(rng.randrange(256) for _ in range(70000)), b"\n" * 70000, b"a" * 200000, b"\xf0\x9f", b"\xc3", b"\x00"]
    # join of a file with itself: on 70,000 empty lines that is 4.9 billion lines out, rightly, as the real one's
    args = {"cmp": ["in", "in"], "comm": ["in", "in"], "join": ["in", "in2"], "paste": ["in", "in"], "fold": ["in"], "nl": ["in"], "rev": ["in"], "od": ["in"],
            "base64": ["in"], "strings": ["in"], "file": ["in"], "diff": ["in", "in2"], "patch": ["-i", "in", "in2"]}
    with tempfile.TemporaryDirectory() as d:
        put(d + "/in2", b"a\nb\n")
        for data in junk:
            put(d + "/in", data)
            for tool, a in args.items():
                r = ours(tool, *a, cwd=d)
                ok(r[0] in (0, 1, 2), "%s on junk: status %d" % (tool, r[0]))
            ok(ours("base64", "-d", "in", cwd=d)[0] in (0, 2), "base64 -d on junk")
            ok(ours("fold", "-s", "-w", "3", "in", cwd=d)[0] == 0, "fold -s on junk")
            ok(ours("od", "-c", "-t", "x8", "in", cwd=d)[0] == 0, "od on junk")
            ok(ours("diff", "-u", "-b", "-w", "-i", "-B", "in", "in2", cwd=d)[0] in (0, 1), "diff on junk")


def test_pipelines():
    """a tool whose reader goes ends cleanly"""
    if not real("sh") or not real("head"):
        return
    big = "x" * 2000000
    with tempfile.TemporaryDirectory() as d:
        put(d + "/big", (big[:70] + "\n") * 40000)
        put(d + "/big2", (big[:71] + "\n") * 40000)
        for cmd in ("rev big", "fold -w 10 big", "nl big", "od big", "base64 big", "strings big", "paste big big", "diff big big2"):
            argv = cmd.split()
            sh = "%s/%s %s | head -1 > /dev/null; echo ${PIPESTATUS:-done}" % (TOOLS, argv[0], " ".join(argv[1:]))
            p = subprocess.run(["sh", "-c", sh], cwd=d, env=ENV, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=TIMEOUT)
            ok(b"AddressSanitizer" not in p.stderr and b"runtime error" not in p.stderr, "%s into a head" % cmd)


def main():
    tests = [test_rev, test_paths, test_cmp, test_comm, test_join, test_paste, test_fold, test_nl, test_od, test_base64, test_strings, test_file, test_tree,
             test_cal, test_diff_against_real, test_diff_options, test_diff_trees, test_patch_files, test_patch_saving, test_patch_against_real,
             test_property_real_files, test_review_output_file, test_review_reversed_first, test_review_rej, test_review_small_patch_things,
             test_review_orig, test_review_second, test_review_nits, test_bad_input, test_pipelines]
    only = sys.argv[2:]
    for t in tests:
        if only and t.__name__ not in only:
            continue
        t()
    for what, n in sorted(skipped.items()):
        print("test_cli_diff: skipped (%d): %s" % (n, what))
    print("test_cli_diff: %d checks, %d failures" % (checks, failures))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
