#!/bin/sh
# SurfCPU board track: tests of tools/fpga-build.sh and tools/program.sh.
#
# Fake ssh, docker, vivado and openFPGALoader (sim/board/tools/fake) stand in for the build host,
# Docker, Vivado and the board: the fake vivado runs the real boards/arty_a7/vivado/build.tcl in
# Yosys's Tcl with stubbed Vivado commands (vivado_stub.tcl). HOME is a temporary directory, so
# the build-host.conf files here are never the user's; the "remote" is a local directory.
# Nothing leaves this machine and nothing is programmed. Run from surfcpu/ in the toolchain
# container (mk/area-board.mk: board-tools). Prints one line per check and exits 1 on a failure.
set -u

root=$(pwd -P)
t=$root/build/sim/board/tools_test
rm -rf "$t"
mkdir -p "$t/home/.config/surfcpu" "$t/remote"
export HOME="$t/home" FAKE_LOG="$t/calls.log" FAKE_REMOTE="$t/remote" FAKE_HOST=builder
export FAKE_STUB="$root/sim/board/tools/vivado_stub.tcl" SOURCE_DATE_EPOCH=1760560000
export SURFCPU_FPGA_OUT="$t/fpga"                     # never the real build/fpga
PATH="$root/sim/board/tools/fake:$PATH"
export PATH
unset DESIGN BIT SURFCPU_BUILD_HOST_CONF SURFCPU_FLASH_CONFIRM OPENFPGALOADER 2>/dev/null || true
conf="$HOME/.config/surfcpu/build-host.conf"

pass=0
failed=0
check() {                                    # NAME COMMAND...: one test
    name=$1
    shift
    if "$@" > /dev/null 2>&1; then
        pass=$((pass + 1)); echo "ok   $name"
    else
        failed=$((failed + 1)); echo "FAIL $name"; echo "     last output:"; sed 's/^/     | /' "$t/out.txt" | tail -n 15
    fi
}
run() {                                      # COMMAND...: run, keep output and status
    : > "$FAKE_LOG"
    "$@" > "$t/out.txt" 2>&1
    rc=$?
}
has() { grep -q -- "$1" "$t/out.txt"; }
logged() { grep -q -- "$1" "$FAKE_LOG"; }
nolog() { ! [ -s "$FAKE_LOG" ]; }
is() { [ "$1" = "$2" ]; }
base=$SURFCPU_FPGA_OUT
time_hex=$(printf '%08x' "$SOURCE_DATE_EPOCH")

# ============================================================================== fpga-build.sh
run tools/fpga-build.sh arty_a7_100t
check "build: no build-host.conf -> pending, exit 2" is "$rc" 2
check "build: says where to read about it" has "docs/BUILD-HOST.md"

printf 'FLOW=vivado-ssh\nHOST=builder\nREMOTE_DIR=surfcpu-build\nVIVADO=vivado   # the pinned Vivado\n' > "$conf"
run tools/fpga-build.sh arty_a7_50t
check "build: unknown BOARD refused" is "$rc" 1
run tools/fpga-build.sh --design nosuch arty_a7_100t
check "build: unknown design refused" is "$rc" 1
run tools/fpga-build.sh arty_a7_100t
check "build: the full top without rtl/soc/surfcpu_soc.v is refused" has "refusing to build"
check "build: ... and nothing was contacted" nolog

printf 'FLOW=vivado-ssh\nHOST=builder;rm\nVIVADO=vivado\n' > "$conf"
run tools/fpga-build.sh --design arty_a7_baseline arty_a7_100t
check "build: a HOST with shell characters refused" has "characters outside"
printf 'FLOW=vivado-scp\nHOST=builder\nVIVADO=vivado\n' > "$conf"
run tools/fpga-build.sh --design arty_a7_baseline arty_a7_100t
check "build: unknown FLOW refused" has "unknown FLOW"

# ------------------------------------------------------------------------------ dry run (ssh)
printf 'FLOW=vivado-ssh\nHOST=builder\nREMOTE_DIR=surfcpu-build\nVIVADO=vivado\n' > "$conf"
run tools/fpga-build.sh --dry-run --design arty_a7_baseline arty_a7_100t
check "dry run: exit 0" is "$rc" 0
check "dry run: contacts nothing (no ssh/vivado call)" nolog
check "dry run: prints the ssh commands" has "ssh -o BatchMode=yes builder"
check "dry run: prints the Vivado command" has "vivado -mode batch"
p=$base/arty_a7_100t/arty_a7_baseline/stage/params.tcl
check "dry run: params.tcl has the 100T part" grep -q "set part {xc7a100tcsg324-1}" "$p"
check "dry run: params.tcl has the build identity generics" grep -q "BOARD {\"arty_a7_100t\"} BUILD_ID 32'h[0-9a-f]\{8\} BUILD_FLAGS 32'h0000000[01] BUILD_TIME 32'h$time_hex" "$p"
check "dry run: params.tcl has the scoped CDC constraints" grep -q "{surf_cdc_word boards/arty_a7/xdc/cdc/surf_cdc_word.xdc}" "$p"
check "dry run: leaves no real manifest" test ! -e "$base/arty_a7_100t/arty_a7_baseline/manifest.txt"
check "dry run: sources staged" test -f "$base/arty_a7_100t/arty_a7_baseline/stage/surfcpu/rtl/lib/surf_cdc_word.v"
check "dry run: include files staged" test -f "$base/arty_a7_100t/arty_a7_baseline/stage/surfcpu/rtl/generated/surfcpu_soc.vh"
check "dry run: build.tcl staged" test -f "$base/arty_a7_100t/arty_a7_baseline/stage/surfcpu/boards/arty_a7/vivado/build.tcl"

