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

// Move ordering bonuses
constexpr int KILLER_SCORE_1 = 9000;   // First killer (just below promotions)
constexpr int KILLER_SCORE_2 = 8000;   // Second killer
constexpr int HISTORY_MAX = 6000;      // Cap history scores

// Killer moves: 2 per ply
static Move32 killers[MAX_DEPTH][2];

// History heuristic: indexed by [color][from][to]
static int history[2][64][64];

static void clear_killers() {
    for (int ply = 0; ply < MAX_DEPTH; ++ply) {
        killers[ply][0] = Move32(0);
        killers[ply][1] = Move32(0);
    }
}

static void clear_history() {
    for (int c = 0; c < 2; ++c) {
        for (int from = 0; from < 64; ++from) {
            for (int to = 0; to < 64; ++to) {
                history[c][from][to] = 0;
            }
        }
    }
}

static void update_killer(int ply, Move32 move) {
    // Don't store captures as killers
    if (move.is_capture()) return;

    // Don't store if already first killer
    if (killers[ply][0].same_move(move)) return;

    // Shift and insert
    killers[ply][1] = killers[ply][0];
    killers[ply][0] = move;
}

static void update_history(Color color, Move32 move, int depth) {
    // Only update for quiet moves
    if (move.is_capture()) return;

    int bonus = depth * depth;
    int& h = history[(int)color][move.from()][move.to()];
    h += bonus;

    // Cap to prevent overflow
    if (h > HISTORY_MAX) h = HISTORY_MAX;
}

// Score a move for ordering purposes
// Higher score = searched first
// For noisy moves: MVV-LVA + promotion bonus
// For quiet moves: killer bonus + history score
static int score_move(const Move32& move, const Board& board, int ply) {
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

    // Quiet move ordering: killers then history
    if (!move.is_capture() && !move.is_promotion()) {
        if (killers[ply][0].same_move(move)) {
            score += KILLER_SCORE_1;
        } else if (killers[ply][1].same_move(move)) {
            score += KILLER_SCORE_2;
        } else {
            // History score
            Color color = board.turn;
            score += history[(int)color][move.from()][move.to()];
        }
    }

    return score;
}

