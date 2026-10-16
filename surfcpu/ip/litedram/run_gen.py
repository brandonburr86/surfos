#!/usr/bin/env python3
"""
SurfCPU: litedram_gen with a fixed clock
--------------------
File: run_gen.py Date: 10/16/26 (SurfCPU)
--------------------
Runs LiteDRAM's generator (python3 -m litedram.gen, the same arguments) with Python's clock
frozen at SOURCE_DATE_EPOCH, so the "Date" lines LiteX writes into the Verilog, the register map
and the build scripts are the same on every run. ip/litedram/generate.sh calls it with
SOURCE_DATE_EPOCH set to the pinned LiteDRAM commit's time: the generated files are then
byte-identical from run to run (ip/litedram/arty.sha256 holds their hashes). Nothing else in the
generator is changed.

    SOURCE_DATE_EPOCH=<seconds> python3 ip/litedram/run_gen.py <config.yml> [litedram_gen options]
"""

import os
import runpy
import sys
import time

epoch = os.environ.get("SOURCE_DATE_EPOCH", "")
if not epoch.isdigit():
    sys.exit("run_gen.py: SOURCE_DATE_EPOCH must be set to a number of seconds")
frozen = float(epoch)
time.time = lambda: frozen
time.time_ns = lambda: int(epoch) * 1_000_000_000
sys.argv = ["litedram_gen"] + sys.argv[1:]
runpy.run_module("litedram.gen", run_name="__main__", alter_sys=True)
