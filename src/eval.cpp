#include "eval.hpp"
#include "pst_tables.hpp"
#include "precalc.hpp"
#include "move.hpp"

#include <algorithm>

// Mobility tables (indexed by reachable square count)
constexpr int MOBILITY_KNIGHT_MG[9] = {-50, -25, -10, 0, 10, 20, 28, 34, 38};
constexpr int MOBILITY_KNIGHT_EG[9] = {-60, -30, -10, 5, 15, 25, 32, 38, 42};

constexpr int MOBILITY_BISHOP_MG[14] = {-40, -15, 0, 12, 22, 30, 36, 42, 46, 50, 54, 56, 58, 60};
constexpr int MOBILITY_BISHOP_EG[14] = {-50, -20, 0, 15, 28, 38, 48, 55, 62, 68, 72, 76, 78, 80};

constexpr int MOBILITY_ROOK_MG[15] = {-50, -20, -5, 0, 5, 10, 15, 20, 24, 28, 32, 35, 38, 40, 42};
constexpr int MOBILITY_ROOK_EG[15] = {-70, -30, -5, 10, 25, 40, 55, 68, 80, 90, 100, 108, 115, 120, 125};

constexpr int MOBILITY_QUEEN_MG[28] = {-30, -15, -5, 0, 3, 6, 9, 12, 15, 18, 20, 22, 24, 26, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41};
constexpr int MOBILITY_QUEEN_EG[28] = {-40, -20, -5, 5, 12, 18, 24, 30, 36, 42, 47, 52, 57, 62, 66, 70, 74, 78, 81, 84, 87, 90, 93, 96, 99, 102, 105, 108};

// Space evaluation zones
constexpr Bitboard CENTER_4 = 0x0000001818000000ULL;         // d4, d5, e4, e5
constexpr Bitboard EXTENDED_CENTER = 0x00003C3C3C3C0000ULL;  // c3-f6 region
constexpr int SPACE_CENTER_MG = 4;
constexpr int SPACE_CENTER_EG = 1;
constexpr int SPACE_EXTENDED_MG = 1;
constexpr int SPACE_EXTENDED_EG = 0;

// King safety: penalty per attack on king zone (king square + adjacent squares)
constexpr int KING_ATTACK_MG = 8;   // Per square attacked near enemy king
constexpr int KING_ATTACK_EG = 2;   // Less important in endgame

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

// Evaluate piece activity: mobility, space control, and king safety
static void evaluate_pieces(const Board& board, int& mg, int& eg) {
    Bitboard occ = board.all_occupied;
    Bitboard attacks[2] = {0, 0};  // Combined attack bitboards per side

    for (int c = 0; c < 2; ++c) {
        int sign = (c == 0) ? 1 : -1;
        Bitboard friendly = board.occupied[c];

        // Pawns (attacks only, no mobility score for pawns)
        Bitboard pawns = board.pieces[c][(int)Piece::Pawn];
        while (pawns) {
            attacks[c] |= PAWN_ATTACKS[c][lsb_index(pawns)];
            pawns &= pawns - 1;
        }

        // Knights
        Bitboard knights = board.pieces[c][(int)Piece::Knight];
        while (knights) {
            int sq = lsb_index(knights);
            Bitboard att = KNIGHT_MOVES[sq];
            attacks[c] |= att;
            int mob = popcount(att & ~friendly);
            mg += sign * MOBILITY_KNIGHT_MG[mob];
            eg += sign * MOBILITY_KNIGHT_EG[mob];
            knights &= knights - 1;
        }

        // Bishops
        Bitboard bishops = board.pieces[c][(int)Piece::Bishop];
        while (bishops) {
            int sq = lsb_index(bishops);
            Bitboard att = get_bishop_attacks(sq, occ);
            attacks[c] |= att;
            int mob = std::min(popcount(att & ~friendly), 13);
            mg += sign * MOBILITY_BISHOP_MG[mob];
            eg += sign * MOBILITY_BISHOP_EG[mob];
            bishops &= bishops - 1;
        }

        // Rooks
        Bitboard rooks = board.pieces[c][(int)Piece::Rook];
        while (rooks) {
            int sq = lsb_index(rooks);
            Bitboard att = get_rook_attacks(sq, occ);
            attacks[c] |= att;
            int mob = std::min(popcount(att & ~friendly), 14);
            mg += sign * MOBILITY_ROOK_MG[mob];
            eg += sign * MOBILITY_ROOK_EG[mob];
            rooks &= rooks - 1;
        }

        // Queens
        Bitboard queens = board.pieces[c][(int)Piece::Queen];
        while (queens) {
            int sq = lsb_index(queens);
            Bitboard att = get_queen_attacks(sq, occ);
            attacks[c] |= att;
            int mob = std::min(popcount(att & ~friendly), 27);
            mg += sign * MOBILITY_QUEEN_MG[mob];
            eg += sign * MOBILITY_QUEEN_EG[mob];
            queens &= queens - 1;
        }

        // King attacks (for space calculation only, no mobility bonus)
        attacks[c] |= KING_MOVES[board.king_sq[c]];
    }

    // Space evaluation: control over key zones
    int center_diff = popcount(attacks[0] & CENTER_4) - popcount(attacks[1] & CENTER_4);
    mg += center_diff * SPACE_CENTER_MG;
    eg += center_diff * SPACE_CENTER_EG;

    int ext_diff = popcount(attacks[0] & EXTENDED_CENTER) - popcount(attacks[1] & EXTENDED_CENTER);
    mg += ext_diff * SPACE_EXTENDED_MG;
    eg += ext_diff * SPACE_EXTENDED_EG;

    // King safety: count attacks on enemy king zone
    Bitboard white_king_zone = KING_MOVES[board.king_sq[0]] | square_bb(board.king_sq[0]);
    Bitboard black_king_zone = KING_MOVES[board.king_sq[1]] | square_bb(board.king_sq[1]);

    // White's attacks on black's king zone (good for white)
    int white_king_pressure = popcount(attacks[0] & black_king_zone);
    // Black's attacks on white's king zone (good for black)
    int black_king_pressure = popcount(attacks[1] & white_king_zone);

    int king_safety_diff = white_king_pressure - black_king_pressure;
    mg += king_safety_diff * KING_ATTACK_MG;
    eg += king_safety_diff * KING_ATTACK_EG;
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

    // Piece activity: mobility, space, king safety
    evaluate_pieces(board, mg_score, eg_score);

    // Interpolate between middlegame and endgame using incremental phase
    int phase = std::min(board.phase, 24);  // Clamp to 24 (shouldn't exceed, but defensive)
    int score = (mg_score * phase + eg_score * (24 - phase)) / 24;

    return (board.turn == Color::White) ? score : -score;
}
