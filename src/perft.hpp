#pragma once

#include "board.hpp"
#include "cachemiss.hpp"
#include <vector>

struct PerftEntry {
    u64 hash;
    u64 nodes;
    u8 depth;
};

class PerftTable {
    std::vector<PerftEntry> table;
    size_t mask;
    mutable u64 hits = 0;
    mutable u64 misses = 0;

public:
    explicit PerftTable(size_t mb);

    bool probe(u64 hash, int depth, u64& nodes) const;
    void store(u64 hash, int depth, u64 nodes);

    u64 get_hits() const { return hits; }
    u64 get_misses() const { return misses; }
};

u64 perft(Board& board, int depth, PerftTable* tt = nullptr);
void divide(Board& board, int depth, PerftTable* tt = nullptr);
