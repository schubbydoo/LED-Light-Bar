#!/usr/bin/env bash
# Run the bridge's hardware-free tests. Needs only a C++ compiler.
#
#   1. test_logic.cpp   — routing, queue and pacing rules, in isolation
#   2. test_sketch.cpp  — the REAL bridge.ino, driven against a fake radio
#
# Passing both means the logic is right and the sketch behaves as designed. It
# does NOT mean the sketch compiles on the real ESP32 core — only
# `./flash.sh bridge bridge` proves that.
set -euo pipefail
cd "$(dirname "$0")"
out="$(mktemp)"
trap 'rm -f "$out"' EXIT

g++ -std=c++11 -Wall -Wextra -Werror -o "$out" test_logic.cpp
"$out"

# -fpermissive is NOT used. -Wno-unused-parameter only: the radio callbacks
# receive an info struct the sketch does not need.
g++ -std=c++17 -Wall -Wno-unused-parameter -Werror -I stubs -o "$out" test_sketch.cpp
"$out"
