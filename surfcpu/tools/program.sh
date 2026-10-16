#!/bin/sh
# SurfCPU: load a bitstream into the Arty A7 over its USB/JTAG cable with openFPGALoader
# (docs/BOARD.md section 9, docs/BUILD-HOST.md "Programming stays on the Mac").
#
#   tools/program.sh [--ftdi-serial S] detect   BOARD        show what is on the JTAG chain
#   tools/program.sh [--ftdi-serial S] volatile BOARD [BIT]  load configuration RAM (lost at power-off)
#   tools/program.sh [--ftdi-serial S] flash    BOARD [BIT]  write the configuration flash (milestone 9;
#                                          refused unless SURFCPU_FLASH_CONFIRM=write-flash-BOARD,
#                                          or the board name is typed at the prompt)
#   make program-volatile BOARD=arty_a7_100t [DESIGN=arty_a7_baseline] [BIT=file.bit] [CABLE_SERIAL=S]
#
# BOARD is a line of boards/arty_a7/boards.txt. BIT defaults to the last build of DESIGN
# (default surfcpu_arty_a7_top): build/fpga/BOARD/DESIGN/DESIGN.bit. Before loading anything it
# checks the following, and refuses on any mismatch:
#   cable      one FT2232H (USB 0403:6010, the Arty's) among the USB devices openFPGALoader
#              --scan-usb lists, or the one whose serial number --ftdi-serial or CABLE_SERIAL names
#   chain      exactly one device, whose IDCODE bits 27:0 are BOARD's FPGA
#   bitstream  its header names BOARD's FPGA and package (boards.txt, bit_parts)
#   manifest   the build manifest next to it (manifest.txt), if any, says every check passed and
#              describes this file (SHA-256); flash needs one from tools/fpga-build.sh, for this
#              board and part. A flow manifest (tools/openxc7-build.sh's own) must also show
#              every clock meeting nextpnr's timing
# OPENFPGALOADER overrides the program's path.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd -P)              # surfcpu/
me=program
die() { echo "$me: $*" >&2; exit 1; }

serial=${CABLE_SERIAL:-}
while [ $# -gt 0 ]; do
    case "$1" in
        --ftdi-serial)   [ $# -ge 2 ] || die "--ftdi-serial needs the cable's serial number"; serial=$2; shift 2 ;;
        --ftdi-serial=*) serial=${1#*=}; shift ;;
        -h|--help)       sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*)              die "unknown option $1" ;;
        *)               break ;;
    esac
done
mode=${1:-}
board=${2:-}
case "$mode" in
    detect|volatile|flash) ;;
    *) die "usage: $0 [--ftdi-serial S] detect|volatile|flash BOARD [BIT]" ;;
esac
[ -n "$board" ] || die "usage: $0 $mode BOARD [BIT] (BOARD: arty_a7_35t or arty_a7_100t)"
line=$(awk -v b="$board" '$1 == b { print }' "$root/boards/arty_a7/boards.txt")
[ -n "$line" ] || die "unknown BOARD '$board' (boards/arty_a7/boards.txt)"
part=$(echo "$line" | awk '{ print $2 }')
ofl_board=$(echo "$line" | awk '{ print $3 }')
want_id=$(echo "$line" | awk '{ print $4 }')
bit_parts=$(echo "$line" | awk '{ print $5 }')
case "$serial" in                                       # it goes on openFPGALoader's command line
    '') sel= ;;
    -*|*[!A-Za-z0-9._-]*) die "cable serial number '$serial': only letters, digits, '.', '_' and '-' (not first)" ;;
    *)  sel="--ftdi-serial $serial" ;;
esac

ofl=${OPENFPGALOADER:-openFPGALoader}
if ! command -v "$ofl" >/dev/null 2>&1; then
    echo "$me: openFPGALoader is not installed: programming is pending (brew install openfpgaloader)" >&2
    exit 2
fi

# ------------------------------------------------------------------------------ the cable
# openFPGALoader takes the first FT2232H it finds; with two boards (or another FT2232H device)
# on the USB bus that may be the wrong one, so more than one needs a choice by serial number.
if [ -z "$serial" ]; then
    if scan_out=$("$ofl" --scan-usb 2>&1); then :; else
        echo "$scan_out" | sed "s/^/$me: | /" >&2
        die "openFPGALoader --scan-usb failed, so the connected cables cannot be counted; name the Arty's with CABLE_SERIAL=<serial> (or --ftdi-serial <serial>)"
    fi
    cables=$(echo "$scan_out" | grep -i '0x0403:0x6010' || true)
    ncables=$(echo "$cables" | grep -c . || true)
    if [ "$ncables" -gt 1 ]; then
        echo "$cables" | sed "s/^/$me: | /" >&2
        die "$ncables FT2232H cables (USB 0403:6010) are connected; refusing to guess which one is the Arty: choose it by serial number with CABLE_SERIAL=<serial> (make) or --ftdi-serial <serial>"
    fi
