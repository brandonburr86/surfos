#!/bin/sh
# SurfCPU board track: the whole SoC through the open flow, for the record. On the Mac, from
# surfcpu/, with Docker and both images, on a committed tree (the ROMs and bitstreams carry the
# commit); about 45 minutes. Nothing is programmed and nothing leaves the Mac.
#   1. make build-rom, once: the boot ROM images of this commit (nothing rebuilds them later);
#   2. make build-fpga FLOW=openxc7 for surfcpu_arty_a7_bramdiag (the BRAM diagnostics,
#      milestone 3) and surfcpu_arty_a7_top (SurfBoot) on both boards, the two boards of a
#      design at once;
#   3. make board-netlist-sim for each build: its Yosys netlist run from its pins until the
#      UART line the design ends on, compared with the RTL simulation's output;
#   4. make board-uart-expect for each board: what the serial port should show, from RTL
#      simulation (BOARD.md section 12);
#   5. make program-volatile BOARD=... DESIGN=... against the fake openFPGALoader of
#      sim/board/tools/fake (its header, IDCODE and manifest checks only).
# Writes docs/evidence/board/openxc7-soc.txt and docs/evidence/board/uart-expect.txt. The
# bitstreams stay in build/fpga/ (never committed). --report-only writes the two files again
# from the last run's outputs without building or simulating anything.
set -eu
report_only=0
[ "${1:-}" != --report-only ] || report_only=1

out=docs/evidence/board/openxc7-soc.txt
uout=docs/evidence/board/uart-expect.txt
designs="surfcpu_arty_a7_bramdiag surfcpu_arty_a7_top"
boards="arty_a7_100t arty_a7_35t"
t=build/sim/board/openxc7_soc_evidence
rev=$(git rev-parse --short=12 HEAD)
state=clean
[ -z "$(git status --porcelain)" ] || state="dirty (uncommitted changes)"
started=$(date +%s)

field() {         # KEY FILE: a value of the [openxc7] table of a manifest
    awk -v k="$1" '/^\[openxc7\]/ { s = 1; next } /^\[/ { s = 0 } s && $1 == k && $2 == "=" {
        v = $0; sub(/^[^=]*=[ \t]*/, "", v); gsub(/^"|"$/, "", v); print v; exit }' "$2"
}
top_key() {       # KEY FILE: a top-level value of a manifest
    awk -v k="$1" '/^\[/ { exit } $1 == k && $2 == "=" { v = $0; sub(/^[^=]*=[ \t]*/, "", v); gsub(/^"|"$/, "", v); print v; exit }' "$2"
}
stop_text() { case "$1" in surfcpu_arty_a7_top) echo "The LEDs" ;; *) echo "BRAMDIAG:" ;; esac; }

if [ "$report_only" = 0 ]; then
rm -rf "$t"
mkdir -p "$t"
echo "$rev ($state)" > "$t/source"

# ------------------------------------------------------------------ 1. the ROMs, once
make --no-print-directory build-rom > "$t/build-rom.txt" 2>&1 || { tail -20 "$t/build-rom.txt"; exit 1; }
for r in bramdiag surfboot; do shasum -a 256 "build/fw/$r.mem" | awk '{ print $1 }' > "$t/$r.sha"; done
cp build/fw/gen/fw_build.h "$t/fw_build.h"

# ------------------------------------------------------------------ 2. the builds
for design in $designs; do
    for board in $boards; do
        (
            t0=$(date +%s)
            if make --no-print-directory build-fpga FLOW=openxc7 BOARD=$board DESIGN=$design > "$t/$board-$design.txt" 2>&1
            then rc=0; else rc=$?; fi
            echo "$rc $(( $(date +%s) - t0 ))" > "$t/$board-$design.status"
        ) &
    done
    wait
    for board in $boards; do echo "$board $design: make exit $(cut -d' ' -f1 "$t/$board-$design.status"), $(cut -d' ' -f2 "$t/$board-$design.status") s"; done
done

