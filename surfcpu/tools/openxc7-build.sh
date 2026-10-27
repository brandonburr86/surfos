#!/bin/sh
# SurfCPU open-source bitstream flow (openXC7; docs/OPEN-FLOW.md): Yosys synthesis, nextpnr
# placement and routing, Project X-Ray bitstream assembly, and the checks that need no board.
# This is the entry `make build-fpga` uses with FLOW=openxc7 (docs/BUILD-HOST.md, option C).
#
#   tools/openxc7-build.sh --top <module> --xdc <file> (--board <board> | --part <part>)
#                          [options] <source.v>...
#   tools/openxc7-build.sh --recheck --out <dir> [--lvs-solver libz3|z3cmd] [--lvs-timeout <s>] [--dry-run]
#
#   --board arty_a7_35t | arty_a7_100t    the board, which sets the part
#   --part xc7a35tcsg324-1 | xc7a100tcsg324-1 | ...  the part (Artix-7 xc7a35t/50t/100t)
#   --out <dir>        where everything goes (default build/openxc7/<top>-<board or part>)
#   -D NAME[=VALUE]    a Verilog define for synthesis (repeatable)
#   -I <dir>           an include directory for synthesis (repeatable)
#   --seed <n>[,<n>...]  placer seed (default 1); a list: placement and routing with each seed in
#                      turn until one routes with every clock meeting its constraint (the gate is
#                      the same for each; every attempt is recorded in seeds.txt and the manifest)
#   --pnr-timeout <s>  how long one seed's placement and routing may run before it is stopped and
#                      counted as failed ("timeout" in seeds.txt), so the next seed is tried
#                      (default 1800, or OPENXC7_PNR_TIMEOUT inside the image; tools/openxc7-steps.sh)
#   --min-slack <clock>=<ns>  the least slack this clock may have (repeatable); every clock without
#                      one wants max(0.5 ns, 4 % of its period). A placement with less is thin: the
#                      next seed is tried, and if every seed is thin the best is kept and marked THIN
#                      (devenv/openxc7/slackcheck.py, tools/openxc7-steps.sh)
#   --dsp safe|full|none   multipliers: safe (default) maps them to DSP48E1 but lets Yosys pack
#                      no registers or adders into the DSPs (scratchpad xilinx_dsp.multonly),
#                      because its packing can delete logic (docs/OPEN-FLOW.md); full is
#                      openXC7's plain synth_xilinx; none keeps multipliers out of DSPs (-nodsp)
#   --expect <file>    minimum flip-flops per instance for the synthesis check (synthcheck.py)
#   --synth-only       stop after synthesis and its check (no pins needed, --xdc optional)
#   --manifest-include <file>  append this file (ROM hashes, ISA/ABI versions...) to the manifest
#   --no-lvs           skip the LVS proof (it can take long on a large design)
#   --lvs-timeout <s>  how long the LVS proof may run before it counts as BLOCKED (default
#                      1800, or OPENXC7_LVS_TIMEOUT inside the image); the whole SoC needs more
#   --lvs-solver libz3|z3cmd  how the proof's questions reach Z3 (tools/openxc7-steps.sh):
#                      libz3 (default, or OPENXC7_LVS_SOLVER inside the image), lvs_equiv's own
#                      incremental session, whose memory grows with the run; z3cmd, one solver
#                      process a question (devenv/openxc7/z3cmd.py), slower, its memory a
#                      question's. The same questions either way; the manifest says which ran
#   --no-check         skip every check after the bitstream
#   --dry-run          print what would run, without the image
#   --recheck          the checks of a routed build again, on the build in --out, without
#                      synthesis, placement or routing (docs/OPEN-FLOW.md section 3): the
#                      bitstream, round trip, block RAM, LVS (gold.v from the synthesized JSON and
#                      fabric.v from the FASM made again, bramwe, the proof by --lvs-solver), clocks,
#                      slack and timing. It first checks the build against its manifest and refuses
#                      (exit 3, nothing changed) when a file is missing, the bitstream, the XDC (or,
#                      format 2, the netlist, FASM or placement) is not the one recorded, a check it
#                      keeps (SRL cells, synthesis) did not pass, or the image's tools are not the
#                      build's. Then it puts the build's manifest aside as manifest.build.txt (once)
#                      and writes manifest.txt anew: the build's identity and its kept checks as they
#                      were, the checks run again, when, by which commit's tools, with which LVS
#                      route. Without --lvs-solver and --lvs-timeout it takes the build's. Exit 0
#                      only if every check passed (BLOCKED aside, as in a build)
#
# The manifest's format is 2 (its "manifest format" line; none means 1): format 2 records the
# synthesized netlist, the FASM and the placement by SHA-256, the LVS solver, and a recheck's lines.
#
# Outputs in <dir>: <top>.bit (the bitstream), <top>.fasm/.frames/.json, timing.txt,
# utilization.txt, slack-seed<n>.txt (the slack rule on each routed attempt), check-srl.txt (a
# netlist with SRL cells stops here), check-synthesis.txt,
# check-bitstream.txt, check-roundtrip.txt,
# check-bram.txt, check-lvs.txt, check-clocks.txt, the logs and manifest.txt, seeds.txt (each
# placement attempt), and <top>.netlist.v with Yosys's cells_sim.v for gate-level simulation.
# Exit status 0 only if every step and every check passed. A bitstream made here has not been
# on a board: its physical behaviour is pending until it has.
#
# The chip database nextpnr places and routes with is this image's own, regenerated once per
# image and die without the global buffers' route-through pips (devenv/openxc7/chipdb-gen.py,
# about a minute and 3 GB the first time; kept in build/openxc7/chipdb/<image tag>/ and checked
# against devenv/openxc7/chipdb.sha256): through them nextpnr routed ordinary nets into unused
# BUFGCTRLs and onto the clock network, which the bitstream does not configure and LVS cannot
# see, and on the board those nets did not arrive (docs/OPEN-FLOW.md 6.8). The clock check also
# fails on any trace of that (devenv/openxc7/gclkcheck.py). SURFCPU_CHIPDB_DIR, inside the
# image, takes chipdb-<die>.bin from another directory as it is (the image's own ENV
# OPENXC7_CHIPDB_DIR, its unfiltered database, is not used).
#
# On the Mac it runs itself in the surfcpu-openxc7 image (devenv/openxc7/build-image.sh)
# with the repository at /work; paths must lie inside the repository. OPENXC7_THREADS
# (default 4) limits nextpnr's threads.
set -eu
IMAGE_TAG=2026-10-15
here=$(cd "$(dirname "$0")/.." && pwd -P)            # surfcpu/

