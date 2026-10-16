# SurfCPU open-source bitstream flow image (surfcpu-openxc7): the openXC7 toolchain for the
# Artix-7 on the Arty A7, built from pinned sources, natively for arm64 on Apple Silicon.
# Experimental: see surfcpu/docs/OPEN-FLOW.md for what it proves and what it does not.
#
#   Debian trixie, snapshot.debian.org 2026-10-05 (the same base as surfcpu-toolchain)
#   Yosys 0.69                  release tarball, SHA-256 checked
#   openXC7/nextpnr 1.0.0       nextpnr-himbaechel with the xilinx micro-architecture, the
#                               engine that replaced nextpnr-xilinx (archived 2026-09-25),
#                               plus bbasm and the fasm2netlist LVS tools (tileverilog,
#                               lvs_equiv) from its 3rdparty submodule
#   openXC7/prjxray             xc7frames2bit, bitread, fasm2frames.py, bit2fasm.py
#   openXC7/prjxray-db          the Artix-7 database (only artix7/ is kept)
#   fasm                        the openxc7/fasm package pinned by prjxray (pure Python, textX)
#   textX 4.2.3                 wheel from PyPI, SHA-256 checked (Debian has no package)
#   chip databases              chipdb-xc7a50t.bin (serves the xc7a35t, the same die) and
#                               chipdb-xc7a100t.bin, generated here from the pinned sources
#
# The prjxray and prjxray-db pins are the ones openXC7/nextpnr 1.0.0's own CI and the openXC7
# toolchain-installer 1.0.0 use with that engine (PRJXRAY_REV, PRJXRAY_DB_REV), so the chip
# databases, fasm2frames and xc7frames2bit agree on one database. Git sources are pinned by
# commit (git checks every object's hash); tarballs and wheels by SHA-256.
#
# Build:  surfcpu/devenv/openxc7/build-image.sh   (tags surfcpu-openxc7:<IMAGE_TAG>)
# Use:    surfcpu/tools/openxc7-build.sh ...      (runs it with the repository at /work)

ARG DEBIAN_DIGEST=sha256:a29215f6a35e51e22adffa17f89e9d2ef06214e64a2bad10d765c46aea49f11f

# ------------------------------------------------------------------ base: pinned Debian
FROM debian:trixie-slim@${DEBIAN_DIGEST} AS base
ARG SNAPSHOT=20261005T000000Z
RUN rm -f /etc/apt/sources.list.d/debian.sources \
 && printf 'Types: deb\nURIs: http://snapshot.debian.org/archive/debian/%s/\nSuites: trixie trixie-updates\nComponents: main\nCheck-Valid-Until: no\n\nTypes: deb\nURIs: http://snapshot.debian.org/archive/debian-security/%s/\nSuites: trixie-security\nComponents: main\nCheck-Valid-Until: no\n' \
      "$SNAPSHOT" "$SNAPSHOT" > /etc/apt/sources.list.d/snapshot.sources

