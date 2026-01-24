cmake_minimum_required(VERSION 3.20)

project(CacheMiss VERSION 0.1.0 LANGUAGES CXX)

# LTO: use thin for Clang, regular for GCC
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(LTO_FLAG "-flto=thin")
else()
    set(LTO_FLAG "-flto")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Fetch FTXUI for terminal UI
include(FetchContent)
FetchContent_Declare(ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/ftxui
    GIT_TAG main
)
FetchContent_MakeAvailable(ftxui)

# Default to RelWithDebInfo if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Debug build: sanitizers, all warnings, no optimization
set(CMAKE_CXX_FLAGS_DEBUG
    "-O0 -g3 -fno-omit-frame-pointer \
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion \
    -Wnull-dereference -Wdouble-promotion -Wformat=2 \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -D_GLIBCXX_ASSERTIONS")
set(CMAKE_EXE_LINKER_FLAGS_DEBUG "-fsanitize=address,undefined")

# RelWithDebInfo: optimized with debug symbols for profiling
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO
    "-O3 -g -DNDEBUG -fno-omit-frame-pointer \
    -march=native -mtune=native \
    ${LTO_FLAG} \
    -Wall -Wextra -Wpedantic")
set(CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO "${LTO_FLAG}")

# Release: maximum performance, no debug info
set(CMAKE_CXX_FLAGS_RELEASE
    "-O3 -DNDEBUG \
    -march=native -mtune=native \
    ${LTO_FLAG}")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${LTO_FLAG}")

# Profile: optimized for perf analysis (no LTO for accurate function attribution)
set(CMAKE_CXX_FLAGS_PROFILE
    "-O3 -g -DNDEBUG -fno-omit-frame-pointer \
    -march=native -mtune=native \
    -Wall -Wextra -Wpedantic")
set(CMAKE_EXE_LINKER_FLAGS_PROFILE "")

# Core chess library (shared between engine and tools)
add_library(cachemiss_core STATIC
    src/board.cpp
    src/move.cpp
    src/zobrist.cpp
    src/eval.cpp
    src/pawn_cache.cpp
    src/perft.cpp
    src/epd.cpp
    src/ttable.cpp
)
target_include_directories(cachemiss_core PUBLIC src)

# Main engine executable
add_executable(cachemiss
    src/main.cpp
    src/uci.cpp
    src/search.cpp
    src/bench.cpp
)
target_link_libraries(cachemiss cachemiss_core)

# Match supervisor tool (with FTXUI for TUI)
add_executable(match tools/match.cpp)
target_link_libraries(match cachemiss_core ftxui::screen ftxui::dom ftxui::component)

# PGN to EPD converter tool
add_executable(pgn2epd tools/pgn2epd.cpp)
target_link_libraries(pgn2epd cachemiss_core)

# Magic bitboard generator (standalone tool)
add_executable(gen_magics tools/gen_magics.cpp)
target_include_directories(gen_magics PRIVATE src)

# Eval tuner tool (with OpenMP parallelization)
find_package(OpenMP REQUIRED)
add_executable(tune_eval tools/tune_eval.cpp)
target_link_libraries(tune_eval cachemiss_core OpenMP::OpenMP_CXX)

# WAC comparison tool
add_executable(wac_compare tools/wac_compare.cpp)
target_link_libraries(wac_compare pthread)

# Test suite
add_executable(run_tests
    tests/test_main.cpp
    tests/test_movegen.cpp
    tests/test_see.cpp
    tests/test_eval.cpp
    tests/test_search.cpp
    tests/test_hash.cpp
    tests/test_board.cpp
    tests/test_perft.cpp
    tests/test_uci.cpp
    src/search.cpp
    src/uci.cpp
)
target_link_libraries(run_tests cachemiss_core)
target_include_directories(run_tests PRIVATE tests)