die_usage() { echo "openxc7-build.sh: $*" >&2; echo "usage: see the top of $0" >&2; exit 2; }
die_recheck() { echo "openxc7-build.sh: --recheck: $*; refusing (nothing was changed)" >&2; exit 3; }

top= xdc= board= part= out= seed=1 lvs=1 check=1 dry=0 dsp=safe expect= synthonly=0 recheck=0
lvs_timeout=${OPENXC7_LVS_TIMEOUT:-1800}
lvs_solver=${OPENXC7_LVS_SOLVER:-libz3}
pnr_timeout=${OPENXC7_PNR_TIMEOUT:-1800}
defs= incs= srcs= extras= min_slack= lvs_timeout_given= lvs_solver_given=
for a in "$@"; do [ "$a" != --recheck ] || recheck=1; done
if [ "$recheck" = 1 ]; then          # a recheck takes the build's own settings from its manifest
  while [ $# -gt 0 ]; do
    case "$1" in
      --recheck) shift ;;
      --out)     [ $# -ge 2 ] || die_usage "--out needs a value"; out=$2; shift 2 ;;
      --lvs-timeout) [ $# -ge 2 ] || die_usage "--lvs-timeout needs a value"; lvs_timeout=$2 lvs_timeout_given=1; shift 2 ;;
      --lvs-solver)  [ $# -ge 2 ] || die_usage "--lvs-solver needs a value"; lvs_solver=$2 lvs_solver_given=1; shift 2 ;;
      --dry-run) dry=1; shift ;;
      *) die_usage "--recheck takes the build's own settings from its manifest: give it --out and, if you wish, --lvs-solver, --lvs-timeout and --dry-run, not $1" ;;
    esac
  done
  [ -n "$out" ] || die_usage "--recheck needs --out, the build's output directory"
fi
while [ "$recheck" = 0 ] && [ $# -gt 0 ]; do
  case "$1" in
    --top)   [ $# -ge 2 ] || die_usage "--top needs a value";   top=$2; shift 2 ;;
    --xdc)   [ $# -ge 2 ] || die_usage "--xdc needs a value";   xdc=$2; shift 2 ;;
    --board) [ $# -ge 2 ] || die_usage "--board needs a value"; board=$2; shift 2 ;;
    --part)  [ $# -ge 2 ] || die_usage "--part needs a value";  part=$2; shift 2 ;;
    --out)   [ $# -ge 2 ] || die_usage "--out needs a value";   out=$2; shift 2 ;;
    --seed)  [ $# -ge 2 ] || die_usage "--seed needs a value";  seed=$2; shift 2 ;;
    -D)      [ $# -ge 2 ] || die_usage "-D needs a value";      defs="$defs $2"; shift 2 ;;
    -D*)     defs="$defs ${1#-D}"; shift ;;
    -I)      [ $# -ge 2 ] || die_usage "-I needs a value";      incs="$incs $2"; shift 2 ;;
    -I*)     incs="$incs ${1#-I}"; shift ;;
    --manifest-include) [ $# -ge 2 ] || die_usage "--manifest-include needs a value"; extras="$extras $2"; shift 2 ;;
    --dsp)   [ $# -ge 2 ] || die_usage "--dsp needs a value";   dsp=$2; shift 2 ;;
    --expect) [ $# -ge 2 ] || die_usage "--expect needs a value"; expect=$2; shift 2 ;;
    --synth-only) synthonly=1; shift ;;
    --no-lvs)   lvs=0; shift ;;
    --lvs-timeout) [ $# -ge 2 ] || die_usage "--lvs-timeout needs a value"; lvs_timeout=$2; shift 2 ;;
    --lvs-solver)  [ $# -ge 2 ] || die_usage "--lvs-solver needs a value"; lvs_solver=$2; shift 2 ;;
    --pnr-timeout) [ $# -ge 2 ] || die_usage "--pnr-timeout needs a value"; pnr_timeout=$2; shift 2 ;;
    --min-slack) [ $# -ge 2 ] || die_usage "--min-slack needs a value"; min_slack="$min_slack $2"; shift 2 ;;
    --no-check) check=0; shift ;;
    --dry-run)  dry=1; shift ;;
    -h|--help)  awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"; exit 0 ;;
    -*)      die_usage "unknown option $1" ;;
    *)       srcs="$srcs $1"; shift ;;
  esac
