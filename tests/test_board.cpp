// test_board.cpp - Board state and make/unmake invariant tests
#include "test_framework.hpp"
#include "board.hpp"
#include "move.hpp"
#include "zobrist.hpp"
#include <cstring>

// Helper to compare boards for equality
static bool boards_equal(const Board& a, const Board& b) {
    if (a.turn != b.turn) return false;
    if (a.ep_file != b.ep_file) return false;
    if (a.castling != b.castling) return false;
    if (a.halfmove_clock != b.halfmove_clock) return false;
    if (a.hash != b.hash) return false;
    if (a.pawn_key != b.pawn_key) return false;
    if (a.phase != b.phase) return false;
    if (a.all_occupied != b.all_occupied) return false;

    for (int c = 0; c < 2; c++) {
        if (a.occupied[c] != b.occupied[c]) return false;
        if (a.king_sq[c] != b.king_sq[c]) return false;
        for (int p = 0; p < 6; p++) {
            if (a.pieces[c][p] != b.pieces[c][p]) return false;
        }
    }

    for (int sq = 0; sq < 64; sq++) {
        if (a.pieces_on_square[sq] != b.pieces_on_square[sq]) return false;
    }

    return true;
}

// ============================================================================
// Make/Unmake Invariant Tests
// ============================================================================

static void test_unmake_restores_state() {
    Board board;  // Starting position
    Board original = board;

    auto moves = generate_moves(board);
    for (int i = 0; i < moves.size; i++) {
        Move32 m = moves[i];
        UndoInfo undo = make_move(board, m);
        unmake_move(board, m, undo);

        ASSERT_TRUE(boards_equal(board, original));
    }
}

static void test_unmake_after_capture() {
    Board board("r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 0 1");
    Board original = board;

    // Find a capture
    auto moves = generate_moves(board);
    for (int i = 0; i < moves.size; i++) {
        if (moves[i].is_capture()) {
            Move32 m = moves[i];
            UndoInfo undo = make_move(board, m);
            unmake_move(board, m, undo);

            ASSERT_TRUE(boards_equal(board, original));
        }
    }
}

static void test_unmake_after_castling() {
    Board board("r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w KQkq - 0 1");
    Board original = board;

    // Find castling moves
    auto moves = generate_moves(board);
    for (int i = 0; i < moves.size; i++) {
        if (moves[i].is_castling()) {
            Move32 m = moves[i];
            UndoInfo undo = make_move(board, m);
            unmake_move(board, m, undo);

            ASSERT_TRUE(boards_equal(board, original));
        }
    }
}

static void test_unmake_after_en_passant() {
    Board board("rnbqkbnr/ppp1p1pp/8/3pPp2/8/8/PPPP1PPP/RNBQKBNR w KQkq d6 0 1");
    Board original = board;

    // Find EP capture
    auto moves = generate_moves(board);
    for (int i = 0; i < moves.size; i++) {
        if (moves[i].is_en_passant()) {
            Move32 m = moves[i];
            UndoInfo undo = make_move(board, m);
            unmake_move(board, m, undo);

            ASSERT_TRUE(boards_equal(board, original));
        }
    }
}

static void test_unmake_after_promotion() {
    Board board("8/P7/8/8/8/8/8/K6k w - - 0 1");
    Board original = board;

    auto moves = generate_moves(board);
    for (int i = 0; i < moves.size; i++) {
        if (moves[i].is_promotion()) {
            Move32 m = moves[i];
            UndoInfo undo = make_move(board, m);
            unmake_move(board, m, undo);

            ASSERT_TRUE(boards_equal(board, original));
        }
    }
}

static void test_unmake_after_promotion_capture() {
    Board board("1n6/P7/8/8/8/8/8/K6k w - - 0 1");
    Board original = board;

    auto moves = generate_moves(board);
    for (int i = 0; i < moves.size; i++) {
        if (moves[i].is_promotion() && moves[i].is_capture()) {
            Move32 m = moves[i];
            UndoInfo undo = make_move(board, m);
            unmake_move(board, m, undo);

            ASSERT_TRUE(boards_equal(board, original));
        }
    }
}

