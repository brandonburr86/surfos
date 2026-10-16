#!/usr/bin/env python3
"""
SurfCPU: no combinational loop through the core's interrupt inputs in the SoC
--------------------
File: soc_loop_check.py Date: 10/15/26 (SurfCPU)
--------------------
surfcpu_core's dbus_req_valid depends combinationally on irq_external, irq_timer and
irq_software (through the interrupt decision), and its ibus_req_valid on ibus_rsp_valid
(SURFBUS.md rule 6). That is safe only if nothing in the SoC makes an interrupt input
depend combinationally on a bus request. This reads the whole surfcpu_soc with the real
core (synthesis view), flattens it and asks Yosys:
  * the combinational input cone of each of the core's interrupt inputs (stopping at
    flip-flops): it must not contain any of the core's ibus_req_* or dbus_req_* nets;
  * as a sanity check of the method, the cone of dbus_req_valid must contain the
    interrupt inputs (the dependency the core does have);
  * `check` on the whole flattened SoC: no combinational loop anywhere.
It changes no file. Exit 0 when all three hold.

    soc_loop_check.py [--out build/cpu/soc-loop]
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent          # surfcpu/
FF_CELLS = "$dff,$dffe,$adff,$adffe,$sdff,$sdffe,$sdffce,$dffsr,$dffsre,$aldff,$aldffe,$dlatch,$adlatch,$ff"
IRQS = ["irq_external", "irq_timer", "irq_software"]
REQS = ["ibus_req_valid", "ibus_req_addr", "ibus_req_write", "ibus_req_be", "ibus_req_wdata", "ibus_req_fetch",
        "dbus_req_valid", "dbus_req_addr", "dbus_req_write", "dbus_req_be", "dbus_req_wdata", "dbus_req_fetch"]


def make_var(name: str) -> str:
    r = subprocess.run(["make", "--no-print-directory", "-s", f"--eval=print-%: ; @echo $($*)", f"print-{name}"],
                       cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"soc_loop_check: cannot read {name} from the makefiles: {r.stderr.strip()}")
    return r.stdout.strip()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=str(ROOT / "build" / "cpu" / "soc-loop"))
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    files = " ".join([make_var("SOC_TOP_RTL"), make_var("SOC_CORE_SRC"), make_var("SOC_IO_SRC")]).split()
    if "rtl/cpu/surfcpu_core.v" not in files:
        print("soc_loop_check: the SoC build does not use the real core (rtl/cpu/surfcpu_core.v)")
        return 1
    inc = make_var("SOC_INC")
    stop = f"%ci*:-{FF_CELLS}"
    lines = [f"read_verilog -DSYNTHESIS {inc} {' '.join(files)}",
             "hierarchy -top surfcpu_soc", "proc", "flatten", "opt_clean"]
    for irq in IRQS:
        lines += [f"log ==== cone of {irq}", f"select -list w:{irq} {stop} w:* %i"]
    lines += ["log ==== cone of dbus_req_valid", f"select -list w:dbus_req_valid {stop} w:* %i",
              "log ==== check", "check"]
    ys = out / "loop.ys"
    ys.write_text("\n".join(lines) + "\n")
    r = subprocess.run(["yosys", "-q", "-l", str(out / "loop.log"), "-s", str(ys)], cwd=ROOT,
                       capture_output=True, text=True)
    log = (out / "loop.log").read_text() if (out / "loop.log").exists() else r.stdout + r.stderr
    if r.returncode != 0:
        print(f"soc_loop_check: yosys failed:\n{(r.stdout + r.stderr)[-1500:]}")
        return 1
    sections = re.split(r"==== ", log)
    cones: dict[str, set[str]] = {}
    for sec in sections[1:]:
        title, _, body = sec.partition("\n")
        if title.startswith("cone of "):
            nets = set()
            for l in body.splitlines():
                m = re.match(r"\s*surfcpu_soc/(\S+)\s*$", l)
                if m:
                    nets.add(m.group(1).lstrip("\\"))
            cones[title[len("cone of "):].strip()] = nets
    def is_net(name: str, base: str) -> bool:      # a net or one of its aliases after flattening
        return name == base or name.endswith("." + base)

    ok = True
    for irq in IRQS:
        cone = cones.get(irq, set())
        hit = sorted(n for n in cone if any(is_net(n, q) for q in REQS))
        if hit:
            ok = False
            print(f"soc_loop_check: LOOP RISK: {irq} depends combinationally on {', '.join(hit)}")
        else:
            regs = sorted(n for n in cone if n.endswith("_q") or n.endswith("_r"))
            print(f"soc_loop_check: {irq}: combinational cone of {len(cone)} nets, none of them an ibus_req_* "
                  f"or dbus_req_* net (it starts at registers such as {', '.join(regs[:4]) or 'none'})")
    # dbus_req_valid depends on each interrupt input, so (cones being transitive) each input's
    # cone lies inside dbus_req_valid's; the input's own name may survive flattening only as an
    # alias (u_core.csr.irq_timer) or as the register driving it (u_timer.msip_q)
    dcone = cones.get("dbus_req_valid", set())
    sane = all((cones.get(irq, set()) - {irq}) and (cones.get(irq, set()) - {irq}) <= dcone for irq in IRQS)
    print(f"soc_loop_check: sanity: dbus_req_valid's cone {'contains' if sane else 'does NOT contain'} the cones "
          f"of the three interrupt inputs (the core's own dependency is seen)")
    ok &= sane
    loops = [l.strip() for l in log.splitlines() if "logic loop" in l.lower()]
    if loops:
        ok = False
        print("soc_loop_check: yosys check found a logic loop:\n  " + "\n  ".join(loops[:10]))
    else:
        print("soc_loop_check: yosys check: no logic loop in the flattened surfcpu_soc")
    print(f"soc_loop_check: {'PASS' if ok else 'FAIL'} (files: {len(files)} RTL files of the SoC build; log {out / 'loop.log'})")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
