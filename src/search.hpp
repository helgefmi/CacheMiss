#pragma once

#include "board.hpp"
#include "move.hpp"
#include "ttable.hpp"

constexpr int MAX_PLY = 64;

struct SearchResult {
    Move32 best_move;
    int score;
    int depth;
    Move32 pv[MAX_PLY];    // Principal variation line
    int pv_length = 0;      // Number of moves in PV
};

// Search for the best move with iterative deepening.
// Stops after time_limit_ms milliseconds.
SearchResult search(Board& board, TTable& tt, int time_limit_ms = 10000);
