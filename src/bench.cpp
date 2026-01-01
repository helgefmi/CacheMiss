#include "bench.hpp"
#include "board.hpp"
#include "epd.hpp"
#include "perft.hpp"
#include <chrono>
#include <iomanip>
#include <iostream>

void bench_perftsuite(const std::string& filename, int max_depth) {
    auto entries = parse_epd_file(filename);

    if (entries.empty()) {
        std::cerr << "Failed to open or parse: " << filename << '\n';
        return;
    }

    std::cout << "Running perft suite: " << filename << '\n';
    std::cout << "Positions: " << entries.size() << '\n';
    if (max_depth > 0) {
        std::cout << "Max depth: " << max_depth << '\n';
    }
    std::cout << '\n';

    int passed = 0;
    int failed = 0;
    u64 total_nodes = 0;

    auto suite_start = std::chrono::steady_clock::now();

    for (size_t i = 0; i < entries.size(); i++) {
        const auto& entry = entries[i];
        Board board(entry.fen);

        std::cout << "[" << (i + 1) << "/" << entries.size() << "] " << entry.fen << '\n';

        bool position_passed = true;
        int depths_to_test = static_cast<int>(entry.expected_nodes.size());
        if (max_depth > 0 && max_depth < depths_to_test) {
            depths_to_test = max_depth;
        }

        for (int d = 0; d < depths_to_test; d++) {
            int depth = d + 1;
            u64 expected = entry.expected_nodes[d];

            u64 nodes = perft(board, depth);
            total_nodes += nodes;

            auto now = std::chrono::steady_clock::now();
            auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(now - suite_start).count();
            double mnps = (elapsed_us > 0) ? (total_nodes / (elapsed_us / 1e6)) / 1e6 : 0.0;

            if (nodes == expected) {
                std::cout << "  depth " << depth << ": " << nodes << " ("
                          << std::fixed << std::setprecision(2) << mnps << " Mnps) OK\n";
            } else {
                std::cout << "  depth " << depth << ": " << nodes << " (expected " << expected << ") FAIL\n";
                position_passed = false;
                break;
            }
        }

        if (position_passed) {
            passed++;
        } else {
            failed++;
        }
    }

    auto suite_end = std::chrono::steady_clock::now();
    auto total_us = std::chrono::duration_cast<std::chrono::microseconds>(suite_end - suite_start).count();
    double total_mnps = (total_us > 0) ? (total_nodes / (total_us / 1e6)) / 1e6 : 0.0;

    std::cout << "\n=== Results ===\n";
    std::cout << "Passed: " << passed << "/" << (passed + failed) << '\n';
    std::cout << "Failed: " << failed << '\n';
    std::cout << "Total nodes: " << total_nodes << '\n';
    std::cout << "Total time: " << total_us / 1000 << " ms\n";
    std::cout << "NPS: " << std::fixed << std::setprecision(2) << total_mnps << " Mnps\n";
}
