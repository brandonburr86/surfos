#!/bin/sh
# SurfCPU: the LVS proof's two routes and the recheck through the real open flow (docs/OPEN-FLOW.md
# section 3), from the Mac with Docker and the surfcpu-openxc7 image; not part of `make check`
# (their tests with stand-ins are: make test-flow, make check-board). Writes
# docs/evidence/flow/lvs-routes.txt.
#   1. devenv/openxc7/test/bram_we_sdp.v (a block RAM; 104 LVS points) through tools/openxc7-build.sh
#      for the A7-100T by each route: LVS PASS both, the same points proved, z3cmd's self-test
#      first and passing on the image's libz3;
#   2. the libz3 build rechecked by the z3cmd route (--recheck): PASS, the build's manifest put
#      aside as it was, its lines up to the bitstream kept, gold.v and fabric.v made again the same
#      bytes; then that recheck's directory rechecked again by libz3, from the build's manifest;
#   3. the z3cmd build's bitstream with a byte added: the recheck refuses it (exit 3), changing
#      nothing;
#   4. byte 3 of the block RAM really enabled with byte 0 while the netlist ties it to 0 (the
#      difference of tools/openxc7-checks-test.sh's case 1): both routes find WEBWE[3] differing,
#      and the flow reads each as FAIL (1 differ) (devenv/openxc7/test/lvs_routes_inner.sh).
set -eu
here=$(cd "$(dirname "$0")/.." && pwd -P)
cd "$here"
out=build/flow/lvs-routes
ev=docs/evidence/flow/lvs-routes.txt
rm -rf "$out"
mkdir -p "$out"
image=surfcpu-openxc7:$(sed -n 's/^IMAGE_TAG=//p' tools/openxc7-build.sh)
rev=$(git rev-parse --short=12 HEAD)                  # the tree as it was at the start, this record aside
state=$( [ -z "$(git status --porcelain | grep -v " $ev\$")" ] || echo ' (uncommitted changes)')
rc=0
ok() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; rc=1; fi; }
top=bram_we_sdp
{
echo "== 1. one design, both routes"
for s in libz3 z3cmd; do
    if tools/openxc7-build.sh --top $top --board arty_a7_100t --xdc devenv/openxc7/test/bram_we.xdc --lvs-solver $s \
           --out "$out/$s" devenv/openxc7/test/$top.v > "$out/$s.log" 2>&1; then st=0; else st=$?; fi
    echo "$s: openxc7-build.sh exit $st; $(grep 'checks:' "$out/$s.log" | sed 's/^openxc7-build.sh: //')"
    grep -E '^solver:|proved,|^z3cmd:' "$out/$s/check-lvs.txt" | sed 's/^/  /'
    grep -E '^(check LVS|lvs solver|lvs timeout|manifest format|netlist|routed FASM|placement) ' "$out/$s/manifest.txt" | sed 's/^/  /'
    ok "$s: LVS PASS, the manifest naming the route" \
       "[ $st = 0 ] && grep -q '^check LVS           PASS (.*by the $s route' '$out/$s/manifest.txt' && grep -q '^lvs solver          $s ' '$out/$s/manifest.txt'"
done
grep -h '^  proved  ' "$out/libz3/check-lvs.txt" | sort > "$out/libz3.proved"
grep -h '^  proved  ' "$out/z3cmd/check-lvs.txt" | sort > "$out/z3cmd.proved"
ok "the two routes prove the same $(wc -l < "$out/libz3.proved" | tr -d ' ') points" "[ -s '$out/libz3.proved' ] && cmp -s '$out/libz3.proved' '$out/z3cmd.proved'"
sed 's/^/  /' "$out/z3cmd/lvs/z3cmd-selftest.txt"
ok "z3cmd: its self-test ran first and passed, on the image's libz3" "grep -q '^z3cmd self-test: PASS' '$out/z3cmd/lvs/z3cmd-selftest.txt'"
echo
echo "== 2. the libz3 build rechecked by the z3cmd route, then by libz3"
cp "$out/libz3/manifest.txt" "$out/libz3-build-manifest.txt"
if tools/openxc7-build.sh --recheck --out "$out/libz3" --lvs-solver z3cmd > "$out/recheck.log" 2>&1; then st=0; else st=$?; fi
echo "recheck by z3cmd: exit $st; $(grep 'recheck:' "$out/recheck.log" | sed 's/^openxc7-build.sh: //')"
grep -E '^(manifest format|check |lvs |recheck |checks |build manifest)' "$out/libz3/manifest.txt" | sed 's/^/  /'
ok "the recheck passes, LVS run again by z3cmd" "[ $st = 0 ] && grep -q '^check LVS           PASS (run again: .*by the z3cmd route' '$out/libz3/manifest.txt'"
ok "the build's manifest is put aside as it was" "cmp -s '$out/libz3/manifest.build.txt' '$out/libz3-build-manifest.txt'"
ok "the build's lines up to its bitstream are kept" \
   "[ \"\$(awk 'NR > 2 && /^bitstream / { print; exit } NR > 2' '$out/libz3/manifest.txt')\" = \"\$(awk 'NR > 2 && /^bitstream / { print; exit } NR > 2' '$out/libz3-build-manifest.txt')\" ]"
ok "gold.v, fabric.raw.v and fabric.v made again are the build's bytes" \
   "grep -q \"^lvs inputs .*gold.v the same bytes as the build's; fabric.raw.v the same bytes as the build's; fabric.v the same bytes as the build's\" '$out/libz3/manifest.txt'"
if tools/openxc7-build.sh --recheck --out "$out/libz3" --lvs-solver libz3 > "$out/recheck2.log" 2>&1; then st=0; else st=$?; fi
echo "recheck by libz3: exit $st; $(grep 'recheck:' "$out/recheck2.log" | sed 's/^openxc7-build.sh: //')"
ok "a second recheck reads the build's own manifest and passes by libz3" \
   "[ $st = 0 ] && grep -q 'its record $out/libz3/manifest.build.txt' '$out/recheck2.log' && grep -q '^check LVS           PASS (run again: .*by the libz3 route' '$out/libz3/manifest.txt' && cmp -s '$out/libz3/manifest.build.txt' '$out/libz3-build-manifest.txt'"
echo
echo "== 3. a bitstream changed after its build"
cp "$out/z3cmd/$top.bit" "$out/z3cmd.bit.saved"
cp "$out/z3cmd/manifest.txt" "$out/z3cmd-manifest.saved"
printf 'x' >> "$out/z3cmd/$top.bit"
if tools/openxc7-build.sh --recheck --out "$out/z3cmd" > "$out/refused.log" 2>&1; then st=0; else st=$?; fi
sed 's/^/  /' "$out/refused.log"
ok "the recheck refuses it (exit 3), changing nothing" \
   "[ $st = 3 ] && grep -q 'is not the one' '$out/refused.log' && [ ! -e '$out/z3cmd/manifest.build.txt' ] && cmp -s '$out/z3cmd/manifest.txt' '$out/z3cmd-manifest.saved'"
cp "$out/z3cmd.bit.saved" "$out/z3cmd/$top.bit"
echo
echo "== 4. a difference, by both routes"
SURFCPU_IMAGE=$image devenv/run.sh sh devenv/openxc7/test/lvs_routes_inner.sh "$out/z3cmd" $top xc7a100t \
    > "$out/inner.txt" 2>&1 || true
sed 's/^/  /' "$out/inner.txt"
ok "libz3 finds WEBWE[3] differing: FAIL (1 differ)" \
   "grep -q '^libz3: lvs_equiv exit 1, read as FAIL (1 differ)$' '$out/inner.txt' && grep -q '^libz3:   *DIFFER .*WEBWE\[3\]' '$out/inner.txt'"
ok "z3cmd finds it too: FAIL (1 differ)" \
   "grep -q '^z3cmd: lvs_equiv exit 1, read as FAIL (1 differ)$' '$out/inner.txt' && grep -q '^z3cmd:   *DIFFER .*WEBWE\[3\]' '$out/inner.txt'"
} > "$out/report.txt" 2>&1 || true
grep -q '^FAIL' "$out/report.txt" && rc=1
{
    echo "SurfCPU flow evidence: the LVS proof's two routes and the recheck through the real open flow"
    echo "(tools/openxc7-lvs-test.sh; tools/openxc7-steps.sh, devenv/openxc7/z3cmd.py; docs/OPEN-FLOW.md section 3)."
    echo "Generated on $(date -u +%Y-%m-%d) at $rev$state, image $image."
    echo "Result: $( [ $rc = 0 ] && echo PASS || echo FAIL)"
    echo
    cat "$out/report.txt"
} > "$ev"
cat "$out/report.txt"
echo "openxc7-lvs-test: $( [ $rc = 0 ] && echo PASS || echo FAIL) ($ev)"
exit $rc
