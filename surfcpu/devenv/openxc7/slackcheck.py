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
thin; 2, with the reason on stderr, when the placement cannot be judged or must not be kept:
report.json cannot be read, has no clock (a design without a clock constraint: the rule has
nothing to judge, so it does not build), a clock's figures are not positive numbers, a clock
misses its constraint, or a --min names a clock twice or one the design does not have; 3 when this
script itself fails (a crash, or an --out file it cannot write). Only 1 lets the seed loop keep a
placement as thin. Only the Python standard library.
"""

import argparse
import json
import math
import os
import subprocess
import sys
import tempfile

RULE_NS = 0.5           # the least slack any clock may have
RULE_SHARE = 0.04       # ... and the least share of its period


def wants(constraint_mhz: float) -> float:
    """the slack the rule wants of a clock with this constraint"""
    return max(RULE_NS, RULE_SHARE * 1000.0 / constraint_mhz)


class Unjudged(Exception):
    """a placement the rule cannot judge, or one that must not be kept even as thin"""


def judge(fmax: dict, mins: dict) -> tuple[int, list, str]:
    """(exit status, a line per clock, the seed loop's line) for nextpnr's fmax table"""
    if not fmax:
        raise Unjudged("the report has no clock (nextpnr's fmax is empty): a design without a clock constraint "
                       "gives the slack rule nothing to judge")
    rows = []
    for clk, v in sorted(fmax.items()):
        got = {}
        for what in ("achieved", "constraint"):
            x = v[what]
            if isinstance(x, bool) or not isinstance(x, (int, float)) or not math.isfinite(x) or x <= 0:
                raise Unjudged("clock %s: %s %r MHz is not a positive number" % (clk, what, x))
            got[what] = float(x)
        slack = 1000.0 / got["constraint"] - 1000.0 / got["achieved"]
        if slack < 0:
            raise Unjudged("clock %s misses its constraint (%.2f MHz of %.2f, slack %+.3f ns): not a placement to "
                           "keep, thin or not" % (clk, got["achieved"], got["constraint"], slack))
        want = mins.get(clk, wants(got["constraint"]))
        rows.append((slack - want, clk, slack, want, "the design's" if clk in mins else "the rule's"))
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
        if not sep or not clk or not math.isfinite(value) or value < 0:
            raise ValueError("--min wants CLOCK=NS with NS a number of nanoseconds, not %r" % it)
        if clk in mins:
            raise ValueError("min-slack names %s twice (%s and %s ns)" % (clk, ns_text(mins[clk]), ns))
        mins[clk] = value
    return mins


def ns_text(v: float) -> str:
    return ("%f" % v).rstrip("0").rstrip(".")


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
    for name, fmax, words in (("no clock (a design without a clock constraint)", {}, "has no clock"),
                              ("a clock whose achieved frequency is 0", {"c": {"achieved": 0.0, "constraint": 80.0}},
                               "not a positive number"),
                              ("a clock whose achieved frequency is NaN", {"c": {"achieved": float("nan"),
                                                                                 "constraint": 80.0}}, "not a positive"),
                              ("a clock that misses its constraint (79 MHz of 80)",
                               {"dram_clk": {"achieved": 79.0, "constraint": 80.0}}, "misses its constraint")):
        cases += 1
        try:
            judge(fmax, {})
            fails += 1
            print("FAIL %s: judged" % name)
        except Unjudged as e:
            ok = words in str(e)
            fails += not ok
            print("%s %s: not judged (%s)" % ("ok  " if ok else "FAIL", name, e))
    for name, items, words in (("a --min without =NS", ["dram_clk"], "wants CLOCK=NS"),
                               ("a --min naming a clock twice", ["dram_clk=0.5", "dram_clk=0.05"], "twice")):
        cases += 1
        try:
            parse_mins(items)
            fails += 1
            print("FAIL %s was taken" % name)
        except ValueError as e:
            ok = words in str(e)
            fails += not ok
            print("%s %s is refused (%s)" % ("ok  " if ok else "FAIL", name, e))
    # the program itself: --out written with a line per clock (exit 0), and --out that cannot be written,
    # which is this script failing (exit 3), never a verdict
    with tempfile.TemporaryDirectory() as d:
        rep = os.path.join(d, "report.json")
        with open(rep, "w") as f:
            json.dump({"fmax": ddr}, f)
        for name, out, status, check in (
                ("--out written", os.path.join(d, "slack.txt"), 0, lambda r, o: r.stdout.startswith("pass") and
                 len(open(o).read().splitlines()) == 5),
                ("--out in a directory that does not exist", os.path.join(d, "no", "slack.txt"), 3,
                 lambda r, o: r.stdout.strip() == "unreadable" and "internal error" in r.stderr)):
            cases += 1
            r = subprocess.run([sys.executable, "-I", os.path.abspath(__file__), "--report", rep, "--min",
                                "dram_clk=0.05", "--out", out], capture_output=True, text=True)
            ok = r.returncode == status and check(r, out)
            fails += not ok
            print("%s %s: exit %d%s" % ("ok  " if ok else "FAIL", name, r.returncode,
                                        (", " + r.stderr.strip().splitlines()[-1]) if r.stderr.strip() else ""))
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
        try:
            mins = parse_mins(a.min)
            fmax = json.load(open(a.report)).get("fmax", {})
            fmax = {k: {"achieved": float(v["achieved"]), "constraint": float(v["constraint"])} for k, v in fmax.items()}
        except (OSError, ValueError, KeyError, TypeError, AttributeError) as e:
            raise Unjudged(str(e))
        unknown = sorted(set(mins) - set(fmax))
        if unknown and fmax:
            raise Unjudged("min-slack names %s, which the design does not have (its clocks: %s)"
                           % (", ".join(unknown), ", ".join(sorted(fmax))))
        status, lines, summary = judge(fmax, mins)
        if a.out:                               # a file that cannot be written is this script failing: 3
            with open(a.out, "w") as f:
                f.write("slackcheck: each clock's slack at least max(%.1f ns, %d %% of its period), or the design's\n"
                        % (RULE_NS, round(RULE_SHARE * 100)) + "".join(l + "\n" for l in lines) + summary + "\n")
    except Unjudged as e:
        print("unreadable")
        print("slackcheck: %s" % e, file=sys.stderr)
        return 2
    except Exception as e:                      # noqa: BLE001 (a bug here must never read as "thin")
        print("unreadable")
        print("slackcheck: internal error: %s: %s" % (type(e).__name__, e), file=sys.stderr)
        return 3
    print(summary)
    return status


if __name__ == "__main__":
    sys.exit(main())
