#pragma once

#include "board.hpp"
#include "cachemiss.hpp"

u64 perft(Board& board, int depth);
void divide(Board& board, int depth);
