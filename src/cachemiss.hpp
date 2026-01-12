#pragma once

#include <bit>
#include <cstdint>
#include <optional>
#include <utility>
#include <iostream>

//
// Types
//

typedef uint64_t u64;
typedef int64_t  s64;
typedef uint32_t u32;
typedef int32_t  s32;
typedef uint16_t u16;
typedef int16_t  s16;
typedef uint8_t  u8;
typedef int8_t   s8;

//
// Bitboard operations
//

using Bitboard = u64;

inline int popcount(Bitboard bb) {
    return std::popcount(bb);
}

inline int lsb_index(Bitboard bb) {
    return std::countr_zero(bb);
}

inline int msb_index(Bitboard bb) {
    return 63 - std::countl_zero(bb);
}

inline Bitboard lsb(Bitboard bb) {
    return bb & -bb;
}

inline Bitboard msb(Bitboard bb) {
    if (bb == 0) return 0;
    return Bitboard(1) << msb_index(bb);
}

static void print_bitboard(Bitboard bb) {
    for (int rank = 7; rank >= 0; --rank) {
        for (int file = 0; file < 8; ++file) {
            int sq = rank * 8 + file;
            std::cout << ((bb & (Bitboard(1) << sq)) ? '1' : '.') << ' ';
        }
        std::cout << '\n';
    }
    std::cout << '\n';
}

//
// Color
//

enum struct Color : u8 { White = 0, Black = 1 };

constexpr Color opposite(Color c) {
    return (c == Color::White) ? Color::Black : Color::White;
}

constexpr char color_to_char(Color c) {
    return (c == Color::White) ? 'W' : 'B';
}

//
// Piece
//

enum struct Piece : u8 { Pawn = 0, Knight = 1, Bishop = 2, Rook = 3, Queen = 4, King = 5, None = 7 };

constexpr char piece_to_char(Piece p) {
    switch (p) {
        case Piece::Pawn:   return 'P';
        case Piece::Knight: return 'N';
        case Piece::Bishop: return 'B';
        case Piece::Rook:   return 'R';
        case Piece::Queen:  return 'Q';
        case Piece::King:   return 'K';
        default:            return '?';
    }
}

// Phase values for tapered evaluation (Knight=1, Bishop=1, Rook=2, Queen=4)
// Max phase = 24 (2*1 + 2*1 + 2*2 + 2*4) * 2 = 24
constexpr int PHASE_VALUES[] = {
    0,  // Pawn
    1,  // Knight
    1,  // Bishop
    2,  // Rook
    4,  // Queen
    0,  // King
    0,  // (unused)
    0   // None
};

//
// Square utilities
//

constexpr Bitboard square_bb(int sq) {
    return Bitboard(1) << sq;
}

constexpr int square_from_coords(int file, int rank) {
    return rank * 8 + file;
}

constexpr std::optional<std::pair<Color, Piece>> char_to_piece(char c) {
    switch (c) {
        case 'P': return std::make_pair(Color::White, Piece::Pawn);
        case 'N': return std::make_pair(Color::White, Piece::Knight);
        case 'B': return std::make_pair(Color::White, Piece::Bishop);
        case 'R': return std::make_pair(Color::White, Piece::Rook);
        case 'Q': return std::make_pair(Color::White, Piece::Queen);
        case 'K': return std::make_pair(Color::White, Piece::King);
        case 'p': return std::make_pair(Color::Black, Piece::Pawn);
        case 'n': return std::make_pair(Color::Black, Piece::Knight);
        case 'b': return std::make_pair(Color::Black, Piece::Bishop);
        case 'r': return std::make_pair(Color::Black, Piece::Rook);
        case 'q': return std::make_pair(Color::Black, Piece::Queen);
        case 'k': return std::make_pair(Color::Black, Piece::King);
        default:  return std::nullopt;
    }
}
