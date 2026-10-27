#!/bin/sh
# SurfCPU open flow: tests of tools/openxc7-build.sh --recheck run for real, not as a dry run, with a
# stand-in for every program it runs (part of `make test-flow`). They need no image and no Docker (a
# stand-in docker refuses to run), so they also run on the Mac, under /bin/sh and dash with its BSD
# awk: TEST_SH names the shell that runs the scripts under test (default sh; dash for dash).
# File: openxc7-recheck-test.sh Date: 10/27/26 (SurfCPU, the flow track: the review of c10e0fa)
#
#   0. its dry run, and each check of the build before anything runs, in dry runs (from
#      tools/openxc7-flow-test.sh, made to run under BSD tools too)
#   1. a recheck stopped at each of its steps (SIGKILL while that step runs, or while it keeps the
#      build's manifest and logs, or renames its own manifest into place): the manifest beside the
#      bitstream is the build's, byte for byte, so tools/program.sh refuses the bitstream; and a
#      recheck run afterwards passes, the build's own logs kept in build-evidence/
#   2. each refusal before the recheck begins (exit 3) leaves the directory as it was, byte for
#      byte, and runs nothing: the files the record names by SHA-256, format 2's lines (each
#      required), a format 1 build without the LVS inputs that tie its netlist and placement, a kept
#      check that failed, a file missing, a format it does not know, a recheck's manifest alone,
#      the image's tools not the build's, a later build over an earlier recheck's files
#   3. a format 1 build whose netlist, FASM or placement is not the one it proved: the LVS inputs
#      made again are not its bytes, refused (exit 4) before the proof, its manifest as it was
#   4. a recheck of a recheck: the build's manifest and logs stay the build's
#   5. lvs_verdict (tools/openxc7-steps.sh) on every way lvs_equiv can end, with the review's: exit 0
#      with an unknown, with a DIFFER line, with no point proved
#   6. devenv/openxc7/z3cmd.py --summary on a line holding Z3's error text, and on one cut short
set -eu
here=$(cd "$(dirname "$0")/.." && pwd -P)
cd "$here"
SH=${TEST_SH:-sh}
t=build/flow/recheck-test
b=$t/b
rm -rf "$t"
mkdir -p "$t/bin"
fails=0 checks=0
ok()  { checks=$((checks + 1)); echo "  ok    $1"; }
bad() { checks=$((checks + 1)); fails=$((fails + 1)); echo "  FAIL  $1"; }
sha() { if command -v sha256sum > /dev/null 2>&1; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1; }
awk_is=$(awk --version 2> /dev/null | head -n 1) || true
[ -n "$awk_is" ] || awk_is=$(awk -W version 2> /dev/null | head -n 1) || true
echo "openxc7-recheck-test: the scripts under test run by $SH ($(command -v "$SH")), awk: ${awk_is:-unknown}"

# ---------------------------------------------------------------- the stand-ins
# Each logs its call to $CALLS. STOP_AT names a point where one waits to be killed (its PID in
# STOP_MARK): the step it stands in for, or keep-evidence, evidence-rename, keep-manifest and
# manifest-rename, the recheck's own copies and renames (cp and mv stand in for themselves).
cat > "$t/lib.sh" <<'EOF'
called() { echo "$*" >> "$CALLS"; }
stop_here() { if [ "${STOP_AT:-}" = "$1" ]; then echo $$ > "$STOP_MARK"; exec sleep 600; fi; }
arg() { _f=$1; shift; while [ $# -gt 1 ]; do if [ "$1" = "$_f" ]; then printf '%s\n' "$2"; return 0; fi; shift; done; return 1; }
hash() { if command -v sha256sum > /dev/null 2>&1; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1; }
EOF
standin() {   # NAME: a stand-in from its body on stdin
  { echo '#!/bin/sh'; cat "$t/lib.sh"; cat; } > "$t/bin/$1"
  chmod +x "$t/bin/$1"
}
standin python3 <<'EOF'
[ "${1:-}" != -I ] || shift
script=${1##*/}; shift
called "python3 $script $*"
case "$script" in
  report.py)
    stop_here report
    printf 'Timing (stand-in)\n\nclock sys_clk: constraint 25.00 MHz, achieved 43.44 MHz, slack +16.982 ns  PASS\n' > "$(arg --timing "$@")"
    printf "Against the xc7a100t's documented capacity (stand-in):\n  flip-flops   2 / 126800   0.0%%\n" > "$(arg --utilization "$@")" ;;
  slackcheck.py)
    stop_here slack
    echo "slackcheck: stand-in" > "$(arg --out "$@")"
    echo "pass sys_clk +16.982 0.500 +16.482" ;;
  cmtcheck.py)  stop_here check-clk; echo "cmtcheck: no MMCME2/PLLE2 in the design" ;;
  gclkcheck.py) stop_here check-gclk; echo "gclkcheck: no ordinary net on the clock network (stand-in)" ;;
  bitinfo.py)   stop_here check-bit; echo "check for xc7a100t: PASS (stand-in)" ;;
  fasmcheck.py) stop_here roundtrip; echo "the routed FASM and the decoded bitstream agree (stand-in)" ;;
  bramwe.py)
    stop_here lvs-bramwe
    f=$(arg --fabric "$@"); r=$(arg --report "$@")
    cp "$f" "${f%/*}/fabric.raw.v"
    echo "// bramwe: no RAMB18E1 enable to read again (stand-in)" >> "$f"
    echo "bramwe: 0 block RAM(s), 0 enable(s) re-read, 0 difference(s)" > "$r"
    cat "$r" ;;
  z3cmd.py)
    case "${1:-}" in
      --self-test) stop_here lvs-selftest; echo "z3cmd self-test: PASS (stand-in)" ;;
      --summary)   stop_here lvs-summary; echo "z3cmd: the stand-in's summary of $2" ;;
      *)           echo unsat ;;
    esac ;;
  *) echo "stand-in python3: nothing stands in for $script" >&2; exit 99 ;;
