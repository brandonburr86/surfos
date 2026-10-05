#!/bin/bash
# Boot the USB image under this machine's QEMU and UEFI firmware (OVMF), from an
# emulated USB stick, and run the boot tests (tools/qemu-usb.py --test).
#
#   devenv/test-usb.sh                       build/usb/surfos-ideapad-15iil05.img
#   devenv/test-usb.sh path/to/image.img     another copy of the image
#   devenv/test-usb.sh --screen              just boot it and print the screen
#
# Needs qemu-system-x86_64 with its EDK2 firmware (brew install qemu ships both).
# Evidence (serial transcripts, screenshots, results.json) goes to build/usb-test/.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="$ROOT/build/usb/surfos-ideapad-15iil05.img"
if [ $# -gt 0 ] && [ "${1#-}" = "$1" ]; then
    IMAGE="$1"
    shift
fi
[ -s "$IMAGE" ] || { echo "No image at $IMAGE - run devenv/build-usb.sh first." >&2; exit 1; }
MODE=(--test)
[ $# -gt 0 ] && MODE=("$@")
exec python3 "$ROOT/tools/qemu-usb.py" --image "$IMAGE" --output-dir "$ROOT/build/usb-test" "${MODE[@]}"
