#pragma once
#include "cachemiss.hpp"
#include <array>
#include <string_view>

struct Board {
    Color turn;
    std::array<std::array<Bitboard, 6>, 2> pieces;  // pieces[Color][Piece]
    std::array<Bitboard, 2> occupied;               // occupied[Color]
    Bitboard all_occupied;
    u8 ep_file;                                     // En passant target file (0-7), 8 = none
    u8 castling;                                    // Castling rights: bit0=wQ, bit1=wK, bit2=bQ, bit3=bK
    std::array<Piece, 64> pieces_on_square;
    u64 hash;  // Zobrist hash

    Board() : Board("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1") {}
    Board(std::string_view fen);
    void print() const;
};
