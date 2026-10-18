#!/bin/sh
# SurfCPU: build a bitstream on the configured build host (docs/BUILD-HOST.md, docs/BOARD.md section 8).
#
#   tools/fpga-build.sh [--dry-run] [--design NAME] BOARD
#   tools/fpga-build.sh --fetch [--design NAME] BOARD    the results of the last vivado-ssh run
#   make build-fpga BOARD=arty_a7_100t [DESIGN=arty_a7_baseline] [DRY_RUN=1]
#
# BOARD is a line of boards/arty_a7/boards.txt (arty_a7_35t, arty_a7_100t). The design is
# surfcpu_arty_a7_top unless --design or $DESIGN names another boards/arty_a7/designs/*.files.
# FLOW comes from the environment (make build-fpga FLOW=openxc7) or else from
# ~/.config/surfcpu/build-host.conf (never committed; tests point SURFCPU_BUILD_HOST_CONF
# elsewhere), which also holds the settings of the Vivado flows:
#   FLOW=vivado-ssh      HOST (an ssh alias using key authentication), REMOTE_DIR, VIVADO
#   FLOW=vivado-docker   VIVADO, IMAGE (an x86-64 Linux image with Vivado), DOCKER (optional)
#   FLOW=openxc7         the open flow on this Mac (tools/openxc7-build.sh, docs/OPEN-FLOW.md);
#                        needs only its image (make openxc7-image), no build-host.conf
#
# Vivado: it stages the design's sources and constraints, boards/arty_a7/vivado/build.tcl and a
# params.tcl in build/fpga/BOARD/DESIGN/stage, runs the non-project flow on the host (over ssh,
# or in Docker) and brings back out/ (bitstream, reports, vivado.log, build-results.txt).
# openxc7: it gives tools/openxc7-build.sh the design's sources, include directories, the top's
# build values and the design's own defines as defines, the constraints nextpnr can apply
# (boards/arty_a7/nextpnr-xdc.awk; the rest is listed as not checked) and --dsp safe, with its
# output in build/fpga/BOARD/DESIGN/openxc7. Either way it writes build/fpga/BOARD/DESIGN/
# manifest.txt (SURFCPU_FPGA_OUT replaces build/fpga; its kernel_sha256 is of SURFCPU_KERNEL, by
# default the last make build-kernel's SURFOS.BIN, for the record only; ROM= replaces the design's
# boot ROM image and PLACER_SEEDS= its placer seeds, both recorded). A design may read generated files from
# build/ (the LiteDRAM core of the DDR designs): its "generated" lists (ip/litedram/arty.sha256)
# give their SHA-256, and the build counts as clean only if they match. Exit 0 only with a bitstream whose checks passed; a new build first removes the
# previous one's bitstream and results. --dry-run prints every command it would run, without
# contacting a host or starting Docker, and leaves the last build's results alone.
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
        -h|--help) sed -n '2,31p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
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
conf_get() {      # KEY: the last KEY=value line, without comments or surrounding quotes
    [ -f "$conf" ] || return 0
    sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*//p" "$conf" | tail -n 1 |
        sed -e 's/[[:space:]]*#.*$//' -e 's/[[:space:]]*$//' -e 's/^"\(.*\)"$/\1/' -e "s/^'\(.*\)'\$/\1/"
}
if [ -n "${FLOW:-}" ]; then flow_from=environment; else FLOW=$(conf_get FLOW); flow_from=$conf; fi
if [ "$FLOW" != openxc7 ] && [ ! -f "$conf" ]; then      # the open flow needs no build host
    echo "$me: no build host configured ($conf): bitstream generation is pending." >&2
    echo "$me: docs/BUILD-HOST.md describes the options and the file; FLOW=openxc7 needs none." >&2
    exit 2
