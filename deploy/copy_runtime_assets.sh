#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 <qmake-output-directory>" >&2
    exit 2
fi

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTPUT_DIR="$1"

mkdir -p "$OUTPUT_DIR/models" "$OUTPUT_DIR/config"
cp -f "$PROJECT_ROOT"/assets/models/*.rknn "$OUTPUT_DIR/models/"
cp -f "$PROJECT_ROOT/assets/models/SHA256SUMS" "$OUTPUT_DIR/models/"
cp -f "$PROJECT_ROOT/assets/config/runtime.env" "$OUTPUT_DIR/config/"
