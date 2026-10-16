#!/bin/sh
# SurfCPU: Digilent's MIG project files for the Arty A7, from a pinned commit.
# File: fetch-digilent.sh Date: 10/15/26 (SurfCPU)
#
# Fetches github.com/Digilent/vivado-boards at COMMIT into build/third_party/vivado-boards
# (a shallow fetch of that one commit) and compares the files kept in ip/mig/digilent/ with
# it, byte for byte, and with the SHA-256 sums recorded in ip/mig/digilent/SOURCE.txt.
#   ip/mig/fetch-digilent.sh            verify (exit 1 on any difference)
#   ip/mig/fetch-digilent.sh --update   first copy the upstream files into ip/mig/digilent/
# Needs network access; run it from surfcpu/ (in the toolchain container or on the Mac).
set -eu

REPO=https://github.com/Digilent/vivado-boards
COMMIT=d8d43ed9f3f7e367ee3de82782ed601d88305677
here=$(cd "$(dirname "$0")" && pwd -P)
top=$(cd "$here/../.." && pwd -P)               # surfcpu/
dst=$top/build/third_party/vivado-boards
keep=$here/digilent
update=0
[ "${1:-}" = "--update" ] && update=1

sha() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
  else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

if [ ! -d "$dst/.git" ] || [ "$(git -C "$dst" rev-parse HEAD 2>/dev/null)" != "$COMMIT" ]; then
  rm -rf "$dst"
  mkdir -p "$dst"
  git -C "$dst" init -q
  git -C "$dst" remote add origin "$REPO"
  git -C "$dst" fetch -q --depth 1 origin "$COMMIT"
  git -C "$dst" checkout -q FETCH_HEAD
fi
[ "$(git -C "$dst" rev-parse HEAD)" = "$COMMIT" ] || { echo "fetch-digilent: wrong commit" >&2; exit 1; }

status=0
for pair in "License.txt:License.txt" \
            "arty-a7-100/mig.prj:new/board_files/arty-a7-100/E.0/1.1/mig.prj" \
            "arty-a7-35/mig.prj:new/board_files/arty-a7-35/E.0/1.1/mig.prj"; do
  mine=${pair%%:*}
  up=${pair#*:}
  if [ $update = 1 ]; then
    mkdir -p "$(dirname "$keep/$mine")"
    cp "$dst/$up" "$keep/$mine"
  fi
  want=$(grep " $mine\$" "$keep/SOURCE.txt" | cut -d' ' -f1 || true)
  got_up=$(sha "$dst/$up")
  got_mine=$(sha "$keep/$mine")
  if [ "$got_up" != "$got_mine" ] || [ "$got_up" != "$want" ]; then
    echo "fetch-digilent: $mine differs (upstream $got_up, kept $got_mine, SOURCE.txt ${want:-none})" >&2
    status=1
  else
    echo "fetch-digilent: $mine = $up at $COMMIT ($got_up)"
  fi
done
exit $status
