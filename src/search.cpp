#include "search.hpp"
#include "eval.hpp"
#include <chrono>
#include <cmath>
#include <iostream>

// Constants
constexpr int INFINITY_SCORE = 30000;
constexpr int MATE_SCORE = 29000;
constexpr int ASPIRATION_WINDOW = 50;  // Initial aspiration window size (centipawns)

// LMR (Late Move Reduction) table
// Indexed by [depth][move_count], values are reduction amounts
constexpr int LMR_MAX_DEPTH = 64;
constexpr int LMR_MAX_MOVES = 64;
static int LMR_TABLE[LMR_MAX_DEPTH][LMR_MAX_MOVES];

// Initialize LMR table with log-based formula
// Called once at startup
static bool init_lmr_table() {
    for (int depth = 0; depth < LMR_MAX_DEPTH; ++depth) {
        for (int moves = 0; moves < LMR_MAX_MOVES; ++moves) {
            if (depth == 0 || moves == 0) {
                LMR_TABLE[depth][moves] = 0;
            } else {
                // Standard log formula used by many engines
                // R = 0.5 + ln(depth) * ln(moves) / 2.25
                double reduction = 0.5 + std::log(depth) * std::log(moves) / 2.25;
                LMR_TABLE[depth][moves] = static_cast<int>(reduction);
            }
        }
    }
    return true;
}

// Static initialization
static bool lmr_initialized = init_lmr_table();

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
constexpr int KILLER_SCORE_1 = 9000;
constexpr int KILLER_SCORE_2 = 8000;
constexpr int HISTORY_MAX = 6000;

// ============================================================================
// SearchContext - bundles all search state
// ============================================================================

struct SearchContext {
    Board& board;
    TTable& tt;

    // Time control
    std::chrono::steady_clock::time_point start_time;
    int time_limit_ms;
    bool stop_search = false;
    u64 nodes_searched = 0;

    // Move ordering tables
    Move32 killers[MAX_PLY][2] = {};
    int history[2][64][64] = {};

    // PV tracking
    Move32 pv_table[MAX_PLY][MAX_PLY] = {};
    int pv_length[MAX_PLY] = {};
    Move32 prev_best_move{0};

    SearchContext(Board& b, TTable& t, int time_ms)
        : board(b), tt(t),
          start_time(std::chrono::steady_clock::now()),
          time_limit_ms(time_ms) {}

    bool check_time() {
        if ((nodes_searched & 2047) == 0) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();
            if (elapsed >= time_limit_ms) {
                stop_search = true;
            }
        }
        return stop_search;
    }

    void update_killer(int ply, Move32 move) {
        if (move.is_capture()) return;
        if (killers[ply][0].same_move(move)) return;
        killers[ply][1] = killers[ply][0];
        killers[ply][0] = move;
    }

    void update_history(Color color, Move32 move, int depth) {
        if (move.is_capture()) return;
        int bonus = depth * depth;
        int& h = history[(int)color][move.from()][move.to()];
        h += bonus;
        if (h > HISTORY_MAX) h = HISTORY_MAX;
    }

    void init_pv(int ply) {
        pv_length[ply] = ply;
    }

    void update_pv(int ply, Move32 move) {
        pv_table[ply][ply] = move;
        for (int i = ply + 1; i < pv_length[ply + 1]; ++i) {
            pv_table[ply][i] = pv_table[ply + 1][i];
        }
        pv_length[ply] = pv_length[ply + 1];
    }
};

// ============================================================================
// MovePicker - staged move generation with ordering
// ============================================================================

struct MovePicker {
    enum Stage { TT_MOVE, PREV_BEST, NOISY, QUIET, DONE };

    SearchContext& ctx;
    int ply;
    Move32 tt_move;
    Move32 prev_best;

    Stage stage;
    MoveList moves;
    int index;

    MovePicker(SearchContext& ctx, int ply, Move32 tt_move, Move32 prev_best = Move32(0))
        : ctx(ctx), ply(ply), tt_move(tt_move), prev_best(prev_best),
          stage(TT_MOVE), index(0) {}

