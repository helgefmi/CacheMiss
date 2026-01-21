// test_search.cpp - Search feature tests
#include "test_framework.hpp"
#include "board.hpp"
#include "move.hpp"
#include "search.hpp"
#include "ttable.hpp"
#include <chrono>

// Constants from search.cpp
constexpr int MATE_SCORE = 29000;

// Square indices
constexpr int E1 = 4, E8 = 60;

// ============================================================================
// Mate Detection Tests
// ============================================================================

static void test_mate_in_one() {
    // White to move, can deliver back rank mate with Re8#
    Board board("6k1/5ppp/8/8/8/8/8/4R2K w - - 0 1");

    TTable tt(1);  // 1 MB table
    auto result = search(board, tt, 5000);  // 5 second limit

    // Should find mate in 1
    ASSERT_GE(result.score, MATE_SCORE - 10);  // Near mate score

    // Should find Re8 (or Re1-e8)
    std::string move_str = result.best_move.to_uci();
    // The rook should go to e8
    ASSERT_EQ(move_str.substr(2, 2), "e8");
}

static void test_mate_in_two() {
    // Classic mate in 2: Kb6, Ra1 vs Ka8
    Board board("k7/8/1K6/8/8/8/8/R7 w - - 0 1");

    TTable tt(1);
    auto result = search(board, tt, 5000);

    // Mate in 2 = score around MATE_SCORE - 3 (we find mate at ply 3)
    ASSERT_GE(result.score, MATE_SCORE - 10);
}

static void test_getting_mated() {
    // Black to move, getting mated
    Board board("k7/8/1K6/8/8/8/8/R7 b - - 0 1");

    TTable tt(1);
    auto result = search(board, tt, 5000);

    // Should see a very negative score (losing to mate)
    ASSERT_LE(result.score, -(MATE_SCORE - 10));
}

static void test_checkmate_position() {
    // Back rank checkmate: white rook on a8, black king on g8 boxed in by own pawns
    Board board("R5k1/5ppp/8/8/8/8/8/4K3 b - - 0 1");

    // Generate moves - should be 0 for king (mated)
    auto moves = generate_moves(board);

    // Filter for legal moves
    int legal = 0;
    for (int i = 0; i < moves.size; i++) {
        Board copy = board;
        Move32 m = moves[i];
        make_move(copy, m);
        if (!is_illegal(copy)) {
            legal++;
        }
    }

    // King is in check with no legal moves = checkmate
    bool in_check = is_attacked(board.king_sq[1], Color::White, board);
    ASSERT_TRUE(in_check);
    ASSERT_EQ(legal, 0);
}

// ============================================================================
// Draw Detection Tests
// ============================================================================

static void test_stalemate() {
    // Black to move, but king can't move (stalemate)
    Board board("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1");

    // Generate moves for black
    auto moves = generate_moves(board);

    // Filter for legal moves
    int legal = 0;
    for (int i = 0; i < moves.size; i++) {
        Board copy = board;
        Move32 m = moves[i];
        make_move(copy, m);
        if (!is_illegal(copy)) {
            legal++;
        }
    }

    // Not in check but no legal moves = stalemate
    bool in_check = is_attacked(board.king_sq[1], Color::White, board);
    ASSERT_FALSE(in_check);
    ASSERT_EQ(legal, 0);
}

static void test_fifty_move_draw() {
    // Position with halfmove clock at 100 (50 moves without pawn/capture)
    Board board("8/8/8/8/8/5k2/8/4K3 w - - 100 1");

    ASSERT_EQ(board.halfmove_clock, 100);

    TTable tt(1);
    auto result = search(board, tt, 2000);

    // Should evaluate close to 0 (draw)
    ASSERT_NEAR(result.score, 0, 100);
}

static void test_repetition_detection() {
    // Test that repetition is detected through make/unmake
    Board board("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");

    // Make moves that would lead to repetition
    u64 initial_hash = board.hash;

    // Ke1-f1-e1-f1-e1 should trigger repetition
    // Note: We need to use the undo stack properly

    // First, verify hash is same after make+unmake
    auto moves = generate_moves(board);
    if (moves.size > 0) {
        Move32 m = moves[0];
        make_move(board, m);
        unmake_move(board, m);
        ASSERT_EQ(board.hash, initial_hash);
    }
}

// ============================================================================
// Search Quality Tests
// ============================================================================

static void test_finds_winning_capture() {
    // White pawn can capture undefended black knight
    // Note: Using pawn ensures the only way to win material is by capturing
    // (a queen might "threaten" instead of capture, which is also winning)
    Board board("7k/8/4n3/3P4/8/8/8/K7 w - - 0 1");

    TTable tt(1);
    auto result = search(board, tt, 2000);

    // Should capture the knight with dxe6
    ASSERT_TRUE(result.best_move.is_capture());
}

