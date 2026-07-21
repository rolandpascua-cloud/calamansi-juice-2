#ifndef LEMON_DEPRECATED_ALIAS_H
#define LEMON_DEPRECATED_ALIAS_H

// Rebrand backward-compat shim.
//
// The Calamansi Juice 2 rebrand renamed the shipped binaries (see
// MERGING.md for the full mapping: lemond -> calamansid, lemonade ->
// calamansi, lemonade-tray -> calamansi-tray). Build- and install-time
// tooling still produces same-directory old-name aliases (symlinks on
// POSIX, copies on Windows) so anything that looks for the historical
// filename keeps finding a working binary. This header prints a single
// deprecation notice to stderr when a binary is invoked via one of those
// old-name aliases, then lets normal execution continue unchanged.
//
// Remove this shim (and the CMake POST_BUILD/install alias blocks that
// create the old-name files) once the aliases are retired — see the
// "Deprecation-alias policy" section of MERGING.md.

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <string>

namespace lemon {

// Extracts the filename component of argv[0], stripping a trailing
// ".exe" (case-insensitive) so Windows invocations match the same alias
// names as POSIX ones.
inline std::string deprecated_alias_basename(const char* argv0) {
    if (argv0 == nullptr) {
        return std::string();
    }
    std::string path(argv0);
    size_t slash = path.find_last_of("/\\");
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);

    const std::string exe_suffix = ".exe";
    if (base.size() >= exe_suffix.size()) {
        std::string tail = base.substr(base.size() - exe_suffix.size());
        std::transform(tail.begin(), tail.end(), tail.begin(),
                        [](unsigned char c) { return std::tolower(c); });
        if (tail == exe_suffix) {
            base = base.substr(0, base.size() - exe_suffix.size());
        }
    }
    return base;
}

// If argv0's basename matches one of old_names, prints a one-line
// deprecation warning to stderr naming new_name as the replacement.
// Does not alter stdout, exit codes, or any other behavior.
inline void warn_if_deprecated_alias(const char* argv0, const std::string& new_name,
                                      std::initializer_list<const char*> old_names) {
    const std::string base = deprecated_alias_basename(argv0);
    if (base.empty()) {
        return;
    }
    for (const char* old_name : old_names) {
        if (base == old_name) {
            std::cerr << "warning: '" << old_name << "' is a deprecated alias for '" << new_name
                      << "' and will be removed in a future release." << std::endl;
            return;
        }
    }
}

}  // namespace lemon

#endif  // LEMON_DEPRECATED_ALIAS_H
