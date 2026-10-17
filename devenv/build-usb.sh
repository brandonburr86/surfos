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
#
# 10/17/26 (one stick): also build/usb/stick/, the image's files as a folder to copy onto a
# FAT32 stick instead (tools/stick.py, docs/USB-BOOT.md section 14), checked against the
# manifest as it is copied and replaced as a whole.
set -euo pipefail
LEVEL="${O:-2}"
export O="$LEVEL"
source "$(dirname "$0")/common.sh"
NAME=surfos-ideapad-15iil05.img

# 10/10/26: the Wi-Fi firmware for the laptop's card (firmware/, checked against its SHA-256 values);
# without a network the files already there are used, and without them the image has no Wi-Fi firmware
python3 "$ROOT/tools/fetch-firmware.py" || echo "build-usb: no Wi-Fi firmware (tools/fetch-firmware.py failed): the image works without Wi-Fi" >&2

# 10/11/26: Mbed TLS for the web browser's HTTPS (third_party/README.md), checked against its pinned SHA-256; an
# image is not made without it (devenv/build.sh fetches it too, for every build)
python3 "$ROOT/tools/fetch-webdeps.py" \
    || { echo "build-usb: Mbed TLS is missing (tools/fetch-webdeps.py, above): no image without the browser's HTTPS" >&2; exit 1; }

"$ROOT/devenv/build.sh" O="$LEVEL" usb-image

SRC="$OUT/build/usb"
DEST="$ROOT/build/usb"
for f in "$NAME" "$NAME.sha256" build-manifest.json stick/EFI/BOOT/BOOTX64.EFI; do
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
# no extended attributes on macOS, which Finder would carry onto a FAT stick as ._ files
if [ "$(uname -s)" = Darwin ]; then cp -RX "$SRC/stick" "$STAGE/stick"; else cp -R "$SRC/stick" "$STAGE/stick"; fi
python3 "$ROOT/tools/stick.py" kit --kit "$STAGE/stick" --manifest "$STAGE/build-manifest.json" > /dev/null
# the old folder is replaced only when it holds nothing but a stick folder's files (and Finder's .DS_Store)
python3 "$ROOT/tools/mkusb.py" --check-stick-dir "$DEST/stick" \
    || { echo "build-usb: nothing was replaced; move those files out of $DEST/stick, then build again" >&2; exit 1; }
mv -f "$STAGE/$NAME" "$STAGE/$NAME.sha256" "$STAGE/build-manifest.json" "$DEST/"
if [ -d "$DEST/stick" ]; then mv "$DEST/stick" "$STAGE/stick.old"; fi
mv "$STAGE/stick" "$DEST/stick"

cat <<EOF

SurfOS USB image (O$LEVEL), checksum verified:
  $DEST/$NAME
  $DEST/$NAME.sha256
  $DEST/build-manifest.json
  sha256 $(cut -d' ' -f1 "$DEST/$NAME.sha256")
  $DEST/stick/   (the same files, to copy onto a FAT32 stick)

Test it under QEMU first:  devenv/test-usb.sh
Then write the whole .img to a USB drive with a raw-image writer (balenaEtcher,
Raspberry Pi Imager, Rufus in DD mode, or dd to a device you have checked twice).
Everything on that drive is replaced. Step by step: docs/USB-BOOT.md

Or keep your own FAT32 stick and its files, with no image to write (section 14):
  tools/stick.py install /Volumes/<the stick>    the first time
  tools/stick.py update /Volumes/<the stick>     each new build after that
EOF
