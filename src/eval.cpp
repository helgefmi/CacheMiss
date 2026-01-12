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

// Pawn structure penalties
constexpr int DOUBLED_PAWN_MG = -10;
constexpr int DOUBLED_PAWN_EG = -20;
constexpr int ISOLATED_PAWN_MG = -15;
constexpr int ISOLATED_PAWN_EG = -10;
constexpr int BACKWARD_PAWN_MG = -10;
constexpr int BACKWARD_PAWN_EG = -8;

// Evaluate pawn structure (doubled, isolated, backward pawns)
static void evaluate_pawn_structure(const Board& board, int& mg, int& eg) {
    for (int c = 0; c < 2; ++c) {
        int sign = (c == 0) ? 1 : -1;
        Bitboard our_pawns = board.pieces[c][(int)Piece::Pawn];
        Bitboard enemy_pawns = board.pieces[c ^ 1][(int)Piece::Pawn];

        // Doubled pawns: count extra pawns on each file
        for (int f = 0; f < 8; ++f) {
            int pawns_on_file = popcount(our_pawns & FILE_MASKS[f]);
            if (pawns_on_file > 1) {
                mg += sign * DOUBLED_PAWN_MG * (pawns_on_file - 1);
                eg += sign * DOUBLED_PAWN_EG * (pawns_on_file - 1);
            }
        }

        // Isolated and backward pawns
        Bitboard pawns = our_pawns;
        while (pawns) {
            int sq = lsb_index(pawns);
            int rank = sq / 8;
            int file = sq % 8;

            // Isolated pawn: no friendly pawns on adjacent files
            bool is_isolated = (our_pawns & ADJACENT_FILES[file]) == 0;
            if (is_isolated) {
                mg += sign * ISOLATED_PAWN_MG;
                eg += sign * ISOLATED_PAWN_EG;
            } else {
                // Backward pawn: can't be defended by friendly pawns
                // A pawn is backward if:
                // 1. No friendly pawns behind it on adjacent files to defend it
                // 2. The stop square is controlled by enemy pawns
                int eff_rank = (c == 0) ? rank : (7 - rank);
                if (eff_rank > 1) {  // Not on starting rank
                    // Check if any friendly pawn can defend this pawn
                    Bitboard defenders_zone;
                    if (c == 0) {
                        // White: check ranks behind this pawn on adjacent files
                        Bitboard behind_mask = (1ULL << (rank * 8)) - 1;  // All squares below this rank
                        defenders_zone = ADJACENT_FILES[file] & behind_mask;
                    } else {
                        // Black: check ranks behind (higher ranks) on adjacent files
                        Bitboard behind_mask = ~((1ULL << ((rank + 1) * 8)) - 1);  // All squares above this rank
                        defenders_zone = ADJACENT_FILES[file] & behind_mask;
                    }

                    bool no_defenders = (our_pawns & defenders_zone) == 0;

                    // Check if stop square is attacked by enemy pawns
                    int stop_sq = (c == 0) ? sq + 8 : sq - 8;
                    bool stop_attacked = (stop_sq >= 0 && stop_sq < 64) &&
                                        (PAWN_ATTACKS[c ^ 1][stop_sq] & enemy_pawns);

                    if (no_defenders && stop_attacked) {
                        mg += sign * BACKWARD_PAWN_MG;
                        eg += sign * BACKWARD_PAWN_EG;
                    }
                }
            }

            pawns &= pawns - 1;
        }
    }
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

    // Pawn structure evaluation
    evaluate_pawn_structure(board, mg_score, eg_score);
    evaluate_passed_pawns(board, mg_score, eg_score);

    // Interpolate between middlegame and endgame using incremental phase
    int phase = std::min(board.phase, 24);  // Clamp to 24 (shouldn't exceed, but defensive)
    int score = (mg_score * phase + eg_score * (24 - phase)) / 24;

    return (board.turn == Color::White) ? score : -score;
}
