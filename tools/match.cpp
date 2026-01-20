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
#include <csignal>
#include <fcntl.h>

// ============================================================================
// Debug logging
// ============================================================================
static std::mutex log_mutex;
static std::ofstream log_file;
static bool logging_enabled = false;

// ============================================================================
// Console message buffer (for TUI display)
// ============================================================================
class ConsoleBuffer {
    std::vector<std::string> messages;
    mutable std::mutex mtx;
    static constexpr size_t MAX_MESSAGES = 100;

public:
    void add(const std::string& msg) {
        std::lock_guard<std::mutex> lock(mtx);
        messages.push_back(msg);
        if (messages.size() > MAX_MESSAGES) {
            messages.erase(messages.begin());
        }
    }

    std::vector<std::string> get_all() const {
        std::lock_guard<std::mutex> lock(mtx);
        return messages;
    }

    std::vector<std::string> get_last(size_t n) const {
        std::lock_guard<std::mutex> lock(mtx);
        if (messages.size() <= n) {
            return messages;
        }
        return std::vector<std::string>(messages.end() - n, messages.end());
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mtx);
        return messages.size();
    }
};

static ConsoleBuffer console_buffer;

void console_msg(const std::string& msg) {
    console_buffer.add(msg);
}

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

// ============================================================================
// Signal handling globals - uses self-pipe pattern for async-signal-safety
// ============================================================================
static int g_signal_pipe[2] = {-1, -1};

void signal_handler(int sig) {
    // Only async-signal-safe operations: write single byte to pipe
    // This is safe because write() to a pipe is async-signal-safe
    char c = (char)sig;
    (void)write(g_signal_pipe[1], &c, 1);  // Ignore errors - best effort
}

// Initialize the signal pipe - call before setting up signal handlers
bool init_signal_pipe() {
    if (pipe(g_signal_pipe) < 0) {
        return false;
    }
    // Set write end to non-blocking so signal handler never blocks
    int flags = fcntl(g_signal_pipe[1], F_GETFL, 0);
    fcntl(g_signal_pipe[1], F_SETFL, flags | O_NONBLOCK);
    return true;
}

void close_signal_pipe() {
    if (g_signal_pipe[0] >= 0) {
        close(g_signal_pipe[0]);
        g_signal_pipe[0] = -1;
    }
    if (g_signal_pipe[1] >= 0) {
        close(g_signal_pipe[1]);
        g_signal_pipe[1] = -1;
    }
}

// Move result with stats (declared before Engine class so it can be used as return type)
struct MoveResult {
    std::string bestmove;
    int depth = 0;
    u64 nodes = 0;
};

// UCI Engine wrapper - manages subprocess communication
class Engine {
    FILE* to_engine;
    int from_engine_fd;  // Raw file descriptor for reading (no FILE* buffering issues)
    int stderr_fd;       // File descriptor for engine's stderr
    std::string read_buffer;  // Our own line buffer
    std::string name;
    std::string path;
    pid_t child_pid;
    static int instance_counter;
    int instance_id;
    int current_game_id = -1;  // Set by caller to include in logs
    std::thread stderr_thread;
    std::atomic<bool> stopping{false};

    std::string log_prefix() const {
        if (current_game_id >= 0) {
            return "Engine[" + std::to_string(instance_id) + "] Game[" + std::to_string(current_game_id + 1) + "]";
        }
        return "Engine[" + std::to_string(instance_id) + "]";
    }

    std::string short_name() const {
        size_t pos = path.rfind('/');
        return (pos != std::string::npos) ? path.substr(pos + 1) : path;
    }

