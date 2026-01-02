#include "board.hpp"
#include "move.hpp"
#include "zobrist.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>
#include <sys/wait.h>

// UCI Engine wrapper - manages subprocess communication
class Engine {
    FILE* to_engine;
    FILE* from_engine;
    std::string name;
    std::string path;
    pid_t child_pid;
    static int instance_counter;
    int instance_id;

public:
    Engine(const std::string& engine_path) : path(engine_path), instance_id(++instance_counter) {
        // Create bidirectional pipe to engine

        int to_child[2], from_child[2];
        if (pipe(to_child) < 0 || pipe(from_child) < 0) {
            throw std::runtime_error("Failed to create pipes");
        }

        pid_t pid = fork();
        if (pid < 0) {
            throw std::runtime_error("Failed to fork");
        }

        if (pid == 0) {
            // Child process
            close(to_child[1]);
            close(from_child[0]);
            dup2(to_child[0], STDIN_FILENO);
            dup2(from_child[1], STDOUT_FILENO);
            close(to_child[0]);
            close(from_child[1]);
            execlp(engine_path.c_str(), engine_path.c_str(), nullptr);
            _exit(1);
        }

        // Parent process
        child_pid = pid;
        close(to_child[0]);
        close(from_child[1]);
        to_engine = fdopen(to_child[1], "w");
        from_engine = fdopen(from_child[0], "r");

        if (!to_engine || !from_engine) {
            throw std::runtime_error("Failed to open engine streams");
        }

        // Initialize UCI
        send("uci");
        wait_for("uciok");

        send("isready");
        wait_for("readyok");
    }

    void set_hash(int mb) {
        send("setoption name Hash value " + std::to_string(mb));
        send("isready");
        wait_for("readyok");
    }

    ~Engine() {
        if (to_engine) {
            send("quit");
            fclose(to_engine);
        }
        if (from_engine) {
            fclose(from_engine);
        }
    }

    void send(const std::string& cmd) {
        fprintf(to_engine, "%s\n", cmd.c_str());
        fflush(to_engine);
    }

    std::string read_line() {
        char buffer[4096];
        if (fgets(buffer, sizeof(buffer), from_engine)) {
            // Remove trailing newline
            size_t len = strlen(buffer);
            if (len > 0 && buffer[len-1] == '\n') {
                buffer[len-1] = '\0';
            }
            return std::string(buffer);
        }
        return "";
    }

    void wait_for(const std::string& expected) {
        while (true) {
            std::string line = read_line();
            if (line.find(expected) == 0) {
                break;
            }
        }
    }

    std::string get_bestmove(const std::string& position, const std::vector<std::string>& moves, int movetime_ms) {
        std::string cmd = "position fen " + position;
        if (!moves.empty()) {
            cmd += " moves";
            for (const auto& m : moves) {
                cmd += " " + m;
            }
        }
        send(cmd);

        send("isready");
        wait_for("readyok");

        send("go movetime " + std::to_string(movetime_ms));

        // Wait for bestmove
        while (true) {
            std::string line = read_line();
            if (line.find("bestmove") == 0) {
                // Parse "bestmove e2e4 ..."
                std::istringstream iss(line);
                std::string token, bestmove;
                iss >> token >> bestmove;
                return bestmove;
            }
        }
    }

    void new_game() {
        send("ucinewgame");
        send("isready");
        wait_for("readyok");
    }

    const std::string& get_path() const { return path; }
};

int Engine::instance_counter = 0;

// Game result
enum class GameResult { WhiteWin, BlackWin, Draw };
enum class DrawReason { None, FiftyMove, Repetition, Stalemate, InsufficientMaterial };

struct GameOutcome {
    GameResult result;
    DrawReason draw_reason;
    int num_moves;
    std::string final_fen;
};

