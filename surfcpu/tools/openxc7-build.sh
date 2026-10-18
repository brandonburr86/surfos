#!/bin/sh
# SurfCPU open-source bitstream flow (openXC7; docs/OPEN-FLOW.md): Yosys synthesis, nextpnr
# placement and routing, Project X-Ray bitstream assembly, and the checks that need no board.
# This is the entry `make build-fpga` uses with FLOW=openxc7 (docs/BUILD-HOST.md, option C).
#
#   tools/openxc7-build.sh --top <module> --xdc <file> (--board <board> | --part <part>)
#                          [options] <source.v>...
#
#   --board arty_a7_35t | arty_a7_100t    the board, which sets the part
#   --part xc7a35tcsg324-1 | xc7a100tcsg324-1 | ...  the part (Artix-7 xc7a35t/50t/100t)
#   --out <dir>        where everything goes (default build/openxc7/<top>-<board or part>)
#   -D NAME[=VALUE]    a Verilog define for synthesis (repeatable)
#   -I <dir>           an include directory for synthesis (repeatable)
#   --seed <n>[,<n>...]  placer seed (default 1); a list: placement and routing with each seed in
#                      turn until one routes with every clock meeting its constraint (the gate is
#                      the same for each; every attempt is recorded in seeds.txt and the manifest)
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
#   --no-check         skip every check after the bitstream
#   --dry-run          print what would run, without the image
#
# Outputs in <dir>: <top>.bit (the bitstream), <top>.fasm/.frames/.json, timing.txt,
# utilization.txt, check-srl.txt (a netlist with SRL cells stops here), check-synthesis.txt,
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

top= xdc= board= part= out= seed=1 lvs=1 check=1 dry=0 dsp=safe expect= synthonly=0
lvs_timeout=${OPENXC7_LVS_TIMEOUT:-1800}
defs= incs= srcs= extras=
while [ $# -gt 0 ]; do
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
    --no-check) check=0; shift ;;
    --dry-run)  dry=1; shift ;;
    -h|--help)  awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"; exit 0 ;;
    -*)      die_usage "unknown option $1" ;;
    *)       srcs="$srcs $1"; shift ;;
  esac
done
[ -n "$top" ] || die_usage "--top is required"
[ -n "$xdc" ] || [ "$synthonly" = 1 ] || die_usage "--xdc is required"
case "$dsp" in safe|full|none) ;; *) die_usage "--dsp must be safe, full or none" ;; esac
[ -n "$srcs" ] || die_usage "no Verilog sources given"
case "$seed" in ''|*[!0-9,]*|,*|*,|*,,*) die_usage "--seed must be a number or a list of numbers (2,3,4)" ;; esac
seeds=$(echo "$seed" | tr ',' ' ')
case "$lvs_timeout" in ''|*[!0-9]*) die_usage "--lvs-timeout must be a number of seconds" ;; esac

if [ -z "$part" ]; then
  case "$board" in
    arty_a7_35t)  part=xc7a35tcsg324-1 ;;   # the board's XC7A35TICSG324-1L: same die and
                                             # bitstream; the -1L timing is not modelled
    arty_a7_100t) part=xc7a100tcsg324-1 ;;
    "") die_usage "--board or --part is required" ;;
    *)  die_usage "unknown board $board (arty_a7_35t, arty_a7_100t)" ;;
  esac
fi
case "$part" in
  xc7a35t*)  device=xc7a35t;  chipdie=xc7a50t ;;      # the 35T is the 50T die (prjxray-db)
  xc7a50t*)  device=xc7a50t;  chipdie=xc7a50t ;;
  xc7a100t*) device=xc7a100t; chipdie=xc7a100t ;;
  *) die_usage "part $part is not supported by this image (xc7a35t, xc7a50t, xc7a100t)" ;;
esac
family=artix7
[ -n "$out" ] || out=build/openxc7/$top-${board:-$part}

# ------------------------------------------------------------------ on the Mac: into the image
if [ "${SURFCPU_OPENXC7:-0}" != 1 ] && [ "$dry" = 0 ]; then
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
  set -- "$@" --lvs-timeout "$lvs_timeout"
  [ "$check" = 0 ] && set -- "$@" --no-check
  for s in $srcs; do set -- "$@" "$(rel "$s")"; done
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
if [ -n "${SURFCPU_CHIPDB_DIR:-}" ]; then
  chipdb=$SURFCPU_CHIPDB_DIR/chipdb-$chipdie.bin
  chipdb_from="SURFCPU_CHIPDB_DIR, as it is"
