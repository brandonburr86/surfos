#!/bin/sh
# SurfCPU board track: tests of tools/fpga-build.sh, tools/program.sh and the Digilent fetch.sh.
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
sshopts="-o BatchMode=yes -o StrictHostKeyChecking=yes -o ConnectTimeout=20 -o ServerAliveInterval=30 -o ServerAliveCountMax=4"

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
check "dry run: prints the ssh commands, with the strict options" has "ssh $sshopts builder"
check "dry run: Vivado under nohup, its pid and exit status in out/" has "nohup sh -c 'echo \$\$ > out/vivado.pid; vivado -mode batch"
check "dry run: prints the Vivado command" has "vivado -mode batch"
p=$base/arty_a7_100t/arty_a7_baseline/stage/params.tcl
check "dry run: params.tcl has the 100T part" grep -q "set part {xc7a100tcsg324-1}" "$p"
check "dry run: params.tcl has the build identity generics" grep -q "BOARD {\"arty_a7_100t\"} BUILD_ID 32'h[0-9a-f]\{8\} BUILD_FLAGS 32'h0000000[01] BUILD_TIME 32'h$time_hex" "$p"
check "dry run: params.tcl has the scoped CDC constraints" grep -q "{surf_cdc_word boards/arty_a7/xdc/cdc/surf_cdc_word.xdc}" "$p"
check "dry run: leaves no real manifest" test ! -e "$base/arty_a7_100t/arty_a7_baseline/manifest.txt"
check "dry run: sources staged" test -f "$base/arty_a7_100t/arty_a7_baseline/stage/surfcpu/rtl/lib/surf_cdc_word.v"
check "dry run: include files staged" test -f "$base/arty_a7_100t/arty_a7_baseline/stage/surfcpu/rtl/generated/surfcpu_soc.vh"
check "dry run: build.tcl staged" test -f "$base/arty_a7_100t/arty_a7_baseline/stage/surfcpu/boards/arty_a7/vivado/build.tcl"
run env DRY_RUN=1 tools/fpga-build.sh --design arty_a7_baseline arty_a7_100t
check "dry run: DRY_RUN=1 (make build-fpga DRY_RUN=1) is a dry run too" test "$rc" -eq 0 -a ! -s "$FAKE_LOG"
run env DRY_RUN=yes tools/fpga-build.sh --design arty_a7_baseline arty_a7_100t
check "dry run: DRY_RUN other than 0 or 1 refused" has "DRY_RUN must be 0 or 1"

# ------------------------------------------------------------------------------ ssh flow, passing build
run tools/fpga-build.sh --design arty_a7_baseline arty_a7_35t
b=$base/arty_a7_35t/arty_a7_baseline
check "ssh: passing build exits 0" is "$rc" 0
check "ssh: made the remote directory" logged "ssh $sshopts builder mkdir -p surfcpu-build/arty_a7_baseline-arty_a7_35t-"
check "ssh: every ssh call checks the host key, times out, keeps the connection alive" \
    test "$(grep -c '^ssh ' "$FAKE_LOG")" -ge 4 -a "$(grep -c '^ssh ' "$FAKE_LOG")" -eq "$(grep -c "^ssh $sshopts builder " "$FAKE_LOG")"
check "ssh: Vivado ran under nohup" logged "nohup sh -c"
check "ssh: ran Vivado on the host" logged "vivado -mode batch -nojournal -log out/vivado.log -source surfcpu/boards/arty_a7/vivado/build.tcl -tclargs params.tcl"
check "ssh: the bitstream came back" test -f "$b/arty_a7_baseline.bit"
check "ssh: no checkpoint came back" test ! -e "$b/out/post_route.dcp"
check "ssh: reports came back" test -f "$b/out/reports/timing_summary.rpt"
check "ssh: ... with the run's own log and exit status" test -f "$b/out/vivado.nohup.txt" -a "$(cat "$b/out/vivado.exit")" = 0
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

# ------------------------------------------------------------------------------ ssh flow, lost connection and --fetch
b=$base/arty_a7_35t/arty_a7_baseline
run env FAKE_SSH_DROP=1 tools/fpga-build.sh --design arty_a7_baseline arty_a7_35t
check "drop: a connection lost during the run exits 3" is "$rc" 3
check "drop: ... and says how to fetch the results later" has "tools/fpga-build.sh --fetch --design arty_a7_baseline arty_a7_35t"
check "drop: ... no bitstream yet, and a manifest without a result" \
    test ! -e "$b/arty_a7_baseline.bit" -a -z "$(grep '^result' "$b/manifest.txt")"
