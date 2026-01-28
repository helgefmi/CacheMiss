// test_uci.cpp - UCI protocol parsing tests
#include "test_framework.hpp"
#include "board.hpp"
#include "move.hpp"
#include "uci.hpp"
#include "search.hpp"

// Helper to apply UCI move
static void apply_move(Board& board, const std::string& uci) {
    Move32 move = parse_uci_move(uci, board);
    make_move(board, move);
}

// ============================================================================
// Position Command Parsing Tests
// ============================================================================

static void test_position_startpos() {
    Board board("8/8/8/8/8/8/8/8 w - - 0 1");  // Empty board
    parse_position_command("position startpos", board);

    Board expected;
    ASSERT_EQ(board.hash, expected.hash);
}

static void test_position_startpos_moves() {
    Board board;
    parse_position_command("position startpos moves e2e4 e7e5 g1f3", board);

    Board expected;
    apply_move(expected, "e2e4");
    apply_move(expected, "e7e5");
    apply_move(expected, "g1f3");

    ASSERT_EQ(board.hash, expected.hash);
}

static void test_position_fen() {
    Board board;
    parse_position_command("position fen r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w KQkq - 0 1", board);

    Board expected("r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w KQkq - 0 1");
    ASSERT_EQ(board.hash, expected.hash);
}

static void test_position_fen_moves() {
    Board board;
    parse_position_command("position fen r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w KQkq - 0 1 moves e1g1", board);

    Board expected("r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w KQkq - 0 1");
    apply_move(expected, "e1g1");

    ASSERT_EQ(board.hash, expected.hash);
}

// ============================================================================
// Go Command Parsing Tests
// ============================================================================

static void test_go_movetime() {
    Board board;
    GoParams params = parse_go_command("go movetime 5000", board, 0, 100);

    ASSERT_EQ(params.time_ms, 5000);
    ASSERT_EQ(params.normal_time_ms, 5000);
    ASSERT_FALSE(params.is_ponder);
}

static void test_go_infinite() {
    Board board;
    GoParams params = parse_go_command("go infinite", board, 0, 100);

    // Infinite should return very large time
    ASSERT_GT(params.time_ms, 100000000);
    ASSERT_FALSE(params.is_ponder);
}

static void test_go_ponder() {
    Board board;
    GoParams params = parse_go_command("go ponder wtime 60000 btime 60000", board, 0, 100);

    // Ponder mode: time_ms should be very large (infinite)
    ASSERT_GT(params.time_ms, 100000000);
    ASSERT_TRUE(params.is_ponder);
    // But normal_time_ms should have a calculated value
    ASSERT_GT(params.normal_time_ms, 0);
    ASSERT_LT(params.normal_time_ms, 60000);
}

static void test_go_time_white() {
    Board board;  // White to move
    GoParams params = parse_go_command("go wtime 60000 btime 60000", board, 0, 100);

    // In opening (moves_played < 10), expect ~50 moves remaining
    // Time = (60000 - 100) / 50 = ~1198 ms
    ASSERT_GT(params.time_ms, 500);
    ASSERT_LT(params.time_ms, 5000);
}

static void test_go_time_black() {
    Board board("rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1");  // Black to move
    GoParams params = parse_go_command("go wtime 60000 btime 30000", board, 1, 100);

    // Black has 30000ms, should use black's time
    ASSERT_GT(params.time_ms, 200);
    ASSERT_LT(params.time_ms, 3000);
}

static void test_go_time_with_increment() {
    Board board;
    GoParams params = parse_go_command("go wtime 60000 btime 60000 winc 2000 binc 2000", board, 0, 100);

    // Should add 3/4 of increment to base time
    ASSERT_GT(params.time_ms, 1500);
    ASSERT_LT(params.time_ms, 5000);
}

static void test_go_movestogo() {
    Board board;
    GoParams params = parse_go_command("go wtime 60000 btime 60000 movestogo 10", board, 20, 100);

    // With movestogo=10, time = (60000 - 100) / 10 = ~5990
    ASSERT_GT(params.time_ms, 4000);
    ASSERT_LT(params.time_ms, 7000);
}

static void test_go_move_overhead() {
    Board board;
    GoParams params1 = parse_go_command("go wtime 10000 btime 10000", board, 0, 100);
    GoParams params2 = parse_go_command("go wtime 10000 btime 10000", board, 0, 2000);

    // Higher overhead should give less time
    ASSERT_LT(params2.time_ms, params1.time_ms);
}

static void test_go_time_min_bound() {
    Board board;
    GoParams params = parse_go_command("go wtime 50 btime 50", board, 0, 0);

    // Very low time should still give at least 10ms
    ASSERT_GE(params.time_ms, 10);
}

static void test_go_critical_time() {
    Board board;
    // 100ms on clock with 100ms overhead = 0ms after subtraction
    // Should use 10ms minimum, NOT 1 second default
    GoParams params = parse_go_command("go wtime 100 btime 100", board, 0, 100);

    ASSERT_EQ(params.time_ms, 10);
    ASSERT_LT(params.time_ms, 100);  // Must NOT be 1000ms default
}

