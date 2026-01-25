#pragma once

#include "cachemiss.hpp"
#include "move.hpp"
#include <vector>

enum TTFlag : u8 { TT_EXACT = 0, TT_LOWER = 1, TT_UPPER = 2 };

// Compact TTEntry: 16 bytes for optimal cache line packing (4 entries per 64-byte line)
// Hash verification uses upper 32 bits of hash; lower bits are already the table index.
// Combined with ~25 index bits, this gives ~57 bits of effective hash coverage.
struct TTEntry {
    u32 hash_verify;  // Upper 32 bits of full hash for collision detection
    s16 score;
    u8 depth;
    u8 flags;         // Lower 2 bits: TTFlag, upper 6 bits: generation (wraps at 64)
    Move32 best_move;
    u32 _padding;     // Pad to 16 bytes for cache line alignment
};
static_assert(sizeof(TTEntry) == 16, "TTEntry must be 16 bytes");

struct TTStats {
    u64 hits = 0;       // Probe found matching hash
    u64 misses = 0;     // Probe found no match
    u64 stores = 0;     // Total store calls
    u64 overwrites = 0; // Store replaced existing entry
};

class TTable {
    std::vector<TTEntry> table;
    size_t mask;
    u8 current_generation = 0;
    mutable TTStats stats;

public:
    explicit TTable(size_t mb);

    // Call before each new search to age existing entries
    void new_search() { current_generation++; }

    // Prefetch entry for given hash into cache.
    // Call early, then do other work, then call probe().
    void prefetch(u64 hash) const {
#ifdef __GNUC__
        __builtin_prefetch(&table[hash & mask], 0, 0);
#endif
    }

    // Probe the TT. Returns true if entry can be used for cutoff.
    // Always sets best_move if entry exists (for move ordering).
    // ply is needed to adjust mate scores to be ply-independent.
    bool probe(u64 hash, int depth, int ply, int alpha, int beta, int& score, Move32& best_move);

    void store(u64 hash, int depth, int ply, int score, TTFlag flag, Move32 best_move);
    void clear();
    void reset_stats();

    const TTStats& get_stats() const { return stats; }
    size_t size() const { return table.size(); }
    size_t count_occupied() const;
    double occupancy_percent() const;
};
