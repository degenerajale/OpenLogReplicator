#!/usr/bin/env bash
#
# Build a bench source against an optimized (Release) OLR build tree by reusing that
# tree's link line. The default cmake-build-debug is -O0 + ASAN and is useless for
# timing, so a Release tree must exist first:
#
#   cmake -S . -B bench-build -DCMAKE_BUILD_TYPE=Release \
#         -DWITH_OCI=<oci> -DWITH_PROMETHEUS=<prom> -DWITH_RDKAFKA=<kafka> \
#         -DWITH_RAPIDJSON=<rapidjson> -DWITH_TESTS=OFF
#   cmake --build bench-build -j
#
# Usage:
#   bench/build-bench.sh <source.cpp> <build-dir> <output-name> [extra flags...]
#
# Examples:
#   bench/build-bench.sh bench/ff-bench.cpp bench-build ff-bench
#   bench/build-bench.sh bench/reader-bench.cpp bench-build reader-bench-new -DHAS_READ_PARALLEL
#
# The source must live inside the tree whose headers it should compile against, so the
# baseline build uses the copy of the source in that tree (relative ../src includes).
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
[ $# -ge 3 ] || { echo "usage: build-bench.sh <source.cpp> <build-dir> <output-name> [flags...]"; exit 2; }
SRC="$1"
BUILD="$2"
OUT="$3"
shift 3
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

OBJ="$(cd "$BUILD" && pwd)/$(basename "${SRC%.cpp}").o"
echo "compiling $SRC"
"$CXX" -c "$SRC" -o "$OBJ" "${FLAGS[@]}" -I "$ROOT/src" "$@"

LINK_TXT="$BUILD/CMakeFiles/OpenLogReplicator.dir/link.txt"
[ -f "$LINK_TXT" ] || { echo "missing $LINK_TXT - build the Release tree first"; exit 2; }
CMD="$(cat "$LINK_TXT")"
CMD="${CMD//CMakeFiles\/OpenLogReplicator.dir\/src\/OpenLogReplicator.cpp.o/}"
CMD="${CMD//CMakeFiles\/OpenLogReplicator.dir\/src\/main.cpp.o/}"
CMD="${CMD/ -o OpenLogReplicator/ $OBJ -o $OUT}"

echo "linking $BUILD/$OUT"
( cd "$BUILD" && eval "$CMD" )
echo "ok: $BUILD/$OUT"