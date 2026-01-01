#pragma once

#include "cachemiss.hpp"
#include "board.hpp"
#include <string>

// A move is packed into a 32 bit integer as follows:
// Bits 0-5:   From square (0-63)
// Bits 6-11:  To square (0-63)
// Bits 12-14: Promotion piece (0-5 for Pawn, Knight, Bishop, Rook, Queen, King; 7 for no promotion)
// Bits 15-17: Captured piece (0-5 for Pawn, Knight, Bishop, Rook, Queen, King; 7 for no capture)
// Bits 18-21: Previous castling rights (4 bits: bit0=wQ, bit1=wK, bit2=bQ, bit3=bK)
// Bits 22-25: Previous en passant file (4 bits, 0-7 = file, 8+ = none)
// Bit 26:     Is en passant capture (set by generate_moves)
// Bit 27:     Is castling move (set by generate_moves)
// Bits 28-31: Unused
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
    constexpr bool is_en_passant() const { return (data >> 26) & 1; }
    constexpr bool is_castling() const { return (data >> 27) & 1; }

    constexpr void set_en_passant() { data |= (1u << 26); }
    constexpr void set_castling() { data |= (1u << 27); }

    // Undo info stored by make_move, read by unmake_move
    constexpr u8 prev_castling() const { return (data >> 18) & 0xF; }
    constexpr u8 prev_ep_file() const { return (data >> 22) & 0xF; }
    constexpr void set_undo_info(u8 castling, u8 ep_file) {
        // Direct storage - no conversion needed
        data = (data & 0xFC03FFFF) | (castling << 18) | (ep_file << 22);
    }

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

struct MoveList {
    static constexpr int MAX_MOVES = 256;

    Move32 moves[MAX_MOVES];
    int size = 0;

    void add(Move32 move) { moves[size++] = move; }

    Move32* begin() { return moves; }
    Move32* end() { return moves + size; }
    const Move32* begin() const { return moves; }
    const Move32* end() const { return moves + size; }

    Move32& operator[](int i) { return moves[i]; }
    const Move32& operator[](int i) const { return moves[i]; }
};

template <Color turn> MoveList generate_moves(const Board& board);
MoveList generate_moves(const Board& board);

void make_move(Board& board, Move32& move);
void unmake_move(Board& board, const Move32& move);

bool is_attacked(int square, Color attacker, const Board& board);