else
  chipdb=build/openxc7/chipdb/$IMAGE_TAG/chipdb-$chipdie.bin
  chipdb_from="the image's generator without BUFGCTRL route-throughs, devenv/openxc7/chipdb-gen.py"
fi
threads=${OPENXC7_THREADS:-4}
tools=devenv/openxc7
cd "$here"

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

decode_bitstream() {   # Project X-Ray's bit2fasm: the finished bitstream back to FASM
  PYTHONWARNINGS=ignore::RuntimeWarning bit2fasm --db-root "$db" --part "$part" \
    --bitread "$(command -v bitread)" "$out/$top.bit" > "$out/$top.decoded.fasm"
}

run() {   # run <step> <log> <command...>: time it, keep its output, stop on failure
  step=$1 log=$2; shift 2
  if [ "$dry" = 1 ]; then echo "[$step] $*"; return 0; fi
  t0=$(date +%s.%N)
  if "$@" > "$out/$log" 2>&1; then status=0; else status=$?; fi
  t1=$(date +%s.%N)
  awk -v s="$step" -v a="$t0" -v b="$t1" -v r="$([ $status = 0 ] && echo ok || echo "FAILED ($status)")" \
    'BEGIN { printf "%-12s %8.2f s  %s\n", s, b - a, r }' >> "$out/times.txt"
  if [ $status != 0 ]; then
    echo "openxc7-build.sh: $step failed (exit $status); the end of $out/$log:" >&2
    tail -n 25 "$out/$log" >&2
    return $status
  fi
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

# Placement and routing: the settings openXC7/nextpnr's CI uses for hardware-tested designs.
# With a list of seeds, each in turn until nextpnr succeeds: it fails when routing does not
# converge or a clock misses its constraint after routing, the same gate for every seed. Each
# attempt's log is kept (nextpnr-seed<n>.log) and summarized in seeds.txt; nextpnr.log is the
# last attempt's, the one that succeeded if any did.
pnr_summary() {   # SEED STATUS: one line on that attempt, from nextpnr.log
  awk -v s="$1" -v st="$2" '/Max frequency for clock/ { c = $0; sub(/.*clock +/, "", c); sub(/:.*/, "", c)
        l = $0; sub(/^(Info|ERROR|Warning): */, "", l); sub(/^Max frequency for clock +/, "", l); gsub(/ +/, " ", l)
        last[c] = l; if (!(c in seen)) { seen[c] = 1; order[n++] = c } }
      /Routing complete/ { routed = 1 }
      END { printf "seed %s: %s, %s;", s, st == 0 ? "pass" : "fail (nextpnr exit " st ")", routed ? "routed" : "not routed"
            for (i = 0; i < n; i++) printf " %s%s", last[order[i]], i < n - 1 ? "," : ""
            printf "\n" }' "$out/nextpnr.log"
}
if [ -z "${SURFCPU_CHIPDB_DIR:-}" ] && [ "$dry" = 1 ]; then
  echo "[chipdb] python3 $tools/chipdb-gen.py --xray $db --device $chipdie --bba /tmp/chipdb-$chipdie.bba;" \
       "bbasm -l /tmp/chipdb-$chipdie.bba $chipdb (when it is not there yet; then its sha256 against $tools/chipdb.sha256)"
elif [ -z "${SURFCPU_CHIPDB_DIR:-}" ]; then
  run chipdb chipdb.log make_chipdb
  run chipdb-sha chipdb-sha.txt check_chipdb ||
    { echo "openxc7-build.sh: the chip database is not the one this image's generator makes (update devenv/openxc7/chipdb.sha256 only on purpose)" >&2; exit 1; }
fi
pnr_ok=0
for seed in $seeds; do
  if run pnr nextpnr.out nextpnr-himbaechel --device "$part" --chipdb "$chipdb" --threads "$threads" \
       --json "$out/$top.json" -o xdc="$xdc" -o fasm="$out/$top.fasm" -o placement="$out/placement.json" \
       --router router2 -o hold-fix --placer-heap-timingweight 60 --seed "$seed" \
       --report "$out/report.json" --detailed-timing-report -l "$out/nextpnr.log"
  then st=0; else st=$?; fi
  [ "$dry" = 1 ] && { pnr_ok=1; break; }
  cp "$out/nextpnr.log" "$out/nextpnr-seed$seed.log"
  pnr_summary "$seed" "$st" | tee -a "$out/seeds.txt" | sed 's/^/openxc7-build.sh: placement and routing, /'
  [ "$st" = 0 ] && { pnr_ok=1; break; }