// Check for insufficient material
bool is_insufficient_material(const Board& board) {
    // Count pieces
    int white_knights = popcount(board.pieces[0][(int)Piece::Knight]);
    int white_bishops = popcount(board.pieces[0][(int)Piece::Bishop]);
    int white_rooks = popcount(board.pieces[0][(int)Piece::Rook]);
    int white_queens = popcount(board.pieces[0][(int)Piece::Queen]);
    int white_pawns = popcount(board.pieces[0][(int)Piece::Pawn]);

    int black_knights = popcount(board.pieces[1][(int)Piece::Knight]);
    int black_bishops = popcount(board.pieces[1][(int)Piece::Bishop]);
    int black_rooks = popcount(board.pieces[1][(int)Piece::Rook]);
    int black_queens = popcount(board.pieces[1][(int)Piece::Queen]);
    int black_pawns = popcount(board.pieces[1][(int)Piece::Pawn]);

    int white_major = white_rooks + white_queens;
    int black_major = black_rooks + black_queens;
    int white_minor = white_knights + white_bishops;
    int black_minor = black_knights + black_bishops;

    // Any pawns, rooks, or queens = sufficient
    if (white_pawns + black_pawns + white_major + black_major > 0) {
        return false;
    }

    // K vs K
    if (white_minor == 0 && black_minor == 0) {
        return true;
    }

    // K+minor vs K
    if ((white_minor == 1 && black_minor == 0) ||
        (white_minor == 0 && black_minor == 1)) {
        return true;
    }

    // K+B vs K+B (same color bishops)
    if (white_knights == 0 && black_knights == 0 &&
        white_bishops == 1 && black_bishops == 1) {
        // Check if bishops are on same color
        int wb_sq = lsb_index(board.pieces[0][(int)Piece::Bishop]);
        int bb_sq = lsb_index(board.pieces[1][(int)Piece::Bishop]);
        int wb_color = (wb_sq / 8 + wb_sq % 8) % 2;
        int bb_color = (bb_sq / 8 + bb_sq % 8) % 2;
        if (wb_color == bb_color) {
            return true;
        }
    }

    return false;
}

// Check for 3-fold repetition
bool is_threefold_repetition(const Board& board, const std::vector<u64>& position_hashes) {
    int count = 1;  // Current position counts as 1
    for (const auto& h : position_hashes) {
        if (h == board.hash) {
            count++;
            if (count >= 3) {
                return true;
            }
        }
    }
    return false;
}

// Play a single game between two engines
GameOutcome play_game(Engine& white, Engine& black, const std::string& start_fen, int movetime_ms) {
    Board board(start_fen);
    std::vector<std::string> move_history;
    std::vector<u64> position_hashes;

    white.new_game();
    black.new_game();

    GameOutcome outcome;
    outcome.num_moves = 0;
    outcome.draw_reason = DrawReason::None;

    while (true) {
        // Check for 50-move rule
        if (board.halfmove_clock >= 100) {
            outcome.result = GameResult::Draw;
            outcome.draw_reason = DrawReason::FiftyMove;
            break;
        }

        // Check for 3-fold repetition
        if (is_threefold_repetition(board, position_hashes)) {
            outcome.result = GameResult::Draw;
            outcome.draw_reason = DrawReason::Repetition;
            break;
        }

        // Check for insufficient material
        if (is_insufficient_material(board)) {
            outcome.result = GameResult::Draw;
            outcome.draw_reason = DrawReason::InsufficientMaterial;
            break;
        }

        // Generate legal moves to check for checkmate/stalemate
        MoveList moves = generate_moves(board);
        int legal_count = 0;
        for (int i = 0; i < moves.size; ++i) {
            Move32 m = moves[i];
            make_move(board, m);
            if (!is_illegal(board)) {
                legal_count++;
            }
            unmake_move(board, m);
        }

        if (legal_count == 0) {
            // No legal moves - checkmate or stalemate
            Color them = opposite(board.turn);
            bool in_check = is_attacked(board.king_sq[(int)board.turn], them, board);

            if (in_check) {
                // Checkmate - side to move loses
                outcome.result = (board.turn == Color::White) ? GameResult::BlackWin : GameResult::WhiteWin;
            } else {
                // Stalemate
                outcome.result = GameResult::Draw;
                outcome.draw_reason = DrawReason::Stalemate;
            }
            break;
        }

        // Save position hash before move
        position_hashes.push_back(board.hash);

        // Get move from current player
        Engine& current = (board.turn == Color::White) ? white : black;
        std::string uci_move = current.get_bestmove(start_fen, move_history, movetime_ms);

        // Apply move
        Move32 move = parse_uci_move(uci_move, board);
        if (move.data == 0) {
            std::cerr << "Error: Invalid move '" << uci_move << "' from engine" << std::endl;
            outcome.result = (board.turn == Color::White) ? GameResult::BlackWin : GameResult::WhiteWin;
            break;
        }

        make_move(board, move);
        move_history.push_back(uci_move);
        outcome.num_moves++;

        // Safety limit
        if (outcome.num_moves > 500) {
            outcome.result = GameResult::Draw;
            outcome.draw_reason = DrawReason::None;
            break;
        }
    }

    outcome.final_fen = board.to_fen();
    return outcome;
}