fi

# ------------------------------------------------------------------------------ the JTAG chain
# openFPGALoader prints each device as "idcode 0x%x" (no leading zeros; for these FPGAs the
# IDCODE without its revision nibble, 0x362d093 or 0x3631093).
# shellcheck disable=SC2086
detect_out=$("$ofl" $sel -c digilent --detect 2>&1) || {
    echo "$detect_out" | sed "s/^/$me: | /" >&2
    die "openFPGALoader found no JTAG cable or device (is the Arty plugged in and powered?${serial:+ Is $serial the serial number of its cable?})"
}
ids=$(echo "$detect_out" | awk '{
    for (i = 1; i < NF; i++) if ($i == "idcode") {
        v = $(i + 1)
        if (v ~ /^0[xX][0-9a-fA-F]+$/ && length(v) <= 10) print toupper(substr(v, 3)); else print "?" v
    } }')
count=$(echo "$ids" | grep -c . || true)
[ "$count" -ge 1 ] || die "no device on the JTAG chain"
case "$ids" in *'?'*) die "cannot read the IDCODE in openFPGALoader's answer: $(echo "$ids" | tr '\n' ' ')" ;; esac
[ "$count" -eq 1 ] || die "$count devices on the JTAG chain; refusing to guess which is the Arty"
got=$(printf '%d' "0x$ids")
idcode=$(printf '0x%08X' "$got")
got_masked=$(( got & 0x0FFFFFFF ))
want=$(( want_id ))
if [ "$got_masked" -ne "$want" ]; then
    die "the FPGA's IDCODE is $idcode, but BOARD=$board means $(printf '0x%08X' "$want") (bits 27:0); refusing"
fi
echo "$me: JTAG: IDCODE $idcode = BOARD $board ($part)${serial:+, cable $serial}"
[ "$mode" = detect ] && exit 0

# ------------------------------------------------------------------------------ the bitstream
design=${DESIGN:-surfcpu_arty_a7_top}
bit=${3:-${BIT:-${SURFCPU_FPGA_OUT:-$root/build/fpga}/$board/$design/$design.bit}}
[ -f "$bit" ] || die "no bitstream $bit (make build-fpga BOARD=$board DESIGN=$design)"

bit_header_part() {   # FILE: the part field ('b') of a .bit header. The header must be the magic, then the
                      # fields a, b, c, d once each and in this order (NUL-terminated strings), then e: the
                      # layout tools/bitfile.py reads too, so that both always name the same part (bitfile.py
                      # also checks e against the file's length; here the bytes are the build manifest's
                      # SHA-256's business, and openFPGALoader's)
    od -An -v -tu1 -N 1024 "$1" | LC_ALL=C awk '
        { for (i = 1; i <= NF; i++) b[n++] = $i + 0 }
        END {
            split("0 9 15 240 15 240 15 240 15 240 0 0 1", m, " ")
            for (i = 1; i <= 13; i++) if (b[i - 1] != m[i] + 0) exit 1
            split("97 98 99 100", want, " ")              # a, b, c, d
            p = 13
            for (k = 1; k <= 4; k++) {
                if (p + 3 > n || b[p] != want[k] + 0) exit 1
                len = b[p + 1] * 256 + b[p + 2]; p += 3
                if (len < 1 || p + len > n || b[p + len - 1] != 0) exit 1
                v = ""
                for (i = 0; i < len - 1; i++) {
                    if (b[p + i] == 0) exit 1                 # one string, no NUL inside
                    v = v sprintf("%c", b[p + i])
                }
                if (k == 2) part = v
                p += len
            }
            if (p >= n || b[p] != 101) exit 1             # then e, the stream length
            print part
            exit 0
        }'
}
hdr_part=$(bit_header_part "$bit") ||
    die "$bit has no .bit header naming its part (fields a, b, c, d once each, then e): a raw .bin, or a damaged file?; refusing"