done
case "$lvs_timeout" in ''|*[!0-9]*) die_usage "--lvs-timeout must be a number of seconds" ;; esac
case "$lvs_solver" in libz3|z3cmd) ;; *) die_usage "--lvs-solver must be libz3 or z3cmd, not '$lvs_solver'" ;; esac
family=artix7
set_device() {    # the part's device and chip database die (Project X-Ray's)
  case "$part" in
    xc7a35t*)  device=xc7a35t;  chipdie=xc7a50t ;;      # the 35T is the 50T die (prjxray-db)
    xc7a50t*)  device=xc7a50t;  chipdie=xc7a50t ;;
    xc7a100t*) device=xc7a100t; chipdie=xc7a100t ;;
    *) die_usage "part $part is not supported by this image (xc7a35t, xc7a50t, xc7a100t)" ;;
  esac
}
if [ "$recheck" = 1 ]; then        # the build's directory, relative to surfcpu/ as the image sees it
  [ -d "$out" ] || die_recheck "no build directory $out"
  out=$(cd "$out" && pwd -P)
  case "$out" in
    "$here"/*) out=${out#"$here"/} ;;
    *) die_recheck "$out is outside the repository's surfcpu/ directory (the image sees only the repository)" ;;
  esac
fi
if [ "$recheck" = 0 ]; then         # the build's arguments
  [ -n "$top" ] || die_usage "--top is required"
  [ -n "$xdc" ] || [ "$synthonly" = 1 ] || die_usage "--xdc is required"
  case "$dsp" in safe|full|none) ;; *) die_usage "--dsp must be safe, full or none" ;; esac
  [ -n "$srcs" ] || die_usage "no Verilog sources given"
  case "$seed" in ''|*[!0-9,]*|,*|*,|*,,*) die_usage "--seed must be a number or a list of numbers (2,3,4)" ;; esac
  seeds=$(echo "$seed" | tr ',' ' ')
  case "$pnr_timeout" in ''|*[!0-9]*) die_usage "--pnr-timeout must be a number of seconds" ;; esac
  [ "$pnr_timeout" -gt 0 ] 2>/dev/null || die_usage "--pnr-timeout must be more than 0 seconds (timeout takes 0 as no limit)"
  slack_clocks=
  for w in $min_slack; do
    case "$w" in
      *=*=*|=*|*=) die_usage "--min-slack wants CLOCK=NS, not $w" ;;
      *=*) case "${w#*=}" in *[!0-9.]*|.|*.*.*) die_usage "--min-slack $w: NS must be a number of nanoseconds" ;; esac ;;
      *) die_usage "--min-slack wants CLOCK=NS, not $w" ;;
    esac
    case " $slack_clocks " in *" ${w%%=*} "*) die_usage "--min-slack names ${w%%=*} twice" ;; esac
    slack_clocks="$slack_clocks ${w%%=*}"
  done
  if [ -z "$part" ]; then
    case "$board" in
      arty_a7_35t)  part=xc7a35tcsg324-1 ;;   # the board's XC7A35TICSG324-1L: same die and
                                               # bitstream; the -1L timing is not modelled
      arty_a7_100t) part=xc7a100tcsg324-1 ;;
      "") die_usage "--board or --part is required" ;;
      *)  die_usage "unknown board $board (arty_a7_35t, arty_a7_100t)" ;;
    esac
  fi
  set_device
  [ -n "$out" ] || out=build/openxc7/$top-${board:-$part}
fi

# ------------------------------------------------------------------ on the Mac: into the image
if [ "${SURFCPU_OPENXC7:-0}" != 1 ] && [ "$dry" = 0 ]; then
  if [ "$recheck" = 1 ]; then
    # out is already relative to surfcpu/; without the LVS options the image takes the build's
    set -- --recheck --out "$out"
    [ -z "$lvs_solver_given" ] || set -- "$@" --lvs-solver "$lvs_solver"
    [ -z "$lvs_timeout_given" ] || set -- "$@" --lvs-timeout "$lvs_timeout"
  else
    # Give every path relative to surfcpu/, which is where the container starts.
    rel() {
      case "$1" in /*) p=$1 ;; *) p=$PWD/$1 ;; esac
      d=$(dirname "$p"); b=$(basename "$p")
      if [ "${2:-}" = mkdir ]; then mkdir -p "$p"; d=$p; b=; fi
      d=$(cd "$d" 2>/dev/null && pwd -P) || { echo "openxc7-build.sh: no such directory for $1" >&2; exit 2; }
      p=${d}${b:+/$b}
      case "$p" in
        "$here") echo . ;;
        "$here"/*) echo "${p#"$here"/}" ;;
        *) echo "openxc7-build.sh: $1 is outside the repository's surfcpu/ directory" >&2; exit 2 ;;
      esac
    }
    set -- --top "$top" --part "$part" --out "$(rel "$out" mkdir)" --seed "$seed" --dsp "$dsp"
    [ -n "$xdc" ] && set -- "$@" --xdc "$(rel "$xdc")"
    [ -n "$expect" ] && set -- "$@" --expect "$(rel "$expect")"
    [ "$synthonly" = 1 ] && set -- "$@" --synth-only
    [ -n "$board" ] && set -- "$@" --board "$board"
    for d in $defs; do set -- "$@" -D "$d"; done
    for i in $incs; do set -- "$@" -I "$(rel "$i")"; done
    for e in $extras; do set -- "$@" --manifest-include "$(rel "$e")"; done
    [ "$lvs" = 0 ] && set -- "$@" --no-lvs
    set -- "$@" --lvs-timeout "$lvs_timeout" --lvs-solver "$lvs_solver" --pnr-timeout "$pnr_timeout"
    for w in $min_slack; do set -- "$@" --min-slack "$w"; done
    [ "$check" = 0 ] && set -- "$@" --no-check
    for s in $srcs; do set -- "$@" "$(rel "$s")"; done
  fi
  cd "$here"
  image=${SURFCPU_OPENXC7_IMAGE:-surfcpu-openxc7:$IMAGE_TAG}
  docker=${DOCKER:-docker}
  command -v "$docker" >/dev/null 2>&1 || docker=/Applications/Docker.app/Contents/Resources/bin/docker
  if ! "$docker" image inspect "$image" >/dev/null 2>&1; then
    echo "openxc7-build.sh: image $image is missing; build it with devenv/openxc7/build-image.sh" >&2
    exit 2
  fi
  SURFCPU_IMAGE=$image exec devenv/run.sh sh tools/openxc7-build.sh "$@"
fi

# ------------------------------------------------------------------ the flow (in the image, or --dry-run)
dbroot=${PRJXRAY_DB_DIR:-/opt/openxc7/share/prjxray-db}
db=$dbroot/$family
threads=${OPENXC7_THREADS:-4}
tools=devenv/openxc7
cd "$here"
sha() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1; }

# run (each step, timed and logged), place_and_route (the placer seeds, each under pnr_timeout),
# post_route_checks (the checks of a routed build, LVS by lvs_prove and lvs_verdict)
. "$here/tools/openxc7-steps.sh"

# ------------------------------------------------------------------ --recheck
# The build's own record is its manifest: manifest.build.txt once a recheck has put it aside, else
# manifest.txt (never a recheck's: one that is, with no manifest.build.txt beside it, is refused).
# A line's value is what follows its label's 20 columns, as tools/fpga-build.sh and program.sh read it.
if [ "$recheck" = 1 ]; then
  mfield() {      # LABEL FILE: the value of the LABEL line, among the lines before the first "## " section
    awk -v n="$1" 'BEGIN { n = sprintf("%-20s", n) } /^## / { exit } substr($0, 1, 20) == n { print substr($0, 21); exit }' "$2"
  }
  result_of() { mfield "check $1" "$2" | sed 's/ ([^()]*)$//'; }
  section() { awk -v h="## $1" '$0 == h || index($0, h " (") == 1 { on = 1; next } /^## / { on = 0 } on' "$2"; }
  rec=$out/manifest.txt
  [ ! -f "$out/manifest.build.txt" ] || rec=$out/manifest.build.txt
  [ -f "$rec" ] || die_recheck "no manifest in $out: not a finished build of this flow"
  case "$(head -n 1 "$rec")" in "# openXC7 build manifest"*) ;; *) die_recheck "$rec is not this flow's manifest" ;; esac
  [ -z "$(mfield recheck "$rec")" ] ||
    die_recheck "$rec is a recheck's manifest, and the build's own ($out/manifest.build.txt) is not beside it"
  fmt=$(mfield "manifest format" "$rec"); fmt=${fmt%% *}
  case "${fmt:-1}" in 1|2) ;; *) die_recheck "$rec is manifest format $fmt, which this script does not know (1, 2)" ;; esac
  top=$(mfield top "$rec") part=$(mfield part "$rec") board=$(mfield board "$rec")
  part=${part%% *}
  r_xdc=$(mfield xdc "$rec") r_bit=$(mfield bitstream "$rec") r_seed=$(mfield seed "$rec") r_date=$(mfield "date (UTC)" "$rec")
  r_seed=${r_seed%% *}
  [ -n "$top" ] && [ -n "$part" ] && [ -n "$r_xdc" ] && [ -n "$r_bit" ] && [ -n "$r_seed" ] ||
    die_recheck "$rec does not name the build's top, part, XDC, bitstream and seed"
  case "$r_seed" in *[!0-9]*) die_recheck "$rec: the seed '$r_seed' is not a number" ;; esac
  set_device
  xdc=${r_xdc%% *}
  # the files the checks read, all there, and those the record names by SHA-256 the same
  for f in "$top.bit" "$top.fasm" "$top.json" placement.json report.json nextpnr.log stat.json seeds.txt \
           check-srl.txt check-synthesis.txt; do
    [ -f "$out/$f" ] || die_recheck "$out/$f is missing"
  done
  [ -f "$xdc" ] || die_recheck "the build's constraints $xdc are missing"
  recorded() {    # LABEL PATH: PATH is the file the record's LABEL line names, with that SHA-256
    v=$(mfield "$1" "$rec")
    [ "${v%% *}" = "$2" ] || die_recheck "$rec's $1 line names ${v%% *}, not $2 (was the build moved?)"
    want=$(printf '%s\n' "$v" | sed -n 's/^[^ ]* sha256:\([0-9a-f]\{64\}\)\( .*\)\{0,1\}$/\1/p')
    got=$(sha "$2")
    [ "$got" = "$want" ] || die_recheck "$2 (sha256 $got) is not the one $rec records (${want:-no sha256})"
  }
  recorded bitstream "$out/$top.bit"
  recorded xdc "$xdc"
  for l in "netlist:$out/$top.json" "routed FASM:$out/$top.fasm" "placement:$out/placement.json"; do
    [ -z "$(mfield "${l%%:*}" "$rec")" ] || recorded "${l%%:*}" "${l#*:}"   # format 2 records them
  done
  # the checks it keeps (pre-route: they read the synthesized netlist, which a recheck does not touch)
  for c in "SRL cells" synthesis; do
    v=$(result_of "$c" "$rec")
    [ "$v" = PASS ] || die_recheck "the build's check $c is '${v:-missing}': a recheck keeps it, so it must have passed"
  done
  # the build's slack rule: the design list's min-slack words, as its manifest has them
  min_slack=$(mfield "slack rule" "$rec" | sed -n 's/.*; the design list sets: *//p')
  for w in $min_slack; do
    case "$w" in
      *=*=*|=*|*=) die_recheck "$rec's slack rule has '$w', not CLOCK=NS" ;;
      *=*) ;;
      *) die_recheck "$rec's slack rule has '$w', not CLOCK=NS" ;;
    esac
  done
  # the LVS route and limit: the build's, unless given
  if [ -z "$lvs_solver_given" ]; then v=$(mfield "lvs solver" "$rec"); lvs_solver=${v%% *}; lvs_solver=${lvs_solver:-libz3}; fi
  if [ -z "$lvs_timeout_given" ]; then v=$(mfield "lvs timeout" "$rec"); lvs_timeout=${v%% *}; fi
  case "$lvs_solver" in libz3|z3cmd) ;; *) die_recheck "$rec's LVS solver is '$lvs_solver'" ;; esac
  case "$lvs_timeout" in ''|*[!0-9]*) die_recheck "no LVS time limit: $rec records none, and --lvs-timeout was not given" ;; esac
  # the image's tools are the build's: what each check runs is what the build's would have run
  r_tools=$(section "tools" "$rec")
  if [ "$dry" = 0 ]; then
    [ "$r_tools" = "$(cat /etc/surfcpu-openxc7.txt 2>/dev/null)" ] ||
      die_recheck "the tools of this image (/etc/surfcpu-openxc7.txt) are not the ones the build ran with (its manifest's ## tools): recheck in the build's image"
  fi
  echo "openxc7-build.sh: --recheck of $out: $top for $part ($device), built $r_date with seed $r_seed; its record $rec (format ${fmt:-1})"
  echo "openxc7-build.sh: the bitstream, the XDC$( [ -z "$(mfield netlist "$rec")" ] || echo ", the netlist, the FASM, the placement") as recorded; kept: SRL cells PASS, synthesis PASS"
  echo "openxc7-build.sh: run again: bitstream, round trip, block RAM, LVS by $lvs_solver within $lvs_timeout s, clocks, slack margin, timing"
  [ "$dry" = 0 ] || echo "openxc7-build.sh: --dry-run: the image's tools are compared with the record's in the image only; would run:"
  lvs_was=
  if [ "$dry" = 0 ]; then
    if [ "$rec" = "$out/manifest.txt" ]; then mv "$out/manifest.txt" "$out/manifest.build.txt"; rec=$out/manifest.build.txt; fi
    for f in gold.v fabric.raw.v fabric.v; do
      if [ -f "$out/lvs/$f" ]; then lvs_was="$lvs_was $f:$(sha "$out/lvs/$f")"; fi
    done
    for f in manifest.txt times.txt check-bitstream.txt check-roundtrip.txt check-bram.txt check-lvs.txt check-clocks.txt \
             check-gclk.txt timing.txt utilization.txt report.out "$top.decoded.fasm" bit2fasm.log bram-extracted.v \
             bram-extract.log; do
      rm -f "${out:?}/$f"
    done
    rm -rf "${out:?}/lvs"
    mkdir -p "$out/lvs"
  fi
  check=1 lvs=1 fail=0 seed_used=$r_seed
  run report report.out python3 -I $tools/report.py --device "$device" --report "$out/report.json" \
    --log "$out/nextpnr.log" --stat "$out/stat.json" --timing "$out/timing.txt" --utilization "$out/utilization.txt"
  # the slack rule on the placement the build kept (only slackcheck's own thin is thin)
  if [ "$dry" = 1 ]; then
    echo "[slack] python3 -I $tools/slackcheck.py --report $out/report.json $(slack_args)--out $out/slack-seed$seed_used.txt"
    slack_state=PASS slack_note=
  else
    # shellcheck disable=SC2046
    if sl=$(python3 -I "$tools/slackcheck.py" --report "$out/report.json" $(slack_args) \
              --out "$out/slack-seed$seed_used.txt" 2> "$out/slack-seed$seed_used.err")
    then st_sl=0; else st_sl=$?; fi
    read -r _ sclk sslack swant _ <<EOF
