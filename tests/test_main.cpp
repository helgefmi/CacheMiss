// test_main.cpp - Test runner with simple assert framework
#include "test_framework.hpp"
#include "zobrist.hpp"
#include <iostream>
#include <string>

// External test registration functions
void register_movegen_tests();
void register_see_tests();
void register_eval_tests();
void register_search_tests();
void register_hash_tests();
void register_board_tests();
void register_perft_tests();
void register_uci_tests();

int main(int argc, char* argv[]) {
    // Initialize zobrist hashing before any tests
    zobrist::init();

    // Parse optional filter argument
    std::string filter;
    if (argc > 1) {
        filter = argv[1];
    }

    // Register all tests
    register_movegen_tests();
    register_see_tests();
    register_eval_tests();
    register_search_tests();
    register_hash_tests();
    register_board_tests();
    register_perft_tests();
    register_uci_tests();

    // Run tests
    return TestRunner::instance().run(filter);
}
