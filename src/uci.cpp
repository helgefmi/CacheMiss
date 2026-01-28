#include "uci.hpp"
#include "board.hpp"
#include "eval.hpp"
#include "move.hpp"
#include "search.hpp"
#include "ttable.hpp"
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>

#include <sys/select.h>
#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <cmath>

// Trim trailing whitespace and carriage returns (handles CRLF line endings)
static std::string trim_right(const std::string& s) {
    auto end = s.find_last_not_of(" \t\r\n");
    return (end == std::string::npos) ? "" : s.substr(0, end + 1);
}

static const char* ENGINE_NAME = "CacheMiss";
static const char* ENGINE_AUTHOR = "Helge";

// UCI options
static int move_overhead_ms = 100;
static bool ponder_enabled = false;

// Search state
static std::atomic<bool> search_running{false};
static SearchResult last_result;
static std::mutex result_mutex;  // Protects last_result from data races
static int moves_played = 0;     // Track game progress for time management
static std::chrono::steady_clock::time_point search_start_time;  // For ponderhit elapsed calculation

// Estimate moves remaining based on game phase
static int estimate_moves_remaining(int moves_played) {
    if (moves_played < 10) return 50;   // Opening: expect long game
    if (moves_played < 30) return 35;   // Early middlegame
    if (moves_played < 50) return 25;   // Late middlegame
    return 20;                          // Endgame
}

// Non-blocking check if stdin has input available
static bool input_available() {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    struct timeval tv = {0, 0};
    return select(1, &fds, nullptr, nullptr, &tv) > 0;
}

// Parse "position" command
// position startpos [moves e2e4 e7e5 ...]
// position fen <fen> [moves e2e4 e7e5 ...]
void parse_position_command(const std::string& line, Board& board) {
    std::istringstream iss(line);
    std::string token;
    iss >> token;  // "position"

    if (!(iss >> token)) return;

    if (token == "startpos") {
        board = Board();  // Default starting position
        iss >> token;     // Possibly "moves"
    } else if (token == "fen") {
        // Collect FEN string (up to 6 parts)
        std::string fen;
        for (int i = 0; i < 6 && (iss >> token); ++i) {
            if (token == "moves") break;
            if (!fen.empty()) fen += ' ';
            fen += token;
        }
        board = Board(fen);
        // If we collected all 6 FEN parts, check if there's a "moves" token next
        if (token != "moves") {
            iss >> token;  // Try to read "moves"
        }
    }

    // Apply moves if present
    if (token == "moves") {
        while (iss >> token) {
            Move32 move = parse_uci_move(token, board);
            if (move.data != 0) {
                make_move(board, move);
            }
        }
    }
}

// Parse "go" command and return time in ms and ponder flag
// go movetime <ms>
// go wtime <ms> btime <ms> [winc <ms>] [binc <ms>] [movestogo <n>]
// go depth <d>
// go infinite
// go ponder
GoParams parse_go_command(const std::string& line, const Board& board, int moves_played, int move_overhead_ms) {
    std::istringstream iss(line);
    std::string token;
    iss >> token;  // "go"

    int movetime = 0;
    int wtime = 0, btime = 0;
    int winc = 0, binc = 0;
    int movestogo = 0;
    int depth = 0;
    bool infinite = false;
    bool is_ponder = false;

    while (iss >> token) {
        if (token == "movetime") {
            iss >> movetime;
        } else if (token == "wtime") {
            iss >> wtime;
        } else if (token == "btime") {
            iss >> btime;
        } else if (token == "winc") {
            iss >> winc;
        } else if (token == "binc") {
            iss >> binc;
        } else if (token == "movestogo") {
            iss >> movestogo;
        } else if (token == "depth") {
            iss >> depth;
        } else if (token == "infinite") {
            infinite = true;
        } else if (token == "ponder") {
            is_ponder = true;
        }
    }

    // Calculate time we'd use for a normal search (even if pondering)
    int normal_time = 1000;  // Default: 1 second (only used when no time control info)

    if (movetime > 0) {
        normal_time = movetime;
    } else if (wtime > 0 || btime > 0) {
        // We have time control info - calculate time for move
        int our_time = (board.turn == Color::White) ? wtime : btime;
        int our_inc = (board.turn == Color::White) ? winc : binc;

        // Subtract move overhead to account for network/GUI latency
        our_time = std::max(0, our_time - move_overhead_ms);

        int time_for_move;
        if (our_time == 0) {
            // Critical time - use absolute minimum
            time_for_move = 10;
        } else {
            // Determine moves remaining
            int moves_remaining;
            if (movestogo > 0) {
                moves_remaining = movestogo;  // Trust the GUI
            } else {
                moves_remaining = estimate_moves_remaining(moves_played);
            }

            // Time = base allocation + most of increment
            time_for_move = our_time / moves_remaining + our_inc * 3 / 4;

            // Adjust based on opponent's time - use more time if we have a time advantage
            int opp_time = (board.turn == Color::White) ? btime : wtime;
            if (opp_time > 0 && our_time > 0) {
                // sqrt provides diminishing returns: 4x advantage → 2x multiplier
                double ratio = std::sqrt((double)our_time / opp_time);
                // Clamp to [0.7, 1.5] to prevent extreme behavior
                ratio = std::clamp(ratio, 0.7, 1.5);
                time_for_move = (int)(time_for_move * ratio);
            }

            // Safety bounds
            if (time_for_move < 10) time_for_move = 10;
            if (time_for_move > our_time / 4) time_for_move = our_time / 4;
        }

        normal_time = time_for_move;
    }

    // For infinite or ponder, use very long time but remember normal_time for ponderhit
    if (infinite) {
        return {999999999, 999999999, depth, false};
    }
    if (is_ponder) {
        return {999999999, normal_time, depth, true};  // Long time for ponder, but save normal time
    }

    // If only depth specified, use very long time
    if (depth > 0 && movetime == 0 && wtime == 0 && btime == 0) {
        return {999999999, 999999999, depth, false};
    }

    return {normal_time, normal_time, depth, false};
}

