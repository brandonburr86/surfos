#!/bin/sh
# SurfCPU: build a bitstream on the configured build host (docs/BUILD-HOST.md, docs/BOARD.md section 8).
#
#   tools/fpga-build.sh [--dry-run] [--design NAME] BOARD
#   tools/fpga-build.sh --fetch [--design NAME] BOARD    the results of the last vivado-ssh run
#   make build-fpga BOARD=arty_a7_100t [DESIGN=arty_a7_baseline] [DRY_RUN=1]
#
# BOARD is a line of boards/arty_a7/boards.txt (arty_a7_35t, arty_a7_100t). The design is
# surfcpu_arty_a7_top unless --design or $DESIGN names another boards/arty_a7/designs/*.files.
# The build host comes from ~/.config/surfcpu/build-host.conf (never committed; tests point
# SURFCPU_BUILD_HOST_CONF elsewhere):
#   FLOW=vivado-ssh      HOST (an ssh alias using key authentication), REMOTE_DIR, VIVADO
#   FLOW=vivado-docker   VIVADO, IMAGE (an x86-64 Linux image with Vivado), DOCKER (optional)
#   FLOW=openxc7         the flow track's tools/openxc7-build.sh
#
# It stages the design's sources and constraints, boards/arty_a7/vivado/build.tcl and a
# params.tcl in build/fpga/BOARD/DESIGN/stage, runs the Vivado non-project flow on the host
# (over ssh, or in Docker), brings back out/ (bitstream, reports, vivado.log, build-results.txt)
# and writes build/fpga/BOARD/DESIGN/manifest.txt (SURFCPU_FPGA_OUT replaces build/fpga). Exit 0
# only with a bitstream whose checks passed; a new build first removes the previous one's
# bitstream and results. --dry-run stages locally and prints every command it would run,
# without contacting a host or starting Docker, and leaves the last build's results alone.
# No passwords anywhere: ssh uses your keys, checks the host's key strictly (known_hosts), gives
# up on a silent connection after two minutes. Vivado runs on the host under nohup, with its
# own log and exit status in out/, so a dropped connection does not stop it: --fetch brings
# back its results later (exit 3: connection lost, try --fetch; 4: still running).
set -eu

root=$(cd "$(dirname "$0")/.." && pwd -P)              # surfcpu/
me=fpga-build

die() { echo "$me: $*" >&2; exit 1; }

case "${DRY_RUN:-0}" in                                # make build-fpga DRY_RUN=1
    0|'') dry=0 ;;
    1)    dry=1 ;;
    *)    die "DRY_RUN must be 0 or 1" ;;
esac
design=${DESIGN:-surfcpu_arty_a7_top}
board=
fetch=0
while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run) dry=1 ;;
        --fetch)   fetch=1 ;;
        --design)  [ $# -ge 2 ] || die "--design needs a name"; design=$2; shift ;;
        -h|--help) sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*)        die "unknown option $1" ;;
        *)         [ -z "$board" ] || die "one BOARD only"; board=$1 ;;
    esac
    shift
done
[ -n "$board" ] || die "usage: $0 [--dry-run] [--design NAME] BOARD"

# ------------------------------------------------------------------------------ board, design
line=$(awk -v b="$board" '$1 == b { print }' "$root/boards/arty_a7/boards.txt")
[ -n "$line" ] || die "unknown BOARD '$board' (boards/arty_a7/boards.txt: $(awk '!/^#/ && NF { printf "%s ", $1 }' "$root/boards/arty_a7/boards.txt"))"
part=$(echo "$line" | awk '{ print $2 }')
case "$design" in *[!A-Za-z0-9_]*|'') die "bad design name '$design'" ;; esac
list="$root/boards/arty_a7/designs/$design.files"
[ -f "$list" ] || die "unknown design '$design' (boards/arty_a7/designs/)"
fpga_out=${SURFCPU_FPGA_OUT:-$root/build/fpga}
base="$fpga_out/$board/$design"
stage="$base/stage"
manifest="$base/manifest.txt"
head="$base/manifest-head.txt"          # a real run's keys known before it; finish() adds the results

