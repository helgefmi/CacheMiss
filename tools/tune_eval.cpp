#include "board.hpp"
#include "move.hpp"
#include "precalc.hpp"
#include "eval_params.hpp"
#include "zobrist.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

// ============================================================================
// Constants
// ============================================================================

constexpr double LN10 = 2.302585092994046;
constexpr int MAX_PIECE_INSTANCES = 10;  // Max of each piece type per side

// Space evaluation zones (from eval.cpp)
constexpr Bitboard CENTER_4 = 0x0000001818000000ULL;         // d4, d5, e4, e5
constexpr Bitboard EXTENDED_CENTER = 0x00003C3C3C3C0000ULL;  // c3-f6 region

// ============================================================================
// PGN Parser (adapted from tune_pst.cpp)
// ============================================================================

Move32 parse_san_move(const std::string& san, Board& board) {
    if (san.empty()) return Move32(0);

    // Handle castling
    if (san == "O-O" || san == "0-0" || san == "O-O+" || san == "0-0+" || san == "O-O#" || san == "0-0#") {
        MoveList moves = generate_moves(board);
        for (int i = 0; i < moves.size; ++i) {
            Move32& m = moves[i];
            if (m.is_castling()) {
                int from_file = m.from() % 8;
                int to_file = m.to() % 8;
                if (to_file > from_file) {  // Kingside
                    make_move(board, m);
                    if (!is_illegal(board)) {
                        unmake_move(board, m);
                        return m;
                    }
                    unmake_move(board, m);
                }
            }
        }
        return Move32(0);
    }

    if (san == "O-O-O" || san == "0-0-0" || san == "O-O-O+" || san == "0-0-0+" || san == "O-O-O#" || san == "0-0-0#") {
        MoveList moves = generate_moves(board);
        for (int i = 0; i < moves.size; ++i) {
            Move32& m = moves[i];
            if (m.is_castling()) {
                int from_file = m.from() % 8;
                int to_file = m.to() % 8;
                if (to_file < from_file) {  // Queenside
                    make_move(board, m);
                    if (!is_illegal(board)) {
                        unmake_move(board, m);
                        return m;
                    }
                    unmake_move(board, m);
                }
            }
        }
        return Move32(0);
    }

    // Parse SAN components
    size_t pos = 0;
    Piece piece = Piece::Pawn;
    int disambig_file = -1;
    int disambig_rank = -1;
    int to_file = -1;
    int to_rank = -1;
    Piece promotion = Piece::None;

    // Check for piece letter
    if (pos < san.size() && san[pos] >= 'A' && san[pos] <= 'Z' && san[pos] != 'O') {
        switch (san[pos]) {
            case 'N': piece = Piece::Knight; break;
            case 'B': piece = Piece::Bishop; break;
            case 'R': piece = Piece::Rook; break;
            case 'Q': piece = Piece::Queen; break;
            case 'K': piece = Piece::King; break;
            default: return Move32(0);
        }
        pos++;
    }

    // Parse file/rank disambiguation and target square
    std::string coords;
    for (size_t i = pos; i < san.size(); ++i) {
        char c = san[i];
        if (c >= 'a' && c <= 'h') {
            coords += c;
        } else if (c >= '1' && c <= '8') {
            coords += c;
        } else if (c == 'x') {
            // Capture marker, skip
        } else if (c == '+' || c == '#') {
            // Check/checkmate markers, skip
        } else if (c == '=') {
            // Promotion follows
            if (i + 1 < san.size()) {
                switch (san[i + 1]) {
                    case 'Q': promotion = Piece::Queen; break;
                    case 'R': promotion = Piece::Rook; break;
                    case 'B': promotion = Piece::Bishop; break;
                    case 'N': promotion = Piece::Knight; break;
                }
            }
            break;
        }
    }

    // Last two characters of coords should be target square
    if (coords.size() >= 2) {
        size_t target_start = coords.size() - 2;
        if (coords[target_start] >= 'a' && coords[target_start] <= 'h' &&
            coords[target_start + 1] >= '1' && coords[target_start + 1] <= '8') {
            to_file = coords[target_start] - 'a';
            to_rank = coords[target_start + 1] - '1';

            for (size_t i = 0; i < target_start; ++i) {
                if (coords[i] >= 'a' && coords[i] <= 'h') {
                    disambig_file = coords[i] - 'a';
                } else if (coords[i] >= '1' && coords[i] <= '8') {
                    disambig_rank = coords[i] - '1';
                }
            }
        }
    }

    if (to_file < 0 || to_rank < 0) {
        return Move32(0);
    }

    int to_sq = to_rank * 8 + to_file;

    // Find matching legal move
    MoveList moves = generate_moves(board);
    for (int i = 0; i < moves.size; ++i) {
        Move32& m = moves[i];

        if (m.to() != to_sq) continue;

        Piece moving_piece = board.pieces_on_square[m.from()];
        if (moving_piece != piece) continue;

        int from_file = m.from() % 8;
        int from_rank = m.from() / 8;
        if (disambig_file >= 0 && from_file != disambig_file) continue;
        if (disambig_rank >= 0 && from_rank != disambig_rank) continue;

        if (promotion != Piece::None) {
            if (m.promotion() != promotion) continue;
        } else {
            if (m.is_promotion()) continue;
        }

        make_move(board, m);
        bool legal = !is_illegal(board);
        unmake_move(board, m);

        if (legal) {
            return m;
        }
    }

    return Move32(0);
}

struct PGNGame {
    std::map<std::string, std::string> headers;
    std::vector<std::string> moves;
};

class PGNParser {
    std::ifstream& file;
    std::string line;
    bool has_line = false;

public:
    PGNParser(std::ifstream& f) : file(f) {}

    bool next_game(PGNGame& game) {
        game.headers.clear();
        game.moves.clear();

        // Skip empty lines and find headers
        while (true) {
            if (!has_line) {
                if (!std::getline(file, line)) return false;
            }
            has_line = false;

            if (line.empty()) continue;
            if (line[0] == '[') break;
        }

        // Parse headers
        do {
            if (line.empty() || line[0] != '[') break;

            size_t space = line.find(' ');
            if (space != std::string::npos && line.back() == ']') {
                std::string tag = line.substr(1, space - 1);
                size_t quote1 = line.find('"');
                size_t quote2 = line.rfind('"');
                if (quote1 != std::string::npos && quote2 > quote1) {
                    std::string value = line.substr(quote1 + 1, quote2 - quote1 - 1);
                    game.headers[tag] = value;
                }
            }
        } while (std::getline(file, line));

        // Parse moves
        std::string move_text;
        while (std::getline(file, line)) {
            if (line.empty()) {
                has_line = false;
                break;
            }
            if (line[0] == '[') {
                has_line = true;
                break;
            }
            move_text += " " + line;
        }

        // Tokenize moves
        std::istringstream iss(move_text);
        std::string token;
        while (iss >> token) {
            if (!token.empty() && (std::isdigit(token[0]) || token[0] == '.')) {
                bool is_number = true;
                for (char c : token) {
                    if (!std::isdigit(c) && c != '.') {
                        is_number = false;
                        break;
                    }
                }
                if (is_number) continue;
            }

            if (token == "1-0" || token == "0-1" || token == "1/2-1/2" || token == "*") {
                continue;
            }

            if (token[0] == '{') {
                while (!token.empty() && token.back() != '}' && iss >> token) {}
                continue;
            }

            if (token[0] == '$' || token == "!" || token == "?" ||
                token == "!!" || token == "??" || token == "!?" || token == "?!") {
                continue;
            }

            game.moves.push_back(token);
        }

        return true;
    }

    bool eof() const { return !file.good(); }
};

// ============================================================================
// EvalParams - All Tunable Parameters
// ============================================================================

