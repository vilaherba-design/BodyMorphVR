#!/bin/sh
# Unit tests without the game (Linux, g++ or clang++ with C++23). Usage: tests/run.sh
set -e
cd "$(dirname "$0")"
CXX=${CXX:-g++}
OUT=${TMPDIR:-/tmp}/bodymorph_tri_test
$CXX -std=c++23 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -o "$OUT" tri_test.cpp ../src/TriFile.cpp
"$OUT"
