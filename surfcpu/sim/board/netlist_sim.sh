#!/bin/sh
# SurfCPU board track: the post-synthesis simulation of the full top (make board-netlist-sim, in
# the toolchain container, from surfcpu/; docs/BOARD.md section 12).
#
#   sh sim/board/netlist_sim.sh DIR [STOP] [SIM]
#
# DIR is an open-flow build's directory (build/fpga/<board>/<design>/openxc7) holding
# surfcpu_arty_a7_top.netlist.v, the netlist Yosys wrote and LVS proves the bitstream equals,
# and cells_sim.v, the cell models of the same Yosys. sim/board/tb_netlist.v runs that netlist
# from its pins (the ROM is inside it, as the bitstream's block RAM contents) until a UART line
# begins with STOP (default "BRAMDIAG:"). SIM is verilator (default) or icarus.
# Yosys's block RAM models have no behaviour, so those of sim/board/prims replace them; the
# clock tiles are sim/board/prims' too. A DDR design's netlist (ARTY_HAS_DDR: its ports include
# ddr3_*) also gets sim/board/prims' models of LiteDRAM's primitives and no memory on its DDR3L
# pins: the BRAM diagnostics in its boot ROM initialize the controller through the netlist's
# register bridge and level its read path, which finds no working delay (nothing answers).
# NETLIST_MAX_MS sets the simulated-time limit (default 300 ms, 1500 for a DDR design). Results in
# build/sim/board/netlist/<board>-<design>/: uart.txt (the characters as received), run.txt (the
# console), and the time it took.
set -eu

dir=${1:?usage: netlist_sim.sh DIR [STOP] [SIM]}
stop=${2:-BRAMDIAG:}
simulator=${3:-verilator}
net=$dir/surfcpu_arty_a7_top.netlist.v
[ -f "$net" ] && [ -f "$dir/cells_sim.v" ] ||
    { echo "netlist_sim: no netlist in $dir (make build-fpga FLOW=openxc7 ... first)" >&2; exit 2; }
design=$(basename "$(dirname "$dir")")
board=$(basename "$(dirname "$(dirname "$dir")")")
out=build/sim/board/netlist/$board-$design
rm -rf "$out"
mkdir -p "$out"

# Yosys's cell models without its RAMB18E1/RAMB36E1 (ports and parameters only)
awk '/^module (RAMB18E1|RAMB36E1)[ (#]/ { skip = 1 } !skip { print } skip && /^endmodule/ { skip = 0 }' \
    "$dir/cells_sim.v" > "$out/cells_sim_noramb.v"
models="$out/cells_sim_noramb.v sim/board/prims/RAMB36E1.v sim/board/prims/RAMB18E1.v
        sim/board/prims/surf_sim_cmt.v sim/board/prims/MMCME2_BASE.v sim/board/prims/PLLE2_BASE.v"
ddr=
max_ms=${NETLIST_MAX_MS:-300}
if grep -q 'ddr3_addr' "$net"; then                   # a DDR design
    ddr=-DARTY_HAS_DDR
    max_ms=${NETLIST_MAX_MS:-1500}
    models="$models sim/board/prims/PLLE2_ADV.v sim/board/prims/OSERDESE2.v sim/board/prims/ISERDESE2.v
            sim/board/prims/IDELAYE2.v sim/board/prims/IDELAYCTRL.v sim/board/prims/IOBUFDS.v
            sim/board/prims/OBUFDS.v"
fi
echo "netlist_sim: $board $design: $(grep -c '^  [A-Z][A-Z0-9_]* ' "$net") cells in ${net}, $simulator"

t0=$(date +%s)
# shellcheck disable=SC2086
case "$simulator" in
    verilator)
        verilator --binary --timing -j 4 -O3 --timescale 1ps/1ps --top-module tb_netlist \
            -Wno-fatal -Wno-lint -Wno-style -Wno-MULTIDRIVEN $ddr \
            --Mdir "$out/obj" -o tb_netlist sim/board/tb_netlist.v "$net" $models \
            > "$out/build.txt" 2>&1 || { tail -30 "$out/build.txt"; exit 1; }
        run="$out/obj/tb_netlist" ;;
    icarus)
        iverilog -g2012 -Wno-timescale $ddr -s tb_netlist -o "$out/tb_netlist.vvp" \
            sim/board/tb_netlist.v "$net" $models > "$out/build.txt" 2>&1 || { tail -30 "$out/build.txt"; exit 1; }
        run="vvp -n $out/tb_netlist.vvp" ;;
    *) echo "netlist_sim: SIM is verilator or icarus" >&2; exit 2 ;;
esac
t1=$(date +%s)
st=0
$run "+stop=$stop" "+max_ms=$max_ms" "+transcript=$out/uart.txt" > "$out/run.txt" 2>&1 || st=$?
t2=$(date +%s)
rm -rf "$out/obj"
grep -v ' ms simulated' "$out/run.txt" | sed 's/^/netlist_sim: /' | tail -40
echo "netlist_sim: built in $((t1 - t0)) s, ran $((t2 - t1)) s (exit $st)"
[ "$st" = 0 ] && grep -q "uart| $stop" "$out/run.txt"
