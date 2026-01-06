// WAC Comparison Tool
// Runs bench-wac on multiple builds and generates an HTML comparison table
//
// Usage: ./wac_compare <builds_dir> <wac_file> [movetime_ms] [output.html]
// Default: movetime=1000, output=wac_comparison.html

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct PositionResult {
    std::string id;
    std::string found_move;
    std::string expected_move;  // empty if passed
    int depth = 0;
    bool passed = false;
};

struct BuildResult {
    std::string build_name;
    std::vector<PositionResult> results;
    int total_passed = 0;
    int total_failed = 0;
};

// Execute command and capture stdout
std::string exec_cmd(const std::string& cmd) {
    std::array<char, 4096> buffer;
    std::string result;

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return "";
    }

    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }

    pclose(pipe);
    return result;
}

// Check if executable supports --bench-wac (double dash) or -bench-wac (single dash)
bool detect_double_dash(const fs::path& exe_path) {
    // Try --help and look for --bench-wac
    std::string cmd = exe_path.string() + " --help 2>&1";
    std::string output = exec_cmd(cmd);

    if (output.find("--bench-wac") != std::string::npos) {
        return true;
    }
    if (output.find("-bench-wac") != std::string::npos) {
        return false;
    }

    // Default to double dash (modern)
    return true;
}

// Run bench-wac on a single executable
BuildResult run_bench_wac(const fs::path& exe_path, const std::string& wac_file, int movetime_ms) {
    BuildResult result;
    result.build_name = exe_path.filename().string();

    bool use_double_dash = detect_double_dash(exe_path);
    std::string dash = use_double_dash ? "--" : "-";

    std::ostringstream cmd;
    cmd << exe_path.string() << " " << dash << "bench-wac " << wac_file << " " << movetime_ms << " 2>&1";

    std::cout << "Running: " << result.build_name << " (" << dash << "bench-wac)" << std::endl;

    std::string output = exec_cmd(cmd.str());

    // Parse output line by line
    // Position header: [N/total] WAC.XXX: info depth...
    // Success result:  <move> (depth D) OK
    // Failure result:  <move> (expected <expected>, depth D) FAIL
    std::regex pos_regex(R"(\[\d+/\d+\] (WAC\.\d+):)");
    std::regex ok_regex(R"(^(\S+) \(depth (\d+)\) OK)");
    std::regex fail_regex(R"(^(\S+) \(expected ([^,]+), depth (\d+)\) FAIL)");

    std::istringstream iss(output);
    std::string line;
    std::string current_id;

    while (std::getline(iss, line)) {
        std::smatch match;

        // Check for position header
        if (std::regex_search(line, match, pos_regex)) {
            current_id = match[1].str();
        }

        // Check for OK result
        if (std::regex_search(line, match, ok_regex)) {
            PositionResult pos;
            pos.id = current_id;
            pos.found_move = match[1].str();
            pos.depth = std::stoi(match[2].str());
            pos.passed = true;
            result.results.push_back(pos);
            result.total_passed++;
        }
        // Check for FAIL result
        else if (std::regex_search(line, match, fail_regex)) {
            PositionResult pos;
            pos.id = current_id;
            pos.found_move = match[1].str();
            pos.expected_move = match[2].str();
            pos.depth = std::stoi(match[3].str());
            pos.passed = false;
            result.results.push_back(pos);
            result.total_failed++;
        }
    }

    std::cout << "  " << result.build_name << ": " << result.total_passed << "/"
              << (result.total_passed + result.total_failed) << " passed" << std::endl;

    return result;
}