struct EvalParams {
    // PST tables [Piece][Square]
    std::array<std::array<double, 64>, 6> pst_mg;
    std::array<std::array<double, 64>, 6> pst_eg;

    // Mobility tables
    std::array<double, 9> mobility_knight_mg;
    std::array<double, 9> mobility_knight_eg;
    std::array<double, 14> mobility_bishop_mg;
    std::array<double, 14> mobility_bishop_eg;
    std::array<double, 15> mobility_rook_mg;
    std::array<double, 15> mobility_rook_eg;
    std::array<double, 28> mobility_queen_mg;
    std::array<double, 28> mobility_queen_eg;

    // Positional bonuses (scalars)
    double bishop_pair_mg, bishop_pair_eg;
    double rook_open_file_mg, rook_open_file_eg;
    double rook_semi_open_file_mg, rook_semi_open_file_eg;
    double rook_on_seventh_mg, rook_on_seventh_eg;

    // Pawn structure
    double doubled_pawn_mg, doubled_pawn_eg;
    double isolated_pawn_mg, isolated_pawn_eg;
    double backward_pawn_mg, backward_pawn_eg;

    // Passed pawns [rank]
    std::array<double, 8> passed_pawn_mg;
    std::array<double, 8> passed_pawn_eg;
    double protected_passer_mg, protected_passer_eg;
    double connected_passer_mg, connected_passer_eg;

    // Space and king safety
    double space_center_mg, space_center_eg;
    double space_extended_mg, space_extended_eg;
    double king_attack_mg, king_attack_eg;

    void init_from_defaults() {
        // Load PST from pst_tables.hpp
        for (int p = 0; p < 6; ++p) {
            for (int sq = 0; sq < 64; ++sq) {
                pst_mg[p][sq] = PST_MG[p][sq];
                pst_eg[p][sq] = PST_EG[p][sq];
            }
        }

        // Mobility tables (from eval_params.hpp)
        for (int i = 0; i < 9; ++i) {
            mobility_knight_mg[i] = MOBILITY_KNIGHT_MG[i];
            mobility_knight_eg[i] = MOBILITY_KNIGHT_EG[i];
        }
        for (int i = 0; i < 14; ++i) {
            mobility_bishop_mg[i] = MOBILITY_BISHOP_MG[i];
            mobility_bishop_eg[i] = MOBILITY_BISHOP_EG[i];
        }
        for (int i = 0; i < 15; ++i) {
            mobility_rook_mg[i] = MOBILITY_ROOK_MG[i];
            mobility_rook_eg[i] = MOBILITY_ROOK_EG[i];
        }
        for (int i = 0; i < 28; ++i) {
            mobility_queen_mg[i] = MOBILITY_QUEEN_MG[i];
            mobility_queen_eg[i] = MOBILITY_QUEEN_EG[i];
        }

        // Positional bonuses
        bishop_pair_mg = BISHOP_PAIR_MG; bishop_pair_eg = BISHOP_PAIR_EG;
        rook_open_file_mg = ROOK_OPEN_FILE_MG; rook_open_file_eg = ROOK_OPEN_FILE_EG;
        rook_semi_open_file_mg = ROOK_SEMI_OPEN_FILE_MG; rook_semi_open_file_eg = ROOK_SEMI_OPEN_FILE_EG;
        rook_on_seventh_mg = ROOK_ON_SEVENTH_MG; rook_on_seventh_eg = ROOK_ON_SEVENTH_EG;

        // Pawn structure
        doubled_pawn_mg = DOUBLED_PAWN_MG; doubled_pawn_eg = DOUBLED_PAWN_EG;
        isolated_pawn_mg = ISOLATED_PAWN_MG; isolated_pawn_eg = ISOLATED_PAWN_EG;
        backward_pawn_mg = BACKWARD_PAWN_MG; backward_pawn_eg = BACKWARD_PAWN_EG;

        // Passed pawns
        for (int i = 0; i < 8; ++i) {
            passed_pawn_mg[i] = PASSED_PAWN_MG[i];
            passed_pawn_eg[i] = PASSED_PAWN_EG[i];
        }
        protected_passer_mg = PROTECTED_PASSER_MG; protected_passer_eg = PROTECTED_PASSER_EG;
        connected_passer_mg = CONNECTED_PASSER_MG; connected_passer_eg = CONNECTED_PASSER_EG;

        // Space and king safety
        space_center_mg = SPACE_CENTER_MG; space_center_eg = SPACE_CENTER_EG;
        space_extended_mg = SPACE_EXTENDED_MG; space_extended_eg = SPACE_EXTENDED_EG;
        king_attack_mg = KING_ATTACK_MG; king_attack_eg = KING_ATTACK_EG;
    }
};

// ============================================================================
// TrainingPosition - Precomputed Features
// ============================================================================

struct TrainingPosition {
    // Piece positions (for PST)
    std::array<std::array<Bitboard, 6>, 2> pieces;  // [Color][Piece]
    std::array<int, 2> king_sq;

    // Mobility counts per piece instance
    std::array<std::array<u8, MAX_PIECE_INSTANCES>, 2> knight_mob;
    std::array<std::array<u8, MAX_PIECE_INSTANCES>, 2> bishop_mob;
    std::array<std::array<u8, MAX_PIECE_INSTANCES>, 2> rook_mob;
    std::array<std::array<u8, MAX_PIECE_INSTANCES>, 2> queen_mob;
    std::array<u8, 2> num_knights, num_bishops, num_rooks, num_queens;

    // Pawn structure counts
    std::array<u8, 2> doubled_pawns, isolated_pawns, backward_pawns;
    std::array<std::array<u8, 8>, 2> passed_pawn_by_rank;  // [color][effective_rank]
    std::array<u8, 2> protected_passers, connected_passers;

    // Positional features
    std::array<u8, 2> has_bishop_pair;  // 0 or 1
    std::array<u8, 2> rooks_open_file, rooks_semi_open, rooks_on_seventh;

    // Space and king safety (differences, not per-side)
    int center_control_diff;   // white - black
    int extended_center_diff;
    int king_attack_diff;

    float phase;    // 0.0 (endgame) to 1.0 (middlegame)
    float outcome;  // 1.0 white win, 0.5 draw, 0.0 black win
};

// ============================================================================
// Gradients Structure
// ============================================================================

struct Gradients {
    // PST tables
    std::array<std::array<double, 64>, 6> pst_mg;
    std::array<std::array<double, 64>, 6> pst_eg;
    std::array<std::array<int, 64>, 6> pst_counts;

    // Mobility tables
    std::array<double, 9> mobility_knight_mg;
    std::array<double, 9> mobility_knight_eg;
    std::array<int, 9> mobility_knight_counts;
    std::array<double, 14> mobility_bishop_mg;
    std::array<double, 14> mobility_bishop_eg;
    std::array<int, 14> mobility_bishop_counts;
    std::array<double, 15> mobility_rook_mg;
    std::array<double, 15> mobility_rook_eg;
    std::array<int, 15> mobility_rook_counts;
    std::array<double, 28> mobility_queen_mg;
    std::array<double, 28> mobility_queen_eg;
    std::array<int, 28> mobility_queen_counts;

    // Scalar gradients
    double bishop_pair_mg, bishop_pair_eg;
    int bishop_pair_count;
    double rook_open_file_mg, rook_open_file_eg;
    int rook_open_file_count;
    double rook_semi_open_file_mg, rook_semi_open_file_eg;
    int rook_semi_open_file_count;
    double rook_on_seventh_mg, rook_on_seventh_eg;
    int rook_on_seventh_count;

    double doubled_pawn_mg, doubled_pawn_eg;
    int doubled_pawn_count;
    double isolated_pawn_mg, isolated_pawn_eg;
    int isolated_pawn_count;
    double backward_pawn_mg, backward_pawn_eg;
    int backward_pawn_count;

