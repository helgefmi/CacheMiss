// test_see.cpp - Static Exchange Evaluation tests
#include "test_framework.hpp"
#include "board.hpp"
#include "move.hpp"

// SEE values from move.cpp
constexpr int SEE_PAWN = 100;
constexpr int SEE_KNIGHT = 320;
constexpr int SEE_BISHOP = 330;
constexpr int SEE_ROOK = 500;
constexpr int SEE_QUEEN = 900;

// Square indices
constexpr int A1 = 0, B1 = 1, C1 = 2, D1 = 3, E1 = 4, F1 = 5, G1 = 6, H1 = 7;
constexpr int A4 = 24, B4 = 25, C4 = 26, D4 = 27, E4 = 28, F4 = 29, G4 = 30, H4 = 31;
constexpr int A5 = 32, B5 = 33, C5 = 34, D5 = 35, E5 = 36, F5 = 37, G5 = 38, H5 = 39;
constexpr int A6 = 40, B6 = 41, C6 = 42, D6 = 43, E6 = 44, F6 = 45, G6 = 46, H6 = 47;
constexpr int A7 = 48, B7 = 49, C7 = 50, D7 = 51, E7 = 52, F7 = 53, G7 = 54, H7 = 55;
constexpr int A8 = 56, B8 = 57, C8 = 58, D8 = 59, E8 = 60, F8 = 61, G8 = 62, H8 = 63;

// Helper to find a move by from/to squares
static Move32 find_move(const Board& board, int from, int to, Piece promo = Piece::None) {
    auto moves = generate_moves(board);
    for (int i = 0; i < moves.size; i++) {
        if (moves[i].from() == from && moves[i].to() == to) {
            if (promo == Piece::None || moves[i].promotion() == promo) {
                return moves[i];
            }
        }
    }
    return Move32(0);
}

// ============================================================================
// Basic SEE Tests
// ============================================================================

static void test_see_pawn_takes_queen() {
    // Pawn can capture undefended queen
    Board board("8/8/8/3q4/4P3/8/8/K6k w - - 0 1");
    auto move = find_move(board, E4, D5);
    ASSERT_TRUE(move);

    int result = see(board, move);
    // Pawn takes queen: +900
    ASSERT_EQ(result, SEE_QUEEN);
}

static void test_see_knight_takes_knight() {
    // Equal trade: NxN
    Board board("8/8/3n4/4N3/8/8/8/K6k w - - 0 1");
    auto move = find_move(board, E5, D6);

    // Capture is D6, not valid in this position. Let me fix:
    // Nxd6 - knight on e5, black knight on d6? But that's rank 6, e5 knight can't reach d6
    // Let me use a correct position
}

static void test_see_equal_knight_trade() {
    // Knight takes knight, defended by another knight
    Board board("8/8/8/3n4/2N5/8/8/K6k w - - 0 1");
    // White knight c4 can take black knight d5? No, knight can't reach adjacent diagonal
    // Let me use: Nc4 attacks d6, e5, b6, a5, b2, d2, a3, e3
    // Black knight on e5: Nc4xe5
}

static void test_see_nxn_equal() {
    // White knight on d4, black knight on e6 defended by knight on d8
    // Note: Knight on e7 can't reach e6 (not a valid knight move)
    // Knight on d8 CAN reach e6 (2 squares forward, 1 right)
    Board board("3n4/8/4n3/8/3N4/8/8/K6k w - - 0 1");
    auto move = find_move(board, D4, E6);
    ASSERT_TRUE(move);

    int result = see(board, move);
    // Nxe6, Nxe6 -> equal trade, net 0
    ASSERT_EQ(result, 0);
}

static void test_see_queen_takes_defended_pawn() {
    // Queen takes pawn defended by pawn - loses queen to pawn
    Board board("8/8/2p5/3p4/4Q3/8/8/K6k w - - 0 1");
    auto move = find_move(board, E4, D5);
    ASSERT_TRUE(move);

    int result = see(board, move);
    // Qxd5, cxd5 -> Q(900) - P(100) = -800 net
    ASSERT_EQ(result, SEE_PAWN - SEE_QUEEN);  // 100 - 900 = -800
}

