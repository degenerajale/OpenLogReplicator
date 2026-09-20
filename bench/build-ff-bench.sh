#!/usr/bin/env bash
#
# Build the FAST_FILTER synthetic microbenchmark (bench/ff-bench.cpp).
#
# Requires an optimized (Release) OLR build tree, because the default
# cmake-build-debug is -O0 + ASAN and is useless for timing.
#
# Usage:
#   cmake -S . -B bench-build -DCMAKE_BUILD_TYPE=Release \
#         -DWITH_OCI=<oci> -DWITH_PROMETHEUS=<prom> -DWITH_RDKAFKA=<kafka> \
#         -DWITH_RAPIDJSON=<rapidjson> -DWITH_TESTS=OFF
#   cmake --build bench-build -j
#   bench/build-ff-bench.sh bench-build
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/bench-build}"
CXX="${CXX:-g++}"
OCI="${OCI:-/home/user/oracle-instantclient/instantclient_23_26}"
RDKAFKA="${RDKAFKA:-/home/user/local/librdkafka}"
PROMETHEUS="${PROMETHEUS:-/home/user/local/prometheus}"
RAPIDJSON="${RAPIDJSON:-/home/user/local}"

FLAGS=(-std=c++17 -O3 -DNDEBUG -DCTXASSERT=0
       -DLINK_LIBRARY_OCI -DLINK_LIBRARY_PROMETHEUS -DLINK_LIBRARY_RDKAFKA
       -isystem "$RAPIDJSON/include"
       -isystem "$OCI/sdk/include"
       -isystem "$RDKAFKA/include"
       -isystem "$PROMETHEUS/include")

echo "compiling bench/ff-bench.cpp"
"$CXX" -c "$ROOT/bench/ff-bench.cpp" -o "$ROOT/bench/ff-bench.o" "${FLAGS[@]}" -I "$ROOT/src"

LINK_TXT="$BUILD/CMakeFiles/OpenLogReplicator.dir/link.txt"
[ -f "$LINK_TXT" ] || { echo "missing $LINK_TXT - build the Release tree first"; exit 2; }
CMD="$(cat "$LINK_TXT")"
CMD="${CMD//CMakeFiles\/OpenLogReplicator.dir\/src\/OpenLogReplicator.cpp.o/}"
CMD="${CMD//CMakeFiles\/OpenLogReplicator.dir\/src\/main.cpp.o/}"
CMD="${CMD/ -o OpenLogReplicator/ $ROOT/bench/ff-bench.o -o ff-bench}"

echo "linking $BUILD/ff-bench"
( cd "$BUILD" && eval "$CMD" )
echo "ok: $BUILD/ff-bench"
