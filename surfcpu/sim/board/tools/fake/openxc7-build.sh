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
# It also writes pre.json and top.json, the elaborated RTL and the netlist as Yosys JSON, with
# the full top's boot ROM and multiplier (or nothing, for other tops), and an empty cells_sim.v.
echo "openxc7-build.sh $*" >> "$FAKE_LOG"
top= board= xdc= out= dsp= dry=0 srcs=
while [ $# -gt 0 ]; do
    case "$1" in
        --top) top=$2; shift 2 ;;
        --board) board=$2; shift 2 ;;
        --xdc) xdc=$2; shift 2 ;;
        --out) out=$2; shift 2 ;;
        --dsp) dsp=$2; shift 2 ;;
        --manifest-include) [ -f "$2" ] || { echo "fake openxc7: no $2" >&2; exit 2; }; shift 2 ;;
        -D) shift 2 ;;
        --lvs-timeout) case "$2" in ''|*[!0-9]*) echo "fake openxc7: --lvs-timeout $2" >&2; exit 2 ;; esac; shift 2 ;;
        --seed) case "$2" in ''|*[!0-9]*) echo "fake openxc7: --seed $2" >&2; exit 2 ;; esac; shift 2 ;;
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
if [ "${FAKE_OX_STOP:-}" = pnr ]; then
    { echo "Info: Max frequency for clock               'dram_clk': 72.98 MHz (FAIL at 100.00 MHz)"
      echo "Info: Max frequency for clock                'sys_clk': 65.24 MHz (PASS at 25.00 MHz)"
      echo "Info:     iter=45 wires=421923 overused=0 overuse=0 archfail=0"
      echo "Info: Routing complete."
      echo "ERROR: Max frequency for clock               'dram_clk': 72.98 MHz (FAIL at 100.00 MHz)"
      echo "Info: Max frequency for clock                'sys_clk': 65.24 MHz (PASS at 25.00 MHz)"
      echo "0 warnings, 2 errors"; } > "$out/nextpnr.log"
elif [ "${FAKE_OX_STOP:-}" = route ]; then
    { echo "Info: Max frequency for clock               'dram_clk': 84.07 MHz (FAIL at 100.00 MHz)"
      echo "Info: Max frequency for clock                'sys_clk': 77.83 MHz (PASS at 25.00 MHz)"
      echo "Info:     iter=100 wires=494177 overused=839 overuse=839 archfail=NA"
      echo "Info:     iter=101 wires=494496 overused=786 overuse=786 archfail=NA"; } > "$out/nextpnr.log"
fi
n=$(( ${#part} + 1 ))
{ printf '\000\011\017\360\017\360\017\360\017\360\000\000\001a\000\005fake\000b\000'
  printf "\\$(printf '%03o' "$n")"
  printf '%s\000c\000\01310/16/2026\000 fake bitstream\n' "$part"; } > "$out/$top.bit"
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
  echo "top                 $top"
  echo "part                $part (device $dev, chip database chipdb-fake.bin)"
  echo "board               $board"
  echo "bitstream           $out/$top.bit sha256:$(shasum -a 256 "$out/$top.bit" | cut -d' ' -f1) 99 bytes"
  echo "check synthesis     PASS (netlist against the RTL: flip-flops, memories, outputs: check-synthesis.txt)"
  echo "check bitstream     PASS (IDCODE, frames, commands, CRC: check-bitstream.txt)"
  echo "check round trip    PASS (bitstream decoded back to FASM: check-roundtrip.txt)"
  echo "check block RAM     none in the design (contents decoded from the bitstream: check-bram.txt)"
  echo "check LVS           ${FAKE_OX_LVS:-PASS} (routed netlist equivalent to the synthesis: check-lvs.txt)"
  echo "check clocks        ${FAKE_OX_CLOCKS:-PASS} (MMCM/PLL dividers against XAPP888: check-clocks.txt)"
  echo "## timing"
  grep '^clock ' "$out/timing.txt"
  echo "## tools (/etc/surfcpu-openxc7.txt)"
  echo "yosys                  Yosys 0.69+post (fake)"
  echo "nextpnr                \"nextpnr-himbaechel\" -- Next Generation Place and Route (Version 1.0.0)"; } > "$out/manifest.txt"
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
if [ "${FAKE_OX_STOP:-}" = pnr ] || [ "${FAKE_OX_STOP:-}" = route ]; then
    rm -f "$out/$top.bit" "$out/manifest.txt" "$out/timing.txt" "$out/utilization.txt" "$out/check-bitstream.txt"
    if [ "$FAKE_OX_STOP" = route ]; then echo "fake openxc7: stopped while routing" >&2; exit 137; fi
    echo "openxc7-build.sh: pnr failed (exit 1)" >&2
    exit 1
fi
exit "${FAKE_OX_RC:-0}"
