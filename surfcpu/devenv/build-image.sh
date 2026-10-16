#!/bin/sh
# Build the SurfCPU toolchain image from surfcpu/devenv/Dockerfile and print what it holds.
# Takes about 10 minutes the first time (Spike is compiled from source).
set -eu
here=$(cd "$(dirname "$0")" && pwd -P)
tag=$(sed -n 's/^IMAGE_TAG=//p' "$here/run.sh")
docker=${DOCKER:-docker}
command -v "$docker" >/dev/null 2>&1 || docker=/Applications/Docker.app/Contents/Resources/bin/docker
"$docker" build -t "surfcpu-toolchain:$tag" "$here"
"$docker" run --rm "surfcpu-toolchain:$tag" cat /etc/surfcpu-toolchain.txt
