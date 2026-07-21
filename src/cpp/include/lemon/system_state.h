#pragma once

// Calamansi Juice 2 addition (not present in upstream lemonade-sdk/lemonade):
// a point-in-time "what am I running on, and what's installed" snapshot -
// hardware, OS/kernel, firmware, and the AI software stack - distinct from
// the live-updating Resource Dashboard. See docs/calamansi/system-state.md
// and MERGING.md.

#include <nlohmann/json.hpp>
#include <string>

namespace lemon {

// Builds a fresh snapshot. Every section is independently try/caught inside
// (mirrors SystemInfo::get_device_dict()'s per-section fault tolerance) so
// one failing/missing tool never blanks the rest of the response. Safe to
// call from any thread; does its own subprocess calls with short timeouts
// (via lemon::utils::ProcessManager::run_command) so a hung CLI can't hang
// the request.
nlohmann::json build_system_state_snapshot();

// Cached wrapper (~60s TTL, mirroring SystemInfoCache's mutex/flag skeleton
// in system_info.cpp): returns the last computed snapshot if it's still
// fresh, otherwise recomputes. Pass force_refresh=true to always recompute
// (GET /system/state?refresh=true).
nlohmann::json get_system_state_with_cache(bool force_refresh);

} // namespace lemon
