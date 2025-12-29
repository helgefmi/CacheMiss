#!/bin/bash
set -e

cd "$(dirname "$0")/.."

cmake -S . -B build
cmake --build build --target gen_magics

./build/gen_magics > src/magic_tables.hpp
