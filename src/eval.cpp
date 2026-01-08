#include "eval.hpp"
#include "pst_tables.hpp"
#include "precalc.hpp"

#include <algorithm>

// Passed pawn bonus by rank (from pawn's perspective)
// Index = rank for white (2-7), flipped rank for black
constexpr int PASSED_PAWN_MG[8] = { 0, 0, 5, 10, 20, 35, 60, 100 };
constexpr int PASSED_PAWN_EG[8] = { 0, 0, 10, 20, 40, 70, 120, 200 };

// Bonuses for protected/connected passers
constexpr int PROTECTED_PASSER_MG = 10;
constexpr int PROTECTED_PASSER_EG = 20;
constexpr int CONNECTED_PASSER_MG = 15;
constexpr int CONNECTED_PASSER_EG = 25;

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

// Evaluate passed pawns for both sides
static void evaluate_passed_pawns(const Board& board, int& mg, int& eg) {
    for (int c = 0; c < 2; ++c) {
        int sign = (c == 0) ? 1 : -1;
        Bitboard our_pawns = board.pieces[c][(int)Piece::Pawn];
        Bitboard enemy_pawns = board.pieces[c ^ 1][(int)Piece::Pawn];

        // Find passed pawns
        Bitboard passed = 0;
        Bitboard pawns = our_pawns;
        while (pawns) {
            int sq = lsb_index(pawns);
            if ((PASSED_PAWN_MASK[c][sq] & enemy_pawns) == 0)
                passed |= (1ULL << sq);
            pawns &= pawns - 1;
        }

        // Score each passed pawn
        while (passed) {
            int sq = lsb_index(passed);
            int rank = sq / 8;
            int file = sq % 8;
            int eff_rank = (c == 0) ? rank : (7 - rank);

            // Base bonus by rank
            mg += sign * PASSED_PAWN_MG[eff_rank];
            eg += sign * PASSED_PAWN_EG[eff_rank];

            // Protected passer (defended by own pawn)
            if (PAWN_ATTACKS[c ^ 1][sq] & our_pawns) {
                mg += sign * PROTECTED_PASSER_MG;
                eg += sign * PROTECTED_PASSER_EG;
            }

            // Connected passer (adjacent file has our passed pawn)
            if (ADJACENT_FILES[file] & passed) {
                mg += sign * CONNECTED_PASSER_MG;
                eg += sign * CONNECTED_PASSER_EG;
            }

            passed &= passed - 1;
        }
    }
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

    // Passed pawn evaluation
    evaluate_passed_pawns(board, mg_score, eg_score);

    // Interpolate between middlegame and endgame
    int phase = compute_phase(board);
    int score = (mg_score * phase + eg_score * (24 - phase)) / 24;

    return (board.turn == Color::White) ? score : -score;
}
