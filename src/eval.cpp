#include "eval.hpp"
#include "eval_params.hpp"
#include "precalc.hpp"
#include "move.hpp"

#include <algorithm>

// Global pawn structure cache
PawnCache g_pawn_cache(1);

// Space evaluation zones
constexpr Bitboard CENTER_4 = 0x0000001818000000ULL;         // d4, d5, e4, e5
constexpr Bitboard EXTENDED_CENTER = 0x00003C3C3C3C0000ULL;  // c3-f6 region

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

// Evaluate all pieces: PST + mobility + positional features (rook on open files, 7th rank, bishop pair)
// Also populates the attacks[] array for use in space/king safety evaluation
static void evaluate_pieces(const Board& board, int& mg, int& eg, Bitboard attacks[2], const Bitboard pawn_attacks[2]) {
    Bitboard occ = board.all_occupied;

    for (int c = 0; c < 2; ++c) {
        int sign = (c == 0) ? 1 : -1;
        Bitboard friendly = board.occupied[c];
        Bitboard enemy_pawn_att = pawn_attacks[c ^ 1];

        // Pawns (PST only, no mobility)
        Bitboard pawns = board.pieces[c][(int)Piece::Pawn];
        while (pawns) {
            int sq = lsb_index(pawns);
            int flipped_sq = (c == 0) ? sq : (sq ^ 56);
            mg += sign * PST_MG[(int)Piece::Pawn][flipped_sq];
            eg += sign * PST_EG[(int)Piece::Pawn][flipped_sq];
            pawns &= pawns - 1;
        }

        // Knights
        Bitboard knights = board.pieces[c][(int)Piece::Knight];
        while (knights) {
            int sq = lsb_index(knights);
            int flipped_sq = (c == 0) ? sq : (sq ^ 56);

            mg += sign * PST_MG[(int)Piece::Knight][flipped_sq];
            eg += sign * PST_EG[(int)Piece::Knight][flipped_sq];

            Bitboard att = KNIGHT_MOVES[sq];
            attacks[c] |= att;
            int mob = std::min(popcount(att & ~friendly & ~enemy_pawn_att), 8);
            mg += sign * MOBILITY_KNIGHT_MG[mob];
            eg += sign * MOBILITY_KNIGHT_EG[mob];

            knights &= knights - 1;
        }

        // Bishops
        Bitboard bishops = board.pieces[c][(int)Piece::Bishop];
        int bishop_count = popcount(bishops);

        if (bishop_count >= 2) {
            mg += sign * BISHOP_PAIR_MG;
            eg += sign * BISHOP_PAIR_EG;
        }

        while (bishops) {
            int sq = lsb_index(bishops);
            int flipped_sq = (c == 0) ? sq : (sq ^ 56);

            mg += sign * PST_MG[(int)Piece::Bishop][flipped_sq];
            eg += sign * PST_EG[(int)Piece::Bishop][flipped_sq];

            Bitboard att = get_bishop_attacks(sq, occ);
            attacks[c] |= att;
            int mob = std::min(popcount(att & ~friendly & ~enemy_pawn_att), 13);
            mg += sign * MOBILITY_BISHOP_MG[mob];
            eg += sign * MOBILITY_BISHOP_EG[mob];

            bishops &= bishops - 1;
        }

        // Rooks
        Bitboard rooks = board.pieces[c][(int)Piece::Rook];
        Bitboard our_pawns = board.pieces[c][(int)Piece::Pawn];
        Bitboard enemy_pawns = board.pieces[c ^ 1][(int)Piece::Pawn];
        Bitboard occ_xray_rooks = occ ^ rooks;  // X-ray through friendly rooks

        while (rooks) {
            int sq = lsb_index(rooks);
            int flipped_sq = (c == 0) ? sq : (sq ^ 56);
            int file = sq % 8;
            int rank = sq / 8;

            mg += sign * PST_MG[(int)Piece::Rook][flipped_sq];
            eg += sign * PST_EG[(int)Piece::Rook][flipped_sq];

            Bitboard att = get_rook_attacks(sq, occ_xray_rooks);
            attacks[c] |= att;
            int mob = std::min(popcount(att & ~friendly & ~enemy_pawn_att), 14);
            mg += sign * MOBILITY_ROOK_MG[mob];
            eg += sign * MOBILITY_ROOK_EG[mob];

            // Open/semi-open file bonus
            Bitboard file_mask = FILE_MASKS[file];
            bool no_our_pawns = (our_pawns & file_mask) == 0;
            bool no_enemy_pawns = (enemy_pawns & file_mask) == 0;

            if (no_our_pawns && no_enemy_pawns) {
                mg += sign * ROOK_OPEN_FILE_MG;
                eg += sign * ROOK_OPEN_FILE_EG;
            } else if (no_our_pawns) {
                mg += sign * ROOK_SEMI_OPEN_FILE_MG;
                eg += sign * ROOK_SEMI_OPEN_FILE_EG;
            }

            // Rook on 7th rank
            int seventh_rank = (c == 0) ? 6 : 1;
            if (rank == seventh_rank) {
                mg += sign * ROOK_ON_SEVENTH_MG;
                eg += sign * ROOK_ON_SEVENTH_EG;
            }

            rooks &= rooks - 1;
        }

        // Queens
        Bitboard queens = board.pieces[c][(int)Piece::Queen];
        while (queens) {
            int sq = lsb_index(queens);
            int flipped_sq = (c == 0) ? sq : (sq ^ 56);

            mg += sign * PST_MG[(int)Piece::Queen][flipped_sq];
            eg += sign * PST_EG[(int)Piece::Queen][flipped_sq];

            Bitboard att = get_queen_attacks(sq, occ);
            attacks[c] |= att;
            int mob = std::min(popcount(att & ~friendly & ~enemy_pawn_att), 27);
            mg += sign * MOBILITY_QUEEN_MG[mob];
            eg += sign * MOBILITY_QUEEN_EG[mob];

            queens &= queens - 1;
        }

        // King (PST only)
        {
            int sq = board.king_sq[c];
            int flipped_sq = (c == 0) ? sq : (sq ^ 56);
            mg += sign * PST_MG[(int)Piece::King][flipped_sq];
            eg += sign * PST_EG[(int)Piece::King][flipped_sq];
            attacks[c] |= KING_MOVES[sq];
        }
    }
}

