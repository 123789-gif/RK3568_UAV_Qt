#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/out/host-plate-mode-test}"

mkdir -p "$BUILD_DIR"
g++ -std=c++17 -Wall -Wextra -Werror \
    -I"$PROJECT_ROOT/src" \
    "$PROJECT_ROOT/tests/plate_mode_config_test.cpp" \
    -o "$BUILD_DIR/plate_mode_config_test"

"$BUILD_DIR/plate_mode_config_test"