    void stderr_reader() {
        std::string buffer;
        char buf[1024];
        while (!stopping.load()) {
            struct pollfd pfd;
            pfd.fd = stderr_fd;
            pfd.events = POLLIN;

            int ret = poll(&pfd, 1, 100);  // 100ms timeout
            if (ret <= 0) continue;

            if (pfd.revents & POLLIN) {
                ssize_t n = read(stderr_fd, buf, sizeof(buf) - 1);
                if (n <= 0) break;
                buf[n] = '\0';
                buffer += buf;

                // Process complete lines
                size_t pos;
                while ((pos = buffer.find('\n')) != std::string::npos) {
                    std::string line = buffer.substr(0, pos);
                    buffer.erase(0, pos + 1);
                    if (!line.empty()) {
                        console_msg("[" + short_name() + "] " + line);
                    }
                }
            }
            if (pfd.revents & (POLLHUP | POLLERR)) break;
        }
        // Flush any remaining partial line
        if (!buffer.empty()) {
            console_msg("[" + short_name() + "] " + buffer);
        }
    }

public:
    Engine(const std::string& engine_path) : path(engine_path), instance_id(++instance_counter) {
        log_msg("Engine[" + std::to_string(instance_id) + "] creating: " + engine_path);

        // Create pipes: stdin, stdout, and stderr
        int to_child[2], from_child[2], err_child[2];
        if (pipe(to_child) < 0 || pipe(from_child) < 0 || pipe(err_child) < 0) {
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
            close(err_child[0]);
            dup2(to_child[0], STDIN_FILENO);
            dup2(from_child[1], STDOUT_FILENO);
            dup2(err_child[1], STDERR_FILENO);
            close(to_child[0]);
            close(from_child[1]);
            close(err_child[1]);
            execlp(engine_path.c_str(), engine_path.c_str(), nullptr);
            _exit(1);
        }

        // Parent process
        child_pid = pid;
        log_msg("Engine[" + std::to_string(instance_id) + "] forked, child pid=" + std::to_string(pid));
        close(to_child[0]);
        close(from_child[1]);
        close(err_child[1]);
        to_engine = fdopen(to_child[1], "w");
        from_engine_fd = from_child[0];  // Keep raw fd for reading
        stderr_fd = err_child[0];        // Keep raw fd for stderr

        if (!to_engine || from_engine_fd < 0 || stderr_fd < 0) {
            log_msg("Engine[" + std::to_string(instance_id) + "] FAILED to open streams");
            throw std::runtime_error("Failed to open engine streams");
        }

        setvbuf(to_engine, nullptr, _IOLBF, 0);  // Line buffered for writes

        // Start stderr reader thread
        stderr_thread = std::thread(&Engine::stderr_reader, this);

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

    void set_hash(int mb, std::atomic<bool>* stop_flag = nullptr) {
        send("setoption name Hash value " + std::to_string(mb));
        send("isready");
        wait_for("readyok", 10000, stop_flag);
    }

    ~Engine() {
        // IMPORTANT: Destructors must never throw exceptions
        // 1. Signal stderr reader to stop and wait for it first
        //    (must join before closing fds to avoid read-after-close)
        stopping.store(true);
        if (stderr_thread.joinable()) {
            stderr_thread.join();
        }

        // 2. Try graceful shutdown
        if (to_engine) {
            fprintf(to_engine, "quit\n");
            fflush(to_engine);
            fclose(to_engine);
            to_engine = nullptr;
        }

        // 3. Close read fds
        if (from_engine_fd >= 0) {
            close(from_engine_fd);
            from_engine_fd = -1;
        }
        if (stderr_fd >= 0) {
            close(stderr_fd);
            stderr_fd = -1;
        }

        // 4. Wait for child with timeout, then force kill if needed
        if (child_pid > 0) {
            int status;
            // Try non-blocking wait first
            pid_t result = waitpid(child_pid, &status, WNOHANG);
            if (result == 0) {
                // Child still running, give it 100ms then SIGKILL
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                result = waitpid(child_pid, &status, WNOHANG);
                if (result == 0) {
                    kill(child_pid, SIGKILL);
                    waitpid(child_pid, &status, 0);
                }
            }
        }
    }

    void set_game_id(int game_id) { current_game_id = game_id; }

    void send(const std::string& cmd) {
        if (!to_engine) {
            log_msg(log_prefix() + " SEND FAILED - closed: " + cmd);
            throw std::runtime_error("Cannot send to closed engine " + path);
        }
        log_msg(log_prefix() + " >> " + cmd);
        if (fprintf(to_engine, "%s\n", cmd.c_str()) < 0) {
            log_msg(log_prefix() + " SEND FAILED - fprintf error");
            throw std::runtime_error("Failed to send to engine " + path);
        }
        if (fflush(to_engine) != 0) {
            log_msg(log_prefix() + " SEND FAILED - flush error");
            throw std::runtime_error("Failed to flush to engine " + path);
        }
    }

    std::string read_line(int timeout_ms = 30000, std::atomic<bool>* stop_flag = nullptr) {
        // Check if we already have a complete line in the buffer
        size_t newline_pos = read_buffer.find('\n');
        if (newline_pos != std::string::npos) {
            std::string line = read_buffer.substr(0, newline_pos);
            read_buffer.erase(0, newline_pos + 1);
            log_msg(log_prefix() + " << " + line);
            return line;
        }

        // Need to read more data
        auto start_time = std::chrono::steady_clock::now();
        while (true) {
            // Check stop flag before each poll - enables responsive shutdown
            if (stop_flag && stop_flag->load(std::memory_order_acquire)) {
                log_msg(log_prefix() + " read interrupted by stop signal");
                throw std::runtime_error("Read interrupted by stop signal");
            }

            // Calculate remaining timeout
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start_time).count();
            int remaining_ms = timeout_ms - static_cast<int>(elapsed);
            if (remaining_ms <= 0) {
                log_msg(log_prefix() + " TIMEOUT after " + std::to_string(timeout_ms) + "ms");
                throw std::runtime_error("Engine " + path + " timed out after " + std::to_string(timeout_ms) + "ms");
            }

            struct pollfd pfd;
            pfd.fd = from_engine_fd;
            pfd.events = POLLIN;

            // Use short poll timeout (100ms) to check stop flag frequently
            int poll_timeout = std::min(100, remaining_ms);
            int ret = poll(&pfd, 1, poll_timeout);
            if (ret < 0) {
                if (errno == EINTR) {
                    continue;  // Interrupted by signal, retry
                }
                log_msg(log_prefix() + " poll FAILED: " + strerror(errno));
                throw std::runtime_error("poll() failed for engine " + path + ": " + strerror(errno));
            }
            if (ret == 0) {
                continue;  // Poll timeout, check stop flag and remaining time
            }

            // Check for errors
            if (!(pfd.revents & POLLIN)) {
                if (pfd.revents & POLLHUP) {
                    log_msg(log_prefix() + " POLLHUP - pipe closed");
                    throw std::runtime_error("Engine " + path + " closed pipe (crashed?)");
                }
                if (pfd.revents & (POLLERR | POLLNVAL)) {
                    log_msg(log_prefix() + " POLLERR/POLLNVAL");
                    throw std::runtime_error("Engine " + path + " pipe error");
                }
            }

            // Read available data
            char buf[4096];
            ssize_t n = read(from_engine_fd, buf, sizeof(buf));
            if (n < 0) {
                if (errno == EINTR) {
                    continue;  // Interrupted by signal, retry
                }
                log_msg(log_prefix() + " read FAILED: " + strerror(errno));
                throw std::runtime_error("read() failed for engine " + path + ": " + strerror(errno));
            }
            if (n == 0) {
                log_msg(log_prefix() + " EOF");
                throw std::runtime_error("Engine " + path + " closed connection (EOF)");
            }

            read_buffer.append(buf, n);

            // Check for complete line
            newline_pos = read_buffer.find('\n');
            if (newline_pos != std::string::npos) {
                std::string line = read_buffer.substr(0, newline_pos);
                read_buffer.erase(0, newline_pos + 1);
                log_msg(log_prefix() + " << " + line);
                return line;
            }
            // No complete line yet, continue reading
        }
    }