# ------------------------------------------------------------------------------ ssh flow, passing build
run tools/fpga-build.sh --design arty_a7_baseline arty_a7_35t
b=$base/arty_a7_35t/arty_a7_baseline
check "ssh: passing build exits 0" is "$rc" 0
check "ssh: made the remote directory" logged "ssh -o BatchMode=yes builder mkdir -p surfcpu-build/arty_a7_baseline-arty_a7_35t-"
check "ssh: ran Vivado on the host" logged "vivado -mode batch -nojournal -log out/vivado.log -source surfcpu/boards/arty_a7/vivado/build.tcl -tclargs params.tcl"
check "ssh: the bitstream came back" test -f "$b/arty_a7_baseline.bit"
check "ssh: no checkpoint came back" test ! -e "$b/out/post_route.dcp"
check "ssh: reports came back" test -f "$b/out/reports/timing_summary.rpt"
c=$b/out/calls.txt
check "build.tcl: set_part with the 35T's -1L part" grep -q "^set_part xc7a35ticsg324-1L$" "$c"
check "build.tcl: reads all four XDC files" test "$(grep -c '^read_xdc boards/arty_a7/xdc/arty_a7_' "$c")" -eq 4
check "build.tcl: reads the CDC files scoped and late" grep -q "^read_xdc -ref surf_pulse_sync -unmanaged" "$c"
check "build.tcl: CDC files processed late" grep -q "^set_property PROCESSING_ORDER LATE" "$c"
check "build.tcl: synthesizes the top with the generics" grep -q "^synth_design -top arty_a7_baseline -part xc7a35ticsg324-1L .*-generic BOARD=\"arty_a7_35t\" -generic BUILD_ID=32'h" "$c"
check "build.tcl: runs place and route" grep -q "^route_design" "$c"
check "build.tcl: writes the CDC and clock-interaction reports" grep -q "^report_cdc -details" "$c"
check "build.tcl: writes the bitstream last" test "$(tail -n 1 "$c" | cut -d' ' -f1)" = write_bitstream
m=$b/manifest.txt
check "manifest: is valid TOML" python3 -c "import tomllib,sys; tomllib.load(open(sys.argv[1],'rb'))" "$m"
for key in source_revision source_dirty part constraints_sha256 sources_sha256 isa abi map_version build_time; do
    check "manifest: has $key" grep -q "^$key = " "$m"
done
check "manifest: Vivado's results (timing, utilization, version, host)" python3 -c "
import tomllib,sys
d=tomllib.load(open(sys.argv[1],'rb'))
v=d['vivado']
assert v['result']=='pass' and v['wns_ns']==10.0 and v['lut']=='312 / 63400' and v['vivado_version']=='2025.1' and 'build_host' in v
assert d['isa']=='rv32im_zicsr_zifencei' and d['abi']=='ilp32' and d['part']=='xc7a35ticsg324-1L'
" "$m"
check "manifest: the bitstream's SHA-256" grep -q "^bitstream_sha256 = \"$(shasum -a 256 "$b/arty_a7_baseline.bit" | cut -d' ' -f1)\"" "$m"