# ------------------------------------------------------------------ 3. and 4. simulations
for board in $boards; do
    if make --no-print-directory board-uart-expect BOARD=$board > "$t/uart-expect-$board.txt" 2>&1; then rc=0; else rc=$?; fi
    echo "$rc" > "$t/uart-expect-$board.rc"
    mkdir -p "$t/uart-$board"
    cp build/sim/board/uart_expect/bramdiag.txt build/sim/board/uart_expect/surfboot.txt "$t/uart-$board/" 2>/dev/null || true
    cp build/sim/board/uart_expect/mkcard.txt "$t/uart-$board/" 2>/dev/null || true
    [ ! -f build/sim/board/uart_expect/vga/screen.txt ] || cp build/sim/board/uart_expect/vga/screen.txt "$t/uart-$board/"
    for design in $designs; do
        if [ -f "build/fpga/$board/$design/openxc7/surfcpu_arty_a7_top.netlist.v" ]; then
            if make --no-print-directory board-netlist-sim BOARD=$board DESIGN=$design > "$t/netlist-$board-$design.txt" 2>&1
            then rc=0; else rc=$?; fi
        else
            rc=missing
        fi
        echo "$rc" > "$t/netlist-$board-$design.rc"
        [ ! -f "build/sim/board/netlist/$board-$design/uart.txt" ] ||
            cp "build/sim/board/netlist/$board-$design/uart.txt" "$t/netlist-$board-$design.uart"
    done
done
echo "$(( ($(date +%s) - started) / 60 ))" > "$t/minutes"
fi
[ -f "$t/minutes" ] || { echo "openxc7_soc_evidence: no complete run in $t" >&2; exit 1; }
rev_state=$(cat "$t/source")

