#!/bin/sh
set -eu

# armv7hf builds run in external-only mode (no bundled whisper.cpp/model).
"$(dirname "$0")/build.sh" armv7hf