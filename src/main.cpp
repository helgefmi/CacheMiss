#include "bench.hpp"
#include "board.hpp"
#include "move.hpp"
#include "perft.hpp"
#include "search.hpp"
#include "tests.hpp"
#include "uci.hpp"
#include "zobrist.hpp"
#include <getopt.h>
#include <iostream>
#include <string>

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  --fen <fen>              Set position (default: starting position)\n"
              << "  --perft <depth>          Run perft to given depth\n"
              << "  --divide <depth>         Run divide (perft per move) to given depth\n"
              << "  --search[=time]          Search for best move (time in ms, default: 10000)\n"
              << "  --bench-perftsuite <file>[=max_depth]  Run perft test suite\n"
              << "  --bench-wac <file>[=time_ms]  Run WAC test suite (default: 1000ms)\n"
              << "  --wac-id <id>            Filter WAC suite to single position\n"
              << "  --tests                  Run test suite\n"
              << "  --mem <mb>               Hash table size in MB (default: 512)\n"
              << "  -h, --help               Show this help\n";
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
    bool run_tests = false;
    size_t mem_mb = 512;

    enum Opt {
        OPT_FEN = 'f',
        OPT_PERFT = 'p',
        OPT_DIVIDE = 'd',
        OPT_SEARCH = 's',
        OPT_BENCH_PERFTSUITE = 'P',
        OPT_BENCH_WAC = 'w',
        OPT_WAC_ID = 'i',
        OPT_TESTS = 'T',
        OPT_MEM = 'm',
        OPT_HELP = 'h',
    };

    static struct option long_options[] = {
        {"fen",             required_argument, nullptr, OPT_FEN},
        {"perft",           required_argument, nullptr, OPT_PERFT},
        {"divide",          required_argument, nullptr, OPT_DIVIDE},
        {"search",          optional_argument, nullptr, OPT_SEARCH},
        {"bench-perftsuite", required_argument, nullptr, OPT_BENCH_PERFTSUITE},
        {"bench-wac",       required_argument, nullptr, OPT_BENCH_WAC},
        {"wac-id",          required_argument, nullptr, OPT_WAC_ID},
        {"tests",           no_argument,       nullptr, OPT_TESTS},
        {"mem",             required_argument, nullptr, OPT_MEM},
        {"help",            no_argument,       nullptr, OPT_HELP},
        {nullptr, 0, nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "f:p:d:s::P:w:i:m:h", long_options, nullptr)) != -1) {
        switch (opt) {
        case OPT_FEN:
            fen = optarg;
            break;
        case OPT_PERFT:
            perft_depth = std::stoi(optarg);
            break;
        case OPT_DIVIDE:
            divide_depth = std::stoi(optarg);
            break;
        case OPT_SEARCH:
            search_time = optarg ? std::stoi(optarg) : 10000;
            break;
        case OPT_BENCH_PERFTSUITE:
            perftsuite_file = optarg;
            if (optind < argc && argv[optind][0] != '-') {
                perftsuite_max_depth = std::stoi(argv[optind++]);
            }
            break;
        case OPT_BENCH_WAC:
            wac_file = optarg;
            if (optind < argc && argv[optind][0] != '-') {
                wac_time_ms = std::stoi(argv[optind++]);
            }
            break;
        case OPT_WAC_ID:
            wac_id = optarg;
            break;
        case OPT_TESTS:
            run_tests = true;
            break;
        case OPT_MEM:
            mem_mb = std::stoul(optarg);
            break;
        case OPT_HELP:
            print_usage(argv[0]);
            return 0;
        default:
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

    if (run_tests) {
        return run_draw_tests(1000, mem_mb);
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
        SearchResult result = search(board, tt, search_time);
        std::cout << "bestmove " << result.best_move.to_uci() << std::endl;
    } else {
        // Default: UCI mode
        uci_loop(mem_mb);
    }

    return 0;
}
