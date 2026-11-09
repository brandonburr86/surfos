#!/bin/sh
# The games' host tests under AddressSanitizer and UBSan (11/05/26, the desktop applications, track games): Mines,
# Solitaire and Swell, their rules and their windows' models, and their windows on the fake desktop
# (tests/host/gk_fake.c) with libgui and the toolkit as they are. They run here, natively, on the Mac with clang: never
# in the Docker image (a 32-bit -fsanitize binary hangs there) and never from `make unittest`, which builds the same
# tests plainly. The models build for the Mac's own processor; the windows for x86_64 (under Rosetta on an arm64 Mac),
# because the toolkit's host build takes in blibc's formatter and the kernel's 64-bit division, which are x86 code.
#
#   tests/games-sanitize.sh [test_wmines test_wsolitaire test_wswell test_wmines_window ...]
#
# Everything goes in a temporary folder, which is removed. Run it from the top of the worktree.
set -eu
if [ "$(uname -s)" != Darwin ]; then
    echo "games-sanitize.sh: for the Mac only: sanitizer binaries are not run in the Docker image" >&2
    exit 2
fi
CC="${CC:-cc}"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/games-san.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT
SAN="-std=gnu99 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
X86="-arch x86_64"
KERN="-ffreestanding -fno-builtin -nostdinc -Iinclude -include tests/host/surf_names.h"
GK="-Itests/host/gk_shim -idirafter user/include"

# the rules and the windows' models
$CC $SAN -Iuser/wmines tests/host/test_wmines.c user/wmines/mines.c user/wmines/view.c -o "$OUT/test_wmines"
$CC $SAN -Iuser/wsolitaire tests/host/test_wsolitaire.c user/wsolitaire/klondike.c user/wsolitaire/table.c \
    -o "$OUT/test_wsolitaire"
$CC $SAN -Iuser/wswell tests/host/test_wswell.c user/wswell/swell.c user/wswell/view.c -o "$OUT/test_wswell"

# the toolkit on the fake desktop, as the Makefile's GK_HOST_OBJ has it
GKOBJ=""
for f in lib/blibc/vsnprintf.c lib/blibc/string.c lib/blibc/memory.c lib/blibc/stdlib.c lib/blibc/ctype.c \
         user/libsurf/guikit.c shell/guidraw.c driver/fbcore.c driver/font8x16.c driver/cp437.c; do
    o="$OUT/k_$(basename "$f" .c).o"
    $CC $X86 $SAN -Wno-unused-parameter -Wno-sign-compare $KERN -c "$f" -o "$o"
    GKOBJ="$GKOBJ $o"
done
for f in user/libsurf/gui.c user/libsurf/gk*.c tests/host/gk_fake.c; do
    o="$OUT/g_$(basename "$f" .c).o"
    $CC $X86 $SAN -Wno-unused-parameter -Wno-sign-compare $GK -c "$f" -o "$o"
    GKOBJ="$GKOBJ $o"
done
for g in wmines wsolitaire wswell; do
    case $g in
    wmines) src="user/wmines/window.c user/wmines/view.c user/wmines/mines.c" ;;
    wsolitaire) src="user/wsolitaire/window.c user/wsolitaire/table.c user/wsolitaire/klondike.c" ;;
    wswell) src="user/wswell/window.c user/wswell/view.c user/wswell/swell.c" ;;
    esac
    # shellcheck disable=SC2086
    $CC $X86 $SAN $GK -Iuser/$g -Itests/host tests/host/test_${g}_window.c $src $GKOBJ -o "$OUT/test_${g}_window"
done

status=0
for t in ${*:-test_wmines test_wsolitaire test_wswell test_wmines_window test_wsolitaire_window test_wswell_window}; do
    perl -e 'alarm 300; exec @ARGV' "$OUT/$t" || status=1
done
exit $status