    Move32 next() {
        switch (stage) {
        case TT_MOVE:
            stage = PREV_BEST;
            if (tt_move.data != 0) {
                return tt_move;
            }
            [[fallthrough]];

        case PREV_BEST:
            stage = NOISY;
            if (prev_best.data != 0 && !prev_best.same_move(tt_move)) {
                return prev_best;
            }
            // Generate noisy moves for next stage
            moves = generate_moves<MoveType::Noisy>(ctx.board);
            index = 0;
            [[fallthrough]];

        case NOISY:
            while (index < moves.size) {
                pick_best();
                Move32 move = moves[index++];
                if (should_skip(move)) continue;
                return move;
            }
            // Generate quiet moves for next stage
            moves = generate_moves<MoveType::Quiet>(ctx.board);
            index = 0;
            stage = QUIET;
            [[fallthrough]];

        case QUIET:
            while (index < moves.size) {
                pick_best();
                Move32 move = moves[index++];
                if (should_skip(move)) continue;
                return move;
            }
            stage = DONE;
            [[fallthrough]];

        case DONE:
            return Move32(0);
        }
        return Move32(0);
    }

private:
    bool should_skip(const Move32& move) {
        return (tt_move.data != 0 && move.same_move(tt_move)) ||
               (prev_best.data != 0 && move.same_move(prev_best));
    }

    int score_move(const Move32& move) {
        int score = 0;

        if (move.is_capture()) {
            int victim_value = MVV_LVA_VALUES[(int)move.captured()];
            int attacker_value = MVV_LVA_VALUES[(int)ctx.board.pieces_on_square[move.from()]];
            score += 10000 + victim_value * 10 - attacker_value;
        }

        if (move.is_promotion()) {
            score += 9000 + MVV_LVA_VALUES[(int)move.promotion()];
        }

        if (!move.is_capture() && !move.is_promotion()) {
            if (ctx.killers[ply][0].same_move(move)) {
                score += KILLER_SCORE_1;
            } else if (ctx.killers[ply][1].same_move(move)) {
                score += KILLER_SCORE_2;
            } else {
                score += ctx.history[(int)ctx.board.turn][move.from()][move.to()];
            }
        }

        return score;
    }

    void pick_best() {
        int best_idx = index;
        int best_score = score_move(moves[index]);

        for (int i = index + 1; i < moves.size; ++i) {
            int score = score_move(moves[i]);
            if (score > best_score) {
                best_score = score;
                best_idx = i;
            }
        }

        if (best_idx != index) {
            std::swap(moves[index], moves[best_idx]);
        }
    }
};

// ============================================================================
// Search functions
// ============================================================================

static bool in_check(const Board& board) {
    Color us = board.turn;
    Color them = opposite(us);
    return is_attacked(board.king_sq[(int)us], them, board);
}

// Check if current position is a repetition of an earlier position
// Only checks positions with same side to move (every 2nd ply)
// Bounded by halfmove_clock since captures/pawn moves reset repetition possibility
static bool is_repetition(const Board& board) {
    int limit = board.halfmove_clock;
    for (int i = 2; i <= limit; i += 2) {
        int idx = board.hash_sp - i;
        if (idx < 0) break;
        if (board.hash_stack[idx] == board.hash) {
            return true;
        }
    }
    return false;
}

// Forward declarations
static int alpha_beta(SearchContext& ctx, int depth, int alpha, int beta, int ply, bool is_pv_node, bool can_null = true);

