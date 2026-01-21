// test_perft.cpp - Perft tests for move generation correctness
#include "test_framework.hpp"
#include "board.hpp"
#include "move.hpp"
#include "perft.hpp"

// ============================================================================
// Starting Position Perft Tests
// ============================================================================

static void test_perft_start_d1() {
    Board board;
    PerftTable pt(1);
    u64 nodes = perft(board, 1, &pt);
    ASSERT_EQ(nodes, 20);  // 20 legal moves from starting position
}

static void test_perft_start_d2() {
    Board board;
    PerftTable pt(1);
    u64 nodes = perft(board, 2, &pt);
    ASSERT_EQ(nodes, 400);
}

static void test_perft_start_d3() {
    Board board;
    PerftTable pt(1);
    u64 nodes = perft(board, 3, &pt);
    ASSERT_EQ(nodes, 8902);
}

static void test_perft_start_d4() {
    Board board;
    PerftTable pt(1);
    u64 nodes = perft(board, 4, &pt);
    ASSERT_EQ(nodes, 197281);
}

// ============================================================================
// Kiwipete Position (complex position with many edge cases)
// ============================================================================

static void test_perft_kiwipete_d1() {
    Board board("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
    PerftTable pt(1);
    u64 nodes = perft(board, 1, &pt);
    ASSERT_EQ(nodes, 48);
}

static void test_perft_kiwipete_d2() {
    Board board("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
    PerftTable pt(1);
    u64 nodes = perft(board, 2, &pt);
    ASSERT_EQ(nodes, 2039);
}

static void test_perft_kiwipete_d3() {
    Board board("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
    PerftTable pt(1);
    u64 nodes = perft(board, 3, &pt);
    ASSERT_EQ(nodes, 97862);
}

// ============================================================================
// En Passant Position
// ============================================================================

static void test_perft_ep_position_d1() {
    Board board("8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1");
    PerftTable pt(1);
    u64 nodes = perft(board, 1, &pt);
    ASSERT_EQ(nodes, 14);
}

static void test_perft_ep_position_d2() {
    Board board("8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1");
    PerftTable pt(1);
    u64 nodes = perft(board, 2, &pt);
    ASSERT_EQ(nodes, 191);
}

static void test_perft_ep_position_d3() {
    Board board("8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1");
    PerftTable pt(1);
    u64 nodes = perft(board, 3, &pt);
    ASSERT_EQ(nodes, 2812);
}

// ============================================================================
// Promotion Position
// ============================================================================

static void test_perft_promotion_d1() {
    Board board("n1n5/PPPk4/8/8/8/8/4Kppp/5N1N b - - 0 1");
    PerftTable pt(1);
    u64 nodes = perft(board, 1, &pt);
    ASSERT_EQ(nodes, 24);
}

static void test_perft_promotion_d2() {
    Board board("n1n5/PPPk4/8/8/8/8/4Kppp/5N1N b - - 0 1");
    PerftTable pt(1);
    u64 nodes = perft(board, 2, &pt);
    ASSERT_EQ(nodes, 496);
}

static void test_perft_promotion_d3() {
    Board board("n1n5/PPPk4/8/8/8/8/4Kppp/5N1N b - - 0 1");
    PerftTable pt(1);
    u64 nodes = perft(board, 3, &pt);
    ASSERT_EQ(nodes, 9483);
}

// Registration function
void register_perft_tests() {
    REGISTER_TEST(Perft, StartD1, test_perft_start_d1);
    REGISTER_TEST(Perft, StartD2, test_perft_start_d2);
    REGISTER_TEST(Perft, StartD3, test_perft_start_d3);
    REGISTER_TEST(Perft, StartD4, test_perft_start_d4);

    REGISTER_TEST(Perft, KiwipeteD1, test_perft_kiwipete_d1);
    REGISTER_TEST(Perft, KiwipeteD2, test_perft_kiwipete_d2);
    REGISTER_TEST(Perft, KiwipeteD3, test_perft_kiwipete_d3);

    REGISTER_TEST(Perft, EPPositionD1, test_perft_ep_position_d1);
    REGISTER_TEST(Perft, EPPositionD2, test_perft_ep_position_d2);
    REGISTER_TEST(Perft, EPPositionD3, test_perft_ep_position_d3);

    REGISTER_TEST(Perft, PromotionD1, test_perft_promotion_d1);
    REGISTER_TEST(Perft, PromotionD2, test_perft_promotion_d2);
    REGISTER_TEST(Perft, PromotionD3, test_perft_promotion_d3);
}
