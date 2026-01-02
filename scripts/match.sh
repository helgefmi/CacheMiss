#!/bin/bash
# Build and run the match supervisor
#
# Usage: ./scripts/match.sh <engine1> <engine2> [options]
# Example: ./scripts/match.sh ./build/cachemiss ./build/cachemiss_v2 -movetime 100

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# Build if needed
if [ ! -f "$PROJECT_DIR/build/match" ] || [ "$PROJECT_DIR/CMakeLists.txt" -nt "$PROJECT_DIR/build/match" ]; then
    echo "Building..."
    cmake -S "$PROJECT_DIR" -B "$PROJECT_DIR/build" > /dev/null
    cmake --build "$PROJECT_DIR/build" --target match
fi

# Run match
exec "$PROJECT_DIR/build/match" "$@"