fi
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
    openxc7)                    # SURFCPU_OPENXC7_BUILD: a stand-in, for the wrapper tests only
        OPENXC7=${SURFCPU_OPENXC7_BUILD:-$root/tools/openxc7-build.sh}
        [ -x "$OPENXC7" ] || die "FLOW=openxc7 needs $OPENXC7 (docs/OPEN-FLOW.md)"
        ox_image=${SURFCPU_OPENXC7_IMAGE:-surfcpu-openxc7:$(sed -n 's/^IMAGE_TAG=//p' "$root/tools/openxc7-build.sh")} ;;
    '') die "$conf: FLOW is not set (vivado-ssh, vivado-docker or openxc7)" ;;
    *)  die "unknown FLOW '$FLOW' from the $flow_from (vivado-ssh, vivado-docker or openxc7)" ;;
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

# ------------------------------------------------------------------------------ the open flow (functions)
toml_str() { printf '"%s"' "$(printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g')"; }
openxc7_build() {     # FLOW=openxc7: run tools/openxc7-build.sh on the design, then gate its results
    case "$base/" in
        "$root"/*) ;;
        *) die "FLOW=openxc7 builds inside surfcpu/ (its container sees only the repository), but SURFCPU_FPGA_OUT=$fpga_out is outside it" ;;
    esac
    ox="$base/openxc7"
    xdc_nx="$base/nextpnr.xdc"
    notgiven="$base/nextpnr-not-checked.txt"
    mkdir -p "$base"
    if [ "$dry" = 0 ]; then                             # a new build replaces the last one's results
        rm -rf "${base:?}/openxc7" "${base:?}/rom" "${base:?}/${design:?}.bit" "${base:?}/openxc7-results.txt" "$manifest"
    fi
    # The constraints nextpnr applies (pins, the oscillator's clock), and a list of the rest.
    ( cd "$root" && awk -f boards/arty_a7/nextpnr-xdc.awk -v notgiven="$notgiven" $xdcs ) > "$xdc_nx" ||
        die "could not turn the constraints into nextpnr's (boards/arty_a7/nextpnr-xdc.awk)"
    echo "$cdcs" | while read -r mod file; do
        [ -n "$file" ] || continue
        n_md=$(grep -v '^[[:space:]]*#' "$root/$file" | grep -c 'set_max_delay' || true)
        n_fp=$(grep -v '^[[:space:]]*#' "$root/$file" | grep -c 'set_false_path' || true)
        n_ar=$(grep -v '^[[:space:]]*#' "$root/$file" | grep -c 'ASYNC_REG' || true)
        echo "$file: scoped to every $mod: $n_md set_max_delay -datapath_only, $n_fp set_false_path, $n_ar ASYNC_REG  [clock-domain crossing bounds: Vivado only, not checked]"
    done >> "$notgiven"
    n_given=$(grep -c . "$xdc_nx" || true)
    n_not=$(grep -c . "$notgiven" || true)
    # The boot ROM goes in from a copy kept beside the bitstream: synthesis, the manifest's
    # rom_sha256 and romcheck read that one file even if make build-rom rewrites build/fw/
    # during the build (rom_image still names where it came from).
    rom_ox=
    if [ -n "$rom" ]; then
        mkdir -p "$base/rom"
        cp "$romsrc" "$base/$rom"
        romsrc=$base/$rom
        rom_ox=${base#"$root"/}/$rom
    fi
    # The top's build values as defines (the RTL turns them into its parameters' defaults).
    set --
    for g in $generics; do
        case "$g" in
            BOARD)        set -- "$@" -D "ARTY_BUILD_BOARD=\"$board\"" ;;
            BUILD_ID)     set -- "$@" -D "ARTY_BUILD_ID=32'h$build_id" ;;
            BUILD_FLAGS)  set -- "$@" -D "ARTY_BUILD_FLAGS=32'h$(printf '%08x' "$dirty")" ;;
            BUILD_TIME)   set -- "$@" -D "ARTY_BUILD_TIME=32'h$(printf '%08x' "$build_time")" ;;
            BOOTROM_INIT) set -- "$@" -D "ARTY_BOOTROM_INIT=\"$rom_ox\"" ;;
            *) die "$design: no value for generic $g" ;;
        esac
    done
    for d in $defines; do set -- "$@" -D "$d"; done
    for d in $incdirs; do set -- "$@" -I "$d"; done
    write_head
    [ -z "$lvs_timeout" ] || set -- "$@" --lvs-timeout "$lvs_timeout"
    [ -z "$seed" ] || set -- "$@" --seed "$(echo "$seed" | tr ' ' ',')"
    # shellcheck disable=SC2086
    set -- --top "$top" --board "$board" --xdc "${xdc_nx#"$root"/}" --out "${ox#"$root"/}" --dsp safe \
        --manifest-include "${head#"$root"/}" "$@" $sources
    echo "$me: $design for $board ($part), build $build_id$( [ "$dirty" = 1 ] && echo ' dirty'), flow openxc7 ($ox_image)"
    [ -z "$rom" ] || echo "$me: boot ROM $rom_image (as $rom_ox), sha256 $(sha "$romsrc")"
    echo "$me: nextpnr is given $n_given constraint lines (${xdc_nx#"$root"/}); it cannot check these $n_not, which only Vivado does:"
    sed "s/^/$me:   /" "$notgiven"
    cd "$root"
    if [ "$dry" = 1 ]; then
        echo "$me: --dry-run: would run:"
        echo "  ${OPENXC7#"$root"/} $*"
        "$OPENXC7" --dry-run "$@" | sed 's/^/  | /'
        echo "$me: --dry-run: nothing was built; manifest so far in ${head#"$root"/}"
        exit 0
    fi
    if "$OPENXC7" "$@"; then st=0; else st=$?; fi
    openxc7_results "$st"
}
openxc7_results() {   # STATUS: gate on tools/openxc7-build.sh's results, write the manifest, exit
    om="$ox/manifest.txt"
    res="$base/openxc7-results.txt"
    problems=
    [ "$1" = 0 ] || problems="tools/openxc7-build.sh exit status $1"
    check() {         # NAME: the result on the manifest's "check NAME" line (nothing without one)
        [ -f "$om" ] || return 0
        awk -v n="check $1" 'substr($0, 1, length(n) + 1) == n " " {
            r = substr($0, 21); sub(/ \([^()]*\)$/, "", r); print r; exit }' "$om"
    }
    {
        echo "image = $(toml_str "$ox_image")"
        if [ -f "$om" ]; then
            for tool in yosys nextpnr; do
                v=$(awk -v t="$tool" '$1 == t { $1 = ""; sub(/^ +/, ""); print; exit }' "$om")
                [ -z "$v" ] || echo "$tool = $(toml_str "$v")"
            done
            echo "part = $(toml_str "$(sed -n 's/^part  *\([^ ]*\).*/\1/p' "$om" | head -n 1)")"
        fi
        echo "dsp_mode = \"safe\""
        echo "exit_status = $1"
        # the placer seed the bitstream comes from, and every placement attempt (seeds.txt)
        if [ -f "$ox/seeds.txt" ]; then
            seed_used=$(sed -n 's/^seed \([0-9]*\): pass,.*/\1/p' "$ox/seeds.txt" | tail -n 1)
            [ -z "$seed_used" ] || echo "placer_seed = $seed_used"
            printf 'placer_attempts = ['
            while IFS= read -r l; do printf '\n    %s,' "$(toml_str "$l")"; done < "$ox/seeds.txt"
            printf '\n]\n'
        fi
        for c in synthesis:check_synthesis bitstream:check_bitstream "round trip:check_round_trip" \
                 "block RAM:check_block_ram" LVS:check_lvs clocks:check_clocks; do
            name=${c%%:*}
            v=$(check "$name")
            echo "${c#*:} = $(toml_str "${v:-missing}")"
            case "$name:${v:-missing}" in
                *:PASS|"block RAM:none in the design"|"clocks:none in the design") ;;
                *) problems="${problems:+$problems; }check $name ${v:-missing}" ;;
            esac
        done
        # The board's checks of the synthesized netlist: every RTL memory still in RAM cells,
        # the multiplier in its DSP48E1 (boards/arty_a7/netlist_check.py); the boot ROM's
        # blocks hold its .mem (boards/arty_a7/romcheck.py).
        if [ -f "$ox/pre.json" ] && [ -f "$ox/$top.json" ]; then
            # shellcheck disable=SC2086
            if python3 boards/arty_a7/netlist_check.py --top "$top" --pre "$ox/pre.json" --post "$ox/$top.json" \
                   $dsp_args > "$ox/check-netlist.txt" 2>&1
            then v=PASS; else v=FAIL; problems="${problems:+$problems; }check netlist FAIL"; fi
        else
            v=missing; problems="${problems:+$problems; }check netlist missing"
        fi
        echo "check_netlist = \"$v\""
        if [ -n "$rom" ]; then
            if [ -f "$ox/$top.json" ] && "$ROMCHECK" --rom "$rom_ox" --netlist "${ox#"$root"/}/$top.json" \
                   --top "$top" --instance "$rom_inst" --cells-sim "${ox#"$root"/}/cells_sim.v" \
                   --out "${ox#"$root"/}/romcheck" > "$ox/romcheck.out" 2>&1 && [ -f "$ox/romcheck/check-rom.txt" ]
            then v=PASS; cp "$ox/romcheck/check-rom.txt" "$ox/check-rom.txt"
            else v=FAIL; problems="${problems:+$problems; }check boot ROM FAIL"
                 [ -f "$ox/romcheck/check-rom.txt" ] && cp "$ox/romcheck/check-rom.txt" "$ox/check-rom.txt"
            fi
            echo "check_boot_rom = \"$v\""
        fi
        if [ -f "$ox/timing.txt" ]; then
            printf 'timing = ['
            grep '^clock ' "$ox/timing.txt" | while IFS= read -r l; do printf '%s, ' "$(toml_str "$l")"; done
            echo ']'
            wns=$(sed -n 's/^clock .*slack \([-+][0-9.]*\) ns.*/\1/p' "$ox/timing.txt" | sort -g | head -n 1)
            [ -z "$wns" ] || echo "wns_ns = ${wns#+}"
            grep '^clock ' "$ox/timing.txt" | grep -qv ' PASS$' && problems="${problems:+$problems; }timing not met (nextpnr)"
            grep -q '^clock ' "$ox/timing.txt" || problems="${problems:+$problems; }no timing result"
        elif [ -f "$ox/nextpnr.log" ] && grep -q 'Max frequency for clock' "$ox/nextpnr.log"; then
            # nextpnr stopped the flow, a clock having missed its constraint, or it was stopped
            # before routing converged: its last report per clock (after routing, or else the
            # placer's estimate)
            fm=$(awk '/Max frequency for clock/ { c = $0; sub(/.*clock +/, "", c); sub(/:.*/, "", c); last[c] = $0
                      if (!(c in seen)) { seen[c] = 1; order[n++] = c } }
                      END { for (i = 0; i < n; i++) { l = last[order[i]]; sub(/^(Info|ERROR): */, "", l); gsub(/ +/, " ", l); print l } }' "$ox/nextpnr.log")
            routed=false
            grep -q 'Routing complete' "$ox/nextpnr.log" && routed=true
            echo "nextpnr_routed = $routed"
            printf 'nextpnr_fmax = ['
            echo "$fm" | while IFS= read -r l; do printf '%s, ' "$(toml_str "$l")"; done
            echo ']'
            late=$(echo "$fm" | sed -n "s/^Max frequency for clock '\{0,1\}\([^': ]*\)'\{0,1\}: \([0-9.]* MHz\) (FAIL at \([0-9.]* MHz\))$/\1 \2 of \3/p" | tr '\n' ',' | sed 's/,$//')
            if [ "$routed" = true ]; then
                [ -z "$late" ] || problems="${problems:+$problems; }timing not met (nextpnr: $late)"
            else
                it=$(grep 'iter=' "$ox/nextpnr.log" | tail -n 1 | sed -n 's/.*iter=\([0-9]*\) .*overused=\([0-9]*\).*/\1 iterations, \2 overused wires/p')
                problems="${problems:+$problems; }nextpnr did not finish routing${it:+ (router2 after $it)}"
                [ -z "$late" ] || problems="$problems; the placer's timing estimate misses too ($late)"
            fi
        fi
        if [ -f "$ox/utilization.txt" ]; then
            for u in "lut:LUTs (" "lut_positions:LUT positions" "ff:flip-flops" "carry4:carry chains" \
                     "bram36:36 Kb block RAMs" "dsp:DSP48E1"; do
                v=$(grep -F "  ${u#*:}" "$ox/utilization.txt" | sed -n 's/.* \([0-9][0-9]*\) \/ \([0-9][0-9]*\) .*/\1 \/ \2/p' | head -n 1)
                [ -z "$v" ] || echo "${u%%:*} = \"$v\""
            done
        fi
        if [ -f "$ox/check-bitstream.txt" ]; then
            v=$(sed -n 's/^IDCODE written: \(0x[0-9A-Fa-f]*\).*/\1/p' "$ox/check-bitstream.txt" | head -n 1)
            [ -z "$v" ] || echo "idcode = \"$v\""
            v=$(sed -n "s/^header: .* part '\([^']*\)'.*/\1/p" "$ox/check-bitstream.txt" | head -n 1)
            [ -z "$v" ] || echo "bitstream_header_part = \"$v\""
        fi
        echo "constraints_given = $(grep -c . "$xdc_nx" || true)"
        printf 'constraints_not_checked = ['
        while IFS= read -r l; do printf '\n    %s,' "$(toml_str "$l")"; done < "$notgiven"
        printf '\n]\n'
        echo "openxc7_manifest = $(toml_str "${om#"$root"/}")"
    } > "$res"
    [ -f "$ox/$top.bit" ] || problems="${problems:+$problems; }no bitstream"
    if [ -z "$problems" ]; then
        cp "$ox/$top.bit" "$base/$design.bit"
        echo "result = \"pass\"" >> "$res"
        finish pass "" openxc7 "$res"
        echo "$me: PASS: ${base#"$root"/}/$design.bit (open flow: nextpnr's timing is an estimate; the $n_not constraints above are not checked)"
        grep -E '^(wns_ns|lut|ff|bram36|dsp|idcode|check_netlist|check_boot_rom) ' "$res" | sed "s/^/$me:   /"
        exit 0
    fi
    echo "result = \"fail\"" >> "$res"
    finish fail "$problems" openxc7 "$res"
    echo "$me: FAIL: $problems" >&2
    echo "$me: logs and reports in ${ox#"$root"/}${om:+ (manifest ${om#"$root"/})}" >&2
    exit 1
}

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
# A "board" line restricts the design to the boards it names (the GPU designs: VRAM fills more
# block RAM than the 35T has); several lines add up, as in tools/release.py; without one the
# design builds for every board of boards.txt.
for_boards=$(items board | tr '\n\t' '  ' | tr -s ' ' | sed 's/^ //; s/ $//')
if [ -n "$for_boards" ]; then
    for b in $for_boards; do
        awk -v b="$b" '!/^#/ && $1 == b { f = 1 } END { exit !f }' "$root/boards/arty_a7/boards.txt" ||
            die "$design: its board line names $b, which is not in boards/arty_a7/boards.txt"
    done
    case " $for_boards " in
        *" $board "*) ;;
        *) die "$design builds for $(echo "$for_boards" | sed 's/ /, /g') only, not $board (its design list's board line)" ;;
    esac
