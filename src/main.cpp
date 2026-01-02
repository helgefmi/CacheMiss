#include "bench.hpp"
#include "board.hpp"
#include "move.hpp"
#include "perft.hpp"
#include "search.hpp"
#include "zobrist.hpp"
#include <cstring>
#include <iostream>
#include <string>

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  -fen <fen>      Set position (default: starting position)\n"
              << "  -perft <depth>  Run perft to given depth\n"
              << "  -divide <depth> Run divide (perft per move) to given depth\n"
              << "  -search [time]  Search for best move (time in ms, default: 10000)\n"
              << "  -bench-perftsuite <file> [max_depth]  Run perft test suite\n"
              << "  -bench-wac <file> [time_ms]  Run WAC test suite (default: 1000ms per position)\n"
              << "  -wac-id <id>     Filter WAC suite to single position (e.g., \"WAC.007\")\n"
              << "  -mem <mb>       Hash table size in MB (default: 512)\n";
}

int main(int argc, char* argv[]) {
    zobrist::init();

    std::string fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
    int perft_depth = 0;
    int divide_depth = 0;
    int search_time = 0;
    std::string perftsuite_file;
    int perftsuite_max_depth = 0;
    std::string wac_file;
    int wac_time_ms = 1000;
    std::string wac_id;
    size_t mem_mb = 512;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-fen") == 0 && i + 1 < argc) {
            fen = argv[++i];
        } else if (strcmp(argv[i], "-perft") == 0 && i + 1 < argc) {
            perft_depth = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "-divide") == 0 && i + 1 < argc) {
            divide_depth = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "-mem") == 0 && i + 1 < argc) {
            mem_mb = std::stoul(argv[++i]);
        } else if (strcmp(argv[i], "-search") == 0) {
            search_time = 10000;  // default 10 seconds
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') {
                search_time = std::stoi(argv[++i]);
            }
        } else if (strcmp(argv[i], "-bench-perftsuite") == 0 && i + 1 < argc) {
            perftsuite_file = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') {
                perftsuite_max_depth = std::stoi(argv[++i]);
            }
        } else if (strcmp(argv[i], "-bench-wac") == 0 && i + 1 < argc) {
            wac_file = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') {
                wac_time_ms = std::stoi(argv[++i]);
            }
        } else if (strcmp(argv[i], "-wac-id") == 0 && i + 1 < argc) {
            wac_id = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown option: " << argv[i] << '\n';
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!perftsuite_file.empty()) {
        bench_perftsuite(perftsuite_file, perftsuite_max_depth, mem_mb);
        return 0;
    }

    if (!wac_file.empty()) {
        bench_wac(wac_file, wac_time_ms, mem_mb, wac_id);
        return 0;
    }

    Board board(fen);

    if (divide_depth > 0) {
        PerftTable tt(mem_mb);
        divide(board, divide_depth, &tt);
    } else if (perft_depth > 0) {
        PerftTable tt(mem_mb);
        u64 nodes = perft(board, perft_depth, &tt);
        std::cout << nodes << '\n';
    } else if (search_time > 0) {
        TTable tt(mem_mb);
        search(board, tt, search_time);
    } else {
        // TODO: UCI mode
    }

    return 0;
}
