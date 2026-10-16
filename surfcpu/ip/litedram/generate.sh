#!/bin/sh
# SurfCPU: generate a LiteDRAM core with the pinned environment
# File: generate.sh Date: 10/16/26 (SurfCPU)
#
#   ip/litedram/generate.sh <config.yml> <out_dir> [--sim] [--name NAME]
#
# Runs ip/litedram/setup.sh if needed, then litedram_gen (LiteDRAM 2026.08) on the configuration,
# writing the core to <out_dir>/gateware/NAME.v (NAME: litedram_core unless given; with a CPU
# also the BIOS under <out_dir>/software/ and its ROM image NAME_rom.init). The log goes to
# <out_dir>/generate.log.
#
# --sim: the same configuration with LiteDRAM's DRAM model (SDRAMPHYModel) in place of the PHY
# and a plain clock in place of the PLL, for simulation. The configuration is used as it is but
# for two keys, as written to <out_dir>/config.yml: "cpu" becomes None and "uart" goes, because
# the model needs no calibration and LiteX 2026.08 cannot build the calibration BIOS for it (its
# liblitedram then calls the A7 PHY's delay registers, which the model lacks). Without the CPU
# the core has a Wishbone control port (wb_ctrl_*) through which the testbench writes the
# init_done/init_error registers that the BIOS writes on the board.
#
# The output is reproducible: SOURCE_DATE_EPOCH is the pinned LiteDRAM commit's time, which GCC
# uses for the BIOS's __DATE__ and __TIME__, Python's clock is frozen at it for LiteX's "Date"
# lines (ip/litedram/run_gen.py), and TZ is UTC. Two runs write the same bytes; ip/litedram/
# arty.sha256 records them (ip/litedram/lock.sh).
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
if [ -n "$sim" ]; then                 # the configuration for the model: no CPU, no console
  python3 "$here/sim_config.py" "$cfg" "$out/config.yml"
  cfg_used=$out/config.yml
else
  cfg_used=$cfg
fi
# shellcheck disable=SC2086
if python3 "$here/run_gen.py" "$cfg_used" --name "$name" --output-dir "$out" $sim > "$out/generate.log" 2>&1; then
  echo "generate.sh: ${cfg#"$top"/}${sim:+ (--sim)} -> ${out#"$top"/}/gateware/$name.v (SOURCE_DATE_EPOCH $SOURCE_DATE_EPOCH)"
else
  tail -30 "$out/generate.log" >&2
  echo "generate.sh: litedram_gen failed (log: $out/generate.log)" >&2
  exit 1
fi