# ------------------------------------------------------------------ 5. and the record
critical() {      # TIMING: one line per clock: the critical path's length, ends and split
    python3 - "$1" <<'EOF'
import re, sys
text = open(sys.argv[1]).read()
for m in re.finditer(r"Critical path report for clock '([^']+)' \(([^)]*)\):\n(.*?)\n([0-9.]+ ns logic, [0-9.]+ ns routing)", text, re.S):
    clock, edges, body, split = m.groups()
    nets = re.findall(r"Net (\S+) \([^)]*\) -> \([^)]*\)\n(?:\s+Sink \S+\n)?(?:\s+Defined in:\n\s+(\S+))?", body)
    named = [(n, re.sub(r"^(.*?):(\d+).*$", r"\1:\2", d)) for n, d in nets if not n.startswith("$")]
    totals = re.findall(r"^\s+\S+\s+[0-9.]+\s+([0-9.]+) ", body, re.M)
    first = f"{named[0][0]} ({named[0][1]})" if named else "?"
    last = f"{named[-1][0]} ({named[-1][1]})" if named else "?"
    print(f"{clock} ({edges}): {totals[-1] if totals else '?'} ns = {split}; from {first} to {last}")
EOF
}
{
    echo "SurfCPU board track evidence: the whole SoC through the open flow (milestone 3 and SurfBoot)"
    echo "Generated by sim/board/openxc7_soc_evidence.sh on $(date -u +%Y-%m-%d) at source $rev_state, in $(cat "$t/minutes") minutes."
    echo "Commands: make build-rom; make build-fpga FLOW=openxc7 BOARD=<board> DESIGN=<design> for both"
    echo "boards and both designs (no build-host.conf); make board-netlist-sim; make program-volatile"
    echo "with the fake openFPGALoader. Nothing here was loaded on a board: the physical behaviour of"
    echo "these bitstreams is pending. nextpnr's timing is an estimate, not Vivado signoff. HAS_DDR = 0."
    echo
    echo "boot ROM images (make build-rom at this commit):"
    for r in bramdiag surfboot; do echo "  build/fw/$r.mem  sha256 $(cat "$t/$r.sha")"; done
    grep '^#define' "$t/fw_build.h" | sed 's/^#define /  firmware /'
    for design in $designs; do
        for board in $boards; do
            b=build/fpga/$board/$design
            m=$b/manifest.txt
            ox=$b/openxc7
            echo
            echo "== $design on $board ($(top_key part "$m"); placed and timed as $(field part "$m"))"
            echo "make exit status $(cut -d' ' -f1 "$t/$board-$design.status"), $(cut -d' ' -f2 "$t/$board-$design.status") s; result = $(top_key result "$m")$( [ "$(top_key result "$m")" = pass ] || echo " ($(top_key reason "$m"))")"
            echo "image: $(field image "$m"); $(field yosys "$m" | cut -d'(' -f1)/ $(field nextpnr "$m" | sed 's/.*(\(.*\))/nextpnr-himbaechel \1/')"
            echo "build: BUILD_ID $(top_key build_id "$m"), source $(top_key source_revision "$m"), dirty = $(top_key source_dirty "$m"), BUILD_TIME $(top_key build_time "$m")"
            echo "boot ROM: $(top_key rom_image "$m"), sha256 $(top_key rom_sha256 "$m")"
            echo "  built from the copy $b/$(ls "$b/rom")$(cmp -s "$b/rom/$(ls "$b/rom")" "$(top_key rom_image "$m")" && echo ', identical to the file make build-rom wrote')"
            echo "bitstream: $b/$design.bit"
            echo "  $(wc -c < "$b/$design.bit" | tr -d ' ') bytes, sha256 $(shasum -a 256 "$b/$design.bit" | cut -d' ' -f1)"
            echo "  manifest bitstream_sha256 $(top_key bitstream_sha256 "$m")"
            echo "  $(grep '^header:' "$ox/check-bitstream.txt" | sed "s/ date .*,/,/")"
            grep -E '^IDCODE written|^  = |stream [0-9]+ bits|^frame data|^check for' "$ox/check-bitstream.txt" | sed 's/^/  /'
            echo "checks (tools/openxc7-build.sh, then tools/fpga-build.sh's own):"
            grep '^check ' "$ox/manifest.txt" | sed 's/^/  /'
            echo "  check_netlist = $(field check_netlist "$m"), check_boot_rom = $(field check_boot_rom "$m")"
            echo "  synthesis: flip-flops in the RTL / in the netlist / held in DSP, RAM or SRL: $(grep '^whole design' "$ox/check-synthesis.txt" | tr -s ' ' | sed 's/^whole design //')"
            grep -E '^memories|^outputs constant|^synthcheck' "$ox/check-synthesis.txt" | sed 's/^/    /'
            echo "  memories and multiplier (boards/arty_a7/netlist_check.py):"
            sed 's/^/    /' "$ox/check-netlist.txt"
            echo "  boot ROM (boards/arty_a7/romcheck.py):"
            sed 's/^/    /' "$ox/check-rom.txt"
            echo "  block RAM: $(grep -v '^bramcheck' "$ox/check-bram.txt" | head -n 1); $(grep '^bramcheck' "$ox/check-bram.txt")"
            echo "  LVS: $(grep -E 'proved' "$ox/check-lvs.txt" | tail -n 1)"
            echo "  round trip: $(tail -n 1 "$ox/check-roundtrip.txt")"
            sed 's/^/  clocks: /' "$ox/check-clocks.txt"
            echo "timing (nextpnr):"
            grep -E '^clock |^Hold-fix|^nextpnr warnings' "$ox/timing.txt" | sed 's/^/  /'
            sed -n '/^Cross-domain/,/^$/p' "$ox/timing.txt" | sed '/^$/d' | sed 's/^/  /'
            echo "  critical paths:"
            critical "$ox/timing.txt" | sed 's/^/    /'
            echo "utilization:"
            sed -n '/documented capacity/,/DSP48E1/p' "$ox/utilization.txt" | sed 's/^/  /'
            grep '^Yosys cells' "$ox/utilization.txt" | sed 's/^/  /'
            echo "constraints: nextpnr given $(field constraints_given "$m") lines; $(grep -c . "$b/nextpnr-not-checked.txt") not checked by the open flow ($b/nextpnr-not-checked.txt)"
            echo "step times:"
            sed 's/^/  /' "$ox/times.txt"
            echo "post-synthesis simulation (make board-netlist-sim BOARD=$board DESIGN=$design): exit $(cat "$t/netlist-$board-$design.rc")"
            n=$t/netlist-$board-$design.uart
            grep -E 'netlist_sim: (arty|tb_netlist: [0-9]+ lines|tb_netlist: LEDs|tb_netlist: VGA|built in)' "$t/netlist-$board-$design.txt" | sed 's/^/  /'
            grep 'Verilator: \$finish' "$t/netlist-$board-$design.txt" | sed 's/^netlist_sim: - /  /'
            case "$design" in surfcpu_arty_a7_top) rtl=$t/uart-$board/surfboot.txt ;; *) rtl=$t/uart-$board/bramdiag.txt ;; esac
            if [ -f "$n" ] && [ -f "$rtl" ]; then
                nb=$(wc -c < "$n" | tr -d ' ')
                if head -c "$nb" "$rtl" | cmp -s - "$n"
                then echo "  its $nb UART characters equal the first $nb of the RTL simulation's (make board-uart-expect BOARD=$board)"
                else echo "  its $nb UART characters DIFFER from the RTL simulation's:"; head -c "$nb" "$rtl" | diff - "$n" | head -n 20 | sed 's/^/    /'
                fi
                echo "  the netlist's UART output:"
                tr -d '\r' < "$n" | sed 's/^/    | /'
            fi
            case "$board" in arty_a7_35t) id=0x0362d093; other=arty_a7_100t; oid=0x03631093 ;; *) id=0x03631093; other=arty_a7_35t; oid=0x0362d093 ;; esac
            if FAKE_LOG="$t/ofl.log" FAKE_IDCODE=$id OPENFPGALOADER=sim/board/tools/fake/openFPGALoader \
               make --no-print-directory program-volatile BOARD=$board DESIGN=$design > "$t/program-$board-$design.txt" 2>&1
            then prc=0; else prc=$?; fi
            echo "make program-volatile BOARD=$board DESIGN=$design (fake openFPGALoader with the board's IDCODE): exit $prc"
            sed 's/^/  /' "$t/program-$board-$design.txt" | sed "s|$PWD/||"
            echo "  it would run: $(grep -- ' -b ' "$t/ofl.log" | tail -n 1 | sed "s|$PWD/||")"
            if FAKE_LOG="$t/ofl.log" FAKE_IDCODE=$oid OPENFPGALOADER=sim/board/tools/fake/openFPGALoader \
               tools/program.sh volatile "$other" "$b/$design.bit" > "$t/program-x-$board-$design.txt" 2>&1
            then prc=0; else prc=$?; fi
            echo "the same file on an $other: exit $prc, $(tail -n 1 "$t/program-x-$board-$design.txt" | sed "s|$PWD/||" | sed 's/^program: //')"
        done
    done
} > "$out"

