#!/bin/sh
# SurfCPU board track: the DDR designs through the open flow, for the record. On the Mac, from
# surfcpu/, with Docker and both images, on a committed tree. Nothing is programmed and nothing
# leaves the Mac.
#   1. make build-rom and make litedram-gen (the generated LiteDRAM core, checked against
#      ip/litedram/arty.sha256);
#   2. make build-fpga FLOW=openxc7 for surfcpu_arty_a7_ddr_bramdiag and surfcpu_arty_a7_ddr on
#      both boards: the gated builds, whose results are the designs' (2026-10-16: nextpnr's timing
#      for LiteDRAM's 100 MHz clock fails, so they stop after place and route, without a
#      bitstream);
#   3. DIAGNOSTIC builds of the bramdiag design (the SurfBoot design differs only in its boot
#      ROM's contents; LVS takes most of their time, as their step times show) with
#      build/openxc7-diag/openxc7-build.sh, a copy of tools/openxc7-build.sh made here that lets
#      nextpnr finish despite the timing failure (--timing-allow-fail) and runs every check
#      after it anyway, into build/fpga-diag/: they show what the timing failure hides
#      (bitstream assembly, round trip, block RAM, LVS, the clock tiles, the board's own checks).
#      tools/fpga-build.sh still gates them on timing, so their results are fail and
#      tools/program.sh refuses their bitstreams (shown);
#   4. make program-volatile against the fake openFPGALoader of sim/board/tools/fake for each
#      gated build (nothing to load) and each diagnostic bitstream (refused).
# Writes docs/evidence/board/openxc7-ddr.txt. --report-only writes it again from the last run's
# outputs. DDR_EVIDENCE_DIAG=0 skips the diagnostic builds and reports the last ones again (each
# with the source its manifest names).
set -eu
report_only=0
[ "${1:-}" != --report-only ] || report_only=1

out=docs/evidence/board/openxc7-ddr.txt
designs="surfcpu_arty_a7_ddr_bramdiag surfcpu_arty_a7_ddr"
boards="arty_a7_100t arty_a7_35t"
t=build/sim/board/openxc7_ddr_evidence
diag=build/openxc7-diag/openxc7-build.sh
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
critical() {      # NEXTPNR_LOG: per clock, the last critical path's length, split, ends and depth
    python3 - "$1" <<'EOF'
import re, sys
text = open(sys.argv[1]).read()
last = {}
for m in re.finditer(r"Critical path report for (clock '[^']+'|cross-domain path '[^']+' -> '[^']+') \(?([^)\n]*)\)?:\n(.*?)\n\S*\s*([0-9.]+ ns logic, [0-9.]+ ns routing)", text, re.S):
    last[m.group(1)] = m.groups()
for what, edges, body, split in last.values():
    srcs = re.findall(r"Source (\S+)\.(\w+)", body)
    nets = re.findall(r"Net (\S+) \(", body)
    named = [n for n in nets if not n.startswith("$")]
    luts = sum(1 for c, p in srcs if p in ("O5", "O6"))
    totals = re.findall(r"^Info:\s+\S+\s+[0-9.]+\s+([0-9.]+) ", body, re.M)
    print(f"{what}: {totals[-1] if totals else '?'} ns = {split}; {luts} LUT levels; "
          f"from {named[0] if named else '?'} to {named[-1] if named else '?'}")
EOF
}

use() {           # STAT_JSON NEXTPNR_LOG: what synthesis made and what nextpnr placed
    python3 - "$1" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))["design"]["num_cells_by_type"]
n = lambda *ks: sum(v for k, v in d.items() if k.startswith(ks))
print(f"use (Yosys): LUTs {n('LUT', 'INV')} (LUT1-6 and INV), flip-flops {n('FD')}, RAMB36E1 {n('RAMB36E1')}, "
      f"RAMB18E1 {n('RAMB18E1')}, DSP48E1 {n('DSP48E1')}, BUFG {n('BUFG')}, PLLE2 {n('PLLE2')}, MMCME2 {n('MMCME2')}")
EOF
    awk '/Device utilisation/ { s = 1; next } s && /^Info: *$/ { exit }
         s && $2 ~ /^(PLLE2_ADV|MMCME2_ADV|BUFGCTRL|IDELAYCTRL|ISERDESE2|OSERDESE2|IDELAYE2)/ {
             k = $2; sub(/:$/, "", k); h = (length(k) - 1) / 2
             if (h == int(h) && substr(k, 1, h) == substr(k, h + 2)) k = substr(k, 1, h)
             l = l (l ? ", " : "") k " " $3 $4 }
         END { print "placed (nextpnr): " l }' "$2"
}

