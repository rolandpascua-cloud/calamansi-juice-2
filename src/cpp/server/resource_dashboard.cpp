#include "lemon/resource_dashboard.h"

#include "lemon/process_probe.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/utils/http_client.h"
#include "lemon/utils/system_probe.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <unordered_map>

#ifdef __linux__
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace lemon {

namespace {

// --- small local helpers ----------------------------------------------------

std::optional<double> to_number(const std::string& s) {
    std::string trimmed = utils::probe_trim(s);
    if (trimmed.empty()) return std::nullopt;
    std::string upper = trimmed;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
    if (upper == "N/A" || upper == "NA") return std::nullopt;
    try {
        size_t consumed = 0;
        double v = std::stod(trimmed, &consumed);
        return v;
    } catch (...) {
        return std::nullopt;
    }
}

void set_number_or_null(json& target, const std::string& key, const std::optional<double>& v) {
    target[key] = v ? json(*v) : json(nullptr);
}

// --- 1. CPU -------------------------------------------------------------

#ifdef __linux__
struct CoreTimes {
    long long user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
    long long total() const { return user + nice + system + idle + iowait + irq + softirq + steal; }
    long long busy() const { return total() - idle - iowait; }
};

bool parse_proc_stat_line(const std::string& line, CoreTimes& out) {
    std::istringstream iss(line);
    std::string label;
    iss >> label >> out.user >> out.nice >> out.system >> out.idle >> out.iowait >> out.irq >>
        out.softirq >> out.steal;
    return !iss.fail() || iss.eof();
}

// /proc/stat is cumulative-since-boot; a single read can't give a percent,
// so this keeps the previous sample and computes a delta on each call (the
// same non-blocking approach psutil.cpu_percent() uses internally, and
// what collectors.py's startup "prime" call exists to work around - the
// first call after process start has no prior sample, so it reports null
// rather than a meaningless 0/100).
std::mutex g_cpu_mutex;
std::vector<CoreTimes> g_prev_core_times;
bool g_cpu_primed = false;

std::vector<CoreTimes> read_all_core_times() {
    std::vector<CoreTimes> cores;
    std::ifstream f("/proc/stat");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("cpu", 0) != 0) break;
        if (line.size() > 3 && !std::isdigit(static_cast<unsigned char>(line[3]))) continue; // skip aggregate "cpu " line
        CoreTimes ct;
        if (parse_proc_stat_line(line, ct)) cores.push_back(ct);
    }
    return cores;
}
#endif

