#include <iostream>
#include <random>
#include <cstdint>
#include <vector>
#include <array>

using Bitboard = uint64_t;

// Random number generator for magic candidates
struct Random {
    std::mt19937_64 gen;
    std::uniform_int_distribution<uint64_t> dist;
    Random() : gen(std::random_device{}()), dist(0, UINT64_MAX) {}

    uint64_t sparse_rand() {
        // Generate numbers with fewer bits set (better for magics)
        return dist(gen) & dist(gen) & dist(gen);
    }
};

// Helper functions
int popcount(Bitboard bb) {
    return __builtin_popcountll(bb);
}

int lsb(Bitboard bb) {
    return __builtin_ctzll(bb);
}

Bitboard set_occupancy(int index, int bits_in_mask, Bitboard mask) {
    Bitboard occupancy = 0;
    for (int count = 0; count < bits_in_mask; count++) {
        int square = lsb(mask);
        mask &= mask - 1; // Clear the least significant bit
        if (index & (1 << count)) {
            occupancy |= (1ull << square);
        }
    }
    return occupancy;
}

// Generate rook attacks for a given square and occupancy
Bitboard rook_attacks_on_the_fly(int square, Bitboard occupancy) {
    Bitboard attacks = 0;
    int rank = square / 8;
    int file = square % 8;

    // North
    for (int r = rank + 1; r < 8; r++) {
        int sq = r * 8 + file;
        attacks |= (1ull << sq);
        if (occupancy & (1ull << sq)) break;
    }
    // South
    for (int r = rank - 1; r >= 0; r--) {
        int sq = r * 8 + file;
        attacks |= (1ull << sq);
        if (occupancy & (1ull << sq)) break;
    }
    // East
    for (int f = file + 1; f < 8; f++) {
        int sq = rank * 8 + f;
        attacks |= (1ull << sq);
        if (occupancy & (1ull << sq)) break;
    }
    // West
    for (int f = file - 1; f >= 0; f--) {
        int sq = rank * 8 + f;
        attacks |= (1ull << sq);
        if (occupancy & (1ull << sq)) break;
    }

    return attacks;
}

// Generate bishop attacks for a given square and occupancy
Bitboard bishop_attacks_on_the_fly(int square, Bitboard occupancy) {
    Bitboard attacks = 0;
    int rank = square / 8;
    int file = square % 8;

    // Northeast
    for (int r = rank + 1, f = file + 1; r < 8 && f < 8; r++, f++) {
        int sq = r * 8 + f;
        attacks |= (1ull << sq);
        if (occupancy & (1ull << sq)) break;
    }
    // Northwest
    for (int r = rank + 1, f = file - 1; r < 8 && f >= 0; r++, f--) {
        int sq = r * 8 + f;
        attacks |= (1ull << sq);
        if (occupancy & (1ull << sq)) break;
    }
    // Southeast
    for (int r = rank - 1, f = file + 1; r >= 0 && f < 8; r--, f++) {
        int sq = r * 8 + f;
        attacks |= (1ull << sq);
        if (occupancy & (1ull << sq)) break;
    }
    // Southwest
    for (int r = rank - 1, f = file - 1; r >= 0 && f >= 0; r--, f--) {
        int sq = r * 8 + f;
        attacks |= (1ull << sq);
        if (occupancy & (1ull << sq)) break;
    }

    return attacks;
}

// Generate occupancy mask (relevant squares) for rook
Bitboard rook_mask(int square) {
    Bitboard mask = 0;
    int rank = square / 8;
    int file = square % 8;

    // North (exclude last rank)
    for (int r = rank + 1; r < 7; r++) {
        mask |= (1ull << (r * 8 + file));
    }
    // South (exclude first rank)
    for (int r = rank - 1; r > 0; r--) {
        mask |= (1ull << (r * 8 + file));
    }
    // East (exclude last file)
    for (int f = file + 1; f < 7; f++) {
        mask |= (1ull << (rank * 8 + f));
    }
    // West (exclude first file)
    for (int f = file - 1; f > 0; f--) {
        mask |= (1ull << (rank * 8 + f));
    }

    return mask;
}

