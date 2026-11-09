#!/bin/bash
# The Code Editor's host tests (user/wcode/, 11/05/26) under AddressSanitizer and UndefinedBehaviorSanitizer, natively on
# the Mac with its clang. Never part of `make unittest`, and never in the Docker image: a 32-bit -fsanitize binary hangs
# there. Runs test_wcode as it is, again with the text index's marks every 3 lines (every lookup and colouring then
# crosses marks), test_wcode_save on the simulated file system, and test_wcode's speed mode built with -O2 and no
# sanitizers. Usage: tests/host/wcode/mac-sanitize.sh [directory for the binaries] [rounds]
#
# libsurf's string.c is not 64-bit safe and its printf.c is x86 assembly in part, so the files built against SurfOS's
# headers call the host's C library here (tests/host/wcode/host64.c, sim64.c). regex.c adds 0 to a NULL in one place,
# harmlessly, and is built without -fsanitize=pointer-overflow (the awk track's note on regex.c says the same).
# test_wcode_win links the toolkit's host objects, whose blibc has x86 assembly: it is built for x86_64 and runs under
# Rosetta, as track kit runs test_gk and test_wkit.
set -eu
cd "$(dirname "$0")/../../.."
OUT="${1:-$(mktemp -d "${TMPDIR:-/tmp}/wcode-san.XXXXXX")}"
ROUNDS="${2:-200}"
mkdir -p "$OUT/sim"
SAN="-g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
WARN="-Wall -Wextra -Wno-unused-parameter -Wno-sign-compare"
MODEL="user/wcode/text.c user/wcode/undo.c user/wcode/lex.c user/wcode/syntax.c user/wcode/doc.c user/wcode/find.c \
       user/wcode/grep.c user/wcode/view.c user/wcode/app.c"
LIBSURF="-ffreestanding -fno-builtin -nostdinc -Iuser/include -include tests/host/libsurf_names.h"
HOOKS="-DWC_UNDO_REALLOC=wc_test_realloc -DWC_APP_MALLOC=wc_test_malloc"     # 11/09/26: allocations that fail on request
export ASAN_OPTIONS=abort_on_error=1

clang $SAN -fno-sanitize=pointer-overflow -w $LIBSURF -include tests/host/text_names.h -c user/libsurf/regex.c -o "$OUT/regex.o"
clang $SAN $WARN $HOOKS -Itests/host/wcode -Iuser/wcode tests/host/test_wcode.c $MODEL tests/host/wcode/host64.c "$OUT/regex.o" \
    -o "$OUT/test_wcode"
clang $SAN $WARN $HOOKS -DWC_MARK_EVERY=3 -DWC_MARK_BYTES=64 -Itests/host/wcode -Iuser/wcode tests/host/test_wcode.c $MODEL \
    tests/host/wcode/host64.c "$OUT/regex.o" -o "$OUT/test_wcode_marks3"
perl -e 'alarm 1800; exec @ARGV' "$OUT/test_wcode" "$ROUNDS"
perl -e 'alarm 1800; exec @ARGV' "$OUT/test_wcode_marks3" "$ROUNDS"

SIM="$LIBSURF -include tests/host/sim_names.h -Iuser/wcode -Iuser/bin -Itests/host"
for f in tests/host/sim_fs.c tests/host/wcode_save_bridge.c user/wcode/text.c user/wcode/undo.c user/wcode/lex.c \
         user/wcode/syntax.c user/wcode/doc.c user/wcode/fileio.c; do
    clang $SAN -w $SIM -c "$f" -o "$OUT/sim/$(basename "$f" .c).o"
done
clang $SAN -Itests/host tests/host/test_wcode_save.c "$OUT"/sim/*.o tests/host/wcode/sim64.c -o "$OUT/test_wcode_save"
perl -e 'alarm 600; exec @ARGV' "$OUT/test_wcode_save"

# the window on the fake desktop: the toolkit, libgui, the desktop's drawing kit and blibc as the Makefile's GK_HOST_OBJ
mkdir -p "$OUT/gk"
X86="-arch x86_64"
KERN="-ffreestanding -fno-builtin -nostdinc -Iinclude -include tests/host/surf_names.h"
for f in user/libsurf/gui.c user/libsurf/gk*.c tests/host/gk_fake.c; do
    clang $SAN $X86 -w -Itests/host/gk_shim -idirafter user/include -c "$f" -o "$OUT/gk/gk_$(basename "$f" .c).o"
done
for f in lib/blibc/vsnprintf.c lib/blibc/string.c lib/blibc/memory.c lib/blibc/stdlib.c lib/blibc/ctype.c \
         user/libsurf/guikit.c shell/guidraw.c driver/fbcore.c driver/font8x16.c driver/cp437.c; do
    clang $SAN $X86 -w $KERN -c "$f" -o "$OUT/gk/k_$(basename "$f" .c).o"
done
clang $SAN $X86 -fno-sanitize=pointer-overflow -w $LIBSURF -include tests/host/text_names.h -c user/libsurf/regex.c \
    -o "$OUT/gk/x_regex.o"
clang $SAN $X86 -w $LIBSURF -c user/libsurf/ctype.c -o "$OUT/gk/x_ctype.o"
clang $SAN $X86 $WARN -Itests/host/wcode -Itests/host/gk_shim -idirafter user/include -Iuser/wcode tests/host/test_wcode_win.c \
    $MODEL user/wcode/window.c "$OUT"/gk/*.o -o "$OUT/test_wcode_win"
perl -e 'alarm 600; exec @ARGV' arch -x86_64 "$OUT/test_wcode_win"

clang -O2 -g -w $LIBSURF -include tests/host/text_names.h -c user/libsurf/regex.c -o "$OUT/regex_fast.o"
clang -O2 -g $WARN $HOOKS -Itests/host/wcode -Iuser/wcode tests/host/test_wcode.c $MODEL tests/host/wcode/host64.c \
    "$OUT/regex_fast.o" -o "$OUT/test_wcode_fast"
perl -e 'alarm 600; exec @ARGV' "$OUT/test_wcode_fast" speed
echo "mac-sanitize: all passed, binaries in $OUT"