$sl
EOF
    if [ "$st_sl" = 0 ]; then
      slack_state=PASS slack_note="least margin $sclk $sslack ns, wants $swant"
    elif [ "$st_sl" = 1 ] && [ "${sl%% *}" = thin ]; then
      slack_state=THIN slack_note="$sclk $sslack ns, wants $swant: the placement the build kept, seed $seed_used"
    else
      why=$(tail -n 1 "$out/slack-seed$seed_used.err" 2>/dev/null | tr '()' '[]')
      slack_state=FAIL slack_note="the slack rule: ${why:-slackcheck exit $st_sl, no reason given}" fail=1
    fi
  fi
  post_route_checks
  [ "$dry" = 1 ] && exit 0
  lvs_same=
  for f in gold.v fabric.raw.v fabric.v; do
    was=$(printf '%s\n' $lvs_was | sed -n "s/^$f://p")
    if [ ! -f "$out/lvs/$f" ]; then lvs_same="$lvs_same; $f not made"
    elif [ -z "$was" ]; then lvs_same="$lvs_same; $f made, the build left none"
    elif [ "$was" = "$(sha "$out/lvs/$f")" ]; then lvs_same="$lvs_same; $f the same bytes as the build's"
    else lvs_same="$lvs_same; $f differs from the build's, sha256 $was"; fi
  done
  {
    echo "# openXC7 build manifest (tools/openxc7-build.sh), rechecked (--recheck); physical behaviour: pending (never on a board)"
    echo "manifest format     2 (docs/OPEN-FLOW.md section 3)"
    # the build's identity as its record has it, from its top line to its bitstream's: kept
    awk 'NR > 1 && /^check / { exit } NR > 1' "$rec" | grep -v -e '^manifest format ' -e '^netlist  ' -e '^routed FASM  ' -e '^placement  '
    for l in "netlist:$top.json" "routed FASM:$top.fasm" "placement:placement.json"; do
      printf '%-20s%s sha256:%s%s\n' "${l%%:*}" "$out/${l#*:}" "$(sha "$out/${l#*:}")" \
        "$( [ -n "$(mfield "${l%%:*}" "$rec")" ] || echo " (as the recheck read it: the build recorded none)")"
    done
    echo "check SRL cells     PASS (kept from the build: none in the synthesized netlist, which LVS could not check: check-srl.txt)"
    echo "check synthesis     PASS (kept from the build: netlist against the RTL: flip-flops, memories, outputs: check-synthesis.txt)"
    echo "check bitstream     $result_bit (run again: IDCODE, frames, commands, CRC: check-bitstream.txt)"
    echo "check round trip    $result_rt (run again: bitstream decoded back to FASM: check-roundtrip.txt)"
    echo "check block RAM     $result_bram (run again: contents decoded from the bitstream: check-bram.txt)"
    echo "check LVS           $result_lvs (run again: routed netlist equivalent to the synthesis, lvs_equiv by the $lvs_solver route: check-lvs.txt)"
    echo "lvs solver          $lvs_solver ($(lvs_route_note))"
    echo "lvs timeout         $lvs_timeout s"
    echo "check clocks        $result_clk (run again: MMCM/PLL dividers against XAPP888, no net through a global buffer: check-clocks.txt)"
    echo "check slack margin  $slack_state (run again: $slack_note: slack-seed$seed_used.txt)"
    echo "recheck             $(date -u +%Y-%m-%dT%H:%M:%SZ), tools/openxc7-build.sh --recheck at ${SURFCPU_BUILD_ID:-unknown} dirty=${SURFCPU_BUILD_DIRTY:-unknown}, image surfcpu-openxc7:$IMAGE_TAG"
    echo "checks run again    bitstream, round trip, block RAM, LVS, clocks, slack margin, timing and utilization"
    echo "checks kept         SRL cells, synthesis: the build's of $r_date"
    echo "build manifest      $rec sha256:$(sha "$rec") (the build's own: check LVS $(result_of LVS "$rec" | tr '()' '[]'))"
    echo "lvs inputs          made again from the netlist and the FASM${lvs_same}"
    echo "## placement attempts (the build's)"; section "placement attempts" "$rec"
    echo "## timing"; grep '^clock ' "$out/timing.txt" || true
    echo "## utilization"; sed -n '/documented capacity/,$p' "$out/utilization.txt"
    echo "## step times (the build's)"; section "step times" "$rec"
    echo "## step times (this recheck)"; cat "$out/times.txt"
    echo "## tools (/etc/surfcpu-openxc7.txt)"; cat /etc/surfcpu-openxc7.txt
    sed -n '/^## included: /,$p' "$rec"
  } > "$out/manifest.txt"
  grep '^clock ' "$out/timing.txt" | sed 's/^/openxc7-build.sh: /' || true
  echo "openxc7-build.sh: recheck: kept SRL cells PASS, synthesis PASS; run again: bitstream $result_bit, round trip $result_rt, block RAM $result_bram, LVS $result_lvs by $lvs_solver, clocks $result_clk, slack margin $slack_state"
  echo "openxc7-build.sh: $out/manifest.txt written anew; the build's own is $rec"
  exit $fail
fi

if [ -n "${SURFCPU_CHIPDB_DIR:-}" ]; then
  chipdb=$SURFCPU_CHIPDB_DIR/chipdb-$chipdie.bin
  chipdb_from="SURFCPU_CHIPDB_DIR, as it is"
else
  chipdb=build/openxc7/chipdb/$IMAGE_TAG/chipdb-$chipdie.bin
  chipdb_from="the image's generator without BUFGCTRL route-throughs, devenv/openxc7/chipdb-gen.py"
fi

make_chipdb() {   # the default chip database, once per image and die (docs/OPEN-FLOW.md 6.8)
  if [ -f "$chipdb" ]; then echo "$chipdb: there"; return 0; fi
  bba=/tmp/chipdb-$chipdie.$$.bba
  mkdir -p "$(dirname "$chipdb")"
  python3 $tools/chipdb-gen.py --xray "$db" --device "$chipdie" --bba "$bba" &&
    bbasm -l "$bba" "$chipdb.$$.tmp" && rm -f "$bba" && mv -f "$chipdb.$$.tmp" "$chipdb"
}

check_chipdb() {  # its SHA-256 must be the one devenv/openxc7/chipdb.sha256 gives for this image
  want=$(awk -v f="chipdb-$chipdie.bin" -v t="$IMAGE_TAG" '$2 == f && $3 == t { print $1 }' $tools/chipdb.sha256)
  got=$(sha256sum "$chipdb" | cut -d' ' -f1)
  echo "$chipdb sha256:$got (devenv/openxc7/chipdb.sha256 for image $IMAGE_TAG: ${want:-none})"
  [ -n "$want" ] && [ "$want" = "$got" ]
}

if [ "$dry" = 0 ]; then
  command -v nextpnr-himbaechel >/dev/null || { echo "openxc7-build.sh: not in the openXC7 image" >&2; exit 2; }
  [ -z "${SURFCPU_CHIPDB_DIR:-}" ] || [ -f "$chipdb" ] || { echo "openxc7-build.sh: no chip database $chipdb" >&2; exit 2; }
  [ -z "$xdc" ] || [ -f "$xdc" ] || { echo "openxc7-build.sh: no such XDC: $xdc" >&2; exit 2; }
  for s in $srcs; do [ -f "$s" ] || { echo "openxc7-build.sh: no such source: $s" >&2; exit 2; }; done
  rm -rf "$out/lvs"
  mkdir -p "$out/lvs"
  rm -f "$out/times.txt" "$out/$top.bit" "$out/manifest.txt" "$out"/check-*.txt "$out/seeds.txt" "$out"/nextpnr-seed*.log
  echo "openxc7-build.sh: $top for $part ($device, chip database $chipdie) in $out"
fi

# Synthesis: openXC7's recipe (synth_xilinx -flatten -abc9 -arch xc7), with the DSP mode, then
# setundef -zero: Yosys leaves the data inputs of unused memory bits undefined (x; the CPU's
# 63-bit fetch FIFO in 11 RAM32M has three), nextpnr routes them to something, and LVS cannot
# prove a routed pin equal to an x (one DIFFER on the whole SoC). Zeros are a valid choice for
# a don't-care, and with them LVS checks every pin exactly.
rv="read_verilog"
for d in $defs; do rv="$rv -D$d"; done
for i in $incs; do rv="$rv -I$i"; done
rv="$rv$srcs"
case "$dsp" in
  safe) dsp_pre="scratchpad -set xilinx_dsp.multonly 1
