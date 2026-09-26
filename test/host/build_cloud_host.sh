#!/usr/bin/env bash
# Compile the firmware's direct-to-cloud transport for the host:
#   build/cloud_test   unit tests (run with no arguments from the repo root)
#   build/cloud_host   a node that speaks tmnode.v1 over a plain TCP socket,
#                      used by TMedge's `npm run crosscheck` against the real edge
# Same shims as build_packet_host.sh: CommonCrypto on macOS, OpenSSL elsewhere.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-$ROOT/build}"
mkdir -p "$OUT"
LIBS=()
[ "$(uname)" = Darwin ] || LIBS=(-lcrypto)
SRC=("$ROOT/src/tm_ws.cpp" "$ROOT/src/tm_cloud_proto.cpp" "$ROOT/src/tm_cloud_session.cpp" "$ROOT/src/tm_packet.cpp" "$ROOT/src/tm_detector.cpp")
FLAGS=(-std=c++17 -O1 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-deprecated-declarations -DTM_HOST_TEST -I"$ROOT/test/host" -I"$ROOT/include")
g++ "${FLAGS[@]}" "$ROOT/test/host/cloud_test.cpp" "${SRC[@]}" -o "$OUT/cloud_test" ${LIBS[@]+"${LIBS[@]}"}
if [ -f "$ROOT/test/host/cloud_host.cpp" ]; then
  g++ "${FLAGS[@]}" "$ROOT/test/host/cloud_host.cpp" "${SRC[@]}" -o "$OUT/cloud_host" ${LIBS[@]+"${LIBS[@]}"}
fi
echo "$OUT"
