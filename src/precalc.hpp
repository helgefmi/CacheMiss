#pragma once
#include "cachemiss.hpp"
#include <array>

constexpr std::array<std::array<Bitboard, 64>, 2> PAWN_ATTACKS = []{
    std::array<std::array<Bitboard, 64>, 2> attacks = {};
    for (int sq = 0; sq < 64; ++sq) {
        Bitboard white_bb = 0;
        Bitboard black_bb = 0;
        int rank = sq / 8;
        int file = sq % 8;

        // White pawn attacks
        if (rank < 7) {
            if (file > 0) white_bb |= (1ull << ((rank + 1) * 8 + (file - 1)));
            if (file < 7) white_bb |= (1ull << ((rank + 1) * 8 + (file + 1)));
        }

        // Black pawn attacks
        if (rank > 0) {
            if (file > 0) black_bb |= (1ull << ((rank - 1) * 8 + (file - 1)));
            if (file < 7) black_bb |= (1ull << ((rank - 1) * 8 + (file + 1)));
        }

        attacks[0][sq] = white_bb;
        attacks[1][sq] = black_bb;
    }
    return attacks;
}();

constexpr std::array<std::array<Bitboard, 64>, 2> PAWN_MOVES_ONE = []{
    std::array<std::array<Bitboard, 64>, 2> moves = {};
    for (int sq = 0; sq < 64; ++sq) {
        Bitboard white_bb = 0;
        Bitboard black_bb = 0;
        int rank = sq / 8;
        int file = sq % 8;

        // White pawn one square move
        if (rank < 7) {
            white_bb |= (1ull << ((rank + 1) * 8 + file));
        }

        // Black pawn one square move
        if (rank > 0) {
            black_bb |= (1ull << ((rank - 1) * 8 + file));
        }

        moves[0][sq] = white_bb;
        moves[1][sq] = black_bb;
    }
    return moves;
}();

constexpr std::array<std::array<Bitboard, 64>, 2> PAWN_MOVES_TWO = []{
    std::array<std::array<Bitboard, 64>, 2> moves = {};
    for (int sq = 0; sq < 64; ++sq) {
        Bitboard white_bb = 0;
        Bitboard black_bb = 0;
        int rank = sq / 8;
        int file = sq % 8;

        // White pawn two squares move
        if (rank == 1) {
            white_bb |= (1ull << ((rank + 2) * 8 + file));
        }

        // Black pawn two squares move
        if (rank == 6) {
            black_bb |= (1ull << ((rank - 2) * 8 + file));
        }

        moves[0][sq] = white_bb;
        moves[1][sq] = black_bb;
    }
    return moves;
}();

constexpr std::array<Bitboard, 64> KNIGHT_MOVES = []{
    std::array<Bitboard, 64> moves = {};
    for (int sq = 0; sq < 64; ++sq) {
        Bitboard bb = 0;
        int rank = sq / 8;
        int file = sq % 8;

        // All possible knight moves
        int knight_deltas[8][2] = {
            {2, 1}, {2, -1}, {-2, 1}, {-2, -1},
            {1, 2}, {1, -2}, {-1, 2}, {-1, -2}
        };

        for (auto& delta : knight_deltas) {
            int new_rank = rank + delta[0];
            int new_file = file + delta[1];
            if (new_rank >= 0 && new_rank < 8 && new_file >= 0 && new_file < 8) {
                bb |= (1ull << (new_rank * 8 + new_file));
            }
        }
        moves[sq] = bb;
    }
    return moves;
}();

constexpr std::array<Bitboard, 64> KING_MOVES = []{
    std::array<Bitboard, 64> moves = {};
    for (int sq = 0; sq < 64; ++sq) {
        Bitboard bb = 0;
        int rank = sq / 8;
        int file = sq % 8;

        // All possible king moves
        int king_deltas[8][2] = {
            {1, 0}, {-1, 0}, {0, 1}, {0, -1},
            {1, 1}, {1, -1}, {-1, 1}, {-1, -1}
        };

        for (auto& delta : king_deltas) {
            int new_rank = rank + delta[0];
            int new_file = file + delta[1];
            if (new_rank >= 0 && new_rank < 8 && new_file >= 0 && new_file < 8) {
                bb |= (1ull << (new_rank * 8 + new_file));
            }
        }
        moves[sq] = bb;
    }
    return moves;
}();

// Adjacent files for each file (0-7)
constexpr std::array<Bitboard, 8> ADJACENT_FILES = []{
    std::array<Bitboard, 8> adj = {};
    constexpr Bitboard FILE_A = 0x0101010101010101ULL;
    for (int f = 0; f < 8; ++f) {
        Bitboard file_mask = FILE_A << f;
        if (f > 0) adj[f] |= file_mask >> 1;
        if (f < 7) adj[f] |= file_mask << 1;
    }
    return adj;
}();

// PASSED_PAWN_MASK[color][square] - squares where enemy pawns would block a passed pawn
constexpr std::array<std::array<Bitboard, 64>, 2> PASSED_PAWN_MASK = []{
    std::array<std::array<Bitboard, 64>, 2> masks = {};
    constexpr Bitboard FILE_A = 0x0101010101010101ULL;

    for (int sq = 0; sq < 64; ++sq) {
        int rank = sq / 8;
        int file = sq % 8;
        Bitboard file_mask = FILE_A << file;
        Bitboard adjacent = (file > 0 ? file_mask >> 1 : 0) |
                           (file < 7 ? file_mask << 1 : 0);
        Bitboard front_files = file_mask | adjacent;

        // White: all ranks above
        Bitboard white_mask = 0;
        for (int r = rank + 1; r < 8; ++r)
            white_mask |= (front_files & (0xFFULL << (r * 8)));
        masks[0][sq] = white_mask;

        // Black: all ranks below
        Bitboard black_mask = 0;
        for (int r = rank - 1; r >= 0; --r)
            black_mask |= (front_files & (0xFFULL << (r * 8)));
        masks[1][sq] = black_mask;
    }
    return masks;
}();
