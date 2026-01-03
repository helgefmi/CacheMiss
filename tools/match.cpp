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
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>
#include <sys/wait.h>
#include <poll.h>

// ============================================================================
// Debug logging
// ============================================================================
static std::mutex log_mutex;
static std::ofstream log_file;
static bool logging_enabled = false;

void log_init(const std::string& filename) {
    std::lock_guard<std::mutex> lock(log_mutex);
    log_file.open(filename, std::ios::out | std::ios::trunc);
    logging_enabled = log_file.is_open();
    if (logging_enabled) {
        log_file << "=== Match log started ===" << std::endl;
    }
}

void log_close() {
    std::lock_guard<std::mutex> lock(log_mutex);
    if (log_file.is_open()) {
        log_file << "=== Match log ended ===" << std::endl;
        log_file.close();
    }
    logging_enabled = false;
}

void log_msg(const std::string& msg) {
    if (!logging_enabled) return;
    std::lock_guard<std::mutex> lock(log_mutex);
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    auto time = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf;
    localtime_r(&time, &tm_buf);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", &tm_buf);
    log_file << "[" << time_str << "." << std::setfill('0') << std::setw(3) << ms.count() << "] "
             << "[tid:" << std::this_thread::get_id() << "] " << msg << std::endl;
    log_file.flush();
}

// FTXUI for terminal UI
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

