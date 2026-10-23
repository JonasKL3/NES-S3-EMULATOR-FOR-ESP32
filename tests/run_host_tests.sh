#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/save"
g++ -std=c++17 -Wall -Wextra -Werror  -I firmware/NES_S3 -I tests/stubs  -DHALO_SAVE_ROOT=\"$TMP/save\"  -DHALO_LIB_ROOT=\"$TMP/save\"  -DTEST_SAVE_DIR=\"$TMP/save\"  tests/test_saves.cpp firmware/NES_S3/halo_savestates.cpp firmware/NES_S3/halo_library.cpp  -o "$TMP/test_saves"
"$TMP/test_saves"