static void test_see_rook_battery() {
    // Two rooks - x-ray should be handled
    Board board("r7/8/8/8/4n3/8/8/R3K2k w - - 0 1");
    // Ra1 attacks e4 with Ra8 behind... wait, that's on same file but Ra8 is black
    // Let me try: White rooks on a1 and a8, black knight on a4
}

static void test_see_rook_xray() {
    // White rooks on h1 and h8, black knight on h4
    Board board("7R/8/8/8/7n/8/8/K5kR w - - 0 1");
    auto move = find_move(board, H1, H4);
    ASSERT_TRUE(move);

    int result = see(board, move);
    // Rxh4 captures knight (320), black has no recapture visible
    // But h8 rook is through the h1 rook? Actually x-ray means when h1 rook leaves,
    // h8 rook can attack through. But h8 rook is our own.
    // So: Rxh4 +320, no recapture -> +320
    ASSERT_EQ(result, SEE_KNIGHT);
}

static void test_see_rook_takes_defended_knight() {
    // Rook takes knight defended by pawn
    Board board("8/8/5p2/4n3/8/8/8/K3R2k w - - 0 1");
    auto move = find_move(board, E1, E5);
    ASSERT_TRUE(move);

    int result = see(board, move);
    // Rxe5, fxe5 -> gain knight (320), lose rook (500) = -180
    ASSERT_EQ(result, SEE_KNIGHT - SEE_ROOK);
}

static void test_see_en_passant() {
    // En passant capture
    Board board("8/8/8/3Pp3/8/8/8/K6k w - e6 0 1");
    auto move = find_move(board, D5, E6);
    ASSERT_TRUE(move);
    ASSERT_TRUE(move.is_en_passant());

    int result = see(board, move);
    // dxe6 e.p. captures pawn: +100
    ASSERT_EQ(result, SEE_PAWN);
}

static void test_see_promotion_capture() {
    // Pawn promotes while capturing
    Board board("1r6/P7/8/8/8/8/8/K6k w - - 0 1");
    auto move = find_move(board, A7, B8, Piece::Queen);
    ASSERT_TRUE(move);

    int result = see(board, move);
    // axb8=Q captures rook (500), promotes to queen (900-100=+800 net piece value)
    // Total: 500 + 800 = 1300
    // Wait, SEE calculates: gain = captured_value + (promo_value - pawn_value)
    // gain = 500 + (900 - 100) = 1300
    // If undefended: net = 1300
    ASSERT_EQ(result, SEE_ROOK + SEE_QUEEN - SEE_PAWN);
}

static void test_see_defended_promotion() {
    // Pawn promotes, but rook is defended
    Board board("rr6/P7/8/8/8/8/8/K6k w - - 0 1");
    auto move = find_move(board, A7, B8, Piece::Queen);
    ASSERT_TRUE(move);

    int result = see(board, move);
    // axb8=Q+ captures rook (500), becomes queen (+800)
    // Then Rxb8 captures queen (900)
    // Net: 500 + 800 - 900 = 400
    ASSERT_EQ(result, SEE_ROOK + SEE_QUEEN - SEE_PAWN - SEE_QUEEN);  // 500 + 800 - 900 = 400
}

static void test_see_undefended_piece() {
    // Simple capture of undefended piece
    Board board("8/8/8/8/4b3/8/8/K3R2k w - - 0 1");
    auto move = find_move(board, E1, E4);
    ASSERT_TRUE(move);

    int result = see(board, move);
    // Rxe4 captures bishop: +330
    ASSERT_EQ(result, SEE_BISHOP);
}

