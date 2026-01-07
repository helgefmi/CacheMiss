// Lichess Bot - connects CacheMiss to Lichess via their Bot API
//
// Usage: ./lichess --token <token> --engine ./build/cachemiss [options]

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include "json.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <getopt.h>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

using json = nlohmann::json;

// ============================================================================
// Global shutdown flag
// ============================================================================
static std::atomic<bool> g_shutdown{false};

void signal_handler(int) { g_shutdown.store(true); }

// ============================================================================
// Logging
// ============================================================================
static std::mutex log_mutex;
static std::ofstream log_file;
static bool logging_enabled = false;
static bool quiet_mode = false;

void log_init(const std::string& filename) {
    std::lock_guard<std::mutex> lock(log_mutex);
    log_file.open(filename, std::ios::out | std::ios::app);
    logging_enabled = log_file.is_open();
    if (logging_enabled) {
        log_file << "\n=== Lichess bot started ===" << std::endl;
    }
}

void log_close() {
    std::lock_guard<std::mutex> lock(log_mutex);
    if (log_file.is_open()) {
        log_file << "=== Lichess bot stopped ===" << std::endl;
        log_file.close();
    }
    logging_enabled = false;
}

std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) %
              1000;
    auto time = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf;
    localtime_r(&time, &tm_buf);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", &tm_buf);
    std::ostringstream oss;
    oss << time_str << "." << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

void log_msg(const std::string& msg) {
    std::string ts = timestamp();
    if (!quiet_mode) {
        std::cout << "[" << ts << "] " << msg << std::endl;
    }
    if (logging_enabled) {
        std::lock_guard<std::mutex> lock(log_mutex);
        log_file << "[" << ts << "] " << msg << std::endl;
        log_file.flush();
    }
}

void log_error(const std::string& msg) {
    std::string ts = timestamp();
    std::cerr << "[" << ts << "] ERROR: " << msg << std::endl;
    if (logging_enabled) {
        std::lock_guard<std::mutex> lock(log_mutex);
        log_file << "[" << ts << "] ERROR: " << msg << std::endl;
        log_file.flush();
    }
}

// ============================================================================
// Configuration
// ============================================================================
struct LichessConfig {
    std::string token;
    std::string engine_path = "./build/cachemiss";
    int hash_mb = 256;

    // Challenge acceptance criteria
    int min_time_initial = 60;   // seconds
    int max_time_initial = 900;  // seconds
    int min_time_increment = 0;  // seconds
    int max_time_increment = 30; // seconds
    bool accept_rated = true;
    bool accept_casual = true;

    // Seeking configuration
    bool auto_seek = false;
    int seek_time = 180;      // seconds
    int seek_increment = 2;   // seconds

    // Concurrent games
    int max_games = 4;

    // Logging
    std::string log_file;
};

// ============================================================================
// UCI Engine subprocess (based on match.cpp Engine class)
// ============================================================================
class Engine {
    FILE* to_engine = nullptr;
    int from_engine_fd = -1;
    std::string read_buffer;
    pid_t child_pid = -1;
    std::string path;

public:
    Engine(const std::string& engine_path, int hash_mb) : path(engine_path) {
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
        from_engine_fd = from_child[0];

        if (!to_engine || from_engine_fd < 0) {
            throw std::runtime_error("Failed to open engine streams");
        }

        setvbuf(to_engine, nullptr, _IOLBF, 0);

        // Initialize UCI
        send("uci");
        wait_for("uciok");

        // Set hash size
        send("setoption name Hash value " + std::to_string(hash_mb));

        send("isready");
        wait_for("readyok");
    }

    ~Engine() {
        if (to_engine) {
            fprintf(to_engine, "quit\n");
            fflush(to_engine);
            fclose(to_engine);
            to_engine = nullptr;
        }
        if (from_engine_fd >= 0) {
            close(from_engine_fd);
            from_engine_fd = -1;
        }
        if (child_pid > 0) {
            int status;
            waitpid(child_pid, &status, 0);
        }
    }

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void send(const std::string& cmd) {
        if (!to_engine) {
            throw std::runtime_error("Cannot send to closed engine");
        }
        log_msg("Engine <- " + cmd);
        if (fprintf(to_engine, "%s\n", cmd.c_str()) < 0 ||
            fflush(to_engine) != 0) {
            throw std::runtime_error("Failed to send to engine");
        }
    }