" dsp_opt= ;;
  full) dsp_pre= dsp_opt= ;;
  none) dsp_pre= dsp_opt=" -nodsp" ;;
esac
# The elaborated RTL before any mapping, for the synthesis check (synthcheck.py).
pre="$rv
hierarchy -top $top
proc
flatten
opt -full
wreduce
opt_clean
memory -nomap
opt_clean
write_json $out/pre.json"
if [ "$dry" = 1 ]; then printf '[pre.ys]\n%s\n' "$pre"; else printf '%s\n' "$pre" > "$out/pre.ys"; fi
run presynth presynth.log yosys -q -s "$out/pre.ys"
ys="$rv
${dsp_pre}synth_xilinx -flatten -abc9 -arch xc7 -top $top$dsp_opt
setundef -zero
tee -q -o $out/stat.json stat -json -tech xilinx
write_json $out/$top.json
write_verilog -noattr $out/$top.netlist.v"
if [ "$dry" = 1 ]; then printf '[synth.ys]\n%s\n' "$ys"; else printf '%s\n' "$ys" > "$out/synth.ys"; fi
run synth yosys.log yosys -q -l "$out/yosys.full.log" -s "$out/synth.ys"
# Shift-register LUTs (SRL16E, SRLC32E): LVS cannot check them, its extractor having no shift mode
# (devenv/openxc7/srlcheck.py, docs/OPEN-FLOW.md 6.5), so with the checks on a netlist with any is
# refused here, before anything is placed, saying which chains to give a reset.
result_srl=skipped
if [ "$check" = 1 ] || [ "$synthonly" = 1 ]; then
  if run check-srl check-srl.txt python3 -I $tools/srlcheck.py --json "$out/$top.json" --top "$top"; then
    result_srl=PASS
  else
    st=$?
    if [ "$st" = 1 ]; then
      echo "openxc7-build.sh: the synthesized netlist has SRL cells, which the open flow's LVS cannot check;" \
           "give each chain a reset (check-srl.txt)" >&2
    else
      echo "openxc7-build.sh: the SRL check could not read the synthesized netlist (exit $st; check-srl.txt)" >&2
    fi
    exit 1
  fi
