#!/bin/sh
# gzip, tar and unzip's host tests under AddressSanitizer and UBSan (11/01/26). These tools parse hostile files, so this is
# the check that matters for them, and it runs here, natively, on the Mac with clang: never in Docker and never from
# `make unittest`. A gcc -m32 -fsanitize binary hung at 99% CPU in the Docker image, under qemu-i386, though it compiled
# fine, so no compile probe could keep such a build out. The native build is 64-bit: libsurf and the tools are written to
# build either way, and the host tests' sizes say which.
#
#   tests/archive-sanitize.sh [test_deflate test_gztool test_tar test_unzip test_inflate_zlib]
#
# Everything goes in a temporary folder, which is removed. Run it from the top of the worktree.
set -eu
if [ "$(uname -s)" != Darwin ]; then
    echo "archive-sanitize.sh: for the Mac only: sanitizer binaries are not run in the Docker image" >&2
    exit 2
fi
CC="${CC:-cc}"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/ar-san.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT
SAN="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
USERFLAGS="-Wall -Wno-main -ffreestanding -fno-builtin -nostdinc -Iuser/include -include tests/host/libsurf_names.h -include tests/host/ar_names.h"
LIBSURF="stdlib ctype time"             # printf.c's x86 divl does not build here: ar_sim.c gives the host's formatter
LIB="crc32 inflate deflate"

compile() {     # source, object
    $CC $SAN $USERFLAGS $3 -c "$1" -o "$OUT/$2.o"
}

for f in $LIBSURF $LIB; do compile "user/libsurf/$f.c" "lib_$f" ""; done
compile tests/host/ar_string64.c lib_string "-Wno-pointer-to-int-cast -Wno-void-pointer-to-int-cast -Wno-int-to-pointer-cast"
compile user/gzip/tool.c   gz_tool    ""
compile user/gzip/gzfile.c gz_gzfile  ""
compile user/tar/tarfile.c tar_tarfile ""
compile user/tar/arpath.c  tar_arpath ""
compile user/tar/tar.c     tar_tar    -Dmain=tar_main
compile user/unzip/zipfile.c zip_zipfile ""
compile user/unzip/unzip.c zip_unzip  -Dmain=unzip_main
$CC $SAN -Wall -DAR_HOST_PRINTF -Itests/host -c tests/host/ar_sim.c -o "$OUT/sim.o"
$CC $SAN -Wall -c tests/host/ar_crc.c -o "$OUT/ar_crc.o"
$CC $SAN -ffreestanding -fno-builtin -nostdinc -Iinclude -include tests/host/surf_names.h -c driver/cp437.c -o "$OUT/cp437.o"

ALL="$OUT/sim.o $OUT/ar_crc.o $OUT/lib_string.o"
for f in $LIBSURF $LIB; do ALL="$ALL $OUT/lib_$f.o"; done
$CC $SAN -idirafter user/include tests/host/test_deflate.c user/libsurf/crc32.c user/libsurf/inflate.c user/libsurf/deflate.c \
    -o "$OUT/test_deflate"
$CC $SAN -Wall -Itests/host tests/host/test_gztool.c "$OUT/gz_tool.o" "$OUT/gz_gzfile.o" $ALL -o "$OUT/test_gztool"
$CC $SAN -Wall -Itests/host tests/host/test_tar.c "$OUT/tar_tar.o" "$OUT/tar_tarfile.o" "$OUT/tar_arpath.o" "$OUT/gz_gzfile.o" \
    $ALL -o "$OUT/test_tar"
$CC $SAN -Wall -Itests/host tests/host/test_unzip.c "$OUT/zip_unzip.o" "$OUT/zip_zipfile.o" "$OUT/tar_arpath.o" "$OUT/cp437.o" \
    $ALL -o "$OUT/test_unzip"

# zlib's own header and library, where the host has them: inflate must agree with it on every stream
if echo '#include <zlib.h>' | $CC -E -x c - > /dev/null 2>&1; then
    $CC $SAN -Wall -idirafter user/include tests/host/test_inflate_zlib.c user/libsurf/inflate.c user/libsurf/deflate.c -lz \
        -o "$OUT/test_inflate_zlib"
    ZLIB=test_inflate_zlib
else
    ZLIB=
    echo "archive-sanitize.sh: no zlib.h here: the comparison with zlib is skipped"
fi

for t in ${*:-test_deflate test_gztool test_tar test_unzip $ZLIB}; do
    echo "== $t, under ASan and UBSan"
    AR_SLOW=1 "$OUT/$t"
done
echo "archive-sanitize.sh: all passed"
