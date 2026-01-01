#pragma once

#include "cachemiss.hpp"
#include "board.hpp"
#include <string>

// A move is packed into a 32 bit integer as follows:
// Bits 0-5:   From square (0-63)
// Bits 6-11:  To square (0-63)
// Bits 12-14: Promotion piece (0-5 for Pawn, Knight, Bishop, Rook, Queen, King; 7 for no promotion)
// Bits 15-17: Captured piece (0-5 for Pawn, Knight, Bishop, Rook, Queen, King; 7 for no capture)
// Bits 18-21: Previous castling rights (4 bits, stored by make_move for unmake)
// Bits 22-27: Previous en passant square (6 bits, 63 = none, stored by make_move)
// Bit 28:     Is en passant capture (set by generate_moves)
// Bit 29:     Is castling move (set by generate_moves)
// Bits 30-31: Unused
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
    constexpr bool is_en_passant() const { return (data >> 28) & 1; }
    constexpr bool is_castling() const { return (data >> 29) & 1; }

    constexpr void set_en_passant() { data |= (1u << 28); }
    constexpr void set_castling() { data |= (1u << 29); }

    // Undo info stored by make_move, read by unmake_move
    constexpr int prev_castling() const { return (data >> 18) & 0xF; }
    constexpr int prev_ep_square() const { return (data >> 22) & 0x3F; }
    constexpr void set_undo_info(Bitboard castling, Bitboard ep) {
        // Convert castling bitboard to 4-bit value (a1=bit0, h1=bit1, a8=bit2, h8=bit3)
        int castling_bits = ((castling & 1) ? 1 : 0)           // a1
                          | ((castling & (1ULL << 7)) ? 2 : 0)  // h1
                          | ((castling & (1ULL << 56)) ? 4 : 0) // a8
                          | ((castling & (1ULL << 63)) ? 8 : 0); // h8
        int ep_sq = ep ? lsb_index(ep) : 63;
        data = (data & 0x3003FFFF) | (castling_bits << 18) | (ep_sq << 22);
    }
    constexpr Bitboard prev_castling_bb() const {
        int bits = prev_castling();
        return ((bits & 1) ? 1ULL : 0)
             | ((bits & 2) ? (1ULL << 7) : 0)
             | ((bits & 4) ? (1ULL << 56) : 0)
             | ((bits & 8) ? (1ULL << 63) : 0);
    }
    constexpr Bitboard prev_ep_bb() const {
        int sq = prev_ep_square();
        return (sq == 63) ? 0 : (1ULL << sq);
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