fi
for r in $(items require); do
    [ -f "$root/$r" ] || die "$design needs $r, which is not on this branch yet; refusing to build (the simulation stand-in sim/board/stubs/surfcpu_soc.v is never built into a bitstream)"
done
defines=$(items define)
for d in $defines; do
    case "$d" in
        [A-Za-z_]*) ;;
        *) die "$design: bad define '$d'" ;;
    esac
    case "$d" in *[!A-Za-z0-9_=]*) die "$design: bad define '$d' (NAME or NAME=VALUE: letters, digits, _)" ;; esac
done
# Generated files (under build/, never in git): each "generated" list gives their SHA-256 and,
# on a "# generate:" line, the command that makes them.
gens=$(items generated)
for l in $gens; do [ -f "$root/$l" ] || die "$design: the generated-file list $l is missing"; done
gen_sha() {       # FILE: its SHA-256 in the design's generated lists, or nothing
    for l in $gens; do awk -v f="$1" '$1 !~ /^#/ && $2 == f { print $1; exit }' "$root/$l"; done | head -n 1
}
gen_hint() {      # the command that makes the generated files
    for l in $gens; do sed -n 's/^# generate: //p' "$root/$l"; done | head -n 1
}
for kind in src mem xdc; do
    for pat in $(items "$kind"); do
        case "$pat" in
            build/*) [ -n "$(set +f; expand "$pat")" ] ||
                die "$design needs $pat, a generated file$( [ -n "$gens" ] && echo " ($(gen_hint))"); refusing to build" ;;
        esac
    done
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
rom_line=$(items rom)
dsp_args=
for d in $(items dsp | awk 'NF == 2 { print $1 "=" $2 }'); do dsp_args="$dsp_args --dsp $d"; done
lvs_timeout=$(items lvs-timeout)
case "$lvs_timeout" in *[!0-9]*) die "$design: lvs-timeout must be a number of seconds" ;; esac
seed=$(items seed | tr -s ' \t' ' ' | sed 's/^ //; s/ $//')   # the open flow's placer seeds (default 1),
case "$seed" in *[!0-9\ ]*) die "$design: seed must be a number or numbers" ;; esac   # tried in turn
# PLACER_SEEDS= replaces them for one build (a diagnostic build of chosen placements, with its
# own SURFCPU_FPGA_OUT); the manifest's placer_seeds then lists those, and placer_seeds_from says so
seeds_from=design
if [ -n "${PLACER_SEEDS:-}" ]; then
    seed=$(echo "$PLACER_SEEDS" | tr -s ', \t' '   ' | sed 's/^ *//; s/ *$//')
    case "$seed" in ''|*[!0-9\ ]*) die "PLACER_SEEDS must be a number or numbers ($PLACER_SEEDS)" ;; esac
    seeds_from=PLACER_SEEDS
