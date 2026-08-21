#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTPUT="${TMPDIR:-/tmp}/vehicle_wrong_way_engine_test"

g++ -std=c++17 -Wall -Wextra -Werror \
    -I"$PROJECT_ROOT/src" \
    "$PROJECT_ROOT/tests/vehicle_wrong_way_engine_test.cpp" \
    "$PROJECT_ROOT/src/npu/vehicle_wrong_way_engine.cpp" \
    -o "$OUTPUT"
"$OUTPUT"
