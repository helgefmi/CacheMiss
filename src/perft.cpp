#include "perft.hpp"
#include "move.hpp"
#include <iostream>
#include <bit>

// PerftTable implementation
PerftTable::PerftTable(size_t mb) {
    // Calculate number of entries (power of 2)
    size_t bytes = mb * 1024 * 1024;
    size_t count = bytes / sizeof(PerftEntry);
    // Round down to power of 2
    count = size_t(1) << (63 - std::countl_zero(count));
    mask = count - 1;
    table.resize(count);
    // Zero-initialize (hash=0 means empty)
    std::fill(table.begin(), table.end(), PerftEntry{0, 0, 0});
}

bool PerftTable::probe(u64 hash, int depth, u64& nodes) const {
    const PerftEntry& entry = table[hash & mask];
    if (entry.hash == hash && entry.depth == depth) {
        nodes = entry.nodes;
        ++hits;
        return true;
    }
    ++misses;
    return false;
}

void PerftTable::store(u64 hash, int depth, u64 nodes) {
    PerftEntry& entry = table[hash & mask];
    entry.hash = hash;
    entry.nodes = nodes;
    entry.depth = static_cast<u8>(depth);
}

// Check if the side that just moved left their king in check (illegal move)
static bool is_illegal(const Board& board) {
    Color them = board.turn;  // Side to move next
    Color us = opposite(them);  // Side that just moved
    Bitboard king_bb = board.pieces[(int)us][(int)Piece::King];
    int king_sq = lsb_index(king_bb);
    return is_attacked(king_sq, them, board);
}

u64 perft(Board& board, int depth, PerftTable* tt) {
    if (depth == 0) return 1;

    u64 nodes = 0;
    if (tt->probe(board.hash, depth, nodes)) {
        return nodes;
    }

    auto moves = generate_moves(board);
    for (auto& move : moves) {
        make_move(board, move);
        if (!is_illegal(board)) {
            nodes += perft(board, depth - 1, tt);
        }
        unmake_move(board, move);
    }

    // Store in TT
    tt->store(board.hash, depth, nodes);

    return nodes;
}

void divide(Board& board, int depth, PerftTable* tt) {
    auto moves = generate_moves(board);
    u64 total = 0;

    for (auto& move : moves) {
        make_move(board, move);
        if (is_illegal(board)) {
            unmake_move(board, move);
            continue;
        }
        u64 nodes = (depth > 1) ? perft(board, depth - 1, tt) : 1;
        unmake_move(board, move);

        std::cout << move.to_string() << ": " << nodes << '\n';
        total += nodes;
    }

    std::cout << "\nTotal: " << total << '\n';
}