// Generate HTML comparison table
void generate_html(const std::vector<BuildResult>& all_results, const std::string& output_file) {
    std::ofstream out(output_file);
    if (!out) {
        std::cerr << "Failed to open output file: " << output_file << std::endl;
        return;
    }

    // Collect all position IDs (they should be the same across builds)
    std::vector<std::string> position_ids;
    if (!all_results.empty()) {
        for (const auto& pos : all_results[0].results) {
            position_ids.push_back(pos.id);
        }
    }

    // Create lookup maps for each build
    std::vector<std::map<std::string, PositionResult>> build_maps;
    for (const auto& build : all_results) {
        std::map<std::string, PositionResult> pos_map;
        for (const auto& pos : build.results) {
            pos_map[pos.id] = pos;
        }
        build_maps.push_back(pos_map);
    }

    // Write HTML
    out << R"(<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<title>WAC Comparison</title>
<style>
body { font-family: monospace; margin: 20px; }
table { border-collapse: collapse; width: 100%; }
th, td { border: 1px solid #ccc; padding: 6px 10px; text-align: left; }
th { background: #f0f0f0; position: sticky; top: 0; }
.pass { background: #d4edda; }
.fail { background: #f8d7da; }
.summary { font-weight: bold; background: #e9ecef; }
.depth { color: #666; font-size: 0.9em; }
</style>
</head>
<body>
<h1>WAC Comparison</h1>
<table>
<tr>
<th>Position</th>
)";

    // Header row with build names
    for (const auto& build : all_results) {
        out << "<th>" << build.build_name << "</th>\n";
    }
    out << "</tr>\n";

    // Data rows
    for (const auto& pos_id : position_ids) {
        out << "<tr>\n<td>" << pos_id << "</td>\n";

        for (size_t i = 0; i < all_results.size(); i++) {
            auto it = build_maps[i].find(pos_id);
            if (it != build_maps[i].end()) {
                const auto& pos = it->second;
                std::string css_class = pos.passed ? "pass" : "fail";
                std::string symbol = pos.passed ? "&#10003;" : "&#10007;";  // checkmark or X

                out << "<td class=\"" << css_class << "\">"
                    << symbol << " <span class=\"depth\">d" << pos.depth << "</span> "
                    << pos.found_move;

                if (!pos.passed && !pos.expected_move.empty()) {
                    out << " <small>(want " << pos.expected_move << ")</small>";
                }
                out << "</td>\n";
            } else {
                out << "<td>-</td>\n";
            }
        }
        out << "</tr>\n";
    }

    // Summary row
    out << "<tr class=\"summary\">\n<td>Total</td>\n";
    for (const auto& build : all_results) {
        int total = build.total_passed + build.total_failed;
        double pct = total > 0 ? 100.0 * build.total_passed / total : 0.0;
        out << "<td>" << build.total_passed << "/" << total
            << " (" << std::fixed << std::setprecision(1) << pct << "%)</td>\n";
    }
    out << "</tr>\n";

    out << R"(</table>
</body>
</html>
)";

    out.close();
    std::cout << "Generated: " << output_file << std::endl;
}

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " <builds_dir> <wac_file> [movetime_ms] [output.html]\n";
    std::cerr << "  builds_dir   - Directory containing engine executables\n";
    std::cerr << "  wac_file     - WAC test suite EPD file\n";
    std::cerr << "  movetime_ms  - Time per position in ms (default: 1000)\n";
    std::cerr << "  output.html  - Output HTML file (default: wac_comparison.html)\n";
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    std::string builds_dir = argv[1];
    std::string wac_file = argv[2];
    int movetime_ms = (argc > 3) ? std::stoi(argv[3]) : 1000;
    std::string output_file = (argc > 4) ? argv[4] : "wac_comparison.html";

    // Verify inputs
    if (!fs::is_directory(builds_dir)) {
        std::cerr << "Error: builds_dir is not a directory: " << builds_dir << std::endl;
        return 1;
    }
    if (!fs::exists(wac_file)) {
        std::cerr << "Error: wac_file does not exist: " << wac_file << std::endl;
        return 1;
    }

    // Collect executables
    std::vector<fs::path> executables;
    for (const auto& entry : fs::directory_iterator(builds_dir)) {
        if (entry.is_regular_file()) {
            auto perms = entry.status().permissions();
            if ((perms & fs::perms::owner_exec) != fs::perms::none) {
                executables.push_back(entry.path());
            }
        }
    }

    if (executables.empty()) {
        std::cerr << "No executables found in: " << builds_dir << std::endl;
        return 1;
    }

    // Sort executables by name for consistent ordering
    std::sort(executables.begin(), executables.end());

    std::cout << "Found " << executables.size() << " executables\n";
    std::cout << "WAC file: " << wac_file << "\n";
    std::cout << "Movetime: " << movetime_ms << " ms\n";
    std::cout << "Output: " << output_file << "\n\n";

    // Run bench-wac in parallel
    std::vector<std::future<BuildResult>> futures;
    for (const auto& exe : executables) {
        futures.push_back(std::async(std::launch::async, run_bench_wac, exe, wac_file, movetime_ms));
    }

    // Collect results
    std::vector<BuildResult> all_results;
    for (auto& f : futures) {
        all_results.push_back(f.get());
    }

    // Sort results by build name for consistent table ordering
    std::sort(all_results.begin(), all_results.end(),
              [](const BuildResult& a, const BuildResult& b) {
                  return a.build_name < b.build_name;
              });

    // Generate HTML
    generate_html(all_results, output_file);

    return 0;
}
