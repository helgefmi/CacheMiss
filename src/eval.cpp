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

// Bishop pair bonus
constexpr int BISHOP_PAIR_MG = 30;
constexpr int BISHOP_PAIR_EG = 50;

// Rook on open/semi-open file bonuses
constexpr int ROOK_OPEN_FILE_MG = 20;
constexpr int ROOK_OPEN_FILE_EG = 10;
constexpr int ROOK_SEMI_OPEN_FILE_MG = 10;
constexpr int ROOK_SEMI_OPEN_FILE_EG = 5;

// Rook on 7th rank bonus
constexpr int ROOK_ON_SEVENTH_MG = 20;
constexpr int ROOK_ON_SEVENTH_EG = 40;

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

// Compute pawn attacks for one side
static Bitboard compute_pawn_attacks(Bitboard pawns, int color) {
    Bitboard attacks = 0;
    while (pawns) {
        attacks |= PAWN_ATTACKS[color][lsb_index(pawns)];
        pawns &= pawns - 1;
    }
    return attacks;
}

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

// Main evaluation function - combines PST, mobility, and positional features
int evaluate(const Board& board) {
    int mg_score = 0;
    int eg_score = 0;

    Bitboard occ = board.all_occupied;
    Bitboard attacks[2] = {0, 0};  // Combined attack bitboards per side

    // Compute pawn attacks early (needed for safe mobility)
    Bitboard pawn_attacks[2];
    pawn_attacks[0] = compute_pawn_attacks(board.pieces[0][(int)Piece::Pawn], 0);
    pawn_attacks[1] = compute_pawn_attacks(board.pieces[1][(int)Piece::Pawn], 1);
    attacks[0] |= pawn_attacks[0];
    attacks[1] |= pawn_attacks[1];

    // Evaluate pieces - combined PST + mobility + positional features
    for (int c = 0; c < 2; ++c) {
        int sign = (c == 0) ? 1 : -1;
        Bitboard friendly = board.occupied[c];
        Bitboard enemy_pawn_att = pawn_attacks[c ^ 1];  // For safe mobility

        // Pawns (PST only, no mobility)
        Bitboard pawns = board.pieces[c][(int)Piece::Pawn];
        while (pawns) {
            int sq = lsb_index(pawns);
            int flipped_sq = (c == 0) ? sq : (sq ^ 56);
            mg_score += sign * PST_MG[(int)Piece::Pawn][flipped_sq];
            eg_score += sign * PST_EG[(int)Piece::Pawn][flipped_sq];
            pawns &= pawns - 1;
        }

        // Knights
        Bitboard knights = board.pieces[c][(int)Piece::Knight];
        while (knights) {
            int sq = lsb_index(knights);
            int flipped_sq = (c == 0) ? sq : (sq ^ 56);

            // PST
            mg_score += sign * PST_MG[(int)Piece::Knight][flipped_sq];
            eg_score += sign * PST_EG[(int)Piece::Knight][flipped_sq];

            // Mobility (safe squares only - not attacked by enemy pawns)
            Bitboard att = KNIGHT_MOVES[sq];
            attacks[c] |= att;
            int mob = popcount(att & ~friendly & ~enemy_pawn_att);
            mg_score += sign * MOBILITY_KNIGHT_MG[mob];
            eg_score += sign * MOBILITY_KNIGHT_EG[mob];

            knights &= knights - 1;
        }

        // Bishops
        Bitboard bishops = board.pieces[c][(int)Piece::Bishop];
        int bishop_count = popcount(bishops);

        // Bishop pair bonus
        if (bishop_count >= 2) {
            mg_score += sign * BISHOP_PAIR_MG;
            eg_score += sign * BISHOP_PAIR_EG;
        }

        while (bishops) {
            int sq = lsb_index(bishops);
            int flipped_sq = (c == 0) ? sq : (sq ^ 56);

            // PST
            mg_score += sign * PST_MG[(int)Piece::Bishop][flipped_sq];
            eg_score += sign * PST_EG[(int)Piece::Bishop][flipped_sq];

            // Mobility (safe squares)
            Bitboard att = get_bishop_attacks(sq, occ);
            attacks[c] |= att;
            int mob = std::min(popcount(att & ~friendly & ~enemy_pawn_att), 13);
            mg_score += sign * MOBILITY_BISHOP_MG[mob];
            eg_score += sign * MOBILITY_BISHOP_EG[mob];

            bishops &= bishops - 1;
        }

        // Rooks
        Bitboard rooks = board.pieces[c][(int)Piece::Rook];
        Bitboard our_pawns = board.pieces[c][(int)Piece::Pawn];
        Bitboard enemy_pawns = board.pieces[c ^ 1][(int)Piece::Pawn];
        // X-ray occupancy: remove friendly rooks so batteries see through each other
        Bitboard occ_xray_rooks = occ ^ rooks;

        while (rooks) {
            int sq = lsb_index(rooks);
            int flipped_sq = (c == 0) ? sq : (sq ^ 56);
            int file = sq % 8;
            int rank = sq / 8;

            // PST
            mg_score += sign * PST_MG[(int)Piece::Rook][flipped_sq];
            eg_score += sign * PST_EG[(int)Piece::Rook][flipped_sq];

            // Mobility with x-ray through friendly rooks (safe squares)
            Bitboard att = get_rook_attacks(sq, occ_xray_rooks);
            attacks[c] |= att;
            int mob = std::min(popcount(att & ~friendly & ~enemy_pawn_att), 14);
            mg_score += sign * MOBILITY_ROOK_MG[mob];
            eg_score += sign * MOBILITY_ROOK_EG[mob];

            // Open/semi-open file bonus
            Bitboard file_mask = FILE_MASKS[file];
            bool no_our_pawns = (our_pawns & file_mask) == 0;
            bool no_enemy_pawns = (enemy_pawns & file_mask) == 0;

            if (no_our_pawns && no_enemy_pawns) {
                mg_score += sign * ROOK_OPEN_FILE_MG;
                eg_score += sign * ROOK_OPEN_FILE_EG;
            } else if (no_our_pawns) {
                mg_score += sign * ROOK_SEMI_OPEN_FILE_MG;
                eg_score += sign * ROOK_SEMI_OPEN_FILE_EG;
            }

            // Rook on 7th rank
            int seventh_rank = (c == 0) ? 6 : 1;
            if (rank == seventh_rank) {
                mg_score += sign * ROOK_ON_SEVENTH_MG;
                eg_score += sign * ROOK_ON_SEVENTH_EG;
            }

            rooks &= rooks - 1;
        }

        // Queens
        Bitboard queens = board.pieces[c][(int)Piece::Queen];
        while (queens) {
            int sq = lsb_index(queens);
            int flipped_sq = (c == 0) ? sq : (sq ^ 56);

            // PST
            mg_score += sign * PST_MG[(int)Piece::Queen][flipped_sq];
            eg_score += sign * PST_EG[(int)Piece::Queen][flipped_sq];

            // Mobility (safe squares)
            Bitboard att = get_queen_attacks(sq, occ);
            attacks[c] |= att;
            int mob = std::min(popcount(att & ~friendly & ~enemy_pawn_att), 27);
            mg_score += sign * MOBILITY_QUEEN_MG[mob];
            eg_score += sign * MOBILITY_QUEEN_EG[mob];

            queens &= queens - 1;
        }

        // King PST only (no mobility score)
        {
            int sq = board.king_sq[c];
            int flipped_sq = (c == 0) ? sq : (sq ^ 56);
            mg_score += sign * PST_MG[(int)Piece::King][flipped_sq];
            eg_score += sign * PST_EG[(int)Piece::King][flipped_sq];
            attacks[c] |= KING_MOVES[sq];
        }
    }

    // Pawn structure evaluation
    evaluate_pawn_structure(board, mg_score, eg_score);
    evaluate_passed_pawns(board, mg_score, eg_score);

    // Space evaluation: control over key zones
    int center_diff = popcount(attacks[0] & CENTER_4) - popcount(attacks[1] & CENTER_4);
    mg_score += center_diff * SPACE_CENTER_MG;
    eg_score += center_diff * SPACE_CENTER_EG;

    int ext_diff = popcount(attacks[0] & EXTENDED_CENTER) - popcount(attacks[1] & EXTENDED_CENTER);
    mg_score += ext_diff * SPACE_EXTENDED_MG;
    eg_score += ext_diff * SPACE_EXTENDED_EG;

    // King safety: count attacks on enemy king zone
    Bitboard white_king_zone = KING_MOVES[board.king_sq[0]] | square_bb(board.king_sq[0]);
    Bitboard black_king_zone = KING_MOVES[board.king_sq[1]] | square_bb(board.king_sq[1]);

    int white_king_pressure = popcount(attacks[0] & black_king_zone);
    int black_king_pressure = popcount(attacks[1] & white_king_zone);

    int king_safety_diff = white_king_pressure - black_king_pressure;
    mg_score += king_safety_diff * KING_ATTACK_MG;
    eg_score += king_safety_diff * KING_ATTACK_EG;

    // Interpolate between middlegame and endgame using incremental phase
    int phase = std::min(board.phase, 24);
    int score = (mg_score * phase + eg_score * (24 - phase)) / 24;

    return (board.turn == Color::White) ? score : -score;
}