histogram() {     # NEXTPNR_LOG: nextpnr's last slack histogram
    awk '/Slack histogram:/ { n = 0; cap = 1; next }
         cap && /legend|represents|\[ *-?[0-9]+, *-?[0-9]+\) *\|/ { sub(/^Info: */, ""); b[n++] = $0; next }
         cap { cap = 0 }
         END { for (i = 0; i < n; i++) print "  " b[i] }' "$1"
}

fmax() {          # NEXTPNR_LOG: each clock's last "Max frequency" line
    awk '/Max frequency for clock/ { c = $0; sub(/.*clock +/, "", c); sub(/:.*/, "", c); last[c] = $0; if (!(c in seen)) { seen[c] = 1; order[n++] = c } }
         END { for (i = 0; i < n; i++) { l = last[order[i]]; sub(/^(Info|ERROR): */, "", l); print l } }' "$1"
}

if [ "$report_only" = 0 ]; then
if [ "${DDR_EVIDENCE_DIAG:-1}" = 1 ]; then
    rm -rf "$t"
elif [ -d "$t" ]; then           # keep the last diagnostic builds' records, drop the rest
    find "$t" -mindepth 1 -maxdepth 1 ! -name 'diag*' -exec rm -rf {} +
fi
mkdir -p "$t"
echo "$rev ($state)" > "$t/source"

# ------------------------------------------------------------------ 1. the ROMs and the core
make --no-print-directory build-rom > "$t/build-rom.txt" 2>&1 || { tail -20 "$t/build-rom.txt"; exit 1; }
for r in bramdiag surfboot; do shasum -a 256 "build/fw/$r.mem" | awk '{ print $1 }' > "$t/$r.sha"; done
make --no-print-directory litedram-gen > "$t/litedram-gen.txt" 2>&1 || { tail -20 "$t/litedram-gen.txt"; exit 1; }

# ------------------------------------------------------------------ 2. the gated builds (both boards at once)
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
done

# ------------------------------------------------------------------ 3. the diagnostic builds
if [ "${DDR_EVIDENCE_DIAG:-1}" = 1 ]; then
    mkdir -p "$(dirname "$diag")"
    sed -e 's#^here=$(cd "$(dirname "$0")/.." \&\& pwd -P)#here=$(cd "$(dirname "$0")/../.." \&\& pwd -P)#' \
        -e "s#exec devenv/run.sh sh tools/openxc7-build.sh#exec devenv/run.sh sh $diag#" \
        -e 's#--router router2 -o hold-fix#--router router2 --timing-allow-fail -o hold-fix#' \
        -e 's#--utilization "$out/utilization.txt"$#--utilization "$out/utilization.txt" || echo "DIAGNOSTIC: timing not met; the checks run anyway" >> "$out/times.txt"#' \
        tools/openxc7-build.sh > "$diag"
    chmod +x "$diag"
    echo "$rev ($state)" > "$t/diag-source"
    diff tools/openxc7-build.sh "$diag" > "$t/diag.diff" || true
    for design in ${DDR_EVIDENCE_DIAG_DESIGNS:-surfcpu_arty_a7_ddr_bramdiag}; do
        for board in $boards; do
            (
                t0=$(date +%s)
                if SURFCPU_FPGA_OUT=$PWD/build/fpga-diag SURFCPU_OPENXC7_BUILD=$PWD/$diag FLOW=openxc7 \
                       tools/fpga-build.sh --design $design $board > "$t/diag-$board-$design.txt" 2>&1
                then rc=0; else rc=$?; fi
                echo "$rc $(( $(date +%s) - t0 ))" > "$t/diag-$board-$design.status"
            ) &
        done
        wait
    done
fi
echo "$(( ($(date +%s) - started) / 60 ))" > "$t/minutes"
fi
[ -f "$t/minutes" ] || { echo "openxc7_ddr_evidence: no complete run in $t" >&2; exit 1; }

