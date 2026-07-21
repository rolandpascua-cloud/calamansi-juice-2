#include "lemon/process_probe.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_map>

#ifdef __linux__
#include <filesystem>
#include <unistd.h>
namespace fs = std::filesystem;
#endif

namespace lemon {

namespace {

#ifdef __linux__

std::string to_lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
}

std::string read_comm(int pid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/comm");
    std::string line;
    if (f.is_open()) std::getline(f, line);
    return line;
}

std::string read_exe(int pid) {
    std::error_code ec;
    fs::path target = fs::read_symlink("/proc/" + std::to_string(pid) + "/exe", ec);
    return ec ? "" : target.string();
}

// /proc/[pid]/cmdline is NUL-separated argv, terminated by a final NUL.
std::vector<std::string> read_cmdline_args(int pid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
    if (!f.is_open()) return {};
    std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<std::string> args;
    std::string current;
    for (char c : raw) {
        if (c == '\0') {
            if (!current.empty()) args.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty()) args.push_back(current);
    return args;
}

std::string join(const std::vector<std::string>& parts, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

// Hex-string local port from a /proc/net/tcp{,6} line's "local_address"
// field ("IP:PORT" in hex), keyed by the socket inode (last-but-a-few
// column). Only LISTEN-state (st == "0A") entries are kept.
std::unordered_map<std::string, int> listening_ports_by_inode(const std::string& proc_net_path) {
    std::unordered_map<std::string, int> result;
    std::ifstream f(proc_net_path);
    if (!f.is_open()) return result;

    std::string line;
    std::getline(f, line); // header
    while (std::getline(f, line)) {
        std::istringstream iss(line);
        std::string sl, local_address, rem_address, st;
        iss >> sl >> local_address >> rem_address >> st;
        if (st != "0A") continue; // TCP_LISTEN

        // Skip tx_queue:rx_queue, tr:tm_when, retrnsmt, uid, timeout, then inode.
        std::string skip;
        for (int i = 0; i < 4; ++i) iss >> skip;
        std::string inode;
        iss >> inode;
        if (inode.empty()) continue;

        size_t colon = local_address.find(':');
        if (colon == std::string::npos) continue;
        std::string port_hex = local_address.substr(colon + 1);
        int port = 0;
        try {
            port = std::stoi(port_hex, nullptr, 16);
        } catch (...) {
            continue;
        }
        result[inode] = port;
    }
    return result;
}

// Socket inodes this pid currently has open, via /proc/[pid]/fd/* ->
// "socket:[NNNN]" symlink targets. Empty (not an error) if fd/ isn't
// readable - typically because the process is owned by a different user.
std::vector<std::string> socket_inodes_for_pid(int pid) {
    std::vector<std::string> inodes;
    std::error_code ec;
    fs::path fd_dir = "/proc/" + std::to_string(pid) + "/fd";
    if (!fs::exists(fd_dir, ec)) return inodes;

    for (const auto& entry : fs::directory_iterator(fd_dir, ec)) {
        if (ec) break;
        std::error_code link_ec;
        fs::path target = fs::read_symlink(entry.path(), link_ec);
        if (link_ec) continue;
        std::string target_str = target.string();
        if (target_str.rfind("socket:[", 0) == 0) {
            inodes.push_back(target_str.substr(8, target_str.size() - 9));
        }
    }
    return inodes;
}

#endif // __linux__

} // namespace

std::vector<ProcessMatch> find_processes(const std::vector<std::string>& predicates) {
    std::vector<ProcessMatch> matches;
#ifndef __linux__
    (void)predicates;
    return matches;
#else
    std::vector<std::string> lower_predicates;
    for (const auto& p : predicates) lower_predicates.push_back(to_lower(p));

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("/proc", ec)) {
        if (ec) break;
        const std::string filename = entry.path().filename().string();
        if (filename.empty() || !std::all_of(filename.begin(), filename.end(), ::isdigit)) continue;
        int pid = std::stoi(filename);

        ProcessMatch match;
        match.pid = pid;
        match.name = read_comm(pid);
        match.exe = read_exe(pid);
        std::vector<std::string> args = read_cmdline_args(pid);
        match.cmdline = join(args, " ");

        std::string haystack = to_lower(match.name + " " + match.exe + " " + match.cmdline);
        bool matched = std::any_of(lower_predicates.begin(), lower_predicates.end(),
                                    [&](const std::string& pred) { return haystack.find(pred) != std::string::npos; });
        if (matched) {
            matches.push_back(std::move(match));
        }
    }
    return matches;
#endif
}

PortResolution resolve_port(const ProcessMatch& proc) {
    PortResolution result;
#ifndef __linux__
    (void)proc;
    return result;
#else
    // Socket introspection: cross-reference this pid's open socket inodes
    // against every LISTEN-state TCP socket on the box.
    std::vector<std::string> inodes = socket_inodes_for_pid(proc.pid);
    if (!inodes.empty()) {
        auto tcp4 = listening_ports_by_inode("/proc/net/tcp");
        auto tcp6 = listening_ports_by_inode("/proc/net/tcp6");
        std::vector<int> found_ports;
        for (const auto& inode : inodes) {
            auto it4 = tcp4.find(inode);
            if (it4 != tcp4.end()) found_ports.push_back(it4->second);
            auto it6 = tcp6.find(inode);
            if (it6 != tcp6.end()) found_ports.push_back(it6->second);
        }
        if (!found_ports.empty()) {
            std::sort(found_ports.begin(), found_ports.end());
            found_ports.erase(std::unique(found_ports.begin(), found_ports.end()), found_ports.end());
            result.found = true;
            result.ports = found_ports;
            result.source = "socket";
            return result;
        }
    }

    // Fallback: read the process's own argv for an explicit --port/-p flag.
    // This is runtime evidence from the process itself, not a guess - used
    // only when socket introspection is denied (e.g. a different system
    // user owns the process, so /proc/[pid]/fd isn't readable).
    std::vector<std::string> args = read_cmdline_args(proc.pid);
    for (size_t i = 0; i < args.size(); ++i) {
        if ((args[i] == "--port" || args[i] == "-p") && i + 1 < args.size()) {
            try {
                result.ports = {std::stoi(args[i + 1])};
                result.found = true;
                result.source = "cmdline (socket introspection denied)";
                return result;
            } catch (...) {
                continue;
            }
        }
        const std::string prefix = "--port=";
        if (args[i].rfind(prefix, 0) == 0) {
            try {
                result.ports = {std::stoi(args[i].substr(prefix.size()))};
                result.found = true;
                result.source = "cmdline (socket introspection denied)";
                return result;
            } catch (...) {
                continue;
            }
        }
    }
    return result;
#endif
}

} // namespace lemon
