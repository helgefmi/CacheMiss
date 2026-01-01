#pragma once
#include "cachemiss.hpp"

namespace zobrist {

inline u64 pieces[2][6][64];  // [Color][Piece][Square]
inline u64 side_to_move;
inline u64 ep_file[8];
inline u64 castling[16];  // Index by 4-bit castling state

inline u64 xorshift64(u64& state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

inline void init() {
    u64 state = 0x98f107a3c5e2b4d6ULL;  // Fixed seed for reproducibility

    for (int color = 0; color < 2; color++) {
        for (int piece = 0; piece < 6; piece++) {
            for (int sq = 0; sq < 64; sq++) {
                pieces[color][piece][sq] = xorshift64(state);
            }
        }
    }

    side_to_move = xorshift64(state);

    for (int i = 0; i < 8; i++) {
        ep_file[i] = xorshift64(state);
    }

    for (int i = 0; i < 16; i++) {
        castling[i] = xorshift64(state);
    }
}

}  // namespace zobrist

struct Board;
u64 compute_hash(const Board& board);
