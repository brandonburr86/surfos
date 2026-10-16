#!/bin/sh
# SurfCPU: generate a LiteDRAM core with the pinned environment
# File: generate.sh Date: 10/15/26 (SurfCPU)
#
#   ip/litedram/generate.sh <config.yml> <out_dir> [--sim]
#
# Runs ip/litedram/setup.sh if needed, then litedram_gen (LiteDRAM 2026.08) on the configuration,
# writing the core to <out_dir>/gateware/litedram_core.v (and, with a CPU, the BIOS under
# <out_dir>/software/ and its ROM image). The log goes to <out_dir>/generate.log.
set -eu
[ $# -ge 2 ] || { echo "usage: $0 <config.yml> <out_dir> [--sim]" >&2; exit 2; }
here=$(cd "$(dirname "$0")" && pwd -P)
top=$(cd "$here/../.." && pwd -P)
cfg=$(cd "$(dirname "$1")" && pwd -P)/$(basename "$1")
out=$2
shift 2
env=$top/build/third_party/litedram/env.sh
[ -f "$env" ] || "$here/setup.sh"
. "$env"
mkdir -p "$out"
out=$(cd "$out" && pwd -P)
cd "$out"
if python3 -m litedram.gen "$cfg" --name litedram_core --output-dir "$out" "$@" > "$out/generate.log" 2>&1; then
  echo "generate.sh: $(basename "$cfg") -> $out/gateware/litedram_core.v"
else
  tail -30 "$out/generate.log" >&2
  echo "generate.sh: litedram_gen failed (log: $out/generate.log)" >&2
  exit 1
fi