    void wait_for(const std::string& expected, int timeout_ms = 10000,
                  std::atomic<bool>* stop_flag = nullptr) {
        auto start_time = std::chrono::steady_clock::now();
        while (true) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start_time).count();
            int remaining_ms = timeout_ms - static_cast<int>(elapsed);
            if (remaining_ms <= 0) {
                log_msg(log_prefix() + " TIMEOUT waiting for '" + expected + "'");
                throw std::runtime_error("Timeout waiting for '" + expected + "' from engine " + path);
            }

            std::string line = read_line(remaining_ms, stop_flag);
            if (line.find(expected) == 0) {
                break;
            }
        }
    }

    MoveResult get_bestmove(const std::string& position, const std::vector<std::string>& moves,
                            int movetime_ms, std::atomic<bool>* stop_flag = nullptr) {
        std::string cmd = "position fen " + position;
        if (!moves.empty()) {
            cmd += " moves";
            for (const auto& m : moves) {
                cmd += " " + m;
            }
        }
        send(cmd);

        send("isready");
        wait_for("readyok", 10000, stop_flag);

        send("go movetime " + std::to_string(movetime_ms));

        MoveResult result;

        // Wait for bestmove with generous timeout (movetime + 10 seconds buffer)
        int timeout = movetime_ms + 10000;
        while (true) {
            std::string line = read_line(timeout, stop_flag);

            // Parse UCI info lines for depth and nodes
            if (line.find("info") == 0) {
                std::istringstream iss(line);
                std::string token;
                while (iss >> token) {
                    if (token == "depth") {
                        int d;
                        if (iss >> d) {
                            result.depth = d;
                        }
                    } else if (token == "nodes") {
                        u64 n;
                        if (iss >> n) {
                            result.nodes = n;
                        }
                    }
                }
            }

            if (line.find("bestmove") == 0) {
                // Parse "bestmove e2e4 ..."
                std::istringstream iss(line);
                std::string token;
                iss >> token >> result.bestmove;
                return result;
            }
        }
    }

    void new_game(std::atomic<bool>* stop_flag = nullptr) {
        send("ucinewgame");
        send("isready");
        wait_for("readyok", 10000, stop_flag);
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
    u64 white_depth = 0;     // Cumulative depth for white's moves
    u64 black_depth = 0;     // Cumulative depth for black's moves
    u64 white_nodes = 0;     // Cumulative nodes for white's moves
    u64 black_nodes = 0;     // Cumulative nodes for black's moves
    u64 white_moves = 0;     // Number of moves made by white
    u64 black_moves = 0;     // Number of moves made by black
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
                      GameUpdateCallback on_move = nullptr,
                      std::atomic<bool>* stop_flag = nullptr) {
    Board board(start_fen);
    std::vector<std::string> move_history;
    std::vector<u64> position_hashes;

    white.new_game(stop_flag);
    black.new_game(stop_flag);

    GameOutcome outcome;
    outcome.num_moves = 0;
    outcome.draw_reason = DrawReason::None;

    while (true) {
        // Check for stop signal at start of each move
        if (stop_flag && stop_flag->load(std::memory_order_acquire)) {
            // Abort game cleanly
            outcome.result = GameResult::Draw;
            outcome.draw_reason = DrawReason::None;
            break;
        }
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
        MoveResult move_result = current.get_bestmove(start_fen, move_history, movetime_ms, stop_flag);
        std::string uci_move = move_result.bestmove;

        // Track depth/nodes per color
        if (board.turn == ::Color::White) {
            outcome.white_depth += move_result.depth;
            outcome.white_nodes += move_result.nodes;
            outcome.white_moves++;
        } else {
            outcome.black_depth += move_result.depth;
            outcome.black_nodes += move_result.nodes;
            outcome.black_moves++;
        }

        // Validate move format (should be 4-5 chars: e2e4 or e7e8q)
        if (uci_move.length() < 4 || uci_move.length() > 5) {
            const std::string& engine_name = (board.turn == ::Color::White) ? white_name : black_name;
            log_msg("ERROR: Malformed move '" + uci_move + "' (len=" + std::to_string(uci_move.length()) +
                    ") from " + engine_name);
            outcome.result = (board.turn == ::Color::White) ? GameResult::BlackWin : GameResult::WhiteWin;
            break;
        }

        // Apply move
        Move32 move = parse_uci_move(uci_move, board);
        if (move.data == 0) {
            const std::string& engine_name = (board.turn == ::Color::White) ? white_name : black_name;
            log_msg("ERROR: Invalid move '" + uci_move + "' from " + engine_name +
                    " in position " + board.to_fen());
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
    std::atomic<u64> total_depth1{0};
    std::atomic<u64> total_depth2{0};
    std::atomic<u64> total_nodes1{0};
    std::atomic<u64> total_nodes2{0};
    std::atomic<u64> total_moves1{0};
    std::atomic<u64> total_moves2{0};
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

        // Update depth/nodes stats - map white/black to engine1/engine2
        if (report.engine1_is_white) {
            total_depth1.fetch_add(report.outcome.white_depth);
            total_depth2.fetch_add(report.outcome.black_depth);
            total_nodes1.fetch_add(report.outcome.white_nodes);
            total_nodes2.fetch_add(report.outcome.black_nodes);
            total_moves1.fetch_add(report.outcome.white_moves);
            total_moves2.fetch_add(report.outcome.black_moves);
        } else {
            total_depth1.fetch_add(report.outcome.black_depth);
            total_depth2.fetch_add(report.outcome.white_depth);
            total_nodes1.fetch_add(report.outcome.black_nodes);
            total_nodes2.fetch_add(report.outcome.white_nodes);
            total_moves1.fetch_add(report.outcome.black_moves);
            total_moves2.fetch_add(report.outcome.white_moves);
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
    u64 get_total_depth1() const { return total_depth1.load(); }
    u64 get_total_depth2() const { return total_depth2.load(); }
    u64 get_total_nodes1() const { return total_nodes1.load(); }
    u64 get_total_nodes2() const { return total_nodes2.load(); }
    u64 get_total_moves1() const { return total_moves1.load(); }
    u64 get_total_moves2() const { return total_moves2.load(); }

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
// TUI: Helper functions
// ============================================================================

// Format large numbers with K/M/G suffixes
std::string format_nodes(u64 n) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1);
    if (n >= 1000000000ULL) {
        ss << (n / 1000000000.0) << "G";
    } else if (n >= 1000000ULL) {
        ss << (n / 1000000.0) << "M";
    } else if (n >= 1000ULL) {
        ss << (n / 1000.0) << "K";
    } else {
        ss << n;
    }
    return ss.str();
}

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

Element render_game_card(const GameDisplay& game, const std::string& engine1_name, const std::string& engine2_name, bool selected, int card_width) {
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

    // Show which engine is which color - truncate names to fit
    const std::string& white_path = game.engine1_is_white ? engine1_name : engine2_name;
    const std::string& black_path = game.engine1_is_white ? engine2_name : engine1_name;
    std::string white_name = white_path.substr(white_path.rfind('/') + 1);
    std::string black_name = black_path.substr(black_path.rfind('/') + 1);

    // Truncate engine names if too long (card_width - border(2) - "W:"(2) - padding)
    int max_name_len = card_width - 6;
    if ((int)white_name.length() > max_name_len) {
        white_name = white_name.substr(0, max_name_len - 2) + "..";
    }
    if ((int)black_name.length() > max_name_len) {
        black_name = black_name.substr(0, max_name_len - 2) + "..";
    }

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

    // Inner width for content (excluding border)
    int inner_width = card_width - 2;

    auto card = vbox({
        text(header) | bold,
        hbox({text("W:") | dim, text(white_name)}),
        hbox({text("B:") | dim, text(black_name)}),
        text(move_info) | dim,
        separator(),
        render_board(display_fen),
        separator(),
        render_moves(game.moves, view_idx) | size(WIDTH, EQUAL, inner_width) | size(HEIGHT, LESS_THAN, 4),
    }) | size(WIDTH, EQUAL, inner_width);

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
    std::atomic<bool>& fatal_error,
    std::atomic<bool>& all_done,
    std::atomic<bool>& screen_exiting
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
            engine1.set_hash(hash_mb, &all_done);
            engine2.set_hash(hash_mb, &all_done);
        }

        GameTask task;
        while (!fatal_error.load() && !all_done.load() && work_queue.pop(task)) {
            Engine& white = task.engine1_is_white ? engine1 : engine2;
            Engine& black = task.engine1_is_white ? engine2 : engine1;
            const std::string& white_name = task.engine1_is_white ? engine1_path : engine2_path;
            const std::string& black_name = task.engine1_is_white ? engine2_path : engine1_path;

            // Set game_id on both engines for logging
            engine1.set_game_id(task.game_id);
            engine2.set_game_id(task.game_id);

            // Callback to update TUI on each move
            // Note: UI refresh is handled by dedicated refresh thread (10Hz) to avoid event queue flooding
            int game_id = task.game_id;
            auto on_move = [&state, game_id](int, const std::string& fen, const std::vector<std::string>& moves) {
                state.update_game(game_id, fen, moves);
            };

            try {
                GameOutcome outcome = play_game(white, black, task.fen, movetime_ms, white_name, black_name, on_move, &all_done);

                // Mark game as finished in TUI state
                // Note: UI refresh is handled by dedicated refresh thread
                state.finish_game(task.game_id, outcome.result, outcome.draw_reason);

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
                console_msg("Game " + std::to_string(task.game_id + 1) + " error: " + e.what());
                log_msg("Worker[" + std::to_string(thread_id) + "] game error, exiting: " + e.what());

                GameOutcome outcome;
                outcome.result = GameResult::Draw;
                outcome.draw_reason = DrawReason::None;
                outcome.num_moves = 0;

                state.finish_game(task.game_id, outcome.result, outcome.draw_reason);

                GameReport report;
                report.game_id = task.game_id;
                report.outcome = outcome;
                report.engine1_is_white = task.engine1_is_white;
                report.fen = task.fen;
                results.add(std::move(report));

                // Engine crashed, can't continue with this thread's engines
                // Exit the loop - engines are broken
                break;
            }
        }
    } catch (const std::exception& e) {
        // Thread-level error (e.g., engine failed to start)
        console_msg("Thread " + std::to_string(thread_id) + " fatal error: " + e.what());
        log_msg("Worker[" + std::to_string(thread_id) + "] fatal error: " + e.what());
        fatal_error.store(true);
        screen_exiting.store(true);
        // Post exit request to main thread (screen.Exit() is not thread-safe)
        screen.Post([&] { screen.Exit(); });
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
              << "  Arrows           Select game\n"
              << "  ,/.              Previous/next move\n"
              << "  ;/:              Jump 10 moves\n"
              << "  Home/End         First/last move\n"
              << "  PgUp/PgDn        Scroll games\n"
              << "  |                Toggle console\n"
              << "  q/Esc            Quit\n";
}

int main(int argc, char* argv[]) {
    zobrist::init();

    // Ignore SIGPIPE - writing to a pipe after child dies would otherwise crash us
    std::signal(SIGPIPE, SIG_IGN);

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

    // Add initialization messages to console
    console_msg("Match: " + engine1_path + " vs " + engine2_path);
    console_msg("Options: movetime=" + std::to_string(movetime_ms) + "ms, threads=" +
                std::to_string(num_threads) + ", hash=" + std::to_string(hash_mb) + "MB");

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
            game_id++;
        }
    }

    int total_games = game_id;
    log_msg("Total games to play: " + std::to_string(total_games));
    console_msg("Loaded " + std::to_string(positions.size()) + " position(s), " +
                std::to_string(total_games) + " games total");

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
    bool console_expanded = false;  // Whether console panel is expanded

    // Shared flags for thread coordination
    std::atomic<bool> fatal_error{false};
    std::atomic<bool> all_done{false};
    std::atomic<bool> screen_exiting{false};  // Prevents posting to screen after exit initiated

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

        // Add depth and nodes stats
        u64 moves1 = results.get_total_moves1();
        u64 moves2 = results.get_total_moves2();
        u64 depth1 = results.get_total_depth1();
        u64 depth2 = results.get_total_depth2();
        u64 nodes1 = results.get_total_nodes1();
        u64 nodes2 = results.get_total_nodes2();

        if (moves1 > 0 || moves2 > 0) {
            double avg_d1 = (moves1 > 0) ? (double)depth1 / moves1 : 0;
            double avg_d2 = (moves2 > 0) ? (double)depth2 / moves2 : 0;
            header_ss << " | D:" << std::setprecision(1) << avg_d1 << "/" << avg_d2
                      << " N:" << format_nodes(nodes1) << "/" << format_nodes(nodes2);
        }

        bool all_complete = (finished >= total_games);
        if (all_complete) {
            header_ss << "  COMPLETE";
        }

        Element header = text(header_ss.str()) | bold | center;
        if (all_complete) {
            header = header | color(ftxui::Color::Green);
        }

        // Help line
        Element help = text("Arrows=select  ,/.=move  ;/:=10moves  Home/End=first/last  |=console  q=quit") | dim | center;

        // Calculate columns and card width based on terminal width
        int term_width = Terminal::Size().dimx;
        int term_height = Terminal::Size().dimy;
        int min_card_width = 39;  // Minimum: board(35) + border(2) + padding(2)
        int max_card_width = 50;  // Maximum reasonable width
        int outer_padding = 4;    // Space for outer border

        // Calculate how many columns fit, then divide space evenly
        int available_width = term_width - outer_padding;
        cols = std::max(1, available_width / min_card_width);
        int card_width = std::min(max_card_width, available_width / cols);

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
                    row_elements.push_back(render_game_card(games[idx], engine1_path, engine2_path, is_selected, card_width));
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

        // Console panel
        Element console_panel;
        if (console_expanded) {
            // Full console view
            auto messages = console_buffer.get_all();
            Elements console_lines;
            for (const auto& msg : messages) {
                console_lines.push_back(text(msg));
            }
            if (console_lines.empty()) {
                console_lines.push_back(text("(no messages)") | dim);
            }
            console_panel = vbox({
                text("Console (press | to close)") | bold,
                separator(),
                vbox(console_lines) | vscroll_indicator | frame | size(HEIGHT, LESS_THAN, 15),
            }) | border;
        } else {
            // Mini console preview (last 2 lines)
            auto messages = console_buffer.get_last(2);
            Elements console_lines;
            for (const auto& msg : messages) {
                console_lines.push_back(text(msg) | dim);
            }
            if (!console_lines.empty()) {
                console_panel = hbox({
                    text("Console: ") | dim,
                    vbox(console_lines),
                });
            } else {
                console_panel = text("Console: (no messages)") | dim;
            }
        }

        if (console_expanded) {
            return vbox({
                header,
                help,
                separator(),
                console_panel | flex,
            }) | border;
        } else {
            return vbox({
                header,
                help,
                separator(),
                game_grid | flex,
                separator(),
                scroll_indicator,
                separator(),
                console_panel,
            }) | border;
        }
    });

    // Handle keyboard input
    main_component = CatchEvent(main_component, [&](Event event) {
        int num_games = state.size();
        if (num_games == 0) num_games = total_games;

        if (event == Event::Character('q') || event == Event::Escape) {
            screen_exiting.store(true);  // Prevent refresh thread from posting
            all_done.store(true);
            screen.Exit();
            return true;
        }

        // Toggle console panel
        if (event == Event::Character('|')) {
            console_expanded = !console_expanded;
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
    console_msg("Starting " + std::to_string(num_threads) + " worker thread(s)...");
    auto start_time = std::chrono::steady_clock::now();
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
            if (!fatal_error.load() && !all_done.load()) {
                worker_thread(i, engine1_path, engine2_path,
                             movetime_ms, hash_mb, work_queue, results,
                             state, screen, fatal_error, all_done, screen_exiting);
            }
            log_msg("Thread[" + std::to_string(i) + "] exiting");
        });
    }

    // Dedicated UI refresh thread - posts at fixed 5Hz rate to avoid event queue flooding
    // This replaces per-game screen.Post() calls from workers which could overwhelm the UI
    std::thread refresh_thread([&] {
        log_msg("Refresh thread started");
        while (!all_done.load() && !fatal_error.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            // Check screen_exiting to avoid posting after Exit() is called
            if (!screen_exiting.load()) {
                screen.Post(Event::Custom);
            }
        }
        log_msg("Refresh thread exiting");
    });

    // Initialize signal pipe for async-signal-safe shutdown
    if (!init_signal_pipe()) {
        std::cerr << "Failed to create signal pipe" << std::endl;
        fatal_error.store(true);
        all_done.store(true);
        for (auto& t : threads) {
            t.join();
        }
        refresh_thread.join();
        log_close();
        return 1;
    }

    // Signal watcher thread - polls the signal pipe and safely exits the screen
    // This avoids calling non-async-signal-safe FTXUI code from the signal handler
    std::thread signal_watcher_thread([&] {
        log_msg("Signal watcher thread started");
        struct pollfd pfd;
        pfd.fd = g_signal_pipe[0];
        pfd.events = POLLIN;

        while (!all_done.load()) {
            int ret = poll(&pfd, 1, 100);  // 100ms timeout
            if (ret > 0 && (pfd.revents & POLLIN)) {
                char sig;
                if (read(g_signal_pipe[0], &sig, 1) == 1) {
                    log_msg("Signal watcher: received signal " + std::to_string((int)sig));
                    // Set flags with proper memory ordering
                    all_done.store(true, std::memory_order_release);
                    screen_exiting.store(true, std::memory_order_release);
                    // Post exit to screen from non-signal context (safe)
                    screen.Post([&] { screen.Exit(); });
                    break;
                }
            }
        }
        log_msg("Signal watcher thread exiting");
    });

    // Set up signal handlers for graceful shutdown
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Signal that screen is ready via Post (runs after loop starts)
    log_msg("Main: calling screen.Post to set screen_ready");
    screen.Post([&] {
        log_msg("Main: screen.Post callback - setting screen_ready=true");
        screen_ready.store(true);
    });
    log_msg("Main: entering screen.Loop()");
    screen.Loop(main_component);
    log_msg("Main: screen.Loop() returned");

    // Reset signal handlers to default (screen is no longer valid)
    std::signal(SIGINT, SIG_DFL);
    std::signal(SIGTERM, SIG_DFL);

    // Ensure all_done is set so threads can exit
    all_done.store(true, std::memory_order_release);

    // Wait for signal watcher thread
    signal_watcher_thread.join();

    // Close signal pipe
    close_signal_pipe();

    // Wait for all threads to complete
    for (auto& t : threads) {
        t.join();
    }
    refresh_thread.join();

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

    // Print search statistics
    u64 moves1 = results.get_total_moves1();
    u64 moves2 = results.get_total_moves2();
    u64 depth1 = results.get_total_depth1();
    u64 depth2 = results.get_total_depth2();
    u64 nodes1 = results.get_total_nodes1();
    u64 nodes2 = results.get_total_nodes2();

    if (moves1 > 0 || moves2 > 0) {
        std::cout << "\nSearch Statistics:" << std::endl;
        double avg_d1 = (moves1 > 0) ? (double)depth1 / moves1 : 0;
        double avg_d2 = (moves2 > 0) ? (double)depth2 / moves2 : 0;
        std::cout << std::fixed << std::setprecision(1);
        std::cout << "  " << engine1_short << ": avg depth " << avg_d1
                  << ", total nodes " << format_nodes(nodes1)
                  << " (" << moves1 << " moves)" << std::endl;
        std::cout << "  " << engine2_short << ": avg depth " << avg_d2
                  << ", total nodes " << format_nodes(nodes2)
                  << " (" << moves2 << " moves)" << std::endl;
    }

    log_msg("Match finished");
    log_close();
    return 0;
}
