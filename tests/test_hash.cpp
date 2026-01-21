// test_hash.cpp - Zobrist hash consistency tests
#include "test_framework.hpp"
#include "board.hpp"
#include "move.hpp"
#include "zobrist.hpp"

// ============================================================================
// Make/Unmake Hash Consistency
// ============================================================================

static void test_hash_restored_after_unmake() {
    Board board;  // Starting position
    u64 original_hash = board.hash;

    auto moves = generate_moves(board);
    for (int i = 0; i < moves.size && i < 10; i++) {
        Move32 m = moves[i];
        make_move(board, m);
        unmake_move(board, m);

        ASSERT_EQ(board.hash, original_hash);
    }
}

static void test_hash_after_multiple_moves() {
    Board board;
    u64 original_hash = board.hash;

    // Make several moves, then unmake them all
    auto moves = generate_moves(board);
    std::vector<Move32> made_moves;

    // Make 5 moves
    for (int i = 0; i < 5 && i < moves.size; i++) {
        Move32 m = moves[i];
        // Skip if move leaves king in check
        Board copy = board;
        make_move(copy, m);
        if (is_illegal(copy)) continue;

        make_move(board, m);
        made_moves.push_back(m);

        // Generate next set of moves
        moves = generate_moves(board);
    }

    // Unmake all moves in reverse order
    for (auto it = made_moves.rbegin(); it != made_moves.rend(); ++it) {
        unmake_move(board, *it);
    }

    ASSERT_EQ(board.hash, original_hash);
}

// ============================================================================
// Same Position Same Hash
// ============================================================================

static void test_same_position_different_move_order() {
    // Reach the same position via different move orders
    // Use knight-only moves to avoid en passant state differences
    // (Double pawn pushes set ep_file which affects hash)
    Board board1;
    Board board2;

    auto parse_and_make = [](Board& b, const char* uci) {
        Move32 m = parse_uci_move(uci, b);
        make_move(b, m);
    };

    // Board 1: Nf3, Nc6, Nc3, Nf6
    parse_and_make(board1, "g1f3");
    parse_and_make(board1, "b8c6");
    parse_and_make(board1, "b1c3");
    parse_and_make(board1, "g8f6");

    // Board 2: Nc3, Nf6, Nf3, Nc6
    parse_and_make(board2, "b1c3");
    parse_and_make(board2, "g8f6");
    parse_and_make(board2, "g1f3");
    parse_and_make(board2, "b8c6");

    // Same position should have same hash
    ASSERT_EQ(board1.hash, board2.hash);
}

static void test_transposition_hash_match() {
    // Classic transposition: 1.d4 Nf6 2.c4 vs 1.c4 Nf6 2.d4
    // Note: Must end with a non-pawn move to clear ep_file, otherwise
    // path1 ends with c4 (ep on c-file) and path2 ends with d4 (ep on d-file)
    Board board1;
    Board board2;

    auto parse_and_make = [](Board& b, const char* uci) {
        Move32 m = parse_uci_move(uci, b);
        make_move(b, m);
    };

    // Path 1: d4, Nf6, c4, Nc6
    parse_and_make(board1, "d2d4");
    parse_and_make(board1, "g8f6");
    parse_and_make(board1, "c2c4");
    parse_and_make(board1, "b8c6");

    // Path 2: c4, Nf6, d4, Nc6
    parse_and_make(board2, "c2c4");
    parse_and_make(board2, "g8f6");
    parse_and_make(board2, "d2d4");
    parse_and_make(board2, "b8c6");

    ASSERT_EQ(board1.hash, board2.hash);
}

// ============================================================================
// EP Hash Tests
// ============================================================================

static void test_ep_affects_hash() {
    // Same position but with/without EP rights should have different hash
    Board with_ep("rnbqkbnr/pppp1ppp/8/4pP2/8/8/PPPPP1PP/RNBQKBNR w KQkq e6 0 1");
    Board without_ep("rnbqkbnr/pppp1ppp/8/4pP2/8/8/PPPPP1PP/RNBQKBNR w KQkq - 0 1");

    // Hashes should be different because of EP rights
    ASSERT_NE(with_ep.hash, without_ep.hash);
}

static void test_ep_hash_cleared_after_move() {
    // EP rights should be cleared after any move
    Board board("rnbqkbnr/pppp1ppp/8/4pP2/8/8/PPPPP1PP/RNBQKBNR w KQkq e6 0 1");

    ASSERT_EQ(board.ep_file, 4);  // e-file

    // Make any move that doesn't capture EP
    Move32 m = parse_uci_move("a2a3", board);
    make_move(board, m);

    // EP should be cleared
    ASSERT_EQ(board.ep_file, 8);  // No EP
}

// ============================================================================
// Castling Hash Tests
// ============================================================================