esac
EOF
standin yosys <<'EOF'
called "yosys $*"
stop_here lvs-gold
p=$(arg -p "$@")
json=$(printf '%s\n' "$p" | sed -n 's/^read_json \([^;]*\);.*/\1/p')
printf '// gold.v made from the netlist %s\nmodule t; endmodule\n' "$(hash "$json")" > "${p##* }"
EOF
standin tileverilog <<'EOF'
called "tileverilog $*"
stop_here lvs-extract
printf '// fabric.v made from the FASM %s and the placement %s\nmodule fabric; endmodule\n' \
  "$(hash "$(arg --fasm "$@")")" "$(hash "$(arg --placement "$@")")" > "$(arg --out "$@")"
echo "// the tile model (stand-in)" > "$(arg --model-out "$@")"
EOF
standin bit2fasm <<'EOF'
called "bit2fasm $*"
stop_here decode
for a in "$@"; do last=$a; done
echo "# decoded from the bitstream $(hash "$last") (stand-in)"
EOF
standin bitread <<'EOF'
called "bitread $*"
EOF
standin fasm2netlist <<'EOF'
called "fasm2netlist $*"
EOF
standin timeout <<'EOF'
called "timeout $*"
shift
exec "$@"
EOF
standin lvs_equiv <<'EOF'
called "lvs_equiv $*"
stop_here lvs-prove
echo "registers: 2 gold, 2 gate, 2 matched by name"
printf '  proved  r1\n  proved  r2\n\n2 proved, 0 differ, 0 unknown   (0.1s)\n'
EOF
standin cp <<'EOF'
for a in "$@"; do last=$a; done
case "$last" in */build-evidence.tmp/*) case " $* " in *"/check-lvs.txt "*) stop_here keep-evidence ;; esac ;; esac
exec /bin/cp "$@"
EOF
standin mv <<'EOF'
for a in "$@"; do last=$a; done
case "$last" in
  */build-evidence)     stop_here evidence-rename ;;
  */manifest.build.txt) stop_here keep-manifest ;;
  */manifest.txt)       stop_here manifest-rename ;;
esac
exec /bin/mv "$@"
EOF
standin docker <<'EOF'
echo "openxc7-recheck-test: docker must not run here ($*)" >&2
exit 99
EOF
CALLS=$here/$t/calls.txt
export CALLS