static void test_go_very_low_time() {
    Board board;
    // 200ms on clock with 100ms overhead = 100ms after subtraction
    // Time = 100/50 + 0 = 2ms, bounded to 10ms minimum
    GoParams params = parse_go_command("go wtime 200 btime 200", board, 0, 100);

    ASSERT_GE(params.time_ms, 10);
    ASSERT_LE(params.time_ms, 25);  // Max is 100/4 = 25ms
}

static void test_go_time_max_bound() {
    Board board;
    GoParams params = parse_go_command("go wtime 40000 btime 40000 movestogo 1", board, 0, 100);

    // Max should be time/4
    ASSERT_LE(params.time_ms, 10000);
}

// ============================================================================
// Time Advantage Tests
// ============================================================================

static void test_go_time_advantage() {
    Board board;
    // We have 2x opponent's time
    GoParams params = parse_go_command("go wtime 60000 btime 30000", board, 0, 100);
    GoParams baseline = parse_go_command("go wtime 60000 btime 60000", board, 0, 100);

    // Should use more time when we have advantage (sqrt(2) ≈ 1.41x)
    ASSERT_GT(params.time_ms, baseline.time_ms);
    ASSERT_LT(params.time_ms, baseline.time_ms * 2);  // But not unlimited
}

static void test_go_time_disadvantage() {
    Board board;
    // We have half opponent's time
    GoParams params = parse_go_command("go wtime 30000 btime 60000", board, 0, 100);
    GoParams baseline = parse_go_command("go wtime 30000 btime 30000", board, 0, 100);

    // Should use less time when opponent has advantage
    ASSERT_LT(params.time_ms, baseline.time_ms);
    ASSERT_GT(params.time_ms, baseline.time_ms / 2);  // But not too little
}

static void test_go_extreme_time_advantage() {
    Board board;
    // We have 10x opponent's time - should cap at 1.5x multiplier
    GoParams params = parse_go_command("go wtime 100000 btime 10000", board, 0, 100);
    GoParams baseline = parse_go_command("go wtime 100000 btime 100000", board, 0, 100);

    // Multiplier should be capped (not sqrt(10) ≈ 3.16x)
    ASSERT_LE(params.time_ms, baseline.time_ms * 2);
}

// ============================================================================
// Ponderhit Tests
// ============================================================================

static void test_ponderhit_sets_time() {
    g_search_controller.reset();

    int ponder_time_ms = 3500;
    g_search_controller.set_time_limit(ponder_time_ms);

    int retrieved = g_search_controller.get_time_limit_override();
    ASSERT_EQ(retrieved, 3500);

    g_search_controller.reset();
}

static void test_ponder_stores_normal_time() {
    Board board;
    GoParams params = parse_go_command("go ponder wtime 180000 btime 150000", board, 5, 100);

    ASSERT_TRUE(params.is_ponder);
    ASSERT_GT(params.time_ms, 1000000);  // Infinite for ponder

    // normal_time_ms should be reasonable calculated time
    ASSERT_GT(params.normal_time_ms, 1000);
    ASSERT_LT(params.normal_time_ms, 10000);
}

// Registration function
void register_uci_tests() {
    REGISTER_TEST(UCI, PositionStartpos, test_position_startpos);
    REGISTER_TEST(UCI, PositionStartposMoves, test_position_startpos_moves);
    REGISTER_TEST(UCI, PositionFen, test_position_fen);
    REGISTER_TEST(UCI, PositionFenMoves, test_position_fen_moves);

    REGISTER_TEST(UCI, GoMovetime, test_go_movetime);
    REGISTER_TEST(UCI, GoInfinite, test_go_infinite);
    REGISTER_TEST(UCI, GoPonder, test_go_ponder);
    REGISTER_TEST(UCI, GoTimeWhite, test_go_time_white);
    REGISTER_TEST(UCI, GoTimeBlack, test_go_time_black);
    REGISTER_TEST(UCI, GoTimeWithIncrement, test_go_time_with_increment);
    REGISTER_TEST(UCI, GoMovestogo, test_go_movestogo);
    REGISTER_TEST(UCI, GoMoveOverhead, test_go_move_overhead);
    REGISTER_TEST(UCI, GoTimeMinBound, test_go_time_min_bound);
    REGISTER_TEST(UCI, GoCriticalTime, test_go_critical_time);
    REGISTER_TEST(UCI, GoVeryLowTime, test_go_very_low_time);
    REGISTER_TEST(UCI, GoTimeMaxBound, test_go_time_max_bound);

    REGISTER_TEST(UCI, GoTimeAdvantage, test_go_time_advantage);
    REGISTER_TEST(UCI, GoTimeDisadvantage, test_go_time_disadvantage);
    REGISTER_TEST(UCI, GoExtremeTimeAdvantage, test_go_extreme_time_advantage);

    REGISTER_TEST(UCI, PonderhitSetsTime, test_ponderhit_sets_time);
    REGISTER_TEST(UCI, PonderStoresNormalTime, test_ponder_stores_normal_time);
}
