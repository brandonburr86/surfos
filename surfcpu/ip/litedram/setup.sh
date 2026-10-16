#!/bin/sh
# SurfCPU: a pinned, vendored LiteDRAM generator environment
# File: setup.sh Date: 10/15/26 (SurfCPU)
#
# Builds build/third_party/litedram/ inside the toolchain container (surfcpu/devenv/run.sh
# ip/litedram/setup.sh, or make litedram-setup): Migen, LiteX and LiteDRAM, and the LiteX data
# packages a calibration CPU inside the core would need (VexRiscv's Verilog, picolibc and
# compiler-rt sources for its BIOS; the Arty core has had none since round ddr-3, but liblitedram's
# sources, which firmware/common/ddrinit.c ports, come with LiteX), each from one pinned git commit; PyYAML, packaging, colorama, meson and ninja from
# pinned PyPI wheels whose SHA-256 is checked, unpacked with Python's zipfile (the container has
# no pip). Nothing is installed anywhere else. Prints env.sh, which generate.sh sources.
# Re-running only fetches what is missing or at the wrong commit.
set -eu

here=$(cd "$(dirname "$0")" && pwd -P)
top=$(cd "$here/../.." && pwd -P)                    # surfcpu/
dst=$top/build/third_party/litedram

# name                              repository                                             commit (date, what)
GIT_PINS="
migen                               https://github.com/m-labs/migen                        e19524c963a8342952840983047557707fbe0b6a
litex                               https://github.com/enjoy-digital/litex                 b6ae9e0b227354aecffef5339d3e946f2395ac09
litedram                            https://github.com/enjoy-digital/litedram              c454a440ca94afd57bca148a0ba2af491325f0e6
pythondata-cpu-vexriscv             https://github.com/litex-hub/pythondata-cpu-vexriscv   642ecfed1c84460555d6d803d660cc60cfc1ecb6
pythondata-software-picolibc        https://github.com/litex-hub/pythondata-software-picolibc 6a13ccce7c575b32c102dd9dc52178505b81fe39
pythondata-software-compiler_rt     https://github.com/litex-hub/pythondata-software-compiler_rt 6eb76609c9627bf26635e57c63fb22cda7115887
pythondata-misc-tapcfg              https://github.com/litex-hub/pythondata-misc-tapcfg     a12c3f592c99f9c082fdc68c065b81cbd6e6b238
"
# migen 2026-01-05 (the last GitHub commit; the code is unchanged from e64d0bc), litex and
# litedram tag 2026.08 (2026-09-24), the pythondata repositories at their 2026-05/06 heads
# (tapcfg only because LiteX's simulation platform looks it up when writing --sim cores).

# wheel file                                                             SHA-256
WHEELS="
PyYAML-6.0.2-cp313-cp313-manylinux_2_17_aarch64.manylinux2014_aarch64.whl 0ffe8360bab4910ef1b9e87fb812d8bc0a308b0d0eef8c8f44e0254ab3b07133 https://files.pythonhosted.org/packages/7c/9a/337322f27005c33bcb656c655fa78325b730324c78620e8328ae28b64d0c/PyYAML-6.0.2-cp313-cp313-manylinux_2_17_aarch64.manylinux2014_aarch64.whl
packaging-25.0-py3-none-any.whl 29572ef2b1f17581046b3a2227d5c611fb25ec70ca1ba8554b24b0e69331a484 https://files.pythonhosted.org/packages/20/12/38679034af332785aac8774540895e234f4d07f7545804097de4b666afd8/packaging-25.0-py3-none-any.whl
colorama-0.4.6-py2.py3-none-any.whl 4f1d9991f5acc0ca119f9d443620b77f9d6b33703e51011c16baf57afb285fc6 https://files.pythonhosted.org/packages/d1/d6/3965ed04c63042e047cb6a3e6ed1a63a35087b6a609aa3a15ed8ac56c221/colorama-0.4.6-py2.py3-none-any.whl
meson-1.9.1-py3-none-any.whl f824ab770c041a202f532f69e114c971918ed2daff7ea56583d80642564598d0 https://files.pythonhosted.org/packages/9c/07/b48592d325cb86682829f05216e4efb2dc881762b8f1bafb48b57442307a/meson-1.9.1-py3-none-any.whl
ninja-1.13.0-py3-none-manylinux2014_aarch64.manylinux_2_17_aarch64.whl 3d00c692fb717fd511abeb44b8c5d00340c36938c12d6538ba989fe764e79630 https://files.pythonhosted.org/packages/8e/de/6e1cd6b84b412ac1ef327b76f0641aeb5dcc01e9d3f9eee0286d0c34fd93/ninja-1.13.0-py3-none-manylinux2014_aarch64.manylinux_2_17_aarch64.whl
"

