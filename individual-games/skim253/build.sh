#!/usr/bin/env bash
# Builds the golf game against the engine libraries already in build/.
#
# The root CMakeLists.txt declares only the engine, so there is no
# CMake target for this game; this compiles it directly and drops the binary in
# this folder. Run ../../build/ first (cmake -S . -B build && cmake --build build)
# if libengine.a is missing.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
build="$root/build"

for lib in libengine.a libengine-geometry.a libengine-time.a vendored/SDL/libSDL3.dylib; do
    if [[ ! -e "$build/$lib" ]]; then
        echo "missing $build/$lib -- build the engine first:" >&2
        echo "  cmake -S \"$root\" -B \"$build\" && cmake --build \"$build\"" >&2
        exit 1
    fi
done


c++ -std=c++17 -Wall -Wextra -O2 \
    -I"$root/include" -I"$root/vendored/SDL/include" \
    "$here/main.cpp" \
    "$build/libengine.a" "$build/libengine-geometry.a" "$build/libengine-time.a" \
    "$build/vendored/SDL/libSDL3.dylib" \
    -Wl,-rpath,"$build/vendored/SDL" \
    -o "$here/golf-game"

echo "built $here/golf-game"