# ------------------------------------------------------------------ 4. and the record
{
    echo "SurfCPU board track evidence: the DDR designs through the open flow (milestone 4)"
    echo "Generated by sim/board/openxc7_ddr_evidence.sh on $(date -u +%Y-%m-%d) at source $(cat "$t/source"), in $(cat "$t/minutes") minutes$( [ "$report_only" = 0 ] || echo " (this record written again by --report-only at $rev)")."
    echo "Commands: make build-rom; make litedram-gen; make build-fpga FLOW=openxc7 BOARD=<board>"
    echo "DESIGN=<design> for both DDR designs on both boards (no build-host.conf); diagnostic builds"
    echo "(below); make program-volatile with the fake openFPGALoader. Nothing here was loaded on a"
    echo "board. nextpnr's timing is an estimate, not Vivado signoff. HAS_DDR = 1 (ARTY_HAS_DDR)."
    echo
    echo "boot ROM images (make build-rom at this commit):"
    for r in bramdiag surfboot; do echo "  build/fw/$r.mem  sha256 $(cat "$t/$r.sha")"; done
    echo "the generated LiteDRAM core (make litedram-gen; ip/litedram/arty.sha256):"
    grep -E '^lock.sh|^calibration BIOS|^PLL:' "$t/litedram-gen.txt" | sed 's/^/  /'
    echo
    echo "#################### the gated builds (make build-fpga FLOW=openxc7): the designs' results"
    for design in $designs; do
        for board in $boards; do
            b=build/fpga/$board/$design
            m=$b/manifest.txt
            ox=$b/openxc7
            echo
            echo "== $design on $board ($(top_key part "$m"))"
            echo "make exit status $(cut -d' ' -f1 "$t/$board-$design.status"), $(cut -d' ' -f2 "$t/$board-$design.status") s; result = $(top_key result "$m")$( [ "$(top_key result "$m")" = pass ] || echo " ($(top_key reason "$m"))")"
            echo "build: BUILD_ID $(top_key build_id "$m"), source $(top_key source_revision "$m"), dirty = $(top_key source_dirty "$m"); generated files match: $(top_key generated_files_match "$m")"
            [ ! -f "$b/$design.bit" ] || echo "bitstream: $b/$design.bit, sha256 $(shasum -a 256 "$b/$design.bit" | cut -d' ' -f1)"
            [ -f "$b/$design.bit" ] || echo "bitstream: none (the build stopped)"
            echo "steps:"
            sed 's/^/  /' "$ox/times.txt"
            echo "  synthesis check: $(grep '^synthcheck' "$ox/check-synthesis.txt"); flip-flops RTL / netlist / held: $(grep '^whole design' "$ox/check-synthesis.txt" | tr -s ' ' | sed 's/^whole design //')"
            echo "  the board's checks: check_netlist = $(field check_netlist "$m"), check_boot_rom = $(field check_boot_rom "$m")"
            use "$ox/stat.json" "$ox/nextpnr.log"
            if grep -q 'Routing complete' "$ox/nextpnr.log"; then
                echo "nextpnr's timing (Max frequency per clock, after routing):"
            else
                echo "nextpnr did not finish routing (router2's last iterations:"
                grep 'iter=' "$ox/nextpnr.log" | tail -n 2 | sed 's/^Info: */  /'
                echo "  ); its timing, the placer's estimates (Max frequency per clock, the last report):"
            fi
            fmax "$ox/nextpnr.log" | sed 's/^/  /'
            echo "  (LiteDRAM's 400 MHz clocks u_dram.crg_clkout_buf2/3 reach only OSERDESE2/ISERDESE2 clock pins:"
            echo "  nextpnr derives them but has no register-to-register path on them to time)"
            echo "critical paths (nextpnr, the last report):"
            critical "$ox/nextpnr.log" | sed 's/^/  /'
            grep -q 'Critical path report' "$ox/nextpnr.log" || echo "  (none: nextpnr reports them after routing)"
            echo "slack histogram (nextpnr, the last report; ns are in ps):"
            histogram "$ox/nextpnr.log"
            case "$board" in arty_a7_35t) id=0x0362d093 ;; *) id=0x03631093 ;; esac
            if FAKE_LOG="$t/ofl.log" FAKE_IDCODE=$id OPENFPGALOADER=sim/board/tools/fake/openFPGALoader \
               make --no-print-directory program-volatile BOARD=$board DESIGN=$design > "$t/program-$board-$design.txt" 2>&1
            then prc=0; else prc=$?; fi
            echo "make program-volatile BOARD=$board DESIGN=$design (fake openFPGALoader): exit $prc, $(grep '^program' "$t/program-$board-$design.txt" | tail -n 1 | sed "s|$PWD/||")"
        done
    done
    [ -f "$t/diag.diff" ] || { echo; echo "(no diagnostic builds in this run)"; exit 0; }
    echo
    echo "#################### DIAGNOSTIC builds: the same, nextpnr allowed to finish (NOT the designs' results)"
    [ ! -f "$t/diag-source" ] || [ "$(cat "$t/diag-source")" = "$(cat "$t/source")" ] ||
        echo "(from the run at source $(cat "$t/diag-source"); each build's manifest names its source)"
    echo "build/openxc7-diag/openxc7-build.sh is tools/openxc7-build.sh with these changes only:"
    sed 's/^/  /' "$t/diag.diff"
    for design in $designs; do
        for board in $boards; do
            [ -f "$t/diag-$board-$design.status" ] || continue
            b=build/fpga-diag/$board/$design
            m=$b/manifest.txt
            ox=$b/openxc7
            echo
            echo "== DIAGNOSTIC $design on $board"
            echo "exit $(cut -d' ' -f1 "$t/diag-$board-$design.status"), $(cut -d' ' -f2 "$t/diag-$board-$design.status") s; result = $(top_key result "$m") ($(top_key reason "$m"))"
            echo "build: BUILD_ID $(top_key build_id "$m"), source $(top_key source_revision "$m"), dirty = $(top_key source_dirty "$m"); generated files match: $(top_key generated_files_match "$m")"
            echo "bitstream (the flow's, not the design's): $ox/surfcpu_arty_a7_top.bit, $(wc -c < "$ox/surfcpu_arty_a7_top.bit" | tr -d ' ') bytes, sha256 $(shasum -a 256 "$ox/surfcpu_arty_a7_top.bit" | cut -d' ' -f1)"
            grep -E '^IDCODE written|^check for' "$ox/check-bitstream.txt" | sed 's/^/  /'
            echo "checks:"
            grep '^check ' "$ox/manifest.txt" | sed 's/^/  /'
            echo "  check_netlist = $(field check_netlist "$m"), check_boot_rom = $(field check_boot_rom "$m")"
            sed 's/^/    /' "$ox/check-netlist.txt" | grep -E 'u_dram|DSP48E1|netlist_check'
            [ ! -f "$ox/check-rom.txt" ] || grep -E '^romcheck' "$ox/check-rom.txt" | sed 's/^/    /'
            echo "  block RAM: $(grep -E '^RAMB(18|36)E1:' "$ox/check-bram.txt" | tr '\n' ';' | sed 's/;/; /g')$(grep '^bramcheck' "$ox/check-bram.txt")"
            echo "  LVS: $(grep -E 'proved|BLOCKED|differ' "$ox/check-lvs.txt" 2>/dev/null | tail -n 1)"
            grep -E '^ *(DIFFER|BLOCKED|UNKNOWN)' "$ox/check-lvs.txt" 2>/dev/null | sed 's/^ */    /' || true
            echo "  round trip: $(tail -n 1 "$ox/check-roundtrip.txt")"
            sed 's/^/  clocks: /' "$ox/check-clocks.txt"
            echo "timing (nextpnr):"
            grep '^clock ' "$ox/timing.txt" | sed 's/^/  /'
            echo "critical paths:"
            critical "$ox/nextpnr.log" | sed 's/^/  /'
            echo "utilization:"
            sed 's/^/  /' "$ox/utilization.txt"
            echo "step times:"
            sed 's/^/  /' "$ox/times.txt"
            case "$board" in arty_a7_35t) id=0x0362d093 ;; *) id=0x03631093 ;; esac
            for bit in "$b/$design.bit" "$ox/surfcpu_arty_a7_top.bit"; do
                [ -f "$bit" ] || { echo "program.sh volatile $board $bit: no such file (fpga-build.sh, gating on timing, wrote none)"; continue; }
                if FAKE_LOG="$t/ofl.log" FAKE_IDCODE=$id OPENFPGALOADER=sim/board/tools/fake/openFPGALoader \
                   tools/program.sh volatile $board "$bit" > "$t/program-diag.txt" 2>&1
                then prc=0; else prc=$?; fi
                echo "program.sh volatile $board $bit (fake openFPGALoader): exit $prc, $(grep '^program' "$t/program-diag.txt" | tail -n 1 | sed "s|$PWD/||")"
            done
        done
    done
} > "$out"
echo "openxc7_ddr_evidence: wrote $out"
