#!/bin/sh
# Build the plugin (and, with "test", the offline drawing test) with MinGW-w64.
set -eu
CXX=${CXX:-x86_64-w64-mingw32-g++}
FLAGS="-std=c++17 -O2 -Wall -Wextra -static -s"
$CXX $FLAGS -shared -o MGS2HiResFont.asi MGS2HiResFont.cpp
if [ "${1:-}" = test ]; then
    $CXX $FLAGS -municode -DMGS2HIRES_TEST -o test.exe MGS2HiResFont.cpp
fi
