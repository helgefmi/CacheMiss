#!/bin/bash
# Run WAC comparison across all builds
# Usage: ./scripts/wac_compare.sh [movetime_ms] [output.html]

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

MOVETIME="${1:-1000}"
OUTPUT="${2:-wac_comparison.html}"

"$PROJECT_DIR/build/wac_compare" "$PROJECT_DIR/builds" "$PROJECT_DIR/wac.epd" "$MOVETIME" "$OUTPUT"
