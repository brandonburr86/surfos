#!/bin/sh
# Fake tools/openxc7-build.sh for the board track's wrapper tests (SURFCPU_OPENXC7_BUILD points
# here): logs the call, checks the arguments tools/fpga-build.sh gives the real script (the
# files exist, --dsp safe), and writes the outputs fpga-build.sh reads, in the real script's
# formats: <top>.bit (a .bit header naming the part), manifest.txt, timing.txt,
# utilization.txt and check-bitstream.txt. Runs no tool and builds nothing.
#   FAKE_OX_LVS      the LVS result (default PASS), e.g. "BLOCKED (timeout)"
#   FAKE_OX_CLOCKS   the clock check's result (default PASS)
#   FAKE_OX_TIMING   PASS (default) or FAIL for the sys_clk line
#   FAKE_OX_RC       the exit status (default 0)
#   FAKE_OX_NETLIST  lost: the synthesized netlist (top.json) has lost the boot ROM's block RAM
#   FAKE_OX_STOP     pnr: stop as the real script does when nextpnr misses a clock's constraint:
#                    the synthesis outputs and nextpnr.log only, no bitstream, no manifest, exit 1;
#                    route: the same, but stopped (exit 137) while router2 had not converged, so
#                    nextpnr.log holds only the placer's estimates and the router's iterations
#   FAKE_OX_FAIL_SEEDS  the seeds of --seed's list whose placement misses dram_clk (as pnr above);
#                    the first other seed in the list passes; seeds.txt records each attempt as the
#                    real script does, and with every seed failing the run stops as with pnr
#   FAKE_OX_THIN_SEEDS  the seeds whose placement meets every clock with less slack than the rule
#                    wants (thin): the next seed is tried; with every seed thin the first is kept
#                    and the manifest's slack margin says THIN, as the real script does
#   FAKE_OX_SLACK    a recheck's slack margin (default PASS)
# --recheck --out DIR [--lvs-solver S] [--lvs-timeout T] [--dry-run], as the real script's (format 2):
# the build's manifest (manifest.build.txt once a recheck has kept a copy, else manifest.txt) must be
# there; a copy is kept as manifest.build.txt (once), and the new manifest, written aside, replaces
# manifest.txt at the end: the build's lines up to its bitstream and its SRL and synthesis results,
# with the checks run again (FAKE_OX_LVS, FAKE_OX_CLOCKS as above) and the recheck's lines.
# FAKE_OX_RECHECK=refuse refuses as the real one does (exit 3, nothing changed); =die stops after
# keeping the copy (exit 137), =tie refuses after it began (exit 4, its message in recheck-refused.txt:
# the LVS inputs made again are not the build's), =exit4 stops with exit 4 and no such message (another
# program's 4, GNU sed's on a write error say): each way manifest.txt is as it was.
# It also writes pre.json and top.json, the elaborated RTL and the netlist as Yosys JSON, with
# the full top's boot ROM and multiplier (or nothing, for other tops), and an empty cells_sim.v.
echo "openxc7-build.sh $*" >> "$FAKE_LOG"
top= board= xdc= out= dsp= dry=0 srcs= seeds=1 slack_clocks= solver=libz3 limit=1800 incs= defs=
if [ "${1:-}" = --recheck ]; then
    shift
    limit= solver=
    while [ $# -gt 0 ]; do
        case "$1" in
            --out) out=$2; shift 2 ;;
            --lvs-solver) case "$2" in libz3|z3cmd) solver=$2 ;; *) echo "fake openxc7: --lvs-solver $2" >&2; exit 2 ;; esac; shift 2 ;;
            --lvs-timeout) case "$2" in ''|*[!0-9]*) echo "fake openxc7: --lvs-timeout $2" >&2; exit 2 ;; esac; limit=$2; shift 2 ;;
            --dry-run) dry=1; shift ;;
            *) echo "fake openxc7: --recheck takes no $1" >&2; exit 2 ;;
        esac
    done
    rec=$out/manifest.txt
    [ ! -f "$out/manifest.build.txt" ] || rec=$out/manifest.build.txt
    [ -f "$rec" ] || { echo "fake openxc7: --recheck: no manifest in $out; refusing (nothing was changed)" >&2; exit 3; }
    [ "${FAKE_OX_RECHECK:-}" != refuse ] || { echo "fake openxc7: --recheck: refusing (nothing was changed)" >&2; exit 3; }
    if [ "$dry" = 1 ]; then echo "[lvs-prove] fake recheck dry run, solver ${solver:-as built}"; exit 0; fi
    [ "$rec" != "$out/manifest.txt" ] || { cp "$out/manifest.txt" "$out/manifest.build.txt"; rec=$out/manifest.build.txt; }
    [ "${FAKE_OX_RECHECK:-}" != die ] || { echo "fake openxc7: killed while rechecking" >&2; exit 137; }
    if [ "${FAKE_OX_RECHECK:-}" = tie ]; then
        echo "fake openxc7: --recheck: lvs/gold.v made again is not the build's; refusing (exit 4)" | tee "$out/recheck-refused.txt" >&2
        exit 4
    fi
    [ "${FAKE_OX_RECHECK:-}" != exit4 ] || { echo "fake openxc7: sed: couldn't flush stdout: No space left on device" >&2; exit 4; }
    [ -n "$solver" ] || solver=$(sed -n 's/^lvs solver  *\([a-z0-9]*\).*/\1/p' "$rec"); solver=${solver:-libz3}
    [ -n "$limit" ] || limit=$(sed -n 's/^lvs timeout  *\([0-9]*\).*/\1/p' "$rec")
    top=$(sed -n 's/^top  *//p' "$rec" | head -n 1)
    srl=$(awk 'substr($0, 1, 20) == "check SRL cells     " { print $4; exit }' "$rec")
    syn=$(awk 'substr($0, 1, 20) == "check synthesis     " { print $3; exit }' "$rec")
    { echo "# openXC7 build manifest (tools/openxc7-build.sh), rechecked (--recheck); physical behaviour: pending (never on a board)"
      echo "manifest format     2 (docs/OPEN-FLOW.md section 3)"
      awk 'NR > 1 && /^check / { exit } NR > 1' "$rec" | grep -v '^manifest format '
      echo "check SRL cells     $srl (kept from the build: check-srl.txt)"
      echo "check synthesis     $syn (kept from the build: check-synthesis.txt)"
      echo "check bitstream     PASS (run again: IDCODE, frames, commands, CRC: check-bitstream.txt)"
      echo "check round trip    PASS (run again: bitstream decoded back to FASM: check-roundtrip.txt)"
      echo "check block RAM     none in the design (run again: contents decoded from the bitstream: check-bram.txt)"
      echo "check LVS           ${FAKE_OX_LVS:-PASS} (run again: routed netlist equivalent to the synthesis, lvs_equiv by the $solver route: check-lvs.txt)"
      echo "lvs solver          $solver (fake)"
      echo "lvs timeout         $limit s"
      echo "check clocks        ${FAKE_OX_CLOCKS:-PASS} (run again: MMCM/PLL dividers against XAPP888: check-clocks.txt)"
      echo "check slack margin  ${FAKE_OX_SLACK:-PASS} (run again: least margin dram_clk +1.513 ns, wants 0.500: slack-seed1.txt)"
      echo "recheck             2026-10-27T12:00:00Z, tools/openxc7-build.sh --recheck at fake dirty=0, image fake"
      echo "checks run again    bitstream, round trip, block RAM, LVS, clocks, slack margin, timing and utilization"
      echo "checks kept         SRL cells, synthesis: the build's"
      echo "## timing"; grep '^clock ' "$out/timing.txt"
      sed -n '/^## tools/,$p' "$rec"; } > "$out/manifest.txt.new"
    mv -f "$out/manifest.txt.new" "$out/manifest.txt"
    exit "${FAKE_OX_RC:-0}"