run tools/fpga-build.sh --fetch --design arty_a7_baseline arty_a7_35t
check "fetch: brings back the finished run, which passed" is "$rc" 0
check "fetch: ... the bitstream" test -f "$b/arty_a7_baseline.bit"
check "fetch: ... without running Vivado again" test -z "$(grep '^vivado ' "$FAKE_LOG")"
check "fetch: ... and a manifest with the result and the bitstream's SHA-256" \
    grep -q "^bitstream_sha256 = \"$(shasum -a 256 "$b/arty_a7_baseline.bit" | cut -d' ' -f1)\"" "$b/manifest.txt"
run tools/fpga-build.sh --fetch --design arty_a7_baseline arty_a7_35t
check "fetch: a second fetch gives the same manifest" test "$rc" -eq 0 -a "$(grep -c '^result = ' "$b/manifest.txt")" -eq 2
rid=$(sed -n 's/^run_id = "\(.*\)"$/\1/p' "$b/manifest-head.txt")
ro=$t/remote/surfcpu-build/$rid/out
mv "$ro/vivado.exit" "$ro/vivado.exit.saved"
sleep 60 &
spid=$!
echo "$spid" > "$ro/vivado.pid"
run tools/fpga-build.sh --fetch --design arty_a7_baseline arty_a7_35t
check "fetch: Vivado still running exits 4" test "$rc" -eq 4 -a -n "$(grep 'still running' "$t/out.txt")"
kill "$spid" 2>/dev/null
wait "$spid" 2>/dev/null
run tools/fpga-build.sh --fetch --design arty_a7_baseline arty_a7_35t
check "fetch: no exit status and no Vivado running: start a new build" has "start a new build"
mv "$ro/vivado.exit.saved" "$ro/vivado.exit"
run env FAKE_SSH_DOWN=1 tools/fpga-build.sh --fetch --design arty_a7_baseline arty_a7_35t
check "fetch: host unreachable exits 3" test "$rc" -eq 3 -a -n "$(grep 'run --fetch again later' "$t/out.txt")"
run tools/fpga-build.sh --dry-run --design arty_a7_baseline arty_a7_35t
run tools/fpga-build.sh --fetch --design arty_a7_baseline arty_a7_35t
check "fetch: a dry run in between leaves the last run fetchable" is "$rc" 0
run tools/fpga-build.sh --fetch --design surfcpu_arty_a7_top arty_a7_35t
check "fetch: nothing started for that design" has "was started here"
printf 'FLOW=vivado-ssh\nHOST=other\nREMOTE_DIR=surfcpu-build\nVIVADO=vivado\n' > "$conf"
run tools/fpga-build.sh --fetch --design arty_a7_baseline arty_a7_35t
check "fetch: a run on another host is not fetched from this one" has "ran on 'builder'"
printf 'FLOW=vivado-ssh\nHOST=builder\nREMOTE_DIR=surfcpu-build\nVIVADO=vivado\n' > "$conf"

# ------------------------------------------------------------------------------ failing checks stop the bitstream
for case in "FAKE_WNS=-0.250:setup timing" "FAKE_WHS=-0.010:hold timing" "FAKE_DRC=2:DRC errors" \
            "FAKE_CRIT=3:critical warnings" "FAKE_CDC=1:critical CDC" "FAKE_NO_INPUT_DELAY=4:check_timing" \
            "FAKE_CT_FORMAT=renamed:no section 'checking no_input_delay'" \
            "FAKE_CT_FORMAT=wording:a line without its number" \
            "FAKE_CDC_FORMAT=noheader:cdc.rpt is not in the expected format" \
            "FAKE_CDC_FORMAT=caps:2 critical CDC findings"; do
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

# ------------------------------------------------------------------------------ build identity from git
# A scratch repository with the files the baseline build reads (this container cannot use the
# worktree's own git): clean, an unrelated untracked file, a file the build reads made untracked,
# committed again, a tracked file changed.
g=$t/gitrepo
mkdir -p "$g"
tar cf - --exclude=manuals --exclude=__pycache__ tools/fpga-build.sh boards/arty_a7 rtl/lib rtl/generated spec/soc.toml |
    ( cd "$g" && tar xf - )
