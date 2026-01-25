#pragma once

#include "board.hpp"
#include "move.hpp"
#include "ttable.hpp"
#include <atomic>

constexpr int MAX_PLY = 64;

// ============================================================================
// SearchController - encapsulates search control state for thread-safe access
// ============================================================================
// This class manages the stop flag and dynamic time limit that allow the UCI
// thread to control a running search. All methods are thread-safe.

class SearchController {
public:
    SearchController() = default;

    // Reset state for a new search
    void reset() {
        stop_flag_.store(false, std::memory_order_relaxed);
        time_limit_override_ms_.store(0, std::memory_order_relaxed);
    }

    // Request the search to stop (called by UCI thread)
    void request_stop() {
        stop_flag_.store(true, std::memory_order_release);
    }

    // Check if stop was requested (called by search thread)
    bool should_stop() const {
        return stop_flag_.load(std::memory_order_acquire);
    }

    // Set a new time limit (used for ponderhit to switch from infinite to real time)
    void set_time_limit(int ms) {
        time_limit_override_ms_.store(ms, std::memory_order_release);
    }

    // Get the current time limit override (0 means use the original limit)
    int get_time_limit_override() const {
        return time_limit_override_ms_.load(std::memory_order_acquire);
    }

private:
    std::atomic<bool> stop_flag_{false};
    std::atomic<int> time_limit_override_ms_{0};
};

// Global search controller instance - used by UCI and search threads
extern SearchController g_search_controller;

struct SearchResult {
    Move32 best_move;
    int score;
    int depth;
    Move32 pv[MAX_PLY];    // Principal variation line
    int pv_length = 0;      // Number of moves in PV
};

// Search for the best move with iterative deepening.
// Stops after time_limit_ms milliseconds or depth_limit (0 = unlimited).
SearchResult search(Board& board, TTable& tt, int time_limit_ms = 10000, int depth_limit = 0);
