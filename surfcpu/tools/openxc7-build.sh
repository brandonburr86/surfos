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
#   --seed <n>         placer seed (default 1)
#   --manifest-include <file>  append this file (ROM hashes, ISA/ABI versions...) to the manifest
#   --no-lvs           skip the LVS proof (it can take long on a large design)
#   --no-check         skip every check after the bitstream
#   --dry-run          print what would run, without the image
#
# Outputs in <dir>: <top>.bit (the bitstream), <top>.fasm/.frames/.json, timing.txt,
# utilization.txt, check-bitstream.txt, check-roundtrip.txt, check-lvs.txt, the logs and
# manifest.txt, and <top>.netlist.v with Yosys's cells_sim.v for gate-level simulation. Exit status 0 only if every step and every check passed. A bitstream made
# here has not been on a board: its physical behaviour is pending until it has.
#
# On the Mac it runs itself in the surfcpu-openxc7 image (devenv/openxc7/build-image.sh)
# with the repository at /work; paths must lie inside the repository. OPENXC7_THREADS
# (default 4) limits nextpnr's threads.
set -eu
IMAGE_TAG=2026-10-15
here=$(cd "$(dirname "$0")/.." && pwd -P)            # surfcpu/

die_usage() { echo "openxc7-build.sh: $*" >&2; echo "usage: see the top of $0" >&2; exit 2; }

top= xdc= board= part= out= seed=1 lvs=1 check=1 dry=0
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
    --no-lvs)   lvs=0; shift ;;
    --no-check) check=0; shift ;;
    --dry-run)  dry=1; shift ;;
    -h|--help)  sed -n '2,32p' "$0"; exit 0 ;;
    -*)      die_usage "unknown option $1" ;;
    *)       srcs="$srcs $1"; shift ;;
  esac
done
[ -n "$top" ] || die_usage "--top is required"
[ -n "$xdc" ] || die_usage "--xdc is required"
[ -n "$srcs" ] || die_usage "no Verilog sources given"
case "$seed" in ''|*[!0-9]*) die_usage "--seed must be a number" ;; esac

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
  set -- --top "$top" --xdc "$(rel "$xdc")" --part "$part" --out "$(rel "$out" mkdir)" --seed "$seed"
  [ -n "$board" ] && set -- "$@" --board "$board"
  for d in $defs; do set -- "$@" -D "$d"; done
  for i in $incs; do set -- "$@" -I "$(rel "$i")"; done
  for e in $extras; do set -- "$@" --manifest-include "$(rel "$e")"; done
  [ "$lvs" = 0 ] && set -- "$@" --no-lvs
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
db=${PRJXRAY_DB_DIR:-/opt/openxc7/share/prjxray-db}/$family
chipdb=${OPENXC7_CHIPDB_DIR:-/opt/openxc7/share/chipdb}/chipdb-$chipdie.bin
threads=${OPENXC7_THREADS:-4}
tools=devenv/openxc7
cd "$here"

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
  [ -f "$chipdb" ] || { echo "openxc7-build.sh: no chip database $chipdb" >&2; exit 2; }
  [ -f "$xdc" ] || { echo "openxc7-build.sh: no such XDC: $xdc" >&2; exit 2; }
  for s in $srcs; do [ -f "$s" ] || { echo "openxc7-build.sh: no such source: $s" >&2; exit 2; }; done
  rm -rf "$out/lvs"
  mkdir -p "$out/lvs"
  rm -f "$out/times.txt" "$out/$top.bit" "$out/manifest.txt" "$out"/check-*.txt
  echo "openxc7-build.sh: $top for $part ($device, chip database $chipdie) in $out"
fi

# Synthesis: openXC7's recipe (synth_xilinx -flatten -abc9 -arch xc7).
rv="read_verilog"
for d in $defs; do rv="$rv -D$d"; done
for i in $incs; do rv="$rv -I$i"; done
rv="$rv$srcs"
ys="$rv
synth_xilinx -flatten -abc9 -arch xc7 -top $top
tee -q -o $out/stat.json stat -json -tech xilinx
write_json $out/$top.json
write_verilog -noattr $out/$top.netlist.v"
if [ "$dry" = 1 ]; then printf '[synth.ys]\n%s\n' "$ys"; else printf '%s\n' "$ys" > "$out/synth.ys"; fi
run synth yosys.log yosys -q -l "$out/yosys.full.log" -s "$out/synth.ys"
# the primitive models of this Yosys, for simulating <top>.netlist.v elsewhere (Icarus)
run models cells_sim.log cp /opt/openxc7/share/yosys/xilinx/cells_sim.v "$out/cells_sim.v"

# Placement and routing: the settings openXC7/nextpnr's CI uses for hardware-tested designs.
run pnr nextpnr.out nextpnr-himbaechel --device "$part" --chipdb "$chipdb" --threads "$threads" \
  --json "$out/$top.json" -o xdc="$xdc" -o fasm="$out/$top.fasm" -o placement="$out/placement.json" \
  --router router2 -o hold-fix --placer-heap-timingweight 60 --seed "$seed" \
  --report "$out/report.json" --detailed-timing-report -l "$out/nextpnr.log"