    std::string read_line(int timeout_ms = 30000) {
        // Check for complete line in buffer
        size_t newline_pos = read_buffer.find('\n');
        if (newline_pos != std::string::npos) {
            std::string line = read_buffer.substr(0, newline_pos);
            read_buffer.erase(0, newline_pos + 1);
            return line;
        }

        auto start_time = std::chrono::steady_clock::now();
        while (true) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - start_time)
                               .count();
            int remaining_ms = timeout_ms - static_cast<int>(elapsed);
            if (remaining_ms <= 0) {
                throw std::runtime_error("Engine timed out");
            }

            struct pollfd pfd;
            pfd.fd = from_engine_fd;
            pfd.events = POLLIN;

            int ret = poll(&pfd, 1, remaining_ms);
            if (ret < 0) {
                throw std::runtime_error("poll() failed");
            }
            if (ret == 0)
                continue;

            if (pfd.revents & POLLHUP) {
                throw std::runtime_error("Engine closed pipe");
            }
            if (pfd.revents & (POLLERR | POLLNVAL)) {
                throw std::runtime_error("Engine pipe error");
            }

            char buf[4096];
            ssize_t n = read(from_engine_fd, buf, sizeof(buf));
            if (n <= 0) {
                throw std::runtime_error("Engine closed connection");
            }

            read_buffer.append(buf, static_cast<size_t>(n));

