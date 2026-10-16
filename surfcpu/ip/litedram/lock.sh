#!/bin/sh
# SurfCPU: the generated LiteDRAM files against their recorded SHA-256 (ip/litedram/arty.sha256)
# File: lock.sh Date: 10/16/26 (SurfCPU)
#
#   ip/litedram/lock.sh check    every file arty.sha256 lists is there and has its hash (exit 1
#                                otherwise, naming each file that is missing or differs)
#   ip/litedram/lock.sh write    record the files as they are now (only after a deliberate
#                                change: the generator's pins, arty.yml, or the toolchain image)
#
# The DDR designs read the LiteDRAM core from build/ (a build product, never committed):
# ip/litedram/generate.sh makes it byte for byte the same on every run (SOURCE_DATE_EPOCH, a
# frozen clock), so its hashes can be committed in arty.sha256 and checked: by make litedram-gen
# after generating, and by tools/fpga-build.sh, which counts a bitstream built from files that
# differ from these hashes as dirty (not reproducible from the commit). Paths are relative to
# surfcpu/; the format is sha256sum's (two spaces).
set -eu
here=$(cd "$(dirname "$0")" && pwd -P)
top=$(cd "$here/../.." && pwd -P)
lock=$here/arty.sha256
cd "$top"
sha() { if command -v sha256sum > /dev/null 2>&1; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1; }

FILES="build/litedram/arty/gateware/litedram_core.v
build/litedram/arty/gateware/litedram_core_rom.init
build/litedram/arty/gateware/litedram_core_sram.init
build/litedram/arty/gateware/litedram_core.xdc
build/third_party/litedram/src/pythondata-cpu-vexriscv/pythondata_cpu_vexriscv/verilog/VexRiscv_Min.v
build/litedram/arty_sim/gateware/litedram_sim.v"

case "${1:-}" in
  check)
    bad=0 n=0
    while read -r want path; do
      case "$want" in ''|\#*) continue ;; esac
      n=$((n + 1))
      if [ ! -f "$path" ]; then
        echo "lock.sh: missing   $path (make litedram-gen)"; bad=1
      elif [ "$(sha "$path")" != "$want" ]; then
        echo "lock.sh: DIFFERS   $path (sha256 $(sha "$path"), arty.sha256 has $want)"; bad=1
      fi
    done < "$lock"
    if [ "$bad" = 0 ]; then
      echo "lock.sh: the $n generated LiteDRAM files match ip/litedram/arty.sha256"
    else
      echo "lock.sh: the generated LiteDRAM files do not match ip/litedram/arty.sha256: regenerate them" >&2
      echo "lock.sh: (make litedram-gen); if the generator, arty.yml or the image changed on purpose," >&2
      echo "lock.sh: record the new files with ip/litedram/lock.sh write and commit arty.sha256" >&2
    fi
    exit "$bad" ;;
  write)
    for path in $FILES; do
      [ -f "$path" ] || { echo "lock.sh: missing $path (make litedram-gen first)" >&2; exit 1; }
    done
    image=$(sed -n 's/^IMAGE_TAG=//p' devenv/run.sh)
    {
      echo "# SurfCPU: SHA-256 of the generated LiteDRAM files the DDR designs build from (paths relative"
      echo "# to surfcpu/, sha256sum format). File: arty.sha256, written by ip/litedram/lock.sh write."
      echo "# generate: make litedram-gen"
      echo "# ip/litedram/generate.sh makes them byte for byte the same on every run, in the toolchain"
      echo "# image surfcpu-toolchain:$image (whose RISC-V GCC compiles the calibration BIOS) from the"
      echo "# generator ip/litedram/setup.sh pins, ip/litedram/arty.yml and SOURCE_DATE_EPOCH"
      echo "# $(git -C build/third_party/litedram/src/litedram log -1 --format=%ct 2>/dev/null || echo '?') (the LiteDRAM commit's time). tools/fpga-build.sh and"
      echo "# make litedram-gen check them (ip/litedram/lock.sh check). The last line is the simulation"
      echo "# core (make test-ddr-litedram), not part of any bitstream."
      for path in $FILES; do echo "$(sha "$path")  $path"; done
    } > "$lock.tmp"
    mv "$lock.tmp" "$lock"
    echo "lock.sh: wrote ip/litedram/arty.sha256"
    sed -n '/^[0-9a-f]/p' "$lock" ;;
  *)
    echo "usage: $0 check|write" >&2
    exit 2 ;;
esac
