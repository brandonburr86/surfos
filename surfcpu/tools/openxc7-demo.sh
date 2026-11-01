#!/bin/sh
# The open flow's end-to-end proof run on the Mac (docs/OPEN-FLOW.md): builds flow_demo for
# the Arty A7-35T and A7-100T with tools/openxc7-build.sh (every board-free check included),
# simulates each build's gate-level netlist with the demo testbench in the surfcpu-toolchain
# image, rebuilds the 35T to show the flow is deterministic, builds the feature probe
# flow_features (block RAM, DSP, MMCM, PLL, IOBUF, two clock domains) for both parts, and
# builds it once with the planned pixel-clock settings, which must fail the clock check (a
# known nextpnr 1.0.0 defect), and runs the Yosys DSP-packing reproducer, which must fail the
# synthesis check with full DSP packing and pass in the safe mode. Writes a summary to build/openxc7/demo-summary.txt; exit
# status 0 only if everything came out as expected. Nothing here touches a board: the
# bitstreams' behaviour on the hardware stays pending. A build's verdict is its manifest's, not
# only the build script's exit status: a check the flow reports as BLOCKED (LVS stopped at its
# limit, or killed) leaves that status 0 but is not a pass.
#
#   tools/openxc7-demo.sh            both boards
#   tools/openxc7-demo.sh arty_a7_35t
#   tools/openxc7-demo.sh --verdict DIR STATUS   the verdict on one build (its directory and the
#                                    build script's exit status), as the demo gives it
set -eu
here=$(cd "$(dirname "$0")/.." && pwd -P)
cd "$here"

verdict() {     # DIR STATUS: PASS when the build script ended 0 and its manifest names every check
                # PASS (or none in the design); else FAIL, saying why
  if [ "$2" != 0 ]; then echo "FAIL (exit $2)"; return 0; fi
  if [ ! -f "$1/manifest.txt" ]; then echo "FAIL (no manifest)"; return 0; fi
  grep -q '^check ' "$1/manifest.txt" || { echo "FAIL (the manifest names no check)"; return 0; }
  v=$(awk '/^check / { l = substr($0, 7, 14); sub(/ +$/, "", l); r = substr($0, 21); sub(/ \([^()]*\)$/, "", r)
                       if (r != "PASS" && r != "none in the design") printf "%s%s %s", n++ ? "; " : "", l, r }' "$1/manifest.txt")
  if [ -z "$v" ]; then echo PASS; else echo "FAIL ($v)"; fi
}
if [ "${1:-}" = --verdict ]; then
  [ $# = 3 ] || { echo "usage: $0 --verdict DIR STATUS" >&2; exit 2; }
  verdict "$2" "$3"
  exit 0
fi
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
  if tools/openxc7-build.sh --top flow_demo --board "$board" --xdc "$xdc" --out "$out" $src; then st=0; else st=$?; fi
  r=$(verdict "$out" "$st")
  [ "$r" = PASS ] || fail=1
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

# The feature probe, and its planned-pixel-clock variant that the clock check must refuse.
fsrc="devenv/openxc7/demo/flow_features.v devenv/openxc7/demo/flow_features_core.v rtl/lib/surf_reset_sync.v rtl/lib/surf_sync.v rtl/lib/surf_cdc_word.v"
fxdc=devenv/openxc7/demo/flow_features.xdc
for board in $boards; do
  out=build/openxc7/flow_features-$board
  t0=$(date +%s)
  # shellcheck disable=SC2086
  if tools/openxc7-build.sh --top flow_features --board "$board" --xdc "$fxdc" --out "$out" $fsrc > "$out.txt" 2>&1
  then st=0; else st=$?; fi
  r=$(verdict "$out" "$st")
  [ "$r" = PASS ] || fail=1
  t1=$(date +%s)
  {
    echo
    echo "== flow_features on $board: build and checks $r ($((t1 - t0)) s wall clock)"
    grep '^check ' "$out/manifest.txt" 2>/dev/null
    grep '^clock ' "$out/timing.txt" 2>/dev/null || true
    sed -n '/documented capacity/,/DSP48E1/p' "$out/utilization.txt" 2>/dev/null
    grep -E 'proved,|hard blocks|not model|mmcm \(|pll \(' "$out/check-lvs.txt" 2>/dev/null || true
    grep -E 'DIVIDE|MULT|cmtcheck:' "$out/check-clocks.txt" 2>/dev/null || true
    echo "step times:"; sed 's/^/  /' "$out/times.txt" 2>/dev/null || true
  } >> "$summary"
done
case " $boards " in
  *" arty_a7_35t "*)
    out=build/openxc7/flow_features-planned-arty_a7_35t
    # shellcheck disable=SC2086
    tools/openxc7-build.sh --top flow_features --board arty_a7_35t -D FLOW_PIX_PLANNED --xdc "$fxdc" \
      --out "$out" $fsrc > "$out.txt" 2>&1 || true
    if grep -q '^check clocks        FAIL' "$out/manifest.txt" 2>/dev/null &&
       grep -q 'CLKOUT0_DIVIDE_F 35.25: MISMATCH' "$out/check-clocks.txt"
    then r="refused as expected (nextpnr 1.0.0 encodes the odd fractional divider unlike XAPP888)"
    else r="NOT REFUSED (expected the clock check to fail)"; fail=1; fi
    {
      echo
      echo "== flow_features with the planned pixel clock (8.875 / 35.25) on arty_a7_35t: $r"
      grep '^check ' "$out/manifest.txt" 2>/dev/null
      grep -E 'DIVIDE|MULT' "$out/check-clocks.txt" 2>/dev/null || true
    } >> "$summary"
    ;;
esac

# The Yosys DSP-packing defect: the reproducer must fail the synthesis check with Yosys's full
# DSP packing and pass in the default safe mode.
for mode in full safe; do
  out=build/openxc7/dsp_counter-$mode
  if tools/openxc7-build.sh --synth-only --dsp "$mode" --top dsp_counter --board arty_a7_35t --out "$out" \
       devenv/openxc7/test/dsp_counter.v > "$out.txt" 2>&1; then got=PASS; else got=FAIL; fi
  want=PASS; [ "$mode" = full ] && want=FAIL
  [ "$got" = "$want" ] || fail=1
  {
    echo
    echo "== dsp_counter (Yosys DSP-packing reproducer), --dsp $mode: synthesis check $got (expected $want)"
    grep -E '^whole design|^outputs constant|^PROBLEM' "$out/check-synthesis.txt" 2>/dev/null
  } >> "$summary"
done

echo >> "$summary"
echo "result: $([ $fail = 0 ] && echo PASS || echo FAIL) (the bitstreams have not been on a board; physical behaviour pending)" >> "$summary"
cat "$summary"
exit $fail
