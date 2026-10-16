# SurfCPU toolchain image (surfcpu-toolchain): everything SurfCPU builds and simulates
# with on the MacBook, pinned. It runs as Linux arm64 on Apple Silicon (amd64 elsewhere).
# Vivado is not in here: AMD supports it only on x86-64 Linux/Windows (docs/BUILD-HOST.md).
#
#   Debian trixie, snapshot.debian.org 2026-10-05:
#     Verilator 5.032, Icarus Verilog 12.0, Yosys 0.52
#     riscv64-unknown-elf GCC 14.2 + binutils 2.44 + picolibc 1.8.10 (with rv32 multilibs)
#     mtools, dosfstools (mkfs.fat, fsck.fat), python3 3.13, gdb-multiarch,
#     qemu-system-misc (qemu-system-riscv32), z3 + boolector (for formal checks)
#   Spike (riscv-isa-sim) at a pinned commit: the ISA reference model
#   SymbiYosys (sby) at the tag that matches Yosys 0.52
#
# Build:  surfcpu/devenv/build-image.sh      (tags surfcpu-toolchain:<IMAGE_TAG in run.sh>)
# Use:    surfcpu/devenv/run.sh <command>    (the repository is mounted at /work)

ARG DEBIAN_DIGEST=sha256:a29215f6a35e51e22adffa17f89e9d2ef06214e64a2bad10d765c46aea49f11f
FROM debian:trixie-slim@${DEBIAN_DIGEST}

ARG SNAPSHOT=20261005T000000Z
ARG SPIKE_COMMIT=5a760667bf0d0ab893173b9eeff9c109c8d7dd9a
ARG SBY_TAG=v0.52

# Every package comes from one dated snapshot, so a rebuild gets the same versions.
RUN rm -f /etc/apt/sources.list.d/debian.sources \
 && printf 'Types: deb\nURIs: http://snapshot.debian.org/archive/debian/%s/\nSuites: trixie trixie-updates\nComponents: main\nCheck-Valid-Until: no\n\nTypes: deb\nURIs: http://snapshot.debian.org/archive/debian-security/%s/\nSuites: trixie-security\nComponents: main\nCheck-Valid-Until: no\n' \
      "$SNAPSHOT" "$SNAPSHOT" > /etc/apt/sources.list.d/snapshot.sources \
 && apt-get -o Acquire::Retries=5 update \
 && DEBIAN_FRONTEND=noninteractive apt-get -o Acquire::Retries=5 install -y --no-install-recommends \
      verilator iverilog yosys \
      gcc-riscv64-unknown-elf binutils-riscv64-unknown-elf picolibc-riscv64-unknown-elf \
      gdb-multiarch qemu-system-misc \
      build-essential g++ make ccache python3 git ca-certificates curl \
      mtools dosfstools xxd file bsdextrautils procps \
      device-tree-compiler libboost-regex-dev libboost-system-dev \
      z3 boolector \
 && rm -rf /var/lib/apt/lists/*

# Spike, built from a pinned commit (it has had no release since 1.1.0 in 2021).
RUN git clone --filter=blob:none https://github.com/riscv-software-src/riscv-isa-sim /tmp/spike \
 && cd /tmp/spike && git checkout -q "$SPIKE_COMMIT" \
 && mkdir build && cd build \
 && ../configure --prefix=/opt/spike >/dev/null \
 && make -j"$(nproc)" >/dev/null \
 && make install >/dev/null \
 && echo "$SPIKE_COMMIT" > /opt/spike/COMMIT \
 && rm -rf /tmp/spike

# SymbiYosys is a Python front end; install the tag that matches the packaged Yosys.
RUN git clone --depth 1 --branch "$SBY_TAG" https://github.com/YosysHQ/sby /tmp/sby \
 && make -C /tmp/sby install PREFIX=/usr/local >/dev/null \
 && rm -rf /tmp/sby

ENV PATH=/opt/spike/bin:$PATH \
    CCACHE_DIR=/work/surfcpu/build/.ccache \
    SURFCPU_IN_CONTAINER=1

# The worktree is mounted from the Mac with the Mac's owner; let git read it anyway.
RUN git config --system --add safe.directory '*'

# A record of exactly what is inside, printed by `surfcpu/devenv/run.sh versions`.
COPY versions.sh /usr/local/bin/surfcpu-versions
RUN chmod +x /usr/local/bin/surfcpu-versions && surfcpu-versions > /etc/surfcpu-toolchain.txt

WORKDIR /work