            newline_pos = read_buffer.find('\n');
            if (newline_pos != std::string::npos) {
                std::string line = read_buffer.substr(0, newline_pos);
                read_buffer.erase(0, newline_pos + 1);
                // Log significant responses (not info spam)
                if (line.find("bestmove") == 0 || line.find("readyok") == 0 ||
                    line.find("uciok") == 0 || line.find("id ") == 0) {
                    log_msg("Engine -> " + line);
                }
                return line;
            }
        }
    }

    void wait_for(const std::string& expected) {
        while (true) {
            std::string line = read_line();
            if (line.find(expected) == 0) {
                break;
            }
        }
    }

    std::string get_bestmove(const std::string& fen,
                             const std::vector<std::string>& moves, int wtime,
                             int btime, int winc, int binc) {
        // Handle "startpos" specially - it's not a valid FEN
        std::string cmd;
        if (fen == "startpos") {
            cmd = "position startpos";
        } else {
            cmd = "position fen " + fen;
        }
        if (!moves.empty()) {
            cmd += " moves";
            for (const auto& m : moves) {
                cmd += " " + m;
            }
        }
        send(cmd);

        send("isready");
        wait_for("readyok");

        // Use time control
        std::ostringstream go_cmd;
        go_cmd << "go wtime " << wtime << " btime " << btime << " winc " << winc
               << " binc " << binc;
        send(go_cmd.str());

        // Wait for bestmove (generous timeout: max time + buffer)
        int timeout = std::max(wtime, btime) + 30000;
        while (true) {
            std::string line = read_line(timeout);
            if (line.find("bestmove") == 0) {
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
};

// ============================================================================
// Lichess API Client
// ============================================================================
class LichessClient {
    std::string token;
    std::string auth_header;

    // Create a fresh client for each API call to avoid stale connections
    std::unique_ptr<httplib::SSLClient> make_client(int timeout_sec = 30) {
        auto cli = std::make_unique<httplib::SSLClient>("lichess.org", 443);
        cli->set_read_timeout(timeout_sec);
        cli->set_connection_timeout(timeout_sec);
        return cli;
    }

public:
    LichessClient(const std::string& tok)
        : token(tok), auth_header("Bearer " + tok) {}

    bool make_move(const std::string& game_id, const std::string& move) {
        auto cli = make_client();
        httplib::Headers headers = {{"Authorization", auth_header}};
        auto res = cli->Post("/api/bot/game/" + game_id + "/move/" + move, headers);
        return res && res->status == 200;
    }

    bool accept_challenge(const std::string& challenge_id) {
        auto cli = make_client();
        httplib::Headers headers = {{"Authorization", auth_header}};
        auto res = cli->Post("/api/challenge/" + challenge_id + "/accept", headers);
        if (!res) {
            log_error("Accept challenge " + challenge_id + ": connection error - " + httplib::to_string(res.error()));
            return false;
        }
        if (res->status != 200) {
            log_error("Accept challenge " + challenge_id + ": HTTP " + std::to_string(res->status) + " - " + res->body);
            return false;
        }
        return true;
    }

    bool decline_challenge(const std::string& challenge_id,
                           const std::string& reason = "generic") {
        auto cli = make_client();
        httplib::Headers headers = {{"Authorization", auth_header},
                                    {"Content-Type", "application/x-www-form-urlencoded"}};
        std::string body = "reason=" + reason;
        auto res = cli->Post("/api/challenge/" + challenge_id + "/decline",
                             headers, body, "application/x-www-form-urlencoded");
        return res && res->status == 200;
    }

    bool resign_game(const std::string& game_id) {
        auto cli = make_client();
        httplib::Headers headers = {{"Authorization", auth_header}};
        auto res = cli->Post("/api/bot/game/" + game_id + "/resign", headers);
        return res && res->status == 200;
    }

    bool abort_game(const std::string& game_id) {
        auto cli = make_client();
        httplib::Headers headers = {{"Authorization", auth_header}};
        auto res = cli->Post("/api/bot/game/" + game_id + "/abort", headers);
        return res && res->status == 200;
    }

    // Challenge another user
    bool challenge_user(const std::string& username, int time_secs,
                        int increment_secs, bool rated = true) {
        auto cli = make_client();
        httplib::Headers headers = {{"Authorization", auth_header},
                                    {"Content-Type", "application/x-www-form-urlencoded"}};
        std::ostringstream body;
        body << "rated=" << (rated ? "true" : "false")
             << "&clock.limit=" << time_secs
             << "&clock.increment=" << increment_secs;
        auto res = cli->Post("/api/challenge/" + username, headers,
                             body.str(), "application/x-www-form-urlencoded");
        return res && (res->status == 200 || res->status == 201);
    }

    // Get list of online bots
    std::vector<std::string> get_online_bots(int limit = 50) {
        auto cli = make_client();
        httplib::Headers headers = {{"Authorization", auth_header},
                                    {"Accept", "application/x-ndjson"}};
        auto res = cli->Get("/api/bot/online?nb=" + std::to_string(limit), headers);

        std::vector<std::string> bots;
        if (res && res->status == 200) {
            std::istringstream stream(res->body);
            std::string line;
            while (std::getline(stream, line)) {
                if (line.empty()) continue;
                try {
                    json bot = json::parse(line);
                    if (bot.contains("username")) {
                        bots.push_back(bot["username"].get<std::string>());
                    }
                } catch (...) {
                    // Skip malformed lines
                }
            }
        }
        return bots;
    }
};

// ============================================================================
// Game State
// ============================================================================
struct GameState {
    std::string game_id;
    std::string initial_fen;
    std::vector<std::string> moves;
    bool we_are_white = true;
    int wtime = 0, btime = 0, winc = 0, binc = 0;
    std::string status;

    bool is_our_turn() const {
        bool white_to_move = (moves.size() % 2 == 0);
        return (we_are_white == white_to_move);
    }

    bool is_game_over() const {
        return status == "mate" || status == "resign" || status == "stalemate" ||
               status == "timeout" || status == "draw" || status == "outoftime" ||
               status == "aborted" || status == "noStart";
    }
};

// ============================================================================
// Challenge Handler
// ============================================================================
class ChallengeHandler {
    const LichessConfig& config;

public:
    ChallengeHandler(const LichessConfig& cfg) : config(cfg) {}

    bool should_accept(const json& challenge) const {
        // Check variant
        std::string variant = challenge.value("/variant/key"_json_pointer, "standard");
        if (variant != "standard") {
            return false;
        }

        // Check time control
        if (!challenge.contains("timeControl") ||
            challenge["timeControl"]["type"] != "clock") {
            return false;  // Only accept timed games
        }

        int initial = challenge["timeControl"]["limit"].get<int>();
        int increment = challenge["timeControl"]["increment"].get<int>();

        if (initial < config.min_time_initial ||
            initial > config.max_time_initial) {
            return false;
        }
        if (increment < config.min_time_increment ||
            increment > config.max_time_increment) {
            return false;
        }

        // Check rated/casual
        bool rated = challenge.value("rated", false);
        if (rated && !config.accept_rated) {
            return false;
        }
        if (!rated && !config.accept_casual) {
            return false;
        }

        return true;
    }

    std::string decline_reason(const json& challenge) const {
        std::string variant = challenge.value("/variant/key"_json_pointer, "standard");
        if (variant != "standard") {
            return "variant";
        }

        if (!challenge.contains("timeControl") ||
            challenge["timeControl"]["type"] != "clock") {
            return "timeControl";
        }

        int initial = challenge["timeControl"]["limit"].get<int>();
        int increment = challenge["timeControl"]["increment"].get<int>();

        if (initial < config.min_time_initial ||
            initial > config.max_time_initial ||
            increment < config.min_time_increment ||
            increment > config.max_time_increment) {
            return "timeControl";
        }

        bool rated = challenge.value("rated", false);
        if ((rated && !config.accept_rated) ||
            (!rated && !config.accept_casual)) {
            return "casual";
        }

        return "generic";
    }
};

// ============================================================================
// Game Thread - handles a single game
// ============================================================================
void game_thread(const std::string& game_id, const LichessConfig& config,
                 const std::string& our_username) {
    log_msg("Game " + game_id + ": Starting game thread");

    try {
        // Create engine for this game
        Engine engine(config.engine_path, config.hash_mb);
        engine.new_game();

        // Create client for this game
        LichessClient client(config.token);

        // Connect to game stream
        httplib::SSLClient stream_client("lichess.org", 443);
        stream_client.set_read_timeout(std::chrono::hours(24));  // Long timeout for streaming
        stream_client.set_connection_timeout(30);
        stream_client.set_keep_alive(true);

        httplib::Headers headers = {{"Authorization", "Bearer " + config.token},
                                    {"Accept", "application/x-ndjson"}};

        GameState state;
        state.game_id = game_id;

        // Helper for case-insensitive comparison
        auto str_ieq = [](const std::string& a, const std::string& b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(a[i])) !=
                    std::tolower(static_cast<unsigned char>(b[i]))) {
                    return false;
                }
            }
            return true;
        };

        // Stream game events
        auto res = stream_client.Get(
            "/api/bot/game/stream/" + game_id, headers,
            [&](const char* data, size_t len) -> bool {
                if (g_shutdown.load()) {
                    return false;
                }

                std::string chunk(data, len);
                std::istringstream stream(chunk);
                std::string line;

                while (std::getline(stream, line)) {
                    if (line.empty())
                        continue;  // Keep-alive

                    log_msg("Game " + game_id + ": Received: " + line.substr(0, 100) + (line.size() > 100 ? "..." : ""));

                    try {
                        json event = json::parse(line);
                        std::string type = event.value("type", "");

                        if (type == "gameFull") {
                            // Initial game state
                            state.initial_fen = event.value(
                                "/initialFen"_json_pointer,
                                "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");

                            // Determine our color (case-insensitive comparison)
                            std::string white_id =
                                event.value("/white/id"_json_pointer, "");
                            std::string white_name =
                                event.value("/white/name"_json_pointer, "");
                            state.we_are_white =
                                str_ieq(white_id, our_username) || str_ieq(white_name, our_username);

                            log_msg("Game " + game_id + ": We are " +
                                    (state.we_are_white ? "white" : "black"));

                            // Parse initial state
                            auto game_state = event["state"];
                            std::string moves_str =
                                game_state.value("moves", "");
                            state.moves.clear();
                            if (!moves_str.empty()) {
                                std::istringstream iss(moves_str);
                                std::string move;
                                while (iss >> move) {
                                    state.moves.push_back(move);
                                }
                            }

                            state.wtime = game_state.value("wtime", 60000);
                            state.btime = game_state.value("btime", 60000);
                            state.winc = game_state.value("winc", 0);
                            state.binc = game_state.value("binc", 0);
                            state.status = game_state.value("status", "started");

                            // Make move if it's our turn
                            if (!state.is_game_over() && state.is_our_turn()) {
                                log_msg("Game " + game_id + ": Calculating move (wtime=" +
                                        std::to_string(state.wtime) + ", btime=" +
                                        std::to_string(state.btime) + ")");
                                std::string best = engine.get_bestmove(
                                    state.initial_fen, state.moves,
                                    state.wtime, state.btime, state.winc,
                                    state.binc);
                                log_msg("Game " + game_id + ": Playing " + best);
                                if (!client.make_move(game_id, best)) {
                                    log_error("Game " + game_id +
                                              ": Failed to make move " + best);
                                }
                            } else {
                                log_msg("Game " + game_id + ": Waiting for opponent (game_over=" +
                                        std::string(state.is_game_over() ? "true" : "false") +
                                        ", our_turn=" + std::string(state.is_our_turn() ? "true" : "false") + ")");
                            }
                        } else if (type == "gameState") {
                            // Game state update
                            std::string moves_str = event.value("moves", "");
                            state.moves.clear();
                            if (!moves_str.empty()) {
                                std::istringstream iss(moves_str);
                                std::string move;
                                while (iss >> move) {
                                    state.moves.push_back(move);
                                }
                            }

                            state.wtime = event.value("wtime", state.wtime);
                            state.btime = event.value("btime", state.btime);
                            state.status = event.value("status", "started");

                            if (state.is_game_over()) {
                                log_msg("Game " + game_id + ": Game over (" +
                                        state.status + ")");
                                return false;  // Stop streaming
                            }

                            // Make move if it's our turn
                            if (state.is_our_turn()) {
                                std::string best = engine.get_bestmove(
                                    state.initial_fen, state.moves,
                                    state.wtime, state.btime, state.winc,
                                    state.binc);
                                log_msg("Game " + game_id + ": Playing " + best);
                                if (!client.make_move(game_id, best)) {
                                    log_error("Game " + game_id +
                                              ": Failed to make move " + best);
                                }
                            }
                        } else if (type == "chatLine") {
                            // Ignore chat for now
                        }
                    } catch (const json::exception& e) {
                        log_error("Game " + game_id +
                                  ": JSON parse error: " + e.what());
                    }
                }

                return !g_shutdown.load();
            });

        if (res) {
            log_msg("Game " + game_id + ": Stream ended (HTTP " + std::to_string(res->status) + ")");
        } else {
            log_msg("Game " + game_id + ": Stream error: " + httplib::to_string(res.error()));
        }

    } catch (const std::exception& e) {
        log_error("Game " + game_id + ": Exception: " + e.what());
    } catch (...) {
        log_error("Game " + game_id + ": Unknown exception");
    }

    log_msg("Game " + game_id + ": Thread exiting");
}