// Parse "setoption name <name> value <value>"
// Handles multi-word option names like "Move Overhead"
static void parse_setoption(const std::string& line, size_t& hash_mb, bool& hash_changed) {
    std::istringstream iss(line);
    std::string token;
    iss >> token;  // "setoption"

    std::string name, value;
    bool reading_name = false;
    bool reading_value = false;

    while (iss >> token) {
        if (token == "name") {
            reading_name = true;
            reading_value = false;
            name.clear();
        } else if (token == "value") {
            reading_name = false;
            reading_value = true;
            value.clear();
        } else if (reading_name) {
            if (!name.empty()) name += " ";
            name += token;
        } else if (reading_value) {
            if (!value.empty()) value += " ";
            value += token;
        }
    }

    if (name == "Hash" && !value.empty()) {
        size_t new_hash = std::stoul(value);
        if (new_hash >= 1 && new_hash <= 65536) {
            hash_mb = new_hash;
            hash_changed = true;
        }
    } else if (name == "Move Overhead" && !value.empty()) {
        int overhead = std::stoi(value);
        if (overhead >= 0 && overhead <= 5000) {
            move_overhead_ms = overhead;
        }
    } else if (name == "Ponder") {
        ponder_enabled = (value == "true");
    }
}

// Output bestmove with optional ponder move
// Acquires result_mutex to safely read last_result
// Validates ponder move is legal in position after best_move
static void output_bestmove(const Board& board) {
    std::lock_guard<std::mutex> lock(result_mutex);
    std::cout << "bestmove " << last_result.best_move.to_uci();

    if (ponder_enabled && last_result.pv_length >= 2) {
        // Validate ponder move is legal in position after best_move
        Board ponder_board = board;
        make_move(ponder_board, last_result.pv[0]);

        MoveList moves = generate_moves<MoveType::All>(ponder_board);
        bool ponder_valid = false;
        for (int i = 0; i < moves.size; ++i) {
            Board test = ponder_board;
            make_move(test, moves[i]);
            if (!is_illegal(test) && moves[i].same_move(last_result.pv[1])) {
                ponder_valid = true;
                break;
            }
        }

        if (ponder_valid) {
            std::cout << " ponder " << last_result.pv[1].to_uci();
        }
    }
    std::cout << std::endl;
}