gitq() { git -C "$g" -c user.name=test -c user.email=test@example.invalid "$@" > /dev/null 2>&1; }
gitq init -q && gitq add -A && gitq commit -q -m scratch
printf 'FLOW=vivado-ssh\nHOST=builder\nREMOTE_DIR=surfcpu-build\nVIVADO=vivado\n' > "$conf"
gm=$base/arty_a7_100t/arty_a7_baseline/manifest.dry-run.txt
gp=$base/arty_a7_100t/arty_a7_baseline/stage/params.tcl
run "$g/tools/fpga-build.sh" --dry-run --design arty_a7_baseline arty_a7_100t
check "identity: a clean scratch commit is clean" test "$rc" -eq 0 -a -n "$(grep '^source_dirty = false' "$gm")"
check "identity: ... BUILD_FLAGS 0 and the commit as BUILD_ID" \
    grep -q "BUILD_ID 32'h$(git -C "$g" rev-parse HEAD | cut -c1-8) BUILD_FLAGS 32'h00000000" "$gp"
printf 'notes\n' > "$g/notes.txt"
run "$g/tools/fpga-build.sh" --dry-run --design arty_a7_baseline arty_a7_100t
check "identity: an untracked file the build does not read leaves it clean" grep -q '^source_dirty = false' "$gm"
gitq rm -q --cached rtl/lib/surf_sync.v
run "$g/tools/fpga-build.sh" --dry-run --design arty_a7_baseline arty_a7_100t
check "identity: a source git does not track makes it dirty" grep -q '^source_dirty = true' "$gm"
check "identity: ... BUILD_FLAGS bit 0" grep -q "BUILD_FLAGS 32'h00000001" "$gp"
check "identity: ... and the file is named" has "not in git, so the build counts as dirty: rtl/lib/surf_sync.v"
gitq add rtl/lib/surf_sync.v && gitq commit -q -m again
run "$g/tools/fpga-build.sh" --dry-run --design arty_a7_baseline arty_a7_100t
check "identity: committed again, clean" grep -q '^source_dirty = false' "$gm"
printf '// changed\n' >> "$g/rtl/lib/surf_sync.v"
run "$g/tools/fpga-build.sh" --dry-run --design arty_a7_baseline arty_a7_100t
check "identity: a changed tracked file makes it dirty" grep -q '^source_dirty = true' "$gm"
rm -rf "$g"

# ============================================================================== program.sh
bit100=$base/arty_a7_100t/arty_a7_baseline/arty_a7_baseline.bit
bit35=$base/arty_a7_35t/arty_a7_baseline/arty_a7_baseline.bit
m100=$base/arty_a7_100t/arty_a7_baseline/manifest.txt
export DESIGN=arty_a7_baseline
run env PATH=/usr/bin:/bin "$root/tools/program.sh" volatile arty_a7_100t
check "program: no openFPGALoader -> pending, exit 2" is "$rc" 2
run env FAKE_IDCODE= tools/program.sh volatile arty_a7_100t
check "program: no cable/device refused" has "found no JTAG cable"

# the IDCODE as the real openFPGALoader prints it: printf("0x%x"), so 7 digits for revision 0
check "program: the fake prints IDCODEs like openFPGALoader (0x362d093)" \
    test "$(FAKE_IDCODE=0x0362d093 sh sim/board/tools/fake/openFPGALoader --detect | sed -n 's/^.idcode //p')" = 0x362d093
run env FAKE_IDCODE=0x0362d093 tools/program.sh detect arty_a7_35t
check "program: detect reads 'idcode 0x362d093' as the 35T" has "IDCODE 0x0362D093 = BOARD arty_a7_35t"
run env FAKE_IDCODE=0x03631093 tools/program.sh detect arty_a7_100t
check "program: detect reads 'idcode 0x3631093' as the 100T" has "IDCODE 0x03631093 = BOARD arty_a7_100t"
run env FAKE_IDCODE=0x13631093 tools/program.sh detect arty_a7_100t
check "program: detect reads the 100T with revision 1 (0x13631093)" has "IDCODE 0x13631093 = BOARD arty_a7_100t"
run env FAKE_IDCODE=0x0362d093 tools/program.sh volatile arty_a7_100t
check "program: 35T IDCODE with BOARD=arty_a7_100t refused" has "refusing"
check "program: ... and nothing was loaded" test "$(grep -c -- ' -b ' "$FAKE_LOG")" -eq 0
run env FAKE_IDCODE=0x13631093 FAKE_IDCODE2=0x13631093 tools/program.sh volatile arty_a7_100t
check "program: two devices on the chain refused" has "refusing to guess"
run env FAKE_IDCODE=0x1 FAKE_DETECT_TEXT="$(printf 'index 0:\n\tidcode 0x136310930')" tools/program.sh detect arty_a7_100t
check "program: an IDCODE longer than 8 digits is not read as one" has "cannot read the IDCODE"

