#include "search.hpp"
#include "eval.hpp"
#include <chrono>
#include <iostream>

// Constants
constexpr int INFINITY_SCORE = 30000;
constexpr int MATE_SCORE = 29000;
constexpr int MAX_DEPTH = 64;

// Search state
static std::chrono::steady_clock::time_point start_time;
static int time_limit;
static bool stop_search;
static u64 nodes_searched;

static bool check_time() {
    if ((nodes_searched & 2047) == 0) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();
        if (elapsed >= time_limit) {
            stop_search = true;
        }
    }
    return stop_search;
}

// Check if current side is in check
static bool in_check(const Board& board) {
    Color us = board.turn;
    Color them = opposite(us);
    return is_attacked(board.king_sq[(int)us], them, board);
}

static int alpha_beta(Board& board, TTable& tt, int depth, int alpha, int beta, int ply) {
    if (check_time()) return 0;

    nodes_searched++;

    // TT probe
    int tt_score;
    Move32 tt_move(0);
    if (tt.probe(board.hash, depth, alpha, beta, tt_score, tt_move)) {
        return tt_score;
    }

    // Leaf node
    if (depth == 0) {
        return evaluate(board);
    }

    MoveList moves = generate_moves(board);

    // Try TT move first if available
    if (tt_move.data != 0) {
        for (int i = 0; i < moves.size; ++i) {
            if (moves[i].same_move(tt_move)) {
                // Swap to front
                Move32 tmp = moves[0];
                moves[0] = moves[i];
                moves[i] = tmp;
                break;
            }
        }
    }

    int best_score = -INFINITY_SCORE;
    Move32 best_move(0);
    int legal_moves = 0;

    for (int i = 0; i < moves.size; ++i) {
        Move32& move = moves[i];

        make_move(board, move);

        if (is_illegal(board)) {
            unmake_move(board, move);
            continue;
        }

        legal_moves++;

        int score = -alpha_beta(board, tt, depth - 1, -beta, -alpha, ply + 1);

        unmake_move(board, move);

        if (stop_search) return 0;

        if (score > best_score) {
            best_score = score;
            best_move = move;
        }

        if (score >= beta) {
            tt.store(board.hash, depth, beta, TT_LOWER, move);
            return beta;
        }

        if (score > alpha) {
            alpha = score;
        }
    }

    // No legal moves: checkmate or stalemate
    if (legal_moves == 0) {
        if (in_check(board)) {
            return -MATE_SCORE + ply;  // Checkmate
        }
        return 0;  // Stalemate
    }

    // Store in TT
    TTFlag flag = (best_score > alpha - 1) ? TT_EXACT : TT_UPPER;
    tt.store(board.hash, depth, alpha, flag, best_move);

    return alpha;
}

// Root search - returns best move and score
static std::pair<Move32, int> search_root(Board& board, TTable& tt, int depth) {
    MoveList moves = generate_moves(board);

    // Try TT move first if available
    int tt_score;
    Move32 tt_move(0);
    tt.probe(board.hash, depth, -INFINITY_SCORE, INFINITY_SCORE, tt_score, tt_move);

    if (tt_move.data != 0) {
        for (int i = 0; i < moves.size; ++i) {
            if (moves[i].same_move(tt_move)) {
                std::swap(moves[0], moves[i]);
                break;
            }
        }
    }

    int alpha = -INFINITY_SCORE;
    int beta = INFINITY_SCORE;
    Move32 best_move(0);
    int best_score = -INFINITY_SCORE;

    for (int i = 0; i < moves.size; ++i) {
        Move32& move = moves[i];

        make_move(board, move);

        if (is_illegal(board)) {
            unmake_move(board, move);
            continue;
        }

        int score = -alpha_beta(board, tt, depth - 1, -beta, -alpha, 1);

        unmake_move(board, move);

        if (stop_search) break;

        if (score > best_score) {
            best_score = score;
            best_move = move;
        }

        if (score > alpha) {
            alpha = score;
        }
    }

    // Store root position in TT
    if (!stop_search && best_move.data != 0) {
        tt.store(board.hash, depth, best_score, TT_EXACT, best_move);
    }

    return {best_move, best_score};
}

SearchResult search(Board& board, TTable& tt, int time_limit_ms) {
    start_time = std::chrono::steady_clock::now();
    time_limit = time_limit_ms;
    stop_search = false;
    nodes_searched = 0;

    SearchResult result;
    result.best_move = Move32(0);
    result.score = 0;
    result.depth = 0;

    for (int depth = 1; depth <= MAX_DEPTH; ++depth) {
        auto [move, score] = search_root(board, tt, depth);

        if (stop_search) {
            // If we completed searching at least the first move (the TT/best move from
            // previous iteration), use that information rather than discarding it
            if (move.data != 0) {
                result.best_move = move;
                result.score = score;
            }
            break;
        }

        result.best_move = move;
        result.score = score;
        result.depth = depth;

        // Print UCI-style info
        auto now = std::chrono::steady_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();

        std::cout << "info depth " << depth
                  << " score cp " << score
                  << " nodes " << nodes_searched
                  << " time " << elapsed_ms
                  << " pv " << move.to_string(board)
                  << std::endl;

        // Early exit if we found a mate
        if (score >= MATE_SCORE - MAX_DEPTH || score <= -MATE_SCORE + MAX_DEPTH) {
            break;
        }
    }

    std::cout << "bestmove " << result.best_move.to_string(board) << std::endl;

    return result;
}
