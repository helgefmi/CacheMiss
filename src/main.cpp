#include "board.hpp"
#include "move.hpp"
#include "perft.hpp"
#include <iostream>
#include <string>
#include <cstring>

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  -fen <fen>      Set position (default: starting position)\n"
              << "  -perft <depth>  Run perft to given depth\n"
              << "  -divide <depth> Run divide (perft per move) to given depth\n";
}

int main(int argc, char* argv[]) {
    std::string fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
    int perft_depth = 0;
    int divide_depth = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-fen") == 0 && i + 1 < argc) {
            fen = argv[++i];
        } else if (strcmp(argv[i], "-perft") == 0 && i + 1 < argc) {
            perft_depth = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "-divide") == 0 && i + 1 < argc) {
            divide_depth = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown option: " << argv[i] << '\n';
            print_usage(argv[0]);
            return 1;
        }
    }

    Board board(fen);

    if (divide_depth > 0) {
        divide(board, divide_depth);
    } else if (perft_depth > 0) {
        u64 nodes = perft(board, perft_depth);
        std::cout << nodes << '\n';
    } else {
        // Default: show legal moves
        for (auto move : generate_moves(board)) {
            std::cout << move.to_string() << '\n';
        }
    }

    return 0;
}