[ "$(uname -m)" = aarch64 ] || echo "setup.sh: note: the PyYAML and ninja wheels pinned here are for aarch64" >&2
mkdir -p "$dst/src" "$dst/wheels" "$dst/site" "$dst/bin"

echo "$GIT_PINS" | while read -r name url commit; do
  [ -n "$name" ] || continue
  d=$dst/src/$name
  if [ "$(git -C "$d" rev-parse HEAD 2>/dev/null)" != "$commit" ]; then
    echo "setup.sh: fetching $name at $commit"
    rm -rf "$d"
    mkdir -p "$d"
    git -C "$d" init -q
    git -C "$d" remote add origin "$url"
    git -C "$d" fetch -q --depth 1 origin "$commit"
    git -C "$d" checkout -q FETCH_HEAD
    if [ -f "$d/.gitmodules" ]; then
      git -C "$d" submodule -q update --init --recursive --depth 1
    fi
  fi
  [ "$(git -C "$d" rev-parse HEAD)" = "$commit" ] || { echo "setup.sh: $name is not at $commit" >&2; exit 1; }
done

echo "$WHEELS" | while read -r file sha url; do
  [ -n "$file" ] || continue
  w=$dst/wheels/$file
  if [ ! -f "$w" ] || [ "$(sha256sum "$w" | cut -d' ' -f1)" != "$sha" ]; then
    echo "setup.sh: downloading $file"
    curl -fsSL -o "$w.part" "$url"
    mv "$w.part" "$w"
  fi
  got=$(sha256sum "$w" | cut -d' ' -f1)
  [ "$got" = "$sha" ] || { echo "setup.sh: $file has SHA-256 $got, expected $sha" >&2; rm -f "$w"; exit 1; }
  python3 -I -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' "$w" "$dst/site"
done

# ninja's binary and a meson command, for LiteX's BIOS build
chmod +x "$dst/site/ninja-1.13.0.data/scripts/ninja"
ln -sf "$dst/site/ninja-1.13.0.data/scripts/ninja" "$dst/bin/ninja"
cat > "$dst/bin/meson" <<EOF
#!/bin/sh
exec python3 -c 'import sys; from mesonbuild.mesonmain import main; sys.exit(main())' "\$@"
EOF
chmod +x "$dst/bin/meson"

paths=$dst/site
for name in migen litex litedram pythondata-cpu-vexriscv pythondata-software-picolibc pythondata-software-compiler_rt \
            pythondata-misc-tapcfg; do
  paths=$paths:$dst/src/$name
done
cat > "$dst/env.sh" <<EOF
# generated by ip/litedram/setup.sh: source it to use the pinned LiteDRAM generator
export PYTHONPATH=$paths
export PATH=$dst/bin:\$PATH
EOF
. "$dst/env.sh"
echo "setup.sh: $(python3 -c 'import migen, litex, litedram, yaml, packaging; print("migen, litex, litedram, yaml " + yaml.__version__ + ", packaging " + packaging.__version__ + " import")'); meson $(meson -v); ninja $(ninja --version)"
echo "setup.sh: environment in $dst/env.sh"
