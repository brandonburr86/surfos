#!/bin/sh
# SurfCPU: is the LiteDRAM generation reproducible whatever the Python process? (make litedram-repro)
# File: repro.sh Date: 10/16/26 (SurfCPU)
#
# Inside the toolchain container, from surfcpu/, after make litedram-gen. Generates the board
# core and the simulation core from ip/litedram/arty.yml again in eight separate Python
# processes, each different in what moves LiteX's objects in memory: PYTHONHASHSEED (0, 1, 12345,
# random, ...), the environment (generate.sh's env -i, a 2000-character variable added, the
# container's whole environment), the length and depth of the output path, the working
# directory. Every run goes through ip/litedram/run_gen.py twice: as generate.sh runs it, and with
# SURFCPU_LITEDRAM_KEEP_TREE=1, which keeps LiteX's instance tree comment (the control). Passes
# only if the runs without the comment give one set of files, the ones make litedram-gen wrote
# and ip/litedram/arty.sha256 records, and the Verilog outside that comment is the same in every
# run of either kind. Writes build/litedram/repro/ (about 6 minutes).
set -u
top=$PWD
. build/third_party/litedram/env.sh
sde=$(git -C build/third_party/litedram/src/litedram log -1 --format=%ct)
cfg=$top/ip/litedram/arty.yml
gen=$top/ip/litedram/run_gen.py
base=build/litedram/repro
rm -rf "$base"
mkdir -p "$base"
long=pppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppp
pad=$(printf '%2000s' | tr ' ' p)
fail=0
run() {          # DIR SEED MODE KEEP [--sim]: one generation into $base/DIR
    dir=$1 seed=$2 mode=$3 keep=$4; shift 4
    out=$top/$base/$dir
    mkdir -p "$out"
    name=litedram_core
    [ "${1:-}" != --sim ] || name=litedram_sim
    set -- python3 "$gen" "$cfg" --name "$name" --output-dir "$out" "$@"
    p="$top/build/third_party/litedram/bin:/usr/local/bin:/usr/bin:/bin"
    case "$mode" in
      envi)    env -i PATH="$p" PYTHONPATH="$PYTHONPATH" HOME=/tmp LANG=C.UTF-8 TZ=UTC PYTHONHASHSEED="$seed" \
                   SOURCE_DATE_EPOCH="$sde" SURFCPU_LITEDRAM_KEEP_TREE="$keep" "$@" ;;
      bigenv)  env -i PATH="$p" PYTHONPATH="$PYTHONPATH" HOME=/tmp LANG=C.UTF-8 TZ=UTC PYTHONHASHSEED="$seed" \
                   SOURCE_DATE_EPOCH="$sde" SURFCPU_LITEDRAM_KEEP_TREE="$keep" PAD="$pad" "$@" ;;
      fullenv) PYTHONHASHSEED="$seed" SOURCE_DATE_EPOCH="$sde" TZ=UTC SURFCPU_LITEDRAM_KEEP_TREE="$keep" "$@" ;;
      cwd)     (cd /tmp && env -i PATH="$p" PYTHONPATH="$PYTHONPATH" HOME=/tmp LANG=C.UTF-8 TZ=UTC \
                   PYTHONHASHSEED="$seed" SOURCE_DATE_EPOCH="$sde" SURFCPU_LITEDRAM_KEEP_TREE="$keep" "$@") ;;
    esac > "$out.log" 2>&1 || { echo "repro: $dir: the generator failed (log: $base/$dir.log)"; fail=1; }
}
i=0
for v in "0 a envi" "1 $long envi" "12345 b bigenv" "random c fullenv" "777 d/e/f/g/h envi" \
         "4242 i cwd" "99 $long/$long bigenv" "31337 j envi"; do
    set -- $v
    i=$((i + 1))
    run "run$i/$2" "$1" "$3" 0
    run "keep$i/$2" "$1" "$3" 1
done
run sim1 5 envi 0 --sim
run "sim2/$long/$long" 6 bigenv 0 --sim
run keepsim1 5 envi 1 --sim
run "keepsim2/$long/$long" 6 bigenv 1 --sim

# one line per file kind: how many different files the runs gave
files="gateware/litedram_core.v gateware/litedram_core.xdc csr.csv software/include/generated/sdram_phy.h"
count() {        # PATTERN FILE: distinct contents of FILE in the runs whose directory matches PATTERN
    find "$base" -path "$base/$1/*" -path "*/$2" -type f -exec sha256sum {} + | cut -d' ' -f1 | sort -u | wc -l | tr -d ' '
}
for f in $files; do
    n=$(count 'run[0-9]*' "$f")
    k=$(count 'keep[0-9]*' "$f")
    ref=$(sha256sum "build/litedram/arty/$f" | cut -d' ' -f1)
    same=$(find "$base" -path "$base/run[0-9]*/*" -path "*/$f" -type f -exec sha256sum {} + | cut -d' ' -f1 | grep -c "^$ref$")
    echo "repro: board core $f: 8 runs give $n file(s), $same of them make litedram-gen's ($(echo "$ref" | cut -c1-16)...); with the tree comment kept: $k"
    [ "$n" = 1 ] && [ "$same" = 8 ] || fail=1
done
n=$(count 'sim[0-9]*' gateware/litedram_sim.v)
k=$(count 'keepsim[0-9]*' gateware/litedram_sim.v)
ref=$(sha256sum build/litedram/arty_sim/gateware/litedram_sim.v | cut -d' ' -f1)
same=$(find "$base" -path "$base/sim[0-9]*/*" -name litedram_sim.v -type f -exec sha256sum {} + | cut -d' ' -f1 | grep -c "^$ref$")
echo "repro: simulation core gateware/litedram_sim.v: 2 runs give $n file(s), $same of them make litedram-gen's; with the tree comment kept: $k"
[ "$n" = 1 ] && [ "$same" = 2 ] || fail=1

# the Verilog outside the tree comment: the same in every run, with or without the comment
python3 - "$base" <<'EOF' || fail=1
import pathlib, re, sys
tree = re.compile(r"\n// Hierarchy\n//-+\n\n/\*\n.*?\n\*/\n", re.S)
for name in ("litedram_core.v", "litedram_sim.v"):
    texts = {tree.sub("", p.read_text(encoding="utf-8")) for p in pathlib.Path(sys.argv[1]).rglob(name)}
    print(f"repro: {name} outside the tree comment: {len(texts)} version(s) over every run")
    if len(texts) != 1:
        sys.exit(1)
EOF
ip/litedram/lock.sh check || fail=1
echo "repro: $( [ $fail = 0 ] && echo PASS || echo FAIL)"
exit $fail
