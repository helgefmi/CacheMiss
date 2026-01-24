// test_movegen.cpp - Move generation tests
#include "test_framework.hpp"
#include "board.hpp"
#include "move.hpp"
#include <algorithm>

// Helper to check if a specific move exists in move list
static bool has_move(const MoveList& moves, int from, int to,
                     Piece promo = Piece::None, bool is_ep = false, bool is_castle = false) {
    for (int i = 0; i < moves.size; i++) {
        const auto& m = moves[i];
        if (m.from() == from && m.to() == to && m.promotion() == promo) {
            if (is_ep && !m.is_en_passant()) continue;
            if (is_castle && !m.is_castling()) continue;
            return true;
        }
    }
    return false;
}

// Square indices for readability
constexpr int A1 = 0, B1 = 1, C1 = 2, D1 = 3, E1 = 4, F1 = 5, G1 = 6, H1 = 7;
constexpr int A2 = 8, B2 = 9, C2 = 10, D2 = 11, E2 = 12, F2 = 13, G2 = 14, H2 = 15;
constexpr int A3 = 16, B3 = 17, C3 = 18, D3 = 19, E3 = 20, F3 = 21, G3 = 22, H3 = 23;
constexpr int A4 = 24, B4 = 25, C4 = 26, D4 = 27, E4 = 28, F4 = 29, G4 = 30, H4 = 31;
constexpr int A5 = 32, B5 = 33, C5 = 34, D5 = 35, E5 = 36, F5 = 37, G5 = 38, H5 = 39;
constexpr int A6 = 40, B6 = 41, C6 = 42, D6 = 43, E6 = 44, F6 = 45, G6 = 46, H6 = 47;
constexpr int A7 = 48, B7 = 49, C7 = 50, D7 = 51, E7 = 52, F7 = 53, G7 = 54, H7 = 55;
constexpr int A8 = 56, B8 = 57, C8 = 58, D8 = 59, E8 = 60, F8 = 61, G8 = 62, H8 = 63;

// ============================================================================
// En Passant Tests
// ============================================================================

static void test_ep_basic() {
    // Basic en passant: White pawn on e5, black pawn just moved d7-d5
    Board board("8/8/8/3pP3/8/8/8/K6k w - d6 0 1");
    auto moves = generate_moves(board);

    // e5xd6 should be generated as an EP capture
    ASSERT_TRUE(has_move(moves, E5, D6, Piece::None, true));
}

static void test_ep_a_file() {
    // EP on a-file: White pawn on b5, black pawn just moved a7-a5
    Board board("8/8/8/pP6/8/8/8/K6k w - a6 0 1");
    auto moves = generate_moves(board);

    ASSERT_TRUE(has_move(moves, B5, A6, Piece::None, true));
}

static void test_ep_h_file() {
    // EP on h-file: White pawn on g5, black pawn just moved h7-h5
    Board board("8/8/8/6Pp/8/8/8/K6k w - h6 0 1");
    auto moves = generate_moves(board);

    ASSERT_TRUE(has_move(moves, G5, H6, Piece::None, true));
}

static void test_ep_discovers_check() {
    // EP that discovers check: black king on b5, white rook on h5
    Board board("8/8/8/1k1pP2R/8/8/8/4K3 w - d6 0 1");
    auto moves = generate_moves(board);

    // e5xd6 should be generated (discovers check on black king)
    ASSERT_TRUE(has_move(moves, E5, D6, Piece::None, true));
}

static void test_ep_illegal_pin() {
    // EP that would be illegal due to horizontal pin
    // White king on a5, black rook on h5, pawns on d5(black) and e5(white)
    Board board("8/8/8/K2pP2r/8/8/8/7k w - d6 0 1");
    auto all_moves = generate_moves(board);

    // Find the EP move
    bool found_ep = false;
    for (int i = 0; i < all_moves.size; i++) {
        if (all_moves[i].is_en_passant()) {
            // Make the move and check if it leaves king in check
            Board copy = board;
            Move32 m = all_moves[i];
            make_move(copy, m);
            // is_illegal checks if the OPPONENT's king is in check
            // After white moves, it's black's turn, so we check if white king is in check
            bool illegal = is_attacked(copy.king_sq[0], Color::Black, copy);
            if (!illegal) {
                found_ep = true;
            }
        }
    }
    // The EP should NOT be a legal move due to discovered attack
    ASSERT_FALSE(found_ep);
}

// ============================================================================
// Castling Tests
// ============================================================================

