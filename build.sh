#!/bin/sh
set -eu

ARCH="${1:-aarch64}"
USE_LOCAL_WHISPER=1

if [ "${ARCH}" = "armv7hf" ]; then
	USE_LOCAL_WHISPER=0
fi

rm -rf build

# Default build is aarch64 with local whisper.
# armv7hf build is external-only and excludes local whisper/model assets.
docker build --progress=plain --no-cache \
	--build-arg ARCH="${ARCH}" \
	--build-arg USE_LOCAL_WHISPER="${USE_LOCAL_WHISPER}" \
	--tag acap .
container_id=$(docker create acap)
docker cp "$container_id":/opt/app ./build
docker rm "$container_id"
mv build/*.eap .
rm -rf build
