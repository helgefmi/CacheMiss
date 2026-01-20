#include "pawn_cache.hpp"

PawnCache::PawnCache(size_t mb) {
    // Each entry is 16 bytes (8 + 2 + 2 + padding)
    size_t entry_count = (mb * 1024 * 1024) / sizeof(PawnCacheEntry);

    // Round down to power of 2 for efficient masking
    size_t power_of_2 = 1;
    while (power_of_2 * 2 <= entry_count) {
        power_of_2 *= 2;
    }

    table.resize(power_of_2);
    mask = power_of_2 - 1;
    clear();
}

bool PawnCache::probe(u64 key, int& mg, int& eg) const {
    const PawnCacheEntry& entry = table[key & mask];
    if (entry.key == key) {
        mg = entry.mg_score;
        eg = entry.eg_score;
        return true;
    }
    return false;
}

void PawnCache::store(u64 key, int mg, int eg) {
    PawnCacheEntry& entry = table[key & mask];
    entry.key = key;
    entry.mg_score = static_cast<s16>(mg);
    entry.eg_score = static_cast<s16>(eg);
}

void PawnCache::clear() {
    for (auto& entry : table) {
        entry.key = 0;
        entry.mg_score = 0;
        entry.eg_score = 0;
    }
}