done
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

# Checks without a board.
result_bit=skipped result_rt=skipped result_lvs=skipped result_clk=skipped result_bram=skipped
fail=0
[ "$result_syn" = FAIL ] && fail=1
if [ "$check" = 1 ]; then
  # The clock managers' dividers against AMD's XAPP888 (LVS does not model MMCM/PLL).
  if run check-clk check-clocks.txt python3 $tools/cmtcheck.py --json "$out/$top.json" \
       --placement "$out/placement.json" --fasm "$out/$top.fasm"
  then
    if [ "$dry" = 0 ] && grep -q 'no MMCME2/PLLE2' "$out/check-clocks.txt"; then result_clk="none in the design"; else result_clk=PASS; fi
  else result_clk=FAIL; fail=1; fi
  # No ordinary net on the global clock network: no net through an unconfigured BUFGCTRL, no
  # clock line into a data input (docs/OPEN-FLOW.md 6.8). Its report goes into check-clocks.txt.
  if run check-gclk check-gclk.txt python3 -I $tools/gclkcheck.py --fasm "$out/$top.fasm"
  then :
  else result_clk=FAIL; fail=1; fi
  [ "$dry" = 1 ] || { echo; cat "$out/check-gclk.txt"; } >> "$out/check-clocks.txt"
  if run check-bit check-bitstream.txt python3 -I $tools/bitinfo.py --expect-device "$device" "$out/$top.bit"
  then result_bit=PASS; else result_bit=FAIL; fail=1; fi
  if run decode bit2fasm.log decode_bitstream &&
     run roundtrip check-roundtrip.txt python3 $tools/fasmcheck.py --db-root "$db" --part "$part" \
       "$out/$top.fasm" "$out/$top.decoded.fasm"
  then result_rt=PASS; else result_rt=FAIL; fail=1; fi
  # Block RAM contents: fasm2netlist reads them out of the decoded bitstream; they must be
  # the synthesized netlist's, block for block (LVS checks block RAMs only at their pins).
  if [ "$dry" = 1 ] || grep -Eq '"type": "RAMB(18|36)E1"' "$out/$top.json"; then
    if run bram-extract bram-extract.log fasm2netlist --fasm "$out/$top.decoded.fasm" --db "$dbroot" \
         --family "$family" --device "$chipdie" --out "$out/bram-extracted.v" &&
       run check-bram check-bram.txt python3 -I $tools/bramcheck.py --netlist "$out/$top.json" --top "$top" \
         --extracted "$out/bram-extracted.v"
    then result_bram=PASS; else result_bram=FAIL; fail=1; fi
  else
    result_bram="none in the design"
  fi
  # LVS (openXC7's lvs-equiv flow): the routed FASM read back into a fabric netlist by
  # fasm2netlist's tile model and proved equal to the synthesis, register by register. A
  # primitive the tile model lacks, or a proof that runs out of time, leaves it BLOCKED:
  # not proved, which is reported, but not a failure. Between extraction and proof,
  # bramwe.py makes the netlist read each RAMB18E1 byte write enable from the site pins the
  # silicon reads it from (the extractor reads them one to one; docs/OPEN-FLOW.md) and checks
  # every pin it no longer reads: a difference it finds fails LVS like one the proof finds.
  if [ "$lvs" = 1 ]; then
    if run lvs-gold lvs/gold.log yosys -q -p "read_json $out/$top.json; hierarchy -top $top; splitnets; select $top; write_verilog -noattr -norename -selected $out/lvs/gold.v" &&
       run lvs-extract lvs/tileverilog.log tileverilog --fasm "$out/$top.fasm" --db "$db" --device "$chipdie" \
         --xdc "$xdc" --part "$part" --placement "$out/placement.json" --out "$out/lvs/fabric.v" \
         --model-out "$out/lvs/tile_model.v" &&
       run lvs-bramwe lvs/bramwe.log python3 -I $tools/bramwe.py --fabric "$out/lvs/fabric.v" \
         --fasm "$out/$top.fasm" --report "$out/lvs/bramwe.txt"
    then
      if run lvs-prove check-lvs.txt timeout "$lvs_timeout" lvs_equiv --gold "$out/lvs/gold.v" \
           --gold-top "$top" --gate "$out/lvs/fabric.v" --gate-top fabric --placement "$out/placement.json" \
           --gold-json "$out/$top.json" --db "$db" --device "$chipdie"
      then result_lvs=PASS
      else
        st=$?
        if [ "$st" = 124 ]; then result_lvs="BLOCKED (timeout)"; else result_lvs=FAIL; fail=1; fi
      fi
      if [ "$dry" = 0 ]; then
        cat "$out/lvs/bramwe.txt" >> "$out/check-lvs.txt"
        bram_diff=$(sed -n 's/^bramwe: .* \([0-9][0-9]*\) difference(s)$/\1/p' "$out/lvs/bramwe.txt")
        if [ "${bram_diff:-0}" != 0 ]; then result_lvs="FAIL ($bram_diff block RAM write enable difference(s))"; fail=1; fi
      fi
    else
      result_lvs="BLOCKED (extraction: see lvs/tileverilog.log)"
    fi
  fi
