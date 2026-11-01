#!/bin/sh
# SurfCPU: generate a LiteDRAM core with the pinned environment
# File: generate.sh Date: 10/16/26 (SurfCPU)
#
#   ip/litedram/generate.sh <config.yml> <out_dir> [--sim] [--name NAME]
#
# Runs ip/litedram/setup.sh if needed, then litedram_gen (LiteDRAM 2026.08) on the configuration,
# writing the core to <out_dir>/gateware/NAME.v (NAME: litedram_core unless given), its register
# map (<out_dir>/csr.csv) and the headers LiteX's software would use (<out_dir>/software/include/
# generated/: sdram_phy.h with the PHY's settings and the DRAM's init_sequence). The log goes to
# <out_dir>/generate.log.
#
# --sim: the same configuration, unchanged, with LiteDRAM's DRAM model (SDRAMPHYModel) in place
# of the PHY and a plain clock in place of the PLL, for simulation. Like the board's core it has
# no CPU: its Wishbone control port (wb_ctrl_*) carries the same DFII and init registers (the
# model has no PHY registers), through which SurfCPU, or a testbench, initializes it.
#
# The output is reproducible: SOURCE_DATE_EPOCH is the pinned LiteDRAM commit's time, Python's
# clock is frozen at it for LiteX's "Date" lines (ip/litedram/run_gen.py), TZ is UTC, the
# generator starts in a fixed environment (env -i, PYTHONHASHSEED 0), and run_gen.py takes out
# the instance tree LiteX writes into the Verilog as a comment: it lists black-box instances in
# the order of their addresses in the Python process, which moved with the environment, the
# paths and the interpreter (round ddr-3: make's extra variables reordered it in one checkout,
# and a fresh checkout elsewhere still got another order). The Verilog itself never varied. Runs
# in any checkout write the same bytes; ip/litedram/arty.sha256 records them (ip/litedram/lock.sh).
set -eu
[ $# -ge 2 ] || { echo "usage: $0 <config.yml> <out_dir> [--sim] [--name NAME]" >&2; exit 2; }
here=$(cd "$(dirname "$0")" && pwd -P)
top=$(cd "$here/../.." && pwd -P)
cfg=$(cd "$(dirname "$1")" && pwd -P)/$(basename "$1")
out=$2
shift 2
name=litedram_core
sim=
while [ $# -gt 0 ]; do
  case "$1" in
    --sim)  sim=--sim; shift ;;
    --name) [ $# -ge 2 ] || { echo "generate.sh: --name needs a value" >&2; exit 2; }; name=$2; shift 2 ;;
    *)      echo "generate.sh: unknown option $1" >&2; exit 2 ;;
  esac
done
env=$top/build/third_party/litedram/env.sh
[ -f "$env" ] || "$here/setup.sh"
. "$env"
SOURCE_DATE_EPOCH=$(git -C "$top/build/third_party/litedram/src/litedram" log -1 --format=%ct)
TZ=UTC
export SOURCE_DATE_EPOCH TZ
mkdir -p "$out"
out=$(cd "$out" && pwd -P)
rm -rf "$out/gateware" "$out/software"
cd "$out"
rm -f "$out/config.yml"                # a leftover of the generator before round ddr-3
# shellcheck disable=SC2086
if env -i PATH="$top/build/third_party/litedram/bin:/usr/local/bin:/usr/bin:/bin" PYTHONPATH="$PYTHONPATH" \
     HOME=/tmp LANG=C.UTF-8 TZ=UTC PYTHONHASHSEED=0 SOURCE_DATE_EPOCH="$SOURCE_DATE_EPOCH" \
     python3 "$here/run_gen.py" "$cfg" --name "$name" --output-dir "$out" $sim > "$out/generate.log" 2>&1; then
  echo "generate.sh: ${cfg#"$top"/}${sim:+ (--sim)} -> ${out#"$top"/}/gateware/$name.v (SOURCE_DATE_EPOCH $SOURCE_DATE_EPOCH)"
else
  tail -30 "$out/generate.log" >&2
  echo "generate.sh: litedram_gen failed (log: $out/generate.log)" >&2
  exit 1
fi