# ------------------------------------------------------------------------------ build host
conf=${SURFCPU_BUILD_HOST_CONF:-$HOME/.config/surfcpu/build-host.conf}
if [ ! -f "$conf" ]; then
    echo "$me: no build host configured ($conf): bitstream generation is pending." >&2
    echo "$me: docs/BUILD-HOST.md describes the options and the file." >&2
    exit 2
fi
conf_get() {      # KEY: the last KEY=value line, without comments or surrounding quotes
    sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*//p" "$conf" | tail -n 1 |
        sed -e 's/[[:space:]]*#.*$//' -e 's/[[:space:]]*$//' -e 's/^"\(.*\)"$/\1/' -e "s/^'\(.*\)'\$/\1/"
}
FLOW=$(conf_get FLOW)
HOST=$(conf_get HOST)
REMOTE_DIR=$(conf_get REMOTE_DIR)
VIVADO=$(conf_get VIVADO)
IMAGE=$(conf_get IMAGE)
DOCKER=$(conf_get DOCKER)
safe() {          # NAME VALUE CHARS: refuse values that a shell or ssh could misread
    case "$2" in
        '') die "$conf: $1 is not set" ;;
        -*) die "$conf: $1 must not start with '-'" ;;
        *[!$3]*) die "$conf: $1='$2' has characters outside [$3]" ;;
    esac
}
case "$FLOW" in
    vivado-ssh)
        safe HOST "$HOST" 'A-Za-z0-9._@-'
        safe REMOTE_DIR "${REMOTE_DIR:-surfcpu-build}" 'A-Za-z0-9._/~-'
        REMOTE_DIR=${REMOTE_DIR:-surfcpu-build}
        safe VIVADO "$VIVADO" 'A-Za-z0-9._/~+-' ;;
    vivado-docker)
        safe VIVADO "$VIVADO" 'A-Za-z0-9._/~+-'
        safe IMAGE "$IMAGE" 'A-Za-z0-9._/:@-'
        DOCKER=${DOCKER:-docker}
        safe DOCKER "$DOCKER" 'A-Za-z0-9._/~+-' ;;
    openxc7)
        [ -x "$root/tools/openxc7-build.sh" ] ||
            die "FLOW=openxc7 needs tools/openxc7-build.sh (the flow track, docs/OPEN-FLOW.md), not on this branch"
        [ "$dry" = 1 ] && exec "$root/tools/openxc7-build.sh" --dry-run "$board" "$design"
        exec "$root/tools/openxc7-build.sh" "$board" "$design" ;;
    '') die "$conf: FLOW is not set (vivado-ssh, vivado-docker or openxc7)" ;;
    *)  die "$conf: unknown FLOW '$FLOW' (vivado-ssh, vivado-docker or openxc7)" ;;
esac
# Key authentication only, the host key must already be known (no trust on first use), and a
# connection that goes quiet is given up after 20 s to connect or 4 x 30 s without an answer.
ssh_opts="-o BatchMode=yes -o StrictHostKeyChecking=yes -o ConnectTimeout=20 -o ServerAliveInterval=30 -o ServerAliveCountMax=4"

