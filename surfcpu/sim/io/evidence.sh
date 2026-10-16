#!/bin/sh
# SurfCPU io evidence
# --------------------
# File: evidence.sh Date: 10/15/26 (SurfCPU)
# --------------------
# Writes docs/evidence/io/tests.txt and synth.txt from a fresh run of the io tests and the
# Yosys estimates (make evidence-io runs this inside the toolchain container, from surfcpu/).
# The files hold the commands, seeds, the tool versions and the result lines, nothing else.
set -eu

out=docs/evidence/io
build=${BUILD:-build}/sim/io
seeds=${IO_SEEDS:-1 2 3 4 5}
mkdir -p "$out"

{
  echo "SurfCPU io track: test evidence (simulation only; nothing here ran on the board)"
  echo "date (UTC): $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "source: ${SURFCPU_BUILD_ID:-unknown} (dirty=${SURFCPU_BUILD_DIRTY:-?})"
  grep -E '^(verilator|iverilog|yosys|python3) ' /etc/surfcpu-toolchain.txt || true
  echo
  echo "command: make lint-io test-io IO_SEEDS=\"$seeds\""
  echo
  echo "== lint and font"
  cat "$build/lint.txt"
  echo
  echo "== models on their own (software SPI master / PS/2 host / generated VGA signals)"
  for s in $seeds; do
    tail -1 "$build/test_sdcard_model.$s.txt"
    tail -1 "$build/test_ps2_model.$s.txt"
    tail -1 "$build/test_vga_capture.$s.txt"
  done
  echo
  echo "== surf_spi + SD card model (Verilator)"
  for s in $seeds; do cat "$build/spi/run.$s.txt"; done
  echo
  echo "== surf_ps2 + PS/2 keyboard model (Verilator)"
  for s in $seeds; do cat "$build/ps2/run.$s.txt"; done
  echo
  echo "== surf_video + VGA capture (Verilator; sys clock by seed: 1,5 = 25 MHz, 2 = 50, 3 = 10, 4 = 30)"
  for s in $seeds; do cat "$build/video/run.$s.txt"; done
  echo
  echo "== the three blocks in Icarus"
  grep PASS "$build/tb_io_icarus.txt"
} > "$out/tests.txt"

{
  echo "SurfCPU io track: Yosys synth_xilinx estimates (xc7, flattened, per block)"
  echo "An estimate from the open-source flow, not a Vivado result; nothing was placed or routed."
  echo "date (UTC): $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "source: ${SURFCPU_BUILD_ID:-unknown} (dirty=${SURFCPU_BUILD_DIRTY:-?})"
  grep -E '^yosys ' /etc/surfcpu-toolchain.txt || true
  echo "command: make synth-io"
  echo
  cat "$build/synth/summary.txt"
} > "$out/synth.txt"

echo "evidence-io: wrote $out/tests.txt and $out/synth.txt"
