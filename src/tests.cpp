#include "tests.hpp"
#include "board.hpp"
#include "search.hpp"
#include "move.hpp"
#include "ttable.hpp"
#include <iostream>
#include <cmath>

// Helper to apply UCI move
static void apply_move(Board& board, const std::string& uci) {
    Move32 move = parse_uci_move(uci, board);
    make_move(board, move);
}

// Test: Engine finds perpetual check when losing
// Position where White is down a queen but can force perpetual
static bool test_perpetual_saves_game() {
    // White: Kb1, Qc3
    // Black: Ka8, Qh1 (Black up a queen equivalent in material pressure)
    // White can perpetual with Qa5+ Kb8 Qb5+ Ka8 Qa5+ etc.
    Board board("k7/8/8/8/8/2Q5/8/1K5q w - - 0 1");

    // Play out: Qa5+ Kb8 Qb5+ Ka8 - now White should play Qa5+ again (repetition)
    apply_move(board, "c3a5");  // Qa5+
    apply_move(board, "a8b8");  // Kb8
    apply_move(board, "a5b5");  // Qb5+
    apply_move(board, "b8a8");  // Ka8

    // Now it's White's turn. Qa5+ creates repetition (draw)
    // Without repetition detection, White might play something else and lose
    TTable tt(64);
    auto result = search(board, tt, 500);

    // Engine should find perpetual (score near 0)
    // The move should be Qa5+ (b5a5) or any check that maintains the perpetual
    bool score_is_draw = std::abs(result.score) < 100;
    return score_is_draw;
}

// Test: Engine avoids repetition when winning
// KQ vs K endgame - engine must make progress, not shuffle
static bool test_avoid_repetition_when_winning() {
    Board board("8/8/8/4k3/8/8/1Q6/4K3 w - - 0 1");

    // Play some moves to set up potential repetition trap
    apply_move(board, "b2b5");  // Qb5+
    apply_move(board, "e5e6");  // Ke6
    apply_move(board, "b5b6");  // Qb6+
    apply_move(board, "e6e5");  // Ke5

    // Now if White plays Qb5+ again, that's getting close to repetition
    // Engine should find a winning line instead
    TTable tt(64);
    auto result = search(board, tt, 1000);

    // Score should be clearly winning (not near-draw)
    return result.score > 500;
}

// Test: 50-move rule detection
// Position with halfmove_clock = 100 should evaluate as draw
static bool test_50_move_rule_draw() {
    // KR vs K position, but halfmove_clock is 100 (drawn)
    Board board("8/8/8/4k3/8/8/8/4K2R w - - 100 51");

    TTable tt(64);
    auto result = search(board, tt, 500);

    // With halfmove_clock >= 100, position is drawn
    // Score should be near 0
    return std::abs(result.score) < 100;
}

// Test: Repetition detection respects irreversible moves
// After a capture, previous positions don't count for repetition
static bool test_repetition_resets_on_capture() {
    Board board("8/8/8/4k3/8/3p4/1Q6/4K3 w - - 0 1");

    // Set up a position, then capture resets history
    apply_move(board, "b2b5");  // Qb5+
    apply_move(board, "e5e6");  // Ke6
    apply_move(board, "b5d3");  // Qxd3 (capture!)

    // halfmove_clock should be 0 now after capture
    if (board.halfmove_clock != 0) {
        return false;
    }

    apply_move(board, "e6e5");  // Ke5

    // Position is KQ vs K now, clearly winning
    // The key test is that halfmove_clock reset, which we verified above
    TTable tt(64);
    auto result = search(board, tt, 200);

    // Should be winning - KQ vs K is easily winning
    return result.score > 500;
}

// Test: Search tree repetition
// Engine should not play into an immediate repetition in the search tree
static bool test_search_tree_repetition() {
    // Simple position where both sides can shuffle
    Board board("8/8/8/8/8/k7/8/KQ6 w - - 0 1");

    // Set up moves that create a cycle
    apply_move(board, "b1b3");  // Qb3+
    apply_move(board, "a3a4");  // Ka4
    apply_move(board, "b3b4");  // Qb4+
    apply_move(board, "a4a3");  // Ka3

    // If White plays Qb3+ again, that's repetition
    // Engine should find a different winning move
    TTable tt(64);
    auto result = search(board, tt, 1000);

    // With good eval, engine should prefer winning over drawing
    return result.score > 300;
}

int run_draw_tests(int time_limit_ms, size_t mem_mb) {
    (void)time_limit_ms;  // Tests use their own timing
    (void)mem_mb;

    int failures = 0;

    auto run = [&](const char* name, bool (*test)()) {
        std::cout << "  " << name << "... " << std::flush;
        bool passed = test();
        std::cout << (passed ? "OK" : "FAIL") << std::endl;
        if (!passed) failures++;
    };

    std::cout << "=== Draw Detection Tests ===" << std::endl;
    run("perpetual_saves_game", test_perpetual_saves_game);
    run("avoid_repetition_when_winning", test_avoid_repetition_when_winning);
    run("50_move_rule_draw", test_50_move_rule_draw);
    run("repetition_resets_on_capture", test_repetition_resets_on_capture);
    run("search_tree_repetition", test_search_tree_repetition);

    std::cout << "=== " << (failures == 0 ? "All tests passed!" : "Some tests FAILED")
              << " ===" << std::endl;

    return failures;
}
