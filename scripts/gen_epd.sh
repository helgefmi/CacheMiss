#!/bin/bash
# Build and run the PGN to EPD converter
#
# Usage: ./scripts/gen_epd.sh <input.pgn> <output.epd> [options]
# Example: ./scripts/gen_epd.sh data/lichess.pgn openings.epd -n 250

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# Build if needed
if [ ! -f "$PROJECT_DIR/build/pgn2epd" ] || [ "$PROJECT_DIR/CMakeLists.txt" -nt "$PROJECT_DIR/build/pgn2epd" ]; then
    echo "Building..."
    cmake -S "$PROJECT_DIR" -B "$PROJECT_DIR/build" > /dev/null
    cmake --build "$PROJECT_DIR/build" --target pgn2epd
fi

# Run converter
exec "$PROJECT_DIR/build/pgn2epd" "$@"