fi
# the primitive models of this Yosys, for simulating <top>.netlist.v elsewhere (Icarus)
run models cells_sim.log cp /opt/openxc7/share/yosys/xilinx/cells_sim.v "$out/cells_sim.v"

# The synthesized netlist against the RTL: lost flip-flops, memories or outputs fail it.
result_syn=skipped
if [ "$check" = 1 ] || [ "$synthonly" = 1 ]; then
  if run check-synth check-synthesis.txt python3 -I $tools/synthcheck.py --top "$top" --pre "$out/pre.json" \
       --post "$out/$top.json" ${expect:+--expect "$expect"}
  then result_syn=PASS; else result_syn=FAIL; fi
fi
if [ "$synthonly" = 1 ]; then
  [ "$dry" = 1 ] && exit 0
  grep -E '^whole design|^memories|^outputs constant|^PROBLEM|^synthcheck' "$out/check-synthesis.txt" | sed 's/^/openxc7-build.sh: /'
  echo "openxc7-build.sh: synthesis only ($dsp DSP mode): check synthesis $result_syn; $out/$top.json"
  [ "$result_syn" = PASS ]
  exit $?
fi

# Placement and routing (tools/openxc7-steps.sh): each seed in turn, each under pnr_timeout.
if [ -z "${SURFCPU_CHIPDB_DIR:-}" ] && [ "$dry" = 1 ]; then
  echo "[chipdb] python3 $tools/chipdb-gen.py --xray $db --device $chipdie --bba /tmp/chipdb-$chipdie.bba;" \
       "bbasm -l /tmp/chipdb-$chipdie.bba $chipdb (when it is not there yet; then its sha256 against $tools/chipdb.sha256)"
