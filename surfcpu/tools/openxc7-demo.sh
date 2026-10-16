#!/bin/sh
# The open flow's end-to-end proof run on the Mac (docs/OPEN-FLOW.md): builds flow_demo for
# the Arty A7-35T and A7-100T with tools/openxc7-build.sh (every board-free check included),
# simulates each build's gate-level netlist with the demo testbench in the surfcpu-toolchain
# image, rebuilds the 35T to show the flow is deterministic, and writes a summary to
# build/openxc7/demo-summary.txt. Exit status 0 only if everything passed. Nothing here
# touches a board: the bitstreams' behaviour on the hardware stays pending.
#
#   tools/openxc7-demo.sh            both boards
#   tools/openxc7-demo.sh arty_a7_35t
set -eu
here=$(cd "$(dirname "$0")/.." && pwd -P)
cd "$here"
boards=${*:-arty_a7_35t arty_a7_100t}
summary=build/openxc7/demo-summary.txt
mkdir -p build/openxc7
src="devenv/openxc7/demo/flow_demo.v rtl/lib/surf_reset_sync.v"
xdc=devenv/openxc7/demo/flow_demo.xdc
fail=0
seed=1

{
  echo "openXC7 flow demo, $(date -u +%Y-%m-%dT%H:%M:%SZ), revision $(git -C "$here" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)$( [ -n "$(git -C "$here" status --porcelain --untracked-files=no 2>/dev/null)" ] && echo ' (dirty)')"
} > "$summary"

for board in $boards; do
  out=build/openxc7/flow_demo-$board
  t0=$(date +%s)
  # shellcheck disable=SC2086
  if tools/openxc7-build.sh --top flow_demo --board "$board" --xdc "$xdc" --out "$out" $src; then r=PASS; else r=FAIL; fail=1; fi
  t1=$(date +%s)
  {
    echo
    echo "== $board: build and checks $r ($((t1 - t0)) s wall clock, container start included)"
    sed -n 's/^part   */part: /p; s/^bitstream   */bitstream: /p; s/^check \(.*\)/check \1/p' "$out/manifest.txt" 2>/dev/null
    grep '^clock ' "$out/timing.txt" 2>/dev/null || true
    sed -n '/documented capacity/,/DSP48E1/p' "$out/utilization.txt" 2>/dev/null
    sed -n '/^sync word/p; /standard bitstream length/p; /^IDCODE written/p; /^frame data/p' "$out/check-bitstream.txt" 2>/dev/null
    grep -E '^routed FASM|^only in' "$out/check-roundtrip.txt" 2>/dev/null || true
    grep -E 'proved,' "$out/check-lvs.txt" 2>/dev/null || true
    echo "step times:"; sed 's/^/  /' "$out/times.txt" 2>/dev/null || true
  } >> "$summary"

  # The synthesized netlist under the same testbench, with this Yosys's primitive models.
  if [ "$r" = PASS ]; then
    vvp_file=$out/flow_demo_gate.vvp
    if devenv/run.sh iverilog -g2012 -Wno-timescale -DNETLIST -o "$vvp_file" \
         devenv/openxc7/demo/flow_demo_tb.v "$out/flow_demo.netlist.v" "$out/cells_sim.v" \
         > "$out/gate-sim.txt" 2>&1 &&
       devenv/run.sh vvp -n "$vvp_file" +seed="$seed" >> "$out/gate-sim.txt" 2>&1 &&
       grep -q 'flow_demo_tb: PASS' "$out/gate-sim.txt"
    then gs=PASS; else gs=FAIL; fail=1; fi
    echo "gate-level simulation of the synthesized netlist (seed $seed): $gs" >> "$summary"
    grep -E '^reset:|PASS|FATAL' "$out/gate-sim.txt" | sed 's/^/  /' >> "$summary"
    seed=$((seed + 1))
  fi
done

# Determinism: the same sources, seed and image must give the same FASM and frames.
case " $boards " in
  *" arty_a7_35t "*)
    again=build/openxc7/flow_demo-arty_a7_35t-again
    # shellcheck disable=SC2086
    if tools/openxc7-build.sh --top flow_demo --board arty_a7_35t --xdc "$xdc" --out "$again" --no-check $src > /dev/null &&
       cmp -s build/openxc7/flow_demo-arty_a7_35t/flow_demo.fasm "$again/flow_demo.fasm" &&
       cmp -s build/openxc7/flow_demo-arty_a7_35t/flow_demo.frames "$again/flow_demo.frames"
    then det=PASS; else det=FAIL; fail=1; fi
    echo >> "$summary"
    echo "determinism: a second 35T build gives the same FASM and frames: $det" >> "$summary"
    ;;
esac

echo >> "$summary"
echo "result: $([ $fail = 0 ] && echo PASS || echo FAIL) (the bitstreams have not been on a board; physical behaviour pending)" >> "$summary"
cat "$summary"
exit $fail