static void test_see_complex_exchange() {
    // Multiple pieces attacking and defending
    // White: Rook on e1, Knight on d2 (can reach e4 via d2-e4 knight move)
    // Black: Bishop on e4, Pawn on d5 defending
    // Note: Original had knight on f3, but f3 can't reach e4 (not a valid knight move)
    Board board("8/8/8/3p4/4b3/8/3N4/K3R2k w - - 0 1");
    auto move = find_move(board, E1, E4);
    ASSERT_TRUE(move);

    int result = see(board, move);
    // Rxe4: +330 (capture bishop)
    // dxe4: we lose rook (500), so running total: 330 - 500 = -170
    // Nxe4: capture pawn (100), running total: -170 + 100 = -70
    ASSERT_EQ(result, SEE_BISHOP - SEE_ROOK + SEE_PAWN);
}

static void test_see_losing_capture() {
    // Clearly losing capture: pawn takes defended rook
    Board board("8/8/8/3r4/4P3/3R4/8/K6k w - - 0 1");
    // exd5, Rxd5 -> +500 - 100 = ???
    // No wait, pawn takes rook: gain 500
    // Our rook recaptures? No, black rook on d5 takes our pawn
    // Let me redo: white pawn e4, black rook d5, white rook d3
    // After exd5, what attacks d5? Only white rook d3
    // So: exd5 +500, black has nothing to recapture. Net: +500
}

static void test_see_pawn_takes_defended_rook() {
    // Pawn takes rook defended by rook
    Board board("3r4/8/8/3r4/4P3/8/8/K6k w - - 0 1");
    auto move = find_move(board, E4, D5);
    ASSERT_TRUE(move);

    int result = see(board, move);
    // exd5: +500, Rxd5: -100 (we lose pawn). Net: 500 - 100 = 400
    ASSERT_EQ(result, SEE_ROOK - SEE_PAWN);
}

static void test_see_bishop_takes_bishop() {
    // Equal trade: BxB
    Board board("8/8/3b4/8/5B2/8/8/K6k w - - 0 1");
    auto move = find_move(board, F4, D6);
    ASSERT_TRUE(move);

    int result = see(board, move);
    // Bxd6, no defender: +330
    ASSERT_EQ(result, SEE_BISHOP);
}

static void test_see_bad_bishop_trade() {
    // Bishop takes pawn, defended by bishop
    Board board("8/8/2b5/3p4/4B3/8/8/K6k w - - 0 1");
    auto move = find_move(board, E4, D5);
    ASSERT_TRUE(move);

    int result = see(board, move);
    // Bxd5, Bxd5: +100 (pawn) - 330 (bishop) = -230
    ASSERT_EQ(result, SEE_PAWN - SEE_BISHOP);
}

// Registration function
void register_see_tests() {
    REGISTER_TEST(SEE, PawnTakesQueen, test_see_pawn_takes_queen);
    REGISTER_TEST(SEE, NxN_Equal, test_see_nxn_equal);
    REGISTER_TEST(SEE, QueenTakesDefendedPawn, test_see_queen_takes_defended_pawn);
    REGISTER_TEST(SEE, RookXray, test_see_rook_xray);
    REGISTER_TEST(SEE, RookTakesDefendedKnight, test_see_rook_takes_defended_knight);
    REGISTER_TEST(SEE, EnPassant, test_see_en_passant);
    REGISTER_TEST(SEE, PromotionCapture, test_see_promotion_capture);
    REGISTER_TEST(SEE, DefendedPromotion, test_see_defended_promotion);
    REGISTER_TEST(SEE, UndefendedPiece, test_see_undefended_piece);
    REGISTER_TEST(SEE, ComplexExchange, test_see_complex_exchange);
    REGISTER_TEST(SEE, PawnTakesDefendedRook, test_see_pawn_takes_defended_rook);
    REGISTER_TEST(SEE, BishopTakesBishop, test_see_bishop_takes_bishop);
    REGISTER_TEST(SEE, BadBishopTrade, test_see_bad_bishop_trade);
}