// Parse EPD file - simple format: just FEN strings, one per line
std::vector<std::string> parse_epd_file(const std::string& filename) {
    std::vector<std::string> positions;
    std::ifstream file(filename);
    std::string line;

    while (std::getline(file, line)) {
        if (!line.empty() && line[0] != '#') {
            // EPD might have operations after FEN, just take first 4-6 fields
            positions.push_back(line);
        }
    }

    return positions;
}

// Game task for work queue
struct GameTask {
    std::string fen;
    bool engine1_is_white;
    int game_id;  // For ordering output
};

// Thread-safe work queue
class WorkQueue {
    std::queue<GameTask> tasks;
    std::mutex mtx;

public:
    void push(GameTask task) {
        std::lock_guard<std::mutex> lock(mtx);
        tasks.push(std::move(task));
    }

    bool pop(GameTask& task) {
        std::lock_guard<std::mutex> lock(mtx);
        if (tasks.empty()) return false;
        task = std::move(tasks.front());
        tasks.pop();
        return true;
    }

    size_t size() {
        std::lock_guard<std::mutex> lock(mtx);
        return tasks.size();
    }
};

// Game result for reporting
struct GameReport {
    int game_id;
    GameOutcome outcome;
    bool engine1_is_white;
    std::string fen;
};

// Thread-safe results collector
class ResultsCollector {
    std::vector<GameReport> results;
    std::mutex mtx;
    std::atomic<int> completed{0};
    int total_games;

public:
    ResultsCollector(int total) : total_games(total) {}

    void add(GameReport report) {
        std::lock_guard<std::mutex> lock(mtx);
        results.push_back(std::move(report));
        completed++;
    }

    int get_completed() const { return completed.load(); }
    int get_total() const { return total_games; }

    std::vector<GameReport> get_results() {
        std::lock_guard<std::mutex> lock(mtx);
        return results;
    }
};

