#pragma once

#include <string>
#include <cstddef>

void bench_perftsuite(const std::string& filename, int max_depth, size_t mem_mb = 512);
void bench_wac(const std::string& filename, int time_limit_ms = 1000, size_t mem_mb = 512, const std::string& filter_id = "");