    std::array<double, 8> passed_pawn_mg;
    std::array<double, 8> passed_pawn_eg;
    std::array<int, 8> passed_pawn_counts;
    double protected_passer_mg, protected_passer_eg;
    int protected_passer_count;
    double connected_passer_mg, connected_passer_eg;
    int connected_passer_count;

    double space_center_mg, space_center_eg;
    int space_center_count;
    double space_extended_mg, space_extended_eg;
    int space_extended_count;
    double king_attack_mg, king_attack_eg;
    int king_attack_count;

    void clear() {
        for (int p = 0; p < 6; ++p) {
            for (int sq = 0; sq < 64; ++sq) {
                pst_mg[p][sq] = 0.0;
                pst_eg[p][sq] = 0.0;
                pst_counts[p][sq] = 0;
            }
        }

        for (int i = 0; i < 9; ++i) {
            mobility_knight_mg[i] = mobility_knight_eg[i] = 0.0;
            mobility_knight_counts[i] = 0;
        }
        for (int i = 0; i < 14; ++i) {
            mobility_bishop_mg[i] = mobility_bishop_eg[i] = 0.0;
            mobility_bishop_counts[i] = 0;
        }
        for (int i = 0; i < 15; ++i) {
            mobility_rook_mg[i] = mobility_rook_eg[i] = 0.0;
            mobility_rook_counts[i] = 0;
        }
        for (int i = 0; i < 28; ++i) {
            mobility_queen_mg[i] = mobility_queen_eg[i] = 0.0;
            mobility_queen_counts[i] = 0;
        }

        bishop_pair_mg = bishop_pair_eg = 0.0; bishop_pair_count = 0;
        rook_open_file_mg = rook_open_file_eg = 0.0; rook_open_file_count = 0;
        rook_semi_open_file_mg = rook_semi_open_file_eg = 0.0; rook_semi_open_file_count = 0;
        rook_on_seventh_mg = rook_on_seventh_eg = 0.0; rook_on_seventh_count = 0;

        doubled_pawn_mg = doubled_pawn_eg = 0.0; doubled_pawn_count = 0;
        isolated_pawn_mg = isolated_pawn_eg = 0.0; isolated_pawn_count = 0;
        backward_pawn_mg = backward_pawn_eg = 0.0; backward_pawn_count = 0;

        for (int i = 0; i < 8; ++i) {
            passed_pawn_mg[i] = passed_pawn_eg[i] = 0.0;
            passed_pawn_counts[i] = 0;
        }
        protected_passer_mg = protected_passer_eg = 0.0; protected_passer_count = 0;
        connected_passer_mg = connected_passer_eg = 0.0; connected_passer_count = 0;

        space_center_mg = space_center_eg = 0.0; space_center_count = 0;
        space_extended_mg = space_extended_eg = 0.0; space_extended_count = 0;
        king_attack_mg = king_attack_eg = 0.0; king_attack_count = 0;
    }
};

// ============================================================================
// Configuration
// ============================================================================

struct Config {
    std::string input_pgn;
    std::string output_file;
    double K = 400.0;
    double learning_rate = 300.0;
    int epochs = 1000;
    int min_elo = 2200;
    int min_time = 180;
    int skip_moves = 8;
    int max_games = 1000000;
    int max_positions = 0;
    bool verbose = false;
};

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " <input.pgn> [options]\n"
              << "Options:\n"
              << "  -o <file>         Output file (default: stdout)\n"
              << "  -K <value>        Sigmoid scaling factor (default: 400)\n"
              << "  -lr <value>       Learning rate (default: 10)\n"
              << "  -epochs <n>       Number of epochs (default: 1000)\n"
              << "  -min-elo <n>      Minimum average rating (default: 2200)\n"
              << "  -min-time <s>     Minimum initial time in seconds (default: 180)\n"
              << "  -skip <n>         Skip first N moves per side (default: 8)\n"
              << "  -max-games <n>    Maximum games to process (default: 1000000)\n"
              << "  -max-pos <n>      Maximum positions to use (default: unlimited)\n"
              << "  -v                Verbose output\n";
}

Config parse_args(int argc, char* argv[]) {
    Config cfg;
    if (argc < 2) {
        print_usage(argv[0]);
        exit(1);
    }

    cfg.input_pgn = argv[1];

    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            cfg.output_file = argv[++i];
        } else if (strcmp(argv[i], "-K") == 0 && i + 1 < argc) {
            cfg.K = std::stod(argv[++i]);
        } else if (strcmp(argv[i], "-lr") == 0 && i + 1 < argc) {
            cfg.learning_rate = std::stod(argv[++i]);
        } else if (strcmp(argv[i], "-epochs") == 0 && i + 1 < argc) {
            cfg.epochs = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "-min-elo") == 0 && i + 1 < argc) {
            cfg.min_elo = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "-min-time") == 0 && i + 1 < argc) {
            cfg.min_time = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "-skip") == 0 && i + 1 < argc) {
            cfg.skip_moves = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "-max-games") == 0 && i + 1 < argc) {
            cfg.max_games = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "-max-pos") == 0 && i + 1 < argc) {
            cfg.max_positions = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "-v") == 0) {
            cfg.verbose = true;
        } else {
            std::cerr << "Unknown option: " << argv[i] << std::endl;
            print_usage(argv[0]);
            exit(1);
        }
    }

    return cfg;
}

// ============================================================================
// Game Filtering
// ============================================================================

int parse_time_control(const std::string& tc) {
    size_t plus = tc.find('+');
    if (plus == std::string::npos) {
        return std::stoi(tc);
    }
    return std::stoi(tc.substr(0, plus));
}

bool passes_filter(const PGNGame& game, const Config& cfg) {
    auto tc_it = game.headers.find("TimeControl");
    if (tc_it == game.headers.end()) return false;

    int initial_time = parse_time_control(tc_it->second);
    if (initial_time < cfg.min_time) return false;

    auto white_elo = game.headers.find("WhiteElo");
    auto black_elo = game.headers.find("BlackElo");
    if (white_elo == game.headers.end() || black_elo == game.headers.end()) return false;

    int avg_elo = (std::stoi(white_elo->second) + std::stoi(black_elo->second)) / 2;
    if (avg_elo < cfg.min_elo) return false;

    auto term_it = game.headers.find("Termination");
    if (term_it != game.headers.end()) {
        if (term_it->second == "Time forfeit") return false;
        if (term_it->second == "Abandoned") return false;
    }

    auto result_it = game.headers.find("Result");
    if (result_it == game.headers.end()) return false;
    const std::string& result = result_it->second;
    if (result != "1-0" && result != "0-1" && result != "1/2-1/2") return false;

    return true;
}

float parse_outcome(const std::string& result) {
    if (result == "1-0") return 1.0f;
    if (result == "0-1") return 0.0f;
    if (result == "1/2-1/2") return 0.5f;
    return -1.0f;
}

// ============================================================================
// Phase Calculation
// ============================================================================

float compute_phase(const Board& board) {
    int phase = 0;
    for (int c = 0; c < 2; ++c) {
        phase += popcount(board.pieces[c][(int)Piece::Knight]) * 1;
        phase += popcount(board.pieces[c][(int)Piece::Bishop]) * 1;
        phase += popcount(board.pieces[c][(int)Piece::Rook]) * 2;
        phase += popcount(board.pieces[c][(int)Piece::Queen]) * 4;
    }
    phase = std::min(phase, 24);
    return static_cast<float>(phase) / 24.0f;
}

// ============================================================================
// Feature Extraction (Matches eval.cpp logic)
// ============================================================================

Bitboard compute_pawn_attacks(Bitboard pawns, int color) {
    Bitboard attacks = 0;
    while (pawns) {
        attacks |= PAWN_ATTACKS[color][lsb_index(pawns)];
        pawns &= pawns - 1;
    }
    return attacks;
}

