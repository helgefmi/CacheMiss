#pragma once
#include <cstddef>

// Run draw detection tests, returns number of failures
int run_draw_tests(int time_limit_ms = 500, size_t mem_mb = 64);