using namespace ftxui;

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
        log_msg("Engine[" + std::to_string(instance_id) + "] creating: " + engine_path);

        // Create bidirectional pipe to engine
        int to_child[2], from_child[2];
        if (pipe(to_child) < 0 || pipe(from_child) < 0) {
            log_msg("Engine[" + std::to_string(instance_id) + "] FAILED to create pipes");
            throw std::runtime_error("Failed to create pipes");
        }

        pid_t pid = fork();
        if (pid < 0) {
            log_msg("Engine[" + std::to_string(instance_id) + "] FAILED to fork");
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
        log_msg("Engine[" + std::to_string(instance_id) + "] forked, child pid=" + std::to_string(pid));
        close(to_child[0]);
        close(from_child[1]);
        to_engine = fdopen(to_child[1], "w");
        from_engine = fdopen(from_child[0], "r");

        if (!to_engine || !from_engine) {
            log_msg("Engine[" + std::to_string(instance_id) + "] FAILED to open streams");
            throw std::runtime_error("Failed to open engine streams");
        }

        // CRITICAL: Use unbuffered mode for reading so poll() works correctly
        // With buffered I/O, fgets() may read multiple lines into the FILE* buffer,
        // leaving the kernel pipe empty, causing poll() to block forever.
        setvbuf(from_engine, nullptr, _IONBF, 0);
        setvbuf(to_engine, nullptr, _IOLBF, 0);  // Line buffered for writes is fine

        // Initialize UCI
        log_msg("Engine[" + std::to_string(instance_id) + "] sending 'uci'");
        send("uci");
        log_msg("Engine[" + std::to_string(instance_id) + "] waiting for 'uciok'");
        wait_for("uciok");
        log_msg("Engine[" + std::to_string(instance_id) + "] got 'uciok'");

        send("isready");
        log_msg("Engine[" + std::to_string(instance_id) + "] waiting for 'readyok'");
        wait_for("readyok");
        log_msg("Engine[" + std::to_string(instance_id) + "] READY");
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
            to_engine = nullptr;
        }
        if (from_engine) {
            fclose(from_engine);
            from_engine = nullptr;
        }
        // Reap the child process to avoid zombies
        if (child_pid > 0) {
            int status;
            waitpid(child_pid, &status, 0);
        }
    }

    void send(const std::string& cmd) {
        if (!to_engine) {
            log_msg("Engine[" + std::to_string(instance_id) + "] SEND FAILED - closed: " + cmd);
            throw std::runtime_error("Cannot send to closed engine " + path);
        }
        log_msg("Engine[" + std::to_string(instance_id) + "] >> " + cmd);
        if (fprintf(to_engine, "%s\n", cmd.c_str()) < 0) {
            log_msg("Engine[" + std::to_string(instance_id) + "] SEND FAILED - fprintf error");
            throw std::runtime_error("Failed to send to engine " + path);
        }
        if (fflush(to_engine) != 0) {
            log_msg("Engine[" + std::to_string(instance_id) + "] SEND FAILED - flush error");
            throw std::runtime_error("Failed to flush to engine " + path);
        }
    }

    std::string read_line(int timeout_ms = 30000) {
        // Use poll to wait for data with timeout
        struct pollfd pfd;
        pfd.fd = fileno(from_engine);
        pfd.events = POLLIN;

        log_msg("Engine[" + std::to_string(instance_id) + "] poll(timeout=" + std::to_string(timeout_ms) + ")...");
        int ret = poll(&pfd, 1, timeout_ms);
        if (ret < 0) {
            log_msg("Engine[" + std::to_string(instance_id) + "] poll FAILED: " + strerror(errno));
            throw std::runtime_error("poll() failed for engine " + path + ": " + strerror(errno));
        }
        if (ret == 0) {
            log_msg("Engine[" + std::to_string(instance_id) + "] poll TIMEOUT after " + std::to_string(timeout_ms) + "ms");
            throw std::runtime_error("Engine " + path + " timed out after " + std::to_string(timeout_ms) + "ms");
        }
        log_msg("Engine[" + std::to_string(instance_id) + "] poll returned " + std::to_string(ret) + ", revents=0x" +
                ([](int r) { std::ostringstream ss; ss << std::hex << r; return ss.str(); })(pfd.revents));

        // Check for errors, but only if there's no data to read
        // (POLLHUP can be set along with POLLIN if there's data before hangup)
        if (!(pfd.revents & POLLIN)) {
            if (pfd.revents & POLLHUP) {
                log_msg("Engine[" + std::to_string(instance_id) + "] POLLHUP - pipe closed");
                throw std::runtime_error("Engine " + path + " closed pipe (crashed?)");
            }
            if (pfd.revents & (POLLERR | POLLNVAL)) {
                log_msg("Engine[" + std::to_string(instance_id) + "] POLLERR/POLLNVAL");
                throw std::runtime_error("Engine " + path + " pipe error");
            }
        }

        log_msg("Engine[" + std::to_string(instance_id) + "] calling fgets...");
        char buffer[4096];
        if (fgets(buffer, sizeof(buffer), from_engine)) {
            // Remove trailing newline
            size_t len = strlen(buffer);
            if (len > 0 && buffer[len-1] == '\n') {
                buffer[len-1] = '\0';
            }
            log_msg("Engine[" + std::to_string(instance_id) + "] << " + std::string(buffer));
            return std::string(buffer);
        }
        // EOF or error - engine likely crashed
        log_msg("Engine[" + std::to_string(instance_id) + "] fgets returned NULL - EOF/error");
        throw std::runtime_error("Engine " + path + " closed connection (crashed?)");
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

        // Wait for bestmove with generous timeout (movetime + 10 seconds buffer)
        int timeout = movetime_ms + 10000;
        while (true) {
            std::string line = read_line(timeout);
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

// Forward declarations for TUI
class GameStateManager;

// Callback type for TUI updates during game play
using GameUpdateCallback = std::function<void(int game_id, const std::string& fen, const std::vector<std::string>& moves)>;

// Play a single game between two engines (with optional TUI updates)
GameOutcome play_game(Engine& white, Engine& black, const std::string& start_fen, int movetime_ms,
                      const std::string& white_name, const std::string& black_name,
                      GameUpdateCallback on_move = nullptr) {
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
            ::Color them = opposite(board.turn);
            bool in_check = is_attacked(board.king_sq[(int)board.turn], them, board);

            if (in_check) {
                // Checkmate - side to move loses
                outcome.result = (board.turn == ::Color::White) ? GameResult::BlackWin : GameResult::WhiteWin;
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
        Engine& current = (board.turn == ::Color::White) ? white : black;
        std::string uci_move = current.get_bestmove(start_fen, move_history, movetime_ms);

        // Apply move
        Move32 move = parse_uci_move(uci_move, board);
        if (move.data == 0) {
            const std::string& engine_name = (board.turn == ::Color::White) ? white_name : black_name;
            std::cerr << "Error: Invalid move '" << uci_move << "' from " << engine_name << std::endl;
            outcome.result = (board.turn == ::Color::White) ? GameResult::BlackWin : GameResult::WhiteWin;
            break;
        }

        make_move(board, move);
        move_history.push_back(uci_move);
        outcome.num_moves++;

        // Update TUI state after each move
        if (on_move) {
            on_move(-1, board.to_fen(), move_history);  // game_id filled in by caller
        }

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

// Thread-safe results collector with running stats
class ResultsCollector {
    std::vector<GameReport> results;
    std::mutex mtx;
    std::atomic<int> completed{0};
    std::atomic<int> wins1{0};
    std::atomic<int> wins2{0};
    std::atomic<int> draws{0};
    int total_games;

public:
    ResultsCollector(int total) : total_games(total) {}

    void add(GameReport report) {
        // Update running stats
        switch (report.outcome.result) {
            case GameResult::WhiteWin:
                if (report.engine1_is_white) wins1++; else wins2++;
                break;
            case GameResult::BlackWin:
                if (report.engine1_is_white) wins2++; else wins1++;
                break;
            case GameResult::Draw:
                draws++;
                break;
        }

        std::lock_guard<std::mutex> lock(mtx);
        results.push_back(std::move(report));
        completed++;
    }

    int get_completed() const { return completed.load(); }
    int get_total() const { return total_games; }
    int get_wins1() const { return wins1.load(); }
    int get_wins2() const { return wins2.load(); }
    int get_draws() const { return draws.load(); }

    std::vector<GameReport> get_results() {
        std::lock_guard<std::mutex> lock(mtx);
        return results;
    }
};

// ============================================================================
// TUI: Game display state for rendering
// ============================================================================

struct GameDisplay {
    int game_id;
    std::string start_fen;
    std::string current_fen;
    std::vector<std::string> moves;  // UCI moves
    GameResult result = GameResult::Draw;
    DrawReason draw_reason = DrawReason::None;
    bool finished = false;
    bool engine1_is_white = true;
    int view_move_index = -1;  // -1 means show latest position
};

// Thread-safe game state manager for TUI
class GameStateManager {
    std::vector<GameDisplay> games;
    mutable std::mutex mtx;

public:
    void init_game(int game_id, const std::string& fen, bool engine1_is_white) {
        std::lock_guard<std::mutex> lock(mtx);
        if (game_id >= (int)games.size()) {
            games.resize(game_id + 1);
        }
        games[game_id].game_id = game_id;
        games[game_id].start_fen = fen;
        games[game_id].current_fen = fen;
        games[game_id].engine1_is_white = engine1_is_white;
        games[game_id].finished = false;
        games[game_id].moves.clear();
    }

    void update_game(int game_id, const std::string& fen, const std::vector<std::string>& moves) {
        std::lock_guard<std::mutex> lock(mtx);
        if (game_id < (int)games.size()) {
            games[game_id].current_fen = fen;
            games[game_id].moves = moves;
        }
    }

    void finish_game(int game_id, GameResult result, DrawReason reason) {
        std::lock_guard<std::mutex> lock(mtx);
        if (game_id < (int)games.size()) {
            games[game_id].result = result;
            games[game_id].draw_reason = reason;
            games[game_id].finished = true;
        }
    }

    std::vector<GameDisplay> get_games() const {
        std::lock_guard<std::mutex> lock(mtx);
        return games;
    }

    int count_finished() const {
        std::lock_guard<std::mutex> lock(mtx);
        int count = 0;
        for (const auto& g : games) {
            if (g.finished) count++;
        }
        return count;
    }

    void set_view_move(int game_id, int move_index) {
        std::lock_guard<std::mutex> lock(mtx);
        if (game_id < (int)games.size()) {
            games[game_id].view_move_index = move_index;
        }
    }

    int get_view_move(int game_id) const {
        std::lock_guard<std::mutex> lock(mtx);
        if (game_id < (int)games.size()) {
            return games[game_id].view_move_index;
        }
        return -1;
    }

    int get_move_count(int game_id) const {
        std::lock_guard<std::mutex> lock(mtx);
        if (game_id < (int)games.size()) {
            return (int)games[game_id].moves.size();
        }
        return 0;
    }

    GameDisplay get_game(int game_id) const {
        std::lock_guard<std::mutex> lock(mtx);
        if (game_id < (int)games.size()) {
            return games[game_id];
        }
        return GameDisplay{};
    }

    int size() const {
        std::lock_guard<std::mutex> lock(mtx);
        return (int)games.size();
    }
};

// ============================================================================
// TUI: Rendering functions
// ============================================================================

// Get FEN at a specific move index (0 = after first move, -1 = current/latest)
std::string get_fen_at_move(const GameDisplay& game, int move_index) {
    if (move_index < 0 || move_index >= (int)game.moves.size()) {
        return game.current_fen;
    }

    // Replay moves from start position
    Board board(game.start_fen);
    for (int i = 0; i <= move_index; ++i) {
        Move32 move = parse_uci_move(game.moves[i], board);
        if (move.data != 0) {
            make_move(board, move);
        }
    }
    return board.to_fen();
}

// ASCII chess pieces: white uppercase, black lowercase
constexpr char PIECE_ASCII[2][6] = {
    {'P', 'N', 'B', 'R', 'Q', 'K'},  // White
    {'p', 'n', 'b', 'r', 'q', 'k'}   // Black
};

Element render_board(const std::string& fen) {
    Board board(fen);
    Elements rows;

    // Same format as Board::print()
    rows.push_back(text("  +---+---+---+---+---+---+---+---+"));

    for (int rank = 7; rank >= 0; --rank) {
        std::string line = std::to_string(rank + 1) + " |";

        for (int file = 0; file < 8; ++file) {
            int sq = rank * 8 + file;
            char piece_char = '.';

            // Find piece on this square
            for (int c = 0; c < 2; ++c) {
                for (int p = 0; p < 6; ++p) {
                    if (board.pieces[c][p] & (1ULL << sq)) {
                        piece_char = PIECE_ASCII[c][p];
                    }
                }
            }

            line += " ";
            line += piece_char;
            line += " |";
        }

        rows.push_back(text(line));
        rows.push_back(text("  +---+---+---+---+---+---+---+---+"));
    }

    rows.push_back(text("    a   b   c   d   e   f   g   h"));

    return vbox(rows);
}

Element render_moves(const std::vector<std::string>& moves, int highlight_index) {
    if (moves.empty()) {
        return text("(no moves)") | dim;
    }

    Elements parts;
    int move_num = 1;
    for (size_t i = 0; i < moves.size(); ++i) {
        if (i % 2 == 0) {
            if (i > 0) parts.push_back(text(" "));
            parts.push_back(text(std::to_string(move_num++) + ".") | dim);
        }

        auto move_text = text(" " + moves[i]);
        if ((int)i == highlight_index) {
            move_text = move_text | inverted;
        }
        parts.push_back(move_text);
    }

    return hflow(parts);
}

Element render_game_card(const GameDisplay& game, const std::string& engine1_name, const std::string& engine2_name, bool selected) {
    // Header with game number and status
    std::string status;
    if (game.finished) {
        switch (game.result) {
            case GameResult::WhiteWin: status = "1-0"; break;
            case GameResult::BlackWin: status = "0-1"; break;
            case GameResult::Draw: status = "1/2"; break;
        }
        if (game.draw_reason != DrawReason::None) {
            switch (game.draw_reason) {
                case DrawReason::Stalemate: status += " stale"; break;
                case DrawReason::FiftyMove: status += " 50mv"; break;
                case DrawReason::Repetition: status += " rep"; break;
                case DrawReason::InsufficientMaterial: status += " insuf"; break;
                default: break;
            }
        }
    } else {
        status = "...";
    }

    std::string header = "Game " + std::to_string(game.game_id + 1) + " [" + status + "]";

    // Show which engine is which color
    const std::string& white_name = game.engine1_is_white ? engine1_name : engine2_name;
    const std::string& black_name = game.engine1_is_white ? engine2_name : engine1_name;

    // Determine which position and move to show
    int view_idx = game.view_move_index;
    int total_moves = (int)game.moves.size();
    std::string display_fen = get_fen_at_move(game, view_idx);

    // Move indicator: show "move X/Y" or "latest"
    std::string move_info;
    if (view_idx < 0 || view_idx >= total_moves - 1) {
        move_info = "move " + std::to_string(total_moves) + "/" + std::to_string(total_moves);
    } else {
        move_info = "move " + std::to_string(view_idx + 1) + "/" + std::to_string(total_moves);
    }

    auto card = vbox({
        text(header) | bold,
        hbox({text("W:") | dim, text(white_name.substr(white_name.rfind('/') + 1))}),
        hbox({text("B:") | dim, text(black_name.substr(black_name.rfind('/') + 1))}),
        text(move_info) | dim,
        separator(),
        render_board(display_fen),
        separator(),
        render_moves(game.moves, view_idx) | size(HEIGHT, LESS_THAN, 4),
    });

    if (selected) {
        card = card | border | color(ftxui::Color::Yellow);
    } else {
        card = card | border;
    }

    return card;
}

// ============================================================================
// Worker thread function (updated for TUI)
// ============================================================================

void worker_thread(
    int thread_id,
    const std::string& engine1_path,
    const std::string& engine2_path,
    int movetime_ms,
    int hash_mb,
    WorkQueue& work_queue,
    ResultsCollector& results,
    GameStateManager& state,
    ScreenInteractive& screen,
    std::atomic<bool>& fatal_error
) {
    log_msg("Worker[" + std::to_string(thread_id) + "] started");

    // Check if there's any work before creating expensive engine processes
    if (work_queue.size() == 0) {
        log_msg("Worker[" + std::to_string(thread_id) + "] no work available, exiting early");
        return;
    }

    try {
        // Each thread has its own engine instances
        log_msg("Worker[" + std::to_string(thread_id) + "] creating engine1: " + engine1_path);
        Engine engine1(engine1_path);
        log_msg("Worker[" + std::to_string(thread_id) + "] creating engine2: " + engine2_path);
        Engine engine2(engine2_path);
        log_msg("Worker[" + std::to_string(thread_id) + "] both engines created");

        // Configure hash table size
        if (hash_mb > 0) {
            log_msg("Worker[" + std::to_string(thread_id) + "] setting hash to " + std::to_string(hash_mb) + "MB");
            engine1.set_hash(hash_mb);
            engine2.set_hash(hash_mb);
        }

        log_msg("Worker[" + std::to_string(thread_id) + "] entering game loop");
        GameTask task;
        while (!fatal_error.load() && work_queue.pop(task)) {
            log_msg("Worker[" + std::to_string(thread_id) + "] got task: game_id=" + std::to_string(task.game_id));
            Engine& white = task.engine1_is_white ? engine1 : engine2;
            Engine& black = task.engine1_is_white ? engine2 : engine1;
            const std::string& white_name = task.engine1_is_white ? engine1_path : engine2_path;
            const std::string& black_name = task.engine1_is_white ? engine2_path : engine1_path;

            // Callback to update TUI on each move
            int game_id = task.game_id;
            auto on_move = [&state, &screen, game_id](int, const std::string& fen, const std::vector<std::string>& moves) {
                state.update_game(game_id, fen, moves);
                screen.Post(Event::Custom);
            };

            try {
                GameOutcome outcome = play_game(white, black, task.fen, movetime_ms, white_name, black_name, on_move);

                // Mark game as finished in TUI state
                state.finish_game(task.game_id, outcome.result, outcome.draw_reason);
                screen.Post(Event::Custom);

                GameReport report;
                report.game_id = task.game_id;
                report.outcome = outcome;
                report.engine1_is_white = task.engine1_is_white;
                report.fen = task.fen;

                results.add(std::move(report));
            } catch (const std::exception& e) {
                // Game-level error (e.g., engine crashed mid-game)
                // Mark as loss for the side whose engine crashed, but we can't tell which
                // So mark as draw and continue
                std::cerr << "\nGame " << task.game_id << " error: " << e.what() << std::endl;

                GameOutcome outcome;
                outcome.result = GameResult::Draw;
                outcome.draw_reason = DrawReason::None;
                outcome.num_moves = 0;

                state.finish_game(task.game_id, outcome.result, outcome.draw_reason);
                screen.Post(Event::Custom);

                GameReport report;
                report.game_id = task.game_id;
                report.outcome = outcome;
                report.engine1_is_white = task.engine1_is_white;
                report.fen = task.fen;
                results.add(std::move(report));

                // Engine crashed, can't continue with this thread's engines
                // Exit the loop to restart engines
                break;
            }
        }
    } catch (const std::exception& e) {
        // Thread-level error (e.g., engine failed to start)
        std::cerr << "\nThread " << thread_id << " fatal error: " << e.what() << std::endl;
        fatal_error.store(true);
        screen.Exit();
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
              << "  -log <file>      Enable verbose logging to file\n"
              << "\nControls:\n"
              << "  q/Esc            Quit\n"
              << "  PgUp/PgDn        Scroll up/down\n"
              << "  Home/End         Jump to top/bottom\n";
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
    std::string log_filename;

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
        } else if (strcmp(argv[i], "-log") == 0 && i + 1 < argc) {
            log_filename = argv[++i];
        } else {
            std::cerr << "Unknown option: " << argv[i] << std::endl;
            print_usage(argv[0]);
            return 1;
        }
    }

    // Initialize logging if requested
    if (!log_filename.empty()) {
        log_init(log_filename);
        log_msg("Match starting: " + engine1_path + " vs " + engine2_path);
        log_msg("Options: movetime=" + std::to_string(movetime_ms) + "ms, threads=" +
                std::to_string(num_threads) + ", hash=" + std::to_string(hash_mb) + "MB");
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

    // Build work queue and initialize game state
    log_msg("Building work queue from " + std::to_string(positions.size()) + " positions");
    WorkQueue work_queue;
    GameStateManager state;
    int game_id = 0;
    for (const auto& start_fen : positions) {
        for (int game = 0; game < games_per_position; ++game) {
            GameTask task;
            task.fen = start_fen;
            task.engine1_is_white = (game % 2 == 0);
            task.game_id = game_id;
            work_queue.push(task);
            state.init_game(game_id, start_fen, task.engine1_is_white);
            log_msg("Created game " + std::to_string(game_id) + ": " + start_fen.substr(0, 30) + "...");
            game_id++;
        }
    }

    int total_games = game_id;
    log_msg("Total games to play: " + std::to_string(total_games));

    // Don't spawn more threads than games - each thread creates 2 engine processes
    if (num_threads > total_games) {
        log_msg("Reducing threads from " + std::to_string(num_threads) + " to " + std::to_string(total_games));
        num_threads = total_games;
    }

    ResultsCollector results(total_games);

    // Extract short engine names for display
    auto short_name = [](const std::string& path) {
        size_t pos = path.rfind('/');
        return (pos != std::string::npos) ? path.substr(pos + 1) : path;
    };
    std::string engine1_short = short_name(engine1_path);
    std::string engine2_short = short_name(engine2_path);

    // Create FTXUI screen
    auto screen = ScreenInteractive::Fullscreen();

    // UI state
    int selected_game = 0;     // Currently selected game card
    int scroll_row = 0;        // First visible row
    int cols = 1;              // Columns (computed in renderer)

    // Main UI component
    auto main_component = Renderer([&] {
        auto games = state.get_games();
        int num_games = (int)games.size();
        int finished = state.count_finished();
        int w1 = results.get_wins1();
        int w2 = results.get_wins2();
        int d = results.get_draws();
        double score1 = w1 + 0.5 * d;
        double pct = (finished > 0) ? (100.0 * score1 / finished) : 0;

        // Header
        std::ostringstream header_ss;
        header_ss << engine1_short << " vs " << engine2_short
                  << "  |  " << finished << "/" << total_games
                  << " [W:" << w1 << " D:" << d << " L:" << w2
                  << " = " << std::fixed << std::setprecision(1) << pct << "%]";

        Element header = text(header_ss.str()) | bold | center;

        // Help line
        Element help = text("Arrows=select  ,/.=move  ;/:=10moves  Home/End=first/last  q=quit") | dim | center;

        // Calculate columns based on terminal width
        int term_width = Terminal::Size().dimx;
        int term_height = Terminal::Size().dimy;
        int game_width = 42;  // Width of each game card (board is 37 chars + borders)
        cols = std::max(1, (term_width - 4) / game_width);

        // Calculate visible rows (account for header, help, borders)
        int card_height = 22;  // Height of a card (board is 18 lines + header)
        int visible_rows = std::max(1, (term_height - 6) / card_height);

        // Ensure selected game is valid
        if (num_games > 0) {
            selected_game = std::clamp(selected_game, 0, num_games - 1);
        }

        // Calculate which row the selected game is in
        int selected_row = (num_games > 0) ? (selected_game / cols) : 0;
        int total_rows = (num_games + cols - 1) / cols;

        // Auto-scroll to keep selected in view
        if (selected_row < scroll_row) {
            scroll_row = selected_row;
        } else if (selected_row >= scroll_row + visible_rows) {
            scroll_row = selected_row - visible_rows + 1;
        }
        scroll_row = std::clamp(scroll_row, 0, std::max(0, total_rows - visible_rows));

        // Render only visible rows
        Elements rows;
        for (int row = scroll_row; row < scroll_row + visible_rows && row < total_rows; ++row) {
            Elements row_elements;
            for (int c = 0; c < cols; ++c) {
                int idx = row * cols + c;
                if (idx < num_games) {
                    bool is_selected = (idx == selected_game);
                    row_elements.push_back(render_game_card(games[idx], engine1_path, engine2_path, is_selected) | flex);
                }
            }
            if (!row_elements.empty()) {
                rows.push_back(hbox(row_elements));
            }
        }

        Element game_grid = vbox(rows);

        // Scroll indicator
        std::string scroll_info = "Row " + std::to_string(scroll_row + 1) + "-" +
                                  std::to_string(std::min(scroll_row + visible_rows, total_rows)) +
                                  "/" + std::to_string(total_rows);
        Element scroll_indicator = text(scroll_info) | dim | center;

        return vbox({
            header,
            help,
            separator(),
            game_grid | flex,
            separator(),
            scroll_indicator,
        }) | border;
    });

    // Handle keyboard input
    main_component = CatchEvent(main_component, [&](Event event) {
        int num_games = state.size();
        if (num_games == 0) num_games = total_games;

        if (event == Event::Character('q') || event == Event::Escape) {
            screen.Exit();
            return true;
        }

        // Arrow keys for card selection
        if (event == Event::ArrowRight) {
            if (selected_game < num_games - 1) {
                selected_game++;
            }
            return true;
        }
        if (event == Event::ArrowLeft) {
            if (selected_game > 0) {
                selected_game--;
            }
            return true;
        }
        if (event == Event::ArrowDown) {
            if (selected_game + cols < num_games) {
                selected_game += cols;
            }
            return true;
        }
        if (event == Event::ArrowUp) {
            if (selected_game - cols >= 0) {
                selected_game -= cols;
            }
            return true;
        }

        // Page up/down for scrolling
        if (event == Event::PageDown) {
            int visible_rows = 3;  // Move by 3 rows
            selected_game = std::min(selected_game + cols * visible_rows, num_games - 1);
            return true;
        }
        if (event == Event::PageUp) {
            int visible_rows = 3;
            selected_game = std::max(selected_game - cols * visible_rows, 0);
            return true;
        }

        // Move navigation within selected game
        // , = prev move, . = next move
        // ; = prev 10 moves, : = next 10 moves
        auto step_move = [&](int delta) {
            int current_view = state.get_view_move(selected_game);
            int move_count = state.get_move_count(selected_game);
            if (move_count == 0) return;

            int effective_view = (current_view < 0) ? move_count - 1 : current_view;
            int new_view = std::clamp(effective_view + delta, 0, move_count - 1);

            if (new_view == move_count - 1) {
                state.set_view_move(selected_game, -1);  // Latest
            } else {
                state.set_view_move(selected_game, new_view);
            }
        };

        if (event == Event::Character(',')) {
            step_move(-1);
            return true;
        }
        if (event == Event::Character('.')) {
            step_move(1);
            return true;
        }
        if (event == Event::Character(';')) {
            step_move(-10);
            return true;
        }
        if (event == Event::Character(':')) {
            step_move(10);
            return true;
        }

        // Home = first move, End = last move (for selected game)
        if (event == Event::Home) {
            state.set_view_move(selected_game, 0);
            return true;
        }
        if (event == Event::End) {
            state.set_view_move(selected_game, -1);  // -1 = latest
            return true;
        }

        // Redraw on custom event (from worker threads)
        if (event == Event::Custom) {
            return true;
        }
        return false;
    });

    // Spawn worker threads (they will wait for screen_ready)
    log_msg("Main: spawning " + std::to_string(num_threads) + " worker threads");
    auto start_time = std::chrono::steady_clock::now();
    std::atomic<bool> fatal_error{false};
    std::atomic<bool> screen_ready{false};
    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&, i]() {
            log_msg("Thread[" + std::to_string(i) + "] waiting for screen_ready");
            // Wait for screen to be ready before starting
            while (!screen_ready.load() && !fatal_error.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            log_msg("Thread[" + std::to_string(i) + "] screen_ready=" + std::to_string(screen_ready.load()) +
                    ", fatal_error=" + std::to_string(fatal_error.load()));
            if (!fatal_error.load()) {
                worker_thread(i, engine1_path, engine2_path,
                             movetime_ms, hash_mb, work_queue, results,
                             state, screen, fatal_error);
            }
            log_msg("Thread[" + std::to_string(i) + "] exiting");
        });
    }

    // Monitor thread to exit when all games are done or on fatal error
    std::thread monitor([&] {
        log_msg("Monitor thread started");
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (fatal_error.load()) {
                log_msg("Monitor: fatal_error detected, exiting");
                break;  // Exit already called by worker thread
            }
            int completed = results.get_completed();
            if (completed >= total_games) {
                log_msg("Monitor: all " + std::to_string(completed) + " games completed");
                std::this_thread::sleep_for(std::chrono::seconds(1));  // Brief pause to see final state
                screen.Exit();
                break;
            }
        }
        log_msg("Monitor thread exiting");
    });

    // Signal that screen is ready via Post (runs after loop starts)
    log_msg("Main: calling screen.Post to set screen_ready");
    screen.Post([&] {
        log_msg("Main: screen.Post callback - setting screen_ready=true");
        screen_ready.store(true);
    });
    log_msg("Main: entering screen.Loop()");
    screen.Loop(main_component);
    log_msg("Main: screen.Loop() returned");

    // Wait for all threads to complete
    for (auto& t : threads) {
        t.join();
    }
    monitor.join();

    auto end_time = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time).count();

    // Aggregate final results
    auto all_results = results.get_results();
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

    // Print final score to stdout
    std::cout << "\n========================================" << std::endl;
    std::cout << "Final Score (" << total_games << " games in " << elapsed << "s):" << std::endl;
    std::cout << "========================================" << std::endl;

    double pct1 = (total_games > 0) ? (100.0 * score1 / total_games) : 0;
    double pct2 = (total_games > 0) ? (100.0 * score2 / total_games) : 0;

    std::cout << "  " << engine1_path << ": " << score1 << "/" << total_games
              << " (" << pct1 << "%) [W:" << wins1 << " D:" << draws << " L:" << wins2 << "]" << std::endl;
    std::cout << "  " << engine2_path << ": " << score2 << "/" << total_games
              << " (" << pct2 << "%) [W:" << wins2 << " D:" << draws << " L:" << wins1 << "]" << std::endl;

    log_msg("Match finished");
    log_close();
    return 0;
}