fi

[ "$dry" = 1 ] && exit 0

# ------------------------------------------------------------------ the manifest
sha() { sha256sum "$1" | cut -d' ' -f1; }
{
  echo "# openXC7 build manifest (tools/openxc7-build.sh); physical behaviour: pending (never on a board)"
  echo "top                 $top"
  echo "part                $part (device $device, chip database chipdb-$chipdie.bin)"
  echo "chip database       $chipdb sha256:$(sha "$chipdb") ($chipdb_from)"
  [ -n "$board" ] && echo "board               $board"
  echo "source revision     ${SURFCPU_BUILD_ID:-unknown} dirty=${SURFCPU_BUILD_DIRTY:-unknown}"
  echo "build host          ${SURFCPU_HOST:-unknown}, container $(uname -m)"
  echo "date (UTC)          $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "seed                $seed$( [ "$seed" = "$seeds" ] || echo " (of $seeds: $(grep -c . "$out/seeds.txt") tried, seeds.txt)")"
  echo "DSP mode            $dsp (safe: multipliers in DSP48E1 without Yosys's register/adder packing)"
  echo "defines            ${defs:- (none)}"
  [ -n "$expect" ] && echo "expectations        $expect sha256:$(sha "$expect")"
  echo "xdc                 $xdc sha256:$(sha "$xdc")"
  for s in $srcs; do echo "source              $s sha256:$(sha "$s")"; done
  echo "bitstream           $out/$top.bit sha256:$(sha "$out/$top.bit") $(wc -c < "$out/$top.bit" | tr -d ' ') bytes"
  echo "check SRL cells     $result_srl (none in the synthesized netlist, which LVS could not check: check-srl.txt)"
  echo "check synthesis     $result_syn (netlist against the RTL: flip-flops, memories, outputs: check-synthesis.txt)"
  echo "check bitstream     $result_bit (IDCODE, frames, commands, CRC: check-bitstream.txt)"
  echo "check round trip    $result_rt (bitstream decoded back to FASM: check-roundtrip.txt)"
  echo "check block RAM     $result_bram (contents decoded from the bitstream: check-bram.txt)"
  echo "check LVS           $result_lvs (routed netlist equivalent to the synthesis: check-lvs.txt)"
  echo "lvs timeout         $lvs_timeout s"
  echo "check clocks        $result_clk (MMCM/PLL dividers against XAPP888, no net through a global buffer: check-clocks.txt)"
  echo "## placement attempts"; cat "$out/seeds.txt"
  echo "## timing"; grep '^clock ' "$out/timing.txt" || true
  echo "## utilization"; sed -n '/documented capacity/,$p' "$out/utilization.txt"
  echo "## step times"; cat "$out/times.txt"
  echo "## tools (/etc/surfcpu-openxc7.txt)"; cat /etc/surfcpu-openxc7.txt
  for e in $extras; do echo "## included: $e"; cat "$e"; done
} > "$out/manifest.txt"

grep '^clock ' "$out/timing.txt" | sed 's/^/openxc7-build.sh: /'
echo "openxc7-build.sh: checks: SRL cells $result_srl, synthesis $result_syn, bitstream $result_bit, round trip $result_rt, block RAM $result_bram, LVS $result_lvs, clocks $result_clk"
echo "openxc7-build.sh: $out/$top.bit ($(wc -c < "$out/$top.bit" | tr -d ' ') bytes); manifest $out/manifest.txt"
exit $fail