# ------------------------------------------------------------------------------ results (functions)
sha() { shasum -a 256 "$@" | awk '{ print $1 }'; }
# The manifest: the keys known before the run, the outcome (result, and the bitstream's SHA-256
# and size, which tools/program.sh compares with the file), then the flow's own results.
finish() {        # RESULT REASON SECTION FILE: write the final manifest
    {
        cat "$head"
        echo "result = \"$1\""
        if [ "$1" = pass ]; then
            echo "bitstream_sha256 = \"$(sha "$base/$design.bit")\""
            echo "bitstream_bytes = $(wc -c < "$base/$design.bit" | tr -d ' ')"
        else
            echo "reason = \"$(printf '%s' "$2" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g')\""
        fi
        echo
        echo "[$3]"
        cat "$4"
    } > "$manifest.tmp"
    mv "$manifest.tmp" "$manifest"
}
vivado_results() {   # STATUS: gate on Vivado's build-results.txt, write the manifest, exit
    results="$base/out/build-results.txt"
    [ -f "$results" ] || die "Vivado left no build-results.txt (exit status $1; see ${base#$root/}/out/vivado.log)"
    if grep -q '^result = "pass"' "$results" && [ -f "$base/out/$design.bit" ] && [ "$1" = 0 ]; then
        cp "$base/out/$design.bit" "$base/$design.bit"
        finish pass "" vivado "$results"
        echo "$me: PASS: ${base#$root/}/$design.bit"
        grep -E '^(wns_ns|whs_ns|lut|ff|bram_tiles|dsp) ' "$results" | sed "s/^/$me:   /"
        exit 0
    fi
    why=$(sed -n 's/^reason = "\(.*\)"$/\1/p' "$results")
    finish fail "Vivado exit status $1${why:+: $why}" vivado "$results"
    echo "$me: FAIL (Vivado exit status $1): $why" >&2
    echo "$me: reports in ${base#$root/}/out/reports, log ${base#$root/}/out/vivado.log" >&2
    exit 1
}
fetch_ssh() {     # bring back out/ from $HOST:$remote once Vivado has finished there; sets status
    # shellcheck disable=SC2086
    if ssh $ssh_opts "$HOST" "test -f $remote/out/vivado.exit"; then rc=0; else rc=$?; fi
    case $rc in
        0) ;;
        1)  # shellcheck disable=SC2086
            if ssh $ssh_opts "$HOST" "kill -0 \$(cat $remote/out/vivado.pid 2>/dev/null) 2>/dev/null"; then
                echo "$me: Vivado is still running on $HOST ($remote); run --fetch again later" >&2
                exit 4
            fi
            die "Vivado is not running on $HOST and left no exit status in $remote/out (it did not start, or was stopped): start a new build" ;;
        *)  echo "$me: cannot reach $HOST (ssh exit $rc); run --fetch again later" >&2
            exit 3 ;;
    esac
    rm -rf "${base:?}/out" "${base:?}/${design:?}.bit"
    # shellcheck disable=SC2086
    ssh $ssh_opts "$HOST" "cd $remote && tar cf - --exclude='*.dcp' out" | tar xf - -C "$base" ||
        { echo "$me: could not fetch the results from $HOST:$remote; run --fetch again" >&2; exit 3; }
    status=$(cat "$base/out/vivado.exit" 2>/dev/null || echo unknown)
    case "$status" in ''|*[!0-9]*) status=unknown ;; esac
}

# ------------------------------------------------------------------------------ --fetch
if [ "$fetch" = 1 ]; then
    [ "$FLOW" = vivado-ssh ] || die "--fetch is for FLOW=vivado-ssh (a Vivado run on a remote host)"
    [ -f "$head" ] || die "no build of $design for $board was started here (${head#$root/} is missing)"
    run_id=$(sed -n 's/^run_id = "\(.*\)"$/\1/p' "$head")
    was=$(sed -n 's/^build_host_alias = "\(.*\)"$/\1/p' "$head")
    [ "$was" = "$HOST" ] || die "the last build of $design for $board ran on '$was', but $conf names '$HOST'"
    remote="$REMOTE_DIR/$run_id"
    echo "$me: fetching $run_id from $HOST:$remote"
    fetch_ssh
    vivado_results "$status"
fi

