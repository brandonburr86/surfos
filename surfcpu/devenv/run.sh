#!/bin/sh
# Run a command in the SurfCPU toolchain container, with this repository mounted at /work
# and the working directory carried over. On the Mac:
#
#   surfcpu/devenv/run.sh make -C surfcpu test-cpu
#   surfcpu/devenv/run.sh versions          # what the image holds
#   surfcpu/devenv/run.sh shell             # an interactive shell inside it
#
# The build identity (git commit, dirty state) is worked out here on the Mac, because a git
# worktree's metadata lives outside the mounted tree, and passed in as SURFCPU_BUILD_ID /
# SURFCPU_BUILD_DIRTY. SURFCPU_IMAGE overrides the image; DOCKER the docker binary.
set -eu

IMAGE_TAG=2026-10-15
image=${SURFCPU_IMAGE:-surfcpu-toolchain:$IMAGE_TAG}

here=$(cd "$(dirname "$0")" && pwd -P)
repo=$(cd "$here/../.." && pwd -P)
cwd=$(pwd -P)
case "$cwd/" in
  "$repo"/*) sub=${cwd#"$repo"} ;;
  *)         sub=/surfcpu ;;
esac

docker=${DOCKER:-docker}
if ! command -v "$docker" >/dev/null 2>&1; then
  docker=/Applications/Docker.app/Contents/Resources/bin/docker
fi
if ! "$docker" info >/dev/null 2>&1; then
  echo "run.sh: Docker is not running (start Docker Desktop: open -a Docker)" >&2
  exit 2
fi
if ! "$docker" image inspect "$image" >/dev/null 2>&1; then
  echo "run.sh: image $image is missing; build it with surfcpu/devenv/build-image.sh" >&2
  exit 2
fi

build_id=${SURFCPU_BUILD_ID:-$(git -C "$repo" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)}
if [ -z "${SURFCPU_BUILD_DIRTY:-}" ]; then
  if [ -n "$(git -C "$repo" status --porcelain --untracked-files=no 2>/dev/null)" ]; then
    SURFCPU_BUILD_DIRTY=1
  else
    SURFCPU_BUILD_DIRTY=0
  fi
fi

tty=
if [ -t 0 ] && [ -t 1 ]; then tty=-it; fi

case "${1:-}" in
  versions) set -- cat /etc/surfcpu-toolchain.txt ;;
  shell)    set -- bash ;;
  "")       echo "usage: $0 <command> [args...] | versions | shell" >&2; exit 2 ;;
esac

exec "$docker" run --rm $tty \
  -v "$repo:/work" -w "/work$sub" \
  -e HOME=/tmp -e SURFCPU_IN_CONTAINER=1 \
  -e SURFCPU_BUILD_ID="$build_id" -e SURFCPU_BUILD_DIRTY="$SURFCPU_BUILD_DIRTY" \
  -e SURFCPU_HOST="$(uname -s)-$(uname -m)" \
  "$image" "$@"
