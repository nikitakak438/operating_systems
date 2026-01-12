#!/usr/bin/env bash
set -e

OUT="mydaemon"

# Удаляем старый бинарник, чтобы не мешался
rm -f "$OUT"

g++ -std=c++17 -Wall -Werror -O2 main.cpp daemon.cpp pid_manager.cpp -o "$OUT"

echo "Built $OUT"

