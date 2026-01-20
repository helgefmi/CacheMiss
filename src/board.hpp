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
    u8 halfmove_clock;                              // Halfmove clock for 50-move rule (reset on pawn move/capture)
    std::array<Piece, 64> pieces_on_square;
    std::array<int, 2> king_sq;                     // King square for each color
    u64 hash;      // Zobrist hash
    u64 pawn_key;  // Zobrist hash of pawn positions only (for pawn structure cache)
    int phase;                                      // Game phase (0=endgame, 24=opening) for tapered eval
    std::array<u64, 1024> hash_stack;               // Stack for hash restoration in unmake
    std::array<u64, 1024> pawn_key_stack;           // Stack for pawn_key restoration in unmake
    std::array<u8, 1024> halfmove_stack;            // Stack for halfmove_clock restoration
    int hash_sp = 0;                                // Stack pointer

    Board() : Board("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1") {}
    Board(std::string_view fen);
    void print() const;
    std::string to_fen() const;
};