void extract_features(const Board& board, TrainingPosition& pos) {
    // Copy piece bitboards
    for (int c = 0; c < 2; ++c) {
        for (int p = 0; p < 6; ++p) {
            pos.pieces[c][p] = board.pieces[c][p];
        }
        pos.king_sq[c] = board.king_sq[c];
    }

    // Initialize counts
    for (int c = 0; c < 2; ++c) {
        pos.num_knights[c] = pos.num_bishops[c] = pos.num_rooks[c] = pos.num_queens[c] = 0;
        pos.doubled_pawns[c] = pos.isolated_pawns[c] = pos.backward_pawns[c] = 0;
        for (int i = 0; i < 8; ++i) pos.passed_pawn_by_rank[c][i] = 0;
        pos.protected_passers[c] = pos.connected_passers[c] = 0;
        pos.has_bishop_pair[c] = 0;
        pos.rooks_open_file[c] = pos.rooks_semi_open[c] = pos.rooks_on_seventh[c] = 0;
    }

    Bitboard occ = board.all_occupied;
    Bitboard attacks[2] = {0, 0};

    // Compute pawn attacks early
    Bitboard pawn_attacks[2];
    pawn_attacks[0] = compute_pawn_attacks(board.pieces[0][(int)Piece::Pawn], 0);
    pawn_attacks[1] = compute_pawn_attacks(board.pieces[1][(int)Piece::Pawn], 1);
    attacks[0] |= pawn_attacks[0];
    attacks[1] |= pawn_attacks[1];

    // Extract mobility for each piece type
    for (int c = 0; c < 2; ++c) {
        Bitboard friendly = board.occupied[c];
        Bitboard enemy_pawn_att = pawn_attacks[c ^ 1];

        // Knights
        Bitboard knights = board.pieces[c][(int)Piece::Knight];
        while (knights && pos.num_knights[c] < MAX_PIECE_INSTANCES) {
            int sq = lsb_index(knights);
            Bitboard att = KNIGHT_MOVES[sq];
            attacks[c] |= att;
            int mob = popcount(att & ~friendly & ~enemy_pawn_att);
            pos.knight_mob[c][pos.num_knights[c]++] = std::min(mob, 8);
            knights &= knights - 1;
        }

        // Bishops
        Bitboard bishops = board.pieces[c][(int)Piece::Bishop];
        int bishop_count = popcount(bishops);
        if (bishop_count >= 2) pos.has_bishop_pair[c] = 1;

        while (bishops && pos.num_bishops[c] < MAX_PIECE_INSTANCES) {
            int sq = lsb_index(bishops);
            Bitboard att = get_bishop_attacks(sq, occ);
            attacks[c] |= att;
            int mob = std::min(popcount(att & ~friendly & ~enemy_pawn_att), 13);
            pos.bishop_mob[c][pos.num_bishops[c]++] = mob;
            bishops &= bishops - 1;
        }

        // Rooks
        Bitboard rooks = board.pieces[c][(int)Piece::Rook];
        Bitboard our_pawns = board.pieces[c][(int)Piece::Pawn];
        Bitboard enemy_pawns = board.pieces[c ^ 1][(int)Piece::Pawn];
        Bitboard occ_xray_rooks = occ ^ rooks;

        while (rooks && pos.num_rooks[c] < MAX_PIECE_INSTANCES) {
            int sq = lsb_index(rooks);
            int file = sq % 8;
            int rank = sq / 8;

            Bitboard att = get_rook_attacks(sq, occ_xray_rooks);
            attacks[c] |= att;
            int mob = std::min(popcount(att & ~friendly & ~enemy_pawn_att), 14);
            pos.rook_mob[c][pos.num_rooks[c]++] = mob;

            // Open/semi-open file
            Bitboard file_mask = FILE_MASKS[file];
            bool no_our_pawns = (our_pawns & file_mask) == 0;
            bool no_enemy_pawns = (enemy_pawns & file_mask) == 0;

            if (no_our_pawns && no_enemy_pawns) {
                pos.rooks_open_file[c]++;
            } else if (no_our_pawns) {
                pos.rooks_semi_open[c]++;
            }

            // Rook on 7th rank
            int seventh_rank = (c == 0) ? 6 : 1;
            if (rank == seventh_rank) {
                pos.rooks_on_seventh[c]++;
            }

            rooks &= rooks - 1;
        }

        // Queens
        Bitboard queens = board.pieces[c][(int)Piece::Queen];
        while (queens && pos.num_queens[c] < MAX_PIECE_INSTANCES) {
            int sq = lsb_index(queens);
            Bitboard att = get_queen_attacks(sq, occ);
            attacks[c] |= att;
            int mob = std::min(popcount(att & ~friendly & ~enemy_pawn_att), 27);
            pos.queen_mob[c][pos.num_queens[c]++] = mob;
            queens &= queens - 1;
        }

        // King attacks
        attacks[c] |= KING_MOVES[board.king_sq[c]];
    }

    // Pawn structure evaluation
    for (int c = 0; c < 2; ++c) {
        Bitboard our_pawns = board.pieces[c][(int)Piece::Pawn];
        Bitboard enemy_pawns = board.pieces[c ^ 1][(int)Piece::Pawn];

        // Doubled pawns
        for (int f = 0; f < 8; ++f) {
            int pawns_on_file = popcount(our_pawns & FILE_MASKS[f]);
            if (pawns_on_file > 1) {
                pos.doubled_pawns[c] += pawns_on_file - 1;
            }
        }

        // Isolated and backward pawns
        Bitboard pawns = our_pawns;
        while (pawns) {
            int sq = lsb_index(pawns);
            int rank = sq / 8;
            int file = sq % 8;

            bool is_isolated = (our_pawns & ADJACENT_FILES[file]) == 0;
            if (is_isolated) {
                pos.isolated_pawns[c]++;
            } else {
                int eff_rank = (c == 0) ? rank : (7 - rank);
                if (eff_rank > 1) {
                    Bitboard defenders_zone;
                    if (c == 0) {
                        Bitboard behind_mask = (1ULL << (rank * 8)) - 1;
                        defenders_zone = ADJACENT_FILES[file] & behind_mask;
                    } else {
                        Bitboard behind_mask = ~((1ULL << ((rank + 1) * 8)) - 1);
                        defenders_zone = ADJACENT_FILES[file] & behind_mask;
                    }

                    bool no_defenders = (our_pawns & defenders_zone) == 0;
                    int stop_sq = (c == 0) ? sq + 8 : sq - 8;
                    bool stop_attacked = (stop_sq >= 0 && stop_sq < 64) &&
                                        (PAWN_ATTACKS[c ^ 1][stop_sq] & enemy_pawns);

                    if (no_defenders && stop_attacked) {
                        pos.backward_pawns[c]++;
                    }
                }
            }

            pawns &= pawns - 1;
        }

        // Passed pawns
        Bitboard passed = 0;
        pawns = our_pawns;
        while (pawns) {
            int sq = lsb_index(pawns);
            if ((PASSED_PAWN_MASK[c][sq] & enemy_pawns) == 0)
                passed |= (1ULL << sq);
            pawns &= pawns - 1;
        }

        while (passed) {
            int sq = lsb_index(passed);
            int rank = sq / 8;
            int file = sq % 8;
            int eff_rank = (c == 0) ? rank : (7 - rank);

            pos.passed_pawn_by_rank[c][eff_rank]++;

            // Protected passer
            if (PAWN_ATTACKS[c ^ 1][sq] & our_pawns) {
                pos.protected_passers[c]++;
            }

            // Connected passer
            if (ADJACENT_FILES[file] & passed) {
                pos.connected_passers[c]++;
            }

            passed &= passed - 1;
        }
    }

    // Space and king safety (compute differences)
    pos.center_control_diff = popcount(attacks[0] & CENTER_4) - popcount(attacks[1] & CENTER_4);
    pos.extended_center_diff = popcount(attacks[0] & EXTENDED_CENTER) - popcount(attacks[1] & EXTENDED_CENTER);

    Bitboard white_king_zone = KING_MOVES[board.king_sq[0]] | square_bb(board.king_sq[0]);
    Bitboard black_king_zone = KING_MOVES[board.king_sq[1]] | square_bb(board.king_sq[1]);
    int white_king_pressure = popcount(attacks[0] & black_king_zone);
    int black_king_pressure = popcount(attacks[1] & white_king_zone);
    pos.king_attack_diff = white_king_pressure - black_king_pressure;

    pos.phase = compute_phase(board);
}

