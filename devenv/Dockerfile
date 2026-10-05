# ai-dev uses modern compiler flags and explicit trap labels, not the 2004 toolchain.
# Use amd64 with multilib: the host blibc tests also contain x86 inline assembly.
# The USB image (make usb-image) needs GRUB's x86_64-efi files and grub-mkstandalone
# (grub-efi-amd64-bin, grub-common), mtools for the FAT32 system partition, and OVMF
# for booting it under QEMU (make test-usb). The -bin package only provides files: it
# installs nothing into a boot loader, here or anywhere else.
FROM debian:bookworm-slim
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential gcc-multilib binutils nasm \
      python3 mtools qemu-system-x86 rsync ca-certificates \
      grub-common grub-efi-amd64-bin ovmf \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /surfos
