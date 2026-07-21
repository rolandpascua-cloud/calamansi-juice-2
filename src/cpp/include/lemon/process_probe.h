#pragma once

// Calamansi Juice 2 addition: runtime process/port discovery for the
// Resource Dashboard's "other local inference services" section
// (resource_dashboard.cpp). No hardcoded ports - a target process's
// listening port is discovered via /proc/net/tcp{,6} + /proc/[pid]/fd/*
// socket-inode cross-referencing (the same technique psutil uses
// internally on Linux), falling back to parsing the process's own argv for
// an explicit --port/-p flag when socket introspection is denied (e.g. the
// process is owned by a different system user).

#include <optional>
#include <string>
#include <vector>

namespace lemon {

struct ProcessMatch {
    int pid = 0;
    std::string name;      // /proc/[pid]/comm
    std::string exe;       // realpath of /proc/[pid]/exe, if readable
    std::string cmdline;   // space-joined /proc/[pid]/cmdline
};

// Scans /proc/[0-9]+ for processes whose name/exe/cmdline contains any of
// `predicates` (case-insensitive substring match). Linux-only; returns an
// empty vector on other platforms (this feature is Strix Halo/Linux
// specific - see the Resource Dashboard's own platform note).
std::vector<ProcessMatch> find_processes(const std::vector<std::string>& predicates);

struct PortResolution {
    bool found = false;
    std::vector<int> ports;  // usually one; a process can listen on several
    std::string source;      // "socket" or "cmdline (socket introspection denied)"
};

// Socket introspection first (cross-references LISTEN sockets in
// /proc/net/tcp{,6} against the pid's open file descriptors); falls back to
// parsing --port/-p off the process's own argv if that's denied. Never
// throws; returns found=false with an empty source if neither works.
PortResolution resolve_port(const ProcessMatch& proc);

} // namespace lemon
