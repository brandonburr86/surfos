#!/bin/bash
# Boot the modern kernel with its initrd and primary IDE test disk.
# Usage: devenv/run.sh [QEMU options]; O=2 devenv/run.sh for the optimized build.
set -euo pipefail
source "$(dirname "$0")/common.sh"
require_boot_files
# QEMU parses -initrd as a list of filenames separated by spaces. Use a relative
# filename so a project such as "SurfOS Restoration Project" boots correctly.
cd "$OUT"
exec qemu-system-i386 -name SurfOS -m 64 -kernel "$KERNEL" \
    -drive "file=$DISK,format=raw,if=ide,index=0,media=disk" -initrd build/images/initrd.tar \
    -netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
    -serial stdio -no-reboot "$@"