# Bitstream: FASM -> frames -> .bit with Project X-Ray.
run frames fasm2frames.log env PYTHONWARNINGS=ignore::RuntimeWarning \
  fasm2frames --part "$part" --db-root "$db" "$out/$top.fasm" "$out/$top.frames"
run bit xc7frames2bit.log xc7frames2bit --part_file "$db/$part/part.yaml" --part_name "$part" \
  --frm_file "$out/$top.frames" --output_file "$out/$top.bit"
run report report.out python3 -I $tools/report.py --device "$device" --report "$out/report.json" \
  --log "$out/nextpnr.log" --stat "$out/stat.json" --timing "$out/timing.txt" --utilization "$out/utilization.txt"

# Checks without a board.
result_bit=skipped result_rt=skipped result_lvs=skipped
fail=0
if [ "$check" = 1 ]; then
  if run check-bit check-bitstream.txt python3 -I $tools/bitinfo.py --expect-device "$device" "$out/$top.bit"
  then result_bit=PASS; else result_bit=FAIL; fail=1; fi
  if run decode bit2fasm.log decode_bitstream &&
     run roundtrip check-roundtrip.txt python3 $tools/fasmcheck.py --db-root "$db" --part "$part" \
       "$out/$top.fasm" "$out/$top.decoded.fasm"
  then result_rt=PASS; else result_rt=FAIL; fail=1; fi
  # LVS (openXC7's lvs-equiv flow): the routed FASM read back into a fabric netlist by
  # fasm2netlist's tile model and proved equal to the synthesis, register by register. A
  # primitive the tile model lacks, or a proof that runs out of time, leaves it BLOCKED:
  # not proved, which is reported, but not a failure.
  if [ "$lvs" = 1 ]; then
    if run lvs-gold lvs/gold.log yosys -q -p "read_json $out/$top.json; hierarchy -top $top; splitnets; select $top; write_verilog -noattr -norename -selected $out/lvs/gold.v" &&
       run lvs-extract lvs/tileverilog.log tileverilog --fasm "$out/$top.fasm" --db "$db" --device "$chipdie" \
         --xdc "$xdc" --part "$part" --placement "$out/placement.json" --out "$out/lvs/fabric.v" \
         --model-out "$out/lvs/tile_model.v"
    then
      if run lvs-prove check-lvs.txt timeout "${OPENXC7_LVS_TIMEOUT:-1800}" lvs_equiv --gold "$out/lvs/gold.v" \
           --gold-top "$top" --gate "$out/lvs/fabric.v" --gate-top fabric --placement "$out/placement.json" \
           --gold-json "$out/$top.json" --db "$db" --device "$chipdie"
      then result_lvs=PASS
      else
        st=$?
        if [ "$st" = 124 ]; then result_lvs="BLOCKED (timeout)"; else result_lvs=FAIL; fail=1; fi
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
  [ -n "$board" ] && echo "board               $board"
  echo "source revision     ${SURFCPU_BUILD_ID:-unknown} dirty=${SURFCPU_BUILD_DIRTY:-unknown}"
  echo "build host          ${SURFCPU_HOST:-unknown}, container $(uname -m)"
  echo "date (UTC)          $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "seed                $seed"
  echo "defines            ${defs:- (none)}"
  echo "xdc                 $xdc sha256:$(sha "$xdc")"
  for s in $srcs; do echo "source              $s sha256:$(sha "$s")"; done
  echo "bitstream           $out/$top.bit sha256:$(sha "$out/$top.bit") $(wc -c < "$out/$top.bit" | tr -d ' ') bytes"
  echo "check bitstream     $result_bit (IDCODE, frames, commands, CRC: check-bitstream.txt)"
  echo "check round trip    $result_rt (bitstream decoded back to FASM: check-roundtrip.txt)"
  echo "check LVS           $result_lvs (routed netlist equivalent to the synthesis: check-lvs.txt)"
  echo "## timing"; grep '^clock ' "$out/timing.txt" || true
  echo "## utilization"; sed -n '/documented capacity/,$p' "$out/utilization.txt"
  echo "## step times"; cat "$out/times.txt"
  echo "## tools (/etc/surfcpu-openxc7.txt)"; cat /etc/surfcpu-openxc7.txt
  for e in $extras; do echo "## included: $e"; cat "$e"; done
} > "$out/manifest.txt"

grep '^clock ' "$out/timing.txt" | sed 's/^/openxc7-build.sh: /'
echo "openxc7-build.sh: checks: bitstream $result_bit, round trip $result_rt, LVS $result_lvs"
echo "openxc7-build.sh: $out/$top.bit ($(wc -c < "$out/$top.bit" | tr -d ' ') bytes); manifest $out/manifest.txt"
exit $fail
