#pragma once

#include "cachemiss.hpp"
#include "board.hpp"
#include <vector>
#include <string>

// A move is packed into a 32 bit integer as follows:
// Bits 0-5:   From square (0-63)
// Bits 6-11:  To square (0-63)
// Bits 12-14: Promotion piece (0-5 for Pawn, Knight, Bishop, Rook, Queen, King; 7 for no promotion)
// Bits 15-17: Captured piece (0-5 for Pawn, Knight, Bishop, Rook, Queen, King; 7 for no capture)
// Bits 18+: Unused for now. Can be used for flags, and to make sure we can sort moves efficiently.
struct Move32 {
    u32 data;

    constexpr Move32() = default;
    constexpr Move32(u32 d) : data(d) {}
    constexpr Move32(int from, int to, Piece promotion = Piece::None, Piece captured = Piece::None)
        : data(
            (from & 0x3F) |
            ((to & 0x3F) << 6) |
            ((static_cast<u32>(promotion) & 0x7) << 12) |
            ((static_cast<u32>(captured) & 0x7) << 15)) {}

    constexpr int from() const { return data & 0x3F; }
    constexpr int to() const { return (data >> 6) & 0x3F; }
    constexpr Piece promotion() const { return static_cast<Piece>((data >> 12) & 0x7); }
    constexpr Piece captured() const { return static_cast<Piece>((data >> 15) & 0x7); }

    constexpr bool is_capture() const { return captured() != Piece::None; }
    constexpr bool is_promotion() const { return promotion() != Piece::None; }

    std::string to_string() const {
        std::string move_str;
        move_str += 'a' + (from() % 8);
        move_str += '1' + (from() / 8);
        move_str += 'a' + (to() % 8);
        move_str += '1' + (to() / 8);
        if (is_capture()) {
            move_str += 'x';
            move_str += piece_to_char(captured());
        }
        if (is_promotion()) {
            move_str += '=';
            move_str += piece_to_char(promotion());
        }
        return move_str;
    }
};

template <Color turn>
std::vector<Move32> generate_moves(const Board& board);
