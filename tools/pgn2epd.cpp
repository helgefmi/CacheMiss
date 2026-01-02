#include "board.hpp"
#include "move.hpp"
#include "zobrist.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// Game phase detection
enum class GamePhase { Opening, MiddleGame, EndGame };

GamePhase detect_phase(const Board& board, int ply) {
    int total_pieces = popcount(board.all_occupied);  // 2-32 (includes kings)
    int move_number = (ply + 1) / 2;  // Convert half-moves to full moves

    // Endgame: few pieces on board, regardless of move number or queens
    if (total_pieces <= 14) {
        return GamePhase::EndGame;
    }

    // Opening: early moves (1-15), even if there have been trades
    if (move_number <= 15) {
        return GamePhase::Opening;
    }

    // Middle game: later moves with still plenty of pieces
    return GamePhase::MiddleGame;
}

// Material calculation for balance check
int count_material(const Board& board, Color color) {
    int c = (int)color;
    return popcount(board.pieces[c][(int)Piece::Pawn]) * 1 +
           popcount(board.pieces[c][(int)Piece::Knight]) * 3 +
           popcount(board.pieces[c][(int)Piece::Bishop]) * 3 +
           popcount(board.pieces[c][(int)Piece::Rook]) * 5 +
           popcount(board.pieces[c][(int)Piece::Queen]) * 9;
}

bool is_balanced(const Board& board, int max_imbalance) {
    int white_mat = count_material(board, Color::White);
    int black_mat = count_material(board, Color::Black);
    return std::abs(white_mat - black_mat) <= max_imbalance;
}

// Parse SAN move and find matching legal move
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
            default: return Move32(0);  // Invalid piece
        }
        pos++;
    }

    // Parse file/rank disambiguation and target square
    // Format can be: e4, xe4, 1e4, exe4, R1e4, Rexe4, etc.
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

            // Remaining coords are disambiguation
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

        // Check target square
        if (m.to() != to_sq) continue;

        // Check piece type
        Piece moving_piece = board.pieces_on_square[m.from()];
        if (moving_piece != piece) continue;

        // Check disambiguation
        int from_file = m.from() % 8;
        int from_rank = m.from() / 8;
        if (disambig_file >= 0 && from_file != disambig_file) continue;
        if (disambig_rank >= 0 && from_rank != disambig_rank) continue;

        // Check promotion
        if (promotion != Piece::None) {
            if (m.promotion() != promotion) continue;
        } else {
            if (m.is_promotion()) continue;  // SAN didn't specify promotion but move is promotion
        }

        // Verify move is legal
        make_move(board, m);
        bool legal = !is_illegal(board);
        unmake_move(board, m);

        if (legal) {
            return m;
        }
    }

    return Move32(0);
}

// PGN parser
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

            // Parse [Tag "Value"]
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
            // Skip move numbers (e.g., "1.", "1...")
            if (!token.empty() && (std::isdigit(token[0]) || token[0] == '.')) {
                // Check if it's just a move number
                bool is_number = true;
                for (char c : token) {
                    if (!std::isdigit(c) && c != '.') {
                        is_number = false;
                        break;
                    }
                }
                if (is_number) continue;
            }

            // Skip results
            if (token == "1-0" || token == "0-1" || token == "1/2-1/2" || token == "*") {
                continue;
            }

            // Skip comments in braces
            if (token[0] == '{') {
                while (!token.empty() && token.back() != '}' && iss >> token) {}
                continue;
            }

            // Skip annotations
            if (token[0] == '$' || token == "!" || token == "?" ||
                token == "!!" || token == "??" || token == "!?" || token == "?!") {
                continue;
            }

            // Skip NAG annotations (e.g., $1, $2)
            if (!token.empty() && token[0] == '$') {
                continue;
            }

            game.moves.push_back(token);
        }

        return true;  // Return true even if no moves - caller can filter
    }

    bool eof() const { return !file.good(); }
};

