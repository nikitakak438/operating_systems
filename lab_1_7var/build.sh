#!/usr/bin/env bash
set -e

OUT="mydaemon"

g++ -std=c++17 -Wall -Werror -O2 main.cpp -o "$OUT"

echo "Build successful: $OUT"