static int quiescence(SearchContext& ctx, int alpha, int beta, int ply) {
    if (ctx.check_time()) return 0;

    ctx.nodes_searched++;

    bool in_chk = in_check(ctx.board);

    if (!in_chk) {
        int stand_pat = evaluate(ctx.board);

        if (stand_pat >= beta) {
            return beta;
        }

        if (stand_pat > alpha) {
            alpha = stand_pat;
        }
    }

    // When in check, generate all moves; otherwise only noisy
    MoveList moves = in_chk ? generate_moves(ctx.board)
                            : generate_moves<MoveType::Noisy>(ctx.board);

    int legal_moves = 0;

    for (int i = 0; i < moves.size; ++i) {
        // Simple selection sort for qsearch (no killer/history needed)
        int best_idx = i;
        int best_score = -INFINITY_SCORE;
        for (int j = i; j < moves.size; ++j) {
            int score = 0;
            if (moves[j].is_capture()) {
                int victim = MVV_LVA_VALUES[(int)moves[j].captured()];
                int attacker = MVV_LVA_VALUES[(int)ctx.board.pieces_on_square[moves[j].from()]];
                score = victim * 10 - attacker;
            }
            if (moves[j].is_promotion()) {
                score += MVV_LVA_VALUES[(int)moves[j].promotion()];
            }
            if (score > best_score) {
                best_score = score;
                best_idx = j;
            }
        }
        if (best_idx != i) std::swap(moves[i], moves[best_idx]);

        Move32& move = moves[i];

        make_move(ctx.board, move);

        if (is_illegal(ctx.board)) {
            unmake_move(ctx.board, move);
            continue;
        }

        legal_moves++;

        int score = -quiescence(ctx, -beta, -alpha, ply + 1);

        unmake_move(ctx.board, move);

        if (ctx.stop_search) return 0;

        if (score >= beta) {
            return beta;
        }

        if (score > alpha) {
            alpha = score;
        }
    }

    if (in_chk && legal_moves == 0) {
        return -MATE_SCORE + ply;
    }

    return alpha;
}