// ============================================================================
// Bitboard Consistency Tests
// ============================================================================

static void test_occupied_consistency() {
    Board board;

    // occupied[c] should equal OR of all piece bitboards for color c
    for (int c = 0; c < 2; c++) {
        Bitboard computed = 0;
        for (int p = 0; p < 6; p++) {
            computed |= board.pieces[c][p];
        }
        ASSERT_EQ(board.occupied[c], computed);
    }
}

static void test_all_occupied_consistency() {
    Board board;

    // all_occupied should equal occupied[0] | occupied[1]
    ASSERT_EQ(board.all_occupied, board.occupied[0] | board.occupied[1]);
}

static void test_pieces_on_square_consistency() {
    Board board;

    // Each piece bitboard should match pieces_on_square
    for (int c = 0; c < 2; c++) {
        for (int p = 0; p < 6; p++) {
            Bitboard bb = board.pieces[c][p];
            while (bb) {
                int sq = lsb_index(bb);
                ASSERT_EQ(board.pieces_on_square[sq], static_cast<Piece>(p));
                bb &= bb - 1;
            }
        }
    }
}

static void test_king_sq_consistency() {
    Board board;

    // king_sq should match king bitboard
    for (int c = 0; c < 2; c++) {
        Bitboard king_bb = board.pieces[c][(int)Piece::King];
        ASSERT_EQ(popcount(king_bb), 1);  // Exactly one king
        ASSERT_EQ(board.king_sq[c], lsb_index(king_bb));
    }
}

// ============================================================================
// FEN Parsing Tests
// ============================================================================

static void test_starting_fen() {
    Board board;  // Default constructor uses starting position
    ASSERT_EQ(board.turn, Color::White);
    ASSERT_EQ(board.castling, 0xF);  // All castling rights
    ASSERT_EQ(board.ep_file, 8);     // No EP
    ASSERT_EQ(board.halfmove_clock, 0);
}

static void test_custom_fen() {
    Board board("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");

    // Verify castling rights
    ASSERT_EQ(board.castling, 0xF);

    // Verify turn
    ASSERT_EQ(board.turn, Color::White);
}

static void test_fen_with_ep() {
    Board board("rnbqkbnr/pppp1ppp/8/4pP2/8/8/PPPPP1PP/RNBQKBNR w KQkq e6 0 1");

    ASSERT_EQ(board.ep_file, 4);  // e-file
}

static void test_fen_black_to_move() {
    Board board("rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1");

    ASSERT_EQ(board.turn, Color::Black);
}

static void test_fen_partial_castling() {
    Board board("r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w Kq - 0 1");

    // Only white kingside and black queenside
    ASSERT_EQ(board.castling, 2 | 4);  // K=2, q=4
}

static void test_to_fen_roundtrip() {
    const char* fens[] = {
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "8/8/8/8/8/8/8/K6k w - - 0 1",
        "8/P7/8/8/8/8/8/K6k w - - 0 1"
    };

    for (const char* fen : fens) {
        Board board(fen);
        std::string output = board.to_fen();

        // Parse output FEN and compare
        Board board2(output);
        ASSERT_TRUE(boards_equal(board, board2));
    }
}

// ============================================================================
// Phase Consistency Tests
// ============================================================================

static void test_phase_after_moves() {
    Board board;
    int initial_phase = board.phase;

    // Make and unmake a quiet move
    auto moves = generate_moves(board);
    for (int i = 0; i < moves.size; i++) {
        if (!moves[i].is_capture()) {
            Move32 m = moves[i];
            UndoInfo undo = make_move(board, m);
            unmake_move(board, m, undo);

            ASSERT_EQ(board.phase, initial_phase);
            break;
        }
    }
}