norm=$(printf '%s' "$hdr_part" | tr 'A-Z' 'a-z' | sed -e 's/^xc//' -e 's/-.*$//')
case "$norm" in
    ''|*[!a-z0-9]*) die "$bit's header names the part '$hdr_part', which is not a part name; refusing" ;;
esac
case ",$bit_parts," in
    *",$norm,"*) ;;
    *) die "$bit was built for $hdr_part ($norm), but BOARD=$board is a $(echo "$bit_parts" | sed 's/,/ or a /g'); refusing" ;;
esac

# ------------------------------------------------------------------------------ the manifest
manifest="$(dirname "$bit")/manifest.txt"
sha=$(shasum -a 256 "$bit" | awk '{ print $1 }')
kind=none
if [ -f "$manifest" ]; then
    case "$(head -n 1 "$manifest")" in
        "# SurfCPU bitstream build manifest"*) kind=surfcpu ;;    # tools/fpga-build.sh
        "# openXC7 build manifest"*)          kind=openxc7 ;;    # tools/openxc7-build.sh on its own
        *) die "$manifest is not a build manifest this script knows; refusing" ;;
    esac
fi
top_key() {           # KEY: a top-level string or number of the TOML manifest (before its first table)
    awk -v k="$1" '/^\[/ { exit } $1 == k && $2 == "=" { v = $0; sub(/^[^=]*=[ \t]*/, "", v); gsub(/^"|"$/, "", v); print v; exit }' "$manifest"
}
case "$kind" in
    surfcpu)
        [ "$(top_key result)" = pass ] || die "$manifest does not say the build passed its checks; refusing"
        msha=$(top_key bitstream_sha256)
        [ "$msha" = "$sha" ] || die "$manifest describes another bitstream (bitstream_sha256 ${msha:-missing}, the file's is $sha); refusing"
        [ "$(top_key board)" = "$board" ] || die "$manifest is for board '$(top_key board)', not $board; refusing"
        [ "$(top_key part)" = "$part" ] || die "$manifest is for part '$(top_key part)', but $board carries $part; refusing" ;;
    openxc7)
        # Every check line must say PASS (block RAM and clocks may be absent from the design).
        bad=$(awk '/^check / { r = substr($0, 21); sub(/ \([^()]*\)$/, "", r)
                               if (r != "PASS" && r != "none in the design") print substr($0, 1, 20) r }' "$manifest")
        [ -n "$(grep '^check ' "$manifest")" ] || die "$manifest lists no checks; refusing"
        [ -z "$bad" ] || die "$manifest does not say the build passed its checks ($(echo "$bad" | sed 's/  */ /g' | tr '\n' ';')); refusing"
        # and every clock met nextpnr's timing (a build that let nextpnr finish anyway, as a
        # diagnostic run can, has its bitstream and checks but a FAIL clock line)
        late=$(grep '^clock ' "$manifest" | grep -v ' PASS$' || true)
        [ -z "$late" ] || die "$manifest shows a clock missing its timing ($(echo "$late" | sed -e 's/: constraint .*, achieved/:/' -e 's/, slack.*$//' | tr '\n' ';')); refusing"
        msha=$(sed -n 's/^bitstream .* sha256:\([0-9a-f]*\) .*/\1/p' "$manifest" | head -n 1)
        [ "$msha" = "$sha" ] || die "$manifest describes another bitstream (sha256 ${msha:-missing}, the file's is $sha); refusing" ;;
esac

# ------------------------------------------------------------------------------ load
if [ "$mode" = volatile ]; then
    [ "$kind" != none ] || echo "$me: note: no build manifest next to $bit (built elsewhere?)"
    echo "$me: loading $bit into configuration RAM (lost at power-off)"
    # shellcheck disable=SC2086
    "$ofl" $sel -b "$ofl_board" "$bit" || die "openFPGALoader failed"
    echo "$me: done; the FPGA runs the new design (press PROG or power-cycle to return to the flash)"
    exit 0
fi

# flash: milestone 9 only, on the owner's explicit word
[ "$kind" = surfcpu ] ||
    die "flash needs the bitstream's build manifest from tools/fpga-build.sh (result = pass, this file, this board); refusing"
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
# shellcheck disable=SC2086
"$ofl" $sel -b "$ofl_board" -f --verify "$bit" || die "openFPGALoader failed while writing the flash"
echo "$me: flash written and verified; set the MODE jumper (JP1) to boot from it"
