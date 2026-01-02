#pragma once

#include "board.hpp"
#include "move.hpp"
#include "ttable.hpp"

struct SearchResult {
    Move32 best_move;
    int score;
    int depth;
};

// Search for the best move with iterative deepening.
// Stops after time_limit_ms milliseconds.
SearchResult search(Board& board, TTable& tt, int time_limit_ms = 10000);
