#!/bin/bash
# Build the flashable USB image for the Lenovo IdeaPad 3 15IIL05 (docs/USB-BOOT.md).
#
#   devenv/build-usb.sh          optimized kernel and programs (O=2)
#   O=0 devenv/build-usb.sh      the debug build instead
#
# Builds in the Docker toolchain (devenv/build.sh usb-image) and copies the image, its
# .sha256 file and build-manifest.json to build/usb/ in this tree, checking the checksum
# on the way. It writes files only: flashing a USB drive is a separate, deliberate step
# with a raw-image writer (see docs/USB-BOOT.md).
set -euo pipefail
LEVEL="${O:-2}"
export O="$LEVEL"
source "$(dirname "$0")/common.sh"
NAME=surfos-ideapad-15iil05.img

# 10/10/26: the Wi-Fi firmware for the laptop's card (firmware/, checked against its SHA-256 values);
# without a network the files already there are used, and without them the image has no Wi-Fi firmware
python3 "$ROOT/tools/fetch-firmware.py" || echo "build-usb: no Wi-Fi firmware (tools/fetch-firmware.py failed): the image works without Wi-Fi" >&2

"$ROOT/devenv/build.sh" O="$LEVEL" usb-image

SRC="$OUT/build/usb"
DEST="$ROOT/build/usb"
for f in "$NAME" "$NAME.sha256" build-manifest.json; do
    [ -s "$SRC/$f" ] || { echo "The build did not produce $SRC/$f" >&2; exit 1; }
done
mkdir -p "$DEST"
STAGE="$(mktemp -d "$DEST/.copy.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT
cp "$SRC/$NAME" "$SRC/$NAME.sha256" "$SRC/build-manifest.json" "$STAGE/"
if command -v sha256sum > /dev/null; then
    (cd "$STAGE" && sha256sum -c "$NAME.sha256" > /dev/null)
else
    (cd "$STAGE" && shasum -a 256 -c "$NAME.sha256" > /dev/null)
fi
mv -f "$STAGE/$NAME" "$STAGE/$NAME.sha256" "$STAGE/build-manifest.json" "$DEST/"

cat <<EOF

SurfOS USB image (O$LEVEL), checksum verified:
  $DEST/$NAME
  $DEST/$NAME.sha256
  $DEST/build-manifest.json
  sha256 $(cut -d' ' -f1 "$DEST/$NAME.sha256")

Test it under QEMU first:  devenv/test-usb.sh
Then write the whole .img to a USB drive with a raw-image writer (balenaEtcher,
Raspberry Pi Imager, Rufus in DD mode, or dd to a device you have checked twice).
Everything on that drive is replaced. Step by step: docs/USB-BOOT.md
EOF