# ------------------------------------------------------------------------------ failing checks stop the bitstream
for case in "FAKE_WNS=-0.250:setup timing" "FAKE_WHS=-0.010:hold timing" "FAKE_DRC=2:DRC errors" \
            "FAKE_CRIT=3:critical warnings" "FAKE_CDC=1:critical CDC" "FAKE_NO_INPUT_DELAY=4:check_timing"; do
    setting=${case%%:*}
    why=${case#*:}
    run env "$setting" tools/fpga-build.sh --design arty_a7_baseline arty_a7_100t
    b=$base/arty_a7_100t/arty_a7_baseline
    check "gate $setting: build fails" is "$rc" 1
    check "gate $setting: no bitstream written" test ! -e "$b/out/arty_a7_baseline.bit" -a ! -e "$b/arty_a7_baseline.bit"
    check "gate $setting: the reason names $why" grep -q "^reason = \".*$why" "$b/out/build-results.txt"
done

# ------------------------------------------------------------------------------ docker flow
printf 'FLOW=vivado-docker\nVIVADO=/tools/Xilinx/Vivado/2025.1/bin/vivado\nIMAGE=surfcpu-vivado:2025.1\n' > "$conf"
mkdir -p "$t/tools/Xilinx/Vivado/2025.1/bin"
run tools/fpga-build.sh --dry-run --design arty_a7_baseline arty_a7_100t
check "docker dry run: exit 0, no docker call" test "$rc" -eq 0 -a ! -s "$FAKE_LOG"
check "docker dry run: amd64 container with the stage mounted" has "run --rm --platform linux/amd64 -v $base/arty_a7_100t/arty_a7_baseline/stage:/build -w /build surfcpu-vivado:2025.1"
printf 'FLOW=vivado-docker\nVIVADO=vivado\nIMAGE=surfcpu-vivado:2025.1\n' > "$conf"
run tools/fpga-build.sh --design arty_a7_baseline arty_a7_100t
check "docker: passing build exits 0" is "$rc" 0
check "docker: ran in an amd64 container" logged "docker run --rm --platform linux/amd64 -v"
check "docker: the bitstream" test -f "$base/arty_a7_100t/arty_a7_baseline/arty_a7_baseline.bit"

# ============================================================================== program.sh
bit100=$base/arty_a7_100t/arty_a7_baseline/arty_a7_baseline.bit
bit35=$base/arty_a7_35t/arty_a7_baseline/arty_a7_baseline.bit
export DESIGN=arty_a7_baseline
run env PATH=/usr/bin:/bin "$root/tools/program.sh" volatile arty_a7_100t
check "program: no openFPGALoader -> pending, exit 2" is "$rc" 2
run env FAKE_IDCODE= tools/program.sh volatile arty_a7_100t
check "program: no cable/device refused" has "found no JTAG cable"
run env FAKE_IDCODE=0x0362d093 tools/program.sh volatile arty_a7_100t
check "program: 35T IDCODE with BOARD=arty_a7_100t refused" has "refusing"
check "program: ... and nothing was loaded" test "$(grep -c -- ' -b ' "$FAKE_LOG")" -eq 0
run env FAKE_IDCODE=0x13631093 FAKE_IDCODE2=0x13631093 tools/program.sh volatile arty_a7_100t
check "program: two devices on the chain refused" has "refusing to guess"
run env FAKE_IDCODE=0x13631093 tools/program.sh detect arty_a7_100t
check "program: detect reads the 100T (revision 1)" has "IDCODE 0x13631093 = BOARD arty_a7_100t"
run env FAKE_IDCODE=0x13631093 tools/program.sh volatile arty_a7_100t "$bit35"
check "program: a 35T bitstream on the 100T refused" has "was built for 7a35t"
run env FAKE_IDCODE=0x13631093 tools/program.sh volatile arty_a7_100t "$t/nosuch.bit"
check "program: missing bitstream refused" has "no bitstream"
run env FAKE_IDCODE=0x13631093 tools/program.sh volatile arty_a7_100t
check "program: volatile load exits 0" is "$rc" 0
check "program: loads configuration RAM with the board's name" logged "openFPGALoader -b arty_a7_100t $bit100"
check "program: ... and never the flash" test "$(grep -c -- ' -f ' "$FAKE_LOG")" -eq 0
run env FAKE_IDCODE=0x0362d093 tools/program.sh volatile arty_a7_35t
check "program: the 35T board with its own bitstream" is "$rc" 0
run env FAKE_IDCODE=0x13631093 FAKE_PROGRAM_RC=1 tools/program.sh volatile arty_a7_100t
check "program: a failing load exits 1" is "$rc" 1
run env FAKE_IDCODE=0x13631093 tools/program.sh flash arty_a7_100t < /dev/null
check "flash: refused without confirmation" has "not confirmed"
check "flash: ... nothing written" test "$(grep -c -- ' -f ' "$FAKE_LOG")" -eq 0
run env FAKE_IDCODE=0x13631093 SURFCPU_FLASH_CONFIRM=write-flash-arty_a7_35t tools/program.sh flash arty_a7_100t < /dev/null
check "flash: confirmation for another board refused" has "not confirmed"
run env FAKE_IDCODE=0x13631093 SURFCPU_FLASH_CONFIRM=write-flash-arty_a7_100t tools/program.sh flash arty_a7_100t < /dev/null
check "flash: confirmed write exits 0" is "$rc" 0
check "flash: writes and verifies the flash" logged "openFPGALoader -b arty_a7_100t -f --verify $bit100"
sed -i 's/^result = "pass"/result = "fail"/' "$base/arty_a7_100t/arty_a7_baseline/manifest.txt"
run env FAKE_IDCODE=0x13631093 SURFCPU_FLASH_CONFIRM=write-flash-arty_a7_100t tools/program.sh flash arty_a7_100t < /dev/null
check "flash: a bitstream whose build failed its checks refused" has "does not say the build passed"
run env FAKE_IDCODE=0x13631093 tools/program.sh volatile arty_a7_100t
check "program: ... also for a volatile load" has "does not say the build passed"

# ============================================================================== boards.txt
check "boards.txt: every board is a spec/soc.toml board" python3 -c "
import tomllib
b=tomllib.load(open('spec/soc.toml','rb'))['boards']
names=[l.split()[0] for l in open('boards/arty_a7/boards.txt') if l.strip() and not l.startswith('#')]
assert names and all(n in b for n in names), names
"

echo "test_tools: $pass passed, $failed failed"
[ "$failed" -eq 0 ]
