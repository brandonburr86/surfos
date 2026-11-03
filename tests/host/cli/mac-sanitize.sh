#!/bin/bash
# The diff track's tests under AddressSanitizer and UndefinedBehaviorSanitizer (11/01/26), on the Mac with its clang. Not part
# of `make unittest`, and never in the Docker image: 32-bit ASan hangs there under qemu-i386, so sanitized programs are
# not run in it at all. Usage: tests/host/cli/mac-sanitize.sh [directory for the binaries] [rounds]
set -eu
cd "$(dirname "$0")/../../.."
OUT="${1:-$(mktemp -d "${TMPDIR:-/tmp}/cli-diff-san.XXXXXX")}"
ROUNDS="${2:-6000}"
mkdir -p "$OUT"
SAN="-g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
WARN="-Wall -Wextra -Wno-unused-parameter -Wno-sign-compare"
HOST="-include tests/host/cli/cli_host.h -Itests/host/cli"
DIFF_CORE="user/diff/dcore.c user/diff/dinput.c user/diff/dfile.c user/diff/dout.c"
PATCH_CORE="user/diff/dinput.c user/diff/phunk.c user/diff/papply.c"
for t in cmp comm join paste fold nl rev basename dirname od base64 strings file tree; do
    cc $SAN $WARN $HOST "user/bin/$t.c" -o "$OUT/$t"
done
cc $SAN $WARN $HOST user/bin/cal.c user/wclock/clock.c -o "$OUT/cal"
cc $SAN $WARN $HOST user/bin/diff.c $DIFF_CORE -o "$OUT/diff"
cc $SAN $WARN $HOST user/bin/patch.c $PATCH_CORE -o "$OUT/patch"
TEST_SRC="tests/host/test_diff.c user/diff/dcore.c user/diff/dinput.c user/diff/dout.c user/diff/phunk.c user/diff/papply.c"
cc $SAN $WARN $TEST_SRC user/diff/dfile.c -o "$OUT/test_diff"
cc $SAN $WARN -DD_HASH_MASK=3 $TEST_SRC user/diff/dfile.c -o "$OUT/test_diff_weak"
"$OUT/test_diff" "$ROUNDS"
"$OUT/test_diff_weak" "$ROUNDS"
python3 tests/test_cli_diff.py "$OUT"
echo "mac-sanitize: all passed, binaries in $OUT"
