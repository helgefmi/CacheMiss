#pragma once
#include <cstddef>

// Run all engine tests (FEN, make/unmake, hash, castling, en passant, promotion,
// invariants, halfmove, perft, eval, checkmate/stalemate, TT, draw detection, PV)
// Returns number of failures
int run_draw_tests(int time_limit_ms = 500, size_t mem_mb = 64);
