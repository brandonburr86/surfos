#!/usr/bin/env python3
"""
SurfCPU: the simulation core's LiteDRAM configuration
--------------------
File: sim_config.py Date: 10/16/26 (SurfCPU)
--------------------
Writes the configuration ip/litedram/generate.sh --sim gives litedram_gen: the board's
configuration (ip/litedram/arty.yml) read as litedram_gen reads it, with exactly two changes,
"cpu" set to None and "uart" removed (the DRAM model needs no calibration CPU, and LiteX
2026.08 cannot build the calibration BIOS for the model; generate.sh explains). Every other
setting (DRAM module, PHY type, electrical and clock settings, command buffer depth, user
ports) is the board's, so the simulation core cannot drift from the bitstream's.

    python3 ip/litedram/sim_config.py ip/litedram/arty.yml build/litedram/arty_sim/config.yml

Needs PyYAML (the pinned generator environment, build/third_party/litedram/env.sh).
"""

import sys

import yaml

CHANGES = {"cpu": None}                 # set
REMOVED = ("uart",)                     # removed


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__.strip().split("\n\n")[2].strip(), file=sys.stderr)
        return 2
    src, dst = argv[1], argv[2]
    with open(src) as f:
        cfg = yaml.load(f.read(), Loader=yaml.Loader)       # as litedram_gen loads it
    for key, value in CHANGES.items():
        cfg[key] = value
    for key in REMOVED:
        cfg.pop(key, None)
    name = src.split("/surfcpu/")[-1]
    with open(dst, "w") as f:
        f.write(f"# Written by ip/litedram/sim_config.py from {name}: cpu None and no uart; the rest as there.\n")
        yaml.dump(cfg, f, default_flow_style=False, sort_keys=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