fi
while [ $# -gt 0 ]; do
    case "$1" in
        --top) top=$2; shift 2 ;;
        --board) board=$2; shift 2 ;;
        --xdc) xdc=$2; shift 2 ;;
        --out) out=$2; shift 2 ;;
        --dsp) dsp=$2; shift 2 ;;
        --manifest-include) [ -f "$2" ] || { echo "fake openxc7: no $2" >&2; exit 2; }; incs="$incs $2"; shift 2 ;;
        -D) defs="$defs $2"; shift 2 ;;
        --lvs-timeout) case "$2" in ''|*[!0-9]*) echo "fake openxc7: --lvs-timeout $2" >&2; exit 2 ;; esac; limit=$2; shift 2 ;;
        --lvs-solver) case "$2" in libz3|z3cmd) solver=$2 ;; *) echo "fake openxc7: --lvs-solver $2" >&2; exit 2 ;; esac; shift 2 ;;
        --pnr-timeout) case "$2" in ''|*[!0-9]*) echo "fake openxc7: --pnr-timeout $2" >&2; exit 2 ;; esac
                       [ "$2" -gt 0 ] || { echo "fake openxc7: --pnr-timeout $2" >&2; exit 2; }; shift 2 ;;
        --min-slack) case "$2" in *=*=*|=*|*=|*[!a-zA-Z0-9_.=]*) echo "fake openxc7: --min-slack $2" >&2; exit 2 ;; *=*) ;;
                     *) echo "fake openxc7: --min-slack $2" >&2; exit 2 ;; esac
                     case " $slack_clocks " in *" ${2%%=*} "*) echo "fake openxc7: --min-slack ${2%%=*} twice" >&2; exit 2 ;; esac
                     slack_clocks="$slack_clocks ${2%%=*}"; shift 2 ;;
        --seed) case "$2" in ''|*[!0-9,]*|,*|*,|*,,*) echo "fake openxc7: --seed $2" >&2; exit 2 ;; esac; seeds=$(echo "$2" | tr ',' ' '); shift 2 ;;
        -I) [ -d "$2" ] || { echo "fake openxc7: no include directory $2" >&2; exit 2; }; shift 2 ;;
        --dry-run) dry=1; shift ;;
        -*) echo "fake openxc7: unexpected option $1" >&2; exit 2 ;;
        *) [ -f "$1" ] || { echo "fake openxc7: no source $1" >&2; exit 2; }; srcs="$srcs $1"; shift ;;
    esac
