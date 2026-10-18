#!/usr/bin/env python3
"""
SurfCPU open flow: enough slack on every clock
--------------------
File: slackcheck.py Date: 10/19/26 (SurfGPU)
--------------------
nextpnr passes a placement when every clock meets its constraint, by however little. Its timing
is an estimate, not Vivado's sign-off, and no design on record has run on a board with less
than about 0.9 ns: the DDR+GPU design at 72ba06c met LiteDRAM's 80 MHz dram_clk with +0.087 ns.
So the open flow wants more of a placement: every clock's slack (its period at the constraint
minus its period at the frequency nextpnr achieved, as timing.txt prints it) at least
max(0.5 ns, 4 % of its period), or what the design list sets for that clock (min-slack CLOCK NS).
A placement with less is "thin": the flow tries its next placer seed, and keeps the best thin one,
marked THIN, only when every seed is (tools/openxc7-steps.sh; docs/OPEN-FLOW.md section 3).

    python3 -I devenv/openxc7/slackcheck.py --report report.json [--min CLOCK=NS]... [--out FILE]
    python3 -I devenv/openxc7/slackcheck.py --self-test

Prints one line for the seed loop: "pass" or "thin", then the clock with the least margin over
what it wants, its slack, what it wants and the margin, in ns ("thin dram_clk +0.087 0.500
-0.413"); --out gets a line for every clock. Exit 0 when every clock has enough, 1 when one is
thin, 2 when report.json has no clock or cannot be read, or a --min names no clock of the design.
Only the Python standard library.
"""

import argparse
import json
import sys

RULE_NS = 0.5           # the least slack any clock may have
RULE_SHARE = 0.04       # ... and the least share of its period


def wants(constraint_mhz: float) -> float:
    """the slack the rule wants of a clock with this constraint"""
    return max(RULE_NS, RULE_SHARE * 1000.0 / constraint_mhz)


def judge(fmax: dict, mins: dict) -> tuple[int, list, str]:
    """(exit status, a line per clock, the seed loop's line) for nextpnr's fmax table"""
    rows = []
    for clk, v in sorted(fmax.items()):
        slack = 1000.0 / v["constraint"] - 1000.0 / v["achieved"]
        want = mins.get(clk, wants(v["constraint"]))
        rows.append((slack - want, clk, slack, want, "the design's" if clk in mins else "the rule's"))
    if not rows:
        return 2, ["no clock in the report"], "unreadable"
    lines = ["clock %s: slack %+.3f ns, wants %.3f ns (%s): %s" % (clk, slack, want, whose,
             "ok" if margin >= 0 else "THIN") for margin, clk, slack, want, whose in rows]
    margin, clk, slack, want, _ = min(rows)
    verdict = "pass" if margin >= 0 else "thin"
    return (0 if margin >= 0 else 1), lines, "%s %s %+.3f %.3f %+.3f" % (verdict, clk, slack, want, margin)


def parse_mins(items: list) -> dict:
    mins = {}
    for it in items:
        clk, sep, ns = it.partition("=")
        try:
            value = float(ns)
        except ValueError:
            value = -1.0
        if not sep or not clk or value < 0:
            raise ValueError("--min wants CLOCK=NS with NS a number of nanoseconds, not %r" % it)
        mins[clk] = value
    return mins


def self_test() -> int:
    fails = cases = 0

    def case(name, fmax, mins, status, start):
        nonlocal fails, cases
        cases += 1
        st, _, line = judge(fmax, mins)
        ok = st == status and line.startswith(start)
        fails += not ok
        print("%s %s: %s" % ("ok  " if ok else "FAIL", name, line))

    ddr = {"clk100_g": {"achieved": 1013.17, "constraint": 100}, "dram_clk": {"achieved": 80.5607, "constraint": 80},
           "sys_clk": {"achieved": 52.28, "constraint": 25}}
    case("72ba06c's DDR+GPU placement: dram_clk +0.087 ns is thin", ddr, {}, 1, "thin dram_clk +0.087 0.500 -0.413")
    case("... passes where the design list wants 0.05 ns of dram_clk", ddr, {"dram_clk": 0.05}, 0, "pass")
    case("1.0.0's thinnest release design (the 35T DDR design, 0.93 ns) passes",
         {"dram_clk": {"achieved": 86.43, "constraint": 80}}, {}, 0, "pass dram_clk +0.930")
    case("a 25 MHz clock wants 4 % of 40 ns, 1.6 ns: +1.5 ns is thin",
         {"sys_clk": {"achieved": 1000.0 / 38.5, "constraint": 25}}, {}, 1, "thin sys_clk +1.500 1.600")
    case("a 200 MHz clock wants 0.5 ns (4 % is 0.2 ns)", {"iod": {"achieved": 1000.0 / 4.6, "constraint": 200}}, {},
         1, "thin iod +0.400 0.500")
    case("no clock: unreadable", {}, {}, 2, "unreadable")
    cases += 1
    try:
        parse_mins(["dram_clk"])
        fails += 1
        print("FAIL a --min without =NS was taken")
    except ValueError:
        print("ok   a --min without =NS is refused")
    print("slackcheck self-test: %s" % ("PASS (%d cases)" % cases if not fails else "FAIL (%d of %d)" % (fails, cases)))
    return 1 if fails else 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--report")
    ap.add_argument("--min", action="append", default=[], metavar="CLOCK=NS")
    ap.add_argument("--out")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    try:
        mins = parse_mins(a.min)
        fmax = json.load(open(a.report)).get("fmax", {})
        fmax = {k: {"achieved": float(v["achieved"]), "constraint": float(v["constraint"])} for k, v in fmax.items()}
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as e:
        print("unreadable")
        print("slackcheck: %s" % e, file=sys.stderr)
        return 2
    unknown = sorted(set(mins) - set(fmax))
    if unknown and fmax:
        print("unreadable")
        print("slackcheck: min-slack names %s, which the design does not have (its clocks: %s)"
              % (", ".join(unknown), ", ".join(sorted(fmax))), file=sys.stderr)
        return 2
    status, lines, summary = judge(fmax, mins)
    if a.out:
        with open(a.out, "w") as f:
            f.write("slackcheck: each clock's slack at least max(%.1f ns, %d %% of its period), or the design's\n"
                    % (RULE_NS, round(RULE_SHARE * 100)) + "".join(l + "\n" for l in lines) + summary + "\n")
    print(summary)
    return status


if __name__ == "__main__":
    sys.exit(main())
