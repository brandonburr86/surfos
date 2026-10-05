# Shared paths for Docker builds and host QEMU. Source this from a devenv script.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${SURFOS_OUT:-$ROOT/build/docker}"
LOG="${SURFOS_LOG:-$ROOT/build/docker-build.log}"
case "$OUT" in /*) ;; *) OUT="$PWD/$OUT" ;; esac
case "$LOG" in /*) ;; *) LOG="$PWD/$LOG" ;; esac
LEVEL="${O:-0}"
KERNEL="$OUT/build/O$LEVEL/surfos.bin"
DISK="$OUT/build/images/test.img"
INITRD="$OUT/build/images/initrd.tar"

require_boot_files() {
    for artifact in "$KERNEL" "$DISK" "$INITRD"; do
        [ -s "$artifact" ] || {
            echo "Missing $artifact - run devenv/build.sh (O=$LEVEL) first." >&2
            return 1
        }
    done
}
