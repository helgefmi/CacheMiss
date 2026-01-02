#pragma once

#include "cachemiss.hpp"
#include <string>
#include <vector>

struct EPDEntry {
    std::string fen;
    std::vector<u64> expected_nodes;  // expected_nodes[i] = perft(depth i+1)
};

struct WACEntry {
    std::string fen;
    std::vector<std::string> best_moves;  // One or more acceptable best moves (in SAN)
    std::string id;
};

std::vector<EPDEntry> parse_epd_file(const std::string& filename);
std::vector<WACEntry> parse_wac_file(const std::string& filename);
