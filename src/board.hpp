#pragma once
#include "cachemiss.hpp"
#include <array>
#include <optional>
#include <string_view>

class Board {
    private:
    Color turn;
    std::array<std::array<Bitboard, 6>, 2> pieces;  // pieces[Color][Piece]
    std::array<Bitboard, 2> occupied;               // occupied[Color]
    Bitboard all_occupied;
    Bitboard en_passant;                            // The bit below the pawn that just moved two squares
    Bitboard castling;                              // Each corner has the bit set if castling is allowed that way
    std::array<std::optional<Piece>, 64> pieces_on_square;

    public:
    Board() : Board("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1") {}
    Board(std::string_view fen);
    void print() const;
};
