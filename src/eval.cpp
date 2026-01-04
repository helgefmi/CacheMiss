#include "eval.hpp"
#include "pst_tables.hpp"

#include <algorithm>

// Compute game phase (0 = endgame, 24 = opening/middlegame)
static int compute_phase(const Board& board) {
    int phase = 0;
    for (int c = 0; c < 2; ++c) {
        phase += popcount(board.pieces[c][(int)Piece::Knight]) * 1;
        phase += popcount(board.pieces[c][(int)Piece::Bishop]) * 1;
        phase += popcount(board.pieces[c][(int)Piece::Rook]) * 2;
        phase += popcount(board.pieces[c][(int)Piece::Queen]) * 4;
    }
    return std::min(phase, 24);
}

int evaluate(const Board& board) {
    int mg_score = 0;
    int eg_score = 0;

    for (int piece = 0; piece < 6; ++piece) {
        // White pieces
        Bitboard white_bb = board.pieces[0][piece];
        while (white_bb) {
            int sq = lsb_index(white_bb);
            mg_score += PST_MG[piece][sq];
            eg_score += PST_EG[piece][sq];
            white_bb &= white_bb - 1;
        }

        // Black pieces (flip square vertically)
        Bitboard black_bb = board.pieces[1][piece];
        while (black_bb) {
            int sq = lsb_index(black_bb);
            int flipped_sq = sq ^ 56;
            mg_score -= PST_MG[piece][flipped_sq];
            eg_score -= PST_EG[piece][flipped_sq];
            black_bb &= black_bb - 1;
        }
    }

    // Interpolate between middlegame and endgame
    int phase = compute_phase(board);
    int score = (mg_score * phase + eg_score * (24 - phase)) / 24;

    return (board.turn == Color::White) ? score : -score;
}
