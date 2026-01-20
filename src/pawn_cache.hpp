#pragma once
#include "cachemiss.hpp"
#include <vector>

struct PawnCacheEntry {
    u64 key;
    s16 mg_score;
    s16 eg_score;
};

class PawnCache {
public:
    explicit PawnCache(size_t mb = 1);
    bool probe(u64 key, int& mg, int& eg) const;
    void store(u64 key, int mg, int eg);
    void clear();

private:
    std::vector<PawnCacheEntry> table;
    size_t mask;
};
