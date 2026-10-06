#!/bin/bash
# Stream source to Docker; export only successful builds. No Documents bind mount.
set -euo pipefail
source "$(dirname "$0")/common.sh"
IMAGE=surfos-modern-toolchain
VOLUME="${SURFOS_VOLUME:-surfos-modern-build}"   # a separate volume per worktree lets builds run side by side
PLATFORM=linux/amd64
export PATH="/Applications/Docker.app/Contents/Resources/bin:$PATH"

if ! DOCKER_INFO="$(docker info 2>&1)"; then
    echo "Cannot access Docker. Start Docker Desktop and check socket permissions." >&2
    echo "$DOCKER_INFO" >&2
    exit 1
fi
# Docker's layer cache keeps this quick, and changes to the Dockerfile take effect.
docker build --platform "$PLATFORM" -t "$IMAGE" "$ROOT/devenv"
docker volume create "$VOLUME" > /dev/null

# Synchronize deletions as well as edits, preserving only generated build output.
# macOS metadata must not enter the source archive; GNU tar needs no BSD switches.
TAR_OPTIONS=()
if [ "$(uname -s)" = Darwin ]; then
    TAR_OPTIONS=(--no-xattrs --no-mac-metadata --no-acls --no-fflags)
fi
COPYFILE_DISABLE=1 tar -C "$ROOT" --exclude .git --exclude .DS_Store --exclude './build' \
    ${TAR_OPTIONS[@]+"${TAR_OPTIONS[@]}"} --format ustar -cf - . \
  | docker run --rm -i --platform "$PLATFORM" -v "$VOLUME":/surfos "$IMAGE" sh -ec \
      'mkdir /tmp/source; tar -C /tmp/source -xf -; rsync -a --delete --exclude=/build/ /tmp/source/ /surfos/'

# With no explicit target, make both the kernel and everything QEMU needs to boot.
TARGETS=()
for arg in "$@"; do
    case "$arg" in
        O=*) LEVEL="${arg#O=}" ;;
        BUILD=*) BUILD_DIR="${arg#BUILD=}" ;;
        -*|*=*) ;;
        *) TARGETS+=("$arg") ;;
    esac
done
DEFAULT_TARGETS=()
if [ "${#TARGETS[@]}" -eq 0 ]; then
    DEFAULT_TARGETS=(all images)
fi
# The volume has no .git, so the source revision for the USB image manifest comes from here.
REV="$(git -C "$ROOT" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
DIRTY=unknown
if git -C "$ROOT" rev-parse --git-dir > /dev/null 2>&1; then
    if [ -z "$(git -C "$ROOT" status --porcelain 2>/dev/null)" ]; then DIRTY=no; else DIRTY=yes; fi
fi
mkdir -p "$(dirname "$LOG")" "$(dirname "$OUT")"
set +e
docker run --rm --platform "$PLATFORM" -v "$VOLUME":/surfos -w /surfos "$IMAGE" \
    make \
    O="$LEVEL" SURFOS_REV="$REV" SURFOS_DIRTY="$DIRTY" ${DEFAULT_TARGETS[@]+"${DEFAULT_TARGETS[@]}"} "$@" 2>&1 | tee "$LOG"
STATUS=$?
set -e
if [ "$STATUS" -ne 0 ]; then
    echo "Build failed (status $STATUS). Last exported build preserved. Log: $LOG" >&2
    exit "$STATUS"
fi

# Stage the export first so an interrupted copy cannot remove a working build.
STAGE="$(mktemp -d "$(dirname "$OUT")/.surfos-export.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT
docker run --rm --platform "$PLATFORM" -v "$VOLUME":/surfos "$IMAGE" tar -C /surfos -cf - . \
    | tar -C "$STAGE" -xf -
BUILD_DIR="${BUILD_DIR:-build/O$LEVEL}"
if [ "${#TARGETS[@]}" -eq 0 ]; then
    for artifact in "$BUILD_DIR/surfos.bin" build/images/test.img build/images/initrd.tar; do
        [ -s "$STAGE/$artifact" ] || { echo "Build omitted $artifact" >&2; exit 1; }
    done
fi
rm -rf "$OUT"
mv "$STAGE" "$OUT"
trap - EXIT
for target in ${TARGETS[@]+"${TARGETS[@]}"}; do
    if [ "$target" = usb-image ] && [ -s "$OUT/build/usb/surfos-ideapad-15iil05.img" ]; then
        echo "USB image: $OUT/build/usb/surfos-ideapad-15iil05.img"
    fi
done
if [ -s "$OUT/$BUILD_DIR/surfos.bin" ]; then
    echo "Kernel: $OUT/$BUILD_DIR/surfos.bin ($(wc -c < "$OUT/$BUILD_DIR/surfos.bin" | tr -d ' ') bytes)"
else
    echo "Requested make target completed. Export: $OUT"
fi