json get_cpu() {
    try {
#ifndef __linux__
        return json{{"error", "CPU percent collection is Linux-only in this fork"}};
#else
        json result;
        std::vector<double> per_core_percent;
        std::optional<double> aggregate;

        {
            std::lock_guard<std::mutex> lock(g_cpu_mutex);
            std::vector<CoreTimes> current = read_all_core_times();
            if (!g_cpu_primed || g_prev_core_times.size() != current.size()) {
                g_prev_core_times = current;
                g_cpu_primed = true;
                // First sample: report nulls rather than a meaningless value.
                for (size_t i = 0; i < current.size(); ++i) per_core_percent.push_back(-1);
            } else {
                for (size_t i = 0; i < current.size(); ++i) {
                    long long total_delta = current[i].total() - g_prev_core_times[i].total();
                    long long busy_delta = current[i].busy() - g_prev_core_times[i].busy();
                    double pct = total_delta > 0 ? (100.0 * busy_delta / total_delta) : 0.0;
                    per_core_percent.push_back(std::round(pct * 10.0) / 10.0);
                }
                g_prev_core_times = current;
            }
        }

        json per_core_json = json::array();
        double sum = 0;
        int valid = 0;
        for (double v : per_core_percent) {
            if (v < 0) {
                per_core_json.push_back(nullptr);
            } else {
                per_core_json.push_back(v);
                sum += v;
                ++valid;
            }
        }
        result["per_core_percent"] = per_core_json;
        result["aggregate_percent"] = valid > 0 ? json(std::round((sum / valid) * 10.0) / 10.0) : json(nullptr);

        unsigned int logical = std::thread::hardware_concurrency();
        result["core_count_logical"] = logical > 0 ? json(logical) : json(nullptr);

        // Physical core count: unique (physical id, core id) pairs from
        // /proc/cpuinfo; falls back to logical count if those fields are
        // absent (common in some virtualized/container environments).
        {
            std::ifstream cpuinfo("/proc/cpuinfo");
            std::string line;
            std::string cur_physical_id, cur_core_id;
            std::vector<std::pair<std::string, std::string>> pairs;
            while (std::getline(cpuinfo, line)) {
                if (line.rfind("physical id", 0) == 0) {
                    cur_physical_id = utils::probe_trim(line.substr(line.find(':') + 1));
                } else if (line.rfind("core id", 0) == 0) {
                    cur_core_id = utils::probe_trim(line.substr(line.find(':') + 1));
                    pairs.emplace_back(cur_physical_id, cur_core_id);
                }
            }
            if (!pairs.empty()) {
                std::sort(pairs.begin(), pairs.end());
                pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
                result["core_count_physical"] = pairs.size();
            } else {
                result["core_count_physical"] = result["core_count_logical"];
            }
        }

        // CPU frequency, via sysfs (no subprocess needed).
        auto cur_khz = utils::probe_read_file_trimmed("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq");
        auto min_khz = utils::probe_read_file_trimmed("/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq");
        auto max_khz = utils::probe_read_file_trimmed("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq");
        auto khz_to_mhz = [](const std::optional<std::string>& khz) -> json {
            if (!khz) return nullptr;
            try {
                return std::round((std::stod(*khz) / 1000.0) * 10.0) / 10.0;
            } catch (...) {
                return nullptr;
            }
        };
        result["current_mhz"] = khz_to_mhz(cur_khz);
        result["min_mhz"] = khz_to_mhz(min_khz);
        result["max_mhz"] = khz_to_mhz(max_khz);

        double load1 = 0, load5 = 0, load15 = 0;
        double loads[3];
        if (getloadavg(loads, 3) == 3) {
            load1 = loads[0];
            load5 = loads[1];
            load15 = loads[2];
            result["load_average"] = {
                {"1m", std::round(load1 * 100.0) / 100.0},
                {"5m", std::round(load5 * 100.0) / 100.0},
                {"15m", std::round(load15 * 100.0) / 100.0},
            };
        } else {
            result["load_average"] = nullptr;
        }

        return result;
#endif
    } catch (const std::exception& e) {
        return json{{"error", e.what()}};
    }
}

// --- 2. Memory (UMA-aware) -----------------------------------------------

json get_memory() {
    try {
#ifndef __linux__
        return json{{"error", "Memory collection is Linux-only in this fork"}};
#else
        std::unordered_map<std::string, long long> fields;
        std::ifstream f("/proc/meminfo");
        if (!f.is_open()) return json{{"error", "Could not read /proc/meminfo"}};
        std::string line;
        while (std::getline(f, line)) {
            std::istringstream iss(line);
            std::string key;
            long long value = 0;
            iss >> key >> value;
            if (!key.empty() && key.back() == ':') key.pop_back();
            fields[key] = value * 1024; // /proc/meminfo is in kB
        }

        long long total = fields.count("MemTotal") ? fields["MemTotal"] : 0;
        long long available = fields.count("MemAvailable") ? fields["MemAvailable"] : 0;
        long long free = fields.count("MemFree") ? fields["MemFree"] : 0;
        long long buffers = fields.count("Buffers") ? fields["Buffers"] : 0;
        long long cached = fields.count("Cached") ? fields["Cached"] : 0;
        long long shared = fields.count("Shmem") ? fields["Shmem"] : 0;
        long long used = total > 0 ? (total - available) : 0;

        json result = {
            {"total", total},
            {"used", used},
            {"free", free},
            {"available", available},
            {"shared", shared},
            {"buff_cache", buffers + cached},
            {"percent", total > 0 ? json(std::round((double(used) / total) * 1000.0) / 10.0) : json(nullptr)},
        };

        auto cmdline = utils::probe_read_file_trimmed("/proc/cmdline");
        if (!cmdline) {
            result["uma"] = {{"error", "could not read /proc/cmdline"}};
            return result;
        }
        auto tokens = utils::probe_parse_cmdline_tokens(*cmdline);
        bool has_gttsize = tokens.count("amdgpu.gttsize") > 0;
        bool has_pages_limit = tokens.count("ttm.pages_limit") > 0;
        result["uma"] = {
            {"amdgpu_gttsize", has_gttsize ? json(tokens["amdgpu.gttsize"]) : json(nullptr)},
            {"amdgpu_gttsize_set_on_cmdline", has_gttsize},
            {"ttm_pages_limit", has_pages_limit ? json(tokens["ttm.pages_limit"]) : json(nullptr)},
            {"ttm_pages_limit_set_on_cmdline", has_pages_limit},
            {"note", (has_gttsize || has_pages_limit)
                         ? "GTT sizing set explicitly on kernel cmdline."
                         : "Neither amdgpu.gttsize nor ttm.pages_limit set on cmdline - module defaults apply."},
        };
        return result;
#endif
    } catch (const std::exception& e) {
        return json{{"error", e.what()}};
    }
}

