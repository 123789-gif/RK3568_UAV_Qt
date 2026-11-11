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

# Remove only the former traffic assets from this project's build output.
for old in ped_det.rknn plate_det.rknn plate_reco.rknn yolov8_car.rknn yolov8_car_640x320_mmse.rknn car_classes.txt; do
    rm -f -- "$OUTPUT_DIR/models/$old"
done
rm -rf -- "$OUTPUT_DIR/models/v291_candidate"
cp -a "$PROJECT_ROOT/assets/models/." "$OUTPUT_DIR/models/"
rm -f -- "$OUTPUT_DIR/config/pedestrian_violation.ini"
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