# ------------------------------------------------------------------------------ the design's files
items() {         # KIND: the lines of that kind, without the kind word
    awk -v k="$1" '{ sub(/#.*/, "") } $1 == k { $1 = ""; sub(/^ +/, ""); print }' "$list"
}
expand() {        # PATTERN: matching paths relative to surfcpu/, sorted
    ( cd "$root" && for f in $1; do [ -e "$f" ] && echo "$f"; done ) | sort
}
set -f                                                  # patterns are expanded in expand() only
top=$(items top)
[ -n "$top" ] || die "$list has no top"
for r in $(items require); do
    [ -f "$root/$r" ] || die "$design needs $r, which is not on this branch yet; refusing to build (the simulation stand-in sim/board/stubs/surfcpu_soc.v is never built into a bitstream)"
done
sources=
for pat in $(items src); do
    found=$(set +f; expand "$pat")
    [ -n "$found" ] || die "$design: no source matches $pat"
    sources="$sources $found"
done
for pat in $(items 'src?'); do
    sources="$sources $(set +f; expand "$pat")"
done
incdirs=$(items inc)
for d in $incdirs; do [ -d "$root/$d" ] || die "$design: include directory $d is missing"; done
incfiles=
for d in $incdirs; do incfiles="$incfiles $(set +f; expand "$d/*.vh"; expand "$d/*.svh")"; done
mems=
for pat in $(items mem); do mems="$mems $(set +f; expand "$pat")"; done
xdcs=$(items xdc)
for x in $xdcs; do [ -f "$root/$x" ] || die "$design: constraint file $x is missing"; done
cdcs=$(items cdc)
cdc_files=$(echo "$cdcs" | awk 'NF == 2 { print $2 }')
for x in $cdc_files; do [ -f "$root/$x" ] || die "$design: constraint file $x is missing"; done
generics=$(items generic)
set +f

rom=
case " $generics " in
    *" BOOTROM_INIT "*)
        romsrc=${ROM:-$root/build/fw/surfboot.mem}
        [ -f "$romsrc" ] || die "$design needs the boot ROM image $romsrc (make build-rom, the fw track)"
        rom=rom/surfboot.mem ;;
esac

# ------------------------------------------------------------------------------ build identity
# Clean means the bitstream is exactly the commit: no tracked file changed, and every file the
# build reads is in git (a new source that was never added is not in the commit either).
if rev=$(git -C "$root" rev-parse HEAD 2>/dev/null); then
    if [ -n "$(git -C "$root" status --porcelain --untracked-files=no)" ]; then dirty=1; else dirty=0; fi
    untracked=
    for f in $sources $incfiles $mems $xdcs $cdc_files boards/arty_a7/vivado/build.tcl; do
        git -C "$root" ls-files --error-unmatch -- "$f" > /dev/null 2>&1 || untracked="$untracked $f"
    done
    if [ -n "$untracked" ]; then
        dirty=1
        echo "$me: not in git, so the build counts as dirty:$untracked"
    fi
else                                                    # no git here (a container): devenv/run.sh's values
    rev=${SURFCPU_BUILD_ID:-unknown}
    dirty=${SURFCPU_BUILD_DIRTY:-1}
fi
build_time=${SOURCE_DATE_EPOCH:-$(date +%s)}
case "$rev" in
    [0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]*) build_id=$(printf '%.8s' "$rev") ;;
    *) build_id=00000000 ;;
esac
run_id="$design-$board-$build_id-$(date -u +%Y%m%dT%H%M%SZ)"

# ------------------------------------------------------------------------------ stage
rm -rf "$stage"
mkdir -p "$stage/surfcpu" "$stage/out" "$base"
if [ "$dry" = 0 ]; then                                 # a new build replaces the last one's results
    rm -rf "$base/out" "$base/$design.bit" "$base/manifest.txt"
fi
copy() {          # PATH relative to surfcpu/
    mkdir -p "$stage/surfcpu/$(dirname "$1")"
    cp "$root/$1" "$stage/surfcpu/$1"
}
for f in $sources $incfiles $mems $xdcs $cdc_files boards/arty_a7/vivado/build.tcl; do copy "$f"; done
if [ -n "$rom" ]; then
    mkdir -p "$stage/surfcpu/rom"
    cp "$romsrc" "$stage/surfcpu/$rom"
fi

tcl_list() { printf '%s' "$*" | tr '\n' ' ' | sed -e 's/  */ /g' -e 's/^ //' -e 's/ $//'; }
{
    echo "# Written by tools/fpga-build.sh for $run_id; read by boards/arty_a7/vivado/build.tcl"
    echo "set design {$design}"
    echo "set top {$top}"
    echo "set board {$board}"
    echo "set part {$part}"
    echo "set sources {$(tcl_list $sources)}"
    echo "set incdirs {$(tcl_list $incdirs)}"
    echo "set mems {$(tcl_list $mems)}"
    echo "set xdcs {$(tcl_list $xdcs)}"
    printf 'set cdcs {'
    echo "$cdcs" | awk 'NF == 2 { printf "{%s %s} ", $1, $2 }'
    echo '}'
    printf 'set generics {'
    for g in $generics; do
        case "$g" in
            BOARD)        printf 'BOARD {"%s"} ' "$board" ;;
            BUILD_ID)     printf "BUILD_ID 32'h%s " "$build_id" ;;
            BUILD_FLAGS)  printf "BUILD_FLAGS 32'h%08x " "$dirty" ;;
            BUILD_TIME)   printf "BUILD_TIME 32'h%08x " "$build_time" ;;
            BOOTROM_INIT) ;;                    # build.tcl passes the staged ROM as an absolute path
            *) die "$design: no value for generic $g" ;;
        esac
    done
    echo '}'
    echo "set rom {$rom}"
} > "$stage/params.tcl"