{
    echo "SurfCPU board track evidence: what the serial port should show with the SoC designs,"
    echo "EXPECTED FROM SIMULATION, never observed on a board."
    echo "Generated by sim/board/openxc7_soc_evidence.sh (make board-uart-expect BOARD=<board>) on"
    echo "$(date -u +%Y-%m-%d) at source $rev_state: the soc track's surfsim harness, built with surfcpu_soc"
    echo "configured as surfcpu_arty_a7_top configures it (HAS_DDR = 0, the board's BOARD_ID, this"
    echo "commit's BUILD_ID and BUILD_FLAGS, BUILD_TIME 0), runs the ROM images of this commit; 115200 8N1."
    echo "SurfBoot runs with a 64 MiB card from tools/mkcard.py in the socket, and the monitor gets"
    echo "info, sd and ls / typed on the UART. Characters exactly as sent, CR removed."
    for board in $boards; do
        echo
        echo "== $board: make board-uart-expect exit $(cat "$t/uart-expect-$board.rc")"
        grep '^uart_expect:' "$t/uart-expect-$board.txt" | sed 's/^/  /'
        echo "-- surfcpu_arty_a7_bramdiag (build/fw/bramdiag.mem):"
        tr -d '\r' < "$t/uart-$board/bramdiag.txt" | sed 's/^/  | /'
        echo "-- surfcpu_arty_a7_top (build/fw/surfboot.mem), the card:"
        sed 's/^/  /' "$t/uart-$board/mkcard.txt"
        tr -d '\r' < "$t/uart-$board/surfboot.txt" | sed 's/^/  | /'
        if [ -f "$t/uart-$board/screen.txt" ]; then
            echo "-- the VGA screen at the end of the SurfBoot run (text decoded from the pixels):"
            sed 's/^/  | /' "$t/uart-$board/screen.txt"
        fi
    done
} > "$uout"
echo "openxc7_soc_evidence: wrote $out and $uout"
