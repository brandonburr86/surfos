#!/bin/sh
# SurfCPU open-flow test, the part inside the openXC7 image (tools/openxc7-checks-test.sh):
# DIR is an openxc7-build.sh output of devenv/openxc7/test/pll_sized.v, TOP its top. Checks that
# devenv/openxc7/cmtcheck.py fails a FASM in which the PLL's 90-degree output (CLKOUT3, divide 5:
# 1.25 VCO cycles, DELAY_TIME 1 and PHASE_MUX 2 by XAPP888) carries another phase: PHASE_MUX 3
# (an eighth of a VCO cycle late) or DELAY_TIME 0 (a whole cycle early).
set -u
dir=$1 top=$2
t=$dir/cmt-test
rm -rf "$t"
mkdir -p "$t"
mutate() {     # NAME FIELD VALUE: the build's FASM with CLKOUT3's FIELD set to VALUE, checked
    sed "s/\(PLLE2_ADV\.CLKOUT3_$2\[[0-9]:0\] = \)[0-9]*'b[01]*/\1$3/" "$dir/$top.fasm" > "$t/$1.fasm"
    if cmp -s "$dir/$top.fasm" "$t/$1.fasm"; then
        echo "$1: the FASM has no CLKOUT3_$2 to change"
        return
    fi
    python3 devenv/openxc7/cmtcheck.py --json "$dir/$top.json" --placement "$dir/placement.json" \
        --fasm "$t/$1.fasm" > "$t/$1.txt" 2>&1
    echo "$1: cmtcheck exit $?; $(grep 'CLKOUT3' "$t/$1.txt" | sed 's/^ *//')"
}
mutate phase-mux CLKOUT1_PHASE_MUX "3'b011"
mutate delay-time CLKOUT2_DELAY_TIME "6'b000000"