# ------------------------------------------------------------------------------ manifest (this side)
cat_sha() { ( cd "$root" && cat "$@" ) | shasum -a 256 | awk '{ print $1 }'; }
spec_get() { sed -n "s/^$1[[:space:]]*=[[:space:]]*\"\{0,1\}\([^\"#]*\)\"\{0,1\}.*/\1/p" "$root/spec/soc.toml" | head -n 1 | sed 's/[[:space:]]*$//'; }
kernel="$root/../build/riscv32/arty_a7/SURFOS.BIN"
[ "$dry" = 1 ] && head="$base/manifest.dry-run.txt"   # a dry run leaves the last real run alone
{
    echo "# SurfCPU bitstream build manifest (docs/BUILD-HOST.md), written by tools/fpga-build.sh"
    echo "run_id = \"$run_id\""
    echo "design = \"$design\""
    echo "top = \"$top\""
    echo "board = \"$board\""
    echo "part = \"$part\""
    echo "flow = \"$FLOW\""
    case "$FLOW" in
        vivado-ssh)    echo "build_host_alias = \"$HOST\"" ;;
        vivado-docker) echo "build_host_alias = \"docker $IMAGE\"" ;;
    esac
    echo "vivado_binary = \"$VIVADO\""
    echo "source_revision = \"$rev\""
    echo "source_dirty = $( [ "$dirty" = 1 ] && echo true || echo false )"
    echo "build_id = \"$build_id\""
    echo "build_time = $build_time"
    echo "sources_sha256 = \"$(cat_sha $sources)\""
    echo "constraints_sha256 = \"$(cat_sha $xdcs $cdc_files)\""
    echo "rom_sha256 = \"$( [ -n "$rom" ] && sha "$romsrc" || true )\""
    echo "kernel_sha256 = \"$( [ -f "$kernel" ] && sha "$kernel" || true )\""
    echo "isa = \"$(spec_get isa)\""
    echo "abi = \"$(spec_get abi)\""
    echo "map_version = $(spec_get map_version)"
    echo "soc_version = \"$(spec_get soc_version)\""
    echo "platform_abi = $(spec_get platform_abi)"
    echo "mac = \"$(uname -s)-$(uname -m)\""
} > "$head"
[ "$dry" = 1 ] || cp "$head" "$manifest"   # until finish(): no result, so tools/program.sh refuses it

