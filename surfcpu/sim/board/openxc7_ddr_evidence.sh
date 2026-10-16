#!/bin/sh
# SurfCPU board track: the DDR designs through the open flow, for the record. On the Mac, from
# surfcpu/, with Docker and both images, on a committed tree (the ROMs and bitstreams carry the
# commit); about 50 minutes. Nothing is programmed and nothing leaves the Mac.
#   1. make build-rom and make litedram-gen (the generated LiteDRAM core, checked against
#      ip/litedram/arty.sha256), once;
#   2. make build-fpga FLOW=openxc7 for surfcpu_arty_a7_ddr_bramdiag and surfcpu_arty_a7_ddr,
#      the two boards of a design at once (two nextpnr runs at a time): the designs' results,
#      every check of the flow included (round ddr-3: LiteDRAM without its CPU at 80 MHz; the
#      design lists give nextpnr a list of placer seeds, tried in order until one meets every
#      clock, docs/DDR.md section 10.3);
#   3. make program-volatile BOARD=... DESIGN=... against the fake openFPGALoader of
#      sim/board/tools/fake (its header, IDCODE and manifest checks only), and the same file
#      on the other board (refused).
# Writes docs/evidence/board/openxc7-ddr.txt. The bitstreams stay in build/fpga/ (never
# committed). --report-only writes the record again from the last run's outputs.
# (Round ddr-2's diagnostic builds, a copy of the flow that let nextpnr finish despite the
# timing failure, are gone: every check now runs in the designs' own builds.)
set -eu
report_only=0
[ "${1:-}" != --report-only ] || report_only=1

out=docs/evidence/board/openxc7-ddr.txt
designs="surfcpu_arty_a7_ddr_bramdiag surfcpu_arty_a7_ddr"
boards="arty_a7_100t arty_a7_35t"
t=build/sim/board/openxc7_ddr_evidence
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
critical() {      # TIMING: per clock, the critical path's length, split, LUT levels and ends
    [ -f "$1" ] || { echo "(no timing report: the build stopped earlier)"; return; }
    python3 - "$1" <<'EOF'
import re, sys
text = open(sys.argv[1]).read()
for m in re.finditer(r"Critical path report for (clock '[^']+'|cross-domain path '[^']+' -> '[^']+') \(?([^)\n]*)\)?:\n(.*?)\n(?:Info: *)?([0-9.]+ ns logic, [0-9.]+ ns routing)", text, re.S):
    what, edges, body, split = m.groups()
    srcs = re.findall(r"Source (\S+)\.(\w+)", body)
    nets = [re.sub(r"\$(legal|LUT)\d+$", "", n) for n in re.findall(r"Net (\S+) \(", body)]
    named = [n for n in nets if not n.startswith("$")]
    luts = sum(1 for c, p in srcs if p in ("O5", "O6"))
    totals = re.findall(r"^\s*\S+\s+[0-9.]+\s+([0-9.]+) ", body, re.M)
    sinks = re.findall(r"Sink (\S+)\.(\w+)", body)
    print(f"{what}: {totals[-1] if totals else '?'} ns = {split}; {luts} LUT levels; "
          f"from {named[0] if named else '?'}, last named net {named[-1] if named else '?'}, "
          f"ends at a {sinks[-1][1] if sinks else '?'} pin")
EOF
}
show() {          # FILE [PREFIX]: the file, indented, or a note that the build did not get that far
    if [ -f "$1" ]; then sed "s/^/${2:-  }/" "$1"; else echo "${2:-  }(not written: the build stopped earlier)"; fi
}
placed() {        # NEXTPNR_LOG: the clock tiles and I/O logic nextpnr placed
    awk '/Device utilisation/ { s = 1; next } s && /^Info: *$/ { exit }
         s && $2 ~ /^(PLLE2_ADV|MMCME2_ADV|BUFGCTRL|IDELAYCTRL|ISERDESE2|OSERDESE2|IDELAYE2)/ {
             k = $2; sub(/:$/, "", k); h = (length(k) - 1) / 2
             if (h == int(h) && substr(k, 1, h) == substr(k, h + 2)) k = substr(k, 1, h)
             l = l (l ? ", " : "") k " " $3 $4 }
         END { print "placed (nextpnr): " l }' "$1"
}
histogram() {     # NEXTPNR_LOG: nextpnr's last slack histogram
    awk '/Slack histogram:/ { n = 0; cap = 1; next }
         cap && /legend|represents|\[ *-?[0-9]+, *-?[0-9]+\) *\|/ { sub(/^Info: */, ""); b[n++] = $0; next }
         cap { cap = 0 }
         END { for (i = 0; i < n; i++) print "  " b[i] }' "$1"
}

if [ "$report_only" = 0 ]; then
rm -rf "$t"
mkdir -p "$t"
echo "$rev ($state)" > "$t/source"

