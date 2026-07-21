#include "lemon/system_state.h"

#include "lemon/system_info.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/utils/process_manager.h"
#include "lemon/version.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <unordered_map>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace lemon {

namespace {

// --- small shared helpers ---------------------------------------------------

json unavailable(const std::string& reason) {
    return json{{"available", false}, {"reason", reason}};
}

json available(const json& value) {
    json j = {{"available", true}};
    j.update(value.is_object() ? value : json{{"value", value}});
    return j;
}

// PATH search rather than shelling out just to find out a tool doesn't exist
// - consistent with this codebase's existing preference for direct
// filesystem/sysfs checks over subprocess calls where possible (see
// platform/metrics_linux.cpp).
bool is_tool_installed(const std::string& name) {
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
        fs::path candidate = fs::path(dir) / (name + ".exe");
        if (fs::exists(candidate, ec)) {
            return true;
        }
#else
        fs::path candidate = fs::path(dir) / name;
        if (fs::exists(candidate, ec) && access(candidate.c_str(), X_OK) == 0) {
            return true;
        }
#endif
    }
    return false;
}

// Runs an already-PATH-verified tool with a short timeout and returns its
// combined output, or std::nullopt if it exited non-zero / timed out.
std::optional<std::string> run_tool(const std::string& command_line, int timeout_seconds = 3) {
    std::string output;
    int rc = utils::ProcessManager::run_command(command_line, output, timeout_seconds);
    if (rc != 0) {
        return std::nullopt;
    }
    return output;
}

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::optional<std::string> read_file_trimmed(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string content = trim(ss.str());
    if (content.empty()) return std::nullopt;
    return content;
}

// Parses /etc/os-release-style "KEY=value" lines (value optionally quoted).
std::unordered_map<std::string, std::string> parse_env_style_file(const std::string& path) {
    std::unordered_map<std::string, std::string> result;
    std::ifstream f(path);
    if (!f.is_open()) return result;
    std::string line;
    while (std::getline(f, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
            val = val.substr(1, val.size() - 2);
        }
        result[key] = val;
    }
    return result;
}