// --- 3. GPU (gfx1151 iGPU) -----------------------------------------------

json get_gpu() {
    try {
        if (utils::probe_is_tool_installed("rocm-smi")) {
            auto out = utils::probe_run_tool(
                "rocm-smi --showuse --showmemuse --showtemp --showpower --showmeminfo vram --json 2>&1", 2);
            if (out) {
                try {
                    json data = json::parse(*out);
                    std::string card_key;
                    for (auto& [key, value] : data.items()) {
                        if (key.rfind("card", 0) == 0) {
                            card_key = key;
                            break;
                        }
                    }
                    if (!card_key.empty()) {
                        const json& card = data[card_key];
                        auto field = [&](const char* name) -> std::optional<double> {
                            if (!card.contains(name)) return std::nullopt;
                            return to_number(card[name].is_string() ? card[name].get<std::string>()
                                                                     : card[name].dump());
                        };
                        json result = {{"source", "rocm-smi"}, {"device", card_key}};
                        set_number_or_null(result, "utilization_percent", field("GPU use (%)"));
                        set_number_or_null(result, "vram_used_percent", field("GPU Memory Allocated (VRAM%)"));
                        set_number_or_null(result, "vram_total_bytes", field("VRAM Total Memory (B)"));
                        set_number_or_null(result, "vram_used_bytes", field("VRAM Total Used Memory (B)"));
                        set_number_or_null(result, "temperature_c", field("Temperature (Sensor edge) (C)"));
                        set_number_or_null(result, "power_watts", field("Current Socket Graphics Package Power (W)"));
                        return result;
                    }
                } catch (const json::exception&) {
                    // fall through to amd-smi / error below
                }
            }
        }

        std::string rocm_smi_error = utils::probe_is_tool_installed("rocm-smi")
                                          ? "rocm-smi ran but returned unparseable output"
                                          : "rocm-smi not found on PATH";

        if (utils::probe_is_tool_installed("amd-smi")) {
            auto out = utils::probe_run_tool("amd-smi metric --json 2>&1", 2);
            if (out) {
                try {
                    json data = json::parse(*out);
                    // Confirmed against a real Strix Halo box: amd-smi wraps
                    // results in {"gpu_data": [...]}, and every leaf metric is
                    // {"value": N, "unit": "..."} rather than a bare number -
                    // NOT the flatter shape a naive port of the reference
                    // Python (which assumed a bare top-level list of bare
                    // numbers) would expect.
                    if (data.contains("gpu_data") && data["gpu_data"].is_array() && !data["gpu_data"].empty()) {
                        const json& gpu = data["gpu_data"][0];
                        auto leaf = [&](const json& obj, const char* key) -> std::optional<double> {
                            if (!obj.contains(key) || !obj[key].is_object() || !obj[key].contains("value"))
                                return std::nullopt;
                            const json& v = obj[key]["value"];
                            if (v.is_number()) return v.get<double>();
                            if (v.is_string()) return to_number(v.get<std::string>());
                            return std::nullopt;
                        };
                        // mem_usage's *_vram values are in MB (per real output) - convert to bytes to match
                        // rocm-smi's raw-byte fields for a consistent unit across sources.
                        auto vram_bytes = [&](const char* key) -> std::optional<double> {
                            auto mb = gpu.contains("mem_usage") ? leaf(gpu["mem_usage"], key) : std::nullopt;
                            return mb ? std::optional<double>(*mb * 1024.0 * 1024.0) : std::nullopt;
                        };
                        json result = {{"source", "amd-smi"}};
                        set_number_or_null(result, "utilization_percent",
                                           gpu.contains("usage") ? leaf(gpu["usage"], "gfx_activity") : std::nullopt);
                        set_number_or_null(result, "vram_used_bytes", vram_bytes("used_vram"));
                        set_number_or_null(result, "vram_total_bytes", vram_bytes("total_vram"));
                        set_number_or_null(result, "temperature_c",
                                           gpu.contains("temperature") ? leaf(gpu["temperature"], "edge") : std::nullopt);
                        set_number_or_null(result, "power_watts",
                                           gpu.contains("power") ? leaf(gpu["power"], "socket_power") : std::nullopt);
                        return result;
                    }
                } catch (const json::exception&) {
                    // fall through to error below
                }
            }
            return json{{"error", rocm_smi_error + "; amd-smi ran but returned unparseable output"}};
        }

        return json{{"error", rocm_smi_error + "; amd-smi also not found on PATH"}};
    } catch (const std::exception& e) {
        return json{{"error", e.what()}};
    }
}