// ============================================================================
// Evaluation using EvalParams
// ============================================================================

double evaluate(const EvalParams& params, const TrainingPosition& pos) {
    double mg_score = 0.0, eg_score = 0.0;

    // PST evaluation
    for (int piece = 0; piece < 6; ++piece) {
        // White pieces
        Bitboard white_bb = pos.pieces[0][piece];
        while (white_bb) {
            int sq = lsb_index(white_bb);
            mg_score += params.pst_mg[piece][sq];
            eg_score += params.pst_eg[piece][sq];
            white_bb &= white_bb - 1;
        }

        // Black pieces (flipped square)
        Bitboard black_bb = pos.pieces[1][piece];
        while (black_bb) {
            int sq = lsb_index(black_bb);
            int flipped_sq = sq ^ 56;
            mg_score -= params.pst_mg[piece][flipped_sq];
            eg_score -= params.pst_eg[piece][flipped_sq];
            black_bb &= black_bb - 1;
        }
    }

    // Mobility
    for (int c = 0; c < 2; ++c) {
        int sign = (c == 0) ? 1 : -1;

        for (int i = 0; i < pos.num_knights[c]; ++i) {
            int mob = pos.knight_mob[c][i];
            mg_score += sign * params.mobility_knight_mg[mob];
            eg_score += sign * params.mobility_knight_eg[mob];
        }

        for (int i = 0; i < pos.num_bishops[c]; ++i) {
            int mob = pos.bishop_mob[c][i];
            mg_score += sign * params.mobility_bishop_mg[mob];
            eg_score += sign * params.mobility_bishop_eg[mob];
        }

        for (int i = 0; i < pos.num_rooks[c]; ++i) {
            int mob = pos.rook_mob[c][i];
            mg_score += sign * params.mobility_rook_mg[mob];
            eg_score += sign * params.mobility_rook_eg[mob];
        }

        for (int i = 0; i < pos.num_queens[c]; ++i) {
            int mob = pos.queen_mob[c][i];
            mg_score += sign * params.mobility_queen_mg[mob];
            eg_score += sign * params.mobility_queen_eg[mob];
        }
    }

    // Positional bonuses
    for (int c = 0; c < 2; ++c) {
        int sign = (c == 0) ? 1 : -1;

        if (pos.has_bishop_pair[c]) {
            mg_score += sign * params.bishop_pair_mg;
            eg_score += sign * params.bishop_pair_eg;
        }

        mg_score += sign * pos.rooks_open_file[c] * params.rook_open_file_mg;
        eg_score += sign * pos.rooks_open_file[c] * params.rook_open_file_eg;
        mg_score += sign * pos.rooks_semi_open[c] * params.rook_semi_open_file_mg;
        eg_score += sign * pos.rooks_semi_open[c] * params.rook_semi_open_file_eg;
        mg_score += sign * pos.rooks_on_seventh[c] * params.rook_on_seventh_mg;
        eg_score += sign * pos.rooks_on_seventh[c] * params.rook_on_seventh_eg;
    }

    // Pawn structure
    for (int c = 0; c < 2; ++c) {
        int sign = (c == 0) ? 1 : -1;

        mg_score += sign * pos.doubled_pawns[c] * params.doubled_pawn_mg;
        eg_score += sign * pos.doubled_pawns[c] * params.doubled_pawn_eg;
        mg_score += sign * pos.isolated_pawns[c] * params.isolated_pawn_mg;
        eg_score += sign * pos.isolated_pawns[c] * params.isolated_pawn_eg;
        mg_score += sign * pos.backward_pawns[c] * params.backward_pawn_mg;
        eg_score += sign * pos.backward_pawns[c] * params.backward_pawn_eg;

        for (int r = 0; r < 8; ++r) {
            mg_score += sign * pos.passed_pawn_by_rank[c][r] * params.passed_pawn_mg[r];
            eg_score += sign * pos.passed_pawn_by_rank[c][r] * params.passed_pawn_eg[r];
        }

        mg_score += sign * pos.protected_passers[c] * params.protected_passer_mg;
        eg_score += sign * pos.protected_passers[c] * params.protected_passer_eg;
        mg_score += sign * pos.connected_passers[c] * params.connected_passer_mg;
        eg_score += sign * pos.connected_passers[c] * params.connected_passer_eg;
    }

    // Space and king safety (already computed as differences)
    mg_score += pos.center_control_diff * params.space_center_mg;
    eg_score += pos.center_control_diff * params.space_center_eg;
    mg_score += pos.extended_center_diff * params.space_extended_mg;
    eg_score += pos.extended_center_diff * params.space_extended_eg;
    mg_score += pos.king_attack_diff * params.king_attack_mg;
    eg_score += pos.king_attack_diff * params.king_attack_eg;

    // Interpolate
    return pos.phase * mg_score + (1.0 - pos.phase) * eg_score;
}

// ============================================================================
// Position Extraction from Games
// ============================================================================

void extract_positions(const PGNGame& game, const Config& cfg,
                       std::vector<TrainingPosition>& positions) {
    float outcome = parse_outcome(game.headers.at("Result"));
    if (outcome < 0.0f) return;

    Board board;
    int ply = 0;

    for (const std::string& san : game.moves) {
        if (ply >= 250) break;

        Move32 move = parse_san_move(san, board);
        if (move.data == 0) throw std::runtime_error("Invalid SAN move: " + san);

        make_move(board, move);
        ply++;

        if (ply < cfg.skip_moves * 2) continue;
        if (ply % 10 != 0) continue;

        TrainingPosition pos;
        extract_features(board, pos);
        pos.outcome = outcome;

        positions.push_back(pos);
    }
}

// ============================================================================
// Gradient Descent
// ============================================================================

double sigmoid(double eval, double K) {
    return 1.0 / (1.0 + std::pow(10.0, -eval / K));
}

double compute_mse(const EvalParams& params, const std::vector<TrainingPosition>& positions,
                   double K) {
    double total_error = 0.0;
    for (const auto& pos : positions) {
        double eval = evaluate(params, pos);
        double predicted = sigmoid(eval, K);
        double error = predicted - pos.outcome;
        total_error += error * error;
    }
    return total_error / positions.size();
}

