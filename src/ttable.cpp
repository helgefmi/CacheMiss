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

bool TTable::probe(u64 hash, int depth, int alpha, int beta, int& score, Move32& best_move) {
    const TTEntry& entry = table[hash & mask];

    if (entry.hash != hash) {
        stats.misses++;
        return false;
    }

    stats.hits++;

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

    stats.stores++;

    // Age-aware replacement: consider both depth and staleness
    // An old entry needs to be significantly deeper to justify keeping it
    if (entry.hash != 0) {
        // Calculate age difference (handles wraparound via unsigned arithmetic)
        int age_diff = static_cast<u8>(current_generation - entry.generation);

        // Replace if: same position, OR new entry is recent enough relative to depth
        // Formula: replace if new_depth + age_bonus >= old_depth
        // Each generation of age gives the new entry a +2 depth bonus
        bool same_position = (entry.hash == hash);
        bool should_replace = same_position ||
                              (depth + age_diff * 2 >= entry.depth);

        if (!should_replace) {
            return;  // Keep the existing deeper, recent entry
        }
        stats.overwrites++;
    }

    entry.hash = hash;
    entry.score = static_cast<s16>(score);
    entry.depth = static_cast<u8>(depth);
    entry.flag = flag;
    entry.generation = current_generation;
    entry.best_move = best_move;
}

void TTable::clear() {
    std::fill(table.begin(), table.end(), TTEntry{0, 0, 0, 0, 0, Move32(0)});
    current_generation = 0;
    reset_stats();
}

void TTable::reset_stats() {
    stats = TTStats{};
}

size_t TTable::count_occupied() const {
    size_t count = 0;
    for (const auto& entry : table) {
        if (entry.hash != 0) count++;
    }
    return count;
}

double TTable::occupancy_percent() const {
    return table.empty() ? 0.0 : (100.0 * count_occupied() / table.size());
}
