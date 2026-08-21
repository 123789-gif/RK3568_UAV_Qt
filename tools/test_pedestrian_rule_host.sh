#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTPUT="${TMPDIR:-/tmp}/pedestrian_rule_logic_test"

g++ -std=c++17 -Wall -Wextra -Werror \
    -I"$PROJECT_ROOT/src" \
    "$PROJECT_ROOT/tests/pedestrian_rule_logic_test.cpp" \
    -o "$OUTPUT"
"$OUTPUT"
