#!/usr/bin/env bash
# Builds the engine, shell, benchmark and tests without CMake.
#   ./build.sh            # uses $CXX (default c++)
#   CXX=clang++ ./build.sh
set -euo pipefail
cd "$(dirname "$0")"

CXX=${CXX:-c++}
CXXFLAGS=${CXXFLAGS:-"-std=c++17 -O2 -Wall -Wextra"}
JOBS=${JOBS:-4}
mkdir -p build/obj

compile() {
  local src=$1
  local obj="build/obj/$(echo "$src" | tr '/' '_').o"
  $CXX $CXXFLAGS -Isrc -Itests -c "$src" -o "$obj"
  echo "$obj"
}
export -f compile
export CXX CXXFLAGS

find src -name '*.cpp' | sort | xargs -P "$JOBS" -I{} bash -c 'compile {}' > build/engine_objs.txt
find tests -name '*.cpp' | sort | xargs -P "$JOBS" -I{} bash -c 'compile {}' > build/test_objs.txt

ENGINE_OBJS=$(cat build/engine_objs.txt)
$CXX $CXXFLAGS -Isrc tools/sysr_shell.cpp $ENGINE_OBJS -o build/sysr_shell
$CXX $CXXFLAGS -Isrc tools/sysr_bench.cpp $ENGINE_OBJS -o build/sysr_bench
$CXX $CXXFLAGS $(cat build/test_objs.txt) $ENGINE_OBJS -o build/sysr_tests
echo "Built build/sysr_shell, build/sysr_bench, build/sysr_tests"
