#!/bin/sh
# SurfCPU: a resource estimate of the generated LiteDRAM Arty core (Yosys synth_xilinx)
# File: estimate.sh Date: 10/15/26 (SurfCPU)
#
#   ip/litedram/estimate.sh build/litedram/arty
#
# Synthesizes litedram_core.v with its calibration CPU (VexRiscv.v from the pinned
# pythondata-cpu-vexriscv) for the 7 series and prints the cell counts. An estimate from an open
# tool, not Vivado's: the user port is the top level here, so its pins become OBUF/IBUF cells
# that would not exist inside the SoC.
set -eu
core=$(cd "$1" && pwd -P)
top=$(cd "$(dirname "$0")/../.." && pwd -P)
vex=$top/build/third_party/litedram/src/pythondata-cpu-vexriscv/pythondata_cpu_vexriscv/verilog/VexRiscv.v
cd "$core/gateware"          # the ROM's $readmemh file is found from here
yosys -q -p "read_verilog $vex litedram_core.v; synth_xilinx -top litedram_core -family xc7 -flatten; tee -q -o $core/synth_stat.txt stat" \
  > "$core/synth.log" 2>&1 || { tail -20 "$core/synth.log"; exit 1; }
luts=$(awk '/^ +LUT[1-6] /{s+=$2} END{print s}' "$core/synth_stat.txt")
ffs=$(awk '/^ +FD[A-Z]+ /{s+=$2} END{print s}' "$core/synth_stat.txt")
echo "estimate (yosys synth_xilinx, litedram_core + VexRiscv): LUTs $luts, flip-flops $ffs, $(awk '/^ +(RAMB18E1|RAMB36E1|RAM32M|DSP48E1) /{printf "%s %s, ", $1, $2}' "$core/synth_stat.txt" | sed 's/, $//')"