static int alpha_beta(SearchContext& ctx, int depth, int alpha, int beta, int ply, bool is_pv_node, bool can_null) {
    if (ctx.check_time()) return 0;

    ctx.nodes_searched++;
    ctx.init_pv(ply);

    // Draw detection (before TT probe - TT doesn't track repetition history)
    if (ply > 0) {
        // 50-move rule
        if (ctx.board.halfmove_clock >= 100) {
            return 0;
        }
        // Repetition
        if (is_repetition(ctx.board)) {
            return 0;
        }
    }

    // TT probe
    int tt_score;
    Move32 tt_move(0);
    bool tt_hit = ctx.tt.probe(ctx.board.hash, depth, alpha, beta, tt_score, tt_move);
    if (tt_hit && !is_pv_node) {
        return tt_score;
    }

    if (depth == 0) {
        return quiescence(ctx, alpha, beta, ply);
    }

    // Compute in_check once for NMP, checkmate detection, and check extension
    bool in_chk = in_check(ctx.board);

    // Check extension: extend search by 1 ply when in check
    int extension = in_chk ? 1 : 0;
    int new_depth = depth - 1 + extension;

    // Null Move Pruning
    // Skip if: in check, PV node, low depth, or just did NMP (can_null=false)
    if (can_null && !is_pv_node && !in_chk && depth >= 3 && ply >= 1) {
        // Avoid zugzwang: ensure we have non-pawn material
        Color us = ctx.board.turn;
        Bitboard our_pieces = ctx.board.occupied[(int)us];
        Bitboard our_pawns = ctx.board.pieces[(int)us][(int)Piece::Pawn];
        Bitboard our_king = ctx.board.pieces[(int)us][(int)Piece::King];
        bool has_pieces = (our_pieces ^ our_pawns ^ our_king) != 0;

        if (has_pieces) {
            // Conservative reduction: R = 2 at low depths, R = 3 at high depths
            int R = depth >= 6 ? 3 : 2;
            int prev_ep;

            make_null_move(ctx.board, prev_ep);
            // Pass can_null=false to prevent consecutive null moves
            int null_score = -alpha_beta(ctx, depth - 1 - R, -beta, -beta + 1, ply + 1, false, false);
            unmake_null_move(ctx.board, prev_ep);

            if (ctx.stop_search) return 0;

            // Don't trust NMP if score is mate-related (could miss forced mates)
            // Also don't trust if score is near draw (within ~50cp of 0)
            if (null_score >= beta && null_score < MATE_SCORE - MAX_PLY &&
                (null_score > 50 || null_score < -50)) {
                return beta;  // Null move cutoff
            }
        }
    }

    int best_score = -INFINITY_SCORE;
    Move32 best_move(0);
    int moves_searched = 0;
    bool found_pv = false;

    MovePicker picker(ctx, ply, tt_move);
    while (Move32 move = picker.next()) {
        make_move(ctx.board, move);

        if (is_illegal(ctx.board)) {
            unmake_move(ctx.board, move);
            continue;
        }

        moves_searched++;

        // Determine if this move is a candidate for LMR
        bool is_quiet = !move.is_capture() && !move.is_promotion();
        bool is_killer = ctx.killers[ply][0].same_move(move) || ctx.killers[ply][1].same_move(move);
        bool gives_check = in_check(ctx.board);  // opponent is now in check

        // LMR conditions:
        // - Not first few moves (need some moves searched first)
        // - Sufficient depth
        // - Quiet move (not capture or promotion)
        // - Not a killer move
        // - Not when we're in check (in_chk)
        // - Not when move gives check
        bool can_reduce = moves_searched >= 4
                       && depth >= 3
                       && is_quiet
                       && !is_killer
                       && !in_chk
                       && !gives_check;

        int score;

        if (can_reduce) {
            // Calculate reduction from LMR table
            int R = LMR_TABLE[std::min(depth, LMR_MAX_DEPTH - 1)]
                            [std::min(moves_searched, LMR_MAX_MOVES - 1)];

            // Reduce less in PV nodes
            if (is_pv_node && R > 0) {
                R -= 1;
            }

            // Ensure we don't reduce below 1
            int reduced_depth = std::max(1, new_depth - R);

            // Search with reduced depth
            score = -alpha_beta(ctx, reduced_depth, -alpha - 1, -alpha, ply + 1, false);

            // Re-search at full depth if it beats alpha
            if (score > alpha && R > 0 && !ctx.stop_search) {
                score = -alpha_beta(ctx, new_depth, -alpha - 1, -alpha, ply + 1, false);
            }

            // Full PV re-search if still beats alpha in PV node
            if (score > alpha && score < beta && is_pv_node && !ctx.stop_search) {
                score = -alpha_beta(ctx, new_depth, -beta, -alpha, ply + 1, true);
            }
        } else if (found_pv) {
            // Standard PVS for non-reduced moves after PV is found
            score = -alpha_beta(ctx, new_depth, -alpha - 1, -alpha, ply + 1, false);
            if (score > alpha && score < beta && !ctx.stop_search) {
                score = -alpha_beta(ctx, new_depth, -beta, -alpha, ply + 1, true);
            }
        } else {
            // Full search for first moves
            score = -alpha_beta(ctx, new_depth, -beta, -alpha, ply + 1, is_pv_node);
        }

        unmake_move(ctx.board, move);

        if (ctx.stop_search) return 0;

        if (score > best_score) {
            best_score = score;
            best_move = move;
        }

        if (score >= beta) {
            ctx.update_killer(ply, move);
            ctx.update_history(ctx.board.turn, move, depth);
            ctx.tt.store(ctx.board.hash, depth, beta, TT_LOWER, move);
            return beta;
        }

        if (score > alpha) {
            alpha = score;
            found_pv = true;
            ctx.update_pv(ply, move);
        }
    }

    if (moves_searched == 0) {
        return in_chk ? (-MATE_SCORE + ply) : 0;
    }

    TTFlag flag = found_pv ? TT_EXACT : TT_UPPER;
    ctx.tt.store(ctx.board.hash, depth, best_score, flag, best_move);

    return best_score;
}

