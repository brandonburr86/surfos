#!/bin/sh
# SurfCPU: the open flow's checks on the three cases round ddr-3 found them misreading
# (docs/OPEN-FLOW.md), from the Mac with Docker and the surfcpu-openxc7 image, not part of
# `make check` (their self-tests are: make test-flow). Writes docs/evidence/flow/checkers.txt.
#   1. LVS and block RAM write enables: devenv/openxc7/test/bram_we_sdp.v (a 36-bit write port
#      with a byte lane tied to 0) and bram_we_tdp.v (an 18-bit write port with two byte
#      enables) through tools/openxc7-build.sh for the Arty A7-100T, whose LVS must pass; then,
#      on the first, the extractor's own one-to-one reading must fail the proof, and two
#      deliberately wrong routings must fail it (devenv/openxc7/test/lvs_bram_inner.sh);
#   2. the synthesis check and registers merged into a memory read port: LiteX's VexRiscv
#      (lite, build/third_party/litedram, make litedram-setup) synthesized alone (--synth-only),
#      which must pass with its 22 instruction-cache tag bits counted as merged;
#   3. the clock-tile check, Yosys's short bit strings and phases: devenv/openxc7/test/pll_sized.v
#      (LiteDRAM's PLL, its parameters as the generator writes them) through the whole flow,
#      whose clock check must pass with LiteDRAM's dividers read right (16, 8, 20, 5) and its
#      90-degree output's phase checked; then two wrong phases written into its FASM must fail
#      the check (devenv/openxc7/test/cmt_phase_inner.sh);
#   4. nextpnr and an 18-bit port B in true dual port mode: devenv/openxc7/test/bram_we_tdpb.v,
#      whose two byte enables nextpnr 1.0.0 routes to the wrong site pins; LVS must refuse it, with
#      bramwe.py naming both pin pairs. (Should a later nextpnr pair them as Vivado does, LVS
#      passes and this case fails: then the case, the probe and OPEN-FLOW.md 6.5 change.)
set -eu
here=$(cd "$(dirname "$0")/.." && pwd -P)
cd "$here"
out=build/flow/checks
ev=docs/evidence/flow/checkers.txt
rm -rf "$out"
mkdir -p "$out"
image=surfcpu-openxc7:$(sed -n 's/^IMAGE_TAG=//p' tools/openxc7-build.sh)
rc=0
ok() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; rc=1; fi; }
{
echo "== 1. LVS and block RAM write enables"
for d in bram_we_sdp bram_we_tdp; do
    if tools/openxc7-build.sh --top "$d" --board arty_a7_100t --xdc devenv/openxc7/test/bram_we.xdc \
           --out "$out/$d" "devenv/openxc7/test/$d.v" > "$out/$d.log" 2>&1; then st=0; else st=$?; fi
    echo "$d: openxc7-build.sh exit $st; $(grep 'checks:' "$out/$d.log" | sed 's/^openxc7-build.sh: //')"
    grep -E 'proved,|DIFFER|re-read|^bramwe:' "$out/$d/check-lvs.txt" | sed "s/^/  /"
    ok "$d: LVS passes" "grep -q '^check LVS           PASS' '$out/$d/manifest.txt'"
done
SURFCPU_IMAGE=$image devenv/run.sh sh devenv/openxc7/test/lvs_bram_inner.sh "$out/bram_we_sdp" bram_we_sdp xc7a100t \
    > "$out/inner.txt" 2>&1 || true
sed 's/^/  /' "$out/inner.txt"
ok "the extractor's own one-to-one reading fails the proof on WEBWE[3]" \
   "grep -q '^raw: proof on the extractor.s own netlist: exit [1-9]' '$out/inner.txt' && grep -q '^raw:   *DIFFER .*WEBWE\[3\]' '$out/inner.txt'"
ok "a pin pair that disagrees is a difference" "grep -q '^pair:   *bramwe: .* 1 difference(s)$' '$out/inner.txt'"
ok "byte 3 really enabled while the netlist ties it to 0 fails the proof" \
   "grep -q '^byte3: proof on the corrected netlist: exit [1-9]' '$out/inner.txt' && grep -q '^byte3:   *DIFFER .*WEBWE\[3\]' '$out/inner.txt'"
echo
echo "== 2. the synthesis check and registers merged into a memory read port"
vex=build/third_party/litedram/src/pythondata-cpu-vexriscv/pythondata_cpu_vexriscv/verilog/VexRiscv_Lite.v
if [ -f "$vex" ]; then
    if tools/openxc7-build.sh --top VexRiscv --part xc7a100tcsg324-1 --synth-only --out "$out/vexriscv" "$vex" \
           > "$out/vexriscv.log" 2>&1; then st=0; else st=$?; fi
    echo "VexRiscv (lite, synthesis only): openxc7-build.sh exit $st"
    grep -E '^(IBusCachedPlugin_cache|whole design|register bits|memories|PROBLEM|synthcheck)' "$out/vexriscv/check-synthesis.txt" | sed 's/^/  /'
    ok "VexRiscv passes, its 22 tag bits counted as merged" \
       "[ $st = 0 ] && grep -q 'merged into memory read ports in the RTL (memory_dff; not counted): 22$' '$out/vexriscv/check-synthesis.txt'"
else
    echo "FAIL  no $vex (make litedram-setup)"; rc=1
fi
echo
echo "== 3. the clock-tile check and Yosys's short bit strings"
if tools/openxc7-build.sh --top pll_sized --board arty_a7_100t --xdc devenv/openxc7/test/bram_we.xdc --no-lvs \
       --out "$out/pll_sized" devenv/openxc7/test/pll_sized.v > "$out/pll_sized.log" 2>&1; then st=0; else st=$?; fi
echo "pll_sized: openxc7-build.sh exit $st; $(grep 'checks:' "$out/pll_sized.log" | sed 's/^openxc7-build.sh: //')"
sed 's/^/  /' "$out/pll_sized/check-clocks.txt"
ok "LiteDRAM's PLL dividers read as 16, 8, 20 and 5 and match XAPP888" \
   "grep -q '^check clocks        PASS' '$out/pll_sized/manifest.txt' && grep -q 'CLKFBOUT_MULT 16: matches' '$out/pll_sized/check-clocks.txt' && grep -q 'CLKOUT1_DIVIDE 20: matches' '$out/pll_sized/check-clocks.txt'"
ok "the 90-degree output's phase is checked and matches XAPP888 (DELAY_TIME 1, PHASE_MUX 2)" \
   "grep -q 'CLKOUT3_DIVIDE 5, phase 90: matches XAPP888' '$out/pll_sized/check-clocks.txt' && grep -q ' 0 unchecked: PASS' '$out/pll_sized/check-clocks.txt'"
SURFCPU_IMAGE=$image devenv/run.sh sh devenv/openxc7/test/cmt_phase_inner.sh "$out/pll_sized" pll_sized \
    > "$out/cmt-inner.txt" 2>&1 || true
sed 's/^/  /' "$out/cmt-inner.txt"
ok "a PHASE_MUX an eighth of a VCO cycle late fails the check" \
   "grep -q '^phase-mux: cmtcheck exit 1; .*MISMATCH PHASE_MUX 3 (XAPP888: 2)' '$out/cmt-inner.txt'"
ok "a DELAY_TIME a whole cycle early fails the check" \
   "grep -q '^delay-time: cmtcheck exit 1; .*MISMATCH DELAY_TIME 0 (XAPP888: 1)' '$out/cmt-inner.txt'"
echo
echo "== 4. nextpnr and the byte enables of an 18-bit port B in true dual port mode"
if tools/openxc7-build.sh --top bram_we_tdpb --board arty_a7_100t --xdc devenv/openxc7/test/bram_we.xdc \
       --out "$out/bram_we_tdpb" devenv/openxc7/test/bram_we_tdpb.v > "$out/bram_we_tdpb.log" 2>&1; then st=0; else st=$?; fi
echo "bram_we_tdpb: openxc7-build.sh exit $st; $(grep 'checks:' "$out/bram_we_tdpb.log" | sed 's/^openxc7-build.sh: //')"
grep -E 'proved,|DIFFER|^bramwe:' "$out/bram_we_tdpb/check-lvs.txt" | sed 's/^/  /'
ok "nextpnr's port-B pairing (we0, we1, we0, we1 on WEBWE0..3) is refused: LVS fails and bramwe names both pin pairs" \
   "[ $st != 0 ] && grep -q '^check LVS           FAIL' '$out/bram_we_tdpb/manifest.txt' && grep -q '^bramwe: 1 block RAM(s), 2 enable(s) re-read, 2 difference(s)$' '$out/bram_we_tdpb/check-lvs.txt'"
} > "$out/report.txt" 2>&1 || true
grep -q '^FAIL' "$out/report.txt" && rc=1
{
    echo "SurfCPU flow evidence: the open flow's checks on the cases round ddr-3 found them misreading"
    echo "(tools/openxc7-checks-test.sh; devenv/openxc7/bramwe.py, synthcheck.py, cmtcheck.py; docs/OPEN-FLOW.md)."
    echo "Generated on $(date -u +%Y-%m-%d) at $(git rev-parse --short=12 HEAD)$( [ -z "$(git status --porcelain)" ] || echo ' (uncommitted changes)'), image $image."
    echo "Result: $( [ $rc = 0 ] && echo PASS || echo FAIL)"
    echo
    cat "$out/report.txt"
} > "$ev"
cat "$out/report.txt"
echo "openxc7-checks-test: $( [ $rc = 0 ] && echo PASS || echo FAIL) ($ev)"
exit $rc