// Pick the best move from index 'start' onwards and swap it to 'start'
static void pick_move(MoveList& moves, int start, const Board& board, int ply) {
    int best_idx = start;
    int best_score = score_move(moves[start], board, ply);

    for (int i = start + 1; i < moves.size; ++i) {
        int score = score_move(moves[i], board, ply);
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

    // When in check, generate all moves (must find escape)
    // Otherwise, only generate noisy moves (captures + promotions)
    MoveList moves = in_chk ? generate_moves(board)
                            : generate_moves<MoveType::Noisy>(board);

    int legal_moves = 0;

    for (int i = 0; i < moves.size; ++i) {
        // Pick best remaining move
        pick_move(moves, i, board, ply);

        Move32& move = moves[i];

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

// Forward declaration
static int alpha_beta(Board& board, TTable& tt, int depth, int alpha, int beta, int ply);

// Helper to search a single move and update alpha/beta
// Returns true if beta cutoff occurred
static bool search_move(Board& board, TTable& tt, Move32& move, int depth, int& alpha, int beta, int ply,
                        int& best_score, Move32& best_move, int& legal_moves) {
    make_move(board, move);

    if (is_illegal(board)) {
        unmake_move(board, move);
        return false;
    }

    legal_moves++;

    int score = -alpha_beta(board, tt, depth - 1, -beta, -alpha, ply + 1);

    unmake_move(board, move);

    if (stop_search) return false;

    if (score > best_score) {
        best_score = score;
        best_move = move;
    }

    if (score >= beta) {
        tt.store(board.hash, depth, beta, TT_LOWER, move);
        return true;  // Beta cutoff
    }

    if (score > alpha) {
        alpha = score;
    }

    return false;
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

    int best_score = -INFINITY_SCORE;
    Move32 best_move(0);
    int legal_moves = 0;
    bool tt_move_searched = false;

    // Try TT move first if available
    if (tt_move.data != 0) {
        if (search_move(board, tt, tt_move, depth, alpha, beta, ply, best_score, best_move, legal_moves)) {
            // Update killer and history on beta cutoff
            update_killer(ply, tt_move);
            update_history(board.turn, tt_move, depth);
            return beta;
        }
        if (stop_search) return 0;
        tt_move_searched = true;
    }

    // Stage 1: Noisy moves (captures + promotions)
    MoveList noisy = generate_moves<MoveType::Noisy>(board);
    for (int i = 0; i < noisy.size; ++i) {
        pick_move(noisy, i, board, ply);
        Move32& move = noisy[i];

        // Skip if already searched as TT move
        if (tt_move_searched && move.same_move(tt_move)) {
            continue;
        }

        if (search_move(board, tt, move, depth, alpha, beta, ply, best_score, best_move, legal_moves)) {
            // Update killer and history on beta cutoff
            update_killer(ply, move);
            update_history(board.turn, move, depth);
            return beta;
        }
        if (stop_search) return 0;
    }

    // Stage 2: Quiet moves
    MoveList quiet = generate_moves<MoveType::Quiet>(board);
    for (int i = 0; i < quiet.size; ++i) {
        pick_move(quiet, i, board, ply);
        Move32& move = quiet[i];

        // Skip if already searched as TT move
        if (tt_move_searched && move.same_move(tt_move)) {
            continue;
        }

        if (search_move(board, tt, move, depth, alpha, beta, ply, best_score, best_move, legal_moves)) {
            // Update killer and history on beta cutoff
            update_killer(ply, move);
            update_history(board.turn, move, depth);
            return beta;
        }
        if (stop_search) return 0;
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

// Helper to search a move at root level
// Returns true if this is the new best move
static bool search_root_move(Board& board, TTable& tt, Move32& move, int depth,
                             int& alpha, Move32& best_move, int& best_score) {
    make_move(board, move);

    if (is_illegal(board)) {
        unmake_move(board, move);
        return false;
    }

    int score = -alpha_beta(board, tt, depth - 1, -INFINITY_SCORE, -alpha, 1);

    unmake_move(board, move);

    if (stop_search) return false;

    if (score > best_score) {
        best_score = score;
        best_move = move;
        if (score > alpha) {
            alpha = score;
        }
        return true;
    }

    return false;
}

// Root search - returns best move and score
static std::pair<Move32, int> search_root(Board& board, TTable& tt, int depth) {
    // Try TT move first if available
    int tt_score;
    Move32 tt_move(0);
    tt.probe(board.hash, depth, -INFINITY_SCORE, INFINITY_SCORE, tt_score, tt_move);

    int alpha = -INFINITY_SCORE;
    Move32 best_move(0);
    int best_score = -INFINITY_SCORE;
    bool tt_move_searched = false;

    // Try TT move first
    if (tt_move.data != 0) {
        search_root_move(board, tt, tt_move, depth, alpha, best_move, best_score);
        if (stop_search) return {best_move, best_score};
        tt_move_searched = true;
    }

    // Stage 1: Noisy moves (captures + promotions)
    MoveList noisy = generate_moves<MoveType::Noisy>(board);
    for (int i = 0; i < noisy.size; ++i) {
        pick_move(noisy, i, board, 0);  // ply=0 at root
        Move32& move = noisy[i];

        if (tt_move_searched && move.same_move(tt_move)) {
            continue;
        }

        search_root_move(board, tt, move, depth, alpha, best_move, best_score);
        if (stop_search) return {best_move, best_score};
    }

    // Stage 2: Quiet moves
    MoveList quiet = generate_moves<MoveType::Quiet>(board);
    for (int i = 0; i < quiet.size; ++i) {
        pick_move(quiet, i, board, 0);  // ply=0 at root
        Move32& move = quiet[i];

        if (tt_move_searched && move.same_move(tt_move)) {
            continue;
        }

        search_root_move(board, tt, move, depth, alpha, best_move, best_score);
        if (stop_search) return {best_move, best_score};
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

    // Clear move ordering tables for new search
    clear_killers();
    clear_history();

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
