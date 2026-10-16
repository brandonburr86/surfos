#!/bin/sh
# Digilent's reference files for the Arty A7 board track: where each one came from, and checks.
#
#   boards/arty_a7/digilent/fetch.sh verify   check every file against SHA256SUMS (offline; make check)
#   boards/arty_a7/digilent/fetch.sh fetch    download them all again into a temporary directory
#                                             and compare with SHA256SUMS (replaces nothing here)
#
# Licences. The master XDC files are Digilent's, under the MIT licence (xdc/License.txt, from
# the same commit). The reference manuals and schematics are (c) Digilent with no licence
# stated; they are kept here unmodified as reference copies, for checking the wiring
# (docs/WIRING.md cites them page by page). Fetched 2026-10-15.
#
# digilent.com answers scripted downloads with HTTP 403, so its manuals and schematics come
# from Digi-Key's mirror of Digilent's PDFs or from the Internet Archive's copies of the
# digilent.com pages (the snapshot dates are in the URLs; "2025" asks for the nearest one).
# The checksums decide what is accepted.
set -eu

here=$(cd "$(dirname "$0")" && pwd -P)
XDC_COMMIT=c8644a9fe31cc803789643124545d56b3bef4600     # github.com/Digilent/digilent-xdc, 2026-10-13
GH=https://raw.githubusercontent.com/Digilent/digilent-xdc/$XDC_COMMIT
DK='https://media.digikey.com/pdf/Data%20Sheets/Digilent%20PDFs'
WB=https://web.archive.org/web
REF=https://digilent.com/reference

sources() {
    cat <<EOF
xdc/Arty-A7-35-Master.xdc              $GH/Arty-A7-35-Master.xdc
xdc/Arty-A7-100-Master.xdc             $GH/Arty-A7-100-Master.xdc
xdc/License.txt                        $GH/License.txt
manuals/Arty_A7_RM_Web.pdf             $DK/Arty_A7_RM_Web.pdf
manuals/arty-a7-reference-manual.html  $WB/20241107170454id_/$REF/programmable-logic/arty-a7/reference-manual
manuals/arty_a7_sch.pdf                $WB/20241129073427id_/$REF/_media/reference/programmable-logic/arty-a7/arty_a7_sch.pdf
manuals/Pmod_MircoSD_RM_Web.pdf        $DK/Pmod_MircoSD_RM_Web.pdf
manuals/pmodmicrosd_sch.pdf            $WB/2025id_/$REF/_media/reference/pmod/pmodmicrosd/pmodmicrosd_sch.pdf
manuals/pmodvga_rm.pdf                 $WB/2025id_/$REF/_media/reference/pmod/pmodvga/pmodvga_rm.pdf
manuals/pmodvga_sch.pdf                $WB/2025id_/$REF/_media/reference/pmod/pmodvga/pmodvga_sch.pdf
manuals/pmodps2_rm.pdf                 $WB/20220625200318id_/$REF/_media/reference/pmod/pmodps2/pmodps2_rm.pdf
manuals/pmodps2_sch.pdf                $WB/2025id_/$REF/_media/reference/pmod/pmodps2/pmodps2_sch.pdf
EOF
}

case "${1:-verify}" in
verify)
    cd "$here"
    shasum -a 256 -c SHA256SUMS >/dev/null || { shasum -a 256 -c SHA256SUMS; exit 1; }
    echo "digilent: $(wc -l < SHA256SUMS | tr -d ' ') reference files match SHA256SUMS"
    ;;
fetch)
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    bad=0
    sources | while read -r path url; do
        mkdir -p "$tmp/$(dirname "$path")"
        if ! curl -sSfL -A "Mozilla/5.0" -o "$tmp/$path" "$url"; then
            echo "FAILED  $path ($url)"; continue
        fi
        want=$(grep " $path\$" "$here/SHA256SUMS" | cut -d' ' -f1)
        got=$(shasum -a 256 "$tmp/$path" | cut -d' ' -f1)
        if [ "$want" = "$got" ]; then echo "same    $path"; else echo "DIFFERS $path"; fi
        sleep 5                                  # the Internet Archive limits request rates
    done
    ;;
*)
    echo "usage: $0 [verify|fetch]" >&2
    exit 2
    ;;
esac
