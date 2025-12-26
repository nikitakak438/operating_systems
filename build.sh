#!/usr/bin/env bash
set -euo pipefail

rm -rf build
mkdir -p build
cmake -S . -B build
cmake --build build -j
echo "Build done. Binaries are in ./bin"

