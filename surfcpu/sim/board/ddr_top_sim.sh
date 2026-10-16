#!/bin/sh
# SurfCPU board track: the DDR top in RTL (make board-ddr-sim, in the toolchain container, from
# surfcpu/; docs/BOARD.md section 12).
#
#   [DDR_SIM_CORE=model|board] sh sim/board/ddr_top_sim.sh BOARD ROM STOP [MAX_MS] [plusargs...]
#
# surfcpu_arty_a7_top with ARTY_HAS_DDR exactly as the DDR designs build it (the real SoC, the
# bridge, the adapter, the board's clocking and resets; BOARD's BOARD_ID, this commit's BUILD_ID
# and BUILD_FLAGS, ROM in the boot ROM), with litedram_core either
#   model  (default) sim/board/litedram_simcore.v: LiteDRAM's real controller and registers
#          generated from the board's ip/litedram/arty.yml with its DRAM model in place of the
#          PHY and the chip, at its own 80 MHz; the firmware initializes it through surf_ddr_csr;
#   board  the generated core the bitstream builds (build/litedram/arty/), PHY included, with
#          sim/board/prims' models of its primitives (PLLE2_ADV, OSERDESE2, ISERDESE2, IDELAYE2,
#          IDELAYCTRL, IOBUFDS, OBUFDS, FDCE, FDPE) and no memory on its pins: the firmware's
#          initialization reaches its real registers and PHY, and its read leveling finds
#          nothing to read back.
# Both need make litedram-gen. The clock tiles are sim/board/prims' models. sim/board/tb_netlist.v
# runs it from its pins until a UART line begins with STOP, or MAX_MS milliseconds of simulated
# time (default 3000) have passed. Results in build/sim/board/ddr_top/<board>-<rom name>-<core><tag>/:
# uart.txt (the characters as received) and run.txt; DDR_SIM_TAG names a variant.
set -eu

board=${1:?usage: ddr_top_sim.sh BOARD ROM STOP [MAX_MS] [plusargs...]}
rom=${2:?usage: ddr_top_sim.sh BOARD ROM STOP [MAX_MS] [plusargs...]}
stop=${3:?usage: ddr_top_sim.sh BOARD ROM STOP [MAX_MS] [plusargs...]}
shift 3
max_ms=3000
case "${1:-}" in [0-9]*) max_ms=$1; shift ;; esac
mode=${DDR_SIM_CORE:-model}
here=$(pwd -P)
case "$mode" in
    model) core="build/litedram/arty_sim/gateware/litedram_sim.v"
           corefiles="sim/board/litedram_simcore.v $core"
           what="LiteDRAM's controller and DRAM model" ;;
    board) core="build/litedram/arty/gateware/litedram_core.v"
           corefiles="$core sim/board/prims/PLLE2_ADV.v sim/board/prims/OSERDESE2.v sim/board/prims/ISERDESE2.v
                      sim/board/prims/IDELAYE2.v sim/board/prims/IDELAYCTRL.v sim/board/prims/IOBUFDS.v
                      sim/board/prims/OBUFDS.v sim/board/prims/FDCE.v sim/board/prims/FDPE.v"
           what="the board's generated core, primitive models, no memory" ;;
    *) echo "ddr_top_sim: DDR_SIM_CORE is model or board" >&2; exit 2 ;;
esac
[ -f "$core" ] || { echo "ddr_top_sim: no $core (make litedram-gen)" >&2; exit 2; }
[ -f "$rom" ] || { echo "ddr_top_sim: no $rom (make build-rom)" >&2; exit 2; }
list=boards/arty_a7/designs/surfcpu_arty_a7_ddr_bramdiag.files
out=build/sim/board/ddr_top/$board-$(basename "$rom" .mem)-$mode${DDR_SIM_TAG:-}
rm -rf "$out"
mkdir -p "$out"

# the design list's committed sources and include directories (the generated core is replaced)
srcs=
for pat in $(awk '{ sub(/#.*/, "") } $1 == "src" { print $2 }' "$list" | grep -v '^build/'); do
    for f in $pat; do [ -f "$f" ] && srcs="$srcs $f"; done
done
incs=$(awk '{ sub(/#.*/, "") } $1 == "inc" { printf " -I%s", $2 }' "$list")
prims="sim/board/prims/surf_sim_cmt.v sim/board/prims/MMCME2_BASE.v sim/board/prims/PLLE2_BASE.v
       sim/board/prims/BUFG.v sim/board/prims/IOBUF.v"
id=$(printf '%.8s' "${SURFCPU_BUILD_ID:-0}" | grep -E '^[0-9a-f]{8}$' || echo 00000000)
flags=$(( ${SURFCPU_BUILD_DIRTY:-1} & 1 ))
echo "ddr_top_sim: surfcpu_arty_a7_top with ARTY_HAS_DDR as on $board, ROM $rom, BUILD_ID $id, BUILD_FLAGS $flags;"
echo "ddr_top_sim: litedram_core = $what ($core, sha256 $(sha256sum "$core" | cut -c1-16)...)"

t0=$(date +%s)
# shellcheck disable=SC2086
verilator --binary --timing -j 4 -O3 --timescale 1ps/1ps --top-module tb_netlist \
    -Wno-fatal -Wno-lint -Wno-style -Wno-MULTIDRIVEN $incs \
    -DARTY_HAS_DDR "-DARTY_BUILD_BOARD=\"$board\"" "-DARTY_BOOTROM_INIT=\"$here/$rom\"" \
    "-DARTY_BUILD_ID=32'h$id" "-DARTY_BUILD_FLAGS=32'h$(printf '%08x' "$flags")" \
    --Mdir "$out/obj" -o tb_ddr_top sim/board/tb_netlist.v $srcs $corefiles $prims \
    > "$out/build.txt" 2>&1 || { tail -30 "$out/build.txt"; exit 1; }
t1=$(date +%s)
st=0
"$out/obj/tb_ddr_top" "+stop=$stop" "+max_ms=$max_ms" "+transcript=$out/uart.txt" "$@" > "$out/run.txt" 2>&1 || st=$?
t2=$(date +%s)
rm -rf "$out/obj"
grep -v ' ms simulated' "$out/run.txt" | sed 's/^/ddr_top_sim: /' | tail -60
echo "ddr_top_sim: built in $((t1 - t0)) s, ran $((t2 - t1)) s (exit $st)"
[ "$st" = 0 ] && grep -q "uart| $stop" "$out/run.txt"
