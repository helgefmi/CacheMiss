#pragma once

#include <string>
#include <cstddef>

void bench_perftsuite(const std::string& filename, int max_depth, size_t mem_mb = 512);
