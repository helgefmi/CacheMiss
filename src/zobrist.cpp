#include "zobrist.hpp"
#include "board.hpp"

u64 compute_hash(const Board& board) {
    u64 h = 0;

    // Hash all pieces
    for (int color = 0; color < 2; color++) {
        for (int piece = 0; piece < 6; piece++) {
            Bitboard bb = board.pieces[color][piece];
            while (bb) {
                int sq = lsb_index(bb);
                h ^= zobrist::pieces[color][piece][sq];
                bb &= bb - 1;
            }
        }
    }

    // Hash side to move (only if black)
    if (board.turn == Color::Black) {
        h ^= zobrist::side_to_move;
    }

    // Hash en passant file
    if (board.ep_file < 8) {
        h ^= zobrist::ep_file[board.ep_file];
    }

    // Hash castling rights
    h ^= zobrist::castling[board.castling];

    return h;
}
