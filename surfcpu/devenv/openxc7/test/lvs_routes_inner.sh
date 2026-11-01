#!/bin/sh
# SurfCPU open-flow test, the part inside the openXC7 image (tools/openxc7-lvs-test.sh): DIR is an
# openxc7-build.sh output of devenv/openxc7/test/bram_we_sdp.v. Byte 3's enable really routed with
# byte 0's (WEBWE6 and WEBWE7 on WEBWE0's net, devenv/openxc7/test/lvs_bram_mutate.py) while the
# netlist ties it to 0: the difference tools/openxc7-checks-test.sh shows the libz3 route finding.
# Here both routes prove the same mutated fabric, and lvs_verdict (tools/openxc7-steps.sh) reads
# each one's end.
set -u
dir=$1 top=$2 dev=$3
db=${PRJXRAY_DB_DIR:-/opt/openxc7/share/prjxray-db}/artix7
t=$dir/lvs-routes
rm -rf "$t"
mkdir -p "$t"
python3 -I devenv/openxc7/test/lvs_bram_mutate.py "$dir/lvs/fabric.raw.v" "$t/byte3.v" WEBWE6 WEBWE7 | sed 's/^/byte3: /'
python3 -I devenv/openxc7/bramwe.py --fabric "$t/byte3.v" --fasm "$dir/$top.fasm" --report "$t/byte3-bramwe.txt" > /dev/null
grep '^bramwe:' "$t/byte3-bramwe.txt" | sed 's/^/byte3: /'
dry=0 out=$t
. tools/openxc7-steps.sh
for s in libz3 z3cmd; do
  if [ "$s" = libz3 ]; then set --; else set -- --solver "python3 -I devenv/openxc7/z3cmd.py" --format smt2; fi
  lvs_equiv --gold "$dir/lvs/gold.v" --gold-top "$top" --gate "$t/byte3.v" --gate-top fabric \
    --placement "$dir/placement.json" --gold-json "$dir/$top.json" --db "$db" --device "$dev" "$@" > "$t/$s.txt" 2>&1
  st=$?
  echo "$s: lvs_equiv exit $st, read as $(lvs_verdict "$st" "$t/$s.txt")"
  grep -E '^solver:|DIFFER|proved,' "$t/$s.txt" | sed "s/^/$s:   /"
done
