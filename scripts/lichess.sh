#!/bin/bash
set -e

cd "$(dirname "$0")/.."
cmake --build build --target lichess
exec ./build/lichess "$@"