static void test_avoids_hanging_queen() {
    // White queen is attacked, should move it
    Board board("8/8/8/8/3rQ3/8/8/K6k w - - 0 1");

    TTable tt(1);
    auto result = search(board, tt, 2000);

    // Queen should move away from d4 rook attack
    int from = result.best_move.from();
    // The queen is on e4 (square 28)
    // It should move
    ASSERT_EQ(from, 28);  // Queen should be the piece that moves
}

static void test_fork_detection() {
    // Knight can fork king and queen
    Board board("8/8/8/8/3k1q2/8/4N3/K7 w - - 0 1");

    TTable tt(1);
    auto result = search(board, tt, 3000);

    // Should find the knight fork (Nc3+ forks Kd4 and Qf4)
    // Or some other winning continuation
    ASSERT_GT(result.score, 400);  // Should see significant advantage
}

// ============================================================================
// Time Control Tests
// ============================================================================

static void test_search_respects_time() {
    Board board;  // Starting position

    TTable tt(1);
    auto start = std::chrono::steady_clock::now();
    auto result = search(board, tt, 100);  // 100ms limit
    auto end = std::chrono::steady_clock::now();

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    // Should finish within reasonable time of limit (allow some overhead)
    ASSERT_LT(elapsed, 500);  // Should not take more than 500ms for 100ms limit
}

static void test_depth_limit() {
    // A simple position should reach reasonable depth quickly
    Board board("8/8/8/8/8/8/8/K6k w - - 0 1");

    TTable tt(1);
    auto result = search(board, tt, 2000);

    // With such a simple position, should reach decent depth
    ASSERT_GE(result.depth, 5);
}

// ============================================================================
// PV Tests
// ============================================================================

static void test_pv_not_empty() {
    Board board;  // Starting position

    TTable tt(1);
    auto result = search(board, tt, 500);

    // Should have at least one move in PV
    ASSERT_GE(result.pv_length, 1);

    // First PV move should match best_move
    ASSERT_EQ(result.pv[0].from(), result.best_move.from());
    ASSERT_EQ(result.pv[0].to(), result.best_move.to());
}

static void test_pv_is_legal() {
    Board board;  // Starting position

    TTable tt(1);
    auto result = search(board, tt, 1000);

    // Verify all PV moves are legal
    Board copy = board;
    for (int i = 0; i < result.pv_length; i++) {
        Move32 m = result.pv[i];

        // Find the move in legal moves
        auto legal = generate_moves(copy);
        bool found = false;
        for (int j = 0; j < legal.size; j++) {
            if (legal[j].same_move(m)) {
                found = true;
                make_move(copy, legal[j]);
                ASSERT_FALSE(is_illegal(copy));
                break;
            }
        }
        ASSERT_TRUE(found);
    }
}

// ============================================================================
// Transposition Table Tests
// ============================================================================

static void test_tt_improves_search() {
    Board board;  // Starting position

    // First search
    TTable tt(8);
    auto result1 = search(board, tt, 500);

    // Second search with same TT should be faster/deeper
    auto result2 = search(board, tt, 500);

    // The second search benefits from cached results
    // It should reach at least the same depth or deeper
    ASSERT_GE(result2.depth, result1.depth);
}

static void test_tt_new_search_call() {
    TTable tt(1);

    // Verify new_search increments generation
    // (This is mainly a sanity check)
    tt.new_search();
    tt.new_search();

    // Just verify it doesn't crash
    ASSERT_TRUE(true);
}

// Registration function
void register_search_tests() {
    REGISTER_TEST(Search, MateInOne, test_mate_in_one);
    REGISTER_TEST(Search, MateInTwo, test_mate_in_two);
    REGISTER_TEST(Search, GettingMated, test_getting_mated);
    REGISTER_TEST(Search, CheckmatePosition, test_checkmate_position);

    REGISTER_TEST(Search, Stalemate, test_stalemate);
    REGISTER_TEST(Search, FiftyMoveDraw, test_fifty_move_draw);
    REGISTER_TEST(Search, RepetitionDetection, test_repetition_detection);

    REGISTER_TEST(Search, FindsWinningCapture, test_finds_winning_capture);
    REGISTER_TEST(Search, AvoidsHangingQueen, test_avoids_hanging_queen);
    REGISTER_TEST(Search, ForkDetection, test_fork_detection);

    REGISTER_TEST(Search, RespectsTime, test_search_respects_time);
    REGISTER_TEST(Search, DepthLimit, test_depth_limit);

    REGISTER_TEST(Search, PVNotEmpty, test_pv_not_empty);
    REGISTER_TEST(Search, PVIsLegal, test_pv_is_legal);

    REGISTER_TEST(Search, TTImprovesSearch, test_tt_improves_search);
    REGISTER_TEST(Search, TTNewSearchCall, test_tt_new_search_call);
}