# ---------------------------------------------------------------- the build
# A routed build failed at LVS alone, as tools/openxc7-build.sh leaves one: its bitstream (a .bit
# header naming the A7-100T's part, which tools/program.sh reads), the files the checks read, the
# logs of its checks, and its LVS inputs, made by the stand-ins as the recheck makes them (gold.v
# from the netlist, fabric.raw.v and fabric.v from the FASM and the placement).
part=xc7a100tcsg324-1
tools_now=$(cat /etc/surfcpu-openxc7.txt 2> /dev/null || true)    # what the recheck compares the record's with
l() { printf '%-20s%s\n' "$1" "$2"; }
mk_build() {     # FORMAT [TOOLS]: the build in $b/out, its XDC in $b, copies of its own in $t/build-*
  rm -rf "$b"
  mkdir -p "$b/out/lvs"
  n=$(( ${#part} + 1 ))
  { printf '\000\011\017\360\017\360\017\360\017\360\000\000\001a\000\005fake\000b\000'
    printf "\\$(printf '%03o' "$n")"
    printf '%s\000c\000\01310/26/2026\000d\000\01107:44:39\000e\000\000\000\020 made-up stream\n' "$part"; } > "$b/out/t.bit"
  echo '{"modules": {"t": {"cells": {}, "netnames": {}}}}' > "$b/out/t.json"
  echo "made-up routed FASM" > "$b/out/t.fasm"
  echo '{"made-up": "placement"}' > "$b/out/placement.json"
  for mk_f in report.json nextpnr.log stat.json seeds.txt check-srl.txt check-synthesis.txt; do echo "made-up $mk_f" > "$b/out/$mk_f"; done
  for mk_f in times.txt report.out timing.txt utilization.txt slack-seed1.txt check-bitstream.txt check-roundtrip.txt \
           check-bram.txt check-clocks.txt check-gclk.txt bit2fasm.log; do
    echo "the build's $mk_f" > "$b/out/$mk_f"
  done
  printf '  proved  r1\nthe build%s proof, killed at the memory cap\n' "'s" > "$b/out/check-lvs.txt"
  echo "set_property made-up" > "$b/x.xdc"
  ( PATH=$here/$t/bin:$PATH CALLS=$here/$t/mk-calls.txt
    yosys -q -p "read_json $b/out/t.json; hierarchy -top t; splitnets; select t; write_verilog -noattr -norename -selected $b/out/lvs/gold.v"
    tileverilog --fasm "$b/out/t.fasm" --placement "$b/out/placement.json" --out "$b/out/lvs/fabric.v" --model-out "$b/out/lvs/tile_model.v"
    python3 -I devenv/openxc7/bramwe.py --fabric "$b/out/lvs/fabric.v" --fasm "$b/out/t.fasm" --report "$b/out/lvs/bramwe.txt" > /dev/null )
  echo "the build's gold log" > "$b/out/lvs/gold.log"
  { echo "# openXC7 build manifest (tools/openxc7-build.sh); physical behaviour: pending (never on a board)"
    [ "$1" = 1 ] || l "manifest format" "$1 (docs/OPEN-FLOW.md section 3)"
    l top t
    l part "$part (device xc7a100t, chip database chipdb-xc7a100t.bin)"
    l board arty_a7_100t
    l "date (UTC)" 2026-10-26T07:44:39Z
    l seed "1 (of 1: 1 tried, seeds.txt)"
    l "slack rule" "each clock at least max(0.5 ns, 4 % of its period)"
    l xdc "$b/x.xdc sha256:$(sha "$b/x.xdc")"
    l bitstream "$b/out/t.bit sha256:$(sha "$b/out/t.bit") $(wc -c < "$b/out/t.bit" | tr -d ' ') bytes"
    if [ "$1" != 1 ]; then
      l netlist "$b/out/t.json sha256:$(sha "$b/out/t.json")"
      l "routed FASM" "$b/out/t.fasm sha256:$(sha "$b/out/t.fasm")"
      l placement "$b/out/placement.json sha256:$(sha "$b/out/placement.json")"
    fi
    l "check SRL cells" "PASS (none in the synthesized netlist, which LVS could not check: check-srl.txt)"
    l "check synthesis" "PASS (netlist against the RTL: flip-flops, memories, outputs: check-synthesis.txt)"
    l "check bitstream" "PASS (IDCODE, frames, commands, CRC: check-bitstream.txt)"
    l "check round trip" "PASS (bitstream decoded back to FASM: check-roundtrip.txt)"
    l "check block RAM" "none in the design (contents decoded from the bitstream: check-bram.txt)"
    l "check LVS" "BLOCKED (killed: SIGKILL, exit 137) (routed netlist equivalent to the synthesis: check-lvs.txt)"
    [ "$1" = 1 ] || l "lvs solver" "libz3 (lvs_equiv's linked libz3, one incremental session)"
    l "lvs timeout" "14400 s"
    l "check clocks" "none in the design (MMCM/PLL dividers against XAPP888: check-clocks.txt)"
    l "check slack margin" "PASS (least margin sys_clk +16.982 ns, wants 0.500: slack-seed1.txt)"
    echo "## placement attempts"; echo "seed 1: pass, routed"
    echo "## timing"; echo "clock sys_clk: constraint 25.00 MHz, achieved 43.44 MHz, slack +16.982 ns  PASS"
    echo "## step times"; echo "lvs-prove     3058.00 s  FAILED (137)"
    echo "## tools (/etc/surfcpu-openxc7.txt)"; if [ -n "${2:-$tools_now}" ]; then printf '%s\n' "${2:-$tools_now}"; fi
  } > "$b/out/manifest.txt"
  cp "$b/out/manifest.txt" "$t/build-manifest.txt"
  cp "$b/out/check-lvs.txt" "$t/build-check-lvs.txt"
  for mk_f in gold.v fabric.raw.v fabric.v; do printf '%s  %s\n' "$(sha "$b/out/lvs/$mk_f")" "$mk_f"; done > "$t/build-lvs.sha256"
}
rc_run() {       # [VAR=VALUE...]: the recheck of $b/out, for real, by the z3cmd route; output in $t/run.txt
  : > "$CALLS"
  if env "$@" PATH="$here/$t/bin:$PATH" SURFCPU_OPENXC7=1 "$SH" tools/openxc7-build.sh --recheck --out "$b/out" \
       --lvs-solver z3cmd > "$t/run.txt" 2>&1
  then rc=0; else rc=$?; fi
}
prog() {         # tools/program.sh volatile on the flow's bitstream, with the board's stand-ins
  : > "$t/ofl.log"
  if env OPENFPGALOADER="$here/sim/board/tools/fake/openFPGALoader" FAKE_LOG="$here/$t/ofl.log" FAKE_IDCODE=0x13631093 \
       "$SH" tools/program.sh volatile arty_a7_100t "$b/out/t.bit" > "$t/prog.txt" 2>&1
  then prc=0; else prc=$?; fi
}
program_refuses() { prog; [ "$prc" = 1 ] && ! grep -q -- ' -b ' "$t/ofl.log"; }
program_loads()   { prog; [ "$prc" = 0 ] && grep -q -- " -b arty_a7_100t $b/out/t.bit\$" "$t/ofl.log"; }
evidence_kept() {   # build-evidence/: the build's own logs and its LVS inputs by SHA-256, no copy of them
  cmp -s "$b/out/build-evidence/check-lvs.txt" "$t/build-check-lvs.txt" &&
  [ "$(cat "$b/out/build-evidence/times.txt")" = "the build's times.txt" ] &&
  [ "$(cat "$b/out/build-evidence/lvs/gold.log")" = "the build's gold log" ] &&
  cmp -s "$b/out/build-evidence/lvs-inputs.sha256" "$t/build-lvs.sha256" &&
  [ ! -e "$b/out/build-evidence/lvs/gold.v" ] && [ ! -e "$b/out/build-evidence/lvs/fabric.v" ] &&
  [ ! -e "$b/out/build-evidence/build-evidence.tmp" ]
}
passed() {       # the last recheck passed: its manifest beside the bitstream, the build's kept beside it
  [ "$rc" = 0 ] && grep -q '^check LVS           PASS (run again: ' "$b/out/manifest.txt" &&
  grep -q '^recheck  ' "$b/out/manifest.txt" && cmp -s "$b/out/manifest.build.txt" "$t/build-manifest.txt" &&
  [ ! -e "$b/out/manifest.txt.new" ] && [ ! -e "$b/out/manifest.build.txt.tmp" ] && [ ! -e "$b/out/build-evidence.tmp" ]
}
edit() { sed "$2" "$1" > "$1.edit"; /bin/mv "$1.edit" "$1"; }    # FILE SCRIPT (no sed -i: BSD and GNU differ)
add_after() { awk -v re="$2" -v line="$3" '{ print } $0 ~ re { print line }' "$1" > "$1.edit"; /bin/mv "$1.edit" "$1"; }
expect_in() {    # WHAT TEXT PATTERN...: every PATTERN (fixed strings) is in TEXT
  what=$1 text=$2; shift 2
  for p in "$@"; do
    if ! printf '%s\n' "$text" | grep -qF -- "$p"; then bad "$what: no '$p'"; return; fi
  done
  ok "$what"
}
snap() { ( cd "$b" && find . | LC_ALL=C sort | while IFS= read -r f; do
             if [ -f "$f" ]; then echo "$f $(sha "$f")"; else echo "$f"; fi; done ) > "$1"; }

# ---------------------------------------------------------------- 0. the dry run, and the build checked before it
# A routed build made up (a dry run reads only the files the checks read, and their SHA-256), with
# its manifest as tools/openxc7-build.sh writes one; the lines of its sections (the tools' "fasm",
# an included file's "placement") must not be taken for the build's own. (From
# tools/openxc7-flow-test.sh, 10/27/26, made to run under BSD tools too.)
r=$t/rc
dsnap() { ( cd "$r" && find . -type f | LC_ALL=C sort | while IFS= read -r f; do echo "$f $(sha "$f")"; done ) > "$r.sums"; }
unchanged() { ( cd "$r" && find . -type f | LC_ALL=C sort | while IFS= read -r f; do echo "$f $(sha "$f")"; done ) | cmp -s - "$r.sums"; }
mk_dry() {       # FORMAT LVS: a build in $r/out, its XDC in $r
  rm -rf "$r"
  mkdir -p "$r/out/lvs"
  for m in gold.v fabric.raw.v fabric.v; do echo "made-up $m" > "$r/out/lvs/$m"; done
  for m in t.bit t.fasm t.json placement.json report.json nextpnr.log stat.json seeds.txt check-srl.txt check-synthesis.txt; do
    echo "made-up $m" > "$r/out/$m"
  done
  echo "set_property made-up" > "$r/x.xdc"
  s() { sha "$1"; }
  { echo "# openXC7 build manifest (tools/openxc7-build.sh); physical behaviour: pending (never on a board)"
    [ "$1" = 1 ] || echo "manifest format     $1 (docs/OPEN-FLOW.md section 3)"
    echo "top                 t"
    echo "part                xc7a100tcsg324-1 (device xc7a100t, chip database chipdb-xc7a100t.bin)"
    echo "board               arty_a7_100t"
    echo "date (UTC)          2026-10-26T07:44:39Z"
    echo "seed                1 (of 1 2: 1 tried, seeds.txt)"
    echo "slack rule          each clock at least max(0.5 ns, 4 % of its period); the design list sets: dram_clk=0.5"
    echo "defines             ARTY_BUILD_ID=32'h12345678"
    echo "xdc                 $r/x.xdc sha256:$(s "$r/x.xdc")"
    echo "bitstream           $r/out/t.bit sha256:$(s "$r/out/t.bit") 12 bytes"
    if [ "$1" != 1 ]; then
      echo "netlist             $r/out/t.json sha256:$(s "$r/out/t.json")"
      echo "routed FASM         $r/out/t.fasm sha256:$(s "$r/out/t.fasm")"
      echo "placement           $r/out/placement.json sha256:$(s "$r/out/placement.json")"
    fi
    echo "check SRL cells     PASS (none in the synthesized netlist, which LVS could not check: check-srl.txt)"
    echo "check synthesis     PASS (netlist against the RTL: flip-flops, memories, outputs: check-synthesis.txt)"
    echo "check bitstream     PASS (IDCODE, frames, commands, CRC: check-bitstream.txt)"
    echo "check LVS           $2 (routed netlist equivalent to the synthesis: check-lvs.txt)"
    echo "lvs timeout         14400 s"
    echo "check slack margin  PASS (least margin dram_clk +1.037 ns, wants 0.500: slack-seed1.txt)"
    echo "## placement attempts"; echo "seed 1: pass, routed"
    echo "## tools (/etc/surfcpu-openxc7.txt)"; echo "fasm                   0.0.2.post66"
    echo "## included: $r/x.txt"; echo "placement           $r/elsewhere sha256:00 (a line of a file the build included)"
  } > "$r/out/manifest.txt"
  dsnap
}
rc_try() { if "$SH" tools/openxc7-build.sh --recheck --dry-run --out "$r/out" "$@" > "$r.txt" 2>&1; then rc=0; else rc=$?; fi; }
refused_dry() {     # WHAT PATTERN: the last recheck refused, saying PATTERN, and changed nothing
  if [ "$rc" = 3 ] && grep -q -- "$2" "$r.txt" && unchanged; then ok "recheck: refuses $1, changing nothing"
  else bad "recheck: $1 (exit $rc)"; cat "$r.txt"; fi
}
mk_dry 1 "FAIL"
rc_try --lvs-solver z3cmd --lvs-timeout 86400
if [ "$rc" = 0 ] && unchanged; then
  expect_in "recheck: a build failed at LVS alone: its dry run checks it, would run every post-route check again, LVS by z3cmd within 86400 s" "$(cat "$r.txt")" \
    "kept: SRL cells PASS, synthesis PASS" "[report] python3 -I devenv/openxc7/report.py --device xc7a100t --report $r/out/report.json" \
    "[slack] python3 -I devenv/openxc7/slackcheck.py --report $r/out/report.json --min dram_clk=0.5 --out $r/out/slack-seed1.txt" \
    "[check-clk] python3 devenv/openxc7/cmtcheck.py --json $r/out/t.json" "[check-gclk]" "[check-bit]" "[decode]" "[roundtrip]" \
    "[bram-extract]" "[check-bram]" "[lvs-gold]" "[lvs-extract] tileverilog --fasm $r/out/t.fasm" "--xdc $r/x.xdc --part xc7a100tcsg324-1" \
    "[lvs-bramwe]" "[lvs-selftest]" "timeout 86400 lvs_equiv" "--solver python3 -I devenv/openxc7/z3cmd.py --format smt2"
else bad "recheck: the dry run of a build failed at LVS (exit $rc)"; cat "$r.txt"; fi
rc_try
if [ "$rc" = 0 ] && grep -q 'timeout 14400 lvs_equiv' "$r.txt" && ! grep -q -- '--solver' "$r.txt"
then ok "recheck: without --lvs-solver and --lvs-timeout, LVS as the build ran it (libz3, its 14400 s)"
else bad "recheck: the build's own LVS route and limit (exit $rc)"; cat "$r.txt"; fi
mk_dry 2 "BLOCKED (killed: SIGKILL, exit 137)"
rc_try --lvs-solver z3cmd
if [ "$rc" = 0 ] && grep -q 'the bitstream, the XDC, the netlist, the FASM, the placement as recorded' "$r.txt" && unchanged
then ok "recheck: a format 2 build: its netlist, FASM and placement checked against their SHA-256 too"
else bad "recheck: a format 2 build (exit $rc)"; cat "$r.txt"; fi
mk_dry 1 FAIL; printf 'x' >> "$r/out/t.bit"; dsnap; rc_try --lvs-solver z3cmd
refused_dry "a bitstream that is not the one its manifest records" "t.bit (sha256 [0-9a-f]*) is not the one"
mk_dry 1 FAIL; printf 'x' >> "$r/x.xdc"; dsnap; rc_try
refused_dry "constraints that are not the ones its manifest records" "x.xdc (sha256 [0-9a-f]*) is not the one"
mk_dry 2 FAIL; printf 'x' >> "$r/out/t.json"; dsnap; rc_try
refused_dry "a format 2 build whose netlist is not the one recorded" "t.json (sha256 [0-9a-f]*) is not the one"
mk_dry 2 FAIL; printf 'x' >> "$r/out/placement.json"; dsnap; rc_try
refused_dry "a format 2 build whose placement is not the one recorded" "placement.json (sha256 [0-9a-f]*) is not the one"
mk_dry 1 FAIL; edit "$r/out/manifest.txt" 's/^check synthesis     PASS/check synthesis     FAIL/'; dsnap; rc_try
refused_dry "a build whose kept synthesis check failed" "check synthesis is 'FAIL': a recheck keeps it, so it must have passed"
mk_dry 1 FAIL; edit "$r/out/manifest.txt" 's/^check SRL cells     PASS/check SRL cells     skipped/'; dsnap; rc_try
refused_dry "a build whose kept SRL check was skipped" "check SRL cells is 'skipped'"
for gone in placement.json t.fasm report.json check-synthesis.txt; do
  mk_dry 1 FAIL; rm "$r/out/$gone"; dsnap; rc_try
  refused_dry "a build without its $gone" "out/$gone is missing"
done
mk_dry 1 FAIL; rm "$r/x.xdc"; dsnap; rc_try
refused_dry "a build whose constraints are gone" "constraints $r/x.xdc are missing"
mk_dry 1 FAIL; edit "$r/out/manifest.txt" "s|^bitstream           $r/out/t.bit|bitstream           build/elsewhere/t.bit|"; dsnap; rc_try
refused_dry "a build whose manifest names a bitstream elsewhere (moved)" "names build/elsewhere/t.bit, not $r/out/t.bit"
mk_dry 3 FAIL; rc_try
refused_dry "a manifest format it does not know" "manifest format 3, which this script does not know"
mk_dry 1 FAIL; add_after "$r/out/manifest.txt" '^check slack margin  ' 'recheck             2026-10-27T00:00:00Z, made up'; dsnap; rc_try
refused_dry "a recheck's manifest with the build's own not beside it" "is a recheck's manifest, and the build's own"
/bin/mv "$r/out/manifest.txt" "$r/out/recheck.txt"; dsnap; rc_try
refused_dry "a directory without a manifest" "no manifest in"
mk_dry 1 FAIL; cp "$r/out/manifest.txt" "$r/out/manifest.build.txt"; mkdir -p "$r/out/build-evidence"
for m in gold.v fabric.raw.v fabric.v; do printf '%s  %s\n' "$(sha "$r/out/lvs/$m")" "$m"; done > "$r/out/build-evidence/lvs-inputs.sha256"
add_after "$r/out/manifest.txt" '^check slack margin  ' 'recheck             2026-10-27T00:00:00Z, made up'; dsnap; rc_try --lvs-solver z3cmd
if [ "$rc" = 0 ] && grep -q "its record $r/out/manifest.build.txt" "$r.txt" && unchanged
then ok "recheck: once a recheck has kept the build's manifest, the next reads that one"
else bad "recheck: the build's manifest put aside (exit $rc)"; cat "$r.txt"; fi
if "$SH" tools/openxc7-build.sh --recheck --out "$r/out" --top t > "$r.txt" 2>&1; then rc=0; else rc=$?; fi
if [ "$rc" = 2 ] && grep -q "takes the build's own settings from its manifest" "$r.txt"
then ok "recheck: takes no build options (--top ...): the build's come from its manifest"
else bad "recheck: accepted a build option (exit $rc)"; cat "$r.txt"; fi

# ---------------------------------------------------------------- 1. stopped at every step
mk_build 1
rc_run
if passed && evidence_kept && grep -q "^lvs inputs          made again from the netlist, the FASM and the placement; gold.v the same bytes as the build's; fabric.raw.v the same bytes as the build's; fabric.v the same bytes as the build's\$" "$b/out/manifest.txt" &&
   grep -q "^build evidence      $b/out/build-evidence " "$b/out/manifest.txt" && program_loads
then ok "a format 1 build failed at LVS alone rechecks to a pass: its LVS inputs made again the same bytes, its own logs in build-evidence/, program.sh loads it"
else bad "a format 1 build's recheck (exit $rc)"; cat "$t/run.txt"; fi
for point in keep-evidence evidence-rename keep-manifest report slack check-clk check-gclk check-bit decode roundtrip \
             lvs-gold lvs-extract lvs-bramwe lvs-selftest lvs-prove lvs-summary manifest-rename; do
  mk_build 1
  rm -f "$t/mark"
  : > "$CALLS"
  env STOP_AT="$point" STOP_MARK="$here/$t/mark" PATH="$here/$t/bin:$PATH" SURFCPU_OPENXC7=1 \
    "$SH" tools/openxc7-build.sh --recheck --out "$b/out" --lvs-solver z3cmd > "$t/run.txt" 2>&1 &
  pid=$!
  i=0
  while [ ! -s "$t/mark" ] && [ "$i" -lt 300 ]; do sleep 0.1; i=$((i + 1)); done
  if [ ! -s "$t/mark" ]; then
    kill -KILL "$pid" 2> /dev/null || true; wait "$pid" 2> /dev/null || true
    bad "stopped at $point: the recheck never got there"; cat "$t/run.txt"; continue
  fi
  kill -KILL "$pid"; wait "$pid" 2> /dev/null || true
  kill -KILL "$(cat "$t/mark")" 2> /dev/null || true
  if cmp -s "$b/out/manifest.txt" "$t/build-manifest.txt" && program_refuses
  then
    rc_run
    if passed && evidence_kept && program_loads
    then ok "stopped at $point (SIGKILL): the build's manifest beside the bitstream, which program.sh refuses; run again, the recheck passes, the build's logs kept"
    else bad "stopped at $point: the recheck run again (exit $rc)"; cat "$t/run.txt"; fi
  else bad "stopped at $point: the manifest beside the bitstream is not the build's, or program.sh loaded it (exit $prc)"; cat "$t/prog.txt"; fi
done

# ---------------------------------------------------------------- 2. refused before it begins
refused() {      # WHAT PATTERN: the build in $b as mutated, refused (exit 3) saying PATTERN, nothing changed or run
  snap "$t/before.txt"
  rc_run
  snap "$t/after.txt"
  if [ "$rc" = 3 ] && grep -q -- "$2" "$t/run.txt" && cmp -s "$t/before.txt" "$t/after.txt" && [ ! -s "$CALLS" ]
  then ok "refuses $1 (exit 3): the directory byte for byte as it was, nothing run"
  else bad "refuses $1 (exit $rc)"; cat "$t/run.txt"; diff "$t/before.txt" "$t/after.txt" || true; cat "$CALLS"; fi
}
mk_build 1; printf x >> "$b/out/t.bit"; refused "a bitstream that is not the one recorded" "t.bit (sha256 [0-9a-f]*) is not the one"
mk_build 1; printf x >> "$b/x.xdc"; refused "constraints that are not the ones recorded" "x.xdc (sha256 [0-9a-f]*) is not the one"
for f in t.json t.fasm placement.json; do
  mk_build 2; printf x >> "$b/out/$f"; refused "a format 2 build whose $f is not the one recorded" "$f (sha256 [0-9a-f]*) is not the one"
done
for line in netlist "routed FASM" placement; do
  mk_build 2; edit "$b/out/manifest.txt" "/^$line  /d"
  refused "a format 2 manifest without its $line line" "is manifest format 2, which records .*, but has no $line line"
done
for f in gold.v fabric.v; do
  mk_build 1; rm "$b/out/lvs/$f"; refused "a format 1 build without its lvs/$f" "lvs/$f is missing: a format 1 build"
done
mk_build 1; edit "$b/out/manifest.txt" 's/^check synthesis     PASS/check synthesis     FAIL/'
refused "a build whose kept synthesis check failed" "check synthesis is 'FAIL': a recheck keeps it"
mk_build 1; rm "$b/out/report.json"; refused "a build without its report.json" "out/report.json is missing"
mk_build 3; refused "a manifest format it does not know" "manifest format 3, which this script does not know"
mk_build 1; add_after "$b/out/manifest.txt" '^lvs timeout ' 'recheck             2026-10-27T00:00:00Z, made up'
refused "a recheck's manifest with the build's own not beside it" "is a recheck's manifest, and the build's own"
mk_build 1 "yosys   0.0 (another image's)"
refused "a build made with other tools than this image's" "the tools of this image (/etc/surfcpu-openxc7.txt) are not the ones the build ran with"
mk_build 1; cp "$b/out/manifest.txt" "$b/out/manifest.build.txt"; edit "$b/out/manifest.txt" 's/^date (UTC)          .*/date (UTC)          2026-10-27T01:00:00Z/'
refused "a later build's manifest beside an earlier recheck's manifest.build.txt" "is a build's own manifest, but not the one an earlier recheck kept"
mk_build 1; cp "$b/out/manifest.txt" "$b/out/manifest.build.txt"; add_after "$b/out/manifest.txt" '^lvs timeout ' 'recheck             2026-10-27T00:00:00Z, made up'
refused "a format 1 build whose LVS inputs an earlier recheck made again without keeping them" "the build's lvs/gold.v is not known"

# ---------------------------------------------------------------- 3. refused after it begins: the LVS inputs made again
for f in t.json placement.json t.fasm; do
  mk_build 1; printf x >> "$b/out/$f"
  rc_run
  if [ "$rc" = 4 ] && grep -q "made again (sha256 [0-9a-f]*) is not the build's (sha256 [0-9a-f]*); refusing (exit 4)" "$t/run.txt" &&
     cmp -s "$b/out/manifest.txt" "$t/build-manifest.txt" && ! grep -q '^lvs_equiv ' "$CALLS" && grep -q '^tileverilog ' "$CALLS" &&
     evidence_kept && program_refuses
  then ok "a format 1 build whose $f is not the one it proved: refused (exit 4) before the proof, its manifest as it was, program.sh refuses"
  else bad "a format 1 build whose $f is not the one it proved (exit $rc)"; cat "$t/run.txt"; fi
done

# ---------------------------------------------------------------- 4. a recheck of a recheck
mk_build 2
rc_run
cp "$b/out/manifest.txt" "$t/first-recheck.txt"
rc_run
if passed && evidence_kept && grep -q "its record $b/out/manifest.build.txt (format 2)" "$t/run.txt"
then ok "a recheck of a recheck reads the build's manifest and keeps the build's logs, not the first recheck's"
else bad "a recheck of a recheck (exit $rc)"; cat "$t/run.txt"; fi

# ---------------------------------------------------------------- 5. lvs_verdict
verdict() {      # WHAT STATUS WANT REPORT: lvs_verdict on REPORT (printf's %b) for lvs_equiv's STATUS
  printf '%b' "$4" > "$t/report.txt"
  got=$(dry=0 out=$t; . tools/openxc7-steps.sh; lvs_verdict "$2" "$t/report.txt")
  if [ "$got" = "$3" ]; then ok "verdict: $1: $3"; else bad "verdict: $1: '$got', wanted '$3'"; fi
}
verdict "every point proved" 0 PASS '  proved  r1\n  proved  r2\n\n2 proved, 0 differ, 0 unknown   (0.1s)\n'
verdict "exit 0 with an unknown point in its count" 0 "FAIL (lvs_equiv ended 0 without a count of 0 differ and 0 unknown)" \
  '  proved  r1\n  unknown r2\n\n1 proved, 0 differ, 1 unknown   (0.1s)\n'
verdict "exit 0 with a DIFFER line, its count 0 differ" 0 "FAIL (lvs_equiv ended 0 without a count of 0 differ and 0 unknown)" \
  '  proved  r1\n  DIFFER  r2\n\n2 proved, 0 differ, 0 unknown   (0.1s)\n'
verdict "exit 0 with no point proved" 0 "FAIL (lvs_equiv ended 0 having proved no point: nothing was compared)" \
  '0 proved, 0 differ, 0 unknown   (0.0s)\n'
verdict "exit 0 without its count" 0 "FAIL (lvs_equiv ended 0 without a count of 0 differ and 0 unknown)" '  proved  r1\n'
verdict "a point differs" 1 "FAIL (1 differ)" '  proved  r1\n  DIFFER  r2\n\n1 proved, 1 differ, 0 unknown   (0.1s)\n'
verdict "a point unknown, none differs" 1 "BLOCKED (1 unknown, none differ)" '  proved  r1\n  unknown r2\n\n1 proved, 0 differ, 1 unknown\n'
verdict "killed by SIGKILL" 137 "BLOCKED (killed: SIGKILL, exit 137)" '  proved  r1\n'
verdict "a difference, then killed" 137 "FAIL (1 differ before the proof stopped, exit 137)" '  proved  r1\n  DIFFER  r2\n'
verdict "stopped at its limit" 124 "BLOCKED (timeout)" '  proved  r1\n'
verdict "a setup fault" 2 "FAIL (lvs_equiv could not run the proof: exit 2, see check-lvs.txt)" 'parse error\n'
verdict "a crash" 139 "FAIL (lvs_equiv crashed: signal 11, exit 139)" '  proved  r1\n'

# ---------------------------------------------------------------- 6. z3cmd.py --summary
printf '%s\n' '3224975 2.104 0 unsat 812345' '310 0.020 4 (error "line 2 column 8: unknown constant n9") 15000' \
  '100 0.050 0 sat 20000' '48347140 15.9' > "$t/q.log"
if o=$(python3 -I devenv/openxc7/z3cmd.py --summary "$t/q.log" 2>&1) &&
   printf '%s\n' "$o" | grep -q '^z3cmd: 3 questions (1 error 4, 1 sat, 1 unsat); .*; 1 line unreadable$'
then ok "z3cmd: --summary reads a line holding Z3's error text, and counts one cut short as unreadable"
else bad "z3cmd: --summary on Z3's error text: $o"; fi

[ "$fails" = 0 ] || { echo "openxc7-recheck-test: FAIL ($fails of $checks checks)"; exit 1; }
echo "openxc7-recheck-test: PASS ($checks checks; $SH, ${awk_is:-awk})"
