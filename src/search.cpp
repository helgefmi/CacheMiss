#include "search.hpp"
#include "eval.hpp"
#include <chrono>
#include <iostream>

// Constants
constexpr int INFINITY_SCORE = 30000;
constexpr int MATE_SCORE = 29000;
constexpr int MAX_DEPTH = 64;

// Piece values for MVV-LVA move ordering
constexpr int MVV_LVA_VALUES[] = {
    100,   // Pawn
    320,   // Knight
    330,   // Bishop
    500,   // Rook
    900,   // Queen
    20000, // King (shouldn't be captured, but just in case)
    0,     // (unused)
    0      // None
};

// Score a move for ordering purposes
// Higher score = searched first
static int score_move(const Move32& move, const Board& board) {
    int score = 0;

    // Captures: MVV-LVA (Most Valuable Victim - Least Valuable Attacker)
    if (move.is_capture()) {
        int victim_value = MVV_LVA_VALUES[(int)move.captured()];
        int attacker_value = MVV_LVA_VALUES[(int)board.pieces_on_square[move.from()]];
        // Scale victim by 10 so even bad captures are tried before quiet moves
        score += 10000 + victim_value * 10 - attacker_value;
    }

    // Promotions (queen promotion is best)
    if (move.is_promotion()) {
        score += 9000 + MVV_LVA_VALUES[(int)move.promotion()];
    }

    return score;
}

// Pick the best move from index 'start' onwards and swap it to 'start'
static void pick_move(MoveList& moves, int start, const Board& board) {
    int best_idx = start;
    int best_score = score_move(moves[start], board);

    for (int i = start + 1; i < moves.size; ++i) {
        int score = score_move(moves[i], board);
        if (score > best_score) {
            best_score = score;
            best_idx = i;
        }
    }

    if (best_idx != start) {
        std::swap(moves[start], moves[best_idx]);
    }
}

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

// Quiescence search - only searches captures and promotions
static int quiescence(Board& board, int alpha, int beta, int ply) {
    if (check_time()) return 0;

    nodes_searched++;

    bool in_chk = in_check(board);

    // Stand-pat: evaluate the position without making any move
    // But if in check, we must make a move - can't use stand_pat
    if (!in_chk) {
        int stand_pat = evaluate(board);

        if (stand_pat >= beta) {
            return beta;
        }

        if (stand_pat > alpha) {
            alpha = stand_pat;
        }
    }

    MoveList moves = generate_moves(board);

    int legal_moves = 0;

    for (int i = 0; i < moves.size; ++i) {
        // Pick best remaining move (captures/promotions will be picked first due to high scores)
        pick_move(moves, i, board);

        Move32& move = moves[i];

        // Only search captures and promotions (but all moves if in check)
        if (!in_chk && !move.is_capture() && !move.is_promotion()) {
            continue;
        }

        make_move(board, move);

        if (is_illegal(board)) {
            unmake_move(board, move);
            continue;
        }

        legal_moves++;

        int score = -quiescence(board, -beta, -alpha, ply + 1);

        unmake_move(board, move);

        if (stop_search) return 0;

        if (score >= beta) {
            return beta;
        }

        if (score > alpha) {
            alpha = score;
        }
    }

    // If in check and no legal moves, it's checkmate
    if (in_chk && legal_moves == 0) {
        return -MATE_SCORE + ply;
    }

    return alpha;
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

    // Leaf node - enter quiescence search
    if (depth == 0) {
        return quiescence(board, alpha, beta, ply);
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
        // Pick best remaining move (skip i=0 if TT move is already there)
        if (i > 0 || tt_move.data == 0) {
            pick_move(moves, i, board);
        }

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
        // Pick best remaining move (skip i=0 if TT move is already there)
        if (i > 0 || tt_move.data == 0) {
            pick_move(moves, i, board);
        }

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
                  << " pv " << move.to_uci()
                  << std::endl;

        // Early exit if we found a mate
        if (score >= MATE_SCORE - MAX_DEPTH || score <= -MATE_SCORE + MAX_DEPTH) {
            break;
        }
    }

    // Note: bestmove is output by caller (UCI loop) to avoid duplication

    return result;
}