# ------------------------------------------------------------------ build: compilers and -dev packages
FROM base AS build
RUN apt-get -o Acquire::Retries=5 update \
 && DEBIAN_FRONTEND=noninteractive apt-get -o Acquire::Retries=5 install -y --no-install-recommends \
      build-essential cmake git ca-certificates curl pkg-config bison flex gawk \
      python3 python3-dev zlib1g-dev \
      libboost-program-options-dev libboost-iostreams-dev libboost-thread-dev \
      libboost-filesystem-dev libboost-system-dev libeigen3-dev libz3-dev time \
 && rm -rf /var/lib/apt/lists/*
ARG JOBS=6
ENV PREFIX=/opt/openxc7

# ------------------------------------------------------------------ Yosys
FROM build AS yosys
ARG YOSYS_VERSION=0.69
ARG YOSYS_SHA256=6dad6412cae417f5a53e2c943c2aee160162cfc1bdd31669230da1b7e3522571
RUN curl -fsSL --retry 5 -o /tmp/yosys.tar.gz \
      "https://github.com/YosysHQ/yosys/releases/download/v${YOSYS_VERSION}/yosys.tar.gz" \
 && echo "${YOSYS_SHA256}  /tmp/yosys.tar.gz" | sha256sum -c - \
 && mkdir /src && tar -xzf /tmp/yosys.tar.gz -C /src && rm /tmp/yosys.tar.gz \
 && cmake -S /src -B /src/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PREFIX \
      -DYOSYS_ENABLE_UNIT_TESTS=OFF -DYOSYS_WITHOUT_SLANG=ON -DYOSYS_WITHOUT_TCL=ON \
      -DYOSYS_WITHOUT_READLINE=ON -DYOSYS_WITHOUT_EDITLINE=ON -DYOSYS_WITHOUT_LIBFFI=ON \
      > /src/configure.txt \
 && cmake --build /src/build --parallel "$JOBS" \
 && cmake --install /src/build --strip \
 && mkdir -p $PREFIX/share/openxc7 \
 && echo "yosys ${YOSYS_VERSION} sha256:${YOSYS_SHA256}" > $PREFIX/share/openxc7/source-yosys.txt \
 && rm -rf /src

# ------------------------------------------------------------------ prjxray-db (artix7 only)
FROM build AS prjxray-db
ARG PRJXRAY_DB_COMMIT=a90f27c1caefee5276f47440f4c730b50519a86f
RUN git clone --quiet --filter=blob:none --no-checkout https://github.com/openXC7/prjxray-db /src/prjxray-db \
 && cd /src/prjxray-db \
 && git sparse-checkout set artix7 \
 && git checkout --quiet --detach "$PRJXRAY_DB_COMMIT" \
 && test "$(git rev-parse HEAD)" = "$PRJXRAY_DB_COMMIT" \
 && mkdir -p $PREFIX/share/prjxray-db $PREFIX/share/openxc7 \
 && cp -a artix7 LICENSE $PREFIX/share/prjxray-db/ \
 && echo "prjxray-db $PRJXRAY_DB_COMMIT artix7-tree:$(git rev-parse HEAD:artix7)" \
      > $PREFIX/share/openxc7/source-prjxray-db.txt \
 && rm -rf /src

# ------------------------------------------------------------------ nextpnr (himbaechel, xilinx) + LVS tools
FROM build AS nextpnr
ARG NEXTPNR_COMMIT=e860c9c8360d8501a1b55df94e58f3dfe7bde958
COPY --from=prjxray-db /opt/openxc7/share/prjxray-db /opt/openxc7/share/prjxray-db
RUN git clone --quiet --filter=blob:none https://github.com/openXC7/nextpnr /src/nextpnr \
 && cd /src/nextpnr \
 && git checkout --quiet --detach "$NEXTPNR_COMMIT" \
 && test "$(git rev-parse HEAD)" = "$NEXTPNR_COMMIT" \
 && git submodule --quiet update --init himbaechel/uarch/xilinx/meta 3rdparty/fasm2netlist \
 && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PREFIX \
      -DARCH=himbaechel -DHIMBAECHEL_UARCH=xilinx -DBUILD_GUI=OFF -DBUILD_PYTHON=OFF \
      -DBUILD_TESTS=OFF -DUSE_OPENMP=ON \
      -DHIMBAECHEL_PRJXRAY_DB=$PREFIX/share/prjxray-db -DHIMBAECHEL_XILINX_DEVICES= \
      > /src/nextpnr-configure.txt \
 && cmake --build build --target nextpnr-himbaechel bbasm --parallel "$JOBS" \
 && mkdir -p $PREFIX/bin $PREFIX/share/openxc7 $PREFIX/share/nextpnr/himbaechel/uarch/xilinx \
 && install -m755 -s build/nextpnr-himbaechel build/bba/bbasm $PREFIX/bin/ \
 && sed 's|^BIN="nextpnr-himbaechel"$|BIN="/opt/openxc7/bin/nextpnr-himbaechel"|' \
      .github/scripts/nextpnr-xilinx-shim.sh > $PREFIX/bin/nextpnr-xilinx \
 && chmod 755 $PREFIX/bin/nextpnr-xilinx \
 && cp -r himbaechel/uarch/xilinx/gen himbaechel/uarch/xilinx/meta himbaechel/uarch/xilinx/constids.inc \
      $PREFIX/share/nextpnr/himbaechel/uarch/xilinx/ \
 && cp -r himbaechel/himbaechel_dbgen $PREFIX/share/nextpnr/himbaechel/ \
 && rm -rf $PREFIX/share/nextpnr/himbaechel/uarch/xilinx/meta/.git \
 && cmake -S 3rdparty/fasm2netlist -B build-f2n -DCMAKE_BUILD_TYPE=Release > /src/f2n-configure.txt \
 && cmake --build build-f2n --target fasm2netlist tileverilog tiledump lvs_equiv --parallel "$JOBS" \
 && install -m755 -s build-f2n/fasm2netlist build-f2n/tileverilog build-f2n/tiledump build-f2n/lvs_equiv $PREFIX/bin/ \
 && install -m755 .github/scripts/lvs-equiv.sh $PREFIX/bin/lvs-equiv.sh \
 && echo "nextpnr $NEXTPNR_COMMIT $(git describe --tags --always) meta:$(git -C himbaechel/uarch/xilinx/meta rev-parse HEAD) fasm2netlist:$(git -C 3rdparty/fasm2netlist rev-parse HEAD)" \
      > $PREFIX/share/openxc7/source-nextpnr.txt \
 && rm -rf /src

# ------------------------------------------------------------------ chip databases (one per die)
# xc7a35t has no database of its own: it is the xc7a50t die (prjxray-db mapping/devices.yaml;
# nextpnr's device parser maps xc7a35t* onto chipdb-xc7a50t). Peak memory and time of each
# generation are kept next to the databases.
FROM nextpnr AS chipdb
ARG CHIPDB_DIES="xc7a50t xc7a100t"
RUN mkdir -p $PREFIX/share/chipdb \
 && cd /tmp \
 && for die in $CHIPDB_DIES; do \
      /usr/bin/time -v -o $PREFIX/share/chipdb/chipdb-$die.gen.txt \
        python3 $PREFIX/share/nextpnr/himbaechel/uarch/xilinx/gen/xilinx_gen.py \
          --xray $PREFIX/share/prjxray-db/artix7 --device $die --bba /tmp/chipdb-$die.bba \
      && /usr/bin/time -v -o $PREFIX/share/chipdb/chipdb-$die.bbasm.txt \
        $PREFIX/bin/bbasm -l /tmp/chipdb-$die.bba $PREFIX/share/chipdb/chipdb-$die.bin \
      && ls -l /tmp/chipdb-$die.bba >> $PREFIX/share/chipdb/chipdb-$die.gen.txt \
      && rm -f /tmp/chipdb-$die.bba || exit 1; \
    done \
 && cd $PREFIX/share/chipdb && sha256sum chipdb-*.bin > SHA256SUMS

# ------------------------------------------------------------------ prjxray tools, fasm, textX
FROM build AS prjxray
ARG PRJXRAY_COMMIT=ed3331c6200f421164101388759fc2860b0f5634
ARG TEXTX_VERSION=4.2.3
ARG TEXTX_SHA256=dc533d34809408fd7152f499fc97609ba7615f61a9e0ebcc76016a0929be5979
# The CMake-version and -include stdint.h fixes are the ones openXC7/nextpnr's CI and
# toolchain-nix apply to build this older project with current CMake and GCC.
RUN git clone --quiet --filter=blob:none https://github.com/openXC7/prjxray /src/prjxray \
 && cd /src/prjxray \
 && git checkout --quiet --detach "$PRJXRAY_COMMIT" \
 && test "$(git rev-parse HEAD)" = "$PRJXRAY_COMMIT" \
 && git submodule --quiet update --init third_party/abseil-cpp third_party/cctz third_party/gflags \
      third_party/googletest third_party/yaml-cpp third_party/sanitizers-cmake third_party/fasm \
 && sed -i 's/VERSION 3.5.0/VERSION 3.14.0/g' CMakeLists.txt \
 && sed -i 's/VERSION 3.0.2/VERSION 3.14.0/g' third_party/gflags/CMakeLists.txt \
 && sed -i 's/VERSION 2.8.12/VERSION 3.14.0/g' third_party/cctz/CMakeLists.txt \
 && CFLAGS="-include stdint.h -Wno-free-nonheap-object" CXXFLAGS="-include stdint.h -Wno-free-nonheap-object" \
      cmake -S . -B build -DCMAKE_BUILD_TYPE=Release > /src/prjxray-configure.txt \
 && cmake --build build --target xc7frames2bit bitread xc7patch frame_address_decoder bittool --parallel "$JOBS" \
 && mkdir -p $PREFIX/bin $PREFIX/lib/python $PREFIX/share/openxc7 \
 && install -m755 -s build/tools/xc7frames2bit build/tools/bitread build/tools/xc7patch \
      build/tools/frame_address_decoder build/tools/bittool $PREFIX/bin/ \
 && cp -r prjxray utils $PREFIX/lib/python/ \
 && cp -r third_party/fasm/fasm $PREFIX/lib/python/ \
 && rm -f $PREFIX/lib/python/fasm/parser/antlr.py $PREFIX/lib/python/fasm/parser/antlr_to_tuple.pyx \
 && printf '#!/bin/sh\nexec python3 /opt/openxc7/lib/python/utils/fasm2frames.py "$@"\n' > $PREFIX/bin/fasm2frames \
 && printf '#!/bin/sh\nexec python3 /opt/openxc7/lib/python/utils/bit2fasm.py "$@"\n' > $PREFIX/bin/bit2fasm \
 && chmod 755 $PREFIX/bin/fasm2frames $PREFIX/bin/bit2fasm \
 && echo "prjxray $PRJXRAY_COMMIT fasm:$(git -C third_party/fasm rev-parse HEAD)" > $PREFIX/share/openxc7/source-prjxray.txt \
 && curl -fsSL --retry 5 -o /tmp/textx-${TEXTX_VERSION}-py3-none-any.whl \
      "https://files.pythonhosted.org/packages/5e/f4/e748c6781318aa9fef477c66d4dbd378b28a0e5ea73320cdcf630f755fe5/textx-${TEXTX_VERSION}-py3-none-any.whl" \
 && echo "${TEXTX_SHA256}  /tmp/textx-${TEXTX_VERSION}-py3-none-any.whl" | sha256sum -c - \
 && python3 -m zipfile -e /tmp/textx-${TEXTX_VERSION}-py3-none-any.whl $PREFIX/lib/python/ \
 && echo "textx ${TEXTX_VERSION} sha256:${TEXTX_SHA256}" > $PREFIX/share/openxc7/source-textx.txt \
 && rm -rf /src /tmp/textx-*.whl

# ------------------------------------------------------------------ the image
FROM base
RUN apt-get -o Acquire::Retries=5 update \
 && DEBIAN_FRONTEND=noninteractive apt-get -o Acquire::Retries=5 install -y --no-install-recommends \
      python3 python3-yaml python3-simplejson python3-intervaltree python3-progressbar2 \
      python3-arpeggio libboost-program-options1.83.0 libboost-iostreams1.83.0 \
      libboost-thread1.83.0 libgomp1 libz3-4 zlib1g make time procps file xxd \
 && rm -rf /var/lib/apt/lists/*
COPY --from=yosys      /opt/openxc7 /opt/openxc7
COPY --from=prjxray-db /opt/openxc7 /opt/openxc7
COPY --from=nextpnr    /opt/openxc7/bin /opt/openxc7/bin
COPY --from=nextpnr    /opt/openxc7/share/nextpnr /opt/openxc7/share/nextpnr
COPY --from=nextpnr    /opt/openxc7/share/openxc7/source-nextpnr.txt /opt/openxc7/share/openxc7/
COPY --from=chipdb     /opt/openxc7/share/chipdb /opt/openxc7/share/chipdb
COPY --from=prjxray    /opt/openxc7 /opt/openxc7
ENV PATH=/opt/openxc7/bin:$PATH \
    PYTHONPATH=/opt/openxc7/lib/python \
    PRJXRAY_DB_DIR=/opt/openxc7/share/prjxray-db \
    OPENXC7_CHIPDB_DIR=/opt/openxc7/share/chipdb \
    SURFCPU_IN_CONTAINER=1 \
    SURFCPU_OPENXC7=1
COPY versions.sh /usr/local/bin/surfcpu-openxc7-versions
RUN chmod +x /usr/local/bin/surfcpu-openxc7-versions \
 && surfcpu-openxc7-versions > /etc/surfcpu-openxc7.txt
WORKDIR /work