# the cable: one FT2232H, or the one named by its serial number
run env FAKE_IDCODE=0x03631093 FAKE_CABLES="210319A28C7F 210319B0C8A6" tools/program.sh volatile arty_a7_100t
check "cable: two FT2232H cables refused" has "2 FT2232H cables"
check "cable: ... after only listing them (no detect, no load)" test "$(grep -vc -- '--scan-usb' "$FAKE_LOG")" -eq 0
run env FAKE_IDCODE=0x03631093 FAKE_CABLES="210319A28C7F 210319B0C8A6" CABLE_SERIAL=210319B0C8A6 tools/program.sh volatile arty_a7_100t
check "cable: CABLE_SERIAL chooses one, the load exits 0" is "$rc" 0
check "cable: ... detect on that cable" logged "openFPGALoader --ftdi-serial 210319B0C8A6 -c digilent --detect"
check "cable: ... and the load on that cable" logged "openFPGALoader --ftdi-serial 210319B0C8A6 -b arty_a7_100t $bit100"
run env FAKE_IDCODE=0x03631093 FAKE_CABLES="210319A28C7F 210319B0C8A6" tools/program.sh --ftdi-serial 210319A28C7F detect arty_a7_100t
check "cable: --ftdi-serial chooses one too" test "$rc" -eq 0 -a -n "$(grep -- '--ftdi-serial 210319A28C7F -c digilent --detect' "$FAKE_LOG")"
run env FAKE_IDCODE=0x03631093 CABLE_SERIAL=0123NOSUCH tools/program.sh detect arty_a7_100t
check "cable: a serial number no cable has finds no cable" has "found no JTAG cable"
run env FAKE_IDCODE=0x03631093 CABLE_SERIAL='21;rm' tools/program.sh detect arty_a7_100t
check "cable: a serial number with shell characters refused" test "$rc" -eq 1 -a ! -s "$FAKE_LOG"
run env FAKE_IDCODE=0x03631093 FAKE_SCAN_RC=1 tools/program.sh detect arty_a7_100t
check "cable: no USB list (--scan-usb failed) refused without a serial number" has "cannot be counted"
run env FAKE_IDCODE=0x03631093 FAKE_SCAN_RC=1 CABLE_SERIAL=210319A28C7F tools/program.sh detect arty_a7_100t
check "cable: ... and accepted with one" is "$rc" 0

