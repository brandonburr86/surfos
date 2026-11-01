#!/usr/bin/env python3
"""
SurfCPU: litedram_gen with a fixed clock and no instance tree
--------------------
File: run_gen.py Date: 10/16/26 (SurfCPU)
--------------------
Runs LiteDRAM's generator (python3 -m litedram.gen, the same arguments) with Python's clock
frozen at SOURCE_DATE_EPOCH, so the "Date" lines LiteX writes into the Verilog, the register map
and the build scripts are the same on every run. ip/litedram/generate.sh calls it with
SOURCE_DATE_EPOCH set to the pinned LiteDRAM commit's time.

Then it takes out of the generated Verilog (gateware/<name>.v) the one thing that still varied:
the instance tree LiteX writes as a block comment under "// Hierarchy". LiteX lists each
module's black-box instances sorted by their default string, which holds the object's address,
so their order follows the Python process's memory layout: the environment, the paths of the
checkout and the output, the interpreter. One checkout gave the same file every time, a fresh
checkout elsewhere did not (the release track: 16 generations, 5 different files, the Verilog
outside that comment identical in all). The comment is replaced by a fixed one; nothing outside
it changes, and the script stops if it does not find exactly one such comment. With that, the
generated files are byte-identical from run to run and checkout to checkout
(ip/litedram/arty.sha256 holds their hashes). Nothing in the generator is changed.

    SOURCE_DATE_EPOCH=<seconds> python3 ip/litedram/run_gen.py <config.yml> [litedram_gen options]
    python3 -I ip/litedram/run_gen.py --self-test        (make check: the comment's removal alone)

SURFCPU_LITEDRAM_KEEP_TREE=1 keeps LiteX's comment, for ip/litedram/repro.sh's comparison runs.
"""

import os
import re
import runpy
import sys
import time

TREE = re.compile(r"(\n// Hierarchy\n//-+\n\n/\*\n)(.*?)(\n\*/\n)", re.S)
NOTE = ("LiteX's instance hierarchy is left out of this file by ip/litedram/run_gen.py: it listed the\n"
        "black-box instances in the order of their addresses in the generator's Python process, which\n"
        "differed from one checkout to another. The Verilog is the generator's, unchanged.")


def option(args, name, default):
    """The value of --name VALUE or --name=VALUE in litedram_gen's arguments."""
    for i, a in enumerate(args):
        if a == name and i + 1 < len(args):
            return args[i + 1]
        if a.startswith(name + "="):
            return a.split("=", 1)[1]
    return default


def without_tree(text, what):
    """text with LiteX's instance tree comment replaced by NOTE; stops unless there is exactly one."""
    found = TREE.findall(text)
    if len(found) != 1:
        sys.exit(f"run_gen.py: {what}: {len(found)} instance hierarchy comments, expected 1 "
                 "(has LiteX's Verilog header changed?)")
    return TREE.sub(lambda m: m.group(1) + NOTE + m.group(3), text, count=1)


def drop_tree(path):
    with open(path, encoding="utf-8") as f:
        text = f.read()
    with open(path, "w", encoding="utf-8") as f:
        f.write(without_tree(text, path))


def self_test():
    """The comment's removal on LiteX's layout (lines as litedram_gen writes them), without the generator."""
    sep = "//" + "-" * 78 + "\n"
    head = "module litedram_core (\n    input wire clk\n);\n\n\n" + sep + "// Hierarchy\n" + sep + "\n"
    tail = "\n" + sep + "// Signals\n" + sep + "\nwire a;\n/* a comment of the design's */\nendmodule\n"
    trees = ["LiteDRAMCore\n\u251c\u2500\u2500 [BB:OSERDESE2]\n\u2514\u2500\u2500 [BB:OBUFDS]\nLegend:\n",
             "LiteDRAMCore\n\u251c\u2500\u2500 [BB:OBUFDS]\n\u2514\u2500\u2500 [BB:OSERDESE2]\nLegend:\n"]
    a, b = (without_tree(head + "/*\n" + t + "\n*/\n" + tail, "self-test") for t in trees)
    bad = []
    if a != b:
        bad.append("two orders of the tree give two files")
    if a != head + "/*\n" + NOTE + "\n*/\n" + tail:
        bad.append("something outside the comment changed")
    twice = head + "/*\nx\n*/\n" + "\n" + sep + "// Hierarchy\n" + sep + "\n/*\ny\n*/\n" + tail
    for text, n in ((head + tail, 0), (twice, 2)):
        try:
            without_tree(text, "self-test")
            bad.append(f"{n} comments accepted")
        except SystemExit:
            pass
    print("run_gen.py self-test: " + ("PASS (the tree comment replaced, nothing else; none or two refused)"
                                      if not bad else "FAIL: " + "; ".join(bad)))
    return 1 if bad else 0


if sys.argv[1:] == ["--self-test"]:
    sys.exit(self_test())
epoch = os.environ.get("SOURCE_DATE_EPOCH", "")
if not epoch.isdigit():
    sys.exit("run_gen.py: SOURCE_DATE_EPOCH must be set to a number of seconds")
frozen = float(epoch)
time.time = lambda: frozen
time.time_ns = lambda: int(epoch) * 1_000_000_000
args = sys.argv[1:]
sys.argv = ["litedram_gen"] + args
try:
    runpy.run_module("litedram.gen", run_name="__main__", alter_sys=True)
except SystemExit as e:
    if e.code not in (None, 0):
        raise
if os.environ.get("SURFCPU_LITEDRAM_KEEP_TREE") != "1":
    drop_tree(os.path.join(option(args, "--output-dir", "build"), "gateware", option(args, "--name", "litedram_core") + ".v"))
