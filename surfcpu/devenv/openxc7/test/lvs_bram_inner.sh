#!/bin/sh
# SurfCPU open-flow test, the part inside the openXC7 image (tools/openxc7-lvs-bram-test.sh):
# DIR is an openxc7-build.sh output of devenv/openxc7/test/bram_we_sdp.v. Checks what the LVS
# step concludes from the extractor's own netlist (fabric.raw.v, one to one: a difference that
# is not in the bitstream) and from two deliberately wrong routings: WEBWE7 on the first byte
# enable's net (a pin pair that disagrees: bramwe.py must find it), and WEBWE6 and WEBWE7 both
# there (byte 3 really enabled with byte 0, while the netlist ties it to 0: the proof must).
set -u
dir=$1 top=$2
db=${PRJXRAY_DB_DIR:-/opt/openxc7/share/prjxray-db}/artix7
dev=$3
t=$dir/lvs-test
rm -rf "$t"
mkdir -p "$t"
prove() {     # FABRIC LOG: lvs_equiv on that fabric against the build's gold netlist
    lvs_equiv --gold "$dir/lvs/gold.v" --gold-top "$top" --gate "$1" --gate-top fabric \
        --placement "$dir/placement.json" --gold-json "$dir/$top.json" --db "$db" --device "$dev" > "$2" 2>&1
    echo $?
}
echo "raw: proof on the extractor's own netlist: exit $(prove "$dir/lvs/fabric.raw.v" "$t/raw.txt")"
grep -E 'proved,|DIFFER' "$t/raw.txt" | sed 's/^/raw:   /'
python3 -I devenv/openxc7/test/lvs_bram_mutate.py "$dir/lvs/fabric.raw.v" "$t/pair.v" WEBWE7 | sed 's/^/pair: /'
python3 -I devenv/openxc7/bramwe.py --fabric "$t/pair.v" --fasm "$dir/$top.fasm" --report "$t/pair-bramwe.txt" > /dev/null
grep -E 'DIFFER|^bramwe:' "$t/pair-bramwe.txt" | sed 's/^/pair:   /'
python3 -I devenv/openxc7/test/lvs_bram_mutate.py "$dir/lvs/fabric.raw.v" "$t/byte3.v" WEBWE6 WEBWE7 | sed 's/^/byte3: /'
python3 -I devenv/openxc7/bramwe.py --fabric "$t/byte3.v" --fasm "$dir/$top.fasm" --report "$t/byte3-bramwe.txt" > /dev/null
grep -E 'DIFFER|^bramwe:' "$t/byte3-bramwe.txt" | sed 's/^/byte3:   /'
echo "byte3: proof on the corrected netlist: exit $(prove "$t/byte3.v" "$t/byte3.txt")"
grep -E 'proved,|DIFFER' "$t/byte3.txt" | sed 's/^/byte3:   /'