// --- 4. NPU (XDNA2) -------------------------------------------------------

json get_npu() {
    try {
        json result;

        auto cmdline = utils::probe_read_file_trimmed("/proc/cmdline");
        if (cmdline) {
            auto tokens = utils::probe_parse_cmdline_tokens(*cmdline);
            bool set = tokens.count("amd_iommu") > 0;
            std::string value = set ? tokens["amd_iommu"] : "";
            result["amd_iommu_cmdline_value"] = set ? json(value) : json(nullptr);
            result["amd_iommu_set_on_cmdline"] = set;
            result["amd_iommu_ok"] = set && value == "on";
            if (!set) {
                result["amd_iommu_note"] = "amd_iommu not set on cmdline - kernel/BIOS default applies, verify manually.";
            } else if (value != "on") {
                result["amd_iommu_note"] = "amd_iommu=" + value + " - NPU driver requires 'on' to bind.";
            } else {
                result["amd_iommu_note"] = "amd_iommu=on - OK for NPU binding.";
            }
        } else {
            result["amd_iommu_cmdline_value"] = nullptr;
            result["amd_iommu_ok"] = nullptr;
            result["amd_iommu_note"] = "could not read /proc/cmdline";
        }

        // Module-loaded check via sysfs (no subprocess needed) rather than
        // shelling out to `lsmod` - /sys/module/<name> exists iff the module
        // is loaded.
        std::error_code ec;
        result["amdxdna_module_loaded"] = fs::exists("/sys/module/amdxdna", ec);

        if (utils::probe_is_tool_installed("lspci")) {
            auto out = utils::probe_run_tool("lspci 2>&1", 2);
            if (out) {
                json pci_devices = json::array();
                std::istringstream iss(*out);
                std::string line;
                while (std::getline(iss, line)) {
                    std::string lower = line;
                    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                    if (lower.find("signal processing") != std::string::npos) {
                        pci_devices.push_back(line);
                    }
                }
                result["present"] = !pci_devices.empty();
                result["pci_devices"] = pci_devices;
            } else {
                result["present"] = nullptr;
                result["pci_check_error"] = "lspci failed";
            }
        } else {
            result["present"] = nullptr;
            result["pci_check_error"] = "lspci not installed";
        }

        result["utilization_available"] = false;
        result["utilization_note"] =
            "No standard utilization metric is exposed for the NPU on this stack; "
            "reporting presence/binding status only.";
        return result;
    } catch (const std::exception& e) {
        return json{{"error", e.what()}};
    }
}

// --- 5. Disk (btrfs-aware) ------------------------------------------------