// ============================================================================
// Game Manager - tracks active games
// ============================================================================
class GameManager {
    std::map<std::string, std::thread> active_games;
    mutable std::mutex mtx;
    const LichessConfig& config;
    std::string our_username;

public:
    GameManager(const LichessConfig& cfg, const std::string& username)
        : config(cfg), our_username(username) {}

    ~GameManager() { shutdown_all(); }

    bool start_game(const std::string& game_id) {
        std::lock_guard<std::mutex> lock(mtx);

        if (active_games.count(game_id) > 0) {
            return false;  // Already tracking this game
        }

        if (active_games.size() >= static_cast<size_t>(config.max_games)) {
            log_msg("Cannot start game " + game_id +
                    ": max concurrent games reached");
            return false;
        }

        active_games[game_id] =
            std::thread(game_thread, game_id, std::ref(config), our_username);
        return true;
    }

    void end_game(const std::string& game_id) {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = active_games.find(game_id);
        if (it != active_games.end()) {
            if (it->second.joinable()) {
                it->second.join();
            }
            active_games.erase(it);
        }
    }

    void cleanup_finished() {
        // Note: Can't easily check if threads are done without extra flags
        // For simplicity, we let threads self-terminate
    }

    size_t active_count() const {
        std::lock_guard<std::mutex> lock(mtx);
        return active_games.size();
    }

