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
# clock tiles are sim/board/prims' too. Results in build/sim/board/netlist/<board>-<design>/:
# uart.txt (the characters as received), run.txt (the console), and the time it took.
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
echo "netlist_sim: $board $design: $(grep -c '^  [A-Z][A-Z0-9_]* ' "$net") cells in ${net}, $simulator"

t0=$(date +%s)
# shellcheck disable=SC2086
case "$simulator" in
    verilator)
        verilator --binary --timing -j 4 -O3 --timescale 1ps/1ps --top-module tb_netlist \
            -Wno-fatal -Wno-lint -Wno-style -Wno-MULTIDRIVEN \
            --Mdir "$out/obj" -o tb_netlist sim/board/tb_netlist.v "$net" $models \
            > "$out/build.txt" 2>&1 || { tail -30 "$out/build.txt"; exit 1; }
        run="$out/obj/tb_netlist" ;;
    icarus)
        iverilog -g2012 -Wno-timescale -s tb_netlist -o "$out/tb_netlist.vvp" \
            sim/board/tb_netlist.v "$net" $models > "$out/build.txt" 2>&1 || { tail -30 "$out/build.txt"; exit 1; }
        run="vvp -n $out/tb_netlist.vvp" ;;
    *) echo "netlist_sim: SIM is verilator or icarus" >&2; exit 2 ;;
esac
t1=$(date +%s)
st=0
$run "+stop=$stop" "+transcript=$out/uart.txt" > "$out/run.txt" 2>&1 || st=$?
t2=$(date +%s)
rm -rf "$out/obj"
grep -v ' ms simulated' "$out/run.txt" | sed 's/^/netlist_sim: /' | tail -40
echo "netlist_sim: built in $((t1 - t0)) s, ran $((t2 - t1)) s (exit $st)"
[ "$st" = 0 ] && grep -q "uart| $stop" "$out/run.txt"
