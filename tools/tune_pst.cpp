#include "board.hpp"
#include "move.hpp"
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
// PGN Parser (adapted from pgn2epd.cpp)
// ============================================================================

Move32 parse_san_move(const std::string& san, Board& board) {
    if (san.empty()) return Move32(0);

    // Handle castling
    if (san == "O-O" || san == "0-0") {
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

    if (san == "O-O-O" || san == "0-0-0") {
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
// PST Tuning Data Structures
// ============================================================================

struct TrainingPosition {
    std::array<std::array<Bitboard, 6>, 2> pieces;  // [Color][Piece]
    float phase;    // 0.0 (endgame) to 1.0 (middlegame)
    float outcome;  // 1.0 white win, 0.5 draw, 0.0 black win
};

struct PSTTables {
    std::array<std::array<double, 64>, 6> mg;  // [Piece][Square]
    std::array<std::array<double, 64>, 6> eg;

    void init_material() {
        // Initialize with material values (same for all squares initially)
        constexpr double PIECE_VALUES[] = {100, 320, 330, 500, 900, 0};
        for (int p = 0; p < 6; ++p) {
            for (int sq = 0; sq < 64; ++sq) {
                mg[p][sq] = PIECE_VALUES[p];
                eg[p][sq] = PIECE_VALUES[p];
            }
        }
    }

    void init_zero() {
        for (int p = 0; p < 6; ++p) {
            for (int sq = 0; sq < 64; ++sq) {
                mg[p][sq] = 0.0;
                eg[p][sq] = 0.0;
            }
        }
    }
};

struct Gradients {
    std::array<std::array<double, 64>, 6> mg;
    std::array<std::array<double, 64>, 6> eg;
    std::array<std::array<int, 64>, 6> counts;

    void clear() {
        for (int p = 0; p < 6; ++p) {
            for (int sq = 0; sq < 64; ++sq) {
                mg[p][sq] = 0.0;
                eg[p][sq] = 0.0;
                counts[p][sq] = 0;
            }
        }
    }
};

// ============================================================================
// Configuration
// ============================================================================

struct Config {
    std::string input_pgn;
    std::string output_file;
    double K = 400.0;
    double learning_rate = 10.0;
    int epochs = 1000;
    int min_elo = 2200;
    int min_time = 480;  // seconds (8 min = classical)
    int skip_moves = 8;
    int max_games = 0;
    int max_positions = 0;
    int report_interval = 100;
    bool verbose = false;
};

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " <input.pgn> [options]\n"
              << "Options:\n"
              << "  -o <file>       Output file (default: stdout)\n"
              << "  -K <value>      Sigmoid scaling factor (default: 400)\n"
              << "  -lr <value>     Learning rate (default: 10)\n"
              << "  -epochs <n>     Number of epochs (default: 1000)\n"
              << "  -min-elo <n>    Minimum average rating (default: 2200)\n"
              << "  -min-time <s>   Minimum initial time in seconds (default: 480)\n"
              << "  -skip <n>       Skip first N moves per side (default: 8)\n"
              << "  -max-games <n>  Maximum games to process (default: unlimited)\n"
              << "  -max-pos <n>    Maximum positions to use (default: unlimited)\n"
              << "  -report <n>     Report interval in epochs (default: 100)\n"
              << "  -v              Verbose output\n";
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
        } else if (strcmp(argv[i], "-report") == 0 && i + 1 < argc) {
            cfg.report_interval = std::stoi(argv[++i]);
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
    // Format: "initial+increment" e.g., "480+0", "300+3"
    size_t plus = tc.find('+');
    if (plus == std::string::npos) {
        // Try parsing as just a number
        try {
            return std::stoi(tc);
        } catch (...) {
            return 0;
        }
    }
    try {
        return std::stoi(tc.substr(0, plus));
    } catch (...) {
        return 0;
    }
}

bool passes_filter(const PGNGame& game, const Config& cfg) {
    // Check time control
    auto tc_it = game.headers.find("TimeControl");
    if (tc_it == game.headers.end()) return false;
    int initial_time = parse_time_control(tc_it->second);
    if (initial_time < cfg.min_time) return false;

    // Check average Elo
    auto white_elo = game.headers.find("WhiteElo");
    auto black_elo = game.headers.find("BlackElo");
    if (white_elo == game.headers.end() || black_elo == game.headers.end()) return false;

    int avg_elo;
    try {
        avg_elo = (std::stoi(white_elo->second) + std::stoi(black_elo->second)) / 2;
    } catch (...) {
        return false;
    }
    if (avg_elo < cfg.min_elo) return false;

    // Check termination
    auto term_it = game.headers.find("Termination");
    if (term_it != game.headers.end()) {
        if (term_it->second == "Time forfeit") return false;
        if (term_it->second == "Abandoned") return false;
    }

    // Check result
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
    // Phase = (knights*1 + bishops*1 + rooks*2 + queens*4) / 24
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
// Position Extraction
// ============================================================================

void extract_positions(const PGNGame& game, const Config& cfg,
                       std::vector<TrainingPosition>& positions) {
    float outcome = parse_outcome(game.headers.at("Result"));
    if (outcome < 0.0f) return;

    Board board;
    int ply = 0;

    for (const std::string& san : game.moves) {
        // Stop before overflowing hash_stack (size 256)
        if (ply >= 250) break;

        Move32 move = parse_san_move(san, board);
        if (move.data == 0) break;  // Parse error

        make_move(board, move);
        ply++;

        // Skip first N moves from each side (2*N plies)
        if (ply < cfg.skip_moves * 2) continue;

        // Sample every 10th position to reduce correlation
        if (ply % 10 != 0) continue;

        // Extract position
        TrainingPosition pos;
        for (int c = 0; c < 2; ++c) {
            for (int p = 0; p < 6; ++p) {
                pos.pieces[c][p] = board.pieces[c][p];
            }
        }
        pos.phase = compute_phase(board);
        pos.outcome = outcome;

        positions.push_back(pos);
    }
}

// ============================================================================
// PST Evaluation
// ============================================================================

double evaluate_pst(const PSTTables& pst, const TrainingPosition& pos) {
    double mg_score = 0.0, eg_score = 0.0;

    for (int piece = 0; piece < 6; ++piece) {
        // White pieces
        Bitboard white_bb = pos.pieces[0][piece];
        while (white_bb) {
            int sq = lsb_index(white_bb);
            mg_score += pst.mg[piece][sq];
            eg_score += pst.eg[piece][sq];
            white_bb &= white_bb - 1;
        }

        // Black pieces (flip square vertically: sq ^ 56)
        Bitboard black_bb = pos.pieces[1][piece];
        while (black_bb) {
            int sq = lsb_index(black_bb);
            int flipped_sq = sq ^ 56;
            mg_score -= pst.mg[piece][flipped_sq];
            eg_score -= pst.eg[piece][flipped_sq];
            black_bb &= black_bb - 1;
        }
    }

    // Interpolate between mg and eg based on phase
    return pos.phase * mg_score + (1.0 - pos.phase) * eg_score;
}

// ============================================================================
// Gradient Descent
// ============================================================================

constexpr double LN10 = 2.302585092994046;

double sigmoid(double eval, double K) {
    return 1.0 / (1.0 + std::pow(10.0, -eval / K));
}

double compute_mse(const PSTTables& pst, const std::vector<TrainingPosition>& positions,
                   double K) {
    double total_error = 0.0;
    for (const auto& pos : positions) {
        double eval = evaluate_pst(pst, pos);
        double predicted = sigmoid(eval, K);
        double error = predicted - pos.outcome;
        total_error += error * error;
    }
    return total_error / positions.size();
}

void gradient_descent_step(PSTTables& pst, const std::vector<TrainingPosition>& positions,
                           double K, double learning_rate) {
    Gradients grad;
    grad.clear();

    for (const auto& pos : positions) {
        double eval = evaluate_pst(pst, pos);
        double predicted = sigmoid(eval, K);

        // d/dx sigmoid(x) = sigmoid(x) * (1 - sigmoid(x)) * ln(10) / K
        double sigmoid_deriv = predicted * (1.0 - predicted) * LN10 / K;

        // Base gradient: d(MSE)/d(eval) = 2 * (predicted - outcome) * sigmoid'
        double base_grad = 2.0 * (predicted - pos.outcome) * sigmoid_deriv;

        // Gradient for middlegame component
        double grad_mg = base_grad * pos.phase;
        // Gradient for endgame component
        double grad_eg = base_grad * (1.0 - pos.phase);

        // Accumulate gradients for each piece on board
        for (int piece = 0; piece < 6; ++piece) {
            // White pieces (positive contribution to eval)
            Bitboard white_bb = pos.pieces[0][piece];
            while (white_bb) {
                int sq = lsb_index(white_bb);
                grad.mg[piece][sq] += grad_mg;
                grad.eg[piece][sq] += grad_eg;
                grad.counts[piece][sq]++;
                white_bb &= white_bb - 1;
            }

            // Black pieces (negative contribution, flipped square)
            Bitboard black_bb = pos.pieces[1][piece];
            while (black_bb) {
                int sq = lsb_index(black_bb);
                int flipped_sq = sq ^ 56;
                grad.mg[piece][flipped_sq] -= grad_mg;
                grad.eg[piece][flipped_sq] -= grad_eg;
                grad.counts[piece][flipped_sq]++;
                black_bb &= black_bb - 1;
            }
        }
    }

    // Apply gradients with averaging
    for (int piece = 0; piece < 6; ++piece) {
        for (int sq = 0; sq < 64; ++sq) {
            if (grad.counts[piece][sq] > 0) {
                double avg_mg = grad.mg[piece][sq] / grad.counts[piece][sq];
                double avg_eg = grad.eg[piece][sq] / grad.counts[piece][sq];
                pst.mg[piece][sq] -= learning_rate * avg_mg;
                pst.eg[piece][sq] -= learning_rate * avg_eg;
            }
        }
    }
}

// ============================================================================
// Output Formatting
// ============================================================================

void print_pst_cpp(const PSTTables& pst, std::ostream& out) {
    const char* piece_names[] = {"Pawn", "Knight", "Bishop", "Rook", "Queen", "King"};

    out << "// Piece-Square Tables - Middlegame\n";
    out << "constexpr int PST_MG[6][64] = {\n";
    for (int p = 0; p < 6; ++p) {
        out << "    // " << piece_names[p] << "\n    {";
        for (int sq = 0; sq < 64; ++sq) {
            if (sq % 8 == 0 && sq > 0) out << "\n     ";
            out << std::setw(5) << static_cast<int>(std::round(pst.mg[p][sq]));
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
            out << std::setw(5) << static_cast<int>(std::round(pst.eg[p][sq]));
            if (sq < 63) out << ",";
        }
        out << "}";
        if (p < 5) out << ",";
        out << "\n";
    }
    out << "};\n";
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    zobrist::init();

    Config cfg = parse_args(argc, argv);

    std::cerr << "PST Tuner Configuration:\n";
    std::cerr << "  Input: " << cfg.input_pgn << "\n";
    std::cerr << "  K: " << cfg.K << "\n";
    std::cerr << "  Learning rate: " << cfg.learning_rate << "\n";
    std::cerr << "  Epochs: " << cfg.epochs << "\n";
    std::cerr << "  Min Elo: " << cfg.min_elo << "\n";
    std::cerr << "  Min time: " << cfg.min_time << "s\n";
    std::cerr << "  Skip moves: " << cfg.skip_moves << "\n";
    std::cerr << "\n";

    // Phase 1: Load and filter games
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

    // Phase 2: Initialize PST with material values
    PSTTables pst;
    pst.init_material();

    double initial_mse = compute_mse(pst, positions, cfg.K);
    std::cerr << "Initial MSE: " << std::fixed << std::setprecision(6) << initial_mse << "\n\n";

    // Phase 3: Gradient descent
    std::cerr << "Starting gradient descent (" << cfg.epochs << " epochs)\n";
    for (int epoch = 0; epoch < cfg.epochs; ++epoch) {
        gradient_descent_step(pst, positions, cfg.K, cfg.learning_rate);

        if ((epoch + 1) % cfg.report_interval == 0 || epoch == cfg.epochs - 1) {
            double mse = compute_mse(pst, positions, cfg.K);
            std::cerr << "Epoch " << std::setw(5) << (epoch + 1)
                      << ": MSE = " << std::fixed << std::setprecision(6) << mse << "\n";
        }
    }

    // Phase 4: Output results
    std::cerr << "\n";
    if (cfg.output_file.empty()) {
        print_pst_cpp(pst, std::cout);
    } else {
        std::ofstream out(cfg.output_file);
        if (!out) {
            std::cerr << "Error: Cannot write to " << cfg.output_file << std::endl;
            return 1;
        }
        print_pst_cpp(pst, out);
        std::cerr << "Wrote output to " << cfg.output_file << "\n";
    }

    return 0;
}
