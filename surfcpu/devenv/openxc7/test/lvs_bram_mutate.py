#!/usr/bin/env python3
"""
SurfCPU open-flow test: a deliberately wrong routing of a block RAM's write enables
--------------------
File: lvs_bram_mutate.py Date: 10/16/26 (SurfCPU)
--------------------
Writes a copy of an extracted fabric netlist (tileverilog's, before bramwe.py) in which the
given RAMB18E1 site pins of the first block RAM take the route of its WEBWE0 pin: what the
fabric would be if the router had sent the first byte enable to those pins too. Used by
tools/openxc7-checks-test.sh to show that the flow still finds such a difference.

    python3 -I lvs_bram_mutate.py FABRIC.raw.v OUT.v PIN [PIN...]      (PIN: e.g. WEBWE7)
"""
import re
import sys

src, dst, pins = sys.argv[1], sys.argv[2], sys.argv[3:]
text = open(src).read()
m = re.search(r"^\s*assign (\S+)_BRAM_RAMB18_WEBWE0 = (\S+);", text, re.M)
if not m:
    sys.exit("lvs_bram_mutate: no RAMB18 WEBWE0 route in " + src)
tile, route = m.group(1), m.group(2)
for p in pins:
    text, n = re.subn(r"^(\s*assign " + re.escape(tile) + r"_BRAM_RAMB18_" + p + r" = )\S+;", r"\g<1>" + route + ";",
                      text, count=1, flags=re.M)
    if n != 1:
        sys.exit("lvs_bram_mutate: no route for " + p)
open(dst, "w").write(text)
print("lvs_bram_mutate: %s now routed from %s (WEBWE0's route)" % (", ".join(pins), route))
