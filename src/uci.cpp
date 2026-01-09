#include "uci.hpp"
#include "board.hpp"
#include "move.hpp"
#include "search.hpp"
#include "ttable.hpp"
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <atomic>

#include <sys/select.h>
#include <unistd.h>

static const char* ENGINE_NAME = "CacheMiss";
static const char* ENGINE_AUTHOR = "Helge";

// UCI options
static int move_overhead_ms = 100;
static bool ponder_enabled = false;

// Search state
static std::atomic<bool> search_running{false};
static SearchResult last_result;
static int moves_played = 0;  // Track game progress for time management

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
static void parse_position(const std::string& line, Board& board) {
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

// Parse "go" command result
struct GoParams {
    int time_ms;
    bool is_ponder;
};

// Parse "go" command and return time in ms and ponder flag
// go movetime <ms>
// go wtime <ms> btime <ms> [winc <ms>] [binc <ms>] [movestogo <n>]
// go depth <d>
// go infinite
// go ponder
static GoParams parse_go(const std::string& line, const Board& board) {
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

    // Calculate time to use
    if (movetime > 0) {
        return {movetime, is_ponder};
    }

    if (infinite || is_ponder) {
        return {999999999, is_ponder};  // Very long time for infinite/ponder
    }

    int our_time = (board.turn == Color::White) ? wtime : btime;
    int our_inc = (board.turn == Color::White) ? winc : binc;

    // Subtract move overhead to account for network/GUI latency
    our_time = std::max(0, our_time - move_overhead_ms);

    if (our_time > 0) {
        // Determine moves remaining
        int moves_remaining;
        if (movestogo > 0) {
            moves_remaining = movestogo;  // Trust the GUI
        } else {
            moves_remaining = estimate_moves_remaining(moves_played);
        }

        // Time = base allocation + most of increment
        int time_for_move = our_time / moves_remaining + our_inc * 3 / 4;

        // Safety bounds
        if (time_for_move < 10) time_for_move = 10;
        if (time_for_move > our_time / 4) time_for_move = our_time / 4;

        return {time_for_move, is_ponder};
    }

    // Default: 1 second
    return {1000, is_ponder};
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
static void output_bestmove(const SearchResult& result) {
    std::cout << "bestmove " << result.best_move.to_uci();
    if (result.pv_length >= 2) {
        std::cout << " ponder " << result.pv[1].to_uci();
    }
    std::cout << std::endl;
}

void uci_loop(size_t hash_mb) {
    Board board;
    TTable tt(hash_mb);
    bool is_pondering = false;

    std::string line;
    while (std::getline(std::cin, line)) {
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
            parse_position(line, board);
        }
        else if (cmd == "go") {
            GoParams params = parse_go(line, board);
            is_pondering = params.is_ponder;

            // Reset stop flag and start search in a thread
            global_stop_flag.store(false, std::memory_order_relaxed);
            search_running.store(true, std::memory_order_relaxed);

            std::thread search_thread([&board, &tt, time_ms = params.time_ms]() {
                last_result = search(board, tt, time_ms);
                search_running.store(false, std::memory_order_relaxed);
            });

            // Poll for stop/ponderhit while search is running
            while (search_running.load(std::memory_order_relaxed)) {
                if (input_available()) {
                    std::string input_cmd;
                    if (!std::getline(std::cin, input_cmd)) break;

                    if (input_cmd == "stop") {
                        global_stop_flag.store(true, std::memory_order_relaxed);
                        is_pondering = false;
                    }
                    else if (input_cmd == "ponderhit") {
                        // Opponent played our predicted move, continue searching normally
                        is_pondering = false;
                        // Search continues with remaining time
                    }
                    else if (input_cmd == "quit") {
                        global_stop_flag.store(true, std::memory_order_relaxed);
                        search_thread.join();
                        return;  // Exit the UCI loop
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            search_thread.join();

            // Only output bestmove if not pondering (ponderhit clears pondering flag)
            if (!is_pondering) {
                output_bestmove(last_result);
                moves_played++;
            }
        }
        else if (cmd == "stop") {
            // Stop received when not searching - ignore
            global_stop_flag.store(true, std::memory_order_relaxed);
        }
        else if (cmd == "ponderhit") {
            // Ponderhit received when not searching - ignore
            is_pondering = false;
        }
        else if (cmd == "quit") {
            break;
        }
        else {
            std::cerr << "Unknown command: " << cmd << std::endl;
        }
    }
}