fi
set +f

# The boot ROM: the design list's "rom FILE INSTANCE" (FILE relative to surfcpu/), or ROM=.
rom=
rom_inst=
rom_image=
case " $generics " in
    *" BOOTROM_INIT "*)
        set -f
        set -- $rom_line
        set +f
        [ $# -eq 2 ] || die "$design: BOOTROM_INIT needs a line 'rom <file> <instance>' in $list"
        rom_inst=$2
        romsrc=${ROM:-$root/$1}
        case "$romsrc" in /*) ;; *) romsrc=$PWD/$romsrc ;; esac
        [ -f "$romsrc" ] || die "$design needs the boot ROM image $romsrc (make build-rom, the fw track)"
        rom=rom/$(basename "$romsrc")
        case "$romsrc" in
            "$root"/*) rom_image=${romsrc#"$root"/} ;;
            *)         rom_image=$romsrc ;;
        esac ;;
esac
ROMCHECK=${SURFCPU_ROMCHECK:-$root/boards/arty_a7/romcheck.py}

# ------------------------------------------------------------------------------ build identity
# Clean means the bitstream is exactly the commit: no tracked file changed, and every file the
# build reads is in git (a new source that was never added is not in the commit either).
# A generated file is part of the commit when its SHA-256 is the one its generated list (in
# git) records: the generator makes the same bytes from the committed inputs.
gen_ok= gen_bad=
for f in $sources $incfiles $mems $xdcs; do
    want=$(gen_sha "$f")
    [ -n "$want" ] || continue
    if [ "$(sha "$root/$f")" = "$want" ]; then gen_ok="$gen_ok $f"; else gen_bad="$gen_bad $f"; fi
done
if rev=$(git -C "$root" rev-parse HEAD 2>/dev/null); then
    if [ -n "$(git -C "$root" status --porcelain --untracked-files=no)" ]; then dirty=1; else dirty=0; fi
    untracked=
    for f in $sources $incfiles $mems $xdcs $cdc_files boards/arty_a7/vivado/build.tcl; do
        case " $gen_ok $gen_bad " in *" $f "*) continue ;; esac
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
if [ -n "$gen_bad" ]; then
    dirty=1
    echo "$me: generated files that differ from the SHA-256 in$( for l in $gens; do printf ' %s' "$l"; done), so the build counts as dirty:$gen_bad"
    echo "$me: (regenerate them: $(gen_hint))"
fi
[ -z "$gen_ok" ] || echo "$me: $(echo $gen_ok | wc -w | tr -d ' ') generated files match$( for l in $gens; do printf ' %s' "$l"; done)"
build_time=${SOURCE_DATE_EPOCH:-$(date +%s)}
case "$rev" in
    [0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]*) build_id=$(printf '%.8s' "$rev") ;;
    *) build_id=00000000 ;;
esac
run_id="$design-$board-$build_id-$(date -u +%Y%m%dT%H%M%SZ)"

# ------------------------------------------------------------------------------ manifest (this side)
cat_sha() { ( cd "$root" && cat "$@" ) | shasum -a 256 | awk '{ print $1 }'; }
spec_get() { sed -n "s/^$1[[:space:]]*=[[:space:]]*\"\{0,1\}\([^\"#]*\)\"\{0,1\}.*/\1/p" "$root/spec/soc.toml" | head -n 1 | sed 's/[[:space:]]*$//'; }
kernel=${SURFCPU_KERNEL:-$root/../build/riscv32/arty_a7/SURFOS.BIN}   # recorded only (make release names its own)
[ "$dry" = 1 ] && head="$base/manifest.dry-run.txt"   # a dry run leaves the last real run alone
write_head() {    # the keys known before the run, the same for every flow
    mkdir -p "$base"
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
            openxc7)       echo "build_host_alias = \"this Mac, docker $ox_image\"" ;;
        esac
        echo "vivado_binary = \"$VIVADO\""
        echo "source_revision = \"$rev\""
        echo "source_dirty = $( [ "$dirty" = 1 ] && echo true || echo false )"
        echo "build_id = \"$build_id\""
        echo "build_time = $build_time"
        printf 'placer_seeds = ['; for sd in ${seed:-1}; do printf '%s, ' "$sd"; done; echo ']'
        echo "placer_seeds_from = \"$seeds_from\""
        echo "sources_sha256 = \"$(cat_sha $sources)\""
        printf 'defines = ['; for d in $defines; do printf '"%s", ' "$d"; done; echo ']'
        printf 'generated_lists = ['; for l in $gens; do printf '"%s", ' "$l"; done; echo ']'
        echo "generated_files_match = $( [ -z "$gen_bad" ] && echo true || echo false )"
        echo "constraints_sha256 = \"$(cat_sha $xdcs $cdc_files)\""
        echo "rom_image = \"$rom_image\""
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
}

[ "$FLOW" = openxc7 ] && openxc7_build             # does not return

# ------------------------------------------------------------------------------ stage
rm -rf "$stage"
mkdir -p "$stage/surfcpu" "$stage/out" "$base"
if [ "$dry" = 0 ]; then                                 # a new build replaces the last one's results
    rm -rf "${base:?}/out" "${base:?}/${design:?}.bit" "$manifest"
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
    echo "set defines {$(tcl_list $defines)}"
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

write_head

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