// Parses whitespace-separated "key" or "key=value" tokens from a kernel
// cmdline-style string (used for both /proc/cmdline).
std::unordered_map<std::string, std::string> parse_cmdline_tokens(const std::string& cmdline) {
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

// --- section builders --------------------------------------------------------

json snapshot_hardware() {
    json section;

    // CPU / AMD GPU / NVIDIA GPU / NPU: reuse SystemInfo::get_device_dict(),
    // which already does per-device try/catch fault tolerance and family
    // detection (gfx1151-style arch strings via identify_rocm_arch_from_name)
    // - no reason to re-derive this from scratch.
    json devices;
    try {
        auto sys_info = create_system_info();
        devices = sys_info->get_device_dict();
    } catch (const std::exception& e) {
        section["error"] = std::string("Hardware detection failed: ") + e.what();
        devices = json::object();
    }

    section["cpu"] = devices.value("cpu", json::object());

    // Prefer the integrated GPU entry (Strix Halo's gfx1151 is an iGPU) but
    // fall back to the first discrete AMD GPU if no iGPU was detected.
    json gpu = json::object();
    if (devices.contains("amd_gpu") && devices["amd_gpu"].is_array() && !devices["amd_gpu"].empty()) {
        gpu = devices["amd_gpu"][0];
        for (const auto& g : devices["amd_gpu"]) {
            if (g.value("integrated", false)) {
                gpu = g;
                break;
            }
        }
    } else if (devices.contains("nvidia_gpu") && devices["nvidia_gpu"].is_array() &&
               !devices["nvidia_gpu"].empty()) {
        gpu = devices["nvidia_gpu"][0];
    }
    section["gpu"] = gpu.empty() ? unavailable("No GPU detected") : gpu;

    section["npu"] = devices.value("amd_npu", unavailable("NPU detection unavailable"));

    // Total system RAM.
    try {
        auto sys_info = create_system_info();
        json info = sys_info->get_system_info_dict();
        if (info.contains("Physical Memory")) {
            section["memory"] = available({{"total", info["Physical Memory"]}});
        } else {
            section["memory"] = unavailable("Could not read total memory");
        }
    } catch (const std::exception& e) {
        section["memory"] = unavailable(std::string("Detection exception: ") + e.what());
    }

    // UMA config: on Strix Halo-class unified-memory hardware, "how much
    // VRAM" is a kernel-parameter question, not a fixed number. Surface
    // which of amdgpu.gttsize / ttm.pages_limit is actually active (they're
    // mutually exclusive ways of sizing the GTT/carve-out) rather than a
    // flat VRAM figure. Linux-only (kernel cmdline concept doesn't apply on
    // Windows/macOS).
#ifndef __linux__
    section["uma"] = unavailable("not applicable on this platform");
#else
    try {
        auto cmdline = read_file_trimmed("/proc/cmdline");
        if (!cmdline) {
            section["uma"] = unavailable("Could not read /proc/cmdline");
        } else {
            auto tokens = parse_cmdline_tokens(*cmdline);
            if (tokens.count("amdgpu.gttsize")) {
                section["uma"] = available({{"active_param", "amdgpu.gttsize"},
                                            {"value", tokens["amdgpu.gttsize"]}});
            } else if (tokens.count("ttm.pages_limit")) {
                section["uma"] = available({{"active_param", "ttm.pages_limit"},
                                            {"value", tokens["ttm.pages_limit"]}});
            } else {
                section["uma"] = available({{"active_param", nullptr},
                                            {"note", "module defaults apply (neither amdgpu.gttsize nor "
                                                     "ttm.pages_limit set on the kernel cmdline)"}});
            }
        }
    } catch (const std::exception& e) {
        section["uma"] = unavailable(std::string("Detection exception: ") + e.what());
    }
#endif // __linux__

    return section;
}

#ifdef __linux__
json snapshot_os_kernel_linux() {
    json section;

    try {
        auto release = parse_env_style_file("/etc/os-release");
        if (release.empty()) {
            section["distro"] = unavailable("/etc/os-release not found");
        } else {
            json distro = {{"available", true}};
            if (release.count("NAME")) distro["name"] = release["NAME"];
            if (release.count("VERSION_ID")) distro["version_id"] = release["VERSION_ID"];
            if (release.count("PRETTY_NAME")) distro["pretty_name"] = release["PRETTY_NAME"];
            section["distro"] = distro;
        }
    } catch (const std::exception& e) {
        section["distro"] = unavailable(std::string("Detection exception: ") + e.what());
    }

    try {
        auto version = read_file_trimmed("/proc/version");
        if (!version) {
            section["kernel_version"] = unavailable("/proc/version not found");
        } else {
            const std::string tag = "version ";
            size_t pos = version->find(tag);
            std::string kernel = "unknown";
            if (pos != std::string::npos) {
                pos += tag.size();
                size_t end = version->find(' ', pos);
                kernel = version->substr(pos, end - pos);
            }
            section["kernel_version"] = available(kernel);
        }
    } catch (const std::exception& e) {
        section["kernel_version"] = unavailable(std::string("Detection exception: ") + e.what());
    }

    // Boot mode: detect rather than assume, since this varies distro to
    // distro (e.g. Fedora defaults to systemd-boot/BLS on some spins,
    // GRUB on others).
    try {
        std::error_code ec;
        bool uefi = fs::exists("/sys/firmware/efi", ec);
        bool systemd_boot_entries =
            fs::exists("/boot/loader/entries", ec) || fs::exists("/boot/efi/loader/entries", ec);
        bool grub_present = fs::exists("/boot/grub", ec) || fs::exists("/boot/grub2", ec) ||
                            fs::exists("/etc/default/grub", ec);

        std::string mode;
        if (systemd_boot_entries) {
            mode = "systemd-boot (BLS)";
        } else if (uefi && grub_present) {
            mode = "GRUB (UEFI)";
        } else if (grub_present) {
            mode = "GRUB (legacy BIOS)";
        } else if (uefi) {
            mode = "UEFI (bootloader undetected)";
        } else {
            mode = "unknown";
        }
        section["boot_mode"] = available(mode);
    } catch (const std::exception& e) {
        section["boot_mode"] = unavailable(std::string("Detection exception: ") + e.what());
    }

    // amd_iommu cmdline state - flagged if "off", since that disables the NPU.
    try {
        auto cmdline = read_file_trimmed("/proc/cmdline");
        if (!cmdline) {
            section["amd_iommu"] = unavailable("Could not read /proc/cmdline");
        } else {
            auto tokens = parse_cmdline_tokens(*cmdline);
            if (tokens.count("amd_iommu")) {
                const std::string& value = tokens["amd_iommu"];
                section["amd_iommu"] =
                    available({{"state", value}, {"disables_npu", value == "off"}});
            } else {
                section["amd_iommu"] = available({{"state", "not set (kernel default)"}, {"disables_npu", false}});
            }
        }
    } catch (const std::exception& e) {
        section["amd_iommu"] = unavailable(std::string("Detection exception: ") + e.what());
    }

    return section;
}
#endif // __linux__

json snapshot_os_kernel() {
#ifdef __linux__
    return snapshot_os_kernel_linux();
#else
    json section;
    const char* reason = "not applicable on this platform";
    section["distro"] = unavailable(reason);
    section["kernel_version"] = unavailable(reason);
    section["boot_mode"] = unavailable(reason);
    section["amd_iommu"] = unavailable(reason);
    return section;
#endif
}

json snapshot_firmware() {
    json section;

    // BIOS/UEFI version+date via dmidecode. Needs root to read /dev/mem on
    // most distros - detect that distinctly from "not installed" rather
    // than collapsing both into one generic failure.
    try {
        if (!is_tool_installed("dmidecode")) {
            section["bios"] = unavailable("dmidecode not installed");
        } else {
            std::string output;
            int rc = utils::ProcessManager::run_command("dmidecode -t bios 2>&1", output, 3);
            bool permission_denied =
                output.find("Permission denied") != std::string::npos ||
                output.find("/dev/mem: No such file") != std::string::npos;
            if (rc != 0 || permission_denied) {
#ifndef _WIN32
                bool likely_permission_issue = permission_denied || geteuid() != 0;
#else
                bool likely_permission_issue = permission_denied;
#endif
                section["bios"] = unavailable(likely_permission_issue
                                                   ? "unavailable - requires root"
                                                   : "dmidecode failed (no SMBIOS table access)");
            } else {
                std::string version, date;
                std::istringstream iss(output);
                std::string line;
                while (std::getline(iss, line)) {
                    std::string trimmed = trim(line);
                    if (trimmed.rfind("Version:", 0) == 0) {
                        version = trim(trimmed.substr(8));
                    } else if (trimmed.rfind("Release Date:", 0) == 0) {
                        date = trim(trimmed.substr(13));
                    }
                }
                if (version.empty() && date.empty()) {
                    section["bios"] = unavailable("dmidecode returned no BIOS fields");
                } else {
                    section["bios"] = available({{"version", version}, {"date", date}});
                }
            }
        }
    } catch (const std::exception& e) {
        section["bios"] = unavailable(std::string("Detection exception: ") + e.what());
    }

    // GPU VBIOS/IFWI version via rocm-smi/amd-smi, whichever is installed.
    // Note: amd-smi's "-v/--vram" flag is VRAM info, not VBIOS - the video
    // BIOS/IFWI flag is "-I/--ifwi" (confirmed against a real `amd-smi
    // static --help` on a Strix Halo dev box; easy to get backwards since
    // "-v" reads like "version").
    try {
        if (is_tool_installed("amd-smi")) {
            auto out = run_tool("amd-smi static -I --json 2>&1", 3);
            if (!out) {
                section["gpu_vbios"] = unavailable("amd-smi query failed");
            } else {
                std::string version;
                try {
                    json parsed = json::parse(*out);
                    if (parsed.contains("gpu_data") && parsed["gpu_data"].is_array() &&
                        !parsed["gpu_data"].empty() && parsed["gpu_data"][0].contains("ifwi") &&
                        parsed["gpu_data"][0]["ifwi"].contains("version")) {
                        version = parsed["gpu_data"][0]["ifwi"]["version"].get<std::string>();
                    }
                } catch (const json::exception&) {
                    // Fall through to unavailable below - unparseable output
                    // from an unexpected amd-smi version.
                }
                section["gpu_vbios"] =
                    version.empty() ? unavailable("amd-smi returned no IFWI/VBIOS version field")
                                     : available(version);
            }
        } else if (is_tool_installed("rocm-smi")) {
            auto out = run_tool("rocm-smi --showvbios 2>&1", 3);
            std::string version;
            if (out) {
                std::istringstream iss(*out);
                std::string line;
                while (std::getline(iss, line)) {
                    auto pos = line.find("VBIOS version:");
                    if (pos != std::string::npos) {
                        version = trim(line.substr(pos + std::string("VBIOS version:").size()));
                        break;
                    }
                }
            }
            section["gpu_vbios"] = version.empty() ? unavailable("rocm-smi returned no VBIOS version")
                                                     : available(version);
        } else {
            section["gpu_vbios"] = unavailable("rocm-smi/amd-smi not installed");
        }
    } catch (const std::exception& e) {
        section["gpu_vbios"] = unavailable(std::string("Detection exception: ") + e.what());
    }

    return section;
}

// Best-effort ROCm version: prefer the plain version file ROCm installs
// ship (no subprocess needed), fall back to `rocm-smi --version`.
json detect_rocm_version() {
    if (auto v = read_file_trimmed("/opt/rocm/.info/version")) {
        return available(*v);
    }
    if (is_tool_installed("rocm-smi")) {
        if (auto out = run_tool("rocm-smi --version 2>&1", 3)) {
            return available(trim(*out));
        }
    }
    return unavailable("ROCm not detected (no /opt/rocm/.info/version, rocm-smi not installed)");
}

json detect_hip_version() {
    if (auto v = read_file_trimmed("/opt/rocm/.info/version-hip-libraries")) {
        return available(*v);
    }
    if (is_tool_installed("hipconfig")) {
        if (auto out = run_tool("hipconfig --version 2>&1", 3)) {
            return available(trim(*out));
        }
    }
    return unavailable("HIP runtime not detected");
}

json detect_mesa_version() {
    if (!is_tool_installed("glxinfo")) {
        return unavailable("glxinfo not installed");
    }
    auto out = run_tool("glxinfo -B 2>&1", 3);
    if (!out) {
        return unavailable("glxinfo failed (no display/X11 available in this session)");
    }
    std::istringstream iss(*out);
    std::string line;
    while (std::getline(iss, line)) {
        auto pos = line.find("Mesa ");
        if (pos != std::string::npos) {
            return available(trim(line.substr(pos)));
        }
    }
    return unavailable("Mesa version string not found in glxinfo output");
}

json detect_python_version() {
    if (!is_tool_installed("python3")) {
        return unavailable("python3 not installed");
    }
    // Older Python prints --version to stderr; capture both.
    if (auto out = run_tool("python3 --version 2>&1", 3)) {
        return available(trim(*out));
    }
    return unavailable("python3 --version failed");
}

// Backend versions already tracked by the recipes tree (get_recipe_version()
// / per-backend resolve_version() overrides in system_info.cpp) - reused
// rather than re-implementing --version parsing here. Best-effort: only
// llamacpp and fastflowlm currently override resolve_version() for a live
// CLI query when no version.txt exists; vllm/openmoss/trellis fall back to
// whatever version.txt (if any) already records.
json detect_backend_versions() {
    json backends = json::object();
    try {
        json system_info = SystemInfoCache::get_system_info_with_cache();
        if (!system_info.contains("recipes") || !system_info["recipes"].is_object()) {
            return unavailable("Recipe/backend registry unavailable");
        }
        for (const char* recipe : {"llamacpp", "vllm", "fastflowlm", "openmoss", "trellis"}) {
            if (!system_info["recipes"].contains(recipe)) {
                backends[recipe] = unavailable("recipe not present on this build");
                continue;
            }
            const auto& recipe_json = system_info["recipes"][recipe];
            std::string found_version;
            if (recipe_json.contains("backends") && recipe_json["backends"].is_object()) {
                for (auto& [backend_name, backend_json] : recipe_json["backends"].items()) {
                    if (backend_json.contains("version") && backend_json["version"].is_string()) {
                        found_version = backend_json["version"].get<std::string>();
                        break;
                    }
                }
            }
            if (found_version.empty()) {
                backends[recipe] = unavailable("not installed / version unknown");
            } else {
                backends[recipe] = available(found_version);
            }
        }
    } catch (const std::exception& e) {
        return unavailable(std::string("Detection exception: ") + e.what());
    }
    return backends;
}

json snapshot_app_version() {
    // LEMON_VERSION_STRING is "<upstream-lemonade-version>+cj<fork-version>"
    // (see version.h.in / MERGING.md's "Version scheme" section).
    std::string full = LEMON_VERSION_STRING;
    std::string upstream = full;
    std::string fork_version;
    size_t plus = full.find('+');
    if (plus != std::string::npos) {
        upstream = full.substr(0, plus);
        fork_version = full.substr(plus + 1);
        if (fork_version.rfind("cj", 0) == 0) {
            fork_version = fork_version.substr(2);
        }
    }
    return {
        {"full_version", full},
        {"calamansi_version", fork_version.empty() ? nullptr : json(fork_version)},
        {"tracks_upstream_lemonade", upstream},
    };
}

json snapshot_ai_stack() {
    json section;
    section["rocm_version"] = detect_rocm_version();
    section["hip_version"] = detect_hip_version();
    section["mesa_version"] = detect_mesa_version();
    section["python_version"] = detect_python_version();
    section["backends"] = detect_backend_versions();
    section["app_version"] = snapshot_app_version();
    return section;
}

json snapshot_driver_stack() {
    // Strix Halo Z13-specific, best-effort - presence only, never a hard
    // dependency for this feature.
    json section;
    for (const char* tool : {"asusctl", "supergfxctl", "z13ctl"}) {
        section[tool] = json{{"installed", is_tool_installed(tool)}};
    }
    return section;
}

// --- 60s TTL cache, mirroring SystemInfoCache's mutex/flag skeleton --------

std::mutex g_state_mutex;
json g_cached_state;
std::chrono::steady_clock::time_point g_last_computed;
constexpr auto kCacheTtl = std::chrono::seconds(60);

} // namespace