void gradient_descent_step(EvalParams& params, const std::vector<TrainingPosition>& positions,
                           const Config& cfg) {
    Gradients grad;
    grad.clear();

    for (const auto& pos : positions) {
        double eval = evaluate(params, pos);
        double predicted = sigmoid(eval, cfg.K);
        double sigmoid_deriv = predicted * (1.0 - predicted) * LN10 / cfg.K;
        double base_grad = 2.0 * (predicted - pos.outcome) * sigmoid_deriv;

        double grad_mg = base_grad * pos.phase;
        double grad_eg = base_grad * (1.0 - pos.phase);

        // PST gradients
        for (int piece = 0; piece < 6; ++piece) {
            Bitboard white_bb = pos.pieces[0][piece];
            while (white_bb) {
                int sq = lsb_index(white_bb);
                grad.pst_mg[piece][sq] += grad_mg;
                grad.pst_eg[piece][sq] += grad_eg;
                grad.pst_counts[piece][sq]++;
                white_bb &= white_bb - 1;
            }

            Bitboard black_bb = pos.pieces[1][piece];
            while (black_bb) {
                int sq = lsb_index(black_bb);
                int flipped_sq = sq ^ 56;
                grad.pst_mg[piece][flipped_sq] -= grad_mg;
                grad.pst_eg[piece][flipped_sq] -= grad_eg;
                grad.pst_counts[piece][flipped_sq]++;
                black_bb &= black_bb - 1;
            }
        }

        // Mobility gradients
        for (int c = 0; c < 2; ++c) {
            int sign = (c == 0) ? 1 : -1;

            for (int i = 0; i < pos.num_knights[c]; ++i) {
                int mob = pos.knight_mob[c][i];
                grad.mobility_knight_mg[mob] += sign * grad_mg;
                grad.mobility_knight_eg[mob] += sign * grad_eg;
                grad.mobility_knight_counts[mob]++;
            }

            for (int i = 0; i < pos.num_bishops[c]; ++i) {
                int mob = pos.bishop_mob[c][i];
                grad.mobility_bishop_mg[mob] += sign * grad_mg;
                grad.mobility_bishop_eg[mob] += sign * grad_eg;
                grad.mobility_bishop_counts[mob]++;
            }

            for (int i = 0; i < pos.num_rooks[c]; ++i) {
                int mob = pos.rook_mob[c][i];
                grad.mobility_rook_mg[mob] += sign * grad_mg;
                grad.mobility_rook_eg[mob] += sign * grad_eg;
                grad.mobility_rook_counts[mob]++;
            }

            for (int i = 0; i < pos.num_queens[c]; ++i) {
                int mob = pos.queen_mob[c][i];
                grad.mobility_queen_mg[mob] += sign * grad_mg;
                grad.mobility_queen_eg[mob] += sign * grad_eg;
                grad.mobility_queen_counts[mob]++;
            }
        }

        // Positional gradients
        {
            for (int c = 0; c < 2; ++c) {
                int sign = (c == 0) ? 1 : -1;

                if (pos.has_bishop_pair[c]) {
                    grad.bishop_pair_mg += sign * grad_mg;
                    grad.bishop_pair_eg += sign * grad_eg;
                    grad.bishop_pair_count++;
                }

                if (pos.rooks_open_file[c] > 0) {
                    grad.rook_open_file_mg += sign * pos.rooks_open_file[c] * grad_mg;
                    grad.rook_open_file_eg += sign * pos.rooks_open_file[c] * grad_eg;
                    grad.rook_open_file_count += pos.rooks_open_file[c];
                }

                if (pos.rooks_semi_open[c] > 0) {
                    grad.rook_semi_open_file_mg += sign * pos.rooks_semi_open[c] * grad_mg;
                    grad.rook_semi_open_file_eg += sign * pos.rooks_semi_open[c] * grad_eg;
                    grad.rook_semi_open_file_count += pos.rooks_semi_open[c];
                }

                if (pos.rooks_on_seventh[c] > 0) {
                    grad.rook_on_seventh_mg += sign * pos.rooks_on_seventh[c] * grad_mg;
                    grad.rook_on_seventh_eg += sign * pos.rooks_on_seventh[c] * grad_eg;
                    grad.rook_on_seventh_count += pos.rooks_on_seventh[c];
                }
            }

            // Space and king safety
            if (pos.center_control_diff != 0) {
                grad.space_center_mg += pos.center_control_diff * grad_mg;
                grad.space_center_eg += pos.center_control_diff * grad_eg;
                grad.space_center_count += std::abs(pos.center_control_diff);
            }

            if (pos.extended_center_diff != 0) {
                grad.space_extended_mg += pos.extended_center_diff * grad_mg;
                grad.space_extended_eg += pos.extended_center_diff * grad_eg;
                grad.space_extended_count += std::abs(pos.extended_center_diff);
            }

            if (pos.king_attack_diff != 0) {
                grad.king_attack_mg += pos.king_attack_diff * grad_mg;
                grad.king_attack_eg += pos.king_attack_diff * grad_eg;
                grad.king_attack_count += std::abs(pos.king_attack_diff);
            }
        }

        // Pawn structure gradients
        for (int c = 0; c < 2; ++c) {
            int sign = (c == 0) ? 1 : -1;

            if (pos.doubled_pawns[c] > 0) {
                grad.doubled_pawn_mg += sign * pos.doubled_pawns[c] * grad_mg;
                grad.doubled_pawn_eg += sign * pos.doubled_pawns[c] * grad_eg;
                grad.doubled_pawn_count += pos.doubled_pawns[c];
            }

            if (pos.isolated_pawns[c] > 0) {
                grad.isolated_pawn_mg += sign * pos.isolated_pawns[c] * grad_mg;
                grad.isolated_pawn_eg += sign * pos.isolated_pawns[c] * grad_eg;
                grad.isolated_pawn_count += pos.isolated_pawns[c];
            }

            if (pos.backward_pawns[c] > 0) {
                grad.backward_pawn_mg += sign * pos.backward_pawns[c] * grad_mg;
                grad.backward_pawn_eg += sign * pos.backward_pawns[c] * grad_eg;
                grad.backward_pawn_count += pos.backward_pawns[c];
            }

            for (int r = 0; r < 8; ++r) {
                if (pos.passed_pawn_by_rank[c][r] > 0) {
                    grad.passed_pawn_mg[r] += sign * pos.passed_pawn_by_rank[c][r] * grad_mg;
                    grad.passed_pawn_eg[r] += sign * pos.passed_pawn_by_rank[c][r] * grad_eg;
                    grad.passed_pawn_counts[r] += pos.passed_pawn_by_rank[c][r];
                }
            }

            if (pos.protected_passers[c] > 0) {
                grad.protected_passer_mg += sign * pos.protected_passers[c] * grad_mg;
                grad.protected_passer_eg += sign * pos.protected_passers[c] * grad_eg;
                grad.protected_passer_count += pos.protected_passers[c];
            }

            if (pos.connected_passers[c] > 0) {
                grad.connected_passer_mg += sign * pos.connected_passers[c] * grad_mg;
                grad.connected_passer_eg += sign * pos.connected_passers[c] * grad_eg;
                grad.connected_passer_count += pos.connected_passers[c];
            }
        }
    }

    // Apply gradients
    double lr = cfg.learning_rate;

    // PST
    for (int piece = 0; piece < 6; ++piece) {
        for (int sq = 0; sq < 64; ++sq) {
            if (grad.pst_counts[piece][sq] > 0) {
                double avg_mg = grad.pst_mg[piece][sq] / grad.pst_counts[piece][sq];
                double avg_eg = grad.pst_eg[piece][sq] / grad.pst_counts[piece][sq];
                params.pst_mg[piece][sq] -= lr * avg_mg;
                params.pst_eg[piece][sq] -= lr * avg_eg;
            }
        }
    }

    // Mobility
    for (int i = 0; i < 9; ++i) {
        if (grad.mobility_knight_counts[i] > 0) {
            params.mobility_knight_mg[i] -= lr * grad.mobility_knight_mg[i] / grad.mobility_knight_counts[i];
            params.mobility_knight_eg[i] -= lr * grad.mobility_knight_eg[i] / grad.mobility_knight_counts[i];
        }
    }
    for (int i = 0; i < 14; ++i) {
        if (grad.mobility_bishop_counts[i] > 0) {
            params.mobility_bishop_mg[i] -= lr * grad.mobility_bishop_mg[i] / grad.mobility_bishop_counts[i];
            params.mobility_bishop_eg[i] -= lr * grad.mobility_bishop_eg[i] / grad.mobility_bishop_counts[i];
        }
    }
    for (int i = 0; i < 15; ++i) {
        if (grad.mobility_rook_counts[i] > 0) {
            params.mobility_rook_mg[i] -= lr * grad.mobility_rook_mg[i] / grad.mobility_rook_counts[i];
            params.mobility_rook_eg[i] -= lr * grad.mobility_rook_eg[i] / grad.mobility_rook_counts[i];
        }
    }
    for (int i = 0; i < 28; ++i) {
        if (grad.mobility_queen_counts[i] > 0) {
            params.mobility_queen_mg[i] -= lr * grad.mobility_queen_mg[i] / grad.mobility_queen_counts[i];
            params.mobility_queen_eg[i] -= lr * grad.mobility_queen_eg[i] / grad.mobility_queen_counts[i];
        }
    }

    // Positional
    if (grad.bishop_pair_count > 0) {
        params.bishop_pair_mg -= lr * grad.bishop_pair_mg / grad.bishop_pair_count;
        params.bishop_pair_eg -= lr * grad.bishop_pair_eg / grad.bishop_pair_count;
    }
    if (grad.rook_open_file_count > 0) {
        params.rook_open_file_mg -= lr * grad.rook_open_file_mg / grad.rook_open_file_count;
        params.rook_open_file_eg -= lr * grad.rook_open_file_eg / grad.rook_open_file_count;
    }
    if (grad.rook_semi_open_file_count > 0) {
        params.rook_semi_open_file_mg -= lr * grad.rook_semi_open_file_mg / grad.rook_semi_open_file_count;
        params.rook_semi_open_file_eg -= lr * grad.rook_semi_open_file_eg / grad.rook_semi_open_file_count;
    }
    if (grad.rook_on_seventh_count > 0) {
        params.rook_on_seventh_mg -= lr * grad.rook_on_seventh_mg / grad.rook_on_seventh_count;
        params.rook_on_seventh_eg -= lr * grad.rook_on_seventh_eg / grad.rook_on_seventh_count;
    }
    if (grad.space_center_count > 0) {
        params.space_center_mg -= lr * grad.space_center_mg / grad.space_center_count;
        params.space_center_eg -= lr * grad.space_center_eg / grad.space_center_count;
    }
    if (grad.space_extended_count > 0) {
        params.space_extended_mg -= lr * grad.space_extended_mg / grad.space_extended_count;
        params.space_extended_eg -= lr * grad.space_extended_eg / grad.space_extended_count;
    }
    if (grad.king_attack_count > 0) {
        params.king_attack_mg -= lr * grad.king_attack_mg / grad.king_attack_count;
        params.king_attack_eg -= lr * grad.king_attack_eg / grad.king_attack_count;
    }

    // Pawn structure
    if (grad.doubled_pawn_count > 0) {
        params.doubled_pawn_mg -= lr * grad.doubled_pawn_mg / grad.doubled_pawn_count;
        params.doubled_pawn_eg -= lr * grad.doubled_pawn_eg / grad.doubled_pawn_count;
    }
    if (grad.isolated_pawn_count > 0) {
        params.isolated_pawn_mg -= lr * grad.isolated_pawn_mg / grad.isolated_pawn_count;
        params.isolated_pawn_eg -= lr * grad.isolated_pawn_eg / grad.isolated_pawn_count;
    }
    if (grad.backward_pawn_count > 0) {
        params.backward_pawn_mg -= lr * grad.backward_pawn_mg / grad.backward_pawn_count;
        params.backward_pawn_eg -= lr * grad.backward_pawn_eg / grad.backward_pawn_count;
    }
    for (int r = 0; r < 8; ++r) {
        if (grad.passed_pawn_counts[r] > 0) {
            params.passed_pawn_mg[r] -= lr * grad.passed_pawn_mg[r] / grad.passed_pawn_counts[r];
            params.passed_pawn_eg[r] -= lr * grad.passed_pawn_eg[r] / grad.passed_pawn_counts[r];
        }
    }
    if (grad.protected_passer_count > 0) {
        params.protected_passer_mg -= lr * grad.protected_passer_mg / grad.protected_passer_count;
        params.protected_passer_eg -= lr * grad.protected_passer_eg / grad.protected_passer_count;
    }
    if (grad.connected_passer_count > 0) {
        params.connected_passer_mg -= lr * grad.connected_passer_mg / grad.connected_passer_count;
        params.connected_passer_eg -= lr * grad.connected_passer_eg / grad.connected_passer_count;
    }
}