// Generate occupancy mask (relevant squares) for bishop
Bitboard bishop_mask(int square) {
    Bitboard mask = 0;
    int rank = square / 8;
    int file = square % 8;

    // Northeast (exclude edges)
    for (int r = rank + 1, f = file + 1; r < 7 && f < 7; r++, f++) {
        mask |= (1ull << (r * 8 + f));
    }
    // Northwest (exclude edges)
    for (int r = rank + 1, f = file - 1; r < 7 && f > 0; r++, f--) {
        mask |= (1ull << (r * 8 + f));
    }
    // Southeast (exclude edges)
    for (int r = rank - 1, f = file + 1; r > 0 && f < 7; r--, f++) {
        mask |= (1ull << (r * 8 + f));
    }
    // Southwest (exclude edges)
    for (int r = rank - 1, f = file - 1; r > 0 && f > 0; r--, f--) {
        mask |= (1ull << (r * 8 + f));
    }

    return mask;
}

// Magic index calculation
inline int magic_index(Bitboard occupancy, Bitboard magic, int shift) {
    return (occupancy * magic) >> shift;
}

// Find magic number for a square
struct MagicEntry {
    Bitboard mask;
    Bitboard magic;
    int shift;
    std::vector<Bitboard> attacks;
};

MagicEntry find_magic(int square, bool is_rook, Random& rng) {
    MagicEntry entry;
    entry.mask = is_rook ? rook_mask(square) : bishop_mask(square);

    int relevant_bits = popcount(entry.mask);
    int table_size = 1 << relevant_bits;
    entry.shift = 64 - relevant_bits;

    // Generate all occupancy variations
    std::vector<Bitboard> occupancies(table_size);
    std::vector<Bitboard> attacks(table_size);

    for (int i = 0; i < table_size; i++) {
        occupancies[i] = set_occupancy(i, relevant_bits, entry.mask);
        attacks[i] = is_rook ?
            rook_attacks_on_the_fly(square, occupancies[i]) :
            bishop_attacks_on_the_fly(square, occupancies[i]);
    }

    // Try random magics until one works
    std::vector<Bitboard> used(table_size);
    for (int attempt = 0; attempt < 100000000; attempt++) {
        Bitboard magic = rng.sparse_rand();

        // Quick test: magic must map to enough bits
        if (popcount((entry.mask * magic) & 0xFF00000000000000ull) < 6) continue;

        std::fill(used.begin(), used.end(), 0);
        bool fail = false;

        for (int i = 0; i < table_size; i++) {
            int idx = magic_index(occupancies[i], magic, entry.shift);

            if (used[idx] == 0) {
                used[idx] = attacks[i];
            } else if (used[idx] != attacks[i]) {
                fail = true;
                break;
            }
        }

        if (!fail) {
            entry.magic = magic;
            entry.attacks = used;
            std::cerr << (is_rook ? "Rook" : "Bishop") << " square " << square
                      << " magic found after " << attempt + 1 << " attempts\n";
            return entry;
        }
    }

    std::cerr << "Failed to find magic for square " << square << "\n";
    exit(1);
}

void print_cpp_array(const std::string& name, const std::array<Bitboard, 64>& data) {
    std::cout << "constexpr std::array<Bitboard, 64> " << name << " = {{\n";
    for (int i = 0; i < 64; i++) {
        std::cout << "    0x" << std::hex << data[i] << "ull";
        if (i < 63) std::cout << ",";
        std::cout << "\n";
    }
    std::cout << "}};\n\n";
}

