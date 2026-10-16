#!/bin/sh
# SurfCPU core: regenerate the evidence files
# --------------------
# File: make_evidence.sh Date: 10/15/26 (SurfCPU)
# --------------------
# Runs the core's checks and writes their output, with the commit, date and command, to
# docs/evidence/cpu/*.txt. Run inside the toolchain container from surfcpu/:
#
#   devenv/run.sh sim/cpu/make_evidence.sh
set -eu

ev=docs/evidence/cpu
mkdir -p "$ev"
id="${SURFCPU_BUILD_ID:-unknown}, dirty=${SURFCPU_BUILD_DIRTY:-?}"
now=$(date -u +%Y-%m-%dT%H:%MZ)

run() {     # run FILE TITLE COMMAND...
    file=$1; title=$2; shift 2
    {
        printf '# %s\n# commit %s; %s; container %s\n# command: %s\n\n' \
            "$title" "$id" "$now" "$(uname -m)" "$*"
        "$@" 2>&1
    } > "$ev/$file"
    echo "wrote $ev/$file"
}

run lint.txt "surfcpu_core: Verilator -Wall lint" make --no-print-directory lint-cpu
run tb-selftest.txt "surfcpu_tb: its own contract (exit codes, signature, trace)" \
    make --no-print-directory test-cpu-tb
run directed-tests.txt "surfcpu_core: directed tests on surfcpu_tb (Verilator), every timing and seed" \
    make --no-print-directory test-cpu-directed
run riscv-tests.txt "surfcpu_core: riscv-tests rv32ui/rv32um/rv32mi p tests on surfcpu_tb" \
    make --no-print-directory test-cpu-riscv-tests
run icarus.txt "surfcpu_core: RAM-only directed tests under Icarus Verilog" \
    make --no-print-directory test-cpu-icarus
run spike-tests.txt "the directed tests on Spike (checks the tests' own expectations)" \
    make --no-print-directory test-cpu-spike
run spike-fuzz.txt "random programs: surfcpu_tb traces against Spike's commit log, three timings" \
    python3 sim/cpu/fuzz_spike.py --count 200 --seed 1000 --length 600
run synth.txt "surfcpu_core: Yosys synth_xilinx resource estimate" \
    sh -c 'yosys -V; make --no-print-directory synth-cpu'
