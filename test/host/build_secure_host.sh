#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-$ROOT/build}"
mkdir -p "$OUT"
FLAGS=(-std=c++17 -O1 -g -Wall -Wextra -Werror -Wno-deprecated-declarations -fsanitize=undefined -fno-sanitize-recover=undefined -I"$ROOT/include")
LIBS=(-lmbedcrypto)
# Linux validates the real mbedTLS API, rather than only the OpenSSL adapter.
if [ "$(uname)" = Darwin ]; then
  CRYPTO_PREFIX="$(brew --prefix openssl@3)"
  FLAGS+=(-I"$ROOT/test/host" -I"$CRYPTO_PREFIX/include")
  LIBS=(-L"$CRYPTO_PREFIX/lib" -lcrypto)
fi
g++ "${FLAGS[@]}" "$ROOT/test/host/secure_test.cpp" "$ROOT/src/tm_secure.cpp" "$ROOT/src/tm_packet.cpp" "$ROOT/src/tm_detector.cpp" -o "$OUT/secure_test" "${LIBS[@]}"
"$OUT/secure_test"