void print_int_array(const std::string& name, const std::array<int, 64>& data) {
    std::cout << "constexpr std::array<int, 64> " << name << " = {{\n";
    for (int i = 0; i < 64; i++) {
        std::cout << "    " << std::dec << data[i];
        if (i < 63) std::cout << ",";
        std::cout << "\n";
    }
    std::cout << "}};\n\n";
}

void print_attack_table(const std::string& name, const std::vector<std::vector<Bitboard>>& all_attacks) {
    int total_size = 0;
    for (const auto& attacks : all_attacks) {
        total_size += attacks.size();
    }

    std::cout << "constexpr std::array<Bitboard, " << total_size << "> " << name << " = {{\n";

    bool first = true;
    for (const auto& attacks : all_attacks) {
        for (Bitboard attack : attacks) {
            if (!first) std::cout << ",\n";
            std::cout << "    0x" << std::hex << attack << "ull";
            first = false;
        }
    }
    std::cout << "\n}};\n\n";
}

void print_offset_array(const std::string& name, const std::vector<int>& offsets) {
    std::cout << "constexpr std::array<int, 64> " << name << " = {{\n";
    for (int i = 0; i < 64; i++) {
        std::cout << "    " << std::dec << offsets[i];
        if (i < 63) std::cout << ",";
        std::cout << "\n";
    }
    std::cout << "}};\n\n";
}

int main() {
    Random rng;

    std::array<Bitboard, 64> rook_magics;
    std::array<Bitboard, 64> rook_masks;
    std::array<int, 64> rook_shifts;
    std::vector<std::vector<Bitboard>> rook_attacks_all;
    std::vector<int> rook_offsets;

    std::array<Bitboard, 64> bishop_magics;
    std::array<Bitboard, 64> bishop_masks;
    std::array<int, 64> bishop_shifts;
    std::vector<std::vector<Bitboard>> bishop_attacks_all;
    std::vector<int> bishop_offsets;

    std::cerr << "Generating rook magics...\n";
    int rook_offset = 0;
    for (int sq = 0; sq < 64; sq++) {
        MagicEntry entry = find_magic(sq, true, rng);
        rook_magics[sq] = entry.magic;
        rook_masks[sq] = entry.mask;
        rook_shifts[sq] = entry.shift;
        rook_attacks_all.push_back(entry.attacks);
        rook_offsets.push_back(rook_offset);
        rook_offset += entry.attacks.size();
    }

    std::cerr << "\nGenerating bishop magics...\n";
    int bishop_offset = 0;
    for (int sq = 0; sq < 64; sq++) {
        MagicEntry entry = find_magic(sq, false, rng);
        bishop_magics[sq] = entry.magic;
        bishop_masks[sq] = entry.mask;
        bishop_shifts[sq] = entry.shift;
        bishop_attacks_all.push_back(entry.attacks);
        bishop_offsets.push_back(bishop_offset);
        bishop_offset += entry.attacks.size();
    }

    std::cout << "// Generated Magic Bitboards\n";
    std::cout << "// Total rook table size: " << std::dec << rook_offset << " entries\n";
    std::cout << "// Total bishop table size: " << std::dec << bishop_offset << " entries\n";
    std::cout << "\n#pragma once\n#include <array>\n#include <cstdint>\n\n";
    std::cout << "using Bitboard = uint64_t;\n\n";

    print_cpp_array("ROOK_MAGICS", rook_magics);
    print_cpp_array("ROOK_MASKS", rook_masks);
    print_offset_array("ROOK_OFFSETS", rook_offsets);
    print_int_array("ROOK_SHIFTS", rook_shifts);
    print_attack_table("ROOK_ATTACKS", rook_attacks_all);

    print_cpp_array("BISHOP_MAGICS", bishop_magics);
    print_cpp_array("BISHOP_MASKS", bishop_masks);
    print_offset_array("BISHOP_OFFSETS", bishop_offsets);
    print_int_array("BISHOP_SHIFTS", bishop_shifts);
    print_attack_table("BISHOP_ATTACKS", bishop_attacks_all);

    return 0;
}