    void shutdown_all() {
        std::lock_guard<std::mutex> lock(mtx);
        for (auto& [game_id, thread] : active_games) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        active_games.clear();
    }

    std::vector<std::string> get_active_game_ids() const {
        std::lock_guard<std::mutex> lock(mtx);
        std::vector<std::string> ids;
        for (const auto& [game_id, _] : active_games) {
            ids.push_back(game_id);
        }
        return ids;
    }
};

// ============================================================================
// Main Bot
// ============================================================================
class LichessBot {
    LichessConfig config;
    std::unique_ptr<LichessClient> client;
    std::unique_ptr<GameManager> game_manager;
    ChallengeHandler challenge_handler;
    std::string our_username;

public:
    LichessBot(const LichessConfig& cfg)
        : config(cfg), challenge_handler(cfg) {}

    bool init() {
        client = std::make_unique<LichessClient>(config.token);

        // Get our username by fetching account info
        httplib::SSLClient account_client("lichess.org", 443);
        account_client.set_connection_timeout(10);
        httplib::Headers headers = {{"Authorization", "Bearer " + config.token}};
        auto res = account_client.Get("/api/account", headers);

        if (!res || res->status != 200) {
            log_error("Failed to get account info. Check your token.");
            return false;
        }

        try {
            json account = json::parse(res->body);
            our_username = account.value("username", "");
            if (our_username.empty()) {
                log_error("Could not determine username");
                return false;
            }
            log_msg("Logged in as: " + our_username);
        } catch (const json::exception& e) {
            log_error("Failed to parse account info: " + std::string(e.what()));
            return false;
        }

        game_manager =
            std::make_unique<GameManager>(config, our_username);

        return true;
    }

