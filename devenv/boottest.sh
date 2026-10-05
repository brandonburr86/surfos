#!/bin/bash
# Headless VGA/keyboard check. The full serial/network suite is build.sh test-all.
# Usage: devenv/boottest.sh [seconds] [command]
set -euo pipefail
source "$(dirname "$0")/common.sh"
require_boot_files
WAIT="${1:-15}"
TYPE="${2:-}"
DEST="$(dirname "$LOG")"
mkdir -p "$DEST"
TMP="$(mktemp -d /tmp/surfos-boottest.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT
cd "$OUT"
{
    sleep "$WAIT"
    if [ -n "$TYPE" ]; then
        for ((i = 0; i < ${#TYPE}; i++)); do
            ch="${TYPE:$i:1}"; [ "$ch" = " " ] && ch=spc
            echo "sendkey $ch"; sleep 0.2
        done
        echo "sendkey ret"; sleep 3
    fi
    echo "info status"
    echo "screendump $TMP/boot.png -f png"
    sleep 1
    echo quit
} | qemu-system-i386 -m 64 -kernel "$KERNEL" \
      -drive "file=$DISK,format=raw,if=ide,index=0,media=disk,snapshot=on" -initrd build/images/initrd.tar \
      -netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
      -display none -monitor stdio -action reboot=shutdown,shutdown=pause \
      -d cpu_reset,guest_errors -D "$DEST/qemu.log" > "$TMP/monitor.txt" 2>&1
if [ ! -s "$TMP/boot.png" ]; then
    cat "$TMP/monitor.txt" >&2
    exit 1
fi
mv "$TMP/boot.png" "$DEST/boot.png"
echo "Screenshot: $DEST/boot.png"
if ! grep -q 'VM status: running' "$TMP/monitor.txt"; then
    echo "SurfOS reset or crashed - see $DEST/qemu.log" >&2
    exit 1
fi
echo "QEMU was still running. Run build.sh test-all for kernel smoke tests."