// ============================================================================
// Output Formatting
// ============================================================================

void print_eval_params(const EvalParams& params, std::ostream& out) {
    out << "// eval_params.hpp - Auto-generated by tune_eval\n";
    out << "#pragma once\n\n";

    const char* piece_names[] = {"Pawn", "Knight", "Bishop", "Rook", "Queen", "King"};

    // PST tables
    out << "// Piece-Square Tables - Middlegame\n";
    out << "constexpr int PST_MG[6][64] = {\n";
    for (int p = 0; p < 6; ++p) {
        out << "    // " << piece_names[p] << "\n    {";
        for (int sq = 0; sq < 64; ++sq) {
            if (sq % 8 == 0 && sq > 0) out << "\n     ";
            out << std::setw(5) << static_cast<int>(std::round(params.pst_mg[p][sq]));
            if (sq < 63) out << ",";
        }
        out << "}";
        if (p < 5) out << ",";
        out << "\n";
    }
    out << "};\n\n";

    out << "// Piece-Square Tables - Endgame\n";
    out << "constexpr int PST_EG[6][64] = {\n";
    for (int p = 0; p < 6; ++p) {
        out << "    // " << piece_names[p] << "\n    {";
        for (int sq = 0; sq < 64; ++sq) {
            if (sq % 8 == 0 && sq > 0) out << "\n     ";
            out << std::setw(5) << static_cast<int>(std::round(params.pst_eg[p][sq]));
            if (sq < 63) out << ",";
        }
        out << "}";
        if (p < 5) out << ",";
        out << "\n";
    }
    out << "};\n\n";

    // Mobility tables
    out << "// Mobility tables\n";
    out << "constexpr int MOBILITY_KNIGHT_MG[9] = {";
    for (int i = 0; i < 9; ++i) {
        out << static_cast<int>(std::round(params.mobility_knight_mg[i]));
        if (i < 8) out << ", ";
    }
    out << "};\n";

    out << "constexpr int MOBILITY_KNIGHT_EG[9] = {";
    for (int i = 0; i < 9; ++i) {
        out << static_cast<int>(std::round(params.mobility_knight_eg[i]));
        if (i < 8) out << ", ";
    }
    out << "};\n\n";

    out << "constexpr int MOBILITY_BISHOP_MG[14] = {";
    for (int i = 0; i < 14; ++i) {
        out << static_cast<int>(std::round(params.mobility_bishop_mg[i]));
        if (i < 13) out << ", ";
    }
    out << "};\n";

    out << "constexpr int MOBILITY_BISHOP_EG[14] = {";
    for (int i = 0; i < 14; ++i) {
        out << static_cast<int>(std::round(params.mobility_bishop_eg[i]));
        if (i < 13) out << ", ";
    }
    out << "};\n\n";

    out << "constexpr int MOBILITY_ROOK_MG[15] = {";
    for (int i = 0; i < 15; ++i) {
        out << static_cast<int>(std::round(params.mobility_rook_mg[i]));
        if (i < 14) out << ", ";
    }
    out << "};\n";

    out << "constexpr int MOBILITY_ROOK_EG[15] = {";
    for (int i = 0; i < 15; ++i) {
        out << static_cast<int>(std::round(params.mobility_rook_eg[i]));
        if (i < 14) out << ", ";
    }
    out << "};\n\n";

    out << "constexpr int MOBILITY_QUEEN_MG[28] = {";
    for (int i = 0; i < 28; ++i) {
        if (i > 0 && i % 10 == 0) out << "\n    ";
        out << static_cast<int>(std::round(params.mobility_queen_mg[i]));
        if (i < 27) out << ", ";
    }
    out << "};\n";

    out << "constexpr int MOBILITY_QUEEN_EG[28] = {";
    for (int i = 0; i < 28; ++i) {
        if (i > 0 && i % 10 == 0) out << "\n    ";
        out << static_cast<int>(std::round(params.mobility_queen_eg[i]));
        if (i < 27) out << ", ";
    }
    out << "};\n\n";

    // Positional bonuses
    out << "// Positional bonuses\n";
    out << "constexpr int BISHOP_PAIR_MG = " << static_cast<int>(std::round(params.bishop_pair_mg)) << ";\n";
    out << "constexpr int BISHOP_PAIR_EG = " << static_cast<int>(std::round(params.bishop_pair_eg)) << ";\n\n";

    out << "constexpr int ROOK_OPEN_FILE_MG = " << static_cast<int>(std::round(params.rook_open_file_mg)) << ";\n";
    out << "constexpr int ROOK_OPEN_FILE_EG = " << static_cast<int>(std::round(params.rook_open_file_eg)) << ";\n";
    out << "constexpr int ROOK_SEMI_OPEN_FILE_MG = " << static_cast<int>(std::round(params.rook_semi_open_file_mg)) << ";\n";
    out << "constexpr int ROOK_SEMI_OPEN_FILE_EG = " << static_cast<int>(std::round(params.rook_semi_open_file_eg)) << ";\n\n";

    out << "constexpr int ROOK_ON_SEVENTH_MG = " << static_cast<int>(std::round(params.rook_on_seventh_mg)) << ";\n";
    out << "constexpr int ROOK_ON_SEVENTH_EG = " << static_cast<int>(std::round(params.rook_on_seventh_eg)) << ";\n\n";

    // Pawn structure
    out << "// Pawn structure\n";
    out << "constexpr int DOUBLED_PAWN_MG = " << static_cast<int>(std::round(params.doubled_pawn_mg)) << ";\n";
    out << "constexpr int DOUBLED_PAWN_EG = " << static_cast<int>(std::round(params.doubled_pawn_eg)) << ";\n";
    out << "constexpr int ISOLATED_PAWN_MG = " << static_cast<int>(std::round(params.isolated_pawn_mg)) << ";\n";
    out << "constexpr int ISOLATED_PAWN_EG = " << static_cast<int>(std::round(params.isolated_pawn_eg)) << ";\n";
    out << "constexpr int BACKWARD_PAWN_MG = " << static_cast<int>(std::round(params.backward_pawn_mg)) << ";\n";
    out << "constexpr int BACKWARD_PAWN_EG = " << static_cast<int>(std::round(params.backward_pawn_eg)) << ";\n\n";

    // Passed pawns
    out << "// Passed pawns\n";
    out << "constexpr int PASSED_PAWN_MG[8] = {";
    for (int i = 0; i < 8; ++i) {
        out << static_cast<int>(std::round(params.passed_pawn_mg[i]));
        if (i < 7) out << ", ";
    }
    out << "};\n";

    out << "constexpr int PASSED_PAWN_EG[8] = {";
    for (int i = 0; i < 8; ++i) {
        out << static_cast<int>(std::round(params.passed_pawn_eg[i]));
        if (i < 7) out << ", ";
    }
    out << "};\n\n";

    out << "constexpr int PROTECTED_PASSER_MG = " << static_cast<int>(std::round(params.protected_passer_mg)) << ";\n";
    out << "constexpr int PROTECTED_PASSER_EG = " << static_cast<int>(std::round(params.protected_passer_eg)) << ";\n";
    out << "constexpr int CONNECTED_PASSER_MG = " << static_cast<int>(std::round(params.connected_passer_mg)) << ";\n";
    out << "constexpr int CONNECTED_PASSER_EG = " << static_cast<int>(std::round(params.connected_passer_eg)) << ";\n\n";

    // Space and king safety
    out << "// Space and king safety\n";
    out << "constexpr int SPACE_CENTER_MG = " << static_cast<int>(std::round(params.space_center_mg)) << ";\n";
    out << "constexpr int SPACE_CENTER_EG = " << static_cast<int>(std::round(params.space_center_eg)) << ";\n";
    out << "constexpr int SPACE_EXTENDED_MG = " << static_cast<int>(std::round(params.space_extended_mg)) << ";\n";
    out << "constexpr int SPACE_EXTENDED_EG = " << static_cast<int>(std::round(params.space_extended_eg)) << ";\n\n";

    out << "constexpr int KING_ATTACK_MG = " << static_cast<int>(std::round(params.king_attack_mg)) << ";\n";
    out << "constexpr int KING_ATTACK_EG = " << static_cast<int>(std::round(params.king_attack_eg)) << ";\n";
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    zobrist::init();

    Config cfg = parse_args(argc, argv);

    std::cerr << "Eval Parameter Tuner Configuration:\n";
    std::cerr << "  Input: " << cfg.input_pgn << "\n";
    std::cerr << "  K: " << cfg.K << "\n";
    std::cerr << "  Learning rate: " << cfg.learning_rate << "\n";
    std::cerr << "  Epochs: " << cfg.epochs << "\n";
    std::cerr << "  Min Elo: " << cfg.min_elo << "\n";
    std::cerr << "  Min time: " << cfg.min_time << "s\n";
    std::cerr << "  Skip moves: " << cfg.skip_moves << "\n";
    std::cerr << "\n";

    // Load and filter games
    std::cerr << "Loading PGN: " << cfg.input_pgn << "\n";
    std::vector<TrainingPosition> positions;

    std::ifstream infile(cfg.input_pgn);
    if (!infile) {
        std::cerr << "Error: Cannot open " << cfg.input_pgn << std::endl;
        return 1;
    }

    PGNParser parser(infile);
    PGNGame game;

    int games_loaded = 0, games_accepted = 0;
    while (parser.next_game(game)) {
        games_loaded++;

        if (cfg.max_games > 0 && games_accepted >= cfg.max_games) break;
        if (cfg.max_positions > 0 && positions.size() >= (size_t)cfg.max_positions) break;

        if (passes_filter(game, cfg)) {
            extract_positions(game, cfg, positions);
            games_accepted++;
        }

        if (games_loaded % 10000 == 0) {
            std::cerr << "  Loaded " << games_loaded << " games, "
                      << games_accepted << " accepted, "
                      << positions.size() << " positions\n";
        }
    }

    std::cerr << "Total: " << positions.size() << " positions from "
              << games_accepted << " games (out of " << games_loaded << " parsed)\n\n";

    if (positions.empty()) {
        std::cerr << "Error: No positions extracted. Check filter criteria.\n";
        return 1;
    }

    // Initialize parameters from defaults
    EvalParams params;
    params.init_from_defaults();

    double initial_mse = compute_mse(params, positions, cfg.K);
    std::cerr << "Initial MSE: " << std::fixed << std::setprecision(6) << initial_mse << "\n\n";

    // Gradient descent
    std::cerr << "Starting gradient descent (" << cfg.epochs << " epochs)\n";
    for (int epoch = 0; epoch < cfg.epochs; ++epoch) {
        gradient_descent_step(params, positions, cfg);

        double mse = compute_mse(params, positions, cfg.K);
        std::cerr << "Epoch " << std::setw(5) << (epoch + 1)
                  << ": MSE = " << std::fixed << std::setprecision(6) << mse << "\n";
    }

    // Output results
    std::cerr << "\n";
    if (cfg.output_file.empty()) {
        print_eval_params(params, std::cout);
    } else {
        std::ofstream out(cfg.output_file);
        if (!out) {
            std::cerr << "Error: Cannot write to " << cfg.output_file << std::endl;
            return 1;
        }
        print_eval_params(params, out);
        std::cerr << "Wrote output to " << cfg.output_file << "\n";
    }

    return 0;
}