# ------------------------------------------------------------------ 1. the ROMs and the core, once
make --no-print-directory build-rom > "$t/build-rom.txt" 2>&1 || { tail -20 "$t/build-rom.txt"; exit 1; }
for r in bramdiag surfboot; do shasum -a 256 "build/fw/$r.mem" | awk '{ print $1 }' > "$t/$r.sha"; done
cp build/fw/gen/fw_build.h "$t/fw_build.h"
make --no-print-directory litedram-gen > "$t/litedram-gen.txt" 2>&1 || { tail -20 "$t/litedram-gen.txt"; exit 1; }

# ------------------------------------------------------------------ 2. the builds (both boards at once)
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
echo "$(( ($(date +%s) - started) / 60 ))" > "$t/minutes"
fi
[ -f "$t/minutes" ] || { echo "openxc7_ddr_evidence: no complete run in $t" >&2; exit 1; }

# ------------------------------------------------------------------ 3. and the record
{
    echo "SurfCPU board track evidence: the DDR designs through the open flow (milestone 4)"
    echo "Generated by sim/board/openxc7_ddr_evidence.sh on $(date -u +%Y-%m-%d) at source $(cat "$t/source"), in $(cat "$t/minutes") minutes$( [ "$report_only" = 0 ] || echo " (this record written again by --report-only at $rev)")."
    echo "Commands: make build-rom; make litedram-gen; make build-fpga FLOW=openxc7 BOARD=<board>"
    echo "DESIGN=<design> for both DDR designs on both boards (no build-host.conf); make program-volatile"
    echo "with the fake openFPGALoader. Nothing here was loaded on a board: the physical behaviour of"
    echo "these bitstreams is pending. nextpnr's timing is an estimate, not Vivado signoff."
    echo "HAS_DDR = 1 (ARTY_HAS_DDR), DDR_SWINIT = 1: LiteDRAM without a CPU, its controller at 80 MHz."
    echo
    echo "boot ROM images (make build-rom at this commit):"
    for r in bramdiag surfboot; do echo "  build/fw/$r.mem  sha256 $(cat "$t/$r.sha")"; done
    grep '^#define' "$t/fw_build.h" | sed 's/^#define /  firmware /'
    echo "the generated LiteDRAM core (make litedram-gen; ip/litedram/arty.sha256):"
    grep -E '^PLL:|^register map|^PHY settings and init_sequence|^estimate|^lock.sh' "$t/litedram-gen.txt" | sed 's/^/  /'
    for design in $designs; do
        for board in $boards; do
            b=build/fpga/$board/$design
            m=$b/manifest.txt
            ox=$b/openxc7
            echo
            echo "== $design on $board ($(top_key part "$m"); placed and timed as $(field part "$m"))"
            echo "make exit status $(cut -d' ' -f1 "$t/$board-$design.status"), $(cut -d' ' -f2 "$t/$board-$design.status") s; result = $(top_key result "$m")$( [ "$(top_key result "$m")" = pass ] || echo " ($(top_key reason "$m"))")"
            echo "image: $(field image "$m"); $(field yosys "$m" | cut -d'(' -f1)/ $(field nextpnr "$m" | sed 's/.*(\(.*\))/nextpnr-himbaechel \1/')"
            echo "build: BUILD_ID $(top_key build_id "$m"), source $(top_key source_revision "$m"), dirty = $(top_key source_dirty "$m"), BUILD_TIME $(top_key build_time "$m"); generated files match: $(top_key generated_files_match "$m")"
            echo "boot ROM: $(top_key rom_image "$m"), sha256 $(top_key rom_sha256 "$m")"
            echo "placer seeds tried (the design list's seeds, in order, until one meets every clock):"
            show "$ox/seeds.txt"
            if [ -f "$b/$design.bit" ]; then
                echo "bitstream: $b/$design.bit"
                echo "  $(wc -c < "$b/$design.bit" | tr -d ' ') bytes, sha256 $(shasum -a 256 "$b/$design.bit" | cut -d' ' -f1)"
                echo "  manifest bitstream_sha256 $(top_key bitstream_sha256 "$m")"
                echo "  $(grep '^header:' "$ox/check-bitstream.txt" | sed "s/ date .*,/,/")"
                grep -E '^IDCODE written|^  = |stream [0-9]+ bits|^frame data|^check for' "$ox/check-bitstream.txt" | sed 's/^/  /'
            else
                echo "bitstream: none (the build stopped)"
            fi
            echo "checks (tools/openxc7-build.sh, then tools/fpga-build.sh's own):"
            grep '^check ' "$ox/manifest.txt" 2>/dev/null | sed 's/^/  /'
            echo "  check_netlist = $(field check_netlist "$m"), check_boot_rom = $(field check_boot_rom "$m")"
            if [ -f "$ox/check-synthesis.txt" ]; then
                echo "  synthesis: flip-flops in the RTL / in the netlist / held in DSP, RAM or SRL: $(grep '^whole design' "$ox/check-synthesis.txt" | tr -s ' ' | sed 's/^whole design //')"
                grep -E '^memories|^outputs constant|^merged|^synthcheck' "$ox/check-synthesis.txt" | sed 's/^/    /'
                grep -E '^u_dram ' "$ox/check-synthesis.txt" | tr -s ' ' | sed 's/^/    LiteDRAM (RTL, netlist, held): /'
            fi
            echo "  memories and multiplier (boards/arty_a7/netlist_check.py):"
            show "$ox/check-netlist.txt" "    "
            echo "  boot ROM (boards/arty_a7/romcheck.py):"
            show "$ox/check-rom.txt" "    "
            if [ -f "$ox/check-bram.txt" ]; then
                echo "  block RAM: $(grep -v '^bramcheck' "$ox/check-bram.txt" | head -n 1); $(grep '^bramcheck' "$ox/check-bram.txt")"
            fi
            if [ -f "$ox/check-lvs.txt" ]; then
                echo "  LVS: $(grep -E 'proved' "$ox/check-lvs.txt" | tail -n 1)"
                grep -E '^ *(DIFFER|BLOCKED|UNKNOWN)' "$ox/check-lvs.txt" | sed 's/^ */    /' || true
                grep -E '^bramwe' "$ox/check-lvs.txt" | sed 's/^/    /' || true
            fi
            [ ! -f "$ox/check-roundtrip.txt" ] || echo "  round trip: $(tail -n 1 "$ox/check-roundtrip.txt")"
            [ ! -f "$ox/check-clocks.txt" ] || sed 's/^/  clocks: /' "$ox/check-clocks.txt"
            echo "timing (nextpnr, after routing):"
            if [ -f "$ox/timing.txt" ]; then
                grep -E '^clock |^Hold-fix|^nextpnr warnings' "$ox/timing.txt" | sed 's/^/  /'
                echo "  (LiteDRAM's 320 MHz clocks reach only OSERDESE2/ISERDESE2 clock pins: nextpnr derives them"
                echo "  but has no register-to-register path on them to time)"
                sed -n '/^Cross-domain/,/^$/p' "$ox/timing.txt" | sed '/^$/d' | sed 's/^/  /'
            else
                echo "  (not written: the build stopped earlier)"
            fi
            echo "  critical paths:"
            critical "$ox/timing.txt" | sed 's/^/    /'
            echo "  slack histogram (nextpnr's last; ps):"
            [ ! -f "$ox/nextpnr.log" ] || histogram "$ox/nextpnr.log" | sed 's/^/  /'
            echo "utilization:"
            if [ -f "$ox/utilization.txt" ]; then
                sed -n '/documented capacity/,/DSP48E1/p' "$ox/utilization.txt" | sed 's/^/  /'
                grep '^Yosys cells' "$ox/utilization.txt" | sed 's/^/  /'
            fi
            [ ! -f "$ox/nextpnr.log" ] || placed "$ox/nextpnr.log" | sed 's/^/  /'
            echo "constraints: nextpnr given $(field constraints_given "$m") lines; $(grep -c . "$b/nextpnr-not-checked.txt" 2>/dev/null) not checked by the open flow ($b/nextpnr-not-checked.txt)"
            echo "step times:"
            show "$ox/times.txt"
            case "$board" in arty_a7_35t) id=0x0362d093; other=arty_a7_100t; oid=0x03631093 ;; *) id=0x03631093; other=arty_a7_35t; oid=0x0362d093 ;; esac
            if FAKE_LOG="$t/ofl.log" FAKE_IDCODE=$id OPENFPGALOADER=sim/board/tools/fake/openFPGALoader \
               make --no-print-directory program-volatile BOARD=$board DESIGN=$design > "$t/program-$board-$design.txt" 2>&1
            then prc=0; else prc=$?; fi
            echo "make program-volatile BOARD=$board DESIGN=$design (fake openFPGALoader with the board's IDCODE): exit $prc"
            sed 's/^/  /' "$t/program-$board-$design.txt" | sed "s|$PWD/||"
            echo "  it would run: $(grep -- ' -b ' "$t/ofl.log" 2>/dev/null | tail -n 1 | sed "s|$PWD/||")"
            if [ -f "$b/$design.bit" ]; then
                if FAKE_LOG="$t/ofl.log" FAKE_IDCODE=$oid OPENFPGALOADER=sim/board/tools/fake/openFPGALoader \
                   tools/program.sh volatile "$other" "$b/$design.bit" > "$t/program-x-$board-$design.txt" 2>&1
                then prc=0; else prc=$?; fi
                echo "the same file on an $other: exit $prc, $(tail -n 1 "$t/program-x-$board-$design.txt" | sed "s|$PWD/||" | sed 's/^program: //')"
            fi
        done
    done
} > "$out"
echo "openxc7_ddr_evidence: wrote $out"