    void run() {
        log_msg("Starting event stream...");

        // Retry loop for connection
        int retry_delay = 1;
        const int max_retry_delay = 60;

        while (!g_shutdown.load()) {
            try {
                httplib::SSLClient stream_client("lichess.org", 443);
                stream_client.set_read_timeout(std::chrono::hours(24));  // Long timeout for streaming
                stream_client.set_connection_timeout(30);
                stream_client.set_keep_alive(true);

                httplib::Headers headers = {
                    {"Authorization", "Bearer " + config.token},
                    {"Accept", "application/x-ndjson"},
                    {"Connection", "keep-alive"}};

                log_msg("Connecting to event stream...");

                bool first_data = true;
                auto res = stream_client.Get(
                    "/api/stream/event", headers,
                    [&](const char* data, size_t len) -> bool {
                        if (g_shutdown.load()) {
                            return false;
                        }

                        if (first_data) {
                            log_msg("Event stream connected, receiving data...");
                            first_data = false;
                        }

                        // Reset retry delay on successful data
                        retry_delay = 1;

                        std::string chunk(data, len);
                        std::istringstream stream(chunk);
                        std::string line;

                        while (std::getline(stream, line)) {
                            if (line.empty())
                                continue;  // Keep-alive

                            try {
                                json event = json::parse(line);
                                handle_event(event);
                            } catch (const json::exception& e) {
                                log_error("JSON parse error: " +
                                          std::string(e.what()));
                            }
                        }

                        return !g_shutdown.load();
                    });

                if (!g_shutdown.load()) {
                    if (res) {
                        log_msg("Event stream disconnected (HTTP " +
                                std::to_string(res->status) + "), reconnecting...");
                    } else {
                        auto err = res.error();
                        log_msg("Event stream error: " + httplib::to_string(err) +
                                ", reconnecting...");
                    }
                }

            } catch (const std::exception& e) {
                log_error("Stream error: " + std::string(e.what()));
            } catch (...) {
                log_error("Stream error: unknown exception");
            }

            if (!g_shutdown.load()) {
                log_msg("Retrying in " + std::to_string(retry_delay) +
                        " seconds...");
                for (int i = 0; i < retry_delay && !g_shutdown.load(); ++i) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
                retry_delay = std::min(retry_delay * 2, max_retry_delay);
            }
        }

        log_msg("Shutting down...");
        game_manager->shutdown_all();
    }

