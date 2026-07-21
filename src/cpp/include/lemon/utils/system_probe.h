#pragma once

// Calamansi Juice 2 addition: small shared best-effort system-probing
// primitives (kernel cmdline parsing, PATH tool lookup, short-timeout
// subprocess capture), used by both system_state.cpp (System State Viewer)
// and resource_dashboard.cpp (Resource Dashboard) so the two features don't
// each re-derive the same /proc/cmdline and PATH-search logic.

#include "lemon/utils/process_manager.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace lemon {
namespace utils {

inline std::string probe_trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

inline std::optional<std::string> probe_read_file_trimmed(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string content = probe_trim(ss.str());
    if (content.empty()) return std::nullopt;
    return content;
}

// Parses whitespace-separated "key" or "key=value" tokens from a kernel
// cmdline-style string (e.g. the contents of /proc/cmdline).
inline std::unordered_map<std::string, std::string> probe_parse_cmdline_tokens(const std::string& cmdline) {
    std::unordered_map<std::string, std::string> result;
    std::istringstream iss(cmdline);
    std::string token;
    while (iss >> token) {
        size_t eq = token.find('=');
        if (eq == std::string::npos) {
            result[token] = "";
        } else {
            result[token.substr(0, eq)] = token.substr(eq + 1);
        }
    }
    return result;
}

// PATH search rather than shelling out just to find out a tool doesn't exist.
inline bool probe_is_tool_installed(const std::string& name) {
    const char* path_env = std::getenv("PATH");
    if (!path_env) return false;
#ifdef _WIN32
    const char path_sep = ';';
#else
    const char path_sep = ':';
#endif
    std::stringstream ss(path_env);
    std::string dir;
    while (std::getline(ss, dir, path_sep)) {
        if (dir.empty()) continue;
        std::error_code ec;
#ifdef _WIN32
        std::filesystem::path candidate = std::filesystem::path(dir) / (name + ".exe");
        if (std::filesystem::exists(candidate, ec)) {
            return true;
        }
#else
        std::filesystem::path candidate = std::filesystem::path(dir) / name;
        if (std::filesystem::exists(candidate, ec) && access(candidate.c_str(), X_OK) == 0) {
            return true;
        }
#endif
    }
    return false;
}

// Runs an already-PATH-verified tool with a short timeout and returns its
// combined output, or std::nullopt if it exited non-zero / timed out.
inline std::optional<std::string> probe_run_tool(const std::string& command_line, int timeout_seconds = 2) {
    std::string output;
    int rc = ProcessManager::run_command(command_line, output, timeout_seconds);
    if (rc != 0) {
        return std::nullopt;
    }
    return output;
}

} // namespace utils
} // namespace lemon
