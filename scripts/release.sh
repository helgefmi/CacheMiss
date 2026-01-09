#!/bin/bash
# Build a release version of CacheMiss
#
# Usage: ./scripts/release.sh <suffix>
# Example: ./scripts/release.sh v1.0
#          -> builds/cachemiss.v1.0

set -e

if [ -z "$1" ]; then
    echo "Usage: $0 <suffix>"
    echo "Example: $0 v1.0"
    exit 1
fi

SUFFIX="$1"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build-release"
OUTPUT_DIR="$PROJECT_DIR/builds"
OUTPUT_NAME="cachemiss.$SUFFIX"
OUTPUT_PATH="$OUTPUT_DIR/$OUTPUT_NAME"

echo "Building release: $OUTPUT_NAME"

# Configure release build
cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release > /dev/null

# Build
cmake --build "$BUILD_DIR" --target cachemiss

# Create output directory
mkdir -p "$OUTPUT_DIR"

# Copy binary
rm -f "$OUTPUT_PATH"
cp "$BUILD_DIR/cachemiss" "$OUTPUT_PATH"

echo "Built: $OUTPUT_PATH"