    void handle_event(const json& event) {
        std::string type = event.value("type", "");

        if (type == "challenge") {
            handle_challenge(event["challenge"]);
        } else if (type == "challengeCanceled") {
            std::string id = event.value("/challenge/id"_json_pointer, "");
            log_msg("Challenge " + id + " was canceled");
        } else if (type == "challengeDeclined") {
            std::string id = event.value("/challenge/id"_json_pointer, "");
            log_msg("Challenge " + id + " was declined");
        } else if (type == "gameStart") {
            handle_game_start(event["game"]);
        } else if (type == "gameFinish") {
            handle_game_finish(event["game"]);
        }
    }

    void handle_challenge(const json& challenge) {
        std::string id = challenge.value("id", "");
        std::string challenger =
            challenge.value("/challenger/name"_json_pointer, "unknown");

        // Log challenge details
        std::string time_info = "unknown";
        if (challenge.contains("timeControl")) {
            auto& tc = challenge["timeControl"];
            std::string tc_type = tc.value("type", "unknown");
            if (tc_type == "clock") {
                int limit = tc.value("limit", 0);
                int inc = tc.value("increment", 0);
                time_info = std::to_string(limit) + "+" + std::to_string(inc) + "s";
            } else {
                time_info = tc_type;
            }
        }
        std::string variant = challenge.value("/variant/key"_json_pointer, "standard");
        bool rated = challenge.value("rated", false);

        log_msg("Received challenge from " + challenger + " (id: " + id + "): " +
                time_info + ", " + variant + ", " + (rated ? "rated" : "casual"));

        // Check if we can accept more games
        if (game_manager->active_count() >=
            static_cast<size_t>(config.max_games)) {
            log_msg("Declining challenge: too many active games");
            client->decline_challenge(id, "later");
            return;
        }

        if (challenge_handler.should_accept(challenge)) {
            log_msg("Accepting challenge from " + challenger);
            if (!client->accept_challenge(id)) {
                log_error("Failed to accept challenge " + id);
            }
        } else {
            std::string reason = challenge_handler.decline_reason(challenge);
            log_msg("Declining challenge from " + challenger + " (" + reason +
                    ")");
            client->decline_challenge(id, reason);
        }
    }

    void handle_game_start(const json& game) {
        std::string game_id = game.value("gameId", game.value("id", ""));
        if (game_id.empty()) {
            log_error("Game start event missing game ID");
            return;
        }

        log_msg("Game started: " + game_id);
        game_manager->start_game(game_id);
    }

    void handle_game_finish(const json& game) {
        std::string game_id = game.value("gameId", game.value("id", ""));
        if (!game_id.empty()) {
            log_msg("Game finished: " + game_id);
            game_manager->end_game(game_id);
        }
    }

    void seek_game() {
        if (!config.auto_seek) return;
        if (game_manager->active_count() >= static_cast<size_t>(config.max_games)) {
            return;
        }

        auto bots = client->get_online_bots(50);
        if (bots.empty()) {
            log_msg("No online bots found");
            return;
        }

        // Remove ourselves from the list
        bots.erase(std::remove(bots.begin(), bots.end(), our_username), bots.end());

        if (bots.empty()) {
            log_msg("No other bots online");
            return;
        }

        // Pick a random bot
        std::string target = bots[static_cast<size_t>(rand()) % bots.size()];
        log_msg("Challenging " + target + " to a game");

        if (!client->challenge_user(target, config.seek_time, config.seek_increment, true)) {
            log_msg("Challenge to " + target + " failed");
        }
    }
};

// ============================================================================
// Main
// ============================================================================
void print_usage(const char* prog) {
    std::cout
        << "Usage: " << prog << " --token <token> [options]\n"
        << "\nRequired:\n"
        << "  --token <token>      Lichess API token (or set LICHESS_TOKEN env)\n"
        << "\nOptions:\n"
        << "  --engine <path>      UCI engine path (default: ./build/cachemiss)\n"
        << "  --hash <mb>          Hash table size (default: 256)\n"
        << "  --min-time <sec>     Min initial time to accept (default: 60)\n"
        << "  --max-time <sec>     Max initial time to accept (default: 900)\n"
        << "  --min-inc <sec>      Min increment to accept (default: 0)\n"
        << "  --max-inc <sec>      Max increment to accept (default: 30)\n"
        << "  --rated              Accept only rated games\n"
        << "  --casual             Accept only casual games\n"
        << "  --seek               Auto-challenge online bots\n"
        << "  --seek-time <sec>    Seek time control (default: 180)\n"
        << "  --seek-inc <sec>     Seek increment (default: 2)\n"
        << "  --max-games <n>      Max concurrent games (default: 4)\n"
        << "  --log <file>         Log to file\n"
        << "  --quiet              Suppress console output\n"
        << "  --help               Show this help\n";
}

