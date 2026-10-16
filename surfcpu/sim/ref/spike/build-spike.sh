#!/bin/sh
# SurfCPU reference Spike: the container's pinned Spike commit plus surfcpu-contract.patch
# --------------------
# File: build-spike.sh Date: 10/15/26 (SurfCPU)
# --------------------
# Downloads riscv-isa-sim at the commit devenv/Dockerfile pins (SHA-256 checked), applies
# surfcpu-contract.patch and installs it into build/third_party/spike-surfcpu. Run inside the
# toolchain container (make ref-spike does). Does nothing when the installed copy was built
# from the same commit, patch and script. JOBS (default 4) sets the build's parallelism.
set -eu

COMMIT=5a760667bf0d0ab893173b9eeff9c109c8d7dd9a
SHA256=7e59ed05df857727a9f3c07ab99502351a649f9f41e37ecaeeb20a50a3988446
URL=https://codeload.github.com/riscv-software-src/riscv-isa-sim/tar.gz/$COMMIT

here=$(cd "$(dirname "$0")" && pwd -P)
root=$(cd "$here/../../.." && pwd -P)           # surfcpu/
third=$root/build/third_party
prefix=$third/spike-surfcpu
patch=$here/surfcpu-contract.patch
want="$COMMIT $(sha256sum "$patch" | cut -d' ' -f1) $(sha256sum "$0" | cut -d' ' -f1)"

if [ -x "$prefix/bin/spike" ] && [ "$(cat "$prefix/.surfcpu-stamp" 2>/dev/null)" = "$want" ]; then
  echo "build-spike: $prefix is up to date"
  exit 0
fi

# The cpu track's layout (sim/cpu/riscv_tests.py): build/third_party/<name>/<archive>.
mkdir -p "$third/riscv-isa-sim"
tarball=$third/riscv-isa-sim/riscv-isa-sim-$COMMIT.tar.gz
if [ ! -f "$tarball" ] || [ "$(sha256sum "$tarball" | cut -d' ' -f1)" != "$SHA256" ]; then
  echo "build-spike: downloading $URL"
  curl -fsSL -o "$tarball.part" "$URL"
  got=$(sha256sum "$tarball.part" | cut -d' ' -f1)
  if [ "$got" != "$SHA256" ]; then
    rm -f "$tarball.part"
    echo "build-spike: SHA-256 mismatch: got $got, pinned $SHA256" >&2
    exit 1
  fi
  mv "$tarball.part" "$tarball"
fi

src=$third/riscv-isa-sim/src
rm -rf "$src" "$prefix"
mkdir -p "$src"
tar xzf "$tarball" -C "$src" --strip-components=1
(cd "$src" && GIT_CEILING_DIRECTORIES="$third" git apply --whitespace=nowarn "$patch")
mkdir -p "$src/build"
cd "$src/build"
echo "build-spike: configuring and building (a few minutes)"
# No debug information: the stock build's binaries are about 300 MB each.
../configure --prefix="$prefix" CFLAGS=-O2 CXXFLAGS=-O2 > configure.out 2>&1 \
  || { tail -20 configure.out; exit 1; }
make -j"${JOBS:-4}" spike > make.out 2>&1 || { tail -40 make.out; exit 1; }
mkdir -p "$prefix/bin"
cp spike "$prefix/bin/spike"
strip "$prefix/bin/spike"
echo "$want" > "$prefix/.surfcpu-stamp"
cd "$third"
rm -rf "$src"
echo "build-spike: installed $prefix/bin/spike ($(du -h "$prefix/bin/spike" | cut -f1))"
