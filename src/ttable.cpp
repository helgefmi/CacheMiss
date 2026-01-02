#include "ttable.hpp"
#include <bit>

TTable::TTable(size_t mb) {
    size_t bytes = mb * 1024 * 1024;
    size_t count = bytes / sizeof(TTEntry);
    // Round down to power of 2
    count = size_t(1) << (63 - std::countl_zero(count));
    mask = count - 1;
    table.resize(count);
    clear();
}

bool TTable::probe(u64 hash, int depth, int alpha, int beta, int& score, Move32& best_move) const {
    const TTEntry& entry = table[hash & mask];

    if (entry.hash != hash) {
        return false;
    }

    // Always return best move for move ordering
    best_move = entry.best_move;

    // Only use score if depth is sufficient
    if (entry.depth < depth) {
        return false;
    }

    score = entry.score;

    if (entry.flag == TT_EXACT) {
        return true;
    }
    if (entry.flag == TT_LOWER && score >= beta) {
        return true;
    }
    if (entry.flag == TT_UPPER && score <= alpha) {
        return true;
    }

    return false;
}

void TTable::store(u64 hash, int depth, int score, TTFlag flag, Move32 best_move) {
    TTEntry& entry = table[hash & mask];

    // Always replace (simple scheme, can improve later)
    entry.hash = hash;
    entry.score = static_cast<s16>(score);
    entry.depth = static_cast<u8>(depth);
    entry.flag = flag;
    entry.best_move = best_move;
}

void TTable::clear() {
    std::fill(table.begin(), table.end(), TTEntry{0, 0, 0, 0, Move32(0)});
}
