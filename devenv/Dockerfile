# ai-dev uses modern compiler flags and explicit trap labels, not the 2004 toolchain.
# Use amd64 with multilib: the host blibc tests also contain x86 inline assembly.
FROM debian:bookworm-slim
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential gcc-multilib binutils nasm \
      python3 mtools qemu-system-x86 rsync ca-certificates \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /surfos
