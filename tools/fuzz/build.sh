#!/bin/sh
# Build the fuzzing harnesses into build/fuzz (ignored by git).
#   build/fuzz/san   cx + libcx_core.a with UBSan and the libstdc++ debug mode
#   build/fuzz/rel   cx + libcx_core.a, release
#   build/fuzz/bin   harness binaries and guard.so
# usage: tools/fuzz/build.sh [JOBS]
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=$root/build/fuzz
jobs=${1:-12}
san="-O1 -fsanitize=undefined -fno-sanitize-recover=all -D_GLIBCXX_DEBUG -fno-omit-frame-pointer -rdynamic"

mkdir -p "$out/bin"
cmake -S "$root" -B "$out/san" -DCMAKE_BUILD_TYPE=Debug -DCX_BUILD_TESTS=OFF \
  "-DCMAKE_CXX_FLAGS=$san" -DCMAKE_CXX_FLAGS_DEBUG=-g >/dev/null
cmake --build "$out/san" -j "$jobs"
cmake -S "$root" -B "$out/rel" -DCMAKE_BUILD_TYPE=Release -DCX_BUILD_TESTS=OFF >/dev/null
cmake --build "$out/rel" -j "$jobs"

# Correctness harnesses: sanitizer build.
for h in view_fuzz func_fuzz parse_fuzz render_fuzz mgr_fuzz roundtrip regex_fuzz; do
  g++ -std=c++23 -g $san -I"$root/src" "$here/$h.cpp" "$out/san/libcx_core.a" -o "$out/bin/$h" &
done
# Timing harnesses and the table minimiser: release build.
for h in perf_probe table_min bench_multicursor_line; do
  g++ -std=c++23 -O2 -g -I"$root/src" "$here/$h.cpp" "$out/rel/libcx_core.a" -o "$out/bin/$h" &
done
gcc -O1 -shared -fPIC -o "$out/bin/guard.so" "$here/guard.c" -ldl &
wait
echo "harnesses in $out/bin"