struct Config {
    std::string input_file;
    std::string output_file;
    int total_positions = 250;
    int opening_pct = 50;
    int middle_pct = 30;
    int endgame_pct = 20;
    int max_games = 0;  // 0 = unlimited
    int max_imbalance = 1;
    int max_per_eco = 3;
};

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " <input.pgn> <output.epd> [options]\n"
              << "Options:\n"
              << "  -n <total>         Total positions to extract (default: 250)\n"
              << "  -opening <pct>     Percentage from opening (default: 50)\n"
              << "  -middle <pct>      Percentage from middle game (default: 30)\n"
              << "  -endgame <pct>     Percentage from endgame (default: 20)\n"
              << "  -max-games <n>     Max games to parse (default: unlimited)\n"
              << "  -balance <n>       Max material imbalance in pawns (default: 1)\n"
              << "  -max-per-eco <n>   Max positions per ECO code (default: 3)\n";
}

int main(int argc, char* argv[]) {
    zobrist::init();

    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    Config cfg;
    cfg.input_file = argv[1];
    cfg.output_file = argv[2];

    for (int i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            cfg.total_positions = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "-opening") == 0 && i + 1 < argc) {
            cfg.opening_pct = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "-middle") == 0 && i + 1 < argc) {
            cfg.middle_pct = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "-endgame") == 0 && i + 1 < argc) {
            cfg.endgame_pct = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "-max-games") == 0 && i + 1 < argc) {
            cfg.max_games = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "-balance") == 0 && i + 1 < argc) {
            cfg.max_imbalance = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "-max-per-eco") == 0 && i + 1 < argc) {
            cfg.max_per_eco = std::stoi(argv[++i]);
        } else {
            std::cerr << "Unknown option: " << argv[i] << std::endl;
            print_usage(argv[0]);
            return 1;
        }
    }

    // Calculate quotas
    int opening_quota = (cfg.total_positions * cfg.opening_pct) / 100;
    int middle_quota = (cfg.total_positions * cfg.middle_pct) / 100;
    int endgame_quota = cfg.total_positions - opening_quota - middle_quota;

    std::cout << "Target: " << cfg.total_positions << " positions\n";
    std::cout << "  Opening: " << opening_quota << " (" << cfg.opening_pct << "%)\n";
    std::cout << "  Middle:  " << middle_quota << " (" << cfg.middle_pct << "%)\n";
    std::cout << "  Endgame: " << endgame_quota << " (" << cfg.endgame_pct << "%)\n";
    std::cout << "Max imbalance: " << cfg.max_imbalance << " pawns\n";
    std::cout << "Max per ECO: " << cfg.max_per_eco << "\n\n";

    std::ifstream infile(cfg.input_file);
    if (!infile) {
        std::cerr << "Error: Cannot open " << cfg.input_file << std::endl;
        return 1;
    }

    // Position storage
    std::vector<std::string> opening_fens;
    std::vector<std::string> middle_fens;
    std::vector<std::string> endgame_fens;
    std::map<std::string, int> eco_counts;
    std::set<std::string> seen_fens;  // Avoid duplicates

    PGNParser parser(infile);
    PGNGame game;
    int games_parsed = 0;

    std::mt19937 rng(42);  // Fixed seed for reproducibility

    while (!parser.eof() && parser.next_game(game)) {
        games_parsed++;

        // Skip games with no moves
        if (game.moves.empty()) continue;

        if (cfg.max_games > 0 && games_parsed > cfg.max_games) break;

        // Check if quotas are filled
        bool need_opening = (int)opening_fens.size() < opening_quota;
        bool need_middle = (int)middle_fens.size() < middle_quota;
        bool need_endgame = (int)endgame_fens.size() < endgame_quota;
        if (!need_opening && !need_middle && !need_endgame) break;

        // Skip unfinished games
        auto result_it = game.headers.find("Result");
        if (result_it != game.headers.end() && result_it->second == "*") {
            continue;
        }

        // Get ECO code for diversity
        std::string eco = "???";
        auto eco_it = game.headers.find("ECO");
        if (eco_it != game.headers.end()) {
            eco = eco_it->second;
        }

        // Check ECO quota
        if (eco_counts[eco] >= cfg.max_per_eco) {
            continue;
        }

        // Replay game and look for positions
        Board board;
        int ply = 0;
        bool extracted = false;

        for (const auto& san : game.moves) {
            Move32 move = parse_san_move(san, board);
            if (move.data == 0) {
                // Failed to parse move, skip rest of game
                break;
            }

            make_move(board, move);
            ply++;

            if (extracted) continue;  // Already got a position from this game

            // Check phase and quotas
            GamePhase phase = detect_phase(board, ply);
            bool quota_ok = false;
            std::vector<std::string>* target_fens = nullptr;
            int target_quota = 0;

            switch (phase) {
                case GamePhase::Opening:
                    if (need_opening) {
                        quota_ok = true;
                        target_fens = &opening_fens;
                        target_quota = opening_quota;
                    }
                    break;
                case GamePhase::MiddleGame:
                    if (need_middle) {
                        quota_ok = true;
                        target_fens = &middle_fens;
                        target_quota = middle_quota;
                    }
                    break;
                case GamePhase::EndGame:
                    if (need_endgame) {
                        quota_ok = true;
                        target_fens = &endgame_fens;
                        target_quota = endgame_quota;
                    }
                    break;
            }

            if (!quota_ok) continue;

            // Check material balance
            if (!is_balanced(board, cfg.max_imbalance)) continue;

            // Get FEN and check for duplicates
            std::string fen = board.to_fen();
            if (seen_fens.count(fen)) continue;

            // Random sampling: accept with probability based on how much quota remains
            // This helps get diverse positions across games
            double accept_prob = 1.0;
            if ((int)target_fens->size() > target_quota / 2) {
                accept_prob = 0.3;  // Be more selective when quota is filling up
            }
            std::uniform_real_distribution<> dist(0.0, 1.0);
            if (dist(rng) > accept_prob) continue;

            // Accept position
            target_fens->push_back(fen);
            seen_fens.insert(fen);
            eco_counts[eco]++;
            extracted = true;

            // Update need flags
            need_opening = (int)opening_fens.size() < opening_quota;
            need_middle = (int)middle_fens.size() < middle_quota;
            need_endgame = (int)endgame_fens.size() < endgame_quota;
        }

        // Progress report
        if (games_parsed % 10000 == 0) {
            std::cout << "Parsed " << games_parsed << " games, collected: "
                      << opening_fens.size() << "/" << opening_quota << " opening, "
                      << middle_fens.size() << "/" << middle_quota << " middle, "
                      << endgame_fens.size() << "/" << endgame_quota << " endgame\n";
        }
    }

    std::cout << "\nParsed " << games_parsed << " games total\n";
    std::cout << "Collected:\n";
    std::cout << "  Opening: " << opening_fens.size() << "/" << opening_quota << "\n";
    std::cout << "  Middle:  " << middle_fens.size() << "/" << middle_quota << "\n";
    std::cout << "  Endgame: " << endgame_fens.size() << "/" << endgame_quota << "\n";

    // Write output
    std::ofstream outfile(cfg.output_file);
    if (!outfile) {
        std::cerr << "Error: Cannot write to " << cfg.output_file << std::endl;
        return 1;
    }

    // Interleave positions from different phases for variety
    size_t oi = 0, mi = 0, ei = 0;
    int written = 0;
    while (oi < opening_fens.size() || mi < middle_fens.size() || ei < endgame_fens.size()) {
        if (oi < opening_fens.size()) {
            outfile << opening_fens[oi++] << "\n";
            written++;
        }
        if (mi < middle_fens.size()) {
            outfile << middle_fens[mi++] << "\n";
            written++;
        }
        if (ei < endgame_fens.size()) {
            outfile << endgame_fens[ei++] << "\n";
            written++;
        }
    }

    std::cout << "\nWrote " << written << " positions to " << cfg.output_file << "\n";
    std::cout << "ECO codes used: " << eco_counts.size() << "\n";

    return 0;
}