static void test_castling_both_sides() {
    Board board("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
    auto moves = generate_moves(board);

    // White can castle both sides
    ASSERT_TRUE(has_move(moves, E1, G1, Piece::None, false, true));  // O-O
    ASSERT_TRUE(has_move(moves, E1, C1, Piece::None, false, true));  // O-O-O
}

static void test_castling_blocked_kingside() {
    // Bishop on f1 blocks kingside castling
    Board board("r3k2r/8/8/8/8/8/8/R3KB1R w KQkq - 0 1");
    auto moves = generate_moves(board);

    ASSERT_FALSE(has_move(moves, E1, G1, Piece::None, false, true));  // O-O blocked
    ASSERT_TRUE(has_move(moves, E1, C1, Piece::None, false, true));   // O-O-O still ok
}

static void test_castling_through_check() {
    // Rook on e7 attacks e1 (king in check) - can't castle
    Board board("r3k2r/4r3/8/8/8/8/8/R3K2R w KQkq - 0 1");
    auto moves = generate_moves(board);

    // King is in check, no castling allowed
    ASSERT_FALSE(has_move(moves, E1, G1, Piece::None, false, true));
    ASSERT_FALSE(has_move(moves, E1, C1, Piece::None, false, true));
}

static void test_castling_f1_attacked() {
    // Black rook attacks f1 - can't castle kingside (would pass through check)
    Board board("r3k2r/5r2/8/8/8/8/8/R3K2R w KQkq - 0 1");
    auto moves = generate_moves(board);

    ASSERT_FALSE(has_move(moves, E1, G1, Piece::None, false, true));  // f1 attacked
    ASSERT_TRUE(has_move(moves, E1, C1, Piece::None, false, true));   // O-O-O ok
}

static void test_castling_b1_attacked_ok() {
    // Rook on b2 attacks only b1 - queenside castling IS legal (king doesn't cross b1)
    // Note: A queen on b3 would also attack d1, which WOULD prevent O-O-O
    Board board("r3k2r/8/8/8/8/8/1r6/R3K2R w KQkq - 0 1");
    auto moves = generate_moves(board);

    // b1 being attacked doesn't prevent O-O-O
    ASSERT_TRUE(has_move(moves, E1, C1, Piece::None, false, true));
}

static void test_castling_rights_after_rook_capture() {
    // After Rxa8, black should lose queenside castling rights
    Board board("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");

    // Find and make Rxa8
    auto moves = generate_moves(board);
    Move32 rxa8;
    for (int i = 0; i < moves.size; i++) {
        if (moves[i].from() == A1 && moves[i].to() == A8) {
            rxa8 = moves[i];
            break;
        }
    }

    make_move(board, rxa8);

    // Black's queenside castling should be gone (bit 2 = bQ = 4)
    ASSERT_EQ(board.castling & 4, 0);
    // Black's kingside should still be available
    ASSERT_NE(board.castling & 8, 0);
}

// ============================================================================
// Promotion Tests
// ============================================================================

static void test_promotion_all_types() {
    Board board("8/P7/8/8/8/8/8/K6k w - - 0 1");
    auto moves = generate_moves(board);

    // All 4 promotion types should be generated
    ASSERT_TRUE(has_move(moves, A7, A8, Piece::Queen));
    ASSERT_TRUE(has_move(moves, A7, A8, Piece::Rook));
    ASSERT_TRUE(has_move(moves, A7, A8, Piece::Bishop));
    ASSERT_TRUE(has_move(moves, A7, A8, Piece::Knight));
}

static void test_promotion_capture() {
    Board board("1n6/P7/8/8/8/8/8/K6k w - - 0 1");
    auto moves = generate_moves(board);

    // Capture promotions to b8
    ASSERT_TRUE(has_move(moves, A7, B8, Piece::Queen));
    ASSERT_TRUE(has_move(moves, A7, B8, Piece::Rook));
    ASSERT_TRUE(has_move(moves, A7, B8, Piece::Bishop));
    ASSERT_TRUE(has_move(moves, A7, B8, Piece::Knight));

    // Also can push to a8
    ASSERT_TRUE(has_move(moves, A7, A8, Piece::Queen));
}

static void test_promotion_black() {
    Board board("k6K/8/8/8/8/8/p7/8 b - - 0 1");
    auto moves = generate_moves(board);

    // Black promotion
    ASSERT_TRUE(has_move(moves, A2, A1, Piece::Queen));
    ASSERT_TRUE(has_move(moves, A2, A1, Piece::Knight));
}

// ============================================================================
// Double Pawn Push Tests
// ============================================================================

static void test_double_push_sets_ep() {
    Board board("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");

    // Find e2-e4
    auto moves = generate_moves(board);
    Move32 e2e4;
    for (int i = 0; i < moves.size; i++) {
        if (moves[i].from() == E2 && moves[i].to() == E4) {
            e2e4 = moves[i];
            break;
        }
    }

    make_move(board, e2e4);
    ASSERT_EQ(board.ep_file, 4);  // e-file = 4
}

static void test_double_push_blocked() {
    // Knight on e3 blocks e2-e4
    Board board("8/8/8/8/8/4n3/4P3/K6k w - - 0 1");
    auto moves = generate_moves(board);

    // e2-e4 should NOT be generated
    ASSERT_FALSE(has_move(moves, E2, E4));
    // e2-e3 also blocked
    ASSERT_FALSE(has_move(moves, E2, E3));
}

static void test_single_push_blocked() {
    // Knight on e3 blocks e2-e3, but not e2-e4 check... actually e4 is also blocked
    Board board("8/8/8/8/4n3/8/4P3/K6k w - - 0 1");
    auto moves = generate_moves(board);

    // e2-e3 is ok
    ASSERT_TRUE(has_move(moves, E2, E3));
    // e2-e4 blocked by knight
    ASSERT_FALSE(has_move(moves, E2, E4));
}

// ============================================================================
// Move Type Generation Tests
// ============================================================================

static void test_noisy_moves_only() {
    Board board("r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 0 1");
    auto noisy = generate_moves<MoveType::Noisy>(board);

    // All noisy moves should be captures or promotions
    for (int i = 0; i < noisy.size; i++) {
        ASSERT_TRUE(noisy[i].is_capture() || noisy[i].is_promotion());
    }
}

static void test_quiet_moves_only() {
    Board board("r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 0 1");
    auto quiet = generate_moves<MoveType::Quiet>(board);

    // Quiet moves should NOT be captures (but castling is included in quiet)
    for (int i = 0; i < quiet.size; i++) {
        ASSERT_FALSE(quiet[i].is_capture());
        ASSERT_FALSE(quiet[i].is_promotion());
    }
}

// ============================================================================
// Check Evasion Tests
// ============================================================================

static void test_must_block_check() {
    // White king in check from black rook on e8, must block or move king
    Board board("4r3/8/8/8/8/8/8/4K3 w - - 0 1");
    auto moves = generate_moves(board);

    // Move generation produces pseudo-legal moves. We verify that after filtering
    // for legality (king not in check), all remaining moves are valid escapes.
    // In this position, only king moves off the e-file are legal.
    int legal_count = 0;
    for (int i = 0; i < moves.size; i++) {
        Board copy = board;
        Move32 m = moves[i];
        make_move(copy, m);
        // Filter: skip illegal moves (king still in check)
        if (is_attacked(copy.king_sq[0], Color::Black, copy)) {
            continue;  // Pseudo-legal but illegal
        }
        legal_count++;
        // All legal moves should leave king not in check (already verified above)
    }
    // Should have some legal moves (king can move to d1, d2, f1, f2)
    ASSERT_GT(legal_count, 0);
}

static void test_pinned_piece() {
    // Bishop on c1 pinned by rook on a1 (king on d1)
    Board board("8/8/8/8/8/8/8/r1BK3k w - - 0 1");
    auto moves = generate_moves(board);

    // Bishop can't move (pinned along rank)
    // Count legal bishop moves
    int bishop_moves = 0;
    for (int i = 0; i < moves.size; i++) {
        if (moves[i].from() == C1) {
            // Check if move is legal
            Board copy = board;
            Move32 m = moves[i];
            make_move(copy, m);
            if (!is_attacked(copy.king_sq[0], Color::Black, copy)) {
                bishop_moves++;
            }
        }
    }
    ASSERT_EQ(bishop_moves, 0);
}

// Registration function
void register_movegen_tests() {
    REGISTER_TEST(MoveGen, EP_Basic, test_ep_basic);
    REGISTER_TEST(MoveGen, EP_AFile, test_ep_a_file);
    REGISTER_TEST(MoveGen, EP_HFile, test_ep_h_file);
    REGISTER_TEST(MoveGen, EP_DiscoversCheck, test_ep_discovers_check);
    REGISTER_TEST(MoveGen, EP_IllegalPin, test_ep_illegal_pin);

    REGISTER_TEST(MoveGen, Castling_BothSides, test_castling_both_sides);
    REGISTER_TEST(MoveGen, Castling_BlockedKingside, test_castling_blocked_kingside);
    REGISTER_TEST(MoveGen, Castling_ThroughCheck, test_castling_through_check);
    REGISTER_TEST(MoveGen, Castling_F1Attacked, test_castling_f1_attacked);
    REGISTER_TEST(MoveGen, Castling_B1AttackedOK, test_castling_b1_attacked_ok);
    REGISTER_TEST(MoveGen, Castling_RightsAfterCapture, test_castling_rights_after_rook_capture);

    REGISTER_TEST(MoveGen, Promotion_AllTypes, test_promotion_all_types);
    REGISTER_TEST(MoveGen, Promotion_Capture, test_promotion_capture);
    REGISTER_TEST(MoveGen, Promotion_Black, test_promotion_black);

    REGISTER_TEST(MoveGen, DoublePush_SetsEP, test_double_push_sets_ep);
    REGISTER_TEST(MoveGen, DoublePush_Blocked, test_double_push_blocked);
    REGISTER_TEST(MoveGen, SinglePush_Blocked, test_single_push_blocked);

    REGISTER_TEST(MoveGen, NoisyMovesOnly, test_noisy_moves_only);
    REGISTER_TEST(MoveGen, QuietMovesOnly, test_quiet_moves_only);

    REGISTER_TEST(MoveGen, MustBlockCheck, test_must_block_check);
    REGISTER_TEST(MoveGen, PinnedPiece, test_pinned_piece);
}
