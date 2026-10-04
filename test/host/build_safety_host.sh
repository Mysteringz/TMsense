#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-$ROOT/build}"
mkdir -p "$OUT"
LIBS=(-lcrypto)
if [ "$(uname)" = Darwin ]; then
  CRYPTO_PREFIX="$(brew --prefix openssl@3)"
  export CPLUS_INCLUDE_PATH="$CRYPTO_PREFIX/include${CPLUS_INCLUDE_PATH:+:$CPLUS_INCLUDE_PATH}"
  export LIBRARY_PATH="$CRYPTO_PREFIX/lib${LIBRARY_PATH:+:$LIBRARY_PATH}"
fi
g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations \
    -fsanitize=undefined -fno-sanitize-recover=undefined -DTM_HOST_TEST \
    -I"$ROOT/test/host/arduino" -I"$ROOT/test/host" -I"$ROOT/include" \
    "$ROOT/test/host/safety_test.cpp" "$ROOT/src/tm_settings.cpp" "$ROOT/src/tm_detector.cpp" \
    "$ROOT/src/tm_cloud_proto.cpp" "$ROOT/src/MLX90640_I2C_Driver.cpp" "$ROOT/src/MLX90640_API.cpp" \
    -o "$OUT/safety_test" ${LIBS[@]+"${LIBS[@]}"}
echo "$OUT/safety_test"
