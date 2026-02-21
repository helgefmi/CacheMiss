#pragma once

#include "board.hpp"
#include <cstddef>
#include <string>
#include <vector>

// Run the UCI protocol loop
// hash_mb: size of hash table in megabytes
void uci_loop(size_t hash_mb = 512);

// =============================================================================
// Testable UCI components
// =============================================================================

// Result of parsing "go" command
struct GoParams {
    int time_ms;           // Time to pass to search (infinite if pondering)
    int normal_time_ms;    // Time we'd use for a normal search (for ponderhit)
    int depth_limit;       // Max depth to search (0 = unlimited)
    bool is_ponder;
};

// Parse "go" command and return time parameters
// Exposed for testing
GoParams parse_go_command(const std::string& line, const Board& board, int moves_played, int move_overhead_ms);

// Parse "position" command and update board state
// Exposed for testing
void parse_position_command(const std::string& line, Board& board, std::vector<u64>& game_hashes);