done
[ -n "$top" ] && [ -f "$xdc" ] && [ -n "$out" ] && [ -n "$srcs" ] || { echo "fake openxc7: --top, --xdc, --out and sources needed" >&2; exit 2; }
[ "$dsp" = safe ] || { echo "fake openxc7: --dsp $dsp (fpga-build.sh must ask for safe)" >&2; exit 2; }
if [ "$dry" = 1 ]; then echo "[pnr] nextpnr-himbaechel (fake dry run) -o xdc=$xdc"; exit 0; fi
case "$board" in
    arty_a7_35t)  part=xc7a35tcsg324-1;  dev=xc7a35t;  id=0x0362D093 ;;
    arty_a7_100t) part=xc7a100tcsg324-1; dev=xc7a100t; id=0x03631093 ;;
    *) echo "fake openxc7: unknown board $board" >&2; exit 2 ;;
esac
mkdir -p "$out"
rm -f "$out/seeds.txt"
missed() {         # SEED: the nextpnr.log of a placement that misses dram_clk
    f=72.98
    { echo "Info: Max frequency for clock               'dram_clk': $f MHz (FAIL at 80.00 MHz)"
      echo "Info: Max frequency for clock                'sys_clk': 65.24 MHz (PASS at 25.00 MHz)"
      echo "Info:     iter=45 wires=421923 overused=0 overuse=0 archfail=0"
      echo "Info: Routing complete."
      echo "ERROR: Max frequency for clock               'dram_clk': $f MHz (FAIL at 80.00 MHz)"
      echo "Info: Max frequency for clock                'sys_clk': 65.24 MHz (PASS at 25.00 MHz)"
      echo "0 warnings, 2 errors"; } > "$out/nextpnr.log"
    echo "seed $1: fail (nextpnr exit 1), routed; 'dram_clk': $f MHz (FAIL at 80.00 MHz), 'sys_clk': 65.24 MHz (PASS at 25.00 MHz)" >> "$out/seeds.txt"
}
stop=${FAKE_OX_STOP:-}
slack="PASS (least margin dram_clk +1.513 ns, wants 0.500: slack-seed1.txt)"
if [ -z "$stop" ]; then
    passed= thin=
    for sd in $seeds; do
        case " ${FAKE_OX_FAIL_SEEDS:-} " in
            *" $sd "*) missed "$sd"; continue ;;
        esac
        case " ${FAKE_OX_THIN_SEEDS:-} " in
            *" $sd "*) echo "seed $sd: thin, routed; 'dram_clk': 80.56 MHz (PASS at 80.00 MHz), 'sys_clk': 65.24 MHz (PASS at 25.00 MHz); slack: dram_clk +0.087 ns, wants 0.500" >> "$out/seeds.txt"
                       [ -n "$thin" ] || thin=$sd
                       continue ;;
        esac
        passed=$sd
        { echo "Info: Max frequency for clock               'dram_clk': 91.02 MHz (PASS at 80.00 MHz)"
          echo "Info: Max frequency for clock                'sys_clk': 65.24 MHz (PASS at 25.00 MHz)"
          echo "Info: Routing complete."; } > "$out/nextpnr.log"
        echo "seed $sd: pass, routed; 'dram_clk': 91.02 MHz (PASS at 80.00 MHz), 'sys_clk': 65.24 MHz (PASS at 25.00 MHz); slack: least margin dram_clk +1.513 ns, wants 0.500" >> "$out/seeds.txt"
        slack="PASS (least margin dram_clk +1.513 ns, wants 0.500: slack-seed$sd.txt)"
        break
    done
    if [ -z "$passed" ] && [ -n "$thin" ]; then
        passed=$thin
        echo "kept: seed $thin, the thin placement with the most margin (THIN)" >> "$out/seeds.txt"
        slack="THIN (dram_clk +0.087 ns, wants 0.500; every seed tried was thin or failed, this one has the most margin: slack-seed$thin.txt)"
    fi
    [ -n "$passed" ] || stop=pnr