json build_system_state_snapshot() {
    json snapshot;
    snapshot["generated_at_ms"] =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();

    try {
        snapshot["hardware"] = snapshot_hardware();
    } catch (const std::exception& e) {
        LOG(WARNING, "SystemState") << "hardware section failed: " << e.what() << std::endl;
        snapshot["hardware"] = json{{"error", e.what()}};
    }
    try {
        snapshot["os_kernel"] = snapshot_os_kernel();
    } catch (const std::exception& e) {
        LOG(WARNING, "SystemState") << "os_kernel section failed: " << e.what() << std::endl;
        snapshot["os_kernel"] = json{{"error", e.what()}};
    }
    try {
        snapshot["firmware"] = snapshot_firmware();
    } catch (const std::exception& e) {
        LOG(WARNING, "SystemState") << "firmware section failed: " << e.what() << std::endl;
        snapshot["firmware"] = json{{"error", e.what()}};
    }
    try {
        snapshot["ai_stack"] = snapshot_ai_stack();
    } catch (const std::exception& e) {
        LOG(WARNING, "SystemState") << "ai_stack section failed: " << e.what() << std::endl;
        snapshot["ai_stack"] = json{{"error", e.what()}};
    }
    try {
        snapshot["driver_stack"] = snapshot_driver_stack();
    } catch (const std::exception& e) {
        LOG(WARNING, "SystemState") << "driver_stack section failed: " << e.what() << std::endl;
        snapshot["driver_stack"] = json{{"error", e.what()}};
    }

    return snapshot;
}

json get_system_state_with_cache(bool force_refresh) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    auto now = std::chrono::steady_clock::now();
    bool have_cache = g_last_computed.time_since_epoch().count() > 0;
    if (!force_refresh && have_cache && (now - g_last_computed) < kCacheTtl) {
        json cached = g_cached_state;
        cached["cached"] = true;
        return cached;
    }
    g_cached_state = build_system_state_snapshot();
    g_last_computed = now;
    json fresh = g_cached_state;
    fresh["cached"] = false;
    return fresh;
}

} // namespace lemon