static std::pair<Move32, int> search_root(SearchContext& ctx, int depth, int alpha, int beta) {
    ctx.init_pv(0);

    // TT probe for move ordering hint
    int tt_score;
    Move32 tt_move(0);
    ctx.tt.probe(ctx.board.hash, depth, alpha, beta, tt_score, tt_move);

    Move32 best_move(0);
    int best_score = -INFINITY_SCORE;
    bool found_pv = false;

    MovePicker picker(ctx, 0, tt_move, ctx.prev_best_move);
    while (Move32 move = picker.next()) {
        make_move(ctx.board, move);

        if (is_illegal(ctx.board)) {
            unmake_move(ctx.board, move);
            continue;
        }

        // PVS search
        int score;
        if (found_pv) {
            score = -alpha_beta(ctx, depth - 1, -alpha - 1, -alpha, 1, false);
            if (score > alpha && score < beta && !ctx.stop_search) {
                score = -alpha_beta(ctx, depth - 1, -beta, -alpha, 1, true);
            }
        } else {
            score = -alpha_beta(ctx, depth - 1, -beta, -alpha, 1, true);
        }

        unmake_move(ctx.board, move);

        if (ctx.stop_search) {
            // Use partial result if we have one
            if (best_move.data == 0) {
                best_move = move;
                best_score = score;
            }
            return {best_move, best_score};
        }

        if (score > best_score) {
            best_score = score;
            best_move = move;
        }

        if (score > alpha) {
            alpha = score;
            found_pv = true;
            ctx.update_pv(0, move);
        }
    }

    if (!ctx.stop_search && best_move.data != 0) {
        TTFlag flag = found_pv ? TT_EXACT : TT_UPPER;
        ctx.tt.store(ctx.board.hash, depth, best_score, flag, best_move);
    }

    return {best_move, best_score};
}

SearchResult search(Board& board, TTable& tt, int time_limit_ms) {
    SearchContext ctx(board, tt, time_limit_ms);

    SearchResult result;
    result.best_move = Move32(0);
    result.score = 0;
    result.depth = 0;
    result.pv_length = 0;

    for (int depth = 1; depth <= MAX_PLY; ++depth) {
        int alpha, beta, delta;

        // Aspiration windows: use narrow window around previous score after depth 1
        if (depth == 1) {
            alpha = -INFINITY_SCORE;
            beta = INFINITY_SCORE;
            delta = INFINITY_SCORE;
        } else {
            delta = ASPIRATION_WINDOW;
            alpha = std::max(-INFINITY_SCORE, result.score - delta);
            beta = std::min(INFINITY_SCORE, result.score + delta);
        }

        Move32 move;
        int score;

        // Aspiration window loop: widen on fail-low or fail-high
        while (true) {
            auto [m, s] = search_root(ctx, depth, alpha, beta);
            move = m;
            score = s;

            if (ctx.stop_search) break;

            // Fail low: widen alpha
            if (score <= alpha) {
                alpha = std::max(-INFINITY_SCORE, alpha - delta);
                delta *= 2;
                continue;
            }

            // Fail high: widen beta
            if (score >= beta) {
                beta = std::min(INFINITY_SCORE, beta + delta);
                delta *= 2;
                continue;
            }

            // Score within window - done with this depth
            break;
        }

        if (ctx.stop_search) {
            if (move.data != 0) {
                result.best_move = move;
                result.score = score;
            }
            break;
        }

        result.best_move = move;
        result.score = score;
        result.depth = depth;

        // Copy PV to result
        result.pv_length = ctx.pv_length[0];
        for (int i = 0; i < ctx.pv_length[0]; ++i) {
            result.pv[i] = ctx.pv_table[0][i];
        }

        // Save best move for next iteration's move ordering
        ctx.prev_best_move = move;

        // Print UCI info
        auto now = std::chrono::steady_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - ctx.start_time).count();

        std::cout << "info depth " << depth
                  << " score cp " << score
                  << " nodes " << ctx.nodes_searched
                  << " time " << elapsed_ms
                  << " pv";

        for (int i = 0; i < result.pv_length; ++i) {
            std::cout << " " << result.pv[i].to_uci();
        }
        std::cout << std::endl;

        if (score >= MATE_SCORE - MAX_PLY || score <= -MATE_SCORE + MAX_PLY) {
            break;
        }
    }

    return result;
}
