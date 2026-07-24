#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 <qmake-output-directory>" >&2
    exit 2
fi

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTPUT_DIR="$1"
DRIVER_SOURCE_DIR="$PROJECT_ROOT/assets/driver/rk3568-kernel-6.1.99"

mkdir -p "$OUTPUT_DIR/models" "$OUTPUT_DIR/config" "$OUTPUT_DIR/driver"

cp -f "$PROJECT_ROOT"/assets/models/*.rknn "$OUTPUT_DIR/models/"
cp -f "$PROJECT_ROOT/assets/models/SHA256SUMS" "$OUTPUT_DIR/models/"
cp -f "$PROJECT_ROOT"/assets/config/* "$OUTPUT_DIR/config/"

for file in pango_pci_driver.ko SHA256SUMS DRIVER_INFO.txt; do
    [[ -f "$DRIVER_SOURCE_DIR/$file" ]] || {
        echo "ERROR: missing PCIe driver asset: $DRIVER_SOURCE_DIR/$file" >&2
        exit 1
    }
done

cp -f "$DRIVER_SOURCE_DIR"/pango_pci_driver.ko \
      "$DRIVER_SOURCE_DIR"/SHA256SUMS \
      "$DRIVER_SOURCE_DIR"/DRIVER_INFO.txt \
      "$OUTPUT_DIR/driver/"
