#pragma once

#include "board.hpp"
#include "pawn_cache.hpp"

// Global pawn structure cache (1 MB default)
extern PawnCache g_pawn_cache;

// Evaluate the position from the side-to-move's perspective
int evaluate(const Board& board);
