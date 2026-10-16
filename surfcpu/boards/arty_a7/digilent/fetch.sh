#!/bin/sh
# Digilent's reference files for the Arty A7 board track: where each one comes from, and checks.
#
#   boards/arty_a7/digilent/fetch.sh verify              check against SHA256SUMS, offline (make check)
#   boards/arty_a7/digilent/fetch.sh fetch               download into a temporary directory, check
#                                                        each file, install the matching manuals
#   boards/arty_a7/digilent/fetch.sh fetch --from DIR    the same with copies saved in DIR (by a browser)
#
# In git: the master XDC files (Digilent's, under the MIT licence in xdc/License.txt, from the
# same commit), this script and SHA256SUMS. Not in git: the reference manuals and schematics that
# docs/WIRING.md cites, which are (c) Digilent with no licence stated. manuals/ is a local cache
# (.gitignore) that `fetch` fills, installing only copies whose SHA-256 matches SHA256SUMS.
# `verify` always checks the XDC files and the licence; it checks each manual that is present
# and lists the ones not fetched without failing, so make check passes offline on a fresh
# checkout. A manual that is present but differs fails it.
#
# digilent.com answers scripted downloads with HTTP 403, so its manuals and schematics come from
# Digi-Key's mirror of Digilent's PDFs or from the Internet Archive's copies of the digilent.com
# pages (the snapshot dates are in the URLs; "2025" asks for the nearest one). A browser can
# save them from digilent.com too; `fetch --from` then checks and installs them.
set -eu

here=$(cd "$(dirname "$0")" && pwd -P)
sums="$here/SHA256SUMS"
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

die() { echo "digilent: $*" >&2; exit 1; }
sha() { shasum -a 256 "$1" | cut -d' ' -f1; }
want_of() { awk -v p="$1" '$2 == p { print $1 }' "$sums"; }

case "${1:-verify}" in
verify)
    bad=0
    tracked=0
    have=0
    missing=
    while read -r sum path; do
        [ -n "$path" ] || continue
        case "$path" in
            manuals/*)
                if [ ! -f "$here/$path" ]; then
                    missing="$missing $path"
                elif [ "$(sha "$here/$path")" = "$sum" ]; then
                    have=$((have + 1))
                else
                    echo "digilent: $path differs from SHA256SUMS (remove it and fetch again)" >&2
                    bad=1
                fi ;;
            *)
                tracked=$((tracked + 1))
                if [ ! -f "$here/$path" ] || [ "$(sha "$here/$path")" != "$sum" ]; then
                    echo "digilent: $path is missing or differs from SHA256SUMS" >&2
                    bad=1
                fi ;;
        esac
    done < "$sums"
    [ "$bad" = 0 ] || exit 1
    echo "digilent: $tracked files in git match SHA256SUMS; manuals: $have of $(grep -c ' manuals/' "$sums") present and matching"
    if [ -n "$missing" ]; then
        echo "digilent: not fetched (boards/arty_a7/digilent/fetch.sh fetch):$missing"
    fi
    ;;
fetch)
    from=
    if [ "${2:-}" = --from ]; then
        from=${3:-}
        [ -d "$from" ] || die "fetch --from needs a directory holding the saved files"
    fi
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    mkdir -p "$here/manuals"
    got=0
    bad=0
    while read -r path url; do
        [ -n "$path" ] || continue
        case "$path" in
            manuals/*) ;;
            *) [ -z "$from" ] || continue ;;             # in git: only compared when downloading
        esac
        mkdir -p "$tmp/$(dirname "$path")"
        if [ -n "$from" ]; then
            if [ ! -f "$from/$(basename "$path")" ]; then
                echo "absent  $path (no $(basename "$path") in $from)"
                continue
            fi
            cp "$from/$(basename "$path")" "$tmp/$path"
        else
            if ! curl -sSfL -A "Mozilla/5.0" -o "$tmp/$path" "$url"; then
                echo "FAILED  $path ($url)"
                bad=$((bad + 1))
                continue
            fi
            sleep 5                                     # the Internet Archive limits request rates
        fi
        if [ "$(sha "$tmp/$path")" != "$(want_of "$path")" ]; then
            echo "DIFFERS $path: not installed"
            bad=$((bad + 1))
            continue
        fi
        case "$path" in
            manuals/*)
                cp "$tmp/$path" "$here/$path"
                got=$((got + 1))
                echo "ok      $path" ;;
            *)
                echo "same    $path (in git)" ;;
        esac
    done <<EOF
$(sources)
EOF
    echo "digilent: $got manuals installed in boards/arty_a7/digilent/manuals; $bad failed or differed"
    [ "$bad" = 0 ]
    ;;
*)
    echo "usage: $0 [verify | fetch [--from DIR]]" >&2
    exit 2
    ;;
esac