echo "$me: $design for $board ($part), build $build_id$( [ "$dirty" = 1 ] && echo ' dirty'), flow $FLOW"
echo "$me: staged in ${stage#$root/} ($(echo $sources | wc -w | tr -d ' ') sources, $(echo $xdcs $cdc_files | wc -w | tr -d ' ') constraint files)"

# ------------------------------------------------------------------------------ run
vivado_args="-mode batch -nojournal -log out/vivado.log -source surfcpu/boards/arty_a7/vivado/build.tcl -tclargs params.tcl"
remote_run() {    # REMOTE_DIR: Vivado under nohup; its pid, output and exit status go to out/
    echo "cd $1 && nohup sh -c 'echo \$\$ > out/vivado.pid; $VIVADO $vivado_args; echo \$? > out/vivado.exit' > out/vivado.nohup.txt 2>&1 < /dev/null"
}
status=0
if [ "$dry" = 1 ]; then
    echo "$me: --dry-run: would run:"
    case "$FLOW" in
        vivado-ssh)
            echo "  ssh $ssh_opts $HOST mkdir -p $REMOTE_DIR/$run_id"
            echo "  tar cf - -C ${stage#$root/} . | ssh $ssh_opts $HOST tar xf - -C $REMOTE_DIR/$run_id"
            echo "  ssh $ssh_opts $HOST \"$(remote_run "$REMOTE_DIR/$run_id")\""
            echo "  ssh $ssh_opts $HOST 'cd $REMOTE_DIR/$run_id && tar cf - --exclude=*.dcp out' | tar xf - -C ${base#$root/}" ;;
        vivado-docker)
            echo "  $DOCKER run --rm --platform linux/amd64 -v $stage:/build -w /build $IMAGE $VIVADO $vivado_args"
            echo "  cp -R ${stage#$root/}/out ${base#$root/}/out" ;;
    esac
    echo "$me: --dry-run: params.tcl:"
    sed 's/^/  /' "$stage/params.tcl"
    echo "$me: --dry-run: nothing was sent anywhere; manifest so far in ${head#$root/}"
    exit 0
fi

case "$FLOW" in
    vivado-ssh)
        remote="$REMOTE_DIR/$run_id"
        # shellcheck disable=SC2086
        ssh $ssh_opts "$HOST" "mkdir -p $remote" || die "cannot reach $HOST, or cannot make $remote there"
        # shellcheck disable=SC2086
        tar cf - -C "$stage" . | ssh $ssh_opts "$HOST" "tar xf - -C $remote" || die "could not copy the stage to $HOST:$remote"
        # shellcheck disable=SC2086
        if ssh $ssh_opts "$HOST" "$(remote_run "$remote")"; then rc=0; else rc=$?; fi
        if [ "$rc" = 255 ]; then
            echo "$me: the connection to $HOST ended (ssh exit 255). If Vivado had started, it keeps running" >&2
            echo "$me: there under nohup; fetch its results later: tools/fpga-build.sh --fetch --design $design $board" >&2
            exit 3
        fi
        [ "$rc" = 0 ] || die "could not start Vivado on $HOST (exit $rc; see $remote/out/vivado.nohup.txt there)"
        fetch_ssh
        ;;
    vivado-docker)
        # shellcheck disable=SC2086
        "$DOCKER" run --rm --platform linux/amd64 -v "$stage:/build" -w /build "$IMAGE" "$VIVADO" $vivado_args || status=$?
        cp -R "$stage/out" "$base/out"
        ;;
esac

# ------------------------------------------------------------------------------ results
vivado_results "$status"