elif [ "$stop" = pnr ]; then
    for sd in $seeds; do missed "$sd"; done
elif [ "$stop" = route ]; then
    { echo "Info: Max frequency for clock               'dram_clk': 84.07 MHz (FAIL at 80.00 MHz)"
      echo "Info: Max frequency for clock                'sys_clk': 77.83 MHz (PASS at 25.00 MHz)"
      echo "Info:     iter=100 wires=494177 overused=839 overuse=839 archfail=NA"
      echo "Info:     iter=101 wires=494496 overused=786 overuse=786 archfail=NA"; } > "$out/nextpnr.log"
fi
n=$(( ${#part} + 1 ))
{ printf '\000\011\017\360\017\360\017\360\017\360\000\000\001a\000\005fake\000b\000'
  printf "\\$(printf '%03o' "$n")"
  printf '%s\000c\000\01310/16/2026\000d\000\01112:00:00\000e\000\000\000\020 fake bitstream\n' "$part"; } > "$out/$top.bit"
tim=${FAKE_OX_TIMING:-PASS}
{ echo "Timing (nextpnr's model after routing; an estimate, not Vivado signoff)"
  echo
  echo "clock pix_clk: constraint 25.17 MHz, achieved 194.02 MHz, slack +34.570 ns  PASS"
  echo "clock sys_clk: constraint 25.00 MHz, achieved 143.53 MHz, slack +33.033 ns  $tim"; } > "$out/timing.txt"
{ echo "Against the $dev's documented capacity (AMD DS180):"
  echo "  LUTs (Yosys LUT1-6 and INV cells; packing can only reduce)      285 / 20800     1.4%"
  echo "  LUT positions (nextpnr SLICE_LUTX, two per LUT6)                633 / 41600     1.5%"
  echo "  flip-flops                                                      412 / 41600     1.0%"
  echo "  carry chains (CARRY4, one per slice)                             51 / 5200      1.0%"
  echo "  36 Kb block RAMs (RAMB18 = 1/2)                                   0 / 50        0.0%"
  echo "  DSP48E1                                                           0 / 90        0.0%"; } > "$out/utilization.txt"
{ echo "header: design 'fake.frames;Generator=xc7frames2bit' part '$part' date 2026/10/16 00:00:00, stream 99 bytes"
  echo "IDCODE written: $id"
  echo "check for $dev: PASS"; } > "$out/check-bitstream.txt"
{ echo "# openXC7 build manifest (tools/openxc7-build.sh); physical behaviour: pending (never on a board)"
  echo "manifest format     2 (docs/OPEN-FLOW.md section 3)"
  echo "top                 $top"
  echo "part                $part (device $dev, chip database chipdb-fake.bin)"
  echo "board               $board"
  echo "seed                ${passed:-1} (of $seeds: $(grep -c '^seed ' "$out/seeds.txt" 2>/dev/null || echo 0) tried, seeds.txt)"
  echo "defines            ${defs:- (none)}"
  echo "bitstream           $out/$top.bit sha256:$(shasum -a 256 "$out/$top.bit" | cut -d' ' -f1) 99 bytes"
  echo "check SRL cells     ${FAKE_OX_SRL:-PASS} (none in the synthesized netlist, which LVS could not check: check-srl.txt)"
  echo "check synthesis     PASS (netlist against the RTL: flip-flops, memories, outputs: check-synthesis.txt)"
  echo "check bitstream     PASS (IDCODE, frames, commands, CRC: check-bitstream.txt)"
  echo "check round trip    PASS (bitstream decoded back to FASM: check-roundtrip.txt)"
  echo "check block RAM     none in the design (contents decoded from the bitstream: check-bram.txt)"
  echo "check LVS           ${FAKE_OX_LVS:-PASS} (routed netlist equivalent to the synthesis, lvs_equiv by the $solver route: check-lvs.txt)"
  echo "lvs solver          $solver (fake)"
  echo "lvs timeout         $limit s"
  echo "check clocks        ${FAKE_OX_CLOCKS:-PASS} (MMCM/PLL dividers against XAPP888: check-clocks.txt)"
  echo "check slack margin  $slack"
  echo "## timing"
  grep '^clock ' "$out/timing.txt"
  echo "## tools (/etc/surfcpu-openxc7.txt)"
  echo "yosys                  Yosys 0.69+post (fake)"
  echo "nextpnr                \"nextpnr-himbaechel\" -- Next Generation Place and Route (Version 1.0.0)"
  for e in $incs; do echo "## included: $e"; cat "$e"; done; } > "$out/manifest.txt"
: > "$out/cells_sim.v"
if [ "$top" = surfcpu_arty_a7_top ]; then
    printf '{"modules": {"%s": {"cells": {"rom": {"type": "$mem_v2", "parameters": {"MEMID": "\\\\u_soc.u_bootrom.rom", "WIDTH": 32, "SIZE": 16384}, "connections": {}}}, "netnames": {}}}}\n' "$top" > "$out/pre.json"
    cells=
    if [ "${FAKE_OX_NETLIST:-}" != lost ]; then
        for i in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14; do
            cells="$cells\"u_soc.u_bootrom.rom.0.$i\": {\"type\": \"RAMB36E1\", \"parameters\": {}, \"connections\": {}}, "
        done
    fi
    for i in 0 1 2 3; do
        cells="$cells\"dsp$i\": {\"type\": \"DSP48E1\", \"parameters\": {}, \"connections\": {\"A\": [7]}}, "
    done
    printf '{"modules": {"%s": {"cells": {%s"end": {"type": "INV", "connections": {}}}, "netnames": {"u_soc.u_core.muldiv.ma": {"bits": [7]}}}}}\n' "$top" "$cells" > "$out/$top.json"
else
    printf '{"modules": {"%s": {"cells": {}, "netnames": {}}}}\n' "$top" > "$out/pre.json"
    cp "$out/pre.json" "$out/$top.json"
fi
if [ "$stop" = pnr ] || [ "$stop" = route ]; then
    rm -f "$out/$top.bit" "$out/manifest.txt" "$out/timing.txt" "$out/utilization.txt" "$out/check-bitstream.txt"
    if [ "$stop" = route ]; then echo "fake openxc7: stopped while routing" >&2; exit 137; fi
    echo "openxc7-build.sh: pnr failed (exit 1)" >&2
    exit 1
fi
exit "${FAKE_OX_RC:-0}"