json get_disk() {
    try {
        std::error_code ec;
        fs::space_info space = fs::space("/", ec);
        if (ec) return json{{"error", ec.message()}};

        uintmax_t total = space.capacity;
        uintmax_t free = space.available; // available to non-root, matches typical "usable" framing
        uintmax_t used = total > free ? total - free : 0;
        json result = {
            {"total", total},
            {"used", used},
            {"free", free},
            {"percent", total > 0 ? json(std::round((double(used) / total) * 1000.0) / 10.0) : json(nullptr)},
        };

        if (!utils::probe_is_tool_installed("btrfs")) {
            result["btrfs_status"] = "not_installed";
            return result;
        }

        std::string output;
        int rc = utils::ProcessManager::run_command("btrfs subvolume list / 2>&1", output, 2);
        if (rc == 0) {
            result["btrfs_status"] = "ok";
            json subvolumes = json::array();
            std::istringstream iss(output);
            std::string line;
            while (std::getline(iss, line)) {
                if (!utils::probe_trim(line).empty()) subvolumes.push_back(line);
            }
            result["btrfs_subvolumes"] = subvolumes;
        } else {
            std::string trimmed = utils::probe_trim(output);
            bool permission_issue = trimmed.find("Permission denied") != std::string::npos ||
                                     trimmed.find("Operation not permitted") != std::string::npos;
            result["btrfs_status"] = permission_issue ? "permission_denied_or_error" : "error";
            result["btrfs_error"] = trimmed;
        }
        return result;
    } catch (const std::exception& e) {
        return json{{"error", e.what()}};
    }
}

// --- 6. Sensors / temps ----------------------------------------------------

json get_sensors() {
    try {
#ifndef __linux__
        return json{{"note", "Sensor collection is Linux-only in this fork"}};
#else
        std::error_code ec;
        if (!fs::exists("/sys/class/hwmon", ec)) {
            return json{{"note", "/sys/class/hwmon not present on this system"}};
        }

        json result = json::object();
        for (const auto& hwmon_entry : fs::directory_iterator("/sys/class/hwmon", ec)) {
            if (ec) break;
            fs::path hwmon_dir = hwmon_entry.path();
            std::string chip_name;
            if (auto name = utils::probe_read_file_trimmed((hwmon_dir / "name").string())) {
                chip_name = *name;
            } else {
                continue;
            }

            json readings = json::array();
            std::error_code inner_ec;
            for (const auto& file_entry : fs::directory_iterator(hwmon_dir, inner_ec)) {
                if (inner_ec) break;
                std::string filename = file_entry.path().filename().string();
                // temp<N>_input, with optional temp<N>_label / _max / _crit siblings.
                if (filename.size() < 10 || filename.rfind("temp", 0) != 0 ||
                    filename.find("_input") == std::string::npos) {
                    continue;
                }
                std::string prefix = filename.substr(0, filename.find("_input"));

                auto read_millideg = [&](const std::string& suffix) -> std::optional<double> {
                    auto raw = utils::probe_read_file_trimmed((hwmon_dir / (prefix + suffix)).string());
                    if (!raw) return std::nullopt;
                    try {
                        return std::stod(*raw) / 1000.0;
                    } catch (...) {
                        return std::nullopt;
                    }
                };

                auto current = read_millideg("_input");
                if (!current) continue;
                std::string label = chip_name;
                if (auto raw_label = utils::probe_read_file_trimmed((hwmon_dir / (prefix + "_label")).string())) {
                    label = *raw_label;
                }

                json reading = {{"label", label}, {"current", *current}};
                set_number_or_null(reading, "high", read_millideg("_max"));
                set_number_or_null(reading, "critical", read_millideg("_crit"));
                readings.push_back(reading);
            }

            if (!readings.empty()) {
                if (result.contains(chip_name)) {
                    for (const auto& r : readings) result[chip_name].push_back(r);
                } else {
                    result[chip_name] = readings;
                }
            }
        }

        if (result.empty()) {
            return json{{"note", "No temperature sensors found under /sys/class/hwmon"}};
        }
        return result;
#endif
    } catch (const std::exception& e) {
        return json{{"error", e.what()}};
    }
}

// --- inference service detectors ------------------------------------------
// Deliberately excludes this app itself (lemond/calamansid) - redundant
// with the app's own state, per the feature's own scoping note.

json describe_probed_service(const std::string& base_url, const std::vector<std::string>& paths) {
    for (const auto& path : paths) {
        try {
            auto response =
                utils::HttpClient::get(base_url + path, {}, 2, utils::HttpSecurityPolicy::TrustedLoopback);
            if (response.status_code >= 200 && response.status_code < 300) {
                try {
                    return json::parse(response.body);
                } catch (const json::exception&) {
                    return json{{"raw", response.body.substr(0, 1000)}};
                }
            }
        } catch (const std::exception&) {
            continue;
        }
    }
    return "unreachable";
}