// Poll for stop/ponderhit/quit while search is running
// Returns true if should exit UCI loop (quit received)
static bool poll_during_search(bool& is_pondering, int ponder_time_ms) {
    while (search_running.load(std::memory_order_acquire)) {
        if (input_available()) {
            std::string input_cmd;
            if (!std::getline(std::cin, input_cmd)) break;
            input_cmd = trim_right(input_cmd);

            if (input_cmd == "stop") {
                std::cerr << "info string received: stop" << std::endl;
                g_search_controller.request_stop();
                is_pondering = false;
            }
            else if (input_cmd == "ponderhit") {
                auto now = std::chrono::steady_clock::now();
                auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - search_start_time).count();
                int new_limit = static_cast<int>(elapsed_ms) + ponder_time_ms;
                std::cerr << "info string received: ponderhit (elapsed=" << elapsed_ms
                          << "ms, adding=" << ponder_time_ms << "ms, limit=" << new_limit << "ms)" << std::endl;
                is_pondering = false;
                g_search_controller.set_time_limit(new_limit);
            }
            else if (input_cmd == "quit") {
                std::cerr << "info string received: quit" << std::endl;
                g_search_controller.request_stop();
                return true;  // Signal to exit UCI loop
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

// Wait for stop/ponderhit after ponder search finished naturally
// Returns true if should exit UCI loop (quit received)
static bool wait_for_ponder_end(bool& is_pondering) {
    while (is_pondering) {
        std::cerr << "info string ponder search finished, waiting for stop/ponderhit" << std::endl;
        std::string input_cmd;
        if (!std::getline(std::cin, input_cmd)) break;
        input_cmd = trim_right(input_cmd);

        if (input_cmd == "stop") {
            std::cerr << "info string received: stop (after ponder finished)" << std::endl;
            is_pondering = false;
        }
        else if (input_cmd == "ponderhit") {
            std::cerr << "info string received: ponderhit (after ponder finished)" << std::endl;
            is_pondering = false;
        }
        else if (input_cmd == "quit") {
            return true;  // Exit UCI loop
        }
    }
    return false;
}

// Handle "go" command: start search, poll for commands, output bestmove
// Returns true if should exit UCI loop (quit received)
static bool handle_go_command(const std::string& line, Board& board, TTable& tt) {
    GoParams params = parse_go_command(line, board, moves_played, move_overhead_ms);
    bool is_pondering = params.is_ponder;
    int ponder_time_ms = params.normal_time_ms;

    g_search_controller.reset();
    tt.new_search();
    search_running.store(true, std::memory_order_relaxed);
    search_start_time = std::chrono::steady_clock::now();

    std::thread search_thread([&board, &tt, time_ms = params.time_ms, depth_limit = params.depth_limit]() {
        SearchResult result = search(board, tt, time_ms, depth_limit);
        {
            std::lock_guard<std::mutex> lock(result_mutex);
            last_result = result;
        }
        search_running.store(false, std::memory_order_release);
    });

    bool should_quit = poll_during_search(is_pondering, ponder_time_ms);
    search_thread.join();

    if (should_quit) {
        return true;
    }

    if (wait_for_ponder_end(is_pondering)) {
        return true;
    }

    output_bestmove(board);
    moves_played++;
    return false;
}

void uci_loop(size_t hash_mb) {
    Board board;
    TTable tt(hash_mb);

    std::string line;
    while (std::getline(std::cin, line)) {
        line = trim_right(line);
        if (line.empty()) continue;

        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;

        if (cmd == "uci") {
            std::cout << "id name " << ENGINE_NAME << std::endl;
            std::cout << "id author " << ENGINE_AUTHOR << std::endl;
            std::cout << "option name Hash type spin default 512 min 1 max 65536" << std::endl;
            std::cout << "option name Move Overhead type spin default 100 min 0 max 5000" << std::endl;
            std::cout << "option name Ponder type check default false" << std::endl;
            std::cout << "uciok" << std::endl;
        }
        else if (cmd == "isready") {
            std::cout << "readyok" << std::endl;
        }
        else if (cmd == "ucinewgame") {
            tt.clear();
            g_pawn_cache.clear();
            board = Board();
            moves_played = 0;
        }
        else if (cmd == "setoption") {
            bool hash_changed = false;
            parse_setoption(line, hash_mb, hash_changed);
            if (hash_changed) {
                tt = TTable(hash_mb);
            }
        }
        else if (cmd == "position") {
            parse_position_command(line, board);
        }
        else if (cmd == "go") {
            if (handle_go_command(line, board, tt)) {
                return;  // Quit received during search
            }
        }
        else if (cmd == "stop") {
            g_search_controller.request_stop();
        }
        else if (cmd == "ponderhit") {
            // Ponderhit received when not searching - ignore
        }
        else if (cmd == "quit") {
            break;
        }
        else {
            std::cerr << "Unknown command: " << cmd << std::endl;
        }
    }
}