# the bitstream's header: device and package (UG470 .bit fields a and b, as Vivado writes them)
mkbit() {     # FILE PART: a .bit header with fields a and b, then filler
    n=$(( ${#2} + 1 ))
    { printf '\000\011\017\360\017\360\017\360\017\360\000\000\001a\000\016other_project\000b\000'
      printf "\\$(printf '%03o' "$n")"
      printf '%s\000c\000\01310/01/2026\000 not a real bitstream\n' "$2"; } > "$1"
}
mkdir -p "$t/bits"
for c in "7a35tcpg236:arty_a7_35t:refuse" "7a100tfgg484:arty_a7_100t:refuse" "7a50tcsg324:arty_a7_35t:refuse" \
         "7a35ticsg324:arty_a7_35t:load" "xc7a35tcsg324-1:arty_a7_35t:load" "xc7a35tcsg324-1:arty_a7_100t:refuse" \
         "xc7a100tcsg324-1:arty_a7_100t:load" "XC7A100TCSG324-1:arty_a7_100t:load"; do
    bp=${c%%:*}; rest=${c#*:}; bb=${rest%%:*}; want=${rest#*:}
    mkbit "$t/bits/$bp.bit" "$bp"
    case "$bb" in arty_a7_35t) id=0x0362d093 ;; *) id=0x03631093 ;; esac
    run env FAKE_IDCODE=$id tools/program.sh volatile "$bb" "$t/bits/$bp.bit"
    if [ "$want" = load ]; then
        check "header: $bp on $bb loads" test "$rc" -eq 0 -a -n "$(grep -- " -b $bb " "$FAKE_LOG")"
    else
        check "header: $bp on $bb refused" test "$rc" -eq 1 -a -z "$(grep -- ' -b ' "$FAKE_LOG")"
    fi
done
printf 'no header at all\n' > "$t/bits/raw.bit"
run env FAKE_IDCODE=0x03631093 tools/program.sh volatile arty_a7_100t "$t/bits/raw.bit"
check "header: a file without a .bit header refused" has "no .bit header"
run env FAKE_IDCODE=0x13631093 tools/program.sh volatile arty_a7_100t "$bit35"
check "program: a 35T bitstream on the 100T refused" has "was built for 7a35t"
run env FAKE_IDCODE=0x13631093 tools/program.sh volatile arty_a7_100t "$t/nosuch.bit"
check "program: missing bitstream refused" has "no bitstream"

# loading, with the build manifest next to the bitstream
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
run env FAKE_IDCODE=0x13631093 SURFCPU_FLASH_CONFIRM=write-flash-arty_a7_100t tools/program.sh flash arty_a7_100t "$t/bits/xc7a100tcsg324-1.bit" < /dev/null
check "flash: a bitstream without a build manifest refused" has "flash needs the bitstream's build manifest"

# the manifest must describe this file, this board and its part
cp "$bit100" "$t/bit100.saved"
cp "$m100" "$t/m100.saved"
printf 'x' >> "$bit100"
run env FAKE_IDCODE=0x13631093 SURFCPU_FLASH_CONFIRM=write-flash-arty_a7_100t tools/program.sh flash arty_a7_100t < /dev/null
check "flash: a bitstream that is not the one the manifest describes refused" has "describes another bitstream"
run env FAKE_IDCODE=0x13631093 tools/program.sh volatile arty_a7_100t
check "program: ... also for a volatile load" has "describes another bitstream"
cp "$t/bit100.saved" "$bit100"
sed -i 's/^board = "arty_a7_100t"/board = "arty_a7_35t"/' "$m100"
run env FAKE_IDCODE=0x13631093 SURFCPU_FLASH_CONFIRM=write-flash-arty_a7_100t tools/program.sh flash arty_a7_100t < /dev/null
check "flash: a manifest for another board refused" has "is for board 'arty_a7_35t'"
cp "$t/m100.saved" "$m100"
sed -i 's/^part = "xc7a100tcsg324-1"/part = "xc7a100tcsg324-2"/' "$m100"
run env FAKE_IDCODE=0x13631093 SURFCPU_FLASH_CONFIRM=write-flash-arty_a7_100t tools/program.sh flash arty_a7_100t < /dev/null
check "flash: a manifest for another part refused" has "is for part 'xc7a100tcsg324-2'"
cp "$t/m100.saved" "$m100"
sed -i 's/^result = "pass"/result = "fail"/' "$m100"
run env FAKE_IDCODE=0x13631093 SURFCPU_FLASH_CONFIRM=write-flash-arty_a7_100t tools/program.sh flash arty_a7_100t < /dev/null
check "flash: a bitstream whose build failed its checks refused" has "does not say the build passed"
run env FAKE_IDCODE=0x13631093 tools/program.sh volatile arty_a7_100t
check "program: ... also for a volatile load" has "does not say the build passed"
cp "$t/m100.saved" "$m100"

# tools/openxc7-build.sh's own manifest (a flow track build, docs/OPEN-FLOW.md section 8)
ox=$t/openxc7/flow_demo-arty_a7_35t
mkdir -p "$ox"
mkbit "$ox/flow_demo.bit" xc7a35tcsg324-1
oxmanifest() {   # BLOCK_RAM_RESULT: an openxc7-build.sh manifest for $ox/flow_demo.bit
    { echo "# openXC7 build manifest (tools/openxc7-build.sh); physical behaviour: pending (never on a board)"
      echo "top                 flow_demo"
      echo "part                xc7a35tcsg324-1 (device xc7a35t, chip database chipdb-xc7a50t.bin)"
      echo "bitstream           $ox/flow_demo.bit sha256:$(shasum -a 256 "$ox/flow_demo.bit" | cut -d' ' -f1) 99 bytes"
      echo "check synthesis     PASS (netlist against the RTL: flip-flops, memories, outputs: check-synthesis.txt)"
      echo "check bitstream     PASS (IDCODE, frames, commands, CRC: check-bitstream.txt)"
      echo "check round trip    PASS (bitstream decoded back to FASM: check-roundtrip.txt)"
      echo "check block RAM     $1 (contents decoded from the bitstream: check-bram.txt)"
      echo "check LVS           PASS (routed netlist equivalent to the synthesis: check-lvs.txt)"
      echo "check clocks        none in the design (MMCM/PLL dividers against XAPP888: check-clocks.txt)"; } > "$ox/manifest.txt"
}
oxmanifest "none in the design"
run env FAKE_IDCODE=0x0362d093 tools/program.sh volatile arty_a7_35t "$ox/flow_demo.bit"
check "openxc7 manifest: every check passed, the volatile load exits 0" test "$rc" -eq 0 -a -n "$(grep -- ' -b arty_a7_35t ' "$FAKE_LOG")"
run env FAKE_IDCODE=0x0362d093 SURFCPU_FLASH_CONFIRM=write-flash-arty_a7_35t tools/program.sh flash arty_a7_35t "$ox/flow_demo.bit" < /dev/null
check "openxc7 manifest: ... but no flash without tools/fpga-build.sh's manifest" has "flash needs the bitstream's build manifest from tools/fpga-build.sh"
oxmanifest "FAIL"
run env FAKE_IDCODE=0x0362d093 tools/program.sh volatile arty_a7_35t "$ox/flow_demo.bit"
check "openxc7 manifest: a failed check refused" has "does not say the build passed its checks (check block RAM FAIL"
oxmanifest "PASS"
printf 'x' >> "$ox/flow_demo.bit"
run env FAKE_IDCODE=0x0362d093 tools/program.sh volatile arty_a7_35t "$ox/flow_demo.bit"
check "openxc7 manifest: another bitstream refused" has "describes another bitstream"

# ============================================================================== digilent/fetch.sh
# A copy of the digilent directory whose SHA256SUMS lists three made-up manuals: one saved
# copy matches, one differs, one is not there. Nothing is downloaded.
dd=$t/digilent
mkdir -p "$dd/xdc" "$t/saved"
cp boards/arty_a7/digilent/fetch.sh "$dd/"
cp boards/arty_a7/digilent/xdc/* "$dd/xdc/"
printf 'a schematic\n' > "$t/saved/arty_a7_sch.pdf"
printf 'a manual, damaged\n' > "$t/saved/pmodps2_rm.pdf"
{
    grep -v ' manuals/' boards/arty_a7/digilent/SHA256SUMS
    echo "$(shasum -a 256 "$t/saved/arty_a7_sch.pdf" | cut -d' ' -f1)  manuals/arty_a7_sch.pdf"
    echo "$(printf 'a manual\n' | shasum -a 256 | cut -d' ' -f1)  manuals/pmodps2_rm.pdf"
    echo "$(printf 'a reference\n' | shasum -a 256 | cut -d' ' -f1)  manuals/pmodvga_rm.pdf"
} > "$dd/SHA256SUMS"
run sh "$dd/fetch.sh" verify
check "digilent verify: no manuals fetched still passes (a fresh checkout)" is "$rc" 0
check "digilent verify: ... and lists the manuals not fetched" has "not fetched (boards/arty_a7/digilent/fetch.sh fetch): manuals/arty_a7_sch.pdf manuals/pmodps2_rm.pdf manuals/pmodvga_rm.pdf"
run sh "$dd/fetch.sh" fetch --from "$t/saved"
check "digilent fetch: a copy that differs is reported, the run fails" test "$rc" -eq 1
check "digilent fetch: the matching copy is installed" test -f "$dd/manuals/arty_a7_sch.pdf"
check "digilent fetch: the differing copy is not installed" test ! -e "$dd/manuals/pmodps2_rm.pdf"
check "digilent fetch: the missing one is named" has "absent  manuals/pmodvga_rm.pdf"
run sh "$dd/fetch.sh" verify
check "digilent verify: counts the installed manual" has "manuals: 1 of 3 present and matching"
printf 'changed\n' >> "$dd/manuals/arty_a7_sch.pdf"
run sh "$dd/fetch.sh" verify
check "digilent verify: a present manual that changed fails" test "$rc" -eq 1
rm "$dd/manuals/arty_a7_sch.pdf"
printf '# edited\n' >> "$dd/xdc/Arty-A7-35-Master.xdc"
run sh "$dd/fetch.sh" verify
check "digilent verify: a changed XDC file fails" test "$rc" -eq 1

# ============================================================================== boards.txt
check "boards.txt: every board is a spec/soc.toml board" python3 -c "
import tomllib
b=tomllib.load(open('spec/soc.toml','rb'))['boards']
names=[l.split()[0] for l in open('boards/arty_a7/boards.txt') if l.strip() and not l.startswith('#')]
assert names and all(n in b for n in names), names
"

echo "test_tools: $pass passed, $failed failed"
[ "$failed" -eq 0 ]