void describe_port_resolution(const ProcessMatch& proc, json& out) {
    PortResolution resolution = resolve_port(proc);
    if (!resolution.found) {
        out["port"] = "unknown";
        out["note"] =
            "listening port could not be determined (process likely owned by a different user; "
            "socket introspection denied)";
        return;
    }
    out["port"] = resolution.ports.size() == 1 ? json(resolution.ports[0]) : json(resolution.ports);
    out["port_source"] = resolution.source;
}

json get_lmstudio_status() {
    try {
        std::string binary;
        if (utils::probe_is_tool_installed("lms")) {
            binary = "lms";
        } else {
            const char* home = std::getenv("HOME");
            if (home) {
                fs::path candidate = fs::path(home) / ".lmstudio" / "bin" / "lms";
                std::error_code ec;
#ifndef _WIN32
                if (fs::exists(candidate, ec) && access(candidate.c_str(), X_OK) == 0) {
                    binary = candidate.string();
                }
#endif
            }
        }

        auto app_procs = find_processes({"lm studio", ".lmstudio", "lmstudio"});
        json result = {
            {"cli_found", !binary.empty()},
            {"cli_path", binary.empty() ? json(nullptr) : json(binary)},
            {"app_process_running", !app_procs.empty()},
        };

        if (binary.empty()) {
            result["loaded_models"] = json::array();
            result["note"] = "lms CLI not found on PATH or in ~/.lmstudio/bin";
            return result;
        }

        auto out = utils::probe_run_tool("\"" + binary + "\" ps --json 2>&1", 2);
        json loaded = json::array();
        bool parsed = false;
        if (out) {
            try {
                loaded = json::parse(*out);
                parsed = true;
            } catch (const json::exception&) {
                parsed = false;
            }
        }
        if (!parsed) {
            loaded = json::array();
        }
        result["loaded_models"] = loaded;
        result["running"] = loaded.is_array() ? !loaded.empty() : true;
        return result;
    } catch (const std::exception& e) {
        return json{{"error", e.what()}};
    }
}

json get_llama_server_status() {
    try {
        auto procs = find_processes({"llama-server"});
        json result = {{"process_running", !procs.empty()}};
        if (procs.empty()) {
            result["note"] = "llama-server process not found";
            return result;
        }

        json instances = json::array();
        for (const auto& proc : procs) {
            json instance = {{"pid", proc.pid}, {"cmdline", proc.cmdline.substr(0, 400)}};
            describe_port_resolution(proc, instance);
            if (instance.contains("port") && instance["port"].is_number()) {
                std::string base = "http://127.0.0.1:" + std::to_string(instance["port"].get<int>());
                instance["health"] = describe_probed_service(base, {"/health"});
                instance["models"] = describe_probed_service(base, {"/v1/models"});
            } else {
                instance["health"] = "unreachable";
                instance["models"] = "unknown";
            }
            instances.push_back(instance);
        }
        result["instance_count"] = instances.size();
        result["instances"] = instances;
        result["pid"] = instances[0]["pid"];
        result["port"] = instances[0]["port"];
        result["models"] = instances[0].value("models", json("unknown"));
        return result;
    } catch (const std::exception& e) {
        return json{{"error", e.what()}};
    }
}

