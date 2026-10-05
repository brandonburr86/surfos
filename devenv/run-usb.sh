#!/bin/bash
# Try the USB image by hand: boot it in a QEMU window with UEFI firmware (OVMF) from an
# emulated USB stick, as the laptop would. The boot log and the shell also appear on this
# terminal (serial). A copy of the image is used, so the .img file is never changed.
#
#   devenv/run-usb.sh                      build/usb/surfos-ideapad-15iil05.img
#   devenv/run-usb.sh path/to/image.img    another copy of the image
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="$ROOT/build/usb/surfos-ideapad-15iil05.img"
if [ $# -gt 0 ] && [ -f "$1" ]; then
    IMAGE="$1"
    shift
fi
[ -s "$IMAGE" ] || { echo "No image at $IMAGE - run devenv/build-usb.sh first." >&2; exit 1; }
exec python3 "$ROOT/tools/qemu-usb.py" --image "$IMAGE" --mem 1024 --run "$@"
