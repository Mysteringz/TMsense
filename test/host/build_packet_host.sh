#!/usr/bin/env bash
# Compile the firmware's packet code for the host. Used by TMedge's crosscheck.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-$ROOT/build/packet_host}"
mkdir -p "$(dirname "$OUT")"
# The mbedTLS shim uses CommonCrypto on macOS (linked implicitly) and OpenSSL
# elsewhere, which has to be named -- this is what lets CI run on Linux.
LIBS=()
[ "$(uname)" = Darwin ] || LIBS=(-lcrypto)
g++ -std=c++17 -O1 -Wall -Wextra -Werror -Wno-unused-parameter -DTM_HOST_TEST \
    -I"$ROOT/test/host" -I"$ROOT/include" \
    "$ROOT/test/host/packet_host.cpp" "$ROOT/src/tm_packet.cpp" "$ROOT/src/tm_detector.cpp" -o "$OUT" \
    ${LIBS[@]+"${LIBS[@]}"}
echo "$OUT"