json get_flm_status() {
    try {
        bool cli_found = utils::probe_is_tool_installed("flm");
        json result = {{"cli_found", cli_found}};
        if (!cli_found) {
            result["note"] = "flm not found on PATH";
            return result;
        }

        json inventory = json::array();
        auto list_out = utils::probe_run_tool("flm list -j 2>&1", 2);
        if (list_out) {
            try {
                json data = json::parse(*list_out);
                inventory = data.is_object() && data.contains("models") ? data["models"] : data;
            } catch (const json::exception&) {
                // leave inventory empty
            }
        }

        int installed_count = 0;
        json installed_models = json::array();
        if (inventory.is_array()) {
            for (const auto& m : inventory) {
                if (m.is_object() && m.value("installed", false)) {
                    ++installed_count;
                    installed_models.push_back(m.contains("model") ? m["model"] : json(m.value("name", "")));
                }
            }
        }
        result["installed_count"] = installed_count;
        result["installed_models"] = installed_models;
        result["total_available_count"] = inventory.is_array() ? inventory.size() : 0;

        auto candidates = find_processes({"flm"});
        std::vector<ProcessMatch> server_procs;
        for (const auto& p : candidates) {
            std::string lower_cmdline = p.cmdline;
            std::transform(lower_cmdline.begin(), lower_cmdline.end(), lower_cmdline.begin(), ::tolower);
            std::string lower_name = p.name;
            std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
            if (lower_cmdline.find("serve") != std::string::npos || lower_name == "flm") {
                server_procs.push_back(p);
            }
        }
        result["process_running"] = !server_procs.empty();
        if (!server_procs.empty()) {
            const auto& proc = server_procs[0];
            result["pid"] = proc.pid;
            describe_port_resolution(proc, result);
            if (result.contains("port") && result["port"].is_number()) {
                std::string base = "http://127.0.0.1:" + std::to_string(result["port"].get<int>());
                result["models"] = describe_probed_service(base, {"/v1/models"});
            } else {
                result["models"] = "unknown";
            }
            result["tier"] = "NPU"; // FastFlowLM is XDNA2-NPU-only by design
        }
        return result;
    } catch (const std::exception& e) {
        return json{{"error", e.what()}};
    }
}

json get_vllm_status() {
    try {
        auto procs = find_processes({"vllm"});
        // No Python interpreter embedded in this C++ app to import-check
        // vllm's presence the way the reference dashboard does - process
        // detection is the whole signal here.
        json result = {{"process_running", !procs.empty()}};
        if (procs.empty()) {
            result["note"] = "no vllm process found";
        }
        return result;
    } catch (const std::exception& e) {
        return json{{"error", e.what()}};
    }
}

} // namespace

json get_resources_system() {
    json result;
    try {
        result["cpu"] = get_cpu();
    } catch (const std::exception& e) {
        LOG(WARNING, "ResourceDashboard") << "cpu collector failed: " << e.what() << std::endl;
        result["cpu"] = json{{"error", e.what()}};
    }
    try {
        result["memory"] = get_memory();
    } catch (const std::exception& e) {
        LOG(WARNING, "ResourceDashboard") << "memory collector failed: " << e.what() << std::endl;
        result["memory"] = json{{"error", e.what()}};
    }
    try {
        result["gpu"] = get_gpu();
    } catch (const std::exception& e) {
        LOG(WARNING, "ResourceDashboard") << "gpu collector failed: " << e.what() << std::endl;
        result["gpu"] = json{{"error", e.what()}};
    }
    try {
        result["npu"] = get_npu();
    } catch (const std::exception& e) {
        LOG(WARNING, "ResourceDashboard") << "npu collector failed: " << e.what() << std::endl;
        result["npu"] = json{{"error", e.what()}};
    }
    try {
        result["disk"] = get_disk();
    } catch (const std::exception& e) {
        LOG(WARNING, "ResourceDashboard") << "disk collector failed: " << e.what() << std::endl;
        result["disk"] = json{{"error", e.what()}};
    }
    try {
        result["sensors"] = get_sensors();
    } catch (const std::exception& e) {
        LOG(WARNING, "ResourceDashboard") << "sensors collector failed: " << e.what() << std::endl;
        result["sensors"] = json{{"error", e.what()}};
    }
    result["timestamp"] =
        std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    return result;
}

json get_resources_inference() {
    json result;
    try {
        result["lmstudio"] = get_lmstudio_status();
    } catch (const std::exception& e) {
        result["lmstudio"] = json{{"error", e.what()}};
    }
    try {
        result["llama_server"] = get_llama_server_status();
    } catch (const std::exception& e) {
        result["llama_server"] = json{{"error", e.what()}};
    }
    try {
        result["vllm"] = get_vllm_status();
    } catch (const std::exception& e) {
        result["vllm"] = json{{"error", e.what()}};
    }
    try {
        result["fastflowlm"] = get_flm_status();
    } catch (const std::exception& e) {
        result["fastflowlm"] = json{{"error", e.what()}};
    }
    result["timestamp"] =
        std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    return result;
}

} // namespace lemon
