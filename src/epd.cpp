#include "epd.hpp"
#include <fstream>
#include <sstream>

std::vector<EPDEntry> parse_epd_file(const std::string& filename) {
    std::vector<EPDEntry> entries;
    std::ifstream file(filename);

    if (!file.is_open()) {
        return entries;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;

        EPDEntry entry;
        std::istringstream iss(line);
        std::string token;

        // First token before ';' is the FEN
        if (!std::getline(iss, entry.fen, ';')) continue;

        // Remaining tokens are expected node counts for depths 1, 2, 3, ...
        while (std::getline(iss, token, ';')) {
            if (!token.empty()) {
                entry.expected_nodes.push_back(std::stoull(token));
            }
        }

        if (!entry.fen.empty() && !entry.expected_nodes.empty()) {
            entries.push_back(std::move(entry));
        }
    }

    return entries;
}