elif [ -z "${SURFCPU_CHIPDB_DIR:-}" ]; then
  run chipdb chipdb.log make_chipdb
  run chipdb-sha chipdb-sha.txt check_chipdb ||
    { echo "openxc7-build.sh: the chip database is not the one this image's generator makes (update devenv/openxc7/chipdb.sha256 only on purpose)" >&2; exit 1; }
fi
place_and_route $seeds
[ "$pnr_ok" = 1 ] || { echo "openxc7-build.sh: no placer seed of $seeds placed and routed within its constraints" >&2; exit 1; }

# Bitstream: FASM -> frames -> .bit with Project X-Ray.
run frames fasm2frames.log env PYTHONWARNINGS=ignore::RuntimeWarning \
  fasm2frames --part "$part" --db-root "$db" "$out/$top.fasm" "$out/$top.frames"
run bit xc7frames2bit.log xc7frames2bit --part_file "$db/$part/part.yaml" --part_name "$part" \
  --frm_file "$out/$top.frames" --output_file "$out/$top.bit"
# xc7frames2bit writes the time of the build into the .bit header (fields c and d): with
# SOURCE_DATE_EPOCH set (make release: the commit's time) they take that time instead, so two
# builds of one commit are the same bytes (docs/RELEASE.md section 4). The stream is untouched.
[ -z "${SOURCE_DATE_EPOCH:-}" ] || run stamp bitstamp.log python3 -I tools/bitfile.py stamp "$out/$top.bit" \
  --epoch "$SOURCE_DATE_EPOCH"