// Worker thread function
void worker_thread(
    int thread_id,
    const std::string& engine1_path,
    const std::string& engine2_path,
    int movetime_ms,
    int hash_mb,
    WorkQueue& work_queue,
    ResultsCollector& results,
    bool quiet
) {
    try {
        // Each thread has its own engine instances
        Engine engine1(engine1_path);
        Engine engine2(engine2_path);

        // Configure hash table size
        if (hash_mb > 0) {
            engine1.set_hash(hash_mb);
            engine2.set_hash(hash_mb);
        }

        GameTask task;
        while (work_queue.pop(task)) {
            Engine& white = task.engine1_is_white ? engine1 : engine2;
            Engine& black = task.engine1_is_white ? engine2 : engine1;

            GameOutcome outcome = play_game(white, black, task.fen, movetime_ms);

            GameReport report;
            report.game_id = task.game_id;
            report.outcome = outcome;
            report.engine1_is_white = task.engine1_is_white;
            report.fen = task.fen;

            results.add(std::move(report));

            if (!quiet) {
                int done = results.get_completed();
                int total = results.get_total();
                std::cerr << "\rProgress: " << done << "/" << total << " games completed" << std::flush;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "\nThread " << thread_id << " error: " << e.what() << std::endl;
    }
}

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " <engine1> <engine2> [options]\n"
              << "Options:\n"
              << "  -movetime <ms>   Time per move (default: 100)\n"
              << "  -epd <file>      EPD file with starting positions\n"
              << "  -fen <string>    Single starting position\n"
              << "  -games <n>       Games per position (default: 2)\n"
              << "  -threads <n>     Number of concurrent games (default: CPU count)\n"
              << "  -hash <mb>       Hash table size per engine (default: 512)\n"
              << "  -quiet           Only show final score\n";
}

int main(int argc, char* argv[]) {
    zobrist::init();

    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    std::string engine1_path = argv[1];
    std::string engine2_path = argv[2];
    int movetime_ms = 100;
    std::string epd_file;
    std::string fen;
    int games_per_position = 2;
    int num_threads = std::thread::hardware_concurrency();
    int hash_mb = 512;
    bool quiet = false;

    for (int i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "-movetime") == 0 && i + 1 < argc) {
            movetime_ms = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "-epd") == 0 && i + 1 < argc) {
            epd_file = argv[++i];
        } else if (strcmp(argv[i], "-fen") == 0 && i + 1 < argc) {
            fen = argv[++i];
        } else if (strcmp(argv[i], "-games") == 0 && i + 1 < argc) {
            games_per_position = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "-threads") == 0 && i + 1 < argc) {
            num_threads = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "-hash") == 0 && i + 1 < argc) {
            hash_mb = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "-quiet") == 0) {
            quiet = true;
        } else {
            std::cerr << "Unknown option: " << argv[i] << std::endl;
            print_usage(argv[0]);
            return 1;
        }
    }

    if (num_threads < 1) num_threads = 1;

    // Collect positions
    std::vector<std::string> positions;
    if (!epd_file.empty()) {
        positions = parse_epd_file(epd_file);
    } else if (!fen.empty()) {
        positions.push_back(fen);
    } else {
        // Default to starting position
        positions.push_back("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    }

    if (positions.empty()) {
        std::cerr << "No positions to play" << std::endl;
        return 1;
    }

    // Build work queue with all games
    WorkQueue work_queue;
    int game_id = 0;
    for (const auto& start_fen : positions) {
        for (int game = 0; game < games_per_position; ++game) {
            GameTask task;
            task.fen = start_fen;
            task.engine1_is_white = (game % 2 == 0);
            task.game_id = game_id++;
            work_queue.push(std::move(task));
        }
    }

    int total_games = game_id;
    ResultsCollector results(total_games);

    std::cout << "Playing " << total_games << " games across " << num_threads << " threads" << std::endl;
    std::cout << "Engines: " << engine1_path << " vs " << engine2_path << std::endl;
    std::cout << "Time per move: " << movetime_ms << "ms, Hash: " << hash_mb << "MB per engine" << std::endl;
    std::cout << std::endl;

    // Spawn worker threads
    auto start_time = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker_thread, i, engine1_path, engine2_path,
                            movetime_ms, hash_mb, std::ref(work_queue), std::ref(results), quiet);
    }

    // Wait for all threads to complete
    for (auto& t : threads) {
        t.join();
    }

    auto end_time = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time).count();

    if (!quiet) {
        std::cerr << std::endl;  // Clear progress line
    }

    // Aggregate results
    auto all_results = results.get_results();

    // Sort by game_id for consistent output
    std::sort(all_results.begin(), all_results.end(),
              [](const GameReport& a, const GameReport& b) { return a.game_id < b.game_id; });

    double score1 = 0, score2 = 0;
    int wins1 = 0, wins2 = 0, draws = 0;

    for (const auto& report : all_results) {
        double white_score = 0, black_score = 0;

        switch (report.outcome.result) {
            case GameResult::WhiteWin:
                white_score = 1.0;
                if (report.engine1_is_white) wins1++; else wins2++;
                break;
            case GameResult::BlackWin:
                black_score = 1.0;
                if (report.engine1_is_white) wins2++; else wins1++;
                break;
            case GameResult::Draw:
                white_score = black_score = 0.5;
                draws++;
                break;
        }

        if (report.engine1_is_white) {
            score1 += white_score;
            score2 += black_score;
        } else {
            score1 += black_score;
            score2 += white_score;
        }
    }

    // Print final score
    std::cout << "\n========================================" << std::endl;
    std::cout << "Final Score (" << total_games << " games in " << elapsed << "s):" << std::endl;
    std::cout << "========================================" << std::endl;

    double pct1 = (total_games > 0) ? (100.0 * score1 / total_games) : 0;
    double pct2 = (total_games > 0) ? (100.0 * score2 / total_games) : 0;

    std::cout << "  " << engine1_path << ": " << score1 << "/" << total_games
              << " (" << pct1 << "%) [W:" << wins1 << " D:" << draws << " L:" << wins2 << "]" << std::endl;
    std::cout << "  " << engine2_path << ": " << score2 << "/" << total_games
              << " (" << pct2 << "%) [W:" << wins2 << " D:" << draws << " L:" << wins1 << "]" << std::endl;

    return 0;
}
