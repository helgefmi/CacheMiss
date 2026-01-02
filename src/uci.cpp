#include "uci.hpp"
#include "board.hpp"
#include "move.hpp"
#include "search.hpp"
#include "ttable.hpp"
#include <iostream>
#include <sstream>
#include <string>

static const char* ENGINE_NAME = "CacheMiss";
static const char* ENGINE_AUTHOR = "Helge";

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

// Parse "go" command and return time in ms
// go movetime <ms>
// go wtime <ms> btime <ms> [winc <ms>] [binc <ms>]
// go depth <d>
// go infinite
static int parse_go(const std::string& line, const Board& board) {
    std::istringstream iss(line);
    std::string token;
    iss >> token;  // "go"

    int movetime = 0;
    int wtime = 0, btime = 0;
    int winc = 0, binc = 0;
    int depth = 0;
    bool infinite = false;

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
        } else if (token == "depth") {
            iss >> depth;
        } else if (token == "infinite") {
            infinite = true;
        }
    }

    // Calculate time to use
    if (movetime > 0) {
        return movetime;
    }

    if (infinite) {
        return 999999999;  // Very long time
    }

    // Simple time management: use 1/30 of remaining time + half of increment
    int our_time = (board.turn == Color::White) ? wtime : btime;
    int our_inc = (board.turn == Color::White) ? winc : binc;

    if (our_time > 0) {
        int time_for_move = our_time / 30 + our_inc / 2;
        // Ensure at least 10ms, don't use more than half remaining time
        if (time_for_move < 10) time_for_move = 10;
        if (time_for_move > our_time / 2) time_for_move = our_time / 2;
        return time_for_move;
    }

    // Default: 1 second
    return 1000;
}

void uci_loop(size_t hash_mb) {
    Board board;
    TTable tt(hash_mb);

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
            std::cout << "uciok" << std::endl;
        }
        else if (cmd == "isready") {
            std::cout << "readyok" << std::endl;
        }
        else if (cmd == "ucinewgame") {
            tt.clear();
            board = Board();
        }
        else if (cmd == "position") {
            parse_position(line, board);
        }
        else if (cmd == "go") {
            int time_ms = parse_go(line, board);
            SearchResult result = search(board, tt, time_ms);
            std::cout << "bestmove " << result.best_move.to_uci() << std::endl;
        }
        else if (cmd == "quit") {
            break;
        } else {
            std::cerr << "Unknown command: " << cmd << std::endl;
        }
    }
}
