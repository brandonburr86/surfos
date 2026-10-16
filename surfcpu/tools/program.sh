#!/bin/sh
# SurfCPU: load a bitstream into the Arty A7 over its USB/JTAG cable with openFPGALoader
# (docs/BOARD.md section 9, docs/BUILD-HOST.md "Programming stays on the Mac").
#
#   tools/program.sh detect   BOARD          show what is on the JTAG chain
#   tools/program.sh volatile BOARD [BIT]    load configuration RAM (lost at power-off)
#   tools/program.sh flash    BOARD [BIT]    write the configuration flash (milestone 9; refused
#                                            unless SURFCPU_FLASH_CONFIRM=write-flash-BOARD, or
#                                            the board name is typed at the prompt)
#   make program-volatile BOARD=arty_a7_100t [DESIGN=arty_a7_baseline] [BIT=file.bit]
#
# BOARD is a line of boards/arty_a7/boards.txt. BIT defaults to the last build of DESIGN
# (default surfcpu_arty_a7_top): build/fpga/BOARD/DESIGN/DESIGN.bit. Before loading anything
# it checks that exactly one device is on the chain and that its IDCODE is BOARD's FPGA, that
# the bitstream was built for that FPGA, and (for flash) that its build manifest says every
# check passed; any mismatch is refused. OPENFPGALOADER overrides the program's path.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd -P)              # surfcpu/
me=program
die() { echo "$me: $*" >&2; exit 1; }

mode=${1:-}
board=${2:-}
case "$mode" in
    detect|volatile|flash) ;;
    *) die "usage: $0 detect|volatile|flash BOARD [BIT]" ;;
esac
[ -n "$board" ] || die "usage: $0 $mode BOARD [BIT] (BOARD: arty_a7_35t or arty_a7_100t)"
line=$(awk -v b="$board" '$1 == b { print }' "$root/boards/arty_a7/boards.txt")
[ -n "$line" ] || die "unknown BOARD '$board' (boards/arty_a7/boards.txt)"
ofl_board=$(echo "$line" | awk '{ print $3 }')
want_id=$(echo "$line" | awk '{ print $4 }')
bit_device=$(echo "$line" | awk '{ print $5 }')

ofl=${OPENFPGALOADER:-openFPGALoader}
if ! command -v "$ofl" >/dev/null 2>&1; then
    echo "$me: openFPGALoader is not installed: programming is pending (brew install openfpgaloader)" >&2
    exit 2
fi

# ------------------------------------------------------------------------------ the JTAG chain
detect_out=$("$ofl" -c digilent --detect 2>&1) || {
    echo "$detect_out" | sed "s/^/$me: | /" >&2
    die "openFPGALoader found no JTAG cable or device (is the Arty plugged in and powered?)"
}
ids=$(echo "$detect_out" | sed -n 's/.*idcode[[:space:]]*0x\([0-9a-fA-F]\{8\}\).*/\1/p')
count=$(echo "$ids" | grep -c . || true)
[ "$count" -ge 1 ] || die "no device on the JTAG chain"
[ "$count" -eq 1 ] || die "$count devices on the JTAG chain; refusing to guess which is the Arty"
got=$(printf '%d' "0x$ids")
got_masked=$(( got & 0x0FFFFFFF ))
want=$(( want_id ))
if [ "$got_masked" -ne "$want" ]; then
    die "the FPGA's IDCODE is 0x$ids, but BOARD=$board means $(printf '0x%08X' "$want") (bits 27:0); refusing"
fi
echo "$me: JTAG: IDCODE 0x$ids = BOARD $board ($bit_device, silicon revision $(( got >> 28 )))"
[ "$mode" = detect ] && exit 0

# ------------------------------------------------------------------------------ the bitstream
design=${DESIGN:-surfcpu_arty_a7_top}
bit=${3:-${BIT:-${SURFCPU_FPGA_OUT:-$root/build/fpga}/$board/$design/$design.bit}}
[ -f "$bit" ] || die "no bitstream $bit (make build-fpga BOARD=$board DESIGN=$design)"
hdr=$(head -c 256 "$bit" | LC_ALL=C tr -c '[:alnum:]_.-' '\n' | grep -E '^[0-9]+[a-z][0-9a-z]+$' | head -n 1 || true)
case "$hdr" in
    "$bit_device"*) ;;
    "") die "$bit has no part name in its header; is it a Vivado .bit file?" ;;
    *)  die "$bit was built for $hdr, but BOARD=$board is a $bit_device; refusing" ;;
esac
manifest="$(dirname "$bit")/manifest.txt"
if [ -f "$manifest" ]; then
    grep -q '^result = "pass"' "$manifest" || die "$manifest does not say the build passed its checks; refusing"
    passed=yes
else
    passed=no
fi

# ------------------------------------------------------------------------------ load
if [ "$mode" = volatile ]; then
    [ "$passed" = yes ] || echo "$me: note: no build manifest next to $bit (built elsewhere?)"
    echo "$me: loading $bit into configuration RAM (lost at power-off)"
    "$ofl" -b "$ofl_board" "$bit" || die "openFPGALoader failed"
    echo "$me: done; the FPGA runs the new design (press PROG or power-cycle to return to the flash)"
    exit 0
fi

# flash: milestone 9 only, on the owner's explicit word
[ "$passed" = yes ] || die "flash needs the bitstream's build manifest with result = pass; refusing"
confirm=${SURFCPU_FLASH_CONFIRM:-}
if [ -z "$confirm" ] && [ -t 0 ]; then
    echo "$me: this overwrites the board's configuration flash (it boots from it at power-on)."
    echo "$me: check the flash part first (INVENTORY.md G7) and keep a known-good bitstream."
    printf '%s: type the board name (%s) to write its flash: ' "$me" "$board"
    read -r answer
    [ "$answer" = "$board" ] && confirm="write-flash-$board"
fi
[ "$confirm" = "write-flash-$board" ] ||
    die "not confirmed: writing the configuration flash needs SURFCPU_FLASH_CONFIRM=write-flash-$board (or the prompt); refusing"
echo "$me: writing $bit to the configuration flash of $board"
"$ofl" -b "$ofl_board" -f --verify "$bit" || die "openFPGALoader failed while writing the flash"
echo "$me: flash written and verified; set the MODE jumper (JP1) to boot from it"
