#!/bin/sh
rm -rf build

# aarch64 build (only supported architecture - whisper.cpp CPU inference
# needs ARTPEC-8/9)
docker build --progress=plain --no-cache --build-arg ARCH=aarch64 --tag acap .
container_id=$(docker create acap)
docker cp "$container_id":/opt/app ./build
docker rm "$container_id"
mv build/*.eap .
rm -rf build
