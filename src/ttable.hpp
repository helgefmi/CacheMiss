#pragma once

#include "cachemiss.hpp"
#include "move.hpp"
#include <vector>

enum TTFlag : u8 { TT_EXACT = 0, TT_LOWER = 1, TT_UPPER = 2 };

struct TTEntry {
    u64 hash;
    s16 score;
    u8 depth;
    u8 flag;
    Move32 best_move;
};

struct TTStats {
    u64 hits = 0;       // Probe found matching hash
    u64 misses = 0;     // Probe found no match
    u64 stores = 0;     // Total store calls
    u64 overwrites = 0; // Store replaced existing entry
};

class TTable {
    std::vector<TTEntry> table;
    size_t mask;
    mutable TTStats stats;

public:
    explicit TTable(size_t mb);

    // Probe the TT. Returns true if entry can be used for cutoff.
    // Always sets best_move if entry exists (for move ordering).
    bool probe(u64 hash, int depth, int alpha, int beta, int& score, Move32& best_move);

    void store(u64 hash, int depth, int score, TTFlag flag, Move32 best_move);
    void clear();
    void reset_stats();

    const TTStats& get_stats() const { return stats; }
    size_t size() const { return table.size(); }
    size_t count_occupied() const;
    double occupancy_percent() const;
};