// Evaluate space control: center and extended center
static void evaluate_space(int& mg, int& eg, const Bitboard attacks[2]) {
    int center_diff = popcount(attacks[0] & CENTER_4) - popcount(attacks[1] & CENTER_4);
    mg += center_diff * SPACE_CENTER_MG;
    eg += center_diff * SPACE_CENTER_EG;

    int ext_diff = popcount(attacks[0] & EXTENDED_CENTER) - popcount(attacks[1] & EXTENDED_CENTER);
    mg += ext_diff * SPACE_EXTENDED_MG;
    eg += ext_diff * SPACE_EXTENDED_EG;
}

// Evaluate king safety: attacks on enemy king zone
static void evaluate_king_safety(const Board& board, int& mg, int& eg, const Bitboard attacks[2]) {
    Bitboard white_king_zone = KING_MOVES[board.king_sq[0]] | square_bb(board.king_sq[0]);
    Bitboard black_king_zone = KING_MOVES[board.king_sq[1]] | square_bb(board.king_sq[1]);

    int white_king_pressure = popcount(attacks[0] & black_king_zone);
    int black_king_pressure = popcount(attacks[1] & white_king_zone);

    int king_safety_diff = white_king_pressure - black_king_pressure;
    mg += king_safety_diff * KING_ATTACK_MG;
    eg += king_safety_diff * KING_ATTACK_EG;
}

// Main evaluation function - combines PST, mobility, and positional features
int evaluate(const Board& board) {
    int mg_score = 0;
    int eg_score = 0;

    Bitboard attacks[2] = {0, 0};

    // Compute pawn attacks early (needed for safe mobility)
    Bitboard pawn_attacks[2];
    pawn_attacks[0] = compute_pawn_attacks(board.pieces[0][(int)Piece::Pawn], 0);
    pawn_attacks[1] = compute_pawn_attacks(board.pieces[1][(int)Piece::Pawn], 1);
    attacks[0] |= pawn_attacks[0];
    attacks[1] |= pawn_attacks[1];

    // Evaluate pieces (PST + mobility + positional features)
    evaluate_pieces(board, mg_score, eg_score, attacks, pawn_attacks);

    // Pawn structure evaluation (with cache)
    int pawn_mg = 0, pawn_eg = 0;
    if (!g_pawn_cache.probe(board.pawn_key, pawn_mg, pawn_eg)) {
        evaluate_pawn_structure(board, pawn_mg, pawn_eg);
        evaluate_passed_pawns(board, pawn_mg, pawn_eg);
        g_pawn_cache.store(board.pawn_key, pawn_mg, pawn_eg);
    }
    mg_score += pawn_mg;
    eg_score += pawn_eg;

    // Space control evaluation
    evaluate_space(mg_score, eg_score, attacks);

    // King safety evaluation
    evaluate_king_safety(board, mg_score, eg_score, attacks);

    // Interpolate between middlegame and endgame using incremental phase
    int phase = std::min(board.phase, MAX_PHASE);
    int score = (mg_score * phase + eg_score * (MAX_PHASE - phase)) / MAX_PHASE;

    return (board.turn == Color::White) ? score : -score;
}