static void test_castling_rights_affect_hash() {
    // Same position, different castling rights
    Board full_rights("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
    Board no_rights("r3k2r/8/8/8/8/8/8/R3K2R w - - 0 1");

    ASSERT_NE(full_rights.hash, no_rights.hash);
}

static void test_castling_changes_hash() {
    Board board("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
    u64 hash_before = board.hash;

    // Castle kingside
    Move32 m = parse_uci_move("e1g1", board);
    make_move(board, m);

    // Hash should change (position changed + castling rights changed)
    ASSERT_NE(board.hash, hash_before);

    // After unmake, hash should be restored
    unmake_move(board, m);
    ASSERT_EQ(board.hash, hash_before);
}

static void test_rook_capture_removes_rights() {
    Board board("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");

    // White captures black's a8 rook
    Move32 m = parse_uci_move("a1a8", board);
    make_move(board, m);

    // Black should lose queenside castling rights
    ASSERT_EQ(board.castling & 4, 0);  // bit 2 = black queenside
}

// ============================================================================
// Pawn Key Tests
// ============================================================================

static void test_pawn_key_only_pawns() {
    Board board;
    u64 initial_pawn_key = board.pawn_key;

    // Knight move shouldn't change pawn key
    Move32 m = parse_uci_move("g1f3", board);
    make_move(board, m);

    ASSERT_EQ(board.pawn_key, initial_pawn_key);
}

static void test_pawn_key_changes_on_pawn_move() {
    Board board;
    u64 initial_pawn_key = board.pawn_key;

    // Pawn move should change pawn key
    Move32 m = parse_uci_move("e2e4", board);
    make_move(board, m);

    ASSERT_NE(board.pawn_key, initial_pawn_key);
}

static void test_pawn_key_restored_after_unmake() {
    Board board;
    u64 initial_pawn_key = board.pawn_key;

    Move32 m = parse_uci_move("e2e4", board);
    make_move(board, m);
    unmake_move(board, m);

    ASSERT_EQ(board.pawn_key, initial_pawn_key);
}

static void test_pawn_capture_changes_pawn_key() {
    Board board("8/8/8/3p4/4P3/8/8/K6k w - - 0 1");
    u64 initial_pawn_key = board.pawn_key;

    // exd5 - pawn captures pawn
    Move32 m = parse_uci_move("e4d5", board);
    make_move(board, m);

    // Pawn key should change (two pawns removed from original squares, one added)
    ASSERT_NE(board.pawn_key, initial_pawn_key);
}

// ============================================================================
// Compute Hash Consistency
// ============================================================================

static void test_incremental_vs_computed_hash() {
    Board board;

    // Make several moves
    const char* moves[] = {"e2e4", "e7e5", "g1f3", "b8c6", "f1b5"};
    for (const char* uci : moves) {
        Move32 m = parse_uci_move(uci, board);
        make_move(board, m);
    }

    // Compute hash from scratch
    u64 computed = compute_hash(board);

    // Should match incremental hash
    ASSERT_EQ(board.hash, computed);
}

static void test_hash_after_promotion() {
    Board board("8/P7/8/8/8/8/8/K6k w - - 0 1");
    u64 hash_before = board.hash;

    // Promote pawn
    Move32 m = parse_uci_move("a7a8q", board);
    make_move(board, m);

    // Verify incremental hash matches computed
    u64 computed = compute_hash(board);
    ASSERT_EQ(board.hash, computed);

    // Hash should have changed
    ASSERT_NE(board.hash, hash_before);

    // Unmake and verify restoration
    unmake_move(board, m);
    ASSERT_EQ(board.hash, hash_before);
}

static void test_hash_after_en_passant() {
    Board board("8/8/8/3pP3/8/8/8/K6k w - d6 0 1");
    u64 hash_before = board.hash;

    // EP capture
    Move32 m = parse_uci_move("e5d6", board);
    make_move(board, m);

    // Verify incremental hash matches computed
    u64 computed = compute_hash(board);
    ASSERT_EQ(board.hash, computed);

    // Unmake
    unmake_move(board, m);
    ASSERT_EQ(board.hash, hash_before);
}

static void test_hash_after_castling() {
    Board board("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
    u64 hash_before = board.hash;

    // Castle kingside
    Move32 m = parse_uci_move("e1g1", board);
    make_move(board, m);

    // Verify incremental hash matches computed
    u64 computed = compute_hash(board);
    ASSERT_EQ(board.hash, computed);

    // Unmake
    unmake_move(board, m);
    ASSERT_EQ(board.hash, hash_before);
}

// Registration function
void register_hash_tests() {
    REGISTER_TEST(Hash, RestoredAfterUnmake, test_hash_restored_after_unmake);
    REGISTER_TEST(Hash, AfterMultipleMoves, test_hash_after_multiple_moves);

    REGISTER_TEST(Hash, SamePositionDifferentOrder, test_same_position_different_move_order);
    REGISTER_TEST(Hash, TranspositionMatch, test_transposition_hash_match);

    REGISTER_TEST(Hash, EPAffectsHash, test_ep_affects_hash);
    REGISTER_TEST(Hash, EPHashClearedAfterMove, test_ep_hash_cleared_after_move);

    REGISTER_TEST(Hash, CastlingRightsAffectHash, test_castling_rights_affect_hash);
    REGISTER_TEST(Hash, CastlingChangesHash, test_castling_changes_hash);
    REGISTER_TEST(Hash, RookCaptureRemovesRights, test_rook_capture_removes_rights);

    REGISTER_TEST(Hash, PawnKeyOnlyPawns, test_pawn_key_only_pawns);
    REGISTER_TEST(Hash, PawnKeyChangesOnPawnMove, test_pawn_key_changes_on_pawn_move);
    REGISTER_TEST(Hash, PawnKeyRestoredAfterUnmake, test_pawn_key_restored_after_unmake);
    REGISTER_TEST(Hash, PawnCaptureChangesPawnKey, test_pawn_capture_changes_pawn_key);

    REGISTER_TEST(Hash, IncrementalVsComputed, test_incremental_vs_computed_hash);
    REGISTER_TEST(Hash, AfterPromotion, test_hash_after_promotion);
    REGISTER_TEST(Hash, AfterEnPassant, test_hash_after_en_passant);
    REGISTER_TEST(Hash, AfterCastling, test_hash_after_castling);
}
