#!/bin/sh
# Build the openXC7 image (surfcpu-openxc7:<IMAGE_TAG>) from devenv/openxc7/Dockerfile and
# print what it holds. Everything is compiled from pinned sources, natively for the Docker
# host's architecture (arm64 on the MacBook), which takes about half an hour the first time.
#
# The build runs on a builder of its own (BuildKit in a container, BUILDER below), so its
# cache never mixes with other projects' Docker caches; the image is then loaded into Docker.
#   build-image.sh            build (reusing that builder's cache) and load the image
#   build-image.sh --rm-builder   afterwards remove the builder and its whole cache
# JOBS (default 6) is the compile parallelism; DOCKER the docker binary.
set -eu
IMAGE_TAG=2026-10-15
here=$(cd "$(dirname "$0")" && pwd -P)
image="surfcpu-openxc7:$IMAGE_TAG"
builder=${BUILDER:-surfcpu-openxc7-builder}
jobs=${JOBS:-6}

docker=${DOCKER:-docker}
command -v "$docker" >/dev/null 2>&1 || docker=/Applications/Docker.app/Contents/Resources/bin/docker
"$docker" info >/dev/null 2>&1 || { echo "build-image.sh: Docker is not running" >&2; exit 2; }

if ! "$docker" buildx inspect "$builder" >/dev/null 2>&1; then
  "$docker" buildx create --name "$builder" --driver docker-container >/dev/null
fi

start=$(date +%s)
"$docker" buildx build --builder "$builder" --load --progress=plain \
  --build-arg JOBS="$jobs" -t "$image" "$here"
end=$(date +%s)
echo "build-image.sh: $image built in $((end - start)) s"
"$docker" image inspect "$image" --format 'build-image.sh: {{.Id}} {{.Architecture}} {{.Size}} bytes'
"$docker" run --rm "$image" cat /etc/surfcpu-openxc7.txt

if [ "${1:-}" = "--rm-builder" ]; then
  "$docker" buildx rm "$builder"
fi