run report report.out python3 -I $tools/report.py --device "$device" --report "$out/report.json" \
  --log "$out/nextpnr.log" --stat "$out/stat.json" --timing "$out/timing.txt" --utilization "$out/utilization.txt"

# Checks without a board (tools/openxc7-steps.sh: clocks, bitstream, round trip, block RAM, LVS).
fail=0
[ "$result_syn" = FAIL ] && fail=1
post_route_checks

[ "$dry" = 1 ] && exit 0

# ------------------------------------------------------------------ the manifest
# Format 2 (docs/OPEN-FLOW.md section 3): the netlist, FASM and placement by SHA-256, the LVS
# solver; a recheck (above) writes its own lines.
{
  echo "# openXC7 build manifest (tools/openxc7-build.sh); physical behaviour: pending (never on a board)"
  echo "manifest format     2 (docs/OPEN-FLOW.md section 3)"
  echo "top                 $top"
  echo "part                $part (device $device, chip database chipdb-$chipdie.bin)"
  echo "chip database       $chipdb sha256:$(sha "$chipdb") ($chipdb_from)"
  [ -n "$board" ] && echo "board               $board"
  echo "source revision     ${SURFCPU_BUILD_ID:-unknown} dirty=${SURFCPU_BUILD_DIRTY:-unknown}"
  echo "build host          ${SURFCPU_HOST:-unknown}, container $(uname -m)"
  echo "date (UTC)          $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "seed                $seed_used$( [ "$seed_used" = "$seeds" ] || echo " (of $seeds: $(grep -c '^seed ' "$out/seeds.txt") tried, seeds.txt)")"
  echo "pnr timeout         $pnr_timeout s (each seed's placement and routing)"
  echo "slack rule          each clock at least max(0.5 ns, 4 % of its period)${min_slack:+; the design list sets:$min_slack}"
  echo "DSP mode            $dsp (safe: multipliers in DSP48E1 without Yosys's register/adder packing)"
  echo "defines            ${defs:- (none)}"
  [ -n "$expect" ] && echo "expectations        $expect sha256:$(sha "$expect")"
  echo "xdc                 $xdc sha256:$(sha "$xdc")"
  for s in $srcs; do echo "source              $s sha256:$(sha "$s")"; done
  echo "bitstream           $out/$top.bit sha256:$(sha "$out/$top.bit") $(wc -c < "$out/$top.bit" | tr -d ' ') bytes"
  echo "netlist             $out/$top.json sha256:$(sha "$out/$top.json")"
  echo "routed FASM         $out/$top.fasm sha256:$(sha "$out/$top.fasm")"
  echo "placement           $out/placement.json sha256:$(sha "$out/placement.json")"
  echo "check SRL cells     $result_srl (none in the synthesized netlist, which LVS could not check: check-srl.txt)"
  echo "check synthesis     $result_syn (netlist against the RTL: flip-flops, memories, outputs: check-synthesis.txt)"
  echo "check bitstream     $result_bit (IDCODE, frames, commands, CRC: check-bitstream.txt)"
  echo "check round trip    $result_rt (bitstream decoded back to FASM: check-roundtrip.txt)"
  echo "check block RAM     $result_bram (contents decoded from the bitstream: check-bram.txt)"
  echo "check LVS           $result_lvs (routed netlist equivalent to the synthesis, lvs_equiv by the $lvs_solver route: check-lvs.txt)"
  echo "lvs solver          $lvs_solver ($(lvs_route_note))"
  echo "lvs timeout         $lvs_timeout s"
  echo "check clocks        $result_clk (MMCM/PLL dividers against XAPP888, no net through a global buffer: check-clocks.txt)"
  echo "check slack margin  $slack_state ($slack_note: slack-seed$seed_used.txt)"
  echo "## placement attempts"; cat "$out/seeds.txt"
  echo "## timing"; grep '^clock ' "$out/timing.txt" || true
  echo "## utilization"; sed -n '/documented capacity/,$p' "$out/utilization.txt"
  echo "## step times"; cat "$out/times.txt"
  echo "## tools (/etc/surfcpu-openxc7.txt)"; cat /etc/surfcpu-openxc7.txt
  for e in $extras; do echo "## included: $e"; cat "$e"; done
} > "$out/manifest.txt"

grep '^clock ' "$out/timing.txt" | sed 's/^/openxc7-build.sh: /'
echo "openxc7-build.sh: checks: SRL cells $result_srl, synthesis $result_syn, bitstream $result_bit, round trip $result_rt, block RAM $result_bram, LVS $result_lvs ($lvs_solver), clocks $result_clk, slack margin $slack_state"
[ "$slack_state" != THIN ] || echo "openxc7-build.sh: THIN: every placer seed met timing with less slack than the rule wants ($slack_note); make release refuses this bitstream without PRERELEASE=1" >&2
echo "openxc7-build.sh: $out/$top.bit ($(wc -c < "$out/$top.bit" | tr -d ' ') bytes); manifest $out/manifest.txt"
exit $fail