int main(int argc, char* argv[]) {
    LichessConfig config;

    // Check environment variable for token
    const char* env_token = getenv("LICHESS_TOKEN");
    if (env_token) {
        config.token = env_token;
    }

    static struct option long_options[] = {
        {"token", required_argument, nullptr, 't'},
        {"engine", required_argument, nullptr, 'e'},
        {"hash", required_argument, nullptr, 'H'},
        {"min-time", required_argument, nullptr, 'm'},
        {"max-time", required_argument, nullptr, 'M'},
        {"min-inc", required_argument, nullptr, 'i'},
        {"max-inc", required_argument, nullptr, 'I'},
        {"rated", no_argument, nullptr, 'r'},
        {"casual", no_argument, nullptr, 'c'},
        {"seek", no_argument, nullptr, 's'},
        {"seek-time", required_argument, nullptr, 'S'},
        {"seek-inc", required_argument, nullptr, 'N'},
        {"max-games", required_argument, nullptr, 'g'},
        {"log", required_argument, nullptr, 'l'},
        {"quiet", no_argument, nullptr, 'q'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};

    int opt;
    while ((opt = getopt_long(argc, argv, "t:e:H:m:M:i:I:rcsS:N:g:l:qh",
                              long_options, nullptr)) != -1) {
        switch (opt) {
        case 't':
            config.token = optarg;
            break;
        case 'e':
            config.engine_path = optarg;
            break;
        case 'H':
            config.hash_mb = std::stoi(optarg);
            break;
        case 'm':
            config.min_time_initial = std::stoi(optarg);
            break;
        case 'M':
            config.max_time_initial = std::stoi(optarg);
            break;
        case 'i':
            config.min_time_increment = std::stoi(optarg);
            break;
        case 'I':
            config.max_time_increment = std::stoi(optarg);
            break;
        case 'r':
            config.accept_rated = true;
            config.accept_casual = false;
            break;
        case 'c':
            config.accept_rated = false;
            config.accept_casual = true;
            break;
        case 's':
            config.auto_seek = true;
            break;
        case 'S':
            config.seek_time = std::stoi(optarg);
            break;
        case 'N':
            config.seek_increment = std::stoi(optarg);
            break;
        case 'g':
            config.max_games = std::stoi(optarg);
            break;
        case 'l':
            config.log_file = optarg;
            break;
        case 'q':
            quiet_mode = true;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    if (config.token.empty()) {
        std::cerr << "Error: Lichess API token required\n";
        std::cerr << "Use --token <token> or set LICHESS_TOKEN environment variable\n";
        return 1;
    }

    // Initialize logging
    if (!config.log_file.empty()) {
        log_init(config.log_file);
    }

    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);  // Ignore SIGPIPE to handle broken pipes gracefully

    log_msg("CacheMiss Lichess Bot starting...");
    log_msg("Engine: " + config.engine_path);
    log_msg("Hash: " + std::to_string(config.hash_mb) + " MB");
    log_msg("Time control: " + std::to_string(config.min_time_initial) + "-" +
            std::to_string(config.max_time_initial) + "s, inc " +
            std::to_string(config.min_time_increment) + "-" +
            std::to_string(config.max_time_increment) + "s");
    log_msg("Max concurrent games: " + std::to_string(config.max_games));

    LichessBot bot(config);

    if (!bot.init()) {
        log_error("Failed to initialize bot");
        log_close();
        return 1;
    }

    try {
        bot.run();
    } catch (const std::exception& e) {
        log_error("Fatal exception: " + std::string(e.what()));
    } catch (...) {
        log_error("Fatal unknown exception");
    }

    log_msg("Bot stopped");
    log_close();
    return 0;
}
