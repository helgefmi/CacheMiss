#include "perft.hpp"
#include "move.hpp"
#include <iostream>

// Check if the side that just moved left their king in check (illegal move)
static bool is_illegal(const Board& board) {
    Color them = board.turn;  // Side to move next
    Color us = opposite(them);  // Side that just moved
    Bitboard king_bb = board.pieces[(int)us][(int)Piece::King];
    int king_sq = lsb_index(king_bb);
    return is_attacked(king_sq, them, board);
}

u64 perft(Board& board, int depth) {
    if (depth == 0) return 1;

    auto moves = generate_moves(board);
    u64 nodes = 0;

    for (auto& move : moves) {
        make_move(board, move);
        if (!is_illegal(board)) {
            nodes += perft(board, depth - 1);
        }
        unmake_move(board, move);
    }

    return nodes;
}

void divide(Board& board, int depth) {
    auto moves = generate_moves(board);
    u64 total = 0;

    for (auto& move : moves) {
        make_move(board, move);
        if (is_illegal(board)) {
            unmake_move(board, move);
            continue;
        }
        u64 nodes = (depth > 1) ? perft(board, depth - 1) : 1;
        unmake_move(board, move);

        std::cout << move.to_string() << ": " << nodes << '\n';
        total += nodes;
    }

    std::cout << "\nTotal: " << total << '\n';
}