static void test_phase_after_capture() {
    Board board("rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 0 1");
    int initial_phase = board.phase;

    auto moves = generate_moves(board);
    for (int i = 0; i < moves.size; i++) {
        if (moves[i].is_capture()) {
            Move32 m = moves[i];
            Piece captured = moves[i].captured();

            UndoInfo undo = make_move(board, m);

            // Phase should decrease by captured piece's phase value
            int phase_val = PHASE_VALUES[(int)captured];
            ASSERT_EQ(board.phase, initial_phase - phase_val);

            unmake_move(board, m, undo);
            ASSERT_EQ(board.phase, initial_phase);
            break;
        }
    }
}

// ============================================================================
// Null Move Tests
// ============================================================================

static void test_null_move() {
    Board board;
    u64 original_hash = board.hash;
    Color original_turn = board.turn;
    int prev_ep;

    make_null_move(board, prev_ep);

    // Turn should flip
    ASSERT_NE(board.turn, original_turn);

    // EP should be cleared
    ASSERT_EQ(board.ep_file, 8);

    unmake_null_move(board, prev_ep);

    // Everything should be restored
    ASSERT_EQ(board.hash, original_hash);
    ASSERT_EQ(board.turn, original_turn);
}

static void test_null_move_with_ep() {
    Board board("rnbqkbnr/pppp1ppp/8/4pP2/8/8/PPPPP1PP/RNBQKBNR w KQkq e6 0 1");
    u64 original_hash = board.hash;
    int prev_ep;

    ASSERT_EQ(board.ep_file, 4);

    make_null_move(board, prev_ep);

    ASSERT_EQ(board.ep_file, 8);  // EP cleared
    ASSERT_EQ(prev_ep, 4);        // Previous EP saved

    unmake_null_move(board, prev_ep);

    ASSERT_EQ(board.ep_file, 4);  // EP restored
    ASSERT_EQ(board.hash, original_hash);
}

// Registration function
void register_board_tests() {
    REGISTER_TEST(Board, UnmakeRestoresState, test_unmake_restores_state);
    REGISTER_TEST(Board, UnmakeAfterCapture, test_unmake_after_capture);
    REGISTER_TEST(Board, UnmakeAfterCastling, test_unmake_after_castling);
    REGISTER_TEST(Board, UnmakeAfterEnPassant, test_unmake_after_en_passant);
    REGISTER_TEST(Board, UnmakeAfterPromotion, test_unmake_after_promotion);
    REGISTER_TEST(Board, UnmakeAfterPromotionCapture, test_unmake_after_promotion_capture);

    REGISTER_TEST(Board, OccupiedConsistency, test_occupied_consistency);
    REGISTER_TEST(Board, AllOccupiedConsistency, test_all_occupied_consistency);
    REGISTER_TEST(Board, PiecesOnSquareConsistency, test_pieces_on_square_consistency);
    REGISTER_TEST(Board, KingSqConsistency, test_king_sq_consistency);

    REGISTER_TEST(Board, StartingFen, test_starting_fen);
    REGISTER_TEST(Board, CustomFen, test_custom_fen);
    REGISTER_TEST(Board, FenWithEP, test_fen_with_ep);
    REGISTER_TEST(Board, FenBlackToMove, test_fen_black_to_move);
    REGISTER_TEST(Board, FenPartialCastling, test_fen_partial_castling);
    REGISTER_TEST(Board, ToFenRoundtrip, test_to_fen_roundtrip);

    REGISTER_TEST(Board, PhaseAfterMoves, test_phase_after_moves);
    REGISTER_TEST(Board, PhaseAfterCapture, test_phase_after_capture);

    REGISTER_TEST(Board, NullMove, test_null_move);
    REGISTER_TEST(Board, NullMoveWithEP, test_null_move_with_ep);
}
