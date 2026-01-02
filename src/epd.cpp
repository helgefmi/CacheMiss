#include "epd.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>

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

std::vector<WACEntry> parse_wac_file(const std::string& filename) {
    std::vector<WACEntry> entries;
    std::ifstream file(filename);

    if (!file.is_open()) {
        return entries;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;

        WACEntry entry;

        // Find "bm" to separate FEN from best moves
        size_t bm_pos = line.find(" bm ");
        if (bm_pos == std::string::npos) continue;

        entry.fen = line.substr(0, bm_pos);

        // Find the semicolon after best moves
        size_t bm_end = line.find(';', bm_pos);
        if (bm_end == std::string::npos) continue;

        // Parse best moves (between "bm " and ";")
        std::string moves_str = line.substr(bm_pos + 4, bm_end - (bm_pos + 4));
        std::istringstream moves_iss(moves_str);
        std::string move;
        while (moves_iss >> move) {
            entry.best_moves.push_back(move);
        }

        // Find and parse id
        size_t id_pos = line.find("id \"", bm_end);
        if (id_pos != std::string::npos) {
            size_t id_start = id_pos + 4;
            size_t id_end = line.find('"', id_start);
            if (id_end != std::string::npos) {
                entry.id = line.substr(id_start, id_end - id_start);
            }
        }

        if (!entry.fen.empty() && !entry.best_moves.empty()) {
            entries.push_back(std::move(entry));
        }
    }

    return entries;
}
